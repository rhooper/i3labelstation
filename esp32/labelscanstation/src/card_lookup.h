#pragma once

#include <cstdint>

// Lookup result
#define LOOKUP_NAME_MAX  64
#define LOOKUP_EMAIL_MAX 96
#define LOOKUP_PHONE_MAX 24

typedef struct {
    bool found;
    char name[LOOKUP_NAME_MAX];
    char email[LOOKUP_EMAIL_MAX];   // empty string if not present
    char phone[LOOKUP_PHONE_MAX];   // HelloClub `mobile`, empty if not present
} lookup_result_t;

// Initialize the card lookup subsystem (empty DB until refresh).
void card_lookup_init();

// Fetch member list from HelloClub API and replace the in-memory DB.
// Call after WiFi is connected. Returns true on success.
bool card_lookup_refresh();

// Start background task that refreshes the DB at 10am and 10pm daily.
void card_lookup_start_refresh_task();

// Returns number of cards in the database.
int card_lookup_count();

// Returns true if the DB hasn't been refreshed in over 16 hours.
bool card_lookup_is_stale();

// Look up a card ID and return the associated name.
lookup_result_t card_lookup(uint32_t card_id);
