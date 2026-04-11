#include "usb_printer.h"
#include "app_config.h"

#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "usb/usb_host.h"

static const char *TAG = "usb_printer";

// Semaphore for synchronous transfers
static SemaphoreHandle_t s_xfer_done = nullptr;
static esp_err_t s_xfer_result = ESP_OK;
static int s_xfer_actual_len = 0;

static void transfer_cb(usb_transfer_t *transfer) {
    s_xfer_result = (transfer->status == USB_TRANSFER_STATUS_COMPLETED) ? ESP_OK : ESP_FAIL;
    s_xfer_actual_len = transfer->actual_num_bytes;
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGE(TAG, "Transfer failed, status=%d", transfer->status);
    }
    xSemaphoreGive(s_xfer_done);
}

void printer_init(PrinterState *state) {
    memset(state, 0, sizeof(*state));
    state->interface_number = 0xFF;
    state->connected = false;

    if (s_xfer_done == nullptr) {
        s_xfer_done = xSemaphoreCreateBinary();
    }
}

bool printer_on_connected(PrinterState *state, usb_device_handle_t dev_handle,
                          usb_host_client_handle_t client_handle) {
    state->dev_handle = dev_handle;
    state->client_handle = client_handle;

    const usb_config_desc_t *config_desc;
    esp_err_t err = usb_host_get_active_config_descriptor(dev_handle, &config_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get_active_config_descriptor failed: %s", esp_err_to_name(err));
        return false;
    }

    // Walk descriptors to find printer interface (class 0x07) with bulk endpoints
    int conf_offset = 0;
    bool found = false;

    for (uint8_t intf_idx = 0; intf_idx < config_desc->bNumInterfaces; intf_idx++) {
        const usb_intf_desc_t *intf_desc = usb_parse_interface_descriptor(
            config_desc, intf_idx, 0, &conf_offset);
        if (!intf_desc)
            break;

        ESP_LOGD(TAG, "Interface %d: class=0x%02X subclass=0x%02X protocol=0x%02X eps=%d",
                 intf_desc->bInterfaceNumber, intf_desc->bInterfaceClass,
                 intf_desc->bInterfaceSubClass, intf_desc->bInterfaceProtocol,
                 intf_desc->bNumEndpoints);

        // USB Printer Class = 0x07, or vendor-specific (0xFF)
        if (intf_desc->bInterfaceClass != 0x07 && intf_desc->bInterfaceClass != 0xFF)
            continue;

        state->bulk_in_addr = 0;
        state->bulk_out_addr = 0;

        for (uint8_t ep_idx = 0; ep_idx < intf_desc->bNumEndpoints; ep_idx++) {
            int ep_offset = conf_offset;
            const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(
                intf_desc, ep_idx, config_desc->wTotalLength, &ep_offset);
            if (!ep)
                break;

            if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) != USB_BM_ATTRIBUTES_XFER_BULK)
                continue;

            if (ep->bEndpointAddress & 0x80) {  // IN
                state->bulk_in_addr = ep->bEndpointAddress;
                state->bulk_in_mps = ep->wMaxPacketSize;
                ESP_LOGD(TAG, "BULK IN: 0x%02X (MPS=%d)", ep->bEndpointAddress, ep->wMaxPacketSize);
            } else {  // OUT
                state->bulk_out_addr = ep->bEndpointAddress;
                state->bulk_out_mps = ep->wMaxPacketSize;
                ESP_LOGD(TAG, "BULK OUT: 0x%02X (MPS=%d)", ep->bEndpointAddress, ep->wMaxPacketSize);
            }
        }

        if (state->bulk_out_addr != 0) {
            state->interface_number = intf_desc->bInterfaceNumber;
            found = true;
            break;
        }
    }

    if (!found || state->bulk_out_addr == 0) {
        ESP_LOGE(TAG, "No suitable printer interface found");
        return false;
    }

    err = usb_host_interface_claim(client_handle, dev_handle, state->interface_number, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_interface_claim failed: %s", esp_err_to_name(err));
        return false;
    }

    state->connected = true;
    ESP_LOGI(TAG, "Printer connected (interface %d, OUT=0x%02X, IN=0x%02X)",
             state->interface_number, state->bulk_out_addr, state->bulk_in_addr);
    return true;
}

