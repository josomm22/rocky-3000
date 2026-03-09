#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * display_manager — screen brightness + sleep timeout
 *
 * Call disp_mgr_init() once after LVGL_Init().
 * Call disp_mgr_intercept_touch() from the touchpad read callback.
 */

/* Initialise: loads NVS settings, applies brightness, starts poll timer. */
void disp_mgr_init(void);

/* Brightness: 10–100 percent. Changes applied and persisted immediately. */
uint8_t disp_mgr_get_brightness(void);
void disp_mgr_set_brightness(uint8_t pct);

/* Timeout: minutes of inactivity before sleeping. 0 = never sleep. */
uint8_t disp_mgr_get_timeout_min(void);
void disp_mgr_set_timeout_min(uint8_t min);

/* True while the backlight is off (sleep mode). */
bool disp_mgr_is_sleeping(void);

/*
 * Call this from the touchpad read callback when a physical press is detected.
 * Returns true if the screen was asleep — the caller should report
 * LV_INDEV_STATE_RELEASED to consume the wake touch without triggering UI.
 */
bool disp_mgr_intercept_touch(void);
