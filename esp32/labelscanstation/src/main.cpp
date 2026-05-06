#include "app_config.h"
#include "usb_host_task.h"
#include "usb_printer.h"
#include "brother_ql.h"
#include "ql_models.h"
#include "wifi_manager.h"
#include "label_renderer.h"
#include "rfid_reader.h"
#include "status_led.h"
#include "lcd_display.h"
#include "card_lookup.h"
#include "buzzer.h"
#include "mode_switch.h"

#include <cstring>
#include <cstdio>

#include <ctime>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "main";

static PrinterState s_printers[MAX_PRINTERS];
static const media_profile_t *s_media_profiles[MAX_PRINTERS] = {};
static uint8_t s_media_type[MAX_PRINTERS] = {};      // 0x0A=continuous, 0x0B=die-cut
static uint8_t s_media_length_mm[MAX_PRINTERS] = {}; // 0 for continuous
static bool s_media_queried[MAX_PRINTERS] = {};

// Mutex protecting s_printers[] — held during connect/disconnect and printing
static SemaphoreHandle_t s_printer_mutex = nullptr;

// Print queue carries the data the renderer needs. Mode is captured at scan
// time so a switch flip during a print doesn't change the in-flight job.
typedef struct {
    int  mode;
    char name[LOOKUP_NAME_MAX];
    char email[LOOKUP_EMAIL_MAX];
    char phone[LOOKUP_PHONE_MAX];
} print_msg_t;

static QueueHandle_t s_print_queue = nullptr;

// Transient LCD override (e.g. "Printing...", card scan feedback)
static char s_lcd_override[2][17] = {};  // [line][text], 16 chars + NUL
static int64_t s_lcd_override_until = 0; // microsecond timestamp
static portMUX_TYPE s_lcd_mux = portMUX_INITIALIZER_UNLOCKED;

static void lcd_override(int line, const char *text, int duration_ms) {
    taskENTER_CRITICAL(&s_lcd_mux);
    // Clear the other line if the previous override has expired
    int64_t now = esp_timer_get_time();
    if (now >= s_lcd_override_until) {
        s_lcd_override[0][0] = '\0';
        s_lcd_override[1][0] = '\0';
    }
    strncpy(s_lcd_override[line], text, 16);
    s_lcd_override[line][16] = '\0';
    int64_t until = now + (int64_t)duration_ms * 1000;
    if (until > s_lcd_override_until)
        s_lcd_override_until = until;
    taskEXIT_CRITICAL(&s_lcd_mux);
}

static bool lcd_override_active() {
    return esp_timer_get_time() < s_lcd_override_until;
}

// Get the best printer: prefer highest USB PID (highest model number) among connected printers.
// Caller must hold s_printer_mutex. Sets *slot_out if non-null.
static PrinterState *get_active_printer(int *slot_out = nullptr) {
    PrinterState *best = nullptr;
    int best_slot = -1;
    for (int i = 0; i < MAX_PRINTERS; i++) {
        if (s_printers[i].connected && s_printers[i].model) {
            if (!best || s_printers[i].model->usb_pid > best->model->usb_pid) {
                best = &s_printers[i];
                best_slot = i;
            }
        }
    }
    if (slot_out) *slot_out = best_slot;
    return best;
}

// Check if any printer is connected (lock-free for status display)
static bool has_active_printer() {
    for (int i = 0; i < MAX_PRINTERS; i++) {
        if (s_printers[i].connected)
            return true;
    }
    return false;
}

