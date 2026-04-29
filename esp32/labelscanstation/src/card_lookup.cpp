#include "card_lookup.h"
#include "extra_cards.h"
#include "secrets.h"

#include <cstring>
#include <cstdlib>
#include <cctype>
#include <ctime>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ESP-IDF cert bundle embedded in firmware for HTTPS validation
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");

static const char *TAG = "card_lookup";

#define API_URL    "https://api-v2.helloclub.com/profiles"
#define API_FIELDS "firstName,lastName,customFields"

typedef struct {
    uint32_t card_id;
    char name[LOOKUP_NAME_MAX];
} card_entry_t;

static card_entry_t *s_cards = nullptr;
static int s_card_count = 0;
static int s_card_capacity = 0;
static time_t s_last_refresh = 0;

static SemaphoreHandle_t s_db_mutex = nullptr;

#define STALE_THRESHOLD_SECS (16 * 3600)

static bool add_entry(card_entry_t **cards, int *count, int *capacity,
                      uint32_t card_id, const char *name);

static void merge_extra_cards() {
    for (int i = 0; i < EXTRA_CARD_COUNT; i++) {
        bool dup = false;
        for (int j = 0; j < s_card_count; j++) {
            if (s_cards[j].card_id == extra_cards[i].card_id) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            add_entry(&s_cards, &s_card_count, &s_card_capacity,
                      extra_cards[i].card_id, extra_cards[i].name);
        }
    }
}

void card_lookup_init() {
    s_db_mutex = xSemaphoreCreateMutex();
    merge_extra_cards();
    ESP_LOGI(TAG, "Card lookup initialized (%d extra entries)", s_card_count);
}

int card_lookup_count() {
    xSemaphoreTake(s_db_mutex, portMAX_DELAY);
    int count = s_card_count;
    xSemaphoreGive(s_db_mutex);
    return count;
}

lookup_result_t card_lookup(uint32_t card_id) {
    lookup_result_t result = {};
    xSemaphoreTake(s_db_mutex, portMAX_DELAY);
    for (int i = 0; i < s_card_count; i++) {
        if (s_cards[i].card_id == card_id) {
            result.found = true;
            strncpy(result.name, s_cards[i].name, LOOKUP_NAME_MAX - 1);
            result.name[LOOKUP_NAME_MAX - 1] = '\0';
            break;
        }
    }
    xSemaphoreGive(s_db_mutex);
    if (result.found)
        ESP_LOGI(TAG, "Card 0x%08lX -> %s", (unsigned long)card_id, result.name);
    else
        ESP_LOGW(TAG, "Card 0x%08lX not found", (unsigned long)card_id);
    return result;
}

static bool is_numeric(const char *s) {
    if (!s || !*s) return false;
    for (; *s; s++) {
        if (!isdigit((unsigned char)*s)) return false;
    }
    return true;
}

static bool add_entry(card_entry_t **cards, int *count, int *capacity,
                      uint32_t card_id, const char *name) {
    if (*count >= *capacity) {
        int new_cap = *capacity == 0 ? 64 : *capacity * 2;
        card_entry_t *tmp = (card_entry_t *)realloc(*cards, new_cap * sizeof(card_entry_t));
        if (!tmp) {
            ESP_LOGE(TAG, "realloc failed for %d entries", new_cap);
            return false;
        }
        *cards = tmp;
        *capacity = new_cap;
    }
    card_entry_t *e = &(*cards)[*count];
    e->card_id = card_id;
    strncpy(e->name, name, LOOKUP_NAME_MAX - 1);
    e->name[LOOKUP_NAME_MAX - 1] = '\0';
    (*count)++;
    return true;
}

static bool process_profile(JsonObject profile, card_entry_t **cards, int *count, int *capacity) {
    const char *firstName = profile["firstName"] | "";
    const char *lastName  = profile["lastName"]  | "";
    if (!firstName[0] && !lastName[0]) return true;

    JsonObject cf = profile["customFields"];
    if (cf.isNull()) return true;

    const char *fob_str = cf["fob"] | "";
    if (!fob_str[0]) return true;

    char full_name[LOOKUP_NAME_MAX];
    snprintf(full_name, sizeof(full_name), "%s %s", firstName, lastName);

    // Handle comma-separated fobs
    char fob_buf[256];
    strncpy(fob_buf, fob_str, sizeof(fob_buf) - 1);
    fob_buf[sizeof(fob_buf) - 1] = '\0';

    char *saveptr;
    char *tok = strtok_r(fob_buf, ",", &saveptr);
    while (tok) {
        while (*tok == ' ') tok++;
        char *end = tok + strlen(tok) - 1;
        while (end > tok && *end == ' ') *end-- = '\0';

        if (is_numeric(tok)) {
            uint32_t card_id = (uint32_t)strtoul(tok, nullptr, 10);
            if (!add_entry(cards, count, capacity, card_id, full_name)) {
                return false;
            }
        } else {
            ESP_LOGW(TAG, "Bad fob for %s: '%s'", full_name, tok);
        }
        tok = strtok_r(nullptr, ",", &saveptr);
    }
    return true;
}

