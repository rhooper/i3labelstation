#include "status_led.h"
#include "app_config.h"

#include "led_strip.h"
#include "led_strip_rmt.h"
#include "esp_log.h"

static const char *TAG = "led";
static led_strip_handle_t s_strip = nullptr;

void status_led_init() {
    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = RGB_LED_GPIO;
    strip_config.max_leds = 1;
    strip_config.led_model = LED_MODEL_WS2812;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_config.resolution_hz = 10 * 1000 * 1000;

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED strip init failed: %s", esp_err_to_name(err));
        return;
    }
    led_strip_clear(s_strip);
    ESP_LOGI(TAG, "RGB LED initialized on GPIO%d", RGB_LED_GPIO);
}

void status_led_set(int r, int g, int b) {
    if (!s_strip) return;
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}
