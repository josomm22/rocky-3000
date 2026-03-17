#pragma once

/* Load NVS-persisted presets into RAM.  Call once from app_main before
 * screen_main_load().  Falls back to {18 g, 21 g} defaults if no NVS data. */
void screen_main_preset_init(void);

/* Loads (or reloads) the main screen.
 * Safe to call from any LVGL event callback. */
void screen_main_load(void);

/* Preset mutation — each call persists to NVS and reloads the main screen
 * state on the next screen_main_load(). */
void screen_main_add_preset(float w);
void screen_main_edit_preset(int idx, float w);
void screen_main_delete_preset(int idx);
