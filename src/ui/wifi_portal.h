#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Soft-AP captive portal.
 *
 * Call wifi_portal_start() to bring up the "GBWUI-Setup" access point
 * and an HTTP server on 192.168.4.1.  The portal serves a single-page
 * web app that lists nearby networks and accepts a password.
 *
 * After the user submits credentials, wifi_portal_has_result() returns
 * true and wifi_portal_get_result() retrieves them (safe to call from
 * the LVGL main loop).
 *
 * Call wifi_portal_stop() to tear everything down (automatically clears
 * the pending result).
 */

void wifi_portal_start(void);
void wifi_portal_stop(void);

bool wifi_portal_has_result(void);
void wifi_portal_get_result(char *ssid_out, size_t ssid_len,
                            char *pass_out, size_t pass_len);
