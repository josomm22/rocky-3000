/*
 * GBWUI — Grind Controller
 *
 * GRIND_DEMO_MODE 1 (default while no HX711 is wired):
 *   Weight is simulated by a linear ramp at DEMO_RAMP_G_PER_SEC, with tiny
 *   deterministic noise.  No GPIO or FreeRTOS task involved.
 *   After the pre-stop threshold is crossed a small fixed overshoot is added
 *   so the auto-tune logic actually has something to converge on.
 *
 * GRIND_DEMO_MODE 0  — real hardware path:
 *   hx711_task (Core 0, priority 5) reads the HX711 at its native 80 Hz rate
 *   (~12.5 ms / sample) and writes each calibrated gram value into
 *   s_latest_weight (volatile float, no queue needed — we only care about the
 *   most recent reading, not every sample).
 *
 *   The LVGL poll timer (UI_POLL_MS = 100 ms, ~10 Hz) wakes up, snapshots
 *   s_latest_weight, checks the stop threshold, and updates the display.
 *   The two rates are intentionally independent:
 *     - 80 Hz  → smooth threshold detection, low latency SSR cut-off
 *     - 10 Hz  → fast enough display refresh, doesn't flood the LVGL task
 *
 *   SSR driven on GPIO_NUM_33 (active HIGH, matches DESIGN.md §3.4).
 */

#include "grind_controller.h"
#include "lvgl.h"
#include <math.h>
#include <stdint.h>

/* ── Build-time config ──────────────────────────────────────── */

#ifndef GRIND_DEMO_MODE
#define GRIND_DEMO_MODE 0
#endif

/* Pre-stop residual bias (on top of dynamic coast prediction).
 * Starts at 0; auto-tune adjusts it to absorb any remaining systematic error. */
#define DEFAULT_OFFSET_G  0.0f
#define AUTOTUNE_FACTOR   0.5f
#define AUTOTUNE_DEADBAND 0.1f   /* ignore deltas smaller than this (g) */
#define OFFSET_MIN_G     -5.0f
#define OFFSET_MAX_G      5.0f

/* Dynamic coast prediction: stop_at = target - coast_g - s_offset
 *   coast_g = (motor_latency_ms / 1000) × flow_rate_g_s × COAST_RATIO
 * Falls back to COAST_FALLBACK_G when flow rate is not yet available. */
#define MOTOR_LATENCY_MS_DEFAULT  250.0f   /* coffee grinder motors coast 200-400ms; tune via settings */
#define COAST_RATIO               1.0f
#define COAST_FALLBACK_G          0.3f   /* used when flow rate == 0 */

/* Pulse refinement: short correction bursts after the main stop */
#define PULSE_MIN_G        0.15f  /* below this shortfall → skip pulse      */
#define PULSE_MAX_ATTEMPTS 3      /* safety cap on correction loops         */
#define PULSE_MIN_MS       30     /* shortest meaningful SSR pulse (ms)     */
#define PULSE_MAX_MS       500    /* safety cap (ms)                        */
#define PULSE_FACTOR       0.8f   /* intentional undershoot per pulse       */

/* UI display refresh via LVGL timer (does NOT affect HX711 sample rate) */
#define UI_POLL_MS  100

/* Declared here so hx711_task (real mode) can read it before the rest of
 * the module state block. */
static volatile float s_cal_factor    = 0.000386f;  /* ~20 g / 176 displayed; tune via calibration screen */
static volatile bool  s_tare_requested = false;

#if GRIND_DEMO_MODE
/* Simulated grind speed — realistic espresso dose in ~6 s */
#define DEMO_RAMP_G_PER_SEC   3.0f
/* Simulated mechanical overshoot after SSR cuts off */
#define DEMO_OVERSHOOT_G      0.18f
/* Peak-to-peak noise amplitude per tick */
#define DEMO_NOISE_AMP        0.03f
#else
/* Real mode ────────────────────────────────────────────────── */
#include "hx711.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define GPIO_SSR          GPIO_NUM_4    /* moved here; GPIO43/44 now used by HX711 */
#define GPIO_HX711_DATA   GPIO_NUM_44  /* UART0 RXD — reconfigured as HX711 DOUT */
#define GPIO_HX711_CLK    GPIO_NUM_43  /* UART0 TXD — reconfigured as HX711 SCK; serial monitor disabled */

