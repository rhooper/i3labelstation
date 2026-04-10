// Brother QL raster protocol encoding
// Reference: brother_ql Python package (raster.py, conversion.py)

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)
#include "usb_printer.h"
#include "esphome/core/log.h"

namespace esphome::usb_printer {

void BrotherQLPrinter::print_label() {
  if (!this->connected_) {
    ESP_LOGW(TAG, "Printer not connected");
    return;
  }
  if (this->print_state_ != PRINT_IDLE) {
    ESP_LOGW(TAG, "Print already in progress");
    return;
  }
  if (this->label_buffer_ == nullptr) {
    ESP_LOGE(TAG, "No label buffer");
    return;
  }

  ESP_LOGI(TAG, "Starting print job...");
  this->print_state_ = PRINT_RENDERING;

  // Step 1: Clear framebuffer and render via display lambda
  this->label_buffer_->clear();
  if (this->writer_.has_value()) {
    this->writer_(*this->label_buffer_);
  }

  // Step 2: Build the command buffer
  this->build_command_buffer_();

  ESP_LOGI(TAG, "Command buffer built: %zu bytes (%u raster rows)", this->cmd_buffer_.size(), this->printable_h_);

  // Step 3: Start sending over USB
  this->start_send_();
  this->enable_loop_soon_any_context();
}

void BrotherQLPrinter::build_command_buffer_() {
  size_t estimated = 220 + (3 + this->bytes_per_row_) * this->printable_h_ + 1;
  this->cmd_buffer_.clear();
  this->cmd_buffer_.reserve(estimated);

  this->encode_invalidate_();
  this->encode_initialize_();
  this->encode_status_request_();
  this->encode_media_quality_(this->printable_h_);
  this->encode_margins_(this->feed_margin_);
  this->encode_raster_data_();
  this->encode_print_();
}

// 200 null bytes to clear the command buffer
void BrotherQLPrinter::encode_invalidate_() {
  this->cmd_buffer_.insert(this->cmd_buffer_.end(), 200, 0x00);
}

// ESC @ — initialize printer
void BrotherQLPrinter::encode_initialize_() {
  this->cmd_buffer_.push_back(0x1B);
  this->cmd_buffer_.push_back(0x40);
}

// ESC i S — request status information
void BrotherQLPrinter::encode_status_request_() {
  this->cmd_buffer_.push_back(0x1B);
  this->cmd_buffer_.push_back(0x69);
  this->cmd_buffer_.push_back(0x53);
}

// ESC i z — media type and quality
void BrotherQLPrinter::encode_media_quality_(uint32_t raster_lines) {
  this->cmd_buffer_.push_back(0x1B);
  this->cmd_buffer_.push_back(0x69);
  this->cmd_buffer_.push_back(0x7A);

  uint8_t valid_flags = 0x80;
  valid_flags |= 1 << 6;  // HQ printing
  valid_flags |= 1 << 1;  // media type valid
  valid_flags |= 1 << 2;  // media width valid
  if (this->height_mm_ > 0) {
    valid_flags |= 1 << 3;  // media length valid (die-cut)
  }
  this->cmd_buffer_.push_back(valid_flags);

  this->cmd_buffer_.push_back(this->media_type_);
  this->cmd_buffer_.push_back(this->width_mm_);
  this->cmd_buffer_.push_back(this->height_mm_);

  // Raster lines (little-endian uint32)
  this->cmd_buffer_.push_back(raster_lines & 0xFF);
  this->cmd_buffer_.push_back((raster_lines >> 8) & 0xFF);
  this->cmd_buffer_.push_back((raster_lines >> 16) & 0xFF);
  this->cmd_buffer_.push_back((raster_lines >> 24) & 0xFF);

  this->cmd_buffer_.push_back(0x00);  // page number
  this->cmd_buffer_.push_back(0x00);  // padding
}

// ESC i d — set margins
void BrotherQLPrinter::encode_margins_(uint16_t dots) {
  this->cmd_buffer_.push_back(0x1B);
  this->cmd_buffer_.push_back(0x69);
  this->cmd_buffer_.push_back(0x64);
  this->cmd_buffer_.push_back(dots & 0xFF);
  this->cmd_buffer_.push_back((dots >> 8) & 0xFF);
}

// Convert framebuffer to Brother QL raster data
void BrotherQLPrinter::encode_raster_data_() {
  uint8_t *fb = this->label_buffer_->get_buffer();
  uint32_t fb_stride = this->label_buffer_->get_buffer_stride();

  uint8_t raster_row[162];  // max bytes_per_row (QL-1050 = 162, QL-500 = 90)

  // Right margin offset (standard for most QL labels)
  uint16_t right_margin = 6;
  uint16_t raster_width_px = this->bytes_per_row_ * 8;
  uint16_t left_offset_px = raster_width_px - this->printable_w_ - right_margin;

  for (uint16_t y = 0; y < this->printable_h_; y++) {
    memset(raster_row, 0, this->bytes_per_row_);

    // Copy framebuffer pixels into the raster row at the correct offset
    for (uint16_t x = 0; x < this->printable_w_; x++) {
      uint32_t fb_byte = y * fb_stride + x / 8;
      uint8_t fb_bit = 0x80 >> (x % 8);
      bool pixel_on = (fb[fb_byte] & fb_bit) != 0;

      if (pixel_on) {
        uint16_t raster_x = left_offset_px + x;
        uint32_t r_byte = raster_x / 8;
        uint8_t r_bit = 0x80 >> (raster_x % 8);
        raster_row[r_byte] |= r_bit;
      }
    }

    // Flip the entire raster row horizontally (Brother QL hardware requirement)
    uint8_t flipped[162];
    for (uint8_t i = 0; i < this->bytes_per_row_; i++) {
      uint8_t b = raster_row[this->bytes_per_row_ - 1 - i];
      // Reverse bits within byte
      b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4);
      b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2);
      b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1);
      flipped[i] = b;
    }

    // Emit raster command: 0x67 0x00 <length> <data>
    this->cmd_buffer_.push_back(0x67);
    this->cmd_buffer_.push_back(0x00);
    this->cmd_buffer_.push_back(this->bytes_per_row_);
    this->cmd_buffer_.insert(this->cmd_buffer_.end(), flipped, flipped + this->bytes_per_row_);
  }
}

// Print command — end of page
void BrotherQLPrinter::encode_print_() {
  this->cmd_buffer_.push_back(0x1A);
}

}  // namespace esphome::usb_printer

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
