#include "app_config.h"
#include "usb_host_task.h"
#include "usb_printer.h"
#include "brother_ql.h"
#include "wifi_manager.h"
#include "label_renderer.h"
// button.h no longer used — GPIO polled directly for long-press
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
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "main";

#define MAX_PRINTERS 3
static PrinterState s_printers[MAX_PRINTERS];
static int s_num_printers = 0;
static uint8_t s_active_mode = 0;  // last known mode switch value

// Print queue carries the looked-up name
typedef struct {
    char name[LOOKUP_NAME_MAX];
} print_msg_t;

static QueueHandle_t s_print_queue = nullptr;

// Transient LCD override (e.g. "Printing...", card scan feedback)
static char s_lcd_override[2][17] = {};  // [line][text], 16 chars + NUL
static int64_t s_lcd_override_until = 0; // microsecond timestamp

static void lcd_override(int line, const char *text, int duration_ms) {
    strncpy(s_lcd_override[line], text, 16);
    s_lcd_override[line][16] = '\0';
    int64_t now = esp_timer_get_time();
    int64_t until = now + (int64_t)duration_ms * 1000;
    if (until > s_lcd_override_until)
        s_lcd_override_until = until;
}

static bool lcd_override_active() {
    return esp_timer_get_time() < s_lcd_override_until;
}

// Get the active printer based on mode switch (mode 1/2/3 → index 0/1/2)
static PrinterState *get_active_printer() {
    uint8_t idx = s_active_mode - 1;  // mode is 1-based
    if (idx < MAX_PRINTERS && s_printers[idx].connected)
        return &s_printers[idx];
    return nullptr;
}

static void on_printer_event(usb_device_handle_t dev_handle, bool connected, const ql_model_t *model) {
    if (connected) {
        if (s_num_printers >= MAX_PRINTERS) {
            ESP_LOGW(TAG, "Max printers reached, ignoring %s", model->name);
            usb_host_device_close(usb_host_get_client_handle(), dev_handle);
            return;
        }
        int slot = s_num_printers;
        s_printers[slot].model = model;
        if (printer_on_connected(&s_printers[slot], dev_handle, usb_host_get_client_handle())) {
            s_num_printers++;
            ESP_LOGI(TAG, "Printer %d ready (%s)", slot + 1, model->name);
        } else {
            ESP_LOGE(TAG, "Printer connection setup failed");
            s_printers[slot].model = nullptr;
            usb_host_device_close(usb_host_get_client_handle(), dev_handle);
        }
    } else {
        // Find the printer that disconnected by matching dev_handle
        for (int i = 0; i < s_num_printers; i++) {
            if (s_printers[i].dev_handle == dev_handle) {
                ESP_LOGW(TAG, "Printer %d disconnected (%s)", i + 1,
                         s_printers[i].model ? s_printers[i].model->name : "unknown");
                printer_on_disconnected(&s_printers[i]);
                s_printers[i].model = nullptr;
                // Shift remaining printers down to keep array packed
                for (int j = i; j < s_num_printers - 1; j++) {
                    s_printers[j] = s_printers[j + 1];
                }
                s_num_printers--;
                // Clear the now-unused last slot
                printer_init(&s_printers[s_num_printers]);
                break;
            }
        }
    }
}

