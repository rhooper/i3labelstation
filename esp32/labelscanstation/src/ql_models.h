#pragma once

#include <cstdint>

struct ql_model_t {
    uint16_t usb_pid;
    const char *name;
    uint8_t bytes_per_row;
    uint16_t num_invalidate;
    bool compression;
    bool mode_setting;
    bool expanded_mode;
    bool cutting;
    bool two_color;
};

// Look up model by USB PID. Returns nullptr if not a known Brother QL.
const ql_model_t *ql_model_lookup(uint16_t pid);

// Media profile for a given tape/label width
struct media_profile_t {
    uint8_t  width_mm;       // tape width in mm
    uint16_t printable_px;   // printable pixels across the width
    uint8_t  right_margin;   // right margin in pixels
    uint8_t  feed_margin;    // feed margin for continuous tape (0 for die-cut)
};

// Look up media profile by width in mm. Returns nullptr if unknown width.
const media_profile_t *ql_media_lookup(uint8_t width_mm);
