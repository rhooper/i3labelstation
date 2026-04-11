#pragma once

// Initialize the onboard WS2812 RGB LED.
void status_led_init();

// Set LED color (0-255 each).
void status_led_set(int r, int g, int b);