static void on_printer_event(usb_device_handle_t dev_handle, bool connected, const ql_model_t *model) {
    xSemaphoreTake(s_printer_mutex, portMAX_DELAY);

    if (connected) {
        // Find first empty slot
        int slot = -1;
        for (int i = 0; i < MAX_PRINTERS; i++) {
            if (!s_printers[i].connected && s_printers[i].model == nullptr) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            ESP_LOGW(TAG, "Max printers reached, ignoring %s", model->name);
            xSemaphoreGive(s_printer_mutex);
            usb_host_device_close(usb_host_get_client_handle(), dev_handle);
            return;
        }
        s_printers[slot].model = model;
        if (printer_on_connected(&s_printers[slot], dev_handle, usb_host_get_client_handle())) {
            ESP_LOGI(TAG, "Printer %d ready (%s)", slot + 1, model->name);
            s_media_profiles[slot] = nullptr;
            s_media_queried[slot] = false;
        } else {
            ESP_LOGE(TAG, "Printer connection setup failed");
            s_printers[slot].model = nullptr;
            xSemaphoreGive(s_printer_mutex);
            usb_host_device_close(usb_host_get_client_handle(), dev_handle);
            return;
        }
    } else {
        // Find the printer that disconnected by matching dev_handle — clear in-place
        for (int i = 0; i < MAX_PRINTERS; i++) {
            if (s_printers[i].dev_handle == dev_handle) {
                ESP_LOGW(TAG, "Printer %d disconnected (%s)", i + 1,
                         s_printers[i].model ? s_printers[i].model->name : "unknown");
                printer_on_disconnected(&s_printers[i]);
                s_printers[i].model = nullptr;
                s_media_profiles[i] = nullptr;
                s_media_queried[i] = false;
                break;
            }
        }
    }

    xSemaphoreGive(s_printer_mutex);
}

static void print_task(void *arg) {
    ESP_LOGI(TAG, "Print task started");
    print_msg_t msg;

    while (true) {
        if (xQueueReceive(s_print_queue, &msg, portMAX_DELAY) != pdTRUE)
            continue;

        xSemaphoreTake(s_printer_mutex, portMAX_DELAY);
        int slot = -1;
        PrinterState *printer = get_active_printer(&slot);
        if (!printer) {
            xSemaphoreGive(s_printer_mutex);
            ESP_LOGW(TAG, "Print requested but no printer connected");
            lcd_override(0, "NO PRINTER!", 4000);
            continue;
        }

        ESP_LOGI(TAG, "Printing '%s' on %s...", msg.name, printer->model->name);
        lcd_override(0, "PRINTING...", 5000);
        lcd_override(1, msg.name, 5000);

        // Query media on first print (deferred from connect to avoid USB stack issues).
        // Capture both the cached profile and the loaded-media type/length so the
        // renderer can branch on continuous vs. die-cut and on die-cut length.
        if (slot >= 0 && !s_media_queried[slot]) {
            s_media_queried[slot] = true;
            BrotherQLStatus status = {};
            s_media_profiles[slot] = brother_ql_query_media(printer, printer->model, &status);
            if (s_media_profiles[slot]) {
                s_media_type[slot] = status.media_type;
                s_media_length_mm[slot] = status.media_length;
                ESP_LOGI(TAG, "Printer %d media: %dmm type=0x%02X length=%dmm (%dpx)",
                         slot + 1, s_media_profiles[slot]->width_mm,
                         s_media_type[slot], s_media_length_mm[slot],
                         s_media_profiles[slot]->printable_px);
            }
        }

        // Render at detected media width (or default 29mm)
        const media_profile_t *profile = (slot >= 0) ? s_media_profiles[slot] : nullptr;
        uint16_t render_w = profile ? profile->printable_px : LABEL_PRINTABLE_W;
        uint16_t render_h = LABEL_PRINTABLE_H;

        label_render_req_t req = {};
        req.mode = msg.mode;
        req.name = msg.name;
        req.email = msg.email;
        req.phone = msg.phone;
        req.fb_w = render_w;
        req.fb_h = render_h;
        req.media_type = (slot >= 0) ? s_media_type[slot] : MEDIA_TYPE_DIE_CUT;
        req.media_length_mm = (slot >= 0) ? s_media_length_mm[slot] : 0;

        // Mode 2 on continuous tape: render half-height so the printer feeds
        // ~45 mm of tape instead of the full 90 mm.
        if (req.mode == 2 && req.media_type == MEDIA_TYPE_CONTINUOUS) {
            req.fb_h = render_h / 2;
        }

        const uint8_t *fb = label_renderer_render(&req);
        if (fb == nullptr) {
            xSemaphoreGive(s_printer_mutex);
            ESP_LOGE(TAG, "Render failed");
            lcd_override(0, "RENDER ERROR", 4000);
            continue;
        }

        uint16_t fb_stride = label_renderer_stride();
        char print_err[17] = {};
        const media_profile_t *detected = nullptr;
        bool ok = brother_ql_print(printer, printer->model, fb, render_w, req.fb_h, fb_stride,
                                   print_err, sizeof(print_err), &detected);

        // If print detected different media, update profile, re-render and retry once
        if (!ok && detected && detected->printable_px != render_w) {
            ESP_LOGI(TAG, "Media changed — re-rendering for %dmm (%dpx)",
                     detected->width_mm, detected->printable_px);
            if (slot >= 0) s_media_profiles[slot] = detected;
            render_w = detected->printable_px;
            req.fb_w = render_w;
            fb = label_renderer_render(&req);
            if (fb) {
                fb_stride = label_renderer_stride();
                print_err[0] = '\0';
                ok = brother_ql_print(printer, printer->model, fb, render_w, req.fb_h, fb_stride,
                                      print_err, sizeof(print_err));
            }
        }

        xSemaphoreGive(s_printer_mutex);

        ESP_LOGI(TAG, "Print %s", ok ? "succeeded" : "FAILED");
        if (!ok) {
            if (strcmp(print_err, "BAD MEDIA") == 0) {
                lcd_override(0, "BAD MEDIA", 10000);
                lcd_override(1, "UNKNOWN WIDTH", 10000);
            } else {
                lcd_override(0, "PRINT FAILED!", 10000);
                if (print_err[0])
                    lcd_override(1, print_err, 10000);
            }
        }
    }
}

