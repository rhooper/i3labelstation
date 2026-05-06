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

// Copy the current IP address into `buf` as a dotted-quad string. If WiFi
// isn't connected, copies "no link". Always NUL-terminated.
void wifi_get_ip(char *buf, unsigned len);
