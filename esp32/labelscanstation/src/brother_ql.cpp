#include "brother_ql.h"
#include "usb_printer.h"
#include "ql_models.h"
#include "app_config.h"

#include <cstring>
#include <cstdio>
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "brother_ql";

// Protocol command bytes
static constexpr uint8_t CMD_ESC             = 0x1B;
static constexpr uint8_t CMD_INIT            = 0x40;  // ESC @
static constexpr uint8_t CMD_STATUS_INFO     = 0x69;  // ESC i ...
static constexpr uint8_t CMD_STATUS_REQUEST  = 0x53;  // ESC i S
static constexpr uint8_t CMD_MODE_SETTING    = 0x4D;  // ESC i M
static constexpr uint8_t CMD_EXPANDED_MODE   = 0x4B;  // ESC i K
static constexpr uint8_t CMD_MEDIA_QUALITY   = 0x7A;  // ESC i z
static constexpr uint8_t CMD_AUTO_CUT        = 0x41;  // ESC i A
static constexpr uint8_t CMD_MARGINS         = 0x64;  // ESC i d
static constexpr uint8_t CMD_RASTER          = 0x67;  // g
static constexpr uint8_t CMD_PRINT           = 0x1A;

// Status header bytes
static constexpr uint8_t STATUS_HEADER_0     = 0x80;
static constexpr uint8_t STATUS_HEADER_1     = 0x20;
static constexpr uint8_t STATUS_HEADER_2     = 0x42;

// Status type values
static constexpr uint8_t STATUS_REPLY        = 0x00;
static constexpr uint8_t STATUS_COMPLETE     = 0x01;
static constexpr uint8_t STATUS_ERROR        = 0x02;
static constexpr uint8_t STATUS_NOTIFICATION = 0x05;
static constexpr uint8_t STATUS_PHASE_CHANGE = 0x06;

// Media type values
static constexpr uint8_t MEDIA_NONE          = 0x00;
static constexpr uint8_t MEDIA_CONTINUOUS    = 0x0A;
static constexpr uint8_t MEDIA_DIE_CUT       = 0x0B;

// Mode setting flags
static constexpr uint8_t MODE_AUTO_CUT       = 0x40;  // bit 6

// Media/quality valid flags
static constexpr uint8_t VALID_PI            = 0x80;  // bit 7
static constexpr uint8_t VALID_QUALITY       = 0x40;  // bit 6
static constexpr uint8_t VALID_LENGTH        = 0x08;  // bit 3
static constexpr uint8_t VALID_WIDTH         = 0x04;  // bit 2
static constexpr uint8_t VALID_MEDIA_TYPE    = 0x02;  // bit 1

// Maximum bytes_per_row across all Brother QL models
static constexpr uint8_t MAX_BYTES_PER_ROW   = 90;

// Buffer for building commands (reused per call).
// Must be large enough for max invalidate (400) + protocol commands.
static constexpr size_t CMD_BUF_SIZE = 512;
static uint8_t s_cmd_buf[CMD_BUF_SIZE];

// Phase 1: invalidate + init + status request (sent before reading status)
static size_t build_init_cmd(uint8_t *buf, size_t buf_size, const ql_model_t *model) {
    size_t pos = 0;

    // Invalidate: null bytes (200 for older models, 400 for QL-800+)
    memset(buf + pos, 0x00, model->num_invalidate);
    pos += model->num_invalidate;

    // Initialize: ESC @
    buf[pos++] = CMD_ESC;
    buf[pos++] = CMD_INIT;

    // Status request: ESC i S
    buf[pos++] = CMD_ESC;
    buf[pos++] = CMD_STATUS_INFO;
    buf[pos++] = CMD_STATUS_REQUEST;

    configASSERT(pos <= buf_size);
    return pos;
}

