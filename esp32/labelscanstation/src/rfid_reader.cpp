#include "rfid_reader.h"
#include "app_config.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <cstring>

static const char *TAG = "rfid";

static QueueHandle_t s_card_queue = nullptr;

// HZ-1050 sends 4 raw bytes per card scan at 9600 baud
#define HZ1050_FRAME_SIZE 4

// RDM6300 sends 14-byte ASCII frames at 9600 baud:
// 0x02 + 10 hex chars (2 version + 8 card ID) + 2 hex checksum + 0x03
#define RDM6300_FRAME_SIZE 14
#define RDM6300_STX        0x02
#define RDM6300_ETX        0x03

#define UART_BUF_SIZE     256

// Convert a single ASCII hex character to its 4-bit value.
// Returns -1 on invalid input.
static int hex_char(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Try to parse an RDM6300 frame starting at buf[0] == STX.
// Returns the card ID on success, 0 on failure.
static uint32_t rdm6300_parse(const uint8_t *frame) {
    if (frame[0] != RDM6300_STX || frame[13] != RDM6300_ETX)
        return 0;

    // Decode 10 hex chars (bytes 1–10) into 5 bytes
    uint8_t decoded[5];
    for (int i = 0; i < 5; i++) {
        int hi = hex_char(frame[1 + i * 2]);
        int lo = hex_char(frame[2 + i * 2]);
        if (hi < 0 || lo < 0) return 0;
        decoded[i] = (hi << 4) | lo;
    }

    // Decode 2-char checksum (bytes 11–12)
    int ck_hi = hex_char(frame[11]);
    int ck_lo = hex_char(frame[12]);
    if (ck_hi < 0 || ck_lo < 0) return 0;
    uint8_t checksum = (ck_hi << 4) | ck_lo;

    // Verify XOR checksum over the 5 decoded bytes
    uint8_t xor_sum = 0;
    for (int i = 0; i < 5; i++) xor_sum ^= decoded[i];
    if (xor_sum != checksum) {
        ESP_LOGW(TAG, "RDM6300 checksum mismatch: got 0x%02X, expected 0x%02X", xor_sum, checksum);
        return 0;
    }

    // decoded[0] = version byte, decoded[1..4] = 32-bit card ID
    uint32_t card_id = ((uint32_t)decoded[1] << 24) |
                       ((uint32_t)decoded[2] << 16) |
                       ((uint32_t)decoded[3] << 8)  |
                       (uint32_t)decoded[4];
    return card_id;
}

static void rfid_task(void *arg) {
    uint8_t buf[UART_BUF_SIZE];
    int buffered = 0;
    int poll_count = 0;
    bool detected_rdm6300 = false;
    bool detected_hz1050 = false;

    ESP_LOGI(TAG, "RFID reader task started (UART%d, GPIO%d, %d baud)",
             RFID_UART_NUM, RFID_UART_RX_GPIO, RFID_UART_BAUD);

    while (true) {
        int len = uart_read_bytes(RFID_UART_NUM, buf + buffered,
                                  sizeof(buf) - buffered, pdMS_TO_TICKS(500));

        poll_count++;
        if (poll_count % 10 == 0) {
            size_t uart_buffered = 0;
            uart_get_buffered_data_len(RFID_UART_NUM, &uart_buffered);
            ESP_LOGI(TAG, "RFID poll: read=%d, buffered=%d, uart_pending=%d, reader=%s",
                     len, buffered, (int)uart_buffered,
                     detected_rdm6300 ? "RDM6300" :
                     detected_hz1050  ? "HZ-1050" : "unknown");
        }

        if (len > 0) {
            ESP_LOGI(TAG, "RFID RX %d bytes:", len);
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, buf + buffered, len, ESP_LOG_INFO);
            buffered += len;
        }

        // --- Try RDM6300 frames first (look for STX byte) ---
        bool found_rdm = true;
        while (found_rdm) {
            found_rdm = false;
            // Scan for STX
            for (int i = 0; i < buffered; i++) {
                if (buf[i] != RDM6300_STX) continue;

                // Not enough data for a complete frame yet
                if (buffered - i < RDM6300_FRAME_SIZE) {
                    // Discard bytes before STX
                    if (i > 0) {
                        memmove(buf, buf + i, buffered - i);
                        buffered -= i;
                    }
                    goto done_processing;
                }

                uint32_t card_id = rdm6300_parse(buf + i);
                if (card_id != 0) {
                    if (!detected_rdm6300) {
                        ESP_LOGI(TAG, "RDM6300 reader detected");
                        detected_rdm6300 = true;
                    }
                    ESP_LOGI(TAG, "Card scanned (RDM6300): 0x%08lX (%lu)",
                             (unsigned long)card_id, (unsigned long)card_id);
                    xQueueSend(s_card_queue, &card_id, 0);
                    // Remove STX through ETX
                    int consumed = i + RDM6300_FRAME_SIZE;
                    buffered -= consumed;
                    if (buffered > 0) memmove(buf, buf + consumed, buffered);
                    found_rdm = true;
                    break;
                } else {
                    // Bad frame — skip this STX and keep scanning
                    continue;
                }
            }
        }

        // --- Fall back to HZ-1050 raw 4-byte frames ---
        if (!detected_rdm6300) {
            while (buffered >= HZ1050_FRAME_SIZE) {
                if (!detected_hz1050) {
                    ESP_LOGI(TAG, "HZ-1050 reader detected");
                    detected_hz1050 = true;
                }
                uint32_t card_id = ((uint32_t)buf[0] << 24) |
                                   ((uint32_t)buf[1] << 16) |
                                   ((uint32_t)buf[2] << 8)  |
                                   (uint32_t)buf[3];
                ESP_LOGI(TAG, "Card scanned (HZ-1050): 0x%08lX (%lu)",
                         (unsigned long)card_id, (unsigned long)card_id);
                xQueueSend(s_card_queue, &card_id, 0);
                buffered -= HZ1050_FRAME_SIZE;
                if (buffered > 0) memmove(buf, buf + HZ1050_FRAME_SIZE, buffered);
            }
        }

        done_processing:
        (void)0; // label requires a statement
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

    ESP_LOGI(TAG, "RFID reader initialized (UART%d, RX=GPIO%d, %d baud, auto-detect HZ-1050/RDM6300)",
             RFID_UART_NUM, RFID_UART_RX_GPIO, RFID_UART_BAUD);
    return s_card_queue;
}
