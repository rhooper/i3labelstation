#pragma once

#include <cstdint>
#include <cstddef>
#include "usb/usb_host.h"

// Printer connection state
struct PrinterState {
    usb_device_handle_t dev_handle;
    usb_host_client_handle_t client_handle;
    uint8_t interface_number;
    uint8_t bulk_out_addr;
    uint8_t bulk_in_addr;
    uint16_t bulk_out_mps;
    uint16_t bulk_in_mps;
    bool connected;
};

// Initialize printer state
void printer_init(PrinterState *state);

// Called when USB device connects — walks descriptors, claims interface
bool printer_on_connected(PrinterState *state, usb_device_handle_t dev_handle,
                          usb_host_client_handle_t client_handle);

// Called when USB device disconnects — releases interface
void printer_on_disconnected(PrinterState *state);

// Send data to printer (synchronous, blocking). Returns ESP_OK on success.
esp_err_t printer_send(PrinterState *state, const uint8_t *data, size_t len);

// Read status from printer (synchronous, blocking). Returns bytes read, or -1 on error.
int printer_read_status(PrinterState *state, uint8_t *buf, size_t buf_len);
