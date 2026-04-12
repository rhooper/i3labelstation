#include "led_output.h"
#include "app_config.h"
#include "driver/gpio.h"
#include "esp_check.h"

void led_output_init() {
    gpio_config_t cfg = {};
    cfg.intr_type = GPIO_INTR_DISABLE;
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pin_bit_mask = (1ULL << LED_OUTPUT_GPIO);
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(LED_OUTPUT_GPIO, 0);
}

void led_output_set(bool on) {
    gpio_set_level(LED_OUTPUT_GPIO, on ? 1 : 0);
}
