// Simple test firmware: init USB host, wait for printer, send test print.
// Adapts to whatever media is loaded (12mm, 29mm, etc.)
//
// To use: rename main.cpp → main.cpp.bak, rename this → main.cpp
// Or add to platformio.ini: build_src_filter = +<*> -<main.cpp>

#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "app_config.h"
#include "usb_host_task.h"
#include "usb_printer.h"
#include "brother_ql.h"
#include "ql_models.h"
#include "label_renderer.h"

static const char *TAG = "test_print";

static PrinterState s_printer;
static const ql_model_t *s_model = nullptr;
static volatile bool s_printer_ready = false;

static void on_printer_event(usb_device_handle_t dev_handle, bool connected, const ql_model_t *model) {
    if (connected) {
        ESP_LOGI(TAG, "Printer connected: %s", model->name);
        s_model = model;
        if (printer_on_connected(&s_printer, dev_handle, usb_host_get_client_handle())) {
            s_printer.model = model;
            s_printer_ready = true;
        }
    } else {
        ESP_LOGW(TAG, "Printer disconnected");
        s_printer_ready = false;
        printer_on_disconnected(&s_printer);
        s_model = nullptr;
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "=== Test Print Firmware ===");
    ESP_LOGI(TAG, "Waiting for Brother QL printer on USB...");

    // Init subsystems
    printer_init(&s_printer);
    label_renderer_init();
    usb_host_set_printer_callback(on_printer_event);
    usb_host_init();

    // Wait for printer
    while (!s_printer_ready) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "Printer ready: %s", s_model->name);

    // Small delay to let printer settle
    vTaskDelay(pdMS_TO_TICKS(500));

    // Query media to determine render dimensions
    const media_profile_t *profile = brother_ql_query_media(&s_printer, s_model);
    uint16_t render_w = profile ? profile->printable_px : LABEL_PRINTABLE_W;
    uint16_t render_h = LABEL_PRINTABLE_H;

    ESP_LOGI(TAG, "Rendering test label (%dx%d)...", render_w, render_h);
    const uint8_t *fb = label_renderer_render("TEST PRINT", render_w, render_h);

    // Print it
    ESP_LOGI(TAG, "Sending to printer...");
    uint16_t fb_stride = label_renderer_stride();
    char error_msg[64] = {};
    bool ok = brother_ql_print(&s_printer, s_model, fb, render_w, render_h, fb_stride,
                               error_msg, sizeof(error_msg));

    if (ok) {
        ESP_LOGI(TAG, "*** TEST PRINT SUCCESSFUL ***");
    } else {
        ESP_LOGE(TAG, "*** PRINT FAILED: %s ***", error_msg);
    }

    // Done — idle forever
    ESP_LOGI(TAG, "Test complete. Halting.");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
