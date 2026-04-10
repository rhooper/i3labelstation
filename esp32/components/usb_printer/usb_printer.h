#pragma once

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/components/usb_host/usb_host.h"
#include "esphome/components/display/display.h"
#include "esphome/components/display/display_buffer.h"
#include <atomic>
#include <vector>

namespace esphome::usb_printer {

static const char *const TAG = "usb_printer";

// Print job state machine
enum PrintState : uint8_t {
  PRINT_IDLE = 0,
  PRINT_RENDERING,
  PRINT_SENDING,
  PRINT_WAITING_STATUS,
  PRINT_COMPLETE,
  PRINT_ERROR,
};

// Brother QL status response (32 bytes)
struct BrotherQLStatus {
  uint8_t error_info_1;
  uint8_t error_info_2;
  uint8_t media_width;
  uint8_t media_type;
  uint8_t media_length;
  uint8_t status_type;  // 0x00=reply, 0x01=complete, 0x02=error, 0x05=notification, 0x06=phase change
  uint8_t phase_type;
  bool valid;
};

// Separate display buffer for label rendering (avoids diamond inheritance with Component)
class LabelBuffer : public display::DisplayBuffer {
 public:
  LabelBuffer(uint16_t width, uint16_t height) : width_(width), height_(height) {}

  void setup() override;

  // No-op update — we render on demand, not on a timer
  void update() override {}
  float get_setup_priority() const override { return setup_priority::PROCESSOR; }
  display::DisplayType get_display_type() override { return display::DisplayType::DISPLAY_TYPE_BINARY; }

  uint8_t *get_buffer() { return this->buffer_; }
  uint32_t get_buffer_stride() const { return (this->width_ + 7) / 8; }

 protected:
  int get_width_internal() override { return this->width_; }
  int get_height_internal() override { return this->height_; }
  void draw_absolute_pixel_internal(int x, int y, Color color) override;

  uint16_t width_;
  uint16_t height_;
};

class BrotherQLPrinter : public usb_host::USBClient {
 public:
  BrotherQLPrinter(uint16_t vid, uint16_t pid) : usb_host::USBClient(vid, pid) {}

  void setup() override;
  void loop() override;
  void dump_config() override;

  // Configuration setters (called from codegen)
  void set_model_info(uint8_t bytes_per_row, bool compression, bool cutting) {
    this->bytes_per_row_ = bytes_per_row;
    this->compression_ = compression;
    this->cutting_ = cutting;
  }

  void set_label_info(uint16_t printable_w, uint16_t printable_h,
                      uint8_t width_mm, uint8_t height_mm,
                      uint8_t media_type, uint16_t feed_margin) {
    this->printable_w_ = printable_w;
    this->printable_h_ = printable_h;
    this->width_mm_ = width_mm;
    this->height_mm_ = height_mm;
    this->media_type_ = media_type;
    this->feed_margin_ = feed_margin;
  }

  void set_writer(display::display_writer_t writer) { this->writer_ = std::move(writer); }
  void set_label_buffer(LabelBuffer *buf) { this->label_buffer_ = buf; }

  // Trigger a label print
  void print_label();

 protected:
  // USB connection
  void on_connected() override;
  void on_disconnected() override;

  // Protocol encoding
  void build_command_buffer_();
  void encode_invalidate_();
  void encode_initialize_();
  void encode_status_request_();
  void encode_media_quality_(uint32_t raster_lines);
  void encode_margins_(uint16_t dots);
  void encode_raster_data_();
  void encode_print_();

  // USB data sending
  void start_send_();
  void send_next_chunk_();
  void read_status_();
  BrotherQLStatus parse_status_(const uint8_t *data, size_t len);

  // Endpoint descriptors
  const usb_ep_desc_t *bulk_in_ep_{nullptr};
  const usb_ep_desc_t *bulk_out_ep_{nullptr};
  uint8_t interface_number_{0xFF};

  // Model configuration
  uint8_t bytes_per_row_{90};
  bool compression_{false};
  bool cutting_{false};

  // Label configuration
  uint16_t printable_w_{306};
  uint16_t printable_h_{991};
  uint8_t width_mm_{29};
  uint8_t height_mm_{90};
  uint8_t media_type_{0x0B};
  uint16_t feed_margin_{0};

  // Display writer lambda and label buffer
  display::display_writer_t writer_{};
  LabelBuffer *label_buffer_{nullptr};

  // Command buffer (built before sending)
  std::vector<uint8_t> cmd_buffer_;
  size_t send_offset_{0};

  // Print state
  std::atomic<PrintState> print_state_{PRINT_IDLE};
  std::atomic<bool> send_in_progress_{false};
  bool connected_{false};
};

// Automation action for usb_printer.print
template<typename... Ts> class PrintAction : public Action<Ts...> {
 public:
  explicit PrintAction(BrotherQLPrinter *printer) : printer_(printer) {}

  void play(const Ts &...x) override { this->printer_->print_label(); }

 protected:
  BrotherQLPrinter *printer_;
};

}  // namespace esphome::usb_printer

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
