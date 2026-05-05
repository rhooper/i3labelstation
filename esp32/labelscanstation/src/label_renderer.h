#pragma once

#include <cstdint>

// Initialize the label renderer (TTF font + framebuffer).
void label_renderer_init();

// Render a name label adapted to the given pixel dimensions.
// width: printable pixels across the label width
// height: printable pixels along the label length (raster lines)
// Returns pointer to 1-bit framebuffer (MSB first), or nullptr on error.
const uint8_t *label_renderer_render(const char *name, uint16_t width, uint16_t height);

// Get the stride (bytes per row) of the last rendered framebuffer.
uint16_t label_renderer_stride();
