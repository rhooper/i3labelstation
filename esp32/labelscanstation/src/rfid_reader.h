#pragma once

#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Initialize UART for HZ-1050 RFID reader.
// Returns a queue that receives 32-bit card IDs.
QueueHandle_t rfid_init();
