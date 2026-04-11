#include "button.h"
#include "app_config.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "button";

static QueueHandle_t s_button_queue = nullptr;
static int64_t s_last_press_us = 0;
#define DEBOUNCE_US 250000  // 250ms

static void IRAM_ATTR gpio_isr_handler(void *arg) {
    int64_t now = esp_timer_get_time();
    if (now - s_last_press_us < DEBOUNCE_US)
        return;
    s_last_press_us = now;

    uint8_t val = 1;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(s_button_queue, &val, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

QueueHandle_t button_init() {
    s_button_queue = xQueueCreate(4, sizeof(uint8_t));

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_NEGEDGE;  // Button press = falling edge
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << BUTTON_GPIO);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, gpio_isr_handler, nullptr);

    ESP_LOGI(TAG, "Button initialized on GPIO%d", BUTTON_GPIO);
    return s_button_queue;
}
