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
#include "nvs_flash.h"
#include "nvs.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

/* ── NVS persistence ────────────────────────────────────────────
 * Tuned values (offset, motor latency, calibration factor) outlive
 * reboots so the user doesn't lose autotune convergence or wizard data. */
#define GRIND_NVS_NS    "grind_cfg"
#define GRIND_KEY_OFF   "offset_g"
#define GRIND_KEY_MLAT  "mlat_ms"
#define GRIND_KEY_CAL   "cal_factor"

static void save_settings(void);
static void load_settings(void);

/* ── Build-time config ──────────────────────────────────────── */

#ifndef GRIND_DEMO_MODE
#define GRIND_DEMO_MODE 0
#endif

/* Pre-stop residual bias (on top of dynamic coast prediction).
 * Starts at 0; auto-tune adjusts it to absorb any remaining systematic error.
 * Negative offset is allowed so persistent undershoot (e.g. when motor-latency
 * autotune has over-predicted coast) can still be corrected. */
#define DEFAULT_OFFSET_G 0.0f
#define AUTOTUNE_FACTOR 0.3f    /* gentler step — noisy shots don't swing offset */
#define AUTOTUNE_DEADBAND 0.1f  /* ignore deltas smaller than this (g) */
#define OFFSET_MIN_G -2.0f
#define OFFSET_MAX_G 5.0f

/* Autotune validity gates — skip adjustment when the shot was anomalous
 * (too short, or flow never established).  Prevents a single sensor glitch
 * or bean-starvation shot from corrupting offset / motor-latency. */
#define AUTOTUNE_MIN_GRIND_MS 500
#define AUTOTUNE_MIN_FLOW_G_S 0.5f

/* Motor-latency autotune: EMA factor applied to each shot's observed coast.
 * 0.3 converges in ~5 shots and resists single-shot noise. */
#define MOTOR_LATENCY_ALPHA 0.3f

/* Dynamic coast prediction: stop_at = target - coast_g - s_offset
 *   coast_g = (motor_latency_ms / 1000) × flow_rate_g_s × COAST_RATIO
 * Falls back to COAST_FALLBACK_G when flow rate is not yet available. */
#define MOTOR_LATENCY_MS_DEFAULT 250.0f /* coffee grinder motors coast 200-400ms; tune via settings */
#define COAST_RATIO 1.0f
#define COAST_FALLBACK_G 0.3f /* used when flow rate == 0 */

/* Pulse refinement: short correction bursts after the main stop */
#define PULSE_MIN_G 0.05f    /* below this shortfall → skip pulse      */
#define PULSE_MAX_ATTEMPTS 3 /* safety cap on correction loops         */
#define PULSE_MIN_MS 30      /* shortest meaningful SSR pulse (ms)     */
#define PULSE_MAX_MS 500     /* safety cap (ms)                        */
#define PULSE_FACTOR 0.8f    /* intentional undershoot per pulse       */

/* UI display refresh via LVGL timer (does NOT affect HX711 sample rate) */
#define UI_POLL_MS 100

/* Declared here so hx711_task (real mode) can read it before the rest of
 * the module state block. */
static volatile float s_cal_factor = 0.000386f; /* ~20 g / 176 displayed; tune via calibration screen */
static volatile bool s_tare_requested = false;

#if GRIND_DEMO_MODE
/* Simulated grind speed — realistic espresso dose in ~6 s */
#define DEMO_RAMP_G_PER_SEC 3.0f
/* Simulated mechanical overshoot after SSR cuts off */
#define DEMO_OVERSHOOT_G 0.18f
/* Peak-to-peak noise amplitude per tick */
#define DEMO_NOISE_AMP 0.03f
#else
/* Real mode ────────────────────────────────────────────────── */
#include "hx711.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define GPIO_SSR GPIO_NUM_4         /* moved here; GPIO43/44 now used by HX711 */
#define GPIO_HX711_DATA GPIO_NUM_44 /* UART0 RXD — reconfigured as HX711 DOUT */
#define GPIO_HX711_CLK GPIO_NUM_43  /* UART0 TXD — reconfigured as HX711 SCK; serial monitor disabled */

