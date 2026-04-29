#include "rfid_reader.h"
#include "app_config.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <HardwareSerial.h>

static const char *TAG = "rfid";

// HZ-1050 sends 4 raw bytes per card scan at 9600 baud
#define HZ1050_FRAME_SIZE 4

static QueueHandle_t s_card_queue = nullptr;

static void rfid_task(void *arg) {
    uint8_t buf[16];
    int buffered = 0;
    int poll_count = 0;

    ESP_LOGI(TAG, "RFID reader task started (Serial1, GPIO%d, 9600 baud)", RFID_UART_RX_GPIO);

    while (true) {
        // Collect available bytes (non-blocking)
        while (Serial1.available() && buffered < (int)sizeof(buf)) {
            buf[buffered++] = (uint8_t)Serial1.read();
        }

        // Periodic status log every ~5s (10 * 500ms)
        poll_count++;
        if (poll_count % 10 == 0) {
            ESP_LOGI(TAG, "RFID poll: buffered=%d, uart_pending=%d", buffered, Serial1.available());
        }

        // Process all complete 4-byte frames
        while (buffered >= HZ1050_FRAME_SIZE) {
            uint32_t card_id = ((uint32_t)buf[0] << 24) |
                               ((uint32_t)buf[1] << 16) |
                               ((uint32_t)buf[2] << 8)  |
                               (uint32_t)buf[3];
            ESP_LOGI(TAG, "Card scanned: 0x%08lX (%lu)", (unsigned long)card_id, (unsigned long)card_id);
            xQueueSend(s_card_queue, &card_id, 0);
            buffered -= HZ1050_FRAME_SIZE;
            if (buffered > 0) {
                memmove(buf, buf + HZ1050_FRAME_SIZE, buffered);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

QueueHandle_t rfid_init() {
    // RX only: TX pin = -1
    Serial1.begin(RFID_UART_BAUD, SERIAL_8N1, RFID_UART_RX_GPIO, -1);

    s_card_queue = xQueueCreate(4, sizeof(uint32_t));
    xTaskCreate(rfid_task, "rfid_task", 4096, nullptr, 2, nullptr);

    ESP_LOGI(TAG, "RFID reader initialized (Serial1, RX=GPIO%d, %d baud)",
             RFID_UART_RX_GPIO, RFID_UART_BAUD);
    return s_card_queue;
}
