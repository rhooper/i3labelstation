#include "wifi_manager.h"
#include "secrets.h"

#include <atomic>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static int s_retry_count = 0;
static std::atomic<bool> s_connected{false};
static std::atomic<bool> s_ever_connected{false};
static std::atomic<bool> s_sntp_synced{false};
static bool s_sntp_initialized = false;
#define MAX_RETRY 10

// One-shot timer for retry after max retries exhausted
static esp_timer_handle_t s_retry_timer = nullptr;

static void retry_timer_cb(void *arg) {
    ESP_LOGI(TAG, "Retry timer fired, reconnecting...");
    s_retry_count = 0;
    esp_wifi_connect();
}

static void sntp_init_();

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected.store(false);
        if (s_retry_count < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGI(TAG, "Retrying connection (%d/%d)", s_retry_count, MAX_RETRY);
        } else {
            ESP_LOGW(TAG, "Max retries reached, will retry in 30s");
            esp_timer_start_once(s_retry_timer, 30 * 1000000ULL);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_connected.store(true);
        s_ever_connected.store(true);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        // Start SNTP on first IP acquisition
        if (!s_sntp_initialized) {
            sntp_init_();
        }
    }
}

static void sntp_sync_cb(struct timeval *tv) {
    ESP_LOGI(TAG, "SNTP time synced");
    s_sntp_synced.store(true);
}

// Pick a random second between midnight and 4:59:59 AM (0–17999)
static int pick_nightly_second() {
    return esp_random() % (5 * 3600);
}

// Background task: resync NTP once per night at a random time 0:00–4:59 AM
static void sntp_nightly_task(void *arg) {
    while (true) {
        // Pick tonight's sync target
        int target_sec = pick_nightly_second();
        ESP_LOGI(TAG, "Next NTP sync scheduled at %02d:%02d:%02d local",
                 target_sec / 3600, (target_sec % 3600) / 60, target_sec % 60);

        // Sleep until that time, checking once per minute
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(60000));
            time_t now;
            time(&now);
            struct tm ti;
            localtime_r(&now, &ti);
            int now_sec = ti.tm_hour * 3600 + ti.tm_min * 60 + ti.tm_sec;
            // Trigger if within 60s of target (checked every minute)
            if (now_sec >= target_sec && now_sec < target_sec + 60) {
                break;
            }
        }

        ESP_LOGI(TAG, "Nightly NTP resync...");
        esp_sntp_restart();

        // Wait until next day (sleep at least 20h to avoid re-triggering today)
        vTaskDelay(pdMS_TO_TICKS(20UL * 3600 * 1000));
    }
}

static void sntp_init_() {
    if (s_sntp_initialized) return;
    s_sntp_initialized = true;

    ESP_LOGI(TAG, "Initializing SNTP...");
    // America/Detroit (Eastern Time with automatic DST)
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();

    // Start nightly resync task
    xTaskCreate(sntp_nightly_task, "sntp_nightly", 4096, nullptr, 1, nullptr);
}

void wifi_init() {
    // Initialize NVS (required for WiFi)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Create retry timer (must be done before esp_wifi_start triggers events)
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = retry_timer_cb;
    timer_args.name = "wifi_retry";
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_retry_timer));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &event_handler, nullptr, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &event_handler, nullptr, &instance_got_ip));

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = (WIFI_PASS[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA started, connecting to %s...", WIFI_SSID);

    // Wait for connection (with timeout) — SNTP starts from GOT_IP handler
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (s_connected.load()) {
        ESP_LOGI(TAG, "WiFi connected");
    } else {
        ESP_LOGW(TAG, "WiFi connection timeout, will keep retrying in background");
    }
}

bool wifi_is_connected() {
    return s_connected.load();
}

bool wifi_ever_connected() {
    return s_ever_connected.load();
}

bool sntp_is_synced() {
    return s_sntp_synced.load();
}