#define HX711_POLL_HZ 80                     /* module output data rate (Hz)   */
#define HX711_POLL_MS (1000 / HX711_POLL_HZ) /* 12 ms between samples */
#define HX711_TASK_STACK 2048
#define HX711_TASK_PRIO 5

/*
 * Shared between hx711_task (writer) and the LVGL poll timer (reader).
 * Single float writes on ESP32 are atomic — no mutex needed.
 */
static volatile float s_latest_weight = 0.0f;
static volatile float s_flow_rate_g_s = 0.0f; /* g/s, updated each block */

/* Settling detection: circular buffer of block-averaged weights.
 * 5 blocks × ~200 ms = ~1 s window; std-dev < SETTLE_STD_DEV_G ⇒ settled. */
#define SETTLE_BUF_SIZE 5
#define SETTLE_STD_DEV_G 0.05f
static volatile float s_settle_buf[SETTLE_BUF_SIZE];
static volatile int s_settle_buf_idx = 0;
static volatile bool s_settle_buf_full = false;

/* Block-average this many consecutive HX711 conversions before EMA.
 * 8 samples × 12.5 ms = 100 ms per update — matches the display rate exactly,
 * so every poll_cb call reads a fresh weight (halves measurement staleness vs 16).
 * Reduces white noise by √8 ≈ 2.8× before EMA runs. */
#define HX711_AVG_SAMPLES 8

/* HX711 health monitoring */
#define HX711_READY_TIMEOUT_MS 500   /* max wait for a single DOUT ready       */
#define HX711_HEALTH_TIMEOUT_MS 2000 /* stale after 2 s without a fresh block  */
#define HX711_MAX_RETRIES 3          /* power-cycle attempts before giving up  */

/* EMA on top of the block average: α=0.5 → one block period (~200 ms)
 * to settle; the 16-sample block average already handles noise. */
#define HX711_EMA_ALPHA 1.0f

/* Reject a block whose trimmed average deviates more than this from the
 * previous reading — catches motor-start EMI spikes without adding latency.
 * A 10 g/s grinder at max can add ~2 g per 200 ms block; 10 g is 5× margin. */
#define SPIKE_REJECT_DELTA_G 10.0f

/* Health tracking: timestamp of last successful block read.
 * Written by hx711_task, read by LVGL poll timer & grind_ctrl_start. */
static volatile uint32_t s_hx711_last_ok_tick = 0;
static volatile bool s_hx711_healthy = false;

/* Tare completion flag: set by hx711_task after tare finishes,
 * checked by tare_done_cb before transitioning to GRIND_RUNNING. */
static volatile bool s_tare_complete = false;

