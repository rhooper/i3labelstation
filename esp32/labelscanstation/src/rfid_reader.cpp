#include "rfid_reader.h"
#include "app_config.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "rfid";

static QueueHandle_t s_card_queue = nullptr;

// HZ-1050 sends 4 raw bytes per card scan at 9600 baud
#define HZ1050_FRAME_SIZE 4
#define UART_BUF_SIZE     256

static void rfid_task(void *arg) {
    uint8_t buf[UART_BUF_SIZE];
    int buffered = 0;
    int poll_count = 0;

    ESP_LOGI(TAG, "RFID reader task started (UART%d, GPIO%d, %d baud)",
             RFID_UART_NUM, RFID_UART_RX_GPIO, RFID_UART_BAUD);

    while (true) {
        // Read whatever is available (up to buffer size) with 500ms timeout
        int len = uart_read_bytes(RFID_UART_NUM, buf + buffered,
                                  sizeof(buf) - buffered, pdMS_TO_TICKS(500));

        // Periodic status log every ~5s (10 * 500ms)
        poll_count++;
        if (poll_count % 10 == 0) {
            size_t uart_buffered = 0;
            uart_get_buffered_data_len(RFID_UART_NUM, &uart_buffered);
            ESP_LOGI(TAG, "RFID poll: read=%d, buffered=%d, uart_pending=%d",
                     len, buffered, (int)uart_buffered);
        }

        if (len > 0) {
            // Log raw bytes received
            ESP_LOGI(TAG, "RFID RX %d bytes:", len);
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, buf + buffered, len, ESP_LOG_INFO);
            buffered += len;
        }

        // Process all complete 4-byte frames
        while (buffered >= HZ1050_FRAME_SIZE) {
            uint32_t card_id = ((uint32_t)buf[0] << 24) |
                               ((uint32_t)buf[1] << 16) |
                               ((uint32_t)buf[2] << 8)  |
                               (uint32_t)buf[3];
            ESP_LOGI(TAG, "Card scanned: 0x%08lX (%lu)", (unsigned long)card_id, (unsigned long)card_id);
            xQueueSend(s_card_queue, &card_id, 0);
            // Shift remaining bytes forward
            buffered -= HZ1050_FRAME_SIZE;
            if (buffered > 0) {
                memmove(buf, buf + HZ1050_FRAME_SIZE, buffered);
            }
        }
    }
}

QueueHandle_t rfid_init() {
    uart_config_t uart_config = {};
    uart_config.baud_rate = RFID_UART_BAUD;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_driver_install(RFID_UART_NUM, UART_BUF_SIZE * 2, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(RFID_UART_NUM, &uart_config));
    // RX only — TX pin not used (-1)
    ESP_ERROR_CHECK(uart_set_pin(RFID_UART_NUM, UART_PIN_NO_CHANGE, RFID_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    s_card_queue = xQueueCreate(4, sizeof(uint32_t));
    xTaskCreate(rfid_task, "rfid_task", 4096, nullptr, 2, nullptr);

    ESP_LOGI(TAG, "RFID reader initialized (UART%d, RX=GPIO%d, %d baud)",
             RFID_UART_NUM, RFID_UART_RX_GPIO, RFID_UART_BAUD);
    return s_card_queue;
}
