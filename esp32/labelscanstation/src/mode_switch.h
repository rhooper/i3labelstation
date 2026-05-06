#pragma once

typedef void (*mode_switch_change_cb_t)(int new_mode);

// Configure GPIO 36 + 37 as inputs with pull-up. Idempotent.
void mode_switch_init(void);

// Sample both pins, return decoded mode 1..3, or 0 if invalid (A=gnd, B=open).
int mode_switch_read(void);

// Last debounced mode (1, 2, or 3). Defaults to 1 until the first stable read.
int mode_switch_current(void);

// Optional: callback invoked from the polling task when current mode changes.
void mode_switch_set_change_cb(mode_switch_change_cb_t cb);

// Spawn the FreeRTOS polling task. Call once after init.
void mode_switch_start_task(void);