static void hx711_task(void *arg)
{
    (void)arg;
    hx711_init(GPIO_HX711_DATA, GPIO_HX711_CLK);

    /* Initial tare with recovery — keep trying until the sensor responds. */
    while (!hx711_tare()) {
        hx711_power_cycle();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* Prime the EMA with the first valid reading so it doesn't
     * crawl up from 0.0 on startup. */
    float g;
    if (hx711_wait_ready(HX711_READY_TIMEOUT_MS) && hx711_read_grams(s_cal_factor, &g))
        s_latest_weight = g;

    s_hx711_last_ok_tick = xTaskGetTickCount();
    s_hx711_healthy = true;

    while (1)
    {
        if (s_tare_requested)
        {
            s_tare_complete = false;
            if (hx711_tare()) {
                s_latest_weight = 0.0f;
                s_tare_complete = true;
            }
            /* If tare failed, s_tare_complete stays false — tare_done_cb
             * will not transition to GRIND_RUNNING. */
            s_tare_requested = false;
        }

        /* Collect HX711_AVG_SAMPLES readings, blocking on each conversion
         * with a timeout.  If any sample times out, attempt recovery. */
        float sum = 0.0f;
        float max_g = -1e9f;
        int n = 0;
        bool block_ok = true;
        for (int i = 0; i < HX711_AVG_SAMPLES; i++)
        {
            if (!hx711_wait_ready(HX711_READY_TIMEOUT_MS))
            {
                block_ok = false;
                break;
            }
            if (hx711_read_grams(s_cal_factor, &g))
            {
                sum += g;
                if (g > max_g)
                    max_g = g;
                n++;
            }
        }

        if (!block_ok)
        {
            /* HX711 is not responding — attempt power-cycle recovery. */
            s_hx711_healthy = false;
            for (int retry = 0; retry < HX711_MAX_RETRIES; retry++)
            {
                hx711_power_cycle();
                vTaskDelay(pdMS_TO_TICKS(200));
                if (hx711_wait_ready(HX711_READY_TIMEOUT_MS))
                {
                    /* Sensor is back — re-tare to get a clean baseline. */
                    if (hx711_tare()) {
                        s_latest_weight = 0.0f;
                        s_hx711_last_ok_tick = xTaskGetTickCount();
                        s_hx711_healthy = true;
                    }
                    break;
                }
            }
            continue; /* restart the main loop */
        }

        if (n > 1)
        {
            /* Layer 1: trimmed mean — drop the single highest sample to
             * remove within-block outliers caused by EMI bursts. */
            float avg = (sum - max_g) / (float)(n - 1);

            /* Layer 2: inter-block delta gate — reject the entire block if
             * the jump is physically impossible (spike from motor start etc.) */
            if (fabsf(avg - s_latest_weight) < SPIKE_REJECT_DELTA_G)
            {
                s_latest_weight = HX711_EMA_ALPHA * avg + (1.0f - HX711_EMA_ALPHA) * s_latest_weight;

                /* Flow rate: weight delta over the fixed block period.
                 * Block period = HX711_AVG_SAMPLES / HX711_POLL_HZ (seconds). */
                static float prev_block_weight = 0.0f;
                float dt_s = (float)HX711_AVG_SAMPLES / (float)HX711_POLL_HZ;
                float rate = (avg - prev_block_weight) / dt_s;
                s_flow_rate_g_s = (rate > 0.0f) ? rate : 0.0f;
                prev_block_weight = avg;

                /* Settling detection buffer: push latest block average. */
                s_settle_buf[s_settle_buf_idx] = s_latest_weight;
                s_settle_buf_idx++;
                if (s_settle_buf_idx >= SETTLE_BUF_SIZE)
                {
                    s_settle_buf_idx = 0;
                    s_settle_buf_full = true;
                }
            }
            /* else: spike detected — keep previous weight and flow rate */

            s_hx711_last_ok_tick = xTaskGetTickCount();
            s_hx711_healthy = true;
        }
    }
}
#endif /* !GRIND_DEMO_MODE */

#if !GRIND_DEMO_MODE
/*
 * Returns true when the last SETTLE_BUF_SIZE block-averages have a
 * standard deviation below SETTLE_STD_DEV_G — i.e. the scale is stable.
 * Called only from the LVGL poll timer (Core 1); reads volatile floats
 * written by hx711_task (Core 0).  Occasional torn reads are acceptable
 * for a statistical check — the timeout backstop prevents hanging.
 */
static bool is_settled(void)
{
    if (!s_settle_buf_full)
        return false;
    float sum = 0.0f;
    for (int i = 0; i < SETTLE_BUF_SIZE; i++)
        sum += (float)s_settle_buf[i];
    float mean = sum / (float)SETTLE_BUF_SIZE;
    float var = 0.0f;
    for (int i = 0; i < SETTLE_BUF_SIZE; i++)
    {
        float d = (float)s_settle_buf[i] - mean;
        var += d * d;
    }
    return sqrtf(var / (float)SETTLE_BUF_SIZE) < SETTLE_STD_DEV_G;
}
#endif

/* ── Module state ───────────────────────────────────────────── */

static grind_state_t s_state = GRIND_IDLE;
static float s_target = 18.0f;
static float s_offset = DEFAULT_OFFSET_G; /* residual bias, auto-tuned */
static float s_motor_latency_ms = MOTOR_LATENCY_MS_DEFAULT;
static float s_weight = 0.0f;
static float s_result = 0.0f;
static lv_timer_t *s_timer = NULL;

/* Settle tick counter (counts up after SSR cutoff).
 * Real mode: advance until is_settled() or SETTLE_TIMEOUT_TICKS.
 * Demo mode: use SETTLE_FIXED_TICKS as a simple fixed delay. */
#define SETTLE_TIMEOUT_TICKS 15    /* 15 × 100 ms = 1.5 s max wait (real)              */
#define SETTLE_FIXED_TICKS 2       /* 2 × 100 ms = 200 ms fixed (demo)                 */
#define PULSE_SETTLE_MIN_TICKS 12  /* 12 × 100 ms = 1.2 s minimum post-pulse settle    */
static int s_settle_ticks = 0;

/* Flow rate captured at the main-stop instant (real mode).
 * s_flow_rate_g_s drops to 0 once the grinder coasts — this preserves
 * the value needed for pulse duration calculation in settle_complete(). */
static float s_pulse_flow_rate = 0.0f;

/* Per-shot diagnostics captured during the grind cycle */
static float    s_weight_at_cutoff     = 0.0f; /* weight when SSR first cut       */
static float    s_weight_before_pulses = 0.0f; /* settled weight before any pulse  */
static uint32_t s_grind_start_ms       = 0;    /* lv_tick_get() when SSR turned on */
static uint32_t s_grind_ms             = 0;    /* SSR-on duration (ms)             */

/* Pulse refinement */
static int s_pulse_attempts = 0;
static lv_timer_t *s_pulse_timer = NULL;

/* Tare-settle before grind */
#define TARE_SETTLE_MS 1000
static lv_timer_t *s_tare_timer = NULL;

/* Purge */
#define PURGE_DURATION_MS 1500
static bool s_purging = false;
static lv_timer_t *s_purge_timer = NULL;

/* ── Helpers ────────────────────────────────────────────────── */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ── NVS persistence helpers ────────────────────────────────────
 * Floats are stored as their u32 bit pattern via memcpy — matches the
 * existing convention in screen_main.c (preset weights). */

static void load_settings(void)
{
    nvs_handle_t h;
    if (nvs_open(GRIND_NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return;

    uint32_t bits;
    float f;
    if (nvs_get_u32(h, GRIND_KEY_OFF, &bits) == ESP_OK) {
        memcpy(&f, &bits, sizeof(f));
        s_offset = clampf(f, OFFSET_MIN_G, OFFSET_MAX_G);
    }
    if (nvs_get_u32(h, GRIND_KEY_MLAT, &bits) == ESP_OK) {
        memcpy(&f, &bits, sizeof(f));
        s_motor_latency_ms = clampf(f, 10.0f, 500.0f);
    }
    if (nvs_get_u32(h, GRIND_KEY_CAL, &bits) == ESP_OK) {
        memcpy(&f, &bits, sizeof(f));
        s_cal_factor = clampf(f, 0.00001f, 10.0f);
    }
    nvs_close(h);
}

static void save_settings(void)
{
    nvs_handle_t h;
    if (nvs_open(GRIND_NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return;

    uint32_t bits;
    float f;

    f = s_offset;             memcpy(&bits, &f, sizeof(bits));
    nvs_set_u32(h, GRIND_KEY_OFF,  bits);
    f = s_motor_latency_ms;   memcpy(&bits, &f, sizeof(bits));
    nvs_set_u32(h, GRIND_KEY_MLAT, bits);
    f = s_cal_factor;         memcpy(&bits, &f, sizeof(bits));
    nvs_set_u32(h, GRIND_KEY_CAL,  bits);

    nvs_commit(h);
    nvs_close(h);
}

static void run_autotune(void)
{
    /* Gate: skip anomalous shots (manual stop mid-grind, sensor glitch,
     * bean starvation) so a single bad reading doesn't corrupt tuning. */
    if (s_grind_ms < AUTOTUNE_MIN_GRIND_MS)
        return;
    if (s_pulse_flow_rate < AUTOTUNE_MIN_FLOW_G_S)
        return;

#if !GRIND_DEMO_MODE
    /* Motor-latency autotune: derive the real total latency from the
     * observed coast, subtract the known measurement latency, and EMA
     * the motor component.  This makes the coast prediction self-correcting
     * so the user doesn't have to tune motor_latency_ms by hand. */
    float actual_coast_g = s_weight_before_pulses - s_weight_at_cutoff;
    if (actual_coast_g > 0.02f) {
        float meas_latency_ms   = (HX711_AVG_SAMPLES * 1000.0f / HX711_POLL_HZ) * 0.5f;
        float total_latency_ms  = (actual_coast_g / s_pulse_flow_rate) * 1000.0f;
        float observed_motor_ms = total_latency_ms - meas_latency_ms;
        s_motor_latency_ms = clampf(
            (1.0f - MOTOR_LATENCY_ALPHA) * s_motor_latency_ms +
            MOTOR_LATENCY_ALPHA * observed_motor_ms,
            10.0f, 500.0f);
    }
#endif

    /* Pre-stop offset autotune */
    float delta = s_result - s_target;
    if (delta < 0.0f)
        delta = -delta; /* abs */
    if (delta > AUTOTUNE_DEADBAND) {
        delta = s_result - s_target; /* restore sign */
        s_offset = clampf(s_offset + delta * AUTOTUNE_FACTOR, OFFSET_MIN_G, OFFSET_MAX_G);
    }

    /* Persist tuned values so they survive reboots. */
    save_settings();
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
    return ((float)(int16_t)(s_rng >> 16)) / 32768.0f; /* [-1, +1] */
}
#endif

/* ── Pulse refinement callbacks ─────────────────────────────── */

/* Called when a correction pulse timer expires — SSR off, begin re-settle */
static void pulse_done_cb(lv_timer_t *t)
{
    (void)t;
    s_pulse_timer = NULL; /* auto-deleted (repeat_count = 1) */
    ssr_set(0);
    s_pulse_attempts++;
    s_settle_ticks = 0; /* count-up; will wait for is_settled() or timeout */
#if !GRIND_DEMO_MODE
    /* Invalidate the settle buffer so is_settled() must wait for a full
     * fresh window of post-pulse readings before it can fire again.
     * Without this, stale in-pulse data causes the next pulse to start
     * before the weight has had time to stabilise. */
    s_settle_buf_full = false;
    s_settle_buf_idx = 0;
#endif
    s_state = GRIND_SETTLING;
}

/*
 * Called when the settle countdown reaches zero (after main stop or pulse).
 * Decides: fire another correction pulse, or declare GRIND_DONE.
 */
static void settle_complete(void)
{
    /* Capture the settled weight before any pulse fires.
     * On pulse 1, 2, 3 s_pulse_attempts > 0 — value stays frozen so it
     * always represents post-main-coast weight, not post-pulse weight. */
    if (s_pulse_attempts == 0)
        s_weight_before_pulses = s_weight;

    float shortfall = s_target - s_weight;
    /* Use flow rate captured at the main-stop instant.  s_flow_rate_g_s
     * drops to 0 while the scale coasts, so we must use the saved value. */
    float flow = s_pulse_flow_rate;

    if (shortfall > PULSE_MIN_G && s_pulse_attempts < PULSE_MAX_ATTEMPTS && flow > 0.01f)
    {
        /* pulse = motor startup time + productive grinding time × undershoot factor */
        float productive_ms = (shortfall / flow) * 1000.0f * PULSE_FACTOR;
        float pulse_ms = s_motor_latency_ms + productive_ms;
        pulse_ms = clampf(pulse_ms, (float)PULSE_MIN_MS, (float)PULSE_MAX_MS);

        s_state = GRIND_PULSING;
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
    if (s_state == GRIND_SETTLING)
    {
#if GRIND_DEMO_MODE
        if (++s_settle_ticks >= SETTLE_FIXED_TICKS)
            settle_complete();
#else
        s_weight = (float)s_latest_weight;
        if (s_weight < 0.0f) s_weight = 0.0f;
        s_settle_ticks++;
        /* After a correction pulse, wait longer before re-evaluating — the
         * grinder coasts for ~250 ms and the scale needs time to stabilise.
         * After the main stop, 2 ticks (200 ms) is enough to prime the
         * settle buffer; the std-dev gate does the rest. */
        int min_ticks = (s_pulse_attempts > 0) ? PULSE_SETTLE_MIN_TICKS : 2;
        if (s_settle_ticks >= min_ticks &&
            (is_settled() || s_settle_ticks >= SETTLE_TIMEOUT_TICKS))
            settle_complete();
#endif
        return;
    }

    /* ── GRIND_PULSING: SSR on, weight rising again ── */
    if (s_state == GRIND_PULSING)
    {
#if GRIND_DEMO_MODE
        float inc = DEMO_RAMP_G_PER_SEC * (UI_POLL_MS / 1000.0f);
        s_weight += inc + lcg_noise() * DEMO_NOISE_AMP;
        if (s_weight < 0.0f)
            s_weight = 0.0f;
#else
        s_weight = (float)s_latest_weight;
        if (s_weight < 0.0f) s_weight = 0.0f;
#endif
        return;
    }

    if (s_state != GRIND_RUNNING)
        return;

    /* ── GRIND_RUNNING: update weight snapshot ── */
#if GRIND_DEMO_MODE
    float inc = DEMO_RAMP_G_PER_SEC * (UI_POLL_MS / 1000.0f);
    float noise = lcg_noise() * DEMO_NOISE_AMP;
    s_weight += inc + noise;
    if (s_weight < 0.0f)
        s_weight = 0.0f;
    s_flow_rate_g_s = DEMO_RAMP_G_PER_SEC;
#else
    s_weight = (float)s_latest_weight;
    /* Motor startup vibration / SSR-switching EMI can drive the HX711 briefly
     * negative right after the SSR fires.  Coffee weight is physically >= 0 g;
     * clamping here prevents the grinder from running extra to "pay off" a
     * phantom negative-gram debt before reaching the stop threshold. */
    if (s_weight < 0.0f) s_weight = 0.0f;
#endif

    /* Dynamic stop threshold: coast_g = (motor_latency + measurement_latency) × flow × ratio.
     * Measurement latency ≈ half the block period (avg staleness of s_latest_weight).
     * Falls back to COAST_FALLBACK_G until flow rate is established. */
    float flow = (float)s_flow_rate_g_s;
    float meas_latency_ms = (HX711_AVG_SAMPLES * 1000.0f / HX711_POLL_HZ) * 0.5f;
    float coast_g = (flow > 0.01f)
                        ? ((s_motor_latency_ms + meas_latency_ms) / 1000.0f) * flow * COAST_RATIO
                        : COAST_FALLBACK_G;
    float stop_at = s_target - coast_g - s_offset;

    if (s_weight >= stop_at)
    {
#if !GRIND_DEMO_MODE
        /* Capture flow rate NOW — it drops to 0 once the grinder coasts.
         * This value drives pulse duration in settle_complete(). */
        s_pulse_flow_rate = (float)s_flow_rate_g_s;
#endif
        s_weight_at_cutoff = s_weight;
        s_grind_ms = lv_tick_get() - s_grind_start_ms;
        ssr_set(0);
        s_pulse_attempts = 0;
        s_settle_ticks = 0;
        s_state = GRIND_SETTLING;

#if GRIND_DEMO_MODE
        /* Simulate mechanical overshoot so the settled weight is realistic */
        s_weight = stop_at + coast_g + DEMO_OVERSHOOT_G + lcg_noise() * 0.05f;
        s_pulse_flow_rate = DEMO_RAMP_G_PER_SEC;
        if (s_weight < 0.0f)
            s_weight = 0.0f;
#endif
    }
}

/* ── Tare-settle callback ────────────────────────────────────── */

/* Maximum time to wait for tare to complete before aborting.
 * The 1 s tare settle timer fires first; if tare hasn't finished by
 * TARE_TIMEOUT_MS total, tare_done_cb gives up. */
#define TARE_TIMEOUT_MS 3000
static uint32_t s_tare_start_ms = 0;

static void tare_done_cb(lv_timer_t *t)
{
    (void)t;
    s_tare_timer = NULL; /* auto-deleted (repeat_count = 1) */
    if (s_state != GRIND_TARING)
        return;

    if (!s_tare_complete)
    {
        /* Tare hasn't finished yet — reschedule unless we've exceeded
         * the overall timeout. */
        uint32_t elapsed = lv_tick_get() - s_tare_start_ms;
        if (elapsed < TARE_TIMEOUT_MS)
        {
            s_tare_timer = lv_timer_create(tare_done_cb, 100, NULL);
            lv_timer_set_repeat_count(s_tare_timer, 1);
            return;
        }
        /* Timed out — abort the grind. */
        s_state = GRIND_IDLE;
        lv_timer_pause(s_timer);
        return;
    }

    s_state = GRIND_RUNNING;
    ssr_set(1);
    s_grind_start_ms = lv_tick_get();
}

/* ── Public API ─────────────────────────────────────────────── */

void grind_ctrl_init(void)
{
    s_state = GRIND_IDLE;
    s_weight = 0.0f;
    s_offset = DEFAULT_OFFSET_G;

    /* Restore persisted offset / motor latency / cal factor before the
     * HX711 task starts, so the first reading uses the calibrated factor. */
    load_settings();

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

#if !GRIND_DEMO_MODE
    /* Refuse to start if the HX711 hasn't produced a fresh reading recently. */
    if (!s_hx711_healthy)
        return;
#endif

    s_target = target_g;
    s_weight = 0.0f;
    s_result = 0.0f;
    s_settle_ticks = 0;
    s_pulse_attempts = 0;
    s_pulse_flow_rate = 0.0f;
    s_weight_at_cutoff     = 0.0f;
    s_weight_before_pulses = 0.0f;
    s_grind_start_ms       = 0;
    s_grind_ms             = 0;
    s_tare_complete        = false;
    s_tare_start_ms        = lv_tick_get();
    s_state = GRIND_TARING;

    grind_ctrl_tare();

    s_tare_timer = lv_timer_create(tare_done_cb, TARE_SETTLE_MS, NULL);
    lv_timer_set_repeat_count(s_tare_timer, 1);
    lv_timer_resume(s_timer);
}

void grind_ctrl_stop(void)
{
    if (s_state == GRIND_TARING)
    {
        if (s_tare_timer)
        {
            lv_timer_delete(s_tare_timer);
            s_tare_timer = NULL;
        }
        s_state = GRIND_IDLE;
        lv_timer_pause(s_timer);
        return;
    }

    /* Cancel any in-flight pulse timer */
    if (s_pulse_timer)
    {
        lv_timer_delete(s_pulse_timer);
        s_pulse_timer = NULL;
    }

    /* Turn off SSR if it was on */
    if (s_state == GRIND_RUNNING || s_state == GRIND_PULSING)
        ssr_set(0);

    if (s_state == GRIND_RUNNING || s_state == GRIND_SETTLING || s_state == GRIND_PULSING)
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
    s_purge_timer = NULL; /* auto-deleted by LVGL (repeat_count = 1) */
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

grind_state_t grind_ctrl_get_state(void) { return s_state; }
float grind_ctrl_get_weight(void) { return s_weight; }
float grind_ctrl_get_result(void) { return s_result; }
float grind_ctrl_get_offset(void) { return s_offset; }

void grind_ctrl_set_offset(float g)
{
    s_offset = clampf(g, OFFSET_MIN_G, OFFSET_MAX_G);
    save_settings();
}

float grind_ctrl_get_cal_factor(void) { return s_cal_factor; }

void grind_ctrl_set_cal_factor(float f)
{
    s_cal_factor = clampf(f, 0.00001f, 10.0f);
    save_settings();
}

float grind_ctrl_get_live_weight(void)
{
#if GRIND_DEMO_MODE
    return s_weight; /* 0.0 when idle; rises during a grind */
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
    save_settings();
}

float    grind_ctrl_get_weight_at_cutoff(void)     { return s_weight_at_cutoff; }
float    grind_ctrl_get_weight_before_pulses(void) { return s_weight_before_pulses; }
float    grind_ctrl_get_last_flow_rate(void)       { return s_pulse_flow_rate; }
uint32_t grind_ctrl_get_grind_ms(void)             { return s_grind_ms; }
int      grind_ctrl_get_pulse_count(void)          { return s_pulse_attempts; }

void grind_ctrl_ack_done(void)
{
    if (s_state != GRIND_DONE)
        return;

    s_state = GRIND_IDLE;
    s_weight = 0.0f;
    s_pulse_attempts = 0;
    s_weight_at_cutoff     = 0.0f;
    s_weight_before_pulses = 0.0f;
    lv_timer_pause(s_timer);
}

bool grind_ctrl_hx711_healthy(void)
{
#if GRIND_DEMO_MODE
    return true;
#else
    return s_hx711_healthy;
#endif
}
