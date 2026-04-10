// Should not be needed, but it's required to pass CI clang-tidy checks
#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)
#include "usb_printer.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

#include <cinttypes>
#include <cstring>

namespace esphome::usb_printer {

// --- LabelBuffer ---

void LabelBuffer::setup() {
  uint32_t buffer_size = this->get_buffer_stride() * this->height_;
  this->init_internal_(buffer_size);
  if (this->buffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate label framebuffer (%u bytes)", buffer_size);
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "Label framebuffer allocated: %ux%u (%u bytes)", this->width_, this->height_, buffer_size);
}

void LabelBuffer::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x < 0 || x >= this->width_ || y < 0 || y >= this->height_)
    return;

  uint32_t stride = this->get_buffer_stride();
  uint32_t byte_idx = y * stride + x / 8;
  uint8_t bit_mask = 0x80 >> (x % 8);

  if (color.is_on()) {
    this->buffer_[byte_idx] |= bit_mask;
  } else {
    this->buffer_[byte_idx] &= ~bit_mask;
  }
}

// --- BrotherQLPrinter ---

void BrotherQLPrinter::setup() {
  USBClient::setup();

  if (this->label_buffer_ == nullptr) {
    ESP_LOGE(TAG, "No label buffer configured");
    this->mark_failed();
    return;
  }
}

void BrotherQLPrinter::dump_config() {
  USBClient::dump_config();
  ESP_LOGCONFIG(TAG,
                "Brother QL Printer\n"
                "  Label: %umm x %umm\n"
                "  Printable: %u x %u px\n"
                "  Bytes/row: %u\n"
                "  Compression: %s\n"
                "  Cutting: %s",
                this->width_mm_, this->height_mm_,
                this->printable_w_, this->printable_h_,
                this->bytes_per_row_,
                YESNO(this->compression_), YESNO(this->cutting_));
}

void BrotherQLPrinter::loop() {
  bool had_work = this->process_usb_events_();

  // Handle send completion — kick next chunk if needed
  if (this->print_state_ == PRINT_SENDING && !this->send_in_progress_.load()) {
    had_work = true;
    if (this->send_offset_ < this->cmd_buffer_.size()) {
      this->send_next_chunk_();
    } else {
      ESP_LOGI(TAG, "All data sent (%zu bytes), reading status...", this->cmd_buffer_.size());
      this->print_state_ = PRINT_WAITING_STATUS;
      this->read_status_();
    }
  }

  if (this->print_state_ == PRINT_COMPLETE) {
    had_work = true;
    ESP_LOGI(TAG, "Print job completed successfully");
    this->print_state_ = PRINT_IDLE;
    this->cmd_buffer_.clear();
    this->cmd_buffer_.shrink_to_fit();
  }

  if (this->print_state_ == PRINT_ERROR) {
    had_work = true;
    ESP_LOGE(TAG, "Print job failed");
    this->print_state_ = PRINT_IDLE;
    this->cmd_buffer_.clear();
    this->cmd_buffer_.shrink_to_fit();
  }

  if (!had_work) {
    this->disable_loop();
  }
}

// --- USB Connection ---