// Phase 2: mode/media/margins setup (sent after reading status, with detected media type)
static size_t build_print_setup(uint8_t *buf, size_t buf_size, const ql_model_t *model,
                                uint8_t media_type, uint8_t media_width_mm,
                                uint8_t media_length_mm) {
    size_t pos = 0;

    // Mode setting: ESC i M (models that support it)
    if (model->mode_setting) {
        buf[pos++] = CMD_ESC;
        buf[pos++] = CMD_STATUS_INFO;
        buf[pos++] = CMD_MODE_SETTING;
        buf[pos++] = model->cutting ? MODE_AUTO_CUT : 0x00;
    }

    // Expanded mode: ESC i K (models that support it)
    if (model->expanded_mode) {
        buf[pos++] = CMD_ESC;
        buf[pos++] = CMD_STATUS_INFO;
        buf[pos++] = CMD_EXPANDED_MODE;
        buf[pos++] = 0x00;  // no cut-every-N, no mirror
    }

    // Media/quality: ESC i z + 10 bytes
    buf[pos++] = CMD_ESC;
    buf[pos++] = CMD_STATUS_INFO;
    buf[pos++] = CMD_MEDIA_QUALITY;

    uint8_t valid_flags = VALID_PI | VALID_QUALITY | VALID_WIDTH | VALID_MEDIA_TYPE;
    if (media_type == MEDIA_DIE_CUT && media_length_mm > 0)
        valid_flags |= VALID_LENGTH;
    buf[pos++] = valid_flags;
    buf[pos++] = media_type;
    buf[pos++] = media_width_mm;
    buf[pos++] = media_length_mm;  // 0 for continuous
    // Raster lines LE32
    uint32_t lines = LABEL_PRINTABLE_H;
    buf[pos++] = lines & 0xFF;
    buf[pos++] = (lines >> 8) & 0xFF;
    buf[pos++] = (lines >> 16) & 0xFF;
    buf[pos++] = (lines >> 24) & 0xFF;
    buf[pos++] = 0x00;  // page
    buf[pos++] = 0x00;  // padding

    // Auto-cut: ESC i A (models with cutting capability)
    if (model->cutting) {
        buf[pos++] = CMD_ESC;
        buf[pos++] = CMD_STATUS_INFO;
        buf[pos++] = CMD_AUTO_CUT;
        buf[pos++] = 0x01;  // cut every 1 label
    }

    // Margins: ESC i d + 2 bytes
    buf[pos++] = CMD_ESC;
    buf[pos++] = CMD_STATUS_INFO;
    buf[pos++] = CMD_MARGINS;
    buf[pos++] = LABEL_FEED_MARGIN & 0xFF;
    buf[pos++] = (LABEL_FEED_MARGIN >> 8) & 0xFF;

    configASSERT(pos <= buf_size);
    return pos;
}

// Build one raster row command into buf. Returns length (3 + bytes_per_row).
static size_t build_raster_row(uint8_t *buf, const uint8_t *framebuffer, uint16_t y, uint8_t bytes_per_row) {
    uint8_t raster_row[MAX_BYTES_PER_ROW];
    memset(raster_row, 0, bytes_per_row);

    uint16_t raster_width_px = bytes_per_row * 8;
    uint16_t left_offset_px = raster_width_px - LABEL_PRINTABLE_W - LABEL_RIGHT_MARGIN;

    // Copy framebuffer pixels into raster row at correct offset
    for (uint16_t x = 0; x < LABEL_PRINTABLE_W; x++) {
        uint32_t fb_byte = y * LABEL_FB_STRIDE + x / 8;
        uint8_t fb_bit = 0x80 >> (x % 8);
        if (framebuffer[fb_byte] & fb_bit) {
            uint16_t raster_x = left_offset_px + x;
            raster_row[raster_x / 8] |= (0x80 >> (raster_x % 8));
        }
    }

    // Header
    buf[0] = CMD_RASTER;
    buf[1] = 0x00;
    buf[2] = bytes_per_row;

    // Flip horizontally + reverse bits (Brother QL hardware requirement)
    for (uint8_t i = 0; i < bytes_per_row; i++) {
        uint8_t b = raster_row[bytes_per_row - 1 - i];
        b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4);
        b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2);
        b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1);
        buf[3 + i] = b;
    }

    return 3 + bytes_per_row;
}

