#pragma once

#include <cstdint>

// Lookup result
#define LOOKUP_NAME_MAX 64

typedef struct {
    bool found;
    char name[LOOKUP_NAME_MAX];
} lookup_result_t;

// Initialize the card lookup subsystem (empty DB until refresh).
void card_lookup_init();

// Fetch member list from HelloClub API and replace the in-memory DB.
// Call after WiFi is connected. Returns true on success.
bool card_lookup_refresh();

// Returns number of cards in the database.
int card_lookup_count();

// Look up a card ID and return the associated name.
lookup_result_t card_lookup(uint32_t card_id);
