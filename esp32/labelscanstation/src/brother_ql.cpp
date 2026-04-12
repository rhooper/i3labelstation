#include "brother_ql.h"
#include "usb_printer.h"
#include "ql_models.h"
#include "app_config.h"

#include <cstring>
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "brother_ql";

// Buffer for building header commands (reused per call).
// Must be large enough for max invalidate (400) + protocol commands.
static uint8_t s_cmd_buf[512];

static size_t build_header(uint8_t *buf, const ql_model_t *model) {
    size_t pos = 0;

    // Invalidate: null bytes (200 for older models, 400 for QL-800+)
    memset(buf + pos, 0x00, model->num_invalidate);
    pos += model->num_invalidate;

    // Initialize: ESC @
    buf[pos++] = 0x1B;
    buf[pos++] = 0x40;

    // Status request: ESC i S
    buf[pos++] = 0x1B;
    buf[pos++] = 0x69;
    buf[pos++] = 0x53;

    // Mode setting: ESC i M (models that support it)
    if (model->mode_setting) {
        buf[pos++] = 0x1B;
        buf[pos++] = 0x69;
        buf[pos++] = 0x4D;
        buf[pos++] = 0x00;  // no auto-cut
    }

    // Expanded mode: ESC i K (models that support it)
    if (model->expanded_mode) {
        buf[pos++] = 0x1B;
        buf[pos++] = 0x69;
        buf[pos++] = 0x4B;
        buf[pos++] = 0x00;  // no cut-every-N, no mirror
    }

    // Media/quality: ESC i z + 10 bytes
    buf[pos++] = 0x1B;
    buf[pos++] = 0x69;
    buf[pos++] = 0x7A;

    uint8_t valid_flags = 0x80 | (1 << 6) | (1 << 1) | (1 << 2);
    if (LABEL_HEIGHT_MM > 0)
        valid_flags |= (1 << 3);
    buf[pos++] = valid_flags;
    buf[pos++] = LABEL_MEDIA_TYPE;
    buf[pos++] = LABEL_WIDTH_MM;
    buf[pos++] = LABEL_HEIGHT_MM;
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
        buf[pos++] = 0x1B;
        buf[pos++] = 0x69;
        buf[pos++] = 0x41;
        buf[pos++] = 0x01;  // cut every 1 label
    }

    // Margins: ESC i d + 2 bytes
    buf[pos++] = 0x1B;
    buf[pos++] = 0x69;
    buf[pos++] = 0x64;
    buf[pos++] = LABEL_FEED_MARGIN & 0xFF;
    buf[pos++] = (LABEL_FEED_MARGIN >> 8) & 0xFF;

    return pos;
}

// Build one raster row command into buf. Returns length (3 + bytes_per_row).
static size_t build_raster_row(uint8_t *buf, const uint8_t *framebuffer, uint16_t y, uint8_t bytes_per_row) {
    uint8_t raster_row[bytes_per_row];
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
    buf[0] = 0x67;
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

BrotherQLStatus brother_ql_parse_status(const uint8_t *data, size_t len) {
    BrotherQLStatus status{};
    status.valid = false;

    if (len < 32)
        return status;
    if (data[0] != 0x80 || data[1] != 0x20 || data[2] != 0x42)
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

bool brother_ql_print(PrinterState *printer, const ql_model_t *model, const uint8_t *framebuffer) {
    ESP_LOGI(TAG, "Printing on %s (heap: %lu bytes)", model->name, (unsigned long)esp_get_free_heap_size());

    // Send header (invalidate + init + status + media + margins)
    size_t hdr_len = build_header(s_cmd_buf, model);
    ESP_LOGI(TAG, "Sending header (%zu bytes)...", hdr_len);
    esp_err_t err = printer_send(printer, s_cmd_buf, hdr_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Header send failed: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t bytes_per_row = model->bytes_per_row;

    // Send raster data row by row
    uint8_t row_buf[3 + bytes_per_row];
    ESP_LOGI(TAG, "Sending %d raster rows...", LABEL_PRINTABLE_H);
    for (uint16_t y = 0; y < LABEL_PRINTABLE_H; y++) {
        size_t row_len = build_raster_row(row_buf, framebuffer, y, bytes_per_row);
        err = printer_send(printer, row_buf, row_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Raster row %d send failed: %s", y, esp_err_to_name(err));
            return false;
        }
        if (y % 200 == 0) {
            ESP_LOGI(TAG, "  row %d/%d", y, LABEL_PRINTABLE_H);
        }
    }

    // Send print command (0x1A = print without cut)
    uint8_t print_cmd = 0x1A;
    err = printer_send(printer, &print_cmd, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Print command send failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "All data sent, reading status...");
    uint8_t status_buf[32];

    for (int attempts = 0; attempts < 10; attempts++) {
        int n = printer_read_status(printer, status_buf, sizeof(status_buf));
        if (n < 0) {
            ESP_LOGW(TAG, "Status read failed");
            return true;  // data was sent, assume success
        }

        auto status = brother_ql_parse_status(status_buf, n);
        if (!status.valid) {
            ESP_LOGW(TAG, "Invalid status response (%d bytes)", n);
            continue;
        }

        if (status.status_type == 0x01) {
            ESP_LOGI(TAG, "Print completed successfully");
            return true;
        }

        if (status.status_type == 0x02) {
            ESP_LOGE(TAG, "Printer error: err1=0x%02X err2=0x%02X",
                     status.error_info_1, status.error_info_2);
            if (status.error_info_1 & 0x01) ESP_LOGE(TAG, "  No media");
            if (status.error_info_1 & 0x04) ESP_LOGE(TAG, "  Cutter jam");
            if (status.error_info_1 & 0x10) ESP_LOGE(TAG, "  End of media");
            if (status.error_info_2 & 0x01) ESP_LOGE(TAG, "  Cover open");
            if (status.error_info_2 & 0x02) ESP_LOGE(TAG, "  Overheating");
            return false;
        }

        ESP_LOGD(TAG, "Status: type=0x%02X phase=0x%02X", status.status_type, status.phase_type);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGW(TAG, "No completion status after 10 reads, assuming success");
    return true;
}
