#include "app_config.h"
#include "usb_host_task.h"
#include "usb_printer.h"
#include "brother_ql.h"
#include "wifi_manager.h"
#include "label_renderer.h"
#include "rfid_reader.h"
#include "status_led.h"
#include "lcd_display.h"
#include "card_lookup.h"
#include "buzzer.h"
#include "mode_switch.h"
#include "led_output.h"

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
static uint8_t s_active_mode = 0;  // last known mode switch value

// Mutex protecting s_printers[] — held during connect/disconnect and printing
static SemaphoreHandle_t s_printer_mutex = nullptr;

// Print queue carries label info
typedef struct {
    char name[LOOKUP_NAME_MAX];
    uint8_t label_type;  // label_type_t
    uint8_t days;        // for parking permits
} print_msg_t;

static QueueHandle_t s_print_queue = nullptr;

// Transient LCD override (e.g. "Printing...", card scan feedback)
static char s_lcd_override[2][17] = {};  // [line][text], 16 chars + NUL
static int64_t s_lcd_override_until = 0; // microsecond timestamp
static portMUX_TYPE s_lcd_mux = portMUX_INITIALIZER_UNLOCKED;

static void lcd_override(int line, const char *text, int duration_ms) {
    taskENTER_CRITICAL(&s_lcd_mux);
    strncpy(s_lcd_override[line], text, 16);
    s_lcd_override[line][16] = '\0';
    int64_t now = esp_timer_get_time();
    int64_t until = now + (int64_t)duration_ms * 1000;
    if (until > s_lcd_override_until)
        s_lcd_override_until = until;
    taskEXIT_CRITICAL(&s_lcd_mux);
}

static bool lcd_override_active() {
    return esp_timer_get_time() < s_lcd_override_until;
}

// Get the active printer based on mode switch (mode 1/2/3 → index 0/1/2)
// Caller must hold s_printer_mutex
static PrinterState *get_active_printer() {
    uint8_t idx = s_active_mode - 1;  // mode is 1-based
    if (idx < MAX_PRINTERS && s_printers[idx].connected)
        return &s_printers[idx];
    return nullptr;
}

// Check if any printer is connected for the active mode (lock-free for status display)
static bool has_active_printer() {
    uint8_t idx = s_active_mode - 1;
    if (idx < MAX_PRINTERS)
        return s_printers[idx].connected;
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
        PrinterState *printer = get_active_printer();
        if (!printer) {
            xSemaphoreGive(s_printer_mutex);
            ESP_LOGW(TAG, "Print requested but no printer for mode %d", s_active_mode);
            lcd_override(0, "NO PRINTER!", 3000);
            continue;
        }

        ESP_LOGI(TAG, "Print requested on %s — rendering label for '%s' (type=%d, days=%d)...",
                 printer->model->name, msg.name, msg.label_type, msg.days);
        lcd_override(0, "PRINTING...", 5000);
        lcd_override(1, msg.name, 5000);

        const uint8_t *fb = nullptr;
        switch ((label_type_t)msg.label_type) {
            case LABEL_NAME:
                fb = label_renderer_render(msg.name);
                break;
            case LABEL_SHORT_PARKING:
                fb = label_renderer_render_parking(msg.name, msg.days, true);
                break;
            case LABEL_LONG_PARKING:
                fb = label_renderer_render_parking(msg.name, msg.days, false);
                break;
        }

        if (fb == nullptr) {
            xSemaphoreGive(s_printer_mutex);
            ESP_LOGE(TAG, "Render failed");
            lcd_override(0, "RENDER ERROR", 3000);
            continue;
        }

        ESP_LOGI(TAG, "Sending to printer...");
        char print_err[17] = {};
        bool ok = brother_ql_print(printer, printer->model, fb, print_err, sizeof(print_err));
        xSemaphoreGive(s_printer_mutex);

        ESP_LOGI(TAG, "Print %s", ok ? "succeeded" : "FAILED");
        if (!ok) {
            lcd_override(0, "PRINT FAILED!", 10000);
            if (print_err[0])
                lcd_override(1, print_err, 10000);
        }
    }
}

