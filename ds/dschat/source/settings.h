// Settings screen (bottom screen, menu page MENU_SETTINGS): Wi-Fi network
// (scan list, hidden SSID, open/WEP + key), IP (DHCP or static), relay host
// and port, Test connection, Save / Cancel. While it is open the connection
// is stopped; Save applies the settings (and writes them to the SD card),
// Cancel goes back to the previous ones.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// why: shown as the reason the screen opened (NULL when opened from the menu).
void settings_open(const char *why);
void settings_input(uint32_t down, uint32_t rep, uint32_t held, uint32_t up);
void settings_tick(uint32_t now);
// full: the bottom screen showed something else before.
void settings_draw(bool full);
void settings_on_test(int stage, bool ok, const char *msg);