#define HX711_POLL_HZ   80              /* module output data rate (Hz)   */
#define HX711_POLL_MS   (1000 / HX711_POLL_HZ)  /* 12 ms between samples */
#define HX711_TASK_STACK  2048
#define HX711_TASK_PRIO   5

/*
 * Shared between hx711_task (writer) and the LVGL poll timer (reader).
 * Single float writes on ESP32 are atomic — no mutex needed.
 */
static volatile float s_latest_weight  = 0.0f;
static volatile float s_flow_rate_g_s  = 0.0f;  /* g/s, updated each block */

/* Block-average this many consecutive HX711 conversions before EMA.
 * 8 samples × 12.5 ms = 100 ms per update — matches the display rate exactly,
 * so every poll_cb call reads a fresh weight (halves measurement staleness vs 16).
 * Reduces white noise by √8 ≈ 2.8× before EMA runs. */
#define HX711_AVG_SAMPLES  8

/* EMA on top of the block average: α=0.5 → one block period (~200 ms)
 * to settle; the 16-sample block average already handles noise. */
#define HX711_EMA_ALPHA  1.0f

/* Reject a block whose trimmed average deviates more than this from the
 * previous reading — catches motor-start EMI spikes without adding latency.
 * A 10 g/s grinder at max can add ~2 g per 200 ms block; 10 g is 5× margin. */
#define SPIKE_REJECT_DELTA_G  10.0f

static void hx711_task(void *arg)
{
    (void)arg;
    hx711_init(GPIO_HX711_DATA, GPIO_HX711_CLK);
    hx711_tare();

    /* Prime the EMA with the first valid reading so it doesn't
     * crawl up from 0.0 on startup. */
    float g;
    while (!hx711_read_grams(s_cal_factor, &g))
        vTaskDelay(1);
    s_latest_weight = g;

    while (1) {
        if (s_tare_requested) {
            hx711_tare();
            s_latest_weight  = 0.0f;
            s_tare_requested = false;
        }

        /* Collect HX711_AVG_SAMPLES readings, blocking on each conversion.
         * Track the single highest sample for a trimmed mean (drop max). */
        float sum   = 0.0f;
        float max_g = -1e9f;
        int   n     = 0;
        for (int i = 0; i < HX711_AVG_SAMPLES; i++) {
            while (!hx711_is_ready())
                vTaskDelay(1);
            if (hx711_read_grams(s_cal_factor, &g)) {
                sum += g;
                if (g > max_g) max_g = g;
                n++;
            }
        }
        if (n > 1) {
            /* Layer 1: trimmed mean — drop the single highest sample to
             * remove within-block outliers caused by EMI bursts. */
            float avg = (sum - max_g) / (float)(n - 1);

            /* Layer 2: inter-block delta gate — reject the entire block if
             * the jump is physically impossible (spike from motor start etc.) */
            if (fabsf(avg - s_latest_weight) < SPIKE_REJECT_DELTA_G) {
                s_latest_weight = HX711_EMA_ALPHA * avg
                                  + (1.0f - HX711_EMA_ALPHA) * s_latest_weight;

                /* Flow rate: weight delta over the fixed block period.
                 * Block period = HX711_AVG_SAMPLES / HX711_POLL_HZ (seconds). */
                static float prev_block_weight = 0.0f;
                float dt_s = (float)HX711_AVG_SAMPLES / (float)HX711_POLL_HZ;
                float rate = (avg - prev_block_weight) / dt_s;
                s_flow_rate_g_s = (rate > 0.0f) ? rate : 0.0f;
                prev_block_weight = avg;
            }
            /* else: spike detected — keep previous weight and flow rate */
        }
    }
}
#endif  /* !GRIND_DEMO_MODE */

/* ── Module state ───────────────────────────────────────────── */