// Dedup: ignore same card ID if scanned within 10s of starting a print
#define DEDUP_INTERVAL_US (10 * 1000000LL)
static uint32_t s_last_card_id = 0;
static int64_t  s_last_print_time = 0;

static void enqueue_print(uint32_t card_id, const char *name, label_type_t type, uint8_t days) {
    int64_t now = esp_timer_get_time();
    if (card_id == s_last_card_id && (now - s_last_print_time) < DEDUP_INTERVAL_US) {
        ESP_LOGI(TAG, "Ignoring duplicate card 0x%08lX (within 10s)", (unsigned long)card_id);
        return;
    }

    s_last_card_id = card_id;
    s_last_print_time = now;

    print_msg_t msg;
    strncpy(msg.name, name, sizeof(msg.name) - 1);
    msg.name[sizeof(msg.name) - 1] = '\0';
    msg.label_type = type;
    msg.days = days;
    xQueueSend(s_print_queue, &msg, 0);
}

// Returns status string for line 1, or NULL if ready (SCAN CARD).
static const char *get_status_line() {
    bool first_boot = !sntp_is_synced() && !wifi_ever_connected();
    bool first_ntp = wifi_is_connected() && !sntp_is_synced();
    if (first_boot || first_ntp)    return "NETWORKING...";
    if (!wifi_is_connected())       return "ERR: NO WIFI";
    if (!sntp_is_synced())          return "ERR: NO NTP";
    if (!has_active_printer())      return "ERR: NO PRINTER";
    return nullptr;
}

// Mode-specific idle text for line 1
static const char *get_mode_idle_text() {
    switch (s_active_mode) {
        case 1: return "SCAN CARD";
        case 2: return "SHORT TERM PKG";
        case 3: return "LONG TERM PKG";
        default: return "SCAN CARD";
    }
}

// Format clock line for line 2: "Apr11 - 20:41:45" (16 chars)
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

// Mode 3 state machine
enum long_term_state_t : uint8_t {
    LT_IDLE,       // waiting for card scan
    LT_SELECTING,  // card scanned, selecting days (LED flashing)
};

static long_term_state_t s_lt_state = LT_IDLE;
static char s_lt_name[LOOKUP_NAME_MAX] = {};
static uint32_t s_lt_card_id = 0;
static int s_lt_days = 3;  // default 3 days
static int64_t s_lt_timeout = 0;  // auto-cancel after 30s of no interaction
#define LT_TIMEOUT_US (30 * 1000000LL)
#define LT_MIN_DAYS 3
#define LT_MAX_DAYS 7
#define LED_FLASH_INTERVAL_US (300000LL)  // 300ms half-period = 600ms full cycle

static void lt_cancel() {
    s_lt_state = LT_IDLE;
    s_lt_name[0] = '\0';
    s_lt_card_id = 0;
    s_lt_days = LT_MIN_DAYS;
    led_output_set(false);
}