void BrotherQLPrinter::on_connected() {
  const usb_config_desc_t *config_desc;
  auto err = usb_host_get_active_config_descriptor(this->device_handle_, &config_desc);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "get_active_config_descriptor failed: %s", esp_err_to_name(err));
    this->disconnect();
    return;
  }

  // Walk descriptors to find printer interface with bulk endpoints
  int conf_offset = 0;
  bool found = false;

  for (uint8_t intf_idx = 0; intf_idx < config_desc->bNumInterfaces; intf_idx++) {
    const auto *intf_desc = usb_parse_interface_descriptor(config_desc, intf_idx, 0, &conf_offset);
    if (!intf_desc)
      break;

    ESP_LOGD(TAG, "Interface %d: class=0x%02X subclass=0x%02X protocol=0x%02X endpoints=%d",
             intf_desc->bInterfaceNumber, intf_desc->bInterfaceClass,
             intf_desc->bInterfaceSubClass, intf_desc->bInterfaceProtocol,
             intf_desc->bNumEndpoints);

    // USB Printer Class = 0x07, or accept vendor-specific (0xFF)
    if (intf_desc->bInterfaceClass != 0x07 && intf_desc->bInterfaceClass != 0xFF) {
      continue;
    }

    this->bulk_in_ep_ = nullptr;
    this->bulk_out_ep_ = nullptr;

    for (uint8_t ep_idx = 0; ep_idx < intf_desc->bNumEndpoints; ep_idx++) {
      int ep_offset = conf_offset;
      const auto *ep = usb_parse_endpoint_descriptor_by_index(
          intf_desc, ep_idx, config_desc->wTotalLength, &ep_offset);
      if (!ep)
        break;

      if (ep->bmAttributes != USB_BM_ATTRIBUTES_XFER_BULK)
        continue;

      if (ep->bEndpointAddress & usb_host::USB_DIR_IN) {
        this->bulk_in_ep_ = ep;
        ESP_LOGD(TAG, "Found BULK IN endpoint: 0x%02X (MPS=%d)", ep->bEndpointAddress, ep->wMaxPacketSize);
      } else {
        this->bulk_out_ep_ = ep;
        ESP_LOGD(TAG, "Found BULK OUT endpoint: 0x%02X (MPS=%d)", ep->bEndpointAddress, ep->wMaxPacketSize);
      }
    }

    if (this->bulk_out_ep_ != nullptr) {
      this->interface_number_ = intf_desc->bInterfaceNumber;
      found = true;
      break;
    }
  }

  if (!found || this->bulk_out_ep_ == nullptr) {
    ESP_LOGE(TAG, "No suitable printer interface found");
    this->disconnect();
    return;
  }

  err = usb_host_interface_claim(this->handle_, this->device_handle_, this->interface_number_, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "usb_host_interface_claim failed: %s", esp_err_to_name(err));
    this->disconnect();
    return;
  }

  this->connected_ = true;
  ESP_LOGI(TAG, "Brother QL printer connected (interface %d)", this->interface_number_);
}

void BrotherQLPrinter::on_disconnected() {
  if (this->bulk_out_ep_ != nullptr) {
    usb_host_endpoint_halt(this->device_handle_, this->bulk_out_ep_->bEndpointAddress);
    usb_host_endpoint_flush(this->device_handle_, this->bulk_out_ep_->bEndpointAddress);
  }
  if (this->bulk_in_ep_ != nullptr) {
    usb_host_endpoint_halt(this->device_handle_, this->bulk_in_ep_->bEndpointAddress);
    usb_host_endpoint_flush(this->device_handle_, this->bulk_in_ep_->bEndpointAddress);
  }
  if (this->interface_number_ != 0xFF) {
    usb_host_interface_release(this->handle_, this->device_handle_, this->interface_number_);
    this->interface_number_ = 0xFF;
  }
  this->bulk_in_ep_ = nullptr;
  this->bulk_out_ep_ = nullptr;
  this->connected_ = false;
  this->print_state_ = PRINT_IDLE;
  this->send_in_progress_.store(false);
  ESP_LOGW(TAG, "Brother QL printer disconnected");
  USBClient::on_disconnected();
}

// --- USB Data Transfer ---

void BrotherQLPrinter::start_send_() {
  this->send_offset_ = 0;
  this->print_state_ = PRINT_SENDING;
  this->send_next_chunk_();
}

