#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Navigate to (or reload) the WiFi screen. */
void screen_wifi_load(void);

/* Returns true if STA has an IP address. */
bool screen_wifi_is_connected(void);

/* Called by wifi_portal after credentials are submitted via the web page. */
void screen_wifi_connect_from_portal(const char *ssid, const char *password);

/* Call once at boot to connect using saved NVS credentials (no-op if none). */
void wifi_autoconnect(void);
