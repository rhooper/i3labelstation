#include "buzzer.h"
#include "app_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char *TAG = "buzzer";

// BPM 120: quarter = 500ms, eighth = 250ms
#define BPM         120
#define QUARTER_MS  (60000 / BPM)
#define EIGHTH_MS   (QUARTER_MS / 2)

// Note frequencies (Hz)
#define NOTE_C4   262
#define NOTE_E4   330
#define NOTE_G4   392
#define NOTE_Ab4  415
#define NOTE_C5   523
#define NOTE_C7  2093

struct buzzer_note_t {
    uint16_t freq_hz;
    uint16_t duration_ms;
};

enum buzzer_melody_t : uint8_t {
    MELODY_GOOD,
    MELODY_BAD,
    MELODY_SAD,
};

static const buzzer_note_t s_melody_good[] = {
    {NOTE_C7, QUARTER_MS * 80 / 100}, {0, QUARTER_MS * 20 / 100},
    {NOTE_C7, QUARTER_MS * 80 / 100}, {0, QUARTER_MS * 20 / 100},
    {0, 0}
};

static const buzzer_note_t s_melody_bad[] = {
    {NOTE_C4, EIGHTH_MS},
    {NOTE_Ab4, QUARTER_MS},
    {0, 0}
};

static const buzzer_note_t s_melody_sad[] = {
    {NOTE_C5, EIGHTH_MS * 80 / 100}, {0, EIGHTH_MS * 20 / 100},
    {NOTE_G4, EIGHTH_MS * 80 / 100}, {0, EIGHTH_MS * 20 / 100},
    {NOTE_E4, EIGHTH_MS * 80 / 100},
    {0, 0}
};

static const buzzer_note_t *s_melodies[] = {
    [MELODY_GOOD] = s_melody_good,
    [MELODY_BAD]  = s_melody_bad,
    [MELODY_SAD]  = s_melody_sad,
};

static QueueHandle_t s_buzzer_queue = nullptr;

static void buzzer_tone(uint32_t freq_hz, uint32_t duration_ms) {
    ledcWriteTone(BUZZER_GPIO, freq_hz);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    ledcWrite(BUZZER_GPIO, 0);
}

static void buzzer_task(void *arg) {
    buzzer_melody_t melody;
    while (true) {
        if (xQueueReceive(s_buzzer_queue, &melody, portMAX_DELAY) != pdTRUE)
            continue;

        // Drain queue to play only the latest request
        buzzer_melody_t latest = melody;
        while (xQueueReceive(s_buzzer_queue, &latest, 0) == pdTRUE) {}

        const buzzer_note_t *notes = s_melodies[latest];
        for (int i = 0; notes[i].duration_ms > 0; i++) {
            if (notes[i].freq_hz > 0) {
                buzzer_tone(notes[i].freq_hz, notes[i].duration_ms);
            } else {
                vTaskDelay(pdMS_TO_TICKS(notes[i].duration_ms));
            }
        }
    }
}

void buzzer_init() {
    ledcAttach(BUZZER_GPIO, 1000, 10);
    s_buzzer_queue = xQueueCreate(4, sizeof(buzzer_melody_t));
    xTaskCreate(buzzer_task, "buzzer", 2048, nullptr, 2, nullptr);
    ESP_LOGI(TAG, "Buzzer initialized on GPIO%d", BUZZER_GPIO);
}

void buzzer_beep_good() {
    buzzer_melody_t m = MELODY_GOOD;
    xQueueSend(s_buzzer_queue, &m, 0);
}

void buzzer_beep_bad() {
    buzzer_melody_t m = MELODY_BAD;
    xQueueSend(s_buzzer_queue, &m, 0);
}

void buzzer_beep_sad() {
    buzzer_melody_t m = MELODY_SAD;
    xQueueSend(s_buzzer_queue, &m, 0);
}
