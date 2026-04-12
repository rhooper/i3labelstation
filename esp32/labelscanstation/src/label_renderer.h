#pragma once

#include <cstdint>

// Initialize the label renderer (TTF font + framebuffer).
void label_renderer_init();

// Render a name label (logo + name + today's date).
const uint8_t *label_renderer_render(const char *name);