static void log_status_hex(const uint8_t *data, size_t len) {
    char hex[32 * 3 + 1];
    size_t n = len < 32 ? len : 32;
    for (size_t i = 0; i < n; i++)
        sprintf(hex + i * 3, "%02X ", data[i]);
    hex[n * 3] = '\0';
    ESP_LOGI(TAG, "Status hex: %s", hex);
}

static const char *media_type_str(uint8_t t) {
    switch (t) {
        case MEDIA_NONE:       return "no-media";
        case MEDIA_CONTINUOUS: return "continuous";
        case MEDIA_DIE_CUT:    return "die-cut";
        default: return "unknown";
    }
}

static void log_status_detail(const BrotherQLStatus &s) {
    const char *type_str = "unknown";
    switch (s.status_type) {
        case STATUS_REPLY:        type_str = "status-reply"; break;
        case STATUS_COMPLETE:     type_str = "print-complete"; break;
        case STATUS_ERROR:        type_str = "error"; break;
        case STATUS_NOTIFICATION: type_str = "notification"; break;
        case STATUS_PHASE_CHANGE: type_str = "phase-change"; break;
    }
    ESP_LOGI(TAG, "  type=%s(0x%02X) phase=0x%02X media=%s(0x%02X) width=%dmm length=%dmm",
             type_str, s.status_type, s.phase_type,
             media_type_str(s.media_type), s.media_type, s.media_width, s.media_length);
    ESP_LOGI(TAG, "  err1=0x%02X err2=0x%02X", s.error_info_1, s.error_info_2);
    if (s.error_info_1) {
        if (s.error_info_1 & 0x01) ESP_LOGE(TAG, "    -> No media");
        if (s.error_info_1 & 0x02) ESP_LOGE(TAG, "    -> End of media (continuous)");
        if (s.error_info_1 & 0x04) ESP_LOGE(TAG, "    -> Cutter jam");
        if (s.error_info_1 & 0x10) ESP_LOGE(TAG, "    -> Printer in use");
        if (s.error_info_1 & 0x20) ESP_LOGE(TAG, "    -> Printer turned off");
        if (s.error_info_1 & 0x40) ESP_LOGE(TAG, "    -> High-voltage adapter");
        if (s.error_info_1 & 0x80) ESP_LOGE(TAG, "    -> Fan motor error");
    }
    if (s.error_info_2) {
        if (s.error_info_2 & 0x01) ESP_LOGE(TAG, "    -> Replace media");
        if (s.error_info_2 & 0x02) ESP_LOGE(TAG, "    -> Expansion buffer full");
        if (s.error_info_2 & 0x04) ESP_LOGE(TAG, "    -> Communication error");
        if (s.error_info_2 & 0x08) ESP_LOGE(TAG, "    -> Communication buffer full");
        if (s.error_info_2 & 0x10) ESP_LOGE(TAG, "    -> Cover open");
        if (s.error_info_2 & 0x20) ESP_LOGE(TAG, "    -> Cancel key");
        if (s.error_info_2 & 0x40) ESP_LOGE(TAG, "    -> Media cannot be fed");
        if (s.error_info_2 & 0x80) ESP_LOGE(TAG, "    -> System error");
    }
}

