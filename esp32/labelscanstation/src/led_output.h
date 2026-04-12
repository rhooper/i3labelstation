#pragma once

// Initialize LED output GPIO (held LOW by default).
void led_output_init();

// Set LED on or off.
void led_output_set(bool on);