// Dedup: ignore same card ID if scanned within 8s of starting a print
#define DEDUP_INTERVAL_US (8 * 1000000LL)
static uint32_t s_last_card_id = 0;
static int64_t  s_last_print_time = 0;

static void enqueue_print(uint32_t card_id, int mode, const char *name,
                          const char *email, const char *phone) {
    print_msg_t msg = {};
    msg.mode = mode;
    strncpy(msg.name, name, sizeof(msg.name) - 1);
    strncpy(msg.email, email ? email : "", sizeof(msg.email) - 1);
    strncpy(msg.phone, phone ? phone : "", sizeof(msg.phone) - 1);
    xQueueSend(s_print_queue, &msg, 0);
}

// Returns status string for line 1, or NULL if ready (SCAN CARD).
static const char *get_status_line() {
    bool first_boot = !sntp_is_synced() && !wifi_ever_connected();
    bool first_ntp = wifi_is_connected() && !sntp_is_synced();
    if (first_boot || first_ntp)    return "NETWORKING...";
    if (!wifi_is_connected())       return "ERR: NO WIFI";
    if (!sntp_is_synced())          return "ERR: NO NTP";
    if (card_lookup_count() == 0)   return "ERR: MEMBERDB";
    if (!has_active_printer())      return "ERR: NO PRINTER";
    return nullptr;
}

// Cycle between SCAN CARD / SCAN FOB every 3 seconds
#define SCAN_CYCLE_INTERVAL_US (3 * 1000000LL)

static void format_idle_line(char *buf, size_t len, int64_t now) {
    bool show_fob = ((now / SCAN_CYCLE_INTERVAL_US) % 2) == 1;
    snprintf(buf, len, "%s", show_fob ? "SCAN FOB" : "SCAN CARD");
}

// Right-aligned 6-char mode indicator for LCD line 0 (col 10..15).
// Padding inside the literal aligns NORMAL/SHORT/TODO to col 15.
static const char *mode_label(int m) {
    switch (m) {
        case 1: return "NORMAL";
        case 2: return " SHORT";
        case 3: return "  TODO";
        default: return "      ";
    }
}

// Append the current mode indicator to a steady-state idle line0 (e.g. SCAN CARD,
// SCAN FOB, SCAN HAND). Pads the base text with spaces out to col 10, then writes
// the 6-char mode field at cols 10..15. Caller passes a 17-char buffer.
static void append_mode_indicator(char *line0) {
    int len = (int)strlen(line0);
    if (len > 10) len = 10;
    while (len < 10) line0[len++] = ' ';
    const char *m = mode_label(mode_switch_current());
    memcpy(line0 + 10, m, 6);
    line0[16] = '\0';
}

