#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"

// Brother QL-500 USB identifiers
#define BROTHER_QL_VID 0x04F9
#define BROTHER_QL_PID 0x2015

// Brother QL-500 model capabilities
#define QL500_BYTES_PER_ROW 90
#define QL500_COMPRESSION   false
#define QL500_CUTTING       false

// Label: 29x90mm die-cut
#define LABEL_PRINTABLE_W   306
#define LABEL_PRINTABLE_H   991
#define LABEL_WIDTH_MM      29
#define LABEL_HEIGHT_MM     90
#define LABEL_MEDIA_TYPE    0x0B  // die-cut
#define LABEL_FEED_MARGIN   0
#define LABEL_RIGHT_MARGIN  6

// Derived
#define LABEL_FB_STRIDE     ((LABEL_PRINTABLE_W + 7) / 8)  // 39 bytes
#define LABEL_FB_SIZE       (LABEL_FB_STRIDE * LABEL_PRINTABLE_H)  // ~38KB

// GPIO
#define BUTTON_GPIO         GPIO_NUM_0
// RGB LED (WS2812 on YD-ESP32-S3)
#define RGB_LED_GPIO        GPIO_NUM_48

// RFID
#define RFID_UART_NUM       UART_NUM_1
#define RFID_UART_RX_GPIO   GPIO_NUM_18
#define RFID_UART_BAUD      9600

// WiFi (from secrets.h)
// #define WIFI_SSID and WIFI_PASS in secrets.h

// USB transfer
#define USB_XFER_SIZE       64
