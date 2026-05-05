#pragma once

#include <cstdint>
#include <cstddef>

struct PrinterState;
struct ql_model_t;
struct media_profile_t;

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

// Query the printer for its current media. Returns the detected media profile,
// or nullptr if no valid status or unknown media width.
// Optionally fills status_out with the full parsed status.
const media_profile_t *brother_ql_query_media(PrinterState *printer, const ql_model_t *model,
                                               BrotherQLStatus *status_out = nullptr);

// Print a label with explicit framebuffer dimensions.
// fb_width/fb_height: pixel dimensions of the rendered framebuffer
// fb_stride: bytes per row in the framebuffer
// The function queries the printer for current media and adapts the raster output.
// If detected_profile_out is non-null, it receives the media profile detected during print.
bool brother_ql_print(PrinterState *printer, const ql_model_t *model,
                      const uint8_t *framebuffer, uint16_t fb_width, uint16_t fb_height, uint16_t fb_stride,
                      char *error_msg = nullptr, size_t error_msg_len = 0,
                      const media_profile_t **detected_profile_out = nullptr);
