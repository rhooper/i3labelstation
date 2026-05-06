#pragma once

#include <cstdint>

// Render request — populated by the print task and passed to the renderer.
typedef struct {
    int mode;                 // 1 = NORMAL, 2 = SHORT, 3 = PERMIT
    int days;                 // PERMIT only: number of days from today
    const char *name;
    const char *email;        // empty string if not present
    const char *phone;        // empty string if not present
    uint16_t fb_w;            // printable framebuffer width  (e.g. 306)
    uint16_t fb_h;            // printable framebuffer height (e.g. 991, or shorter)
    uint8_t  media_type;      // 0x0A = continuous, 0x0B = die-cut
    uint8_t  media_length_mm; // 0 for continuous tape
} label_render_req_t;

// Initialize the label renderer (TTF font + framebuffer).
void label_renderer_init();

// Render a label per the request. Returns pointer to 1-bit framebuffer
// (MSB first) of size req->fb_w x req->fb_h, or nullptr on error.
const uint8_t *label_renderer_render(const label_render_req_t *req);

// Get the stride (bytes per row) of the last rendered framebuffer.
uint16_t label_renderer_stride();
