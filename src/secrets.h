/*
 * secrets.h — local WiFi credentials (gitignored, never committed).
 *
 * These are compiled in as fallback defaults used on first boot when NVS
 * has no saved credentials.  After a successful connection the device
 * saves them to NVS automatically.
 *
 * For day-to-day development use:
 *   idf.py app-flash   <-- only reflashes the app, NVS is preserved
 *
 * Only use a full "idf.py flash" when you need to wipe everything.
 */
#pragma once

#define DEFAULT_WIFI_SSID     ""   /* e.g. "MyHomeNetwork" */
#define DEFAULT_WIFI_PASSWORD ""   /* e.g. "hunter2"       */
