#pragma once

#include <cstdint>
#include <cstddef>

struct PrinterState;
struct ql_model_t;

// Brother QL status response (32 bytes)
struct BrotherQLStatus {
    uint8_t error_info_1;
    uint8_t error_info_2;
    uint8_t media_width;
    uint8_t media_type;
    uint8_t media_length;
    uint8_t status_type;  // 0x00=reply, 0x01=complete, 0x02=error
    uint8_t phase_type;
    bool valid;
};

// Parse a 32-byte status response from the printer.
BrotherQLStatus brother_ql_parse_status(const uint8_t *data, size_t len);

// Send command to disable auto power-off. Persists on printer.
bool brother_ql_disable_auto_off(PrinterState *printer, const ql_model_t *model);

// Print a label: stream header + raster rows + print command, then read status.
// framebuffer: 1-bit packed pixel data (MSB first), LABEL_FB_STRIDE * LABEL_PRINTABLE_H bytes.
// model: printer model capabilities (invalidate count, mode setting, etc.)
bool brother_ql_print(PrinterState *printer, const ql_model_t *model, const uint8_t *framebuffer,
                      char *error_msg = nullptr, size_t error_msg_len = 0);