static void lt_update_lcd() {
    // Show: "LONG TERM PERMIT" / "N days (end date)"
    lcd_override(0, "LONG TERM PERMIT", 31000);

    time_t now;
    time(&now);
    struct tm ti;
    time_t end = now + (time_t)s_lt_days * 86400;
    localtime_r(&end, &ti);

    char buf[17];
    snprintf(buf, sizeof(buf), "%dd to %02d/%02d", s_lt_days, ti.tm_mon + 1, ti.tm_mday);
    lcd_override(1, buf, 31000);
}

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
                strncpy(line0, status, sizeof(line0));
                line0[sizeof(line0) - 1] = '\0';
            } else if (easter_egg && s_active_mode == 1) {
                strncpy(line0, "SCAN HAND", sizeof(line0));
            } else {
                strncpy(line0, get_mode_idle_text(), sizeof(line0));
                line0[sizeof(line0) - 1] = '\0';
            }
            format_clock(line1, sizeof(line1));

            lcd_set_line(0, line0);
            lcd_set_line(1, line1);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "=== Label Scan Station ===");

    // LED: dim white during init
    status_led_init();
    status_led_set(20, 20, 20);

    // Initialize LCD display (auto-detects I2C address and pin order)
    lcd_init();
    lcd_status("Starting up...", "");

    // Initialize printer states
    for (int i = 0; i < MAX_PRINTERS; i++)
        printer_init(&s_printers[i]);

    // Create printer mutex
    s_printer_mutex = xSemaphoreCreateMutex();

    // Initialize mode switch
    mode_switch_init();
    s_active_mode = mode_switch_read();

    // Initialize LED output
    led_output_init();

    // Create print queue
    s_print_queue = xQueueCreate(4, sizeof(print_msg_t));

    // Initialize card lookup DB
    card_lookup_init();

    // Initialize buzzer
    buzzer_init();

    // Initialize WiFi + SNTP
    wifi_init();

    // Initialize renderer
    label_renderer_init();

    // Initialize USB host and set printer callback
    usb_host_set_printer_callback(on_printer_event);
    usb_host_init();

    // Initialize button GPIO (simple input, no ISR — we poll for long-press)
    gpio_config_t btn_conf = {};
    btn_conf.intr_type = GPIO_INTR_DISABLE;
    btn_conf.mode = GPIO_MODE_INPUT;
    btn_conf.pin_bit_mask = (1ULL << BUTTON_GPIO);
    btn_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&btn_conf));

    // Initialize RFID reader
    QueueHandle_t rfid_queue = rfid_init();

    // Start print task
    xTaskCreate(print_task, "print_task", 16384, nullptr, 1, nullptr);

    // Start LCD update task (refreshes clock + status every 500ms)
    xTaskCreate(lcd_update_task, "lcd_update", 2048, nullptr, 1, nullptr);

    // LED: green when ready
    status_led_set(0, 20, 0);

    ESP_LOGI(TAG, "System ready — scan RFID card or press button (1s hold) to test print");

    // Main loop: handle RFID scans and button interaction
    uint32_t card_id;
    int64_t btn_press_start = 0;
    bool btn_triggered = false;
    bool btn_was_down = false;

    while (true) {
        int64_t now = esp_timer_get_time();

        // Flash LED in mode 3 LT_SELECTING state
        if (s_lt_state == LT_SELECTING) {
            bool led_on = ((now / LED_FLASH_INTERVAL_US) % 2) == 0;
            led_output_set(led_on);

            // Auto-cancel after timeout
            if (now > s_lt_timeout) {
                ESP_LOGI(TAG, "Long-term selection timed out");
                lt_cancel();
                lcd_override(0, "TIMED OUT", 2000);
            }
        }

        // Poll mode switch for changes
        uint8_t mode = mode_switch_read();
        if (mode != s_active_mode) {
            // Cancel any in-progress long-term selection on mode change
            if (s_lt_state == LT_SELECTING) {
                lt_cancel();
            }
            s_active_mode = mode;
            ESP_LOGI(TAG, "Mode switch → %d", mode);
            xSemaphoreTake(s_printer_mutex, portMAX_DELAY);
            PrinterState *p = get_active_printer();
            if (p && p->model) {
                char buf[17];
                snprintf(buf, sizeof(buf), "%d: %s", mode, p->model->name);
                lcd_override(0, buf, 2000);
            } else {
                char buf[17];
                snprintf(buf, sizeof(buf), "MODE %d (empty)", mode);
                lcd_override(0, buf, 2000);
            }
            xSemaphoreGive(s_printer_mutex);
        }

        // Check RFID queue (non-blocking)
        if (xQueueReceive(rfid_queue, &card_id, 0) == pdTRUE) {
            ESP_LOGI(TAG, "RFID card scanned: 0x%08lX (mode %d)", (unsigned long)card_id, s_active_mode);
            lookup_result_t result = card_lookup(card_id);

            if (!result.found) {
                buzzer_beep_bad();
                ESP_LOGW(TAG, "Unknown card 0x%08lX", (unsigned long)card_id);
                lcd_override(0, "ERR:UNKNOWN CARD", 3000);
            } else if (!has_active_printer()) {
                buzzer_beep_sad();
                lcd_override(0, result.name, 3000);
                lcd_override(1, "NO PRINTER!", 3000);
                ESP_LOGW(TAG, "Card '%s' OK but no printer", result.name);
            } else {
                // Card is valid and printer is available
                switch (s_active_mode) {
                    case 1:  // Name label — print immediately
                        buzzer_beep_good();
                        lcd_override(0, result.name, 3000);
                        enqueue_print(card_id, result.name, LABEL_NAME, 0);
                        break;

                    case 2:  // Short-term parking — print immediately with 2-day permit
                        buzzer_beep_good();
                        lcd_override(0, result.name, 3000);
                        lcd_override(1, "2-DAY PERMIT", 3000);
                        enqueue_print(card_id, result.name, LABEL_SHORT_PARKING, 2);
                        break;

                    case 3:  // Long-term parking — enter selection mode
                        buzzer_beep_good();
                        s_lt_state = LT_SELECTING;
                        strncpy(s_lt_name, result.name, sizeof(s_lt_name) - 1);
                        s_lt_name[sizeof(s_lt_name) - 1] = '\0';
                        s_lt_card_id = card_id;
                        s_lt_days = LT_MIN_DAYS;
                        s_lt_timeout = now + LT_TIMEOUT_US;
                        lt_update_lcd();
                        ESP_LOGI(TAG, "Long-term mode: '%s', select days (3-7), long-press to print",
                                 result.name);
                        break;
                }
            }
        }

        // Button handling
        bool btn_down = gpio_get_level(BUTTON_GPIO) == 0;

        if (s_lt_state == LT_SELECTING) {
            // Mode 3 selecting: short press = cycle days, long press = print
            if (btn_down) {
                if (btn_press_start == 0) {
                    btn_press_start = now;
                } else if (!btn_triggered && (now - btn_press_start) >= 1000000LL) {
                    // Long press — print the permit
                    btn_triggered = true;
                    ESP_LOGI(TAG, "Long press — printing %d-day permit for '%s'", s_lt_days, s_lt_name);
                    lcd_override(0, "PRINTING...", 5000);
                    lcd_override(1, s_lt_name, 5000);
                    enqueue_print(s_lt_card_id, s_lt_name, LABEL_LONG_PARKING, s_lt_days);
                    lt_cancel();
                }
            } else {
                // Button released
                if (btn_was_down && !btn_triggered && btn_press_start != 0) {
                    // Short press — cycle days
                    s_lt_days++;
                    if (s_lt_days > LT_MAX_DAYS) s_lt_days = LT_MIN_DAYS;
                    s_lt_timeout = now + LT_TIMEOUT_US;  // reset timeout
                    lt_update_lcd();
                    ESP_LOGI(TAG, "Long-term days: %d", s_lt_days);
                }
                btn_press_start = 0;
                btn_triggered = false;
            }
        } else {
            // Normal mode: long press = test print
            if (btn_down) {
                if (btn_press_start == 0) {
                    btn_press_start = now;
                } else if (!btn_triggered && (now - btn_press_start) >= 1000000LL) {
                    btn_triggered = true;
                    ESP_LOGI(TAG, "Button held 1s — test print (mode %d)", s_active_mode);
                    lcd_override(0, "Test print...", 3000);
                    switch (s_active_mode) {
                        case 1:
                            enqueue_print(0, "TEST", LABEL_NAME, 0);
                            break;
                        case 2:
                            enqueue_print(0, "TEST", LABEL_SHORT_PARKING, 2);
                            break;
                        case 3:
                            enqueue_print(0, "TEST", LABEL_LONG_PARKING, 5);
                            break;
                    }
                }
            } else {
                btn_press_start = 0;
                btn_triggered = false;
            }
        }

        btn_was_down = btn_down;

        // Small delay to avoid busy-spinning
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
