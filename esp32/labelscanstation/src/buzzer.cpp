#include "buzzer.h"
#include "app_config.h"

#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "buzzer";

#define BUZZER_LEDC_TIMER   LEDC_TIMER_1
#define BUZZER_LEDC_CHANNEL LEDC_CHANNEL_1
#define BUZZER_LEDC_MODE    LEDC_LOW_SPEED_MODE

void buzzer_init() {
    ledc_timer_config_t timer_conf = {};
    timer_conf.speed_mode = BUZZER_LEDC_MODE;
    timer_conf.timer_num = BUZZER_LEDC_TIMER;
    timer_conf.duty_resolution = LEDC_TIMER_8_BIT;
    timer_conf.freq_hz = 1000;
    timer_conf.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t ch_conf = {};
    ch_conf.speed_mode = BUZZER_LEDC_MODE;
    ch_conf.channel = BUZZER_LEDC_CHANNEL;
    ch_conf.timer_sel = BUZZER_LEDC_TIMER;
    ch_conf.intr_type = LEDC_INTR_DISABLE;
    ch_conf.gpio_num = BUZZER_GPIO;
    ch_conf.duty = 0;
    ch_conf.hpoint = 0;
    ledc_channel_config(&ch_conf);

    ESP_LOGI(TAG, "Buzzer initialized on GPIO%d", BUZZER_GPIO);
}

// BPM 120: quarter = 500ms, eighth = 250ms
#define BPM 120
#define QUARTER_MS (60000 / BPM)  // 500ms
#define EIGHTH_MS  (QUARTER_MS / 2)  // 250ms

// Note frequencies (Hz)
#define NOTE_C4   262
#define NOTE_Ab4  415
#define NOTE_C7  2093

static void buzzer_tone(uint32_t freq_hz, uint32_t duration_ms) {
    ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, freq_hz);
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 128);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);

    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

// Known card: C7 two quarter notes, 80% gate
void buzzer_beep_good() {
    uint32_t gate = QUARTER_MS * 80 / 100;
    uint32_t rest = QUARTER_MS - gate;
    buzzer_tone(NOTE_C7, gate);
    vTaskDelay(pdMS_TO_TICKS(rest));
    buzzer_tone(NOTE_C7, gate);
    vTaskDelay(pdMS_TO_TICKS(rest));
}

// Unknown card: C4 eighth note, Ab4 quarter note
void buzzer_beep_bad() {
    buzzer_tone(NOTE_C4, EIGHTH_MS);
    buzzer_tone(NOTE_Ab4, QUARTER_MS);
}

// Sad beep: descending C5 → G4 → E4, eighth notes, 80% gate
#define NOTE_C5  523
#define NOTE_G4  392
#define NOTE_E4  330

void buzzer_beep_sad() {
    uint32_t gate = EIGHTH_MS * 80 / 100;
    uint32_t rest = EIGHTH_MS - gate;
    buzzer_tone(NOTE_C5, gate);
    vTaskDelay(pdMS_TO_TICKS(rest));
    buzzer_tone(NOTE_G4, gate);
    vTaskDelay(pdMS_TO_TICKS(rest));
    buzzer_tone(NOTE_E4, gate);
}
