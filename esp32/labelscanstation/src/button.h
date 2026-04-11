#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Initialize button GPIO with ISR + debounce. Returns queue that receives
// a uint8_t (1) on each press.
QueueHandle_t button_init();
