#pragma once

#include <cstdint>

// 3-state mode switch on GPIO36/GPIO37.
// Mode 1: GPIO37 pulled to ground
// Mode 2: both floating (or both grounded)
// Mode 3: GPIO36 pulled to ground
void mode_switch_init();

// Returns current mode (1, 2, or 3).
uint8_t mode_switch_read();