static grind_state_t  s_state      = GRIND_IDLE;
static float          s_target     = 18.0f;
static float          s_offset     = DEFAULT_OFFSET_G;   /* residual bias, auto-tuned */
static float          s_motor_latency_ms = MOTOR_LATENCY_MS_DEFAULT;
static float          s_weight     = 0.0f;
static float          s_result     = 0.0f;
static lv_timer_t    *s_timer      = NULL;

/* Settle countdown: ticks remaining after SSR cutoff (main stop or pulse)
 * before reading the final/inter-pulse weight.  2 × UI_POLL_MS = 200 ms. */
#define SETTLE_TICKS  2
static int            s_settle_ticks  = 0;

/* Pulse refinement */
static int            s_pulse_attempts = 0;
static lv_timer_t    *s_pulse_timer    = NULL;

/* Tare-settle before grind */
#define TARE_SETTLE_MS  1000
static lv_timer_t    *s_tare_timer   = NULL;

/* Purge */
#define PURGE_DURATION_MS  1500
static bool           s_purging      = false;
static lv_timer_t    *s_purge_timer  = NULL;

/* ── Helpers ────────────────────────────────────────────────── */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void run_autotune(void)
{
    float delta = s_result - s_target;
    if (delta < 0.0f) delta = -delta;   /* abs */
    if (delta <= AUTOTUNE_DEADBAND) return;

    delta = s_result - s_target;        /* restore sign */
    s_offset = clampf(s_offset + delta * AUTOTUNE_FACTOR, OFFSET_MIN_G, OFFSET_MAX_G);
}

static void ssr_set(int on)
{
#if GRIND_DEMO_MODE
    (void)on;
#else
    gpio_set_level(GPIO_SSR, on ? 1 : 0);
#endif
}

#if GRIND_DEMO_MODE
/* Simple LCG — deterministic, zero stdlib dependency */
static uint32_t s_rng = 0xDEADBEEFu;
static float lcg_noise(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return ((float)(int16_t)(s_rng >> 16)) / 32768.0f;  /* [-1, +1] */
}
#endif

/* ── Pulse refinement callbacks ─────────────────────────────── */

/* Called when a correction pulse timer expires — SSR off, begin re-settle */
static void pulse_done_cb(lv_timer_t *t)
{
    (void)t;
    s_pulse_timer = NULL;   /* auto-deleted (repeat_count = 1) */
    ssr_set(0);
    s_pulse_attempts++;
    s_settle_ticks = SETTLE_TICKS;
    s_state        = GRIND_SETTLING;
}

/*
 * Called when the settle countdown reaches zero (after main stop or pulse).
 * Decides: fire another correction pulse, or declare GRIND_DONE.
 */
static void settle_complete(void)
{
    float shortfall = s_target - s_weight;
    float flow      = (float)s_flow_rate_g_s;

    if (shortfall > PULSE_MIN_G
        && s_pulse_attempts < PULSE_MAX_ATTEMPTS
        && flow > 0.01f)
    {
        float pulse_ms = (shortfall / flow) * 1000.0f * PULSE_FACTOR;
        pulse_ms = clampf(pulse_ms, (float)PULSE_MIN_MS, (float)PULSE_MAX_MS);

        s_state       = GRIND_PULSING;
        ssr_set(1);
        s_pulse_timer = lv_timer_create(pulse_done_cb, (uint32_t)pulse_ms, NULL);
        lv_timer_set_repeat_count(s_pulse_timer, 1);
    }
    else
    {
        s_result = s_weight;
        run_autotune();
        s_state = GRIND_DONE;
    }
}

/* ── LVGL poll timer (UI_POLL_MS, ~10 Hz) ───────────────────── */
/*
 * In real mode: snapshots s_latest_weight (kept current by the 80 Hz
 * hx711_task).  The stop / settle / pulse decisions run here at ~10 Hz —
 * adequate for espresso doses.
 */
