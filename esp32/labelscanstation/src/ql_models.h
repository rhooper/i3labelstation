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
