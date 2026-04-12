#include "card_lookup.h"
#include "card_db.h"

#include <cstring>
#include "esp_log.h"

static const char *TAG = "card_lookup";

void card_lookup_init() {
    ESP_LOGI(TAG, "Card DB loaded: %d entries", CARD_DB_COUNT);
}

lookup_result_t card_lookup(uint32_t card_id) {
    lookup_result_t result = {};
    for (int i = 0; i < CARD_DB_COUNT; i++) {
        if (card_db[i].card_id == card_id) {
            result.found = true;
            strncpy(result.name, card_db[i].name, LOOKUP_NAME_MAX - 1);
            result.name[LOOKUP_NAME_MAX - 1] = '\0';
            ESP_LOGI(TAG, "Card 0x%08lX -> %s", (unsigned long)card_id, result.name);
            return result;
        }
    }
    ESP_LOGW(TAG, "Card 0x%08lX not found", (unsigned long)card_id);
    return result;
}
