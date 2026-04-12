#include "card_lookup.h"
#include "secrets.h"

#include <cstring>
#include <cstdlib>
#include <cctype>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "card_lookup";

#define API_URL "https://api-v2.helloclub.com/profiles"
#define API_FIELDS "firstName,lastName,customFields"

// Dynamic card database held in heap
typedef struct {
    uint32_t card_id;
    char name[LOOKUP_NAME_MAX];
} card_entry_t;

static card_entry_t *s_cards = nullptr;
static int s_card_count = 0;
static int s_card_capacity = 0;

void card_lookup_init() {
    ESP_LOGI(TAG, "Card lookup initialized (empty until API refresh)");
}

int card_lookup_count() {
    return s_card_count;
}

lookup_result_t card_lookup(uint32_t card_id) {
    lookup_result_t result = {};
    for (int i = 0; i < s_card_count; i++) {
        if (s_cards[i].card_id == card_id) {
            result.found = true;
            strncpy(result.name, s_cards[i].name, LOOKUP_NAME_MAX - 1);
            result.name[LOOKUP_NAME_MAX - 1] = '\0';
            ESP_LOGI(TAG, "Card 0x%08lX -> %s", (unsigned long)card_id, result.name);
            return result;
        }
    }
    ESP_LOGW(TAG, "Card 0x%08lX not found", (unsigned long)card_id);
    return result;
}

// --- HTTP fetch + JSON parsing ---

// Dynamic buffer for HTTP response
typedef struct {
    char *data;
    size_t len;
    size_t capacity;
} http_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    http_buf_t *buf = (http_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t needed = buf->len + evt->data_len + 1;
        if (needed > buf->capacity) {
            size_t new_cap = needed * 2;
            char *tmp = (char *)realloc(buf->data, new_cap);
            if (!tmp) {
                ESP_LOGE(TAG, "realloc failed (%d bytes)", (int)new_cap);
                return ESP_FAIL;
            }
            buf->data = tmp;
            buf->capacity = new_cap;
        }
        memcpy(buf->data + buf->len, evt->data, evt->data_len);
        buf->len += evt->data_len;
        buf->data[buf->len] = '\0';
    }
    return ESP_OK;
}

static bool is_numeric(const char *s) {
    if (!s || !*s) return false;
    for (; *s; s++) {
        if (!isdigit((unsigned char)*s)) return false;
    }
    return true;
}

// Add one entry to the growing array. Returns false on alloc failure.
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

// Process one profile from the JSON response
static bool process_profile(cJSON *profile, card_entry_t **cards, int *count, int *capacity) {
    cJSON *first = cJSON_GetObjectItem(profile, "firstName");
    cJSON *last = cJSON_GetObjectItem(profile, "lastName");
    cJSON *cf = cJSON_GetObjectItem(profile, "customFields");
    if (!first || !last || !cf) return true;  // skip, not an error

    const char *firstName = cJSON_GetStringValue(first);
    const char *lastName = cJSON_GetStringValue(last);
    if (!firstName || !lastName) return true;

    cJSON *fob_json = cJSON_GetObjectItem(cf, "fob");
    const char *fob_str = fob_json ? cJSON_GetStringValue(fob_json) : nullptr;
    if (!fob_str || !*fob_str) return true;

    // Build full name
    char full_name[LOOKUP_NAME_MAX];
    snprintf(full_name, sizeof(full_name), "%s %s", firstName, lastName);

    // Handle comma-separated fobs
    char fob_buf[256];
    strncpy(fob_buf, fob_str, sizeof(fob_buf) - 1);
    fob_buf[sizeof(fob_buf) - 1] = '\0';

    char *saveptr;
    char *tok = strtok_r(fob_buf, ",", &saveptr);
    while (tok) {
        // Trim whitespace
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

    do {
        // Build URL with pagination
        char url[256];
        snprintf(url, sizeof(url),
                 "%s?fields=%s&withCurrentMembership=true&offset=%d",
                 API_URL, API_FIELDS, offset);

        http_buf_t buf = {nullptr, 0, 0};

        esp_http_client_config_t config = {};
        config.url = url;
        config.event_handler = http_event_handler;
        config.user_data = &buf;
        config.timeout_ms = 10000;
        config.crt_bundle_attach = esp_crt_bundle_attach;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to init HTTP client");
            success = false;
            break;
        }

        // Set auth header
        char auth_header[128];
        snprintf(auth_header, sizeof(auth_header), "%s", HELLOCLUB_API_KEY);
        esp_http_client_set_header(client, "X-Api-Key", auth_header);
        esp_http_client_set_header(client, "Accept", "application/json");
        esp_http_client_set_header(client, "User-Agent", "MemberLabelPrinter/1.0");

        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
            free(buf.data);
            success = false;
            break;
        }

        if (status != 200) {
            ESP_LOGE(TAG, "HTTP status %d", status);
            free(buf.data);
            success = false;
            break;
        }

        // Parse JSON
        cJSON *json = cJSON_Parse(buf.data);
        free(buf.data);

        if (!json) {
            ESP_LOGE(TAG, "JSON parse error");
            success = false;
            break;
        }

        cJSON *meta = cJSON_GetObjectItem(json, "meta");
        cJSON *profiles = cJSON_GetObjectItem(json, "profiles");
        if (!meta || !profiles) {
            ESP_LOGE(TAG, "Unexpected JSON structure");
            cJSON_Delete(json);
            success = false;
            break;
        }

        cJSON *total_json = cJSON_GetObjectItem(meta, "total");
        cJSON *count_json = cJSON_GetObjectItem(meta, "count");
        if (!total_json || !count_json) {
            ESP_LOGE(TAG, "Missing meta fields");
            cJSON_Delete(json);
            success = false;
            break;
        }

        total = total_json->valueint;
        int page_count = count_json->valueint;
        offset += page_count;
        fetched += page_count;

        ESP_LOGI(TAG, "Fetched %d/%d profiles", fetched, total);

        cJSON *profile;
        cJSON_ArrayForEach(profile, profiles) {
            if (!process_profile(profile, &new_cards, &new_count, &new_capacity)) {
                success = false;
                break;
            }
        }

        cJSON_Delete(json);

        if (!success) break;

    } while (fetched < total);

    if (success && new_count > 0) {
        // Swap in new database
        free(s_cards);
        s_cards = new_cards;
        s_card_count = new_count;
        s_card_capacity = new_capacity;
        ESP_LOGI(TAG, "Loaded %d card entries from API", s_card_count);
        return true;
    }

    // Failure — clean up partial results
    free(new_cards);
    ESP_LOGE(TAG, "API fetch failed, card DB has %d entries", s_card_count);
    return false;
}