void BrotherQLPrinter::send_next_chunk_() {
  if (this->send_offset_ >= this->cmd_buffer_.size()) {
    return;
  }

  size_t remaining = this->cmd_buffer_.size() - this->send_offset_;
  size_t chunk_size = std::min(remaining, static_cast<size_t>(64));

  bool expected = false;
  if (!this->send_in_progress_.compare_exchange_strong(expected, true)) {
    return;
  }

  size_t offset = this->send_offset_;
  auto callback = [this, chunk_size, offset](const usb_host::TransferStatus &status) {
    if (!status.success) {
      ESP_LOGE(TAG, "OUT transfer failed at offset %zu: status=%d", offset, status.error_code);
      this->print_state_ = PRINT_ERROR;
      this->send_in_progress_.store(false);
      this->enable_loop_soon_any_context();
#if defined(USE_SOCKET_SELECT_SUPPORT) && defined(USE_WAKE_LOOP_THREADSAFE)
      App.wake_loop_threadsafe();
#endif
      return;
    }

    this->send_offset_ += chunk_size;
    this->send_in_progress_.store(false);

    this->enable_loop_soon_any_context();
#if defined(USE_SOCKET_SELECT_SUPPORT) && defined(USE_WAKE_LOOP_THREADSAFE)
    App.wake_loop_threadsafe();
#endif
  };

  if (!this->transfer_out(this->bulk_out_ep_->bEndpointAddress, callback,
                          this->cmd_buffer_.data() + this->send_offset_,
                          static_cast<uint16_t>(chunk_size))) {
    ESP_LOGE(TAG, "transfer_out submission failed at offset %zu", this->send_offset_);
    this->send_in_progress_.store(false);
    this->print_state_ = PRINT_ERROR;
  }
}

void BrotherQLPrinter::read_status_() {
  if (this->bulk_in_ep_ == nullptr) {
    ESP_LOGW(TAG, "No BULK IN endpoint, skipping status read");
    this->print_state_ = PRINT_COMPLETE;
    return;
  }

  auto callback = [this](const usb_host::TransferStatus &status) {
    if (!status.success) {
      ESP_LOGW(TAG, "Status read failed: %d", status.error_code);
      this->print_state_ = PRINT_COMPLETE;
    } else {
      auto ql_status = this->parse_status_(status.data, status.data_len);
      if (ql_status.valid) {
        if (ql_status.status_type == 0x02) {
          ESP_LOGE(TAG, "Printer error: err1=0x%02X err2=0x%02X", ql_status.error_info_1, ql_status.error_info_2);
          if (ql_status.error_info_1 & 0x01) ESP_LOGE(TAG, "  No media");
          if (ql_status.error_info_1 & 0x04) ESP_LOGE(TAG, "  Cutter jam");
          if (ql_status.error_info_1 & 0x10) ESP_LOGE(TAG, "  End of media");
          if (ql_status.error_info_2 & 0x01) ESP_LOGE(TAG, "  Cover open");
          if (ql_status.error_info_2 & 0x02) ESP_LOGE(TAG, "  Overheating");
          this->print_state_ = PRINT_ERROR;
        } else if (ql_status.status_type == 0x01) {
          ESP_LOGI(TAG, "Printing completed successfully");
          this->print_state_ = PRINT_COMPLETE;
        } else {
          ESP_LOGD(TAG, "Status reply: type=0x%02X phase=0x%02X", ql_status.status_type, ql_status.phase_type);
          // Keep reading status until we get completion or error
          this->read_status_();
          return;
        }
      } else {
        ESP_LOGW(TAG, "Invalid status response (%zu bytes)", status.data_len);
        this->print_state_ = PRINT_COMPLETE;
      }
    }
    this->enable_loop_soon_any_context();
#if defined(USE_SOCKET_SELECT_SUPPORT) && defined(USE_WAKE_LOOP_THREADSAFE)
    App.wake_loop_threadsafe();
#endif
  };

  this->transfer_in(this->bulk_in_ep_->bEndpointAddress, callback, 32);
}

BrotherQLStatus BrotherQLPrinter::parse_status_(const uint8_t *data, size_t len) {
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

}  // namespace esphome::usb_printer

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
