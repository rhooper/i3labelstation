// HD44780 LCD driver via PCF8574 I2C backpack
// Standard PCF8574 pin mapping:
//   P0=RS  P1=RW  P2=EN  P3=Backlight  P4-P7=D4-D7

#include "lcd_display.h"
#include "app_config.h"

#include <cstring>
#include <cstdio>
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lcd";

// PCF8574 bit positions
#define LCD_RS   (1 << 0)
#define LCD_RW   (1 << 1)
#define LCD_EN   (1 << 2)
#define LCD_BL   (1 << 3)

// HD44780 commands
#define LCD_CMD_CLEAR       0x01
#define LCD_CMD_HOME        0x02
#define LCD_CMD_ENTRY_MODE  0x06  // increment, no shift
#define LCD_CMD_DISPLAY_ON  0x0C  // display on, cursor off, blink off
#define LCD_CMD_FUNCTION    0x28  // 4-bit, 2 lines, 5x8
#define LCD_CMD_SET_DDRAM   0x80

// Display geometry
#define LCD_COLS 16
#define LCD_ROWS 2

// DDRAM addresses for each row (HD44780 standard)
static const uint8_t ROW_OFFSETS[] = {0x00, 0x40, 0x14, 0x54};

static uint8_t s_addr = 0;
static bool s_backlight = true;
static bool s_initialized = false;

static esp_err_t pcf8574_write(uint8_t data) {
    uint8_t bl = s_backlight ? LCD_BL : 0;
    uint8_t val = data | bl;
    return i2c_master_write_to_device(LCD_I2C_PORT, s_addr, &val, 1, pdMS_TO_TICKS(10));
}

static void lcd_pulse_enable(uint8_t data) {
    pcf8574_write(data | LCD_EN);
    esp_rom_delay_us(1);
    pcf8574_write(data & ~LCD_EN);
    esp_rom_delay_us(50);
}

static void lcd_write_nibble(uint8_t nibble, bool rs) {
    uint8_t data = (nibble & 0xF0) | (rs ? LCD_RS : 0);
    lcd_pulse_enable(data);
}

static void lcd_write_byte(uint8_t byte, bool rs) {
    lcd_write_nibble(byte & 0xF0, rs);
    lcd_write_nibble((byte << 4) & 0xF0, rs);
}

static void lcd_command(uint8_t cmd) {
    lcd_write_byte(cmd, false);
}

static void lcd_data(uint8_t ch) {
    lcd_write_byte(ch, true);
}

// Try to detect PCF8574 at given address on already-configured I2C bus
static bool try_address(uint8_t addr) {
    // Just try writing 0x00 — if ACK received, device is present
    uint8_t dummy = 0x00;
    esp_err_t err = i2c_master_write_to_device(LCD_I2C_PORT, addr, &dummy, 1, pdMS_TO_TICKS(50));
    return err == ESP_OK;
}