static void poll_cb(lv_timer_t *t)
{
    (void)t;

    /* ── GRIND_SETTLING: wait for scale to stabilise ── */
    if (s_state == GRIND_SETTLING) {
#if !GRIND_DEMO_MODE
        s_weight = (float)s_latest_weight;
#endif
        if (--s_settle_ticks == 0)
            settle_complete();
        return;
    }

    /* ── GRIND_PULSING: SSR on, weight rising again ── */
    if (s_state == GRIND_PULSING) {
#if GRIND_DEMO_MODE
        float inc = DEMO_RAMP_G_PER_SEC * (UI_POLL_MS / 1000.0f);
        s_weight += inc + lcg_noise() * DEMO_NOISE_AMP;
        if (s_weight < 0.0f) s_weight = 0.0f;
#else
        s_weight = (float)s_latest_weight;
#endif
        return;
    }

    if (s_state != GRIND_RUNNING)
        return;

    /* ── GRIND_RUNNING: update weight snapshot ── */
#if GRIND_DEMO_MODE
    float inc   = DEMO_RAMP_G_PER_SEC * (UI_POLL_MS / 1000.0f);
    float noise = lcg_noise() * DEMO_NOISE_AMP;
    s_weight += inc + noise;
    if (s_weight < 0.0f) s_weight = 0.0f;
    s_flow_rate_g_s = DEMO_RAMP_G_PER_SEC;
#else
    s_weight = (float)s_latest_weight;
#endif

    /* Dynamic stop threshold: coast_g = (motor_latency + measurement_latency) × flow × ratio.
     * Measurement latency ≈ half the block period (avg staleness of s_latest_weight).
     * Falls back to COAST_FALLBACK_G until flow rate is established. */
    float flow    = (float)s_flow_rate_g_s;
    float meas_latency_ms = (HX711_AVG_SAMPLES * 1000.0f / HX711_POLL_HZ) * 0.5f;
    float coast_g = (flow > 0.01f)
                    ? ((s_motor_latency_ms + meas_latency_ms) / 1000.0f) * flow * COAST_RATIO
                    : COAST_FALLBACK_G;
    float stop_at = s_target - coast_g - s_offset;

    if (s_weight >= stop_at) {
        ssr_set(0);
        s_pulse_attempts = 0;
        s_settle_ticks   = SETTLE_TICKS;
        s_state          = GRIND_SETTLING;

#if GRIND_DEMO_MODE
        /* Simulate mechanical overshoot so the settled weight is realistic */
        s_weight = stop_at + coast_g + DEMO_OVERSHOOT_G + lcg_noise() * 0.05f;
        if (s_weight < 0.0f) s_weight = 0.0f;
#endif
    }
}

/* ── Tare-settle callback ────────────────────────────────────── */

static void tare_done_cb(lv_timer_t *t)
{
    (void)t;
    s_tare_timer = NULL;   /* auto-deleted (repeat_count = 1) */
    if (s_state != GRIND_TARING)
        return;
    s_state = GRIND_RUNNING;
    ssr_set(1);
}

/* ── Public API ─────────────────────────────────────────────── */