static void print_task(void *arg) {
    ESP_LOGI(TAG, "Print task started");
    print_msg_t msg;

    while (true) {
        if (xQueueReceive(s_print_queue, &msg, portMAX_DELAY) != pdTRUE)
            continue;

        PrinterState *printer = get_active_printer();
        if (!printer) {
            ESP_LOGW(TAG, "Print requested but no printer for mode %d", s_active_mode);
            lcd_override(0, "NO PRINTER!", 3000);
            continue;
        }

        ESP_LOGI(TAG, "Print requested on %s — rendering label for '%s'...",
                 printer->model->name, msg.name);
        lcd_override(0, "PRINTING...", 5000);
        lcd_override(1, msg.name, 5000);
        const uint8_t *fb = label_renderer_render(msg.name);
        if (fb == nullptr) {
            ESP_LOGE(TAG, "Render failed");
            lcd_override(0, "RENDER ERROR", 3000);
            continue;
        }

        ESP_LOGI(TAG, "Sending to printer...");
        char print_err[17] = {};
        bool ok = brother_ql_print(printer, printer->model, fb, print_err, sizeof(print_err));
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

static void enqueue_print(uint32_t card_id, const char *name) {
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
    xQueueSend(s_print_queue, &msg, 0);
}

// Returns status string for line 1, or NULL if ready (SCAN CARD).
// First boot: "NETWORKING..." until WiFi+NTP succeed.
// After that, real errors with precedence: NO WIFI > NO NTP > NO PRINTER
static const char *get_status_line() {
    bool first_boot = !sntp_is_synced() && !wifi_ever_connected();
    bool first_ntp = wifi_is_connected() && !sntp_is_synced();
    if (first_boot || first_ntp)    return "NETWORKING...";
    if (!wifi_is_connected())       return "ERR: NO WIFI";
    if (!sntp_is_synced())          return "ERR: NO NTP";
    if (!get_active_printer())       return "ERR: NO PRINTER";
    // Future: OUT OF PAPER, PRINTER ERROR from status polling
    return nullptr;
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

static void lcd_update_task(void *arg) {
    char line0[17];
    char line1[17];
    int64_t easter_egg_start = esp_timer_get_time();

    while (true) {
        int64_t now = esp_timer_get_time();
        int64_t elapsed = (now - easter_egg_start) % EASTER_EGG_INTERVAL_US;
        bool easter_egg = elapsed < EASTER_EGG_DURATION_US;

        if (lcd_override_active()) {
            lcd_set_line(0, s_lcd_override[0]);
            if (s_lcd_override[1][0] != '\0') {
                lcd_set_line(1, s_lcd_override[1]);
            } else {
                format_clock(line1, sizeof(line1));
                lcd_set_line(1, line1);
            }
        } else {
            const char *status = get_status_line();
            if (status) {
                strncpy(line0, status, sizeof(line0));
                line0[sizeof(line0) - 1] = '\0';
            } else if (easter_egg) {
                strncpy(line0, "SCAN HAND", sizeof(line0));
            } else {
                strncpy(line0, "SCAN CARD", sizeof(line0));
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

    // Initialize mode switch
    mode_switch_init();
    s_active_mode = mode_switch_read();

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
    gpio_config(&btn_conf);

    // Initialize RFID reader
    QueueHandle_t rfid_queue = rfid_init();

    // Start print task
    xTaskCreate(print_task, "print_task", 16384, nullptr, 1, nullptr);

    // Start LCD update task (refreshes clock + status every 500ms)
    xTaskCreate(lcd_update_task, "lcd_update", 2048, nullptr, 1, nullptr);

    // LED: green when ready
    status_led_set(0, 20, 0);

    ESP_LOGI(TAG, "System ready — scan RFID card or press button (1s hold) to test print");

    // Main loop: handle RFID scans and button long-press
    uint32_t card_id;
    int64_t btn_press_start = 0;
    bool btn_triggered = false;

    while (true) {
        // Check RFID queue (non-blocking)
        if (xQueueReceive(rfid_queue, &card_id, 0) == pdTRUE) {
            ESP_LOGI(TAG, "RFID card scanned: 0x%08lX", (unsigned long)card_id);
            lookup_result_t result = card_lookup(card_id);
            if (result.found) {
                if (get_active_printer()) {
                    buzzer_beep_good();
                    lcd_override(0, result.name, 3000);
                    enqueue_print(card_id, result.name);
                } else {
                    buzzer_beep_sad();
                    lcd_override(0, result.name, 3000);
                    lcd_override(1, "NO PRINTER!", 3000);
                    ESP_LOGW(TAG, "Card '%s' OK but no printer", result.name);
                }
            } else {
                buzzer_beep_bad();
                ESP_LOGW(TAG, "Unknown card 0x%08lX", (unsigned long)card_id);
                lcd_override(0, "ERR:UNKNOWN CARD", 3000);
            }
        }

        // Button long-press detection (1 second hold)
        bool btn_down = gpio_get_level(BUTTON_GPIO) == 0;
        if (btn_down) {
            if (btn_press_start == 0) {
                btn_press_start = esp_timer_get_time();
            } else if (!btn_triggered &&
                       (esp_timer_get_time() - btn_press_start) >= 1000000LL) {
                btn_triggered = true;
                ESP_LOGI(TAG, "Button held 1s — test print");
                lcd_override(0, "Test print...", 3000);
                enqueue_print(0, "TEST");
            }
        } else {
            btn_press_start = 0;
            btn_triggered = false;
        }

        // Poll mode switch for changes
        uint8_t mode = mode_switch_read();
        if (mode != s_active_mode) {
            s_active_mode = mode;
            ESP_LOGI(TAG, "Mode switch → %d", mode);
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
        }

        // Small delay to avoid busy-spinning
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