// Initialize I2C bus with given SDA/SCL pins, returns ESP_OK if successful
static esp_err_t init_i2c(int sda_pin, int scl_pin) {
    // Delete existing driver if any (in case we're retrying with swapped pins)
    i2c_driver_delete(LCD_I2C_PORT);

    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = sda_pin;
    conf.scl_io_num = scl_pin;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = 100000;

    esp_err_t err = i2c_param_config(LCD_I2C_PORT, &conf);
    if (err != ESP_OK) return err;

    return i2c_driver_install(LCD_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
}

// HD44780 4-bit mode init sequence per datasheet
static void lcd_init_sequence() {
    vTaskDelay(pdMS_TO_TICKS(50));  // wait >40ms after power-on

    // Force into 4-bit mode from any state (3x 0x30 then 0x20)
    lcd_write_nibble(0x30, false);
    vTaskDelay(pdMS_TO_TICKS(5));
    lcd_write_nibble(0x30, false);
    vTaskDelay(pdMS_TO_TICKS(1));
    lcd_write_nibble(0x30, false);
    esp_rom_delay_us(150);
    lcd_write_nibble(0x20, false);  // set 4-bit mode
    esp_rom_delay_us(150);

    lcd_command(LCD_CMD_FUNCTION);   // 4-bit, 2-line, 5x8
    lcd_command(LCD_CMD_DISPLAY_ON); // display on
    lcd_command(LCD_CMD_CLEAR);      // clear
    vTaskDelay(pdMS_TO_TICKS(2));
    lcd_command(LCD_CMD_ENTRY_MODE); // increment cursor
}

void lcd_init() {
    // Try configured pin order first, then swapped
    struct { int sda; int scl; const char *label; } pin_configs[] = {
        {LCD_SDA_GPIO, LCD_SCL_GPIO, "SDA/SCL as configured"},
        {LCD_SCL_GPIO, LCD_SDA_GPIO, "SDA/SCL swapped"},
    };

    for (auto &pins : pin_configs) {
        esp_err_t err = init_i2c(pins.sda, pins.scl);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2C init failed with %s: %s", pins.label, esp_err_to_name(err));
            continue;
        }

        // Scan full I2C address range for this pin configuration
        ESP_LOGI(TAG, "I2C scan (%s, SDA=%d SCL=%d):", pins.label, pins.sda, pins.scl);
        int found_count = 0;
        for (uint8_t addr = 0x08; addr < 0x78; addr++) {
            if (try_address(addr)) {
                ESP_LOGI(TAG, "  Device at 0x%02X", addr);
                found_count++;
                // Use first device found as LCD address (PCF8574 is typically the only device)
                if (s_addr == 0) {
                    s_addr = addr;
                }
            }
        }
        if (found_count == 0) {
            ESP_LOGI(TAG, "  (no devices found)");
            continue;
        }

        ESP_LOGI(TAG, "LCD using address 0x%02X (%s, SDA=%d SCL=%d)",
                 s_addr, pins.label, pins.sda, pins.scl);
        lcd_init_sequence();
        s_initialized = true;
        return;
    }

    ESP_LOGW(TAG, "No I2C devices found on either pin configuration — LCD disabled");
}

void lcd_set_line(int line, const char *text) {
    if (!s_initialized || line < 0 || line >= LCD_ROWS) return;

    // Build the padded 16-char view actually written to DDRAM.
    char rendered[LCD_COLS + 1];
    int len = strlen(text);
    for (int i = 0; i < LCD_COLS; i++) {
        rendered[i] = i < len ? text[i] : ' ';
    }
    rendered[LCD_COLS] = '\0';

    // Only log when the line's contents actually change — the update task
    // repaints every 500ms and the clock line ticks once a second.
    static char s_last[LCD_ROWS][LCD_COLS + 1] = {{0}};
    if (strcmp(s_last[line], rendered) != 0) {
        ESP_LOGI(TAG, "L%d|%s|", line, rendered);
        memcpy(s_last[line], rendered, sizeof(rendered));
    }

    lcd_command(LCD_CMD_SET_DDRAM | ROW_OFFSETS[line]);
    for (int i = 0; i < LCD_COLS; i++) {
        lcd_data((uint8_t)rendered[i]);
    }
}

void lcd_status(const char *line0, const char *line1) {
    lcd_set_line(0, line0);
    lcd_set_line(1, line1);
}

void lcd_clear() {
    if (!s_initialized) return;
    lcd_command(LCD_CMD_CLEAR);
    vTaskDelay(pdMS_TO_TICKS(2));
}

void lcd_backlight(bool on) {
    s_backlight = on;
    if (s_initialized) {
        pcf8574_write(0);  // just updates backlight bit
    }
}

void lcd_create_char(uint8_t slot, const uint8_t *pattern) {
    if (!s_initialized || slot > 7) return;
    lcd_command(0x40 | (slot << 3));  // set CGRAM address
    for (int i = 0; i < 8; i++) {
        lcd_data(pattern[i] & 0x1F);
    }
    lcd_command(LCD_CMD_SET_DDRAM);  // return to DDRAM
}
