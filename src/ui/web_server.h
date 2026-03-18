#pragma once

#include <stdbool.h>

/*
 * web_server — persistent HTTP server (port 80).
 *
 * Routes:
 *   GET  /history        → grind history page
 *   GET  /api/history    → history data as JSON
 *   GET  /ota            → firmware upload page
 *   POST /update         → OTA firmware upload
 *
 * Call web_server_start() once at boot (after LVGL init).
 * The server runs forever; clients can connect as soon as WiFi is up.
 *
 * OTA progress state is shared with screen_ota via the volatile vars below.
 * screen_ota calls web_server_reset_ota_state() on each screen entry.
 */

/* Written by the httpd worker thread, read by the screen_ota poll timer. */
extern volatile int  web_server_ota_pct;    /* 0–100                   */
extern volatile bool web_server_ota_done;   /* flash write + swap done  */
extern volatile bool web_server_ota_error;  /* any OTA error            */

void            web_server_start(void);
void            web_server_reset_ota_state(void);

/* Returns the running httpd handle so other modules (e.g. wifi_portal)
 * can register/unregister URI handlers on the same server instance.
 * Returns NULL if the server has not started yet. */
#include "esp_http_server.h"
httpd_handle_t  web_server_get_handle(void);