// Format clock line for line 2
static const char *MONTH_ABBR[] = {
    "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};

static void format_clock(char *buf, size_t len) {
    if (!sntp_is_synced()) {
        strncpy(buf, "clock pending", len);
        buf[len - 1] = '\0';
        return;
    }
    time_t now;
    time(&now);
    struct tm ti;
    localtime_r(&now, &ti);
    snprintf(buf, len, "%s %2d  %02d:%02d:%02d",
             MONTH_ABBR[ti.tm_mon], ti.tm_mday,
             ti.tm_hour, ti.tm_min, ti.tm_sec);
}

// Easter egg: show "SCAN HAND" for 1 second every 392 seconds
#define EASTER_EGG_INTERVAL_US (392 * 1000000LL)
#define EASTER_EGG_DURATION_US (1 * 1000000LL)

static void lcd_update_task(void *arg) {
    char line0[17];
    char line1[17];
    char ovr0[17];
    char ovr1[17];
    int64_t easter_egg_start = esp_timer_get_time();

    while (true) {
        int64_t now = esp_timer_get_time();
        int64_t elapsed = (now - easter_egg_start) % EASTER_EGG_INTERVAL_US;
        bool easter_egg = elapsed < EASTER_EGG_DURATION_US;

        if (lcd_override_active()) {
            // Copy override strings under spinlock, then do I2C outside
            taskENTER_CRITICAL(&s_lcd_mux);
            memcpy(ovr0, s_lcd_override[0], sizeof(ovr0));
            memcpy(ovr1, s_lcd_override[1], sizeof(ovr1));
            taskEXIT_CRITICAL(&s_lcd_mux);

            lcd_set_line(0, ovr0);
            if (ovr1[0] != '\0') {
                lcd_set_line(1, ovr1);
            } else {
                format_clock(line1, sizeof(line1));
                lcd_set_line(1, line1);
            }
        } else {
            const char *status = get_status_line();
            if (status) {
                // Error states keep the full 16-char line — no mode indicator.
                strncpy(line0, status, sizeof(line0));
                line0[sizeof(line0) - 1] = '\0';
            } else if (easter_egg) {
                snprintf(line0, sizeof(line0), "SCAN HAND");
                append_mode_indicator(line0);
            } else {
                format_idle_line(line0, sizeof(line0), now);
                append_mode_indicator(line0);
            }
            format_clock(line1, sizeof(line1));

            lcd_set_line(0, line0);
            lcd_set_line(1, line1);
        }

        // Sleep up to 500ms; wake immediately when notified (e.g. mode change).
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
    }
}

static TaskHandle_t s_lcd_task_handle = nullptr;

static void on_mode_change(int new_mode) {
    (void)new_mode;
    if (s_lcd_task_handle) xTaskNotifyGive(s_lcd_task_handle);
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "=== Label Scan Station ===");

    // LED: dim white during init
    status_led_init();
    status_led_set(20, 20, 20);

    // Initialize LCD display (auto-detects I2C address and pin order)
    lcd_init();
    lcd_status("Starting up...", "");

    // Mode switch: read GPIO 36/37 for label-format selection.
    mode_switch_init();
    mode_switch_set_change_cb(on_mode_change);

    // Initialize printer states
    for (int i = 0; i < MAX_PRINTERS; i++)
        printer_init(&s_printers[i]);

    // Create printer mutex
    s_printer_mutex = xSemaphoreCreateMutex();

    // Create print queue
    s_print_queue = xQueueCreate(4, sizeof(print_msg_t));

    // Initialize card lookup DB
    card_lookup_init();

    // Initialize buzzer
    buzzer_init();

    // Initialize WiFi + SNTP
    wifi_init();

    // Fetch member list from API (if WiFi connected)
    if (wifi_is_connected()) {
        card_lookup_refresh();
    }

    // Initialize renderer
    label_renderer_init();

    // Initialize USB host and set printer callback
    usb_host_set_printer_callback(on_printer_event);
    usb_host_init();

    // Initialize button GPIOs (simple input, no ISR — we poll for long-press
    // on BUTTON_GPIO, and log Option-button changes on OPTION_BUTTON_GPIO so
    // we can verify wiring before committing to a driver.)
    gpio_config_t btn_conf = {};
    btn_conf.intr_type = GPIO_INTR_DISABLE;
    btn_conf.mode = GPIO_MODE_INPUT;
    btn_conf.pin_bit_mask = (1ULL << BUTTON_GPIO) | (1ULL << OPTION_BUTTON_GPIO);
    btn_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&btn_conf));

    // Initialize RFID reader
    QueueHandle_t rfid_queue = rfid_init();

    // Start print task
    xTaskCreate(print_task, "print_task", 16384, nullptr, 1, nullptr);

    // Start LCD update task (refreshes clock + status every 500ms, or on mode change)
    xTaskCreate(lcd_update_task, "lcd_update", 2048, nullptr, 1, &s_lcd_task_handle);

    // Mode-switch polling task. Started after LCD task so the change callback
    // has a valid task handle to notify.
    mode_switch_start_task();

    // LED: green when ready
    status_led_set(0, 20, 0);

    ESP_LOGI(TAG, "System ready — scan RFID card or press button (1s hold) to test print");

    // Main loop: handle RFID scans and button
    uint32_t card_id;
    int64_t btn_press_start = 0;
    bool btn_triggered = false;
    bool cards_loaded = wifi_is_connected();

    while (true) {
        int64_t now = esp_timer_get_time();

        // Late WiFi connect: fetch card DB if we didn't at boot
        if (!cards_loaded && wifi_is_connected()) {
            cards_loaded = true;
            card_lookup_refresh();
        }

        // Check RFID queue (non-blocking)
        if (xQueueReceive(rfid_queue, &card_id, 0) == pdTRUE) {
            ESP_LOGI(TAG, "RFID card scanned: 0x%08lX", (unsigned long)card_id);

            // Dedup: suppress repeated scans of the same card
            int64_t now = esp_timer_get_time();
            if (card_id == s_last_card_id && (now - s_last_print_time) < DEDUP_INTERVAL_US) {
                ESP_LOGI(TAG, "Ignoring duplicate card 0x%08lX (within %ds)",
                         (unsigned long)card_id, (int)(DEDUP_INTERVAL_US / 1000000LL));
                continue;
            }

            lookup_result_t result = card_lookup(card_id);

            if (!result.found) {
                buzzer_beep_bad();
                ESP_LOGW(TAG, "Unknown card 0x%08lX", (unsigned long)card_id);
                lcd_override(0, "ERR:UNKNOWN CARD", 4000);
            } else if (!has_active_printer()) {
                buzzer_beep_sad();
                lcd_override(0, result.name, 4000);
                lcd_override(1, "NO PRINTER!", 4000);
                ESP_LOGW(TAG, "Card '%s' OK but no printer", result.name);
            } else {
                int mode = mode_switch_current();
                buzzer_beep_good();
                if (mode == 3) {
                    // Mode 3 is a placeholder — accept the scan but skip print.
                    lcd_override(0, "MODE 3: TODO", 2000);
                    lcd_override(1, result.name, 2000);
                } else {
                    lcd_override(0, result.name, 3000);
                    enqueue_print(card_id, mode, result.name,
                                  result.email, result.phone);
                }
            }

            s_last_card_id = card_id;
            s_last_print_time = esp_timer_get_time();
        }

        // Option button: edge-triggered. In Mode 1, a press shows the
        // current IP and the last DB-refresh time on the LCD for 5 s.
        // Other modes: log only (PERMIT increment will go here later).
        static int s_last_option = 1;
        int option_now = gpio_get_level(OPTION_BUTTON_GPIO);
        if (s_last_option == 1 && option_now == 0) {
            int mode = mode_switch_current();
            if (mode == 1) {
                char ip_line[17] = {};
                char db_line[17] = {};
                wifi_get_ip(ip_line, sizeof(ip_line));
                time_t t = card_lookup_last_refresh();
                if (t == 0) {
                    snprintf(db_line, sizeof(db_line), "DB never");
                } else {
                    struct tm ti;
                    localtime_r(&t, &ti);
                    snprintf(db_line, sizeof(db_line), "DB %s %d %02d:%02d",
                             MONTH_ABBR[ti.tm_mon], ti.tm_mday,
                             ti.tm_hour, ti.tm_min);
                }
                lcd_override(0, ip_line, 5000);
                lcd_override(1, db_line, 5000);
                ESP_LOGI(TAG, "Option pressed (mode 1): %s / %s", ip_line, db_line);
            } else {
                ESP_LOGI(TAG, "Option pressed (mode %d, no action yet)", mode);
            }
        }
        s_last_option = option_now;

        // Button handling: long press = test print
        bool btn_down = gpio_get_level(BUTTON_GPIO) == 0;
        if (btn_down) {
            if (btn_press_start == 0) {
                btn_press_start = now;
            } else if (!btn_triggered && (now - btn_press_start) >= 1000000LL) {
                btn_triggered = true;
                ESP_LOGI(TAG, "Button held 1s — test print");
                lcd_override(0, "Test print...", 3000);
                enqueue_print(0, mode_switch_current(), "TEST", "", "");
            }
        } else {
            btn_press_start = 0;
            btn_triggered = false;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
