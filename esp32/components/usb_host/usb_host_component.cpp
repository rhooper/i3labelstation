// Override of ESPHome's usb_host_component.cpp
// Adds explicit USB PHY initialization for OTG Host mode on ESP32-S3
// when USB-Serial-JTAG is disabled.
#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)
#include "esphome/components/usb_host/usb_host.h"
#include <cinttypes>
#include "esphome/core/log.h"
#include "esp_private/usb_phy.h"

namespace esphome::usb_host {

static usb_phy_handle_t phy_handle_ = nullptr;

void USBHost::setup() {
  // Initialize USB PHY for OTG host mode
  usb_phy_config_t phy_config = {};
  phy_config.controller = USB_PHY_CTRL_OTG;
  phy_config.target = USB_PHY_TARGET_INT;
  phy_config.otg_mode = USB_OTG_MODE_HOST;
  phy_config.otg_speed = USB_PHY_SPEED_UNDEFINED;
  phy_config.otg_io_conf = nullptr;
  auto phy_err = usb_new_phy(&phy_config, &phy_handle_);
  if (phy_err != ESP_OK) {
    ESP_LOGE(TAG, "USB PHY init failed: %s", esp_err_to_name(phy_err));
  } else {
    ESP_LOGI(TAG, "USB PHY initialized for OTG Host mode");
  }

  // Install USB host library, skip PHY setup since we did it above
  usb_host_config_t config{};
  config.skip_phy_setup = (phy_handle_ != nullptr);

  if (usb_host_install(&config) != ESP_OK) {
    this->status_set_error(LOG_STR("usb_host_install failed"));
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "USB Host library installed");
}

void USBHost::loop() {
  int err;
  uint32_t event_flags;
  err = usb_host_lib_handle_events(0, &event_flags);
  if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
    ESP_LOGD(TAG, "lib_handle_events failed: %s", esp_err_to_name(err));
  }
  if (event_flags != 0) {
    ESP_LOGD(TAG, "Event flags %" PRIu32 "X", event_flags);
  }
}

}  // namespace esphome::usb_host

#endif