// Build a short human-readable error string from status (fits 16-char LCD)
static void format_error_msg(const BrotherQLStatus &s, char *buf, size_t len) {
    if (s.error_info_1 & 0x01) { snprintf(buf, len, "NO MEDIA"); return; }
    if (s.error_info_1 & 0x02) { snprintf(buf, len, "END OF MEDIA"); return; }
    if (s.error_info_1 & 0x04) { snprintf(buf, len, "CUTTER JAM"); return; }
    if (s.error_info_2 & 0x10) { snprintf(buf, len, "COVER OPEN"); return; }
    if (s.error_info_2 & 0x40) { snprintf(buf, len, "MEDIA JAM"); return; }
    if (s.error_info_2 & 0x80) { snprintf(buf, len, "SYSTEM ERROR"); return; }
    if (s.error_info_2 & 0x04) { snprintf(buf, len, "COMM ERROR"); return; }
    if (s.error_info_2 & 0x01) { snprintf(buf, len, "REPLACE MEDIA"); return; }
    snprintf(buf, len, "ERR %02X/%02X", s.error_info_1, s.error_info_2);
}

BrotherQLStatus brother_ql_parse_status(const uint8_t *data, size_t len) {
    BrotherQLStatus status{};
    status.valid = false;

    if (len < 32)
        return status;
    if (data[0] != STATUS_HEADER_0 || data[1] != STATUS_HEADER_1 || data[2] != STATUS_HEADER_2)
        return status;

    status.error_info_1 = data[8];
    status.error_info_2 = data[9];
    status.media_width = data[10];
    status.media_type = data[11];
    status.media_length = data[17];
    status.status_type = data[18];
    status.phase_type = data[19];
    status.valid = true;
    return status;
}

static void set_error(char *error_msg, size_t error_msg_len, const char *msg) {
    if (error_msg && error_msg_len > 0)
        snprintf(error_msg, error_msg_len, "%s", msg);
}

