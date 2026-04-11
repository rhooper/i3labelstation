#include "app_config.h"
#include "usb_host_task.h"
#include "usb_printer.h"
#include "brother_ql.h"
#include "wifi_manager.h"
#include "label_renderer.h"
#include "button.h"
#include "rfid_reader.h"
#include "status_led.h"

#include <cstring>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "main";

static PrinterState s_printer;

// Print queue carries a card ID string
#define CARD_ID_MAX_LEN 20
typedef struct {
    char card_id[CARD_ID_MAX_LEN];
} print_msg_t;

static QueueHandle_t s_print_queue = nullptr;

static void on_printer_event(usb_device_handle_t dev_handle, bool connected) {
    if (connected) {
        if (printer_on_connected(&s_printer, dev_handle, usb_host_get_client_handle())) {
            ESP_LOGI(TAG, "Printer ready");
        } else {
            ESP_LOGE(TAG, "Printer connection setup failed");
            usb_host_device_close(usb_host_get_client_handle(), dev_handle);
        }
    } else {
        printer_on_disconnected(&s_printer);
    }
}

static void print_task(void *arg) {
    ESP_LOGI(TAG, "Print task started");
    print_msg_t msg;

    while (true) {
        if (xQueueReceive(s_print_queue, &msg, portMAX_DELAY) != pdTRUE)
            continue;

        if (!s_printer.connected) {
            ESP_LOGW(TAG, "Print requested but printer not connected");
            continue;
        }

        ESP_LOGI(TAG, "Print requested — rendering label for '%s'...", msg.card_id);
        const uint8_t *fb = label_renderer_render(msg.card_id);
        if (fb == nullptr) {
            ESP_LOGE(TAG, "Render failed");
            continue;
        }

        ESP_LOGI(TAG, "Sending to printer...");
        bool ok = brother_ql_print(&s_printer, fb);
        ESP_LOGI(TAG, "Print %s", ok ? "succeeded" : "FAILED");
    }
}

// Dedup: ignore same card ID if scanned within 10s of starting a print
#define DEDUP_INTERVAL_US (10 * 1000000LL)  // 10 seconds in microseconds
static uint32_t s_last_card_id = 0;
static int64_t  s_last_print_time = 0;

static void enqueue_print(uint32_t card_id) {
    int64_t now = esp_timer_get_time();
    if (card_id == s_last_card_id && (now - s_last_print_time) < DEDUP_INTERVAL_US) {
        ESP_LOGI(TAG, "Ignoring duplicate card 0x%08lX (within 10s)", (unsigned long)card_id);
        return;
    }

    s_last_card_id = card_id;
    s_last_print_time = now;

    print_msg_t msg;
    snprintf(msg.card_id, sizeof(msg.card_id), "0x%08lX", (unsigned long)card_id);
    xQueueSend(s_print_queue, &msg, 0);
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "=== Label Scan Station ===");

    // LED: dim white during init
    status_led_init();
    status_led_set(20, 20, 20);

    // Initialize printer state
    printer_init(&s_printer);

    // Create print queue
    s_print_queue = xQueueCreate(4, sizeof(print_msg_t));

    // Initialize WiFi + SNTP
    wifi_init();

    // Initialize renderer
    label_renderer_init();

    // Initialize USB host and set printer callback
    usb_host_set_printer_callback(on_printer_event);
    usb_host_init();

    // Initialize button
    QueueHandle_t button_queue = button_init();

    // Initialize RFID reader
    QueueHandle_t rfid_queue = rfid_init();

    // Start print task
    xTaskCreate(print_task, "print_task", 16384, nullptr, 1, nullptr);

    // Drain any spurious button presses from boot
    uint8_t dummy;
    while (xQueueReceive(button_queue, &dummy, pdMS_TO_TICKS(100)) == pdTRUE) {}

    // LED: green when ready
    status_led_set(0, 20, 0);

    ESP_LOGI(TAG, "System ready — scan RFID card or press button to print");

    // Main loop: handle both button presses and RFID scans
    uint8_t btn;
    uint32_t card_id;
    while (true) {
        // Check RFID queue (non-blocking)
        if (xQueueReceive(rfid_queue, &card_id, 0) == pdTRUE) {
            ESP_LOGI(TAG, "RFID card scanned: 0x%08lX", (unsigned long)card_id);
            enqueue_print(card_id);
        }

        // Check button queue (non-blocking)
        if (xQueueReceive(button_queue, &btn, 0) == pdTRUE) {
            ESP_LOGI(TAG, "Button pressed!");
            // Button uses card_id 0 (test print)
            enqueue_print(0);
        }

        // Small delay to avoid busy-spinning
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
