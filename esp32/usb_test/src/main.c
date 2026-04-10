#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "usb/usb_host.h"

static const char *TAG = "usb_test";

static usb_phy_handle_t phy_handle = NULL;
static usb_host_client_handle_t client_handle = NULL;

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGI(TAG, "*** NEW USB DEVICE detected! Address: %d ***", event_msg->new_dev.address);

        // Try to open and get descriptor
        usb_device_handle_t dev_handle;
        if (usb_host_device_open(client_handle, event_msg->new_dev.address, &dev_handle) == ESP_OK) {
            const usb_device_desc_t *desc;
            if (usb_host_get_device_descriptor(dev_handle, &desc) == ESP_OK) {
                ESP_LOGI(TAG, "  VID: 0x%04X  PID: 0x%04X", desc->idVendor, desc->idProduct);
                ESP_LOGI(TAG, "  Class: 0x%02X  SubClass: 0x%02X  Protocol: 0x%02X",
                         desc->bDeviceClass, desc->bDeviceSubClass, desc->bDeviceProtocol);
            }
            usb_device_info_t dev_info;
            if (usb_host_device_info(dev_handle, &dev_info) == ESP_OK) {
                ESP_LOGI(TAG, "  Speed: %s", dev_info.speed == USB_SPEED_LOW ? "LOW" : "FULL");
            }
            usb_host_device_close(client_handle, dev_handle);
        }
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGW(TAG, "USB device disconnected");
        break;
    default:
        ESP_LOGW(TAG, "Unknown USB event: %d", event_msg->event);
        break;
    }
}

static void usb_host_task(void *arg)
{
    ESP_LOGI(TAG, "USB host task started");
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGW(TAG, "No more clients");
        }
    }
}

static void usb_client_task(void *arg)
{
    ESP_LOGI(TAG, "USB client task started");
    while (1) {
        usb_host_client_handle_events(client_handle, portMAX_DELAY);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== USB Host Test ===");

    // Step 1: Initialize USB PHY
    ESP_LOGI(TAG, "Initializing USB PHY...");
    usb_phy_config_t phy_config = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_HOST,
        .otg_speed = USB_PHY_SPEED_UNDEFINED,
        .otg_io_conf = NULL,
    };
    esp_err_t err = usb_new_phy(&phy_config, &phy_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB PHY init FAILED: %s (0x%x)", esp_err_to_name(err), err);
        // Continue anyway to see if usb_host_install handles it
    } else {
        ESP_LOGI(TAG, "USB PHY init OK");
    }

    // Step 2: Install USB host library
    ESP_LOGI(TAG, "Installing USB host library...");
    usb_host_config_t host_config = {
        .skip_phy_setup = (phy_handle != NULL),  // skip if we already set it up
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install FAILED: %s (0x%x)", esp_err_to_name(err), err);
        return;
    }
    ESP_LOGI(TAG, "usb_host_install OK");

    // Step 3: Start host library task
    xTaskCreate(usb_host_task, "usb_host", 4096, NULL, 2, NULL);

    // Step 4: Register client
    ESP_LOGI(TAG, "Registering USB client...");
    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    err = usb_host_client_register(&client_config, &client_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Client register FAILED: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Client registered OK");

    // Step 5: Start client task
    xTaskCreate(usb_client_task, "usb_client", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "=== Waiting for USB devices... Plug something in! ===");

    // Main loop - just print status periodically
    while (1) {
        ESP_LOGI(TAG, "Heartbeat - waiting for USB device...");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