bool brother_ql_print(PrinterState *printer, const ql_model_t *model, const uint8_t *framebuffer,
                      char *error_msg, size_t error_msg_len) {
    if (error_msg && error_msg_len > 0) error_msg[0] = '\0';

    ESP_LOGI(TAG, "Printing on %s (heap: %lu bytes)", model->name, (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "Model caps: invalidate=%d mode_setting=%d expanded=%d cutting=%d two_color=%d bpr=%d",
             model->num_invalidate, model->mode_setting, model->expanded_mode,
             model->cutting, model->two_color, model->bytes_per_row);

    uint8_t bytes_per_row = model->bytes_per_row;
    if (bytes_per_row > MAX_BYTES_PER_ROW) {
        ESP_LOGE(TAG, "bytes_per_row %d exceeds max %d", bytes_per_row, MAX_BYTES_PER_ROW);
        set_error(error_msg, error_msg_len, "BAD MODEL CFG");
        return false;
    }

    // Phase 1: Send invalidate + init + status request
    size_t init_len = build_init_cmd(s_cmd_buf, CMD_BUF_SIZE, model);
    ESP_LOGI(TAG, "Sending init (%zu bytes)...", init_len);
    esp_err_t err = printer_send(printer, s_cmd_buf, init_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Init send failed: %s", esp_err_to_name(err));
        set_error(error_msg, error_msg_len, "SEND FAILED");
        return false;
    }

    // Read status reply — tells us what media is actually loaded
    ESP_LOGI(TAG, "Reading printer status...");
    uint8_t status_buf[32];
    vTaskDelay(pdMS_TO_TICKS(200));
    int n = printer_read_status(printer, status_buf, sizeof(status_buf));

    // Determine media type: auto-detect from printer, fall back to configured default
    uint8_t media_type = LABEL_MEDIA_TYPE;
    uint8_t media_width = LABEL_WIDTH_MM;
    uint8_t media_length = LABEL_HEIGHT_MM;

    if (n > 0) {
        log_status_hex(status_buf, n);
        auto init_status = brother_ql_parse_status(status_buf, n);
        if (init_status.valid) {
            ESP_LOGI(TAG, "Printer status:");
            log_status_detail(init_status);

            if (init_status.status_type == STATUS_ERROR) {
                ESP_LOGE(TAG, "Printer error before print — aborting");
                format_error_msg(init_status, error_msg ? error_msg : (char*)"", error_msg_len);
                return false;
            }

            // Auto-detect media from printer status
            if (init_status.media_type != MEDIA_NONE) {
                media_type = init_status.media_type;
                ESP_LOGI(TAG, "Detected media: %s", media_type_str(media_type));
            }
            if (init_status.media_width > 0) {
                media_width = init_status.media_width;
                ESP_LOGI(TAG, "Detected width: %dmm", media_width);
            }

            // For continuous media, length = 0 in the command (raster line count determines length)
            if (media_type == MEDIA_CONTINUOUS) {
                media_length = 0;
                ESP_LOGI(TAG, "Continuous media: using length=0, %d raster lines (~%dmm)",
                         LABEL_PRINTABLE_H, LABEL_HEIGHT_MM);
            }
        } else {
            ESP_LOGW(TAG, "Could not parse status (%d bytes)", n);
        }
    } else {
        ESP_LOGW(TAG, "No status reply (n=%d), using defaults", n);
    }

    // Phase 2: Send print setup with detected media parameters
    ESP_LOGI(TAG, "Sending setup: media=%s(0x%02X) width=%d length=%d",
             media_type_str(media_type), media_type, media_width, media_length);
    size_t setup_len = build_print_setup(s_cmd_buf, CMD_BUF_SIZE, model, media_type, media_width, media_length);
    ESP_LOGI(TAG, "Sending print setup (%zu bytes)...", setup_len);
    err = printer_send(printer, s_cmd_buf, setup_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Setup send failed: %s", esp_err_to_name(err));
        set_error(error_msg, error_msg_len, "SEND FAILED");
        return false;
    }

    // Send raster data row by row
    uint8_t row_buf[3 + MAX_BYTES_PER_ROW];
    ESP_LOGI(TAG, "Sending %d raster rows (%d bytes/row)...", LABEL_PRINTABLE_H, bytes_per_row);
    for (uint16_t y = 0; y < LABEL_PRINTABLE_H; y++) {
        size_t row_len = build_raster_row(row_buf, framebuffer, y, bytes_per_row);
        err = printer_send(printer, row_buf, row_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Raster row %d send failed: %s", y, esp_err_to_name(err));
            set_error(error_msg, error_msg_len, "SEND FAILED");
            return false;
        }
        if (y % 200 == 0) {
            ESP_LOGI(TAG, "  row %d/%d", y, LABEL_PRINTABLE_H);
        }
    }

    // Send print command
    uint8_t print_cmd = CMD_PRINT;
    ESP_LOGI(TAG, "Sending print command 0x%02X", print_cmd);
    err = printer_send(printer, &print_cmd, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Print command send failed: %s", esp_err_to_name(err));
        set_error(error_msg, error_msg_len, "SEND FAILED");
        return false;
    }

    ESP_LOGI(TAG, "All data sent, reading completion status...");

    for (int attempts = 0; attempts < 10; attempts++) {
        n = printer_read_status(printer, status_buf, sizeof(status_buf));
        if (n < 0) {
            ESP_LOGW(TAG, "Status read failed (attempt %d)", attempts);
            if (attempts >= 2) {
                ESP_LOGE(TAG, "Status read failed 3 times, reporting failure");
                set_error(error_msg, error_msg_len, "NO RESPONSE");
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        log_status_hex(status_buf, n);
        auto status = brother_ql_parse_status(status_buf, n);
        if (!status.valid) {
            ESP_LOGW(TAG, "Invalid status response (%d bytes)", n);
            continue;
        }

        log_status_detail(status);

        if (status.status_type == STATUS_COMPLETE) {
            ESP_LOGI(TAG, "Print completed successfully");
            return true;
        }

        if (status.status_type == STATUS_ERROR) {
            ESP_LOGE(TAG, "Printer error after print:");
            log_status_detail(status);
            format_error_msg(status, error_msg ? error_msg : (char*)"", error_msg_len);
            return false;
        }

        // Phase change or other status — keep reading
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGW(TAG, "No completion status after 10 reads");
    set_error(error_msg, error_msg_len, "NO RESPONSE");
    return false;
}
