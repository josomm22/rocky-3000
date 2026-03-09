#pragma once

/*
 * GBWUI — Grind Controller
 *
 * Controls the grind cycle: tare → run SSR → watch weight → stop → auto-tune.
 *
 * Compile-time flag:
 *   GRIND_DEMO_MODE 1  (default) — no real HX711/SSR required; weight is
 *                                   simulated by a linear ramp with tiny noise.
 *   GRIND_DEMO_MODE 0            — expects real HX711 task + xQueueWeight.
 */

#include <stdbool.h>

typedef enum {
    GRIND_IDLE = 0,  /* waiting for user to start       */
    GRIND_RUNNING,   /* SSR on, weight rising            */
    GRIND_DONE,      /* SSR off, final weight recorded   */
} grind_state_t;

/*
 * Call once after LVGL_Init().
 * Sets SSR GPIO safe-low (real mode). Creates internal LVGL poll timer.
 */
void grind_ctrl_init(void);

/* Start a grind cycle. No-op if already RUNNING. */
void grind_ctrl_start(float target_g);

/* Manual stop (STOP button pressed). */
void grind_ctrl_stop(void);

/* ── State queries (safe from any LVGL timer/event callback) ── */

grind_state_t grind_ctrl_get_state(void);
float         grind_ctrl_get_weight(void);  /* live weight, grams            */
float         grind_ctrl_get_result(void);  /* final weight; valid when DONE */

/* Pre-stop offset (auto-tuned after each shot). Persisted externally. */
float grind_ctrl_get_offset(void);
void  grind_ctrl_set_offset(float g);

/* Calibration factor applied to raw HX711 counts → grams. */
float grind_ctrl_get_cal_factor(void);
void  grind_ctrl_set_cal_factor(float f);  /* clamps to [0.1, 10.0] */

/*
 * Current scale reading in grams (always live in real mode via 80 Hz task;
 * returns 0.0 when idle in demo mode — useful for calibration step 1).
 */
float grind_ctrl_get_live_weight(void);

/* True when compiled with GRIND_DEMO_MODE=1 (no real hardware). */
bool grind_ctrl_is_demo(void);

/*
 * Acknowledge DONE → return to IDLE.
 * The main screen calls this after showing the result for ~2 s.
 */
void grind_ctrl_ack_done(void);

/*
 * Purge: energise the SSR for PURGE_DURATION_MS then cut it automatically.
 * No-op if a grind or purge is already in progress.
 */
void grind_ctrl_purge(void);
bool grind_ctrl_is_purging(void);
