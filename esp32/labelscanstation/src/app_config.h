#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"

// Brother QL USB vendor ID (shared across all models)
#define BROTHER_QL_VID 0x04F9

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

// I2C LCD (HD44780 via PCF8574 backpack)
// YD-ESP32-S3 docs: GPIO8=SDA, GPIO9=SCL
// If display doesn't work, the driver auto-tries swapped pins.
#define LCD_SDA_GPIO        8
#define LCD_SCL_GPIO        9
#define LCD_I2C_PORT        I2C_NUM_0

// Buzzer (passive piezo via PWM)
#define BUZZER_GPIO         GPIO_NUM_12

// Force the firmware to treat loaded media as 29mm continuous tape regardless
// of what the printer reports. Used because the deployed printer carries
// generic continuous tape that lacks the Brother spool tag — without an
// override the printer reports it as die-cut 29x90mm and Mode 2 (SHORT)
// renders a 2-up stacked layout instead of a single short label.
// Set to 0 to use the printer's actual media report.
#define FORCE_MEDIA_CONTINUOUS 1

// Option button (normally open, grounded when pressed; use internal pull-up)
#define OPTION_BUTTON_GPIO  GPIO_NUM_17

// 3-position mode switch (each pin pulled up internally, grounded by switch)
//   Mode 1: A open,  B open
//   Mode 2: A open,  B gnd
//   Mode 3: A gnd,   B gnd
#define MODE_SWITCH_A_GPIO  GPIO_NUM_11
#define MODE_SWITCH_B_GPIO  GPIO_NUM_13

// Max printers supported via USB hub
#define MAX_PRINTERS        3

// USB transfer
#define USB_XFER_SIZE       64
