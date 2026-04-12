#pragma once

#include <cstdint>

// Lookup result
#define LOOKUP_NAME_MAX 64

typedef struct {
    bool found;
    char name[LOOKUP_NAME_MAX];
} lookup_result_t;

// Initialize the card lookup subsystem.
void card_lookup_init();

// Look up a card ID and return the associated name.
// Currently uses compiled-in CSV data.
// Will eventually POST to an HTTP API endpoint.
lookup_result_t card_lookup(uint32_t card_id);
