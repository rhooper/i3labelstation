#pragma once

#include <cstdint>

// Initialize LVGL and the headless 1-bit display.
void label_renderer_init();

// Render the label content into the framebuffer.
// card_id: text to display as the name/ID line (e.g. "0x002ED47F" or a looked-up name).
// Returns pointer to the internal framebuffer (LABEL_FB_SIZE bytes, valid until next render).
const uint8_t *label_renderer_render(const char *card_id);