void grind_ctrl_init(void)
{
    s_state  = GRIND_IDLE;
    s_weight = 0.0f;
    s_offset = DEFAULT_OFFSET_G;

#if !GRIND_DEMO_MODE
    /* Detach UART0 from its default pins (GPIO43=TXD, GPIO44=RXD) so the
     * HX711 driver can drive them as plain GPIO.  Serial monitor is
     * unavailable; ESP_LOG output continues via USB Serial/JTAG only. */
    uart_set_pin(UART_NUM_0, GPIO_NUM_NC, GPIO_NUM_NC,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    /* SSR — active HIGH, default OFF */
    gpio_reset_pin(GPIO_SSR);
    gpio_set_direction(GPIO_SSR, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_SSR, 0);

    /* Start the 80 Hz HX711 reader on Core 0 */
    xTaskCreatePinnedToCore(hx711_task, "hx711", HX711_TASK_STACK,
                            NULL, HX711_TASK_PRIO, NULL, 0);
#endif

    s_timer = lv_timer_create(poll_cb, UI_POLL_MS, NULL);
    lv_timer_pause(s_timer);
}

void grind_ctrl_start(float target_g)
{
    if (s_state == GRIND_RUNNING || s_state == GRIND_TARING)
        return;

    s_target         = target_g;
    s_weight         = 0.0f;
    s_result         = 0.0f;
    s_settle_ticks   = 0;
    s_pulse_attempts = 0;
    s_state          = GRIND_TARING;

    grind_ctrl_tare();

    s_tare_timer = lv_timer_create(tare_done_cb, TARE_SETTLE_MS, NULL);
    lv_timer_set_repeat_count(s_tare_timer, 1);
    lv_timer_resume(s_timer);
}

void grind_ctrl_stop(void)
{
    if (s_state == GRIND_TARING) {
        if (s_tare_timer) {
            lv_timer_delete(s_tare_timer);
            s_tare_timer = NULL;
        }
        s_state = GRIND_IDLE;
        lv_timer_pause(s_timer);
        return;
    }

    /* Cancel any in-flight pulse timer */
    if (s_pulse_timer) {
        lv_timer_delete(s_pulse_timer);
        s_pulse_timer = NULL;
    }

    /* Turn off SSR if it was on */
    if (s_state == GRIND_RUNNING || s_state == GRIND_PULSING)
        ssr_set(0);

    if (s_state == GRIND_RUNNING
        || s_state == GRIND_SETTLING
        || s_state == GRIND_PULSING)
    {
        s_result = s_weight;
        run_autotune();
        s_state = GRIND_DONE;
    }
}

/* ── Purge ──────────────────────────────────────────────────── */

static void purge_stop_cb(lv_timer_t *t)
{
    (void)t;
    s_purge_timer = NULL;   /* auto-deleted by LVGL (repeat_count = 1) */
    ssr_set(0);
    s_purging = false;
}

void grind_ctrl_purge(void)
{
    if (s_state == GRIND_RUNNING || s_purging)
        return;

    s_purging = true;
    ssr_set(1);

    s_purge_timer = lv_timer_create(purge_stop_cb, PURGE_DURATION_MS, NULL);
    lv_timer_set_repeat_count(s_purge_timer, 1);
}

bool grind_ctrl_is_purging(void) { return s_purging; }

grind_state_t grind_ctrl_get_state(void)  { return s_state;  }
float         grind_ctrl_get_weight(void) { return s_weight; }
float         grind_ctrl_get_result(void) { return s_result; }
float         grind_ctrl_get_offset(void) { return s_offset; }

void grind_ctrl_set_offset(float g)
{
    s_offset = clampf(g, OFFSET_MIN_G, OFFSET_MAX_G);
}

float grind_ctrl_get_cal_factor(void) { return s_cal_factor; }

void grind_ctrl_set_cal_factor(float f)
{
    s_cal_factor = clampf(f, 0.00001f, 10.0f);
    /* TODO (real mode): persist to NVS, apply to hx711_task */
}

float grind_ctrl_get_live_weight(void)
{
#if GRIND_DEMO_MODE
    return s_weight;   /* 0.0 when idle; rises during a grind */
#else
    return (float)s_latest_weight;
#endif
}

void grind_ctrl_tare(void)
{
#if !GRIND_DEMO_MODE
    s_tare_requested = true;
#endif
}

bool grind_ctrl_is_demo(void)
{
#if GRIND_DEMO_MODE
    return true;
#else
    return false;
#endif
}

float grind_ctrl_get_flow_rate(void)
{
    return (float)s_flow_rate_g_s;
}

float grind_ctrl_get_motor_latency(void)
{
    return s_motor_latency_ms;
}

void grind_ctrl_set_motor_latency(float ms)
{
    s_motor_latency_ms = clampf(ms, 10.0f, 500.0f);
}

void grind_ctrl_ack_done(void)
{
    if (s_state != GRIND_DONE)
        return;

    s_state          = GRIND_IDLE;
    s_weight         = 0.0f;
    s_pulse_attempts = 0;
    lv_timer_pause(s_timer);
}
