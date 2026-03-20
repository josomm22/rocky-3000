#pragma once

/*
 * ota_checker — background GitHub release check + cloud OTA
 *
 * Call ota_checker_start() once after boot (after WiFi autoconnect is
 * initiated).  The checker waits up to 60 s for a DHCP lease, then
 * fetches the GitHub Releases API to compare the latest tag against the
 * running firmware version (APP_VERSION_STRING).
 *
 * If a newer release is found, state transitions to OTA_CHECK_AVAILABLE
 * and ota_checker_get_version() / the binary URL are stored.
 *
 * Call ota_checker_apply() to start the download-and-flash sequence.
 * Progress (0–100) is exposed via ota_checker_get_progress().
 * On completion the device reboots automatically.
 *
 * All state transitions are safe to read from the LVGL task (they are
 * written once by a background FreeRTOS task and are volatile).
 */

typedef enum {
    OTA_CHECK_IDLE,
    OTA_CHECK_CHECKING,
    OTA_CHECK_AVAILABLE,
    OTA_CHECK_NO_UPDATE,
    OTA_CHECK_DOWNLOADING,
    OTA_CHECK_DONE,
    OTA_CHECK_ERROR,
} ota_check_state_t;

/* Start the background version-check task (idempotent). */
void              ota_checker_start(void);

/* Current state — safe to call from LVGL task. */
ota_check_state_t ota_checker_get_state(void);

/* Latest release tag, e.g. "v1.2.3".  Valid when state == AVAILABLE. */
const char       *ota_checker_get_version(void);

/* Download progress 0–100.  Valid when state == DOWNLOADING. */
int               ota_checker_get_progress(void);

/* True when the available release has a .bin asset that can be flashed. */
bool              ota_checker_has_binary(void);

/* Begin the download-and-flash sequence.  Only valid from AVAILABLE + has_binary. */
void              ota_checker_apply(void);

/* Force a fresh version check.  No-op while a check or download is in
 * progress; resets and restarts from any other (terminal) state. */
void              ota_checker_recheck(void);
