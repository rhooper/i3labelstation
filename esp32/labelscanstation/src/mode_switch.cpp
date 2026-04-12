#include "mode_switch.h"
#include "driver/gpio.h"

#define MODE_PIN1 GPIO_NUM_37
#define MODE_PIN2 GPIO_NUM_36

void mode_switch_init() {
    gpio_config_t cfg = {};
    cfg.intr_type = GPIO_INTR_DISABLE;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pin_bit_mask = (1ULL << MODE_PIN1) | (1ULL << MODE_PIN2);
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&cfg);
}

uint8_t mode_switch_read() {
    bool pin1_low = gpio_get_level(MODE_PIN1) == 0;
    bool pin2_low = gpio_get_level(MODE_PIN2) == 0;

    if (pin1_low && !pin2_low) return 1;
    if (!pin1_low && pin2_low) return 3;
    return 2;  // both floating or both grounded
}
