#pragma once

#include <cstdint>

// Label types
enum label_type_t : uint8_t {
    LABEL_NAME,           // Mode 1: name label (logo + name + date)
    LABEL_SHORT_PARKING,  // Mode 2: short-term parking permit (2 days)
    LABEL_LONG_PARKING,   // Mode 3: long-term parking permit (N days)
};

// Initialize the label renderer (TTF font + framebuffer).
void label_renderer_init();

// Render a name label (logo + name + today's date).
const uint8_t *label_renderer_render(const char *name);

// Render a parking permit label.
// name: member name
// days: permit validity in days (from today)
const uint8_t *label_renderer_render_parking(const char *name, int days, bool short_term);
