#include "app_config.h"
#include "usb_host_task.h"
#include "usb_printer.h"
#include "brother_ql.h"
#include "label_renderer.h"
#include "rfid_reader.h"
#include "card_lookup.h"
#include "buzzer.h"
#include "version.h"

#include <cstring>
#include <cstdio>
#include <ctime>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <Wire.h>
#include <WiFi.h>
#include <LiquidCrystal_I2C.h>
#include <FastLED.h>

#include "secrets.h"

static const char *TAG = "main";

// ---- Hardware objects ----
static LiquidCrystal_I2C s_lcd(0x27, 16, 2);
static CRGB s_leds[1];

// ---- Printer state ----
static PrinterState s_printers[MAX_PRINTERS];
static SemaphoreHandle_t s_printer_mutex = nullptr;

// ---- Print queue ----
typedef struct {
    char name[LOOKUP_NAME_MAX];
} print_msg_t;
static QueueHandle_t s_print_queue = nullptr;

// ---- LCD override ----
static char s_lcd_override[2][17] = {};
static int64_t s_lcd_override_until = 0;
static portMUX_TYPE s_lcd_mux = portMUX_INITIALIZER_UNLOCKED;

static void lcd_override(int line, const char *text, int duration_ms) {
    taskENTER_CRITICAL(&s_lcd_mux);
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

// ---- WiFi / NTP helpers ----
static bool wifi_is_connected() {
    return WiFi.status() == WL_CONNECTED;
}

static bool sntp_is_synced() {
    return time(nullptr) > 1000000000UL;
}

static bool s_wifi_ever_connected = false;

// ---- LCD helpers ----
// Write exactly 16 chars to the LCD line, padding short text with spaces.
// This matches the v1 behaviour of writing to all 16 DDRAM positions so
// that shorter strings clear any leftover characters from previous content.
static void lcd_set_line(int line, const char *text) {
    s_lcd.setCursor(0, line);
    int len = strlen(text);
    for (int i = 0; i < 16; i++) {
        s_lcd.write(i < len ? text[i] : ' ');
    }
}

static void lcd_status(const char *line0, const char *line1) {
    lcd_set_line(0, line0);
    lcd_set_line(1, line1);
}

// ---- Printer management ----
// Caller must hold s_printer_mutex.
static PrinterState *get_active_printer() {
    PrinterState *best = nullptr;
    for (int i = 0; i < MAX_PRINTERS; i++) {
        if (s_printers[i].connected && s_printers[i].model) {
            if (!best || s_printers[i].model->usb_pid > best->model->usb_pid)
                best = &s_printers[i];
        }
    }
    return best;
}

static bool has_active_printer() {
    xSemaphoreTake(s_printer_mutex, portMAX_DELAY);
    bool found = false;
    for (int i = 0; i < MAX_PRINTERS; i++) {
        if (s_printers[i].connected) { found = true; break; }
    }
    xSemaphoreGive(s_printer_mutex);
    return found;
}

static void on_printer_event(usb_device_handle_t dev_handle, bool connected, const ql_model_t *model) {
    xSemaphoreTake(s_printer_mutex, portMAX_DELAY);

    if (connected) {
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
            brother_ql_disable_auto_off(&s_printers[slot], model);
            ESP_LOGI(TAG, "Printer %d ready (%s)", slot + 1, model->name);
        } else {
            ESP_LOGE(TAG, "Printer connection setup failed");
            s_printers[slot].model = nullptr;
            xSemaphoreGive(s_printer_mutex);
            usb_host_device_close(usb_host_get_client_handle(), dev_handle);
            return;
        }
    } else {
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

// ---- Print task ----
static void print_task(void *arg) {
    ESP_LOGI(TAG, "Print task started");
    print_msg_t msg;

    while (true) {
        if (xQueueReceive(s_print_queue, &msg, portMAX_DELAY) != pdTRUE)
            continue;

        // Render first — pure computation, no shared state
        const uint8_t *fb = label_renderer_render(msg.name);
        if (fb == nullptr) {
            ESP_LOGE(TAG, "Render failed");
            lcd_override(0, "RENDER ERROR", 3000);
            continue;
        }

        // Brief lock: get printer pointer and model, then release.
        // USB I/O runs without the lock so a disconnect can't deadlock.
        xSemaphoreTake(s_printer_mutex, portMAX_DELAY);
        PrinterState *printer = get_active_printer();
        const ql_model_t *model = printer ? printer->model : nullptr;
        xSemaphoreGive(s_printer_mutex);

        if (!printer) {
            ESP_LOGW(TAG, "Print requested but no printer connected");
            lcd_override(0, "NO PRINTER!", 3000);
            continue;
        }

        ESP_LOGI(TAG, "Printing '%s' on %s...", msg.name, model->name);
        lcd_override(0, "PRINTING...", 5000);
        lcd_override(1, msg.name, 5000);

        char print_err[17] = {};
        bool ok = brother_ql_print(printer, model, fb, print_err, sizeof(print_err));

        ESP_LOGI(TAG, "Print %s", ok ? "succeeded" : "FAILED");
        if (!ok) {
            lcd_override(0, "PRINT FAILED!", 10000);
            if (print_err[0])
                lcd_override(1, print_err, 10000);
        }
    }
}

// ---- Dedup ----
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

// ---- LCD update task ----
static const char *get_status_line() {
    bool first_boot = !sntp_is_synced() && !s_wifi_ever_connected;
    bool first_ntp  = wifi_is_connected() && !sntp_is_synced();
    if (first_boot || first_ntp)    return "NETWORKING...";
    if (!wifi_is_connected())       return "ERR: NO WIFI";
    if (!sntp_is_synced())          return "ERR: NO NTP";
    if (card_lookup_count() == 0)   return "ERR: MEMBERDB";
    if (!has_active_printer())      return "ERR: NO PRINTER";
    return nullptr;
}

#define SCAN_CYCLE_INTERVAL_US (3 * 1000000LL)

static void format_idle_line(char *buf, size_t len, int64_t now) {
    bool show_fob = ((now / SCAN_CYCLE_INTERVAL_US) % 2) == 1;
    snprintf(buf, len, "%s", show_fob ? "SCAN FOB" : "SCAN CARD");
}

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
            } else if (easter_egg) {
                snprintf(line0, sizeof(line0), "SCAN HAND");
            } else {
                format_idle_line(line0, sizeof(line0), now);
            }
            format_clock(line1, sizeof(line1));

            lcd_set_line(0, line0);
            lcd_set_line(1, line1);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ---- Arduino entry points ----

static QueueHandle_t s_rfid_queue = nullptr;
static bool s_cards_loaded = false;
static int64_t s_btn_press_start = 0;
static bool s_btn_triggered = false;

void setup() {
    Serial.begin(115200);
    ESP_LOGI(TAG, "=== Label Scan Station v%s ===", FIRMWARE_VERSION);

    // LED: dim white during init
    FastLED.addLeds<WS2812, RGB_LED_GPIO, GRB>(s_leds, 1);
    s_leds[0] = CRGB(20, 20, 20);
    FastLED.show();

    // LCD init — must set I2C pins before library init
    Wire.begin(LCD_SDA_GPIO, LCD_SCL_GPIO);
    s_lcd.init();
    s_lcd.backlight();
    lcd_status("LabelStation v" FIRMWARE_MAJOR_STR, "Starting...");

    // Printer states
    for (int i = 0; i < MAX_PRINTERS; i++)
        printer_init(&s_printers[i]);
    s_printer_mutex = xSemaphoreCreateMutex();
    s_print_queue = xQueueCreate(4, sizeof(print_msg_t));

    // Card DB (loads extra_cards immediately)
    card_lookup_init();

    // Buzzer
    buzzer_init();

    // WiFi + NTP (non-blocking: loop() handles post-connect work)
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();

    // Renderer
    label_renderer_init();

    // USB host
    usb_host_set_printer_callback(on_printer_event);
    usb_host_init();

    // Button GPIO
    pinMode(BUTTON_GPIO, INPUT_PULLUP);

    // RFID
    s_rfid_queue = rfid_init();

    // Tasks
    xTaskCreate(print_task, "print_task", 16384, nullptr, 1, nullptr);
    xTaskCreate(lcd_update_task, "lcd_update", 2048, nullptr, 1, nullptr);
    card_lookup_start_refresh_task();

    // LED: green when ready
    s_leds[0] = CRGB(0, 20, 0);
    FastLED.show();

    ESP_LOGI(TAG, "System ready — scan RFID card or hold button 1s to test print");
}

void loop() {
    int64_t now = esp_timer_get_time();

    // Track WiFi ever-connected for status display
    if (!s_wifi_ever_connected && wifi_is_connected()) {
        s_wifi_ever_connected = true;
    }

    // Late WiFi connect: fetch card DB if not yet loaded
    if (!s_cards_loaded && wifi_is_connected()) {
        s_cards_loaded = true;
        card_lookup_refresh();
    }

    // RFID queue (non-blocking)
    uint32_t card_id;
    if (xQueueReceive(s_rfid_queue, &card_id, 0) == pdTRUE) {
        ESP_LOGI(TAG, "RFID card scanned: 0x%08lX", (unsigned long)card_id);
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
            buzzer_beep_good();
            lcd_override(0, result.name, 3000);
            enqueue_print(card_id, result.name);
        }
    }

    // Button: long press = test print
    bool btn_down = digitalRead(BUTTON_GPIO) == LOW;
    if (btn_down) {
        if (s_btn_press_start == 0) {
            s_btn_press_start = now;
        } else if (!s_btn_triggered && (now - s_btn_press_start) >= 1000000LL) {
            s_btn_triggered = true;
            ESP_LOGI(TAG, "Button held 1s — test print");
            lcd_override(0, "Test print...", 3000);
            enqueue_print(0, "TEST");
        }
    } else {
        s_btn_press_start = 0;
        s_btn_triggered = false;
    }

    delay(10);
}
