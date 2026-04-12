#pragma once

#include "usb/usb_host.h"
#include "ql_models.h"

// Initialize USB PHY + host library, start host and client tasks.
// Calls on_printer_connected/on_printer_disconnected when Brother QL is found.
void usb_host_init();

// Called from client event callback when printer is connected/disconnected.
// model is non-null on connect, nullptr on disconnect.
typedef void (*usb_printer_event_cb_t)(usb_device_handle_t dev_handle, bool connected, const ql_model_t *model);
void usb_host_set_printer_callback(usb_printer_event_cb_t cb);

// Get the client handle (needed for interface claim/release)
usb_host_client_handle_t usb_host_get_client_handle();