bool card_lookup_refresh() {
    ESP_LOGI(TAG, "Fetching members from HelloClub API...");

    card_entry_t *new_cards = nullptr;
    int new_count = 0;
    int new_capacity = 0;
    int offset = 0;
    int total = 0;
    int fetched = 0;
    bool success = true;

    // Create TLS client with ESP-IDF root CA bundle for HTTPS
    WiFiClientSecure secureClient;
    secureClient.setCACertBundle(x509_crt_bundle_start,
                                x509_crt_bundle_end - x509_crt_bundle_start);

    do {
        char url[256];
        snprintf(url, sizeof(url),
                 "%s?fields=%s&withCurrentMembership=true&offset=%d",
                 API_URL, API_FIELDS, offset);

        HTTPClient http;
        http.begin(secureClient, url);
        http.addHeader("X-Api-Key", HELLOCLUB_API_KEY);
        http.addHeader("Accept", "application/json");
        http.addHeader("User-Agent", "MemberLabelPrinter/2.0");
        http.setTimeout(10000);

        int status = http.GET();
        if (status != 200) {
            ESP_LOGE(TAG, "HTTP status %d for %s", status, url);
            http.end();
            success = false;
            break;
        }

        // Parse streamed JSON with filter to limit RAM usage
        JsonDocument filter;
        filter["meta"]["total"] = true;
        filter["meta"]["count"] = true;
        filter["profiles"][0]["firstName"] = true;
        filter["profiles"][0]["lastName"]  = true;
        filter["profiles"][0]["customFields"]["fob"] = true;

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, http.getStream(),
                                                   DeserializationOption::Filter(filter));
        http.end();

        if (err) {
            ESP_LOGE(TAG, "JSON parse error: %s", err.c_str());
            success = false;
            break;
        }

        int page_total = doc["meta"]["total"] | 0;
        int page_count = doc["meta"]["count"] | 0;
        if (page_total == 0 || page_count == 0) {
            ESP_LOGE(TAG, "Unexpected meta: total=%d count=%d", page_total, page_count);
            success = false;
            break;
        }

        total = page_total;
        offset += page_count;
        fetched += page_count;

        ESP_LOGI(TAG, "Fetched %d/%d profiles", fetched, total);

        for (JsonObject profile : doc["profiles"].as<JsonArray>()) {
            if (!process_profile(profile, &new_cards, &new_count, &new_capacity)) {
                success = false;
                break;
            }
        }

        if (!success) break;

    } while (fetched < total);

    if (success && new_count > 0) {
        xSemaphoreTake(s_db_mutex, portMAX_DELAY);
        free(s_cards);
        s_cards = new_cards;
        s_card_count = new_count;
        s_card_capacity = new_capacity;
        merge_extra_cards();
        time(&s_last_refresh);
        int final_count = s_card_count;
        xSemaphoreGive(s_db_mutex);
        ESP_LOGI(TAG, "Loaded %d card entries (%d from API + extras)", final_count, new_count);
        return true;
    }

    free(new_cards);
    ESP_LOGE(TAG, "API fetch failed, card DB has %d entries", s_card_count);
    return false;
}

bool card_lookup_is_stale() {
    xSemaphoreTake(s_db_mutex, portMAX_DELAY);
    time_t last = s_last_refresh;
    xSemaphoreGive(s_db_mutex);
    if (last == 0) return false;
    time_t now;
    time(&now);
    return (now - last) > STALE_THRESHOLD_SECS;
}

static void card_refresh_task(void *arg) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));

        time_t now;
        time(&now);
        struct tm ti;
        localtime_r(&now, &ti);

        if (ti.tm_year < (2020 - 1900)) continue;

        if ((ti.tm_hour == 10 || ti.tm_hour == 22) && ti.tm_min == 0) {
            ESP_LOGI(TAG, "Scheduled DB refresh at %02d:%02d", ti.tm_hour, ti.tm_min);
            card_lookup_refresh();
            vTaskDelay(pdMS_TO_TICKS(61000));
        }
    }
}

void card_lookup_start_refresh_task() {
    xTaskCreate(card_refresh_task, "card_refresh", 8192, nullptr, 1, nullptr);
    ESP_LOGI(TAG, "Card refresh task started (10:00 / 22:00 daily)");
}
