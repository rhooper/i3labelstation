#pragma once

#include <stdbool.h>

// Initialize WiFi STA mode and connect. Also starts SNTP time sync.
void wifi_init();

// Returns true if WiFi is connected (has IP).
bool wifi_is_connected();

// Returns true if WiFi has ever successfully connected.
bool wifi_ever_connected();

// Returns true if SNTP has synced time at least once.
bool sntp_is_synced();
