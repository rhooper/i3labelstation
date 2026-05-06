#include "mode_switch.h"
#include "app_config.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "mode_switch";

#define POLL_INTERVAL_MS 50

static int s_current_mode = 1;
static mode_switch_change_cb_t s_change_cb = nullptr;

void mode_switch_init(void) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << MODE_SWITCH_A_GPIO) | (1ULL << MODE_SWITCH_B_GPIO);
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
    ESP_LOGI(TAG, "Initialized (A=GPIO%d, B=GPIO%d)", MODE_SWITCH_A_GPIO, MODE_SWITCH_B_GPIO);
}

int mode_switch_read(void) {
    int a = gpio_get_level((gpio_num_t)MODE_SWITCH_A_GPIO);  // 1=open, 0=gnd
    int b = gpio_get_level((gpio_num_t)MODE_SWITCH_B_GPIO);
    if (a == 1 && b == 1) return 1;
    if (a == 1 && b == 0) return 2;
    if (a == 0 && b == 0) return 3;
    return 0;
}

int mode_switch_current(void) {
    return s_current_mode;
}

void mode_switch_set_change_cb(mode_switch_change_cb_t cb) {
    s_change_cb = cb;
}

static void mode_switch_task(void *arg) {
    int last = mode_switch_read();
    if (last >= 1 && last <= 3) s_current_mode = last;

    int candidate = last;
    int candidate_count = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
        int now = mode_switch_read();
        if (now == 0) {
            candidate_count = 0;
            continue;
        }
        if (now == candidate) {
            if (++candidate_count >= 2 && s_current_mode != candidate) {
                s_current_mode = candidate;
                ESP_LOGI(TAG, "Mode -> %d", s_current_mode);
                if (s_change_cb) s_change_cb(s_current_mode);
            }
        } else {
            candidate = now;
            candidate_count = 1;
        }
    }
}

void mode_switch_start_task(void) {
    xTaskCreate(mode_switch_task, "mode_sw", 2048, nullptr, 4, nullptr);
}