void printer_on_disconnected(PrinterState *state) {
    if (!state->connected)
        return;

    if (state->bulk_out_addr != 0) {
        usb_host_endpoint_halt(state->dev_handle, state->bulk_out_addr);
        usb_host_endpoint_flush(state->dev_handle, state->bulk_out_addr);
    }
    if (state->bulk_in_addr != 0) {
        usb_host_endpoint_halt(state->dev_handle, state->bulk_in_addr);
        usb_host_endpoint_flush(state->dev_handle, state->bulk_in_addr);
    }
    if (state->interface_number != 0xFF) {
        usb_host_interface_release(state->client_handle, state->dev_handle, state->interface_number);
    }

    usb_host_device_close(state->client_handle, state->dev_handle);

    state->bulk_in_addr = 0;
    state->bulk_out_addr = 0;
    state->interface_number = 0xFF;
    state->connected = false;
    state->dev_handle = nullptr;
    ESP_LOGW(TAG, "Printer disconnected");
}

esp_err_t printer_send(PrinterState *state, const uint8_t *data, size_t len) {
    if (!state->connected || state->bulk_out_addr == 0)
        return ESP_ERR_INVALID_STATE;

    // Allocate a USB transfer
    usb_transfer_t *xfer = nullptr;
    size_t chunk_size = state->bulk_out_mps > 0 ? state->bulk_out_mps : USB_XFER_SIZE;
    esp_err_t err = usb_host_transfer_alloc(chunk_size, 0, &xfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "transfer_alloc failed: %s", esp_err_to_name(err));
        return err;
    }

    xfer->device_handle = state->dev_handle;
    xfer->bEndpointAddress = state->bulk_out_addr;
    xfer->callback = transfer_cb;
    xfer->timeout_ms = 5000;

    size_t offset = 0;
    while (offset < len) {
        size_t to_send = len - offset;
        if (to_send > chunk_size)
            to_send = chunk_size;

        memcpy(xfer->data_buffer, data + offset, to_send);
        xfer->num_bytes = to_send;

        err = usb_host_transfer_submit(xfer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "transfer_submit failed at offset %zu: %s", offset, esp_err_to_name(err));
            break;
        }

        // Wait for completion
        if (xSemaphoreTake(s_xfer_done, pdMS_TO_TICKS(10000)) != pdTRUE) {
            ESP_LOGE(TAG, "Transfer timeout at offset %zu", offset);
            err = ESP_ERR_TIMEOUT;
            break;
        }

        if (s_xfer_result != ESP_OK) {
            ESP_LOGE(TAG, "Transfer error at offset %zu", offset);
            err = s_xfer_result;
            break;
        }

        offset += to_send;
    }

    usb_host_transfer_free(xfer);
    return err;
}

int printer_read_status(PrinterState *state, uint8_t *buf, size_t buf_len) {
    if (!state->connected || state->bulk_in_addr == 0)
        return -1;

    usb_transfer_t *xfer = nullptr;
    size_t read_size = buf_len < 64 ? 64 : buf_len;
    esp_err_t err = usb_host_transfer_alloc(read_size, 0, &xfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "transfer_alloc for read failed: %s", esp_err_to_name(err));
        return -1;
    }

    xfer->device_handle = state->dev_handle;
    xfer->bEndpointAddress = state->bulk_in_addr;
    xfer->callback = transfer_cb;
    xfer->num_bytes = read_size;
    xfer->timeout_ms = 5000;

    err = usb_host_transfer_submit(xfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read transfer_submit failed: %s", esp_err_to_name(err));
        usb_host_transfer_free(xfer);
        return -1;
    }

    if (xSemaphoreTake(s_xfer_done, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGE(TAG, "Read transfer timeout");
        usb_host_transfer_free(xfer);
        return -1;
    }

    int result = -1;
    if (s_xfer_result == ESP_OK && s_xfer_actual_len > 0) {
        size_t copy_len = s_xfer_actual_len < (int)buf_len ? s_xfer_actual_len : buf_len;
        memcpy(buf, xfer->data_buffer, copy_len);
        result = copy_len;
    }

    usb_host_transfer_free(xfer);
    return result;
}
