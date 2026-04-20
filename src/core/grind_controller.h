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
#include <stdint.h>

typedef enum {
    GRIND_IDLE = 0,  /* waiting for user to start                         */
    GRIND_TARING,    /* tare requested; 1 s settle before SSR on          */
    GRIND_RUNNING,   /* SSR on, weight rising                             */
    GRIND_SETTLING,  /* SSR off, waiting for scale to stabilise           */
    GRIND_PULSING,   /* firing a short correction pulse                   */
    GRIND_DONE,      /* final weight recorded                             */
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

/* Pre-stop offset (auto-tuned after each shot). Persisted to NVS. */
float grind_ctrl_get_offset(void);
void  grind_ctrl_set_offset(float g);

/* Calibration factor applied to raw HX711 counts → grams. */
float grind_ctrl_get_cal_factor(void);
void  grind_ctrl_set_cal_factor(float f);  /* clamps to [0.00001, 10.0] */

/*
 * Current scale reading in grams (always live in real mode via 80 Hz task;
 * returns 0.0 when idle in demo mode — useful for calibration step 1).
 */
float grind_ctrl_get_live_weight(void);

/* Re-zero the scale (safe to call from LVGL context; tare runs in hx711_task). */
void grind_ctrl_tare(void);

/* True when compiled with GRIND_DEMO_MODE=1 (no real hardware). */
bool grind_ctrl_is_demo(void);

/* Live flow rate in g/s (rolling window from hx711_task; 0 in demo mode
 * until a grind starts). Useful for diagnostics and the settings screen. */
float grind_ctrl_get_flow_rate(void);

/* Motor latency: total delay from SSR de-energise to burrs stopping (ms).
 * Used to predict coast distance. Auto-tuned from observed coast each shot
 * and persisted to NVS. Default: 250 ms. Typical range: 100–400 ms. */
float grind_ctrl_get_motor_latency(void);
void  grind_ctrl_set_motor_latency(float ms);  /* clamps to [10, 500] */

/* ── Per-shot diagnostics (valid after GRIND_DONE, before ack_done) ── */

/* Scale reading the instant the SSR was first cut (g). */
float grind_ctrl_get_weight_at_cutoff(void);

/* Settled weight after main coast, before any pulse fires (g).
 * Equals weight_at_cutoff_g when no coast occurred. */
float grind_ctrl_get_weight_before_pulses(void);

/* Flow rate captured at the SSR cutoff instant (g/s). */
float grind_ctrl_get_last_flow_rate(void);

/* Main SSR-on duration from grinder start to first SSR cutoff (ms). */
uint32_t grind_ctrl_get_grind_ms(void);

/* Number of correction pulses fired this shot (0–3). */
int grind_ctrl_get_pulse_count(void);

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

/*
 * HX711 health: true when the sensor has produced a fresh reading within the
 * last HX711_HEALTH_TIMEOUT_MS.  grind_ctrl_start() refuses to begin a grind
 * when this returns false.
 */
bool grind_ctrl_hx711_healthy(void);
