#pragma once

#include <cstdint>

// HD44780 LCD via PCF8574 I2C backpack.
// Displays system status on a 16x2 or 20x4 character LCD.

// Initialize I2C and LCD. Tries both 0x27 and 0x3F addresses.
// Tries configured SDA/SCL first, then swapped, logging which worked.
void lcd_init();

// Set text on a specific line (0-based). Truncated to display width.
void lcd_set_line(int line, const char *text);

// Convenience: update the two status lines.
// line0 = top line (e.g. "WiFi: connected")
// line1 = bottom line (e.g. "Printer: ready")
void lcd_status(const char *line0, const char *line1);

// Clear the display.
void lcd_clear();

// Turn backlight on/off.
void lcd_backlight(bool on);

// Define a custom character in CGRAM (slot 0-7).
// pattern is 8 bytes, each byte's lower 5 bits define one row.
void lcd_create_char(uint8_t slot, const uint8_t *pattern);
