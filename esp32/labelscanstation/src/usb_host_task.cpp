#include "usb_host_task.h"
#include "app_config.h"

#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"

static const char *TAG = "usb_host";

static usb_phy_handle_t s_phy_handle = nullptr;
static usb_host_client_handle_t s_client_handle = nullptr;
static usb_printer_event_cb_t s_printer_cb = nullptr;
static usb_device_handle_t s_printer_dev_handle = nullptr;

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg) {
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV: {
        ESP_LOGI(TAG, "New USB device, address: %d", event_msg->new_dev.address);

        usb_device_handle_t dev_handle;
        if (usb_host_device_open(s_client_handle, event_msg->new_dev.address, &dev_handle) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open device");
            break;
        }

        const usb_device_desc_t *desc;
        if (usb_host_get_device_descriptor(dev_handle, &desc) == ESP_OK) {
            ESP_LOGI(TAG, "  VID: 0x%04X  PID: 0x%04X", desc->idVendor, desc->idProduct);
            ESP_LOGI(TAG, "  Class: 0x%02X  SubClass: 0x%02X  Protocol: 0x%02X",
                     desc->bDeviceClass, desc->bDeviceSubClass, desc->bDeviceProtocol);

            if (desc->idVendor == BROTHER_QL_VID && desc->idProduct == BROTHER_QL_PID) {
                ESP_LOGI(TAG, "Brother QL-500 detected!");
                if (s_printer_cb) {
                    s_printer_dev_handle = dev_handle;
                    s_printer_cb(dev_handle, true);
                    return;  // don't close — printer_on_connected takes ownership
                }
            } else if (desc->bDeviceClass == 0x09) {
                ESP_LOGI(TAG, "USB Hub detected (will enumerate downstream devices)");
            }
        }

        usb_device_info_t dev_info;
        if (usb_host_device_info(dev_handle, &dev_info) == ESP_OK) {
            ESP_LOGI(TAG, "  Speed: %s", dev_info.speed == USB_SPEED_LOW ? "LOW" : "FULL");
        }

        usb_host_device_close(s_client_handle, dev_handle);
        break;
    }
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGW(TAG, "USB device disconnected (handle=%p, printer=%p)",
                 event_msg->dev_gone.dev_hdl, s_printer_dev_handle);
        if (s_printer_cb && event_msg->dev_gone.dev_hdl == s_printer_dev_handle) {
            s_printer_dev_handle = nullptr;
            s_printer_cb(nullptr, false);
        }
        break;
    default:
        ESP_LOGW(TAG, "Unknown USB event: %d", event_msg->event);
        break;
    }
}

static void usb_host_lib_task(void *arg) {
    ESP_LOGI(TAG, "USB host library task started");
    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGW(TAG, "No more clients");
        }
    }
}

static void usb_client_task(void *arg) {
    ESP_LOGI(TAG, "USB client task started");
    while (true) {
        usb_host_client_handle_events(s_client_handle, portMAX_DELAY);
    }
}

void usb_host_set_printer_callback(usb_printer_event_cb_t cb) {
    s_printer_cb = cb;
}

usb_host_client_handle_t usb_host_get_client_handle() {
    return s_client_handle;
}

void usb_host_init() {
    // Step 1: Initialize USB PHY for OTG Host mode
    ESP_LOGI(TAG, "Initializing USB PHY...");
    usb_phy_config_t phy_config = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_HOST,
        .otg_speed = USB_PHY_SPEED_UNDEFINED,
        .otg_io_conf = nullptr,
    };
    esp_err_t err = usb_new_phy(&phy_config, &s_phy_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB PHY init FAILED: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "USB PHY init OK");
    }

    // Step 2: Install USB host library
    ESP_LOGI(TAG, "Installing USB host library...");
    usb_host_config_t host_config = {
        .skip_phy_setup = (s_phy_handle != nullptr),
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install FAILED: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "usb_host_install OK");

    // Step 3: Start host library event task
    xTaskCreate(usb_host_lib_task, "usb_host_lib", 4096, nullptr, 2, nullptr);

    // Step 4: Register client
    ESP_LOGI(TAG, "Registering USB client...");
    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = nullptr,
        },
    };
    err = usb_host_client_register(&client_config, &s_client_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Client register FAILED: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Client registered OK");

    // Step 5: Start client event task
    xTaskCreate(usb_client_task, "usb_client", 4096, nullptr, 3, nullptr);

    ESP_LOGI(TAG, "USB host initialized — waiting for devices...");
}
