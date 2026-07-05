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
 *   s_latest_weight, drives the state machine, and updates the display.
 *   The SSR cut-off itself does NOT wait for this timer: hx711_task compares
 *   every fresh sample against the precomputed stop threshold and de-energises
 *   the SSR within CUTOFF_CONFIRM_SAMPLES samples (~37 ms) of the crossing.
 *   The LVGL timer only observes the result (s_cutoff_pending), so display
 *   rendering stalls can no longer add jitter to the cut-off instant.
 *
 *   SSR driven on GPIO_NUM_4 (active HIGH).
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
#define GRIND_KEY_AT_EN "at_enabled"

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

/* Hard ceiling on per-shot offset adjustment.  Keeps the offset stable once
 * the user has dialled in a sweet spot — a single weird shot can nudge it
 * by at most this amount, never sending it to the clamp floor in one go. */
#define AUTOTUNE_MAX_STEP_G 0.15f

/* Reject the shot from autotune when |result - target| exceeds this fraction
 * of the target (with an absolute floor).  Catches sensor-spike-induced
 * early cutoffs (result far below target) and runaway pulses (result far
 * above) without rejecting normal espresso-dose noise. */
#define AUTOTUNE_MAX_DELTA_FRAC 0.25f
#define AUTOTUNE_MAX_DELTA_FLOOR_G 1.5f

/* Plausibility ceiling on flow rate.  A real espresso grinder peaks around
 * 5–10 g/s; anything above this is a sensor glitch.  Used to gate autotune
 * AND clamped on the live value so coast_g can't blow up. */
#define MAX_PLAUSIBLE_FLOW_G_S 15.0f

/* Hard ceiling on the predicted coast distance applied during the grind.
 * coast_g = motor_latency × flow × COAST_RATIO can balloon if flow rate
 * spikes past the spike gate; this cap stops a corrupt flow value from
 * making stop_at deeply negative and cutting the shot at <1 s. */
#define MAX_COAST_G 3.0f

/* Motor-latency autotune: EMA factor applied to each shot's observed coast.
 * 0.3 converges in ~5 shots and resists single-shot noise. */
#define MOTOR_LATENCY_ALPHA 0.3f

/* Dynamic coast prediction: stop_at = target - coast_g - s_offset
 *   coast_g = (motor_latency_ms / 1000) × flow_rate_g_s × COAST_RATIO
 * Falls back to COAST_FALLBACK_G when flow rate is not yet available.
 * motor_latency_ms is auto-tuned from the observed coast each shot, so it
 * absorbs every fixed delay in the loop (motor spin-down, cutoff
 * confirmation window) without those being modelled separately. */
#define MOTOR_LATENCY_MS_DEFAULT 250.0f /* coffee grinder motors coast 200-400ms; tune via settings */
#define COAST_RATIO 1.0f
#define COAST_FALLBACK_G 0.3f /* used when flow rate == 0 */

/* Flow-rate EMA: smooths the per-block weight delta before it feeds coast
 * prediction, pulse sizing and motor-latency autotune.  Raw block deltas
 * amplify scale noise (a few hundredths of a gram per block becomes
 * ±0.2–0.4 g/s); α=0.3 roughly halves that while still tracking a real
 * flow change within ~3 blocks (300 ms). */
#define FLOW_EMA_ALPHA 0.3f

/* SSR cutoff confirmation: this many consecutive 80 Hz samples must sit at
 * or above the stop threshold before hx711_task cuts the SSR.  Rejects
 * single-sample EMI spikes while keeping the reaction latency deterministic
 * (~37 ms) instead of the 0–100 ms+ jitter of the LVGL poll timer. */
#define CUTOFF_CONFIRM_SAMPLES 3

/* Assumed flow when a pulse must fire but flow was never measured (tiny
 * targets can push stop_at ≤ 0, cutting the SSR before any coffee flows).
 * A typical espresso mid-grind rate; PULSE_FACTOR still undershoots, and
 * pulse_done_cb adopts the real observed rate after the first pulse. */
#define PULSE_FALLBACK_FLOW_G_S 3.0f

/* Runaway guard: hard ceiling on SSR-on time for a single grind.  Covers
 * sensor death mid-grind (weight stops rising) and empty-hopper runs. */
#define MAX_GRIND_MS 60000

/* Pulse refinement: short correction bursts after the main stop */
#define PULSE_MIN_G 0.2f     /* shortfall ≤ this → skip pulse; close enough */
#define PULSE_MAX_ATTEMPTS 3 /* safety cap on correction loops         */
#define PULSE_MIN_MS 30      /* shortest meaningful SSR pulse (ms)     */
#define PULSE_MAX_MS 500     /* safety cap (ms)                        */
#define PULSE_FACTOR 0.8f    /* intentional undershoot per pulse       */

/* UI display refresh via LVGL timer (does NOT affect HX711 sample rate) */
#define UI_POLL_MS 100

/* Declared here so hx711_task (real mode) can read them before the rest of
 * the module state block.  target/offset/motor-latency are written by the
 * LVGL thread (Core 1) before the cutoff is armed and read per-block by
 * hx711_task (Core 0); single-float accesses are atomic on ESP32. */
static volatile float s_cal_factor = 0.000386f; /* ~20 g / 176 displayed; tune via calibration screen */
static volatile bool s_tare_requested = false;
static volatile float s_target = 18.0f;
static volatile float s_offset = DEFAULT_OFFSET_G; /* residual bias, auto-tuned */
static volatile float s_motor_latency_ms = MOTOR_LATENCY_MS_DEFAULT;

static void ssr_set(int on);

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

/* EMA on top of the block average.  α=1.0 disables it — the 8-sample block
 * average already handles white noise; kept as a tuning hook. */
#define HX711_EMA_ALPHA 1.0f

/* Reject a block whose trimmed average deviates more than this from the
 * previous reading — catches motor-start EMI spikes without adding latency.
 * A 10 g/s grinder at max adds ~1 g per 100 ms block; 10 g is 10× margin. */
#define SPIKE_REJECT_DELTA_G 10.0f

/* Health tracking: timestamp of last successful block read.
 * Written by hx711_task, read by LVGL poll timer & grind_ctrl_start. */
static volatile uint32_t s_hx711_last_ok_tick = 0;
static volatile bool s_hx711_healthy = false;

/* Tare completion flag: set by hx711_task after tare finishes,
 * checked by tare_done_cb before transitioning to GRIND_RUNNING. */
static volatile bool s_tare_complete = false;

/* Calibration mode: when true, hx711_task bypasses the inter-block delta
 * gate so placing a heavy calibration weight produces an immediate reading.
 * Set via grind_ctrl_set_calibration_mode() from the calibration screen. */
static volatile bool s_calibration_mode = false;

/* Previous block average for flow-rate calculation; file-scope so the tare
 * branch can reset it (pre-tare history is meaningless against a new zero). */
static volatile float s_prev_block_weight = 0.0f;

/* Fast SSR cutoff — armed by tare_done_cb (Core 1), fired by hx711_task
 * (Core 0).  The task compares every fresh sample against s_stop_at_g and
 * cuts the SSR itself; poll_cb only picks up s_cutoff_pending to advance
 * the state machine, so its latency no longer affects the dose. */
static volatile bool  s_cutoff_armed   = false;
static volatile bool  s_cutoff_pending = false;
static volatile float s_cutoff_weight  = 0.0f; /* sample that confirmed cutoff */
static volatile float s_cutoff_flow    = 0.0f; /* smoothed flow at cutoff      */
static volatile float s_stop_at_g      = 0.0f; /* refreshed each block         */

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

            /* Flow and settle state reference the old zero — reset them. */
            s_prev_block_weight = 0.0f;
            s_flow_rate_g_s = 0.0f;
            s_settle_buf_full = false;
            s_settle_buf_idx = 0;
        }

        /* Collect HX711_AVG_SAMPLES readings, blocking on each conversion
         * with a timeout.  If any sample times out, attempt recovery. */
        float sum = 0.0f;
        float max_g = -1e9f;
        float min_g = 1e9f;
        int n = 0;
        bool block_ok = true;
        static int cutoff_confirm = 0;
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
                if (g < min_g)
                    min_g = g;
                n++;

                /* Fast cutoff: compare every fresh sample against the stop
                 * threshold.  CUTOFF_CONFIRM_SAMPLES consecutive samples
                 * must agree so one EMI spike can't end the shot early. */
                if (s_cutoff_armed)
                {
                    if (g >= s_stop_at_g)
                    {
                        if (++cutoff_confirm >= CUTOFF_CONFIRM_SAMPLES)
                        {
                            ssr_set(0);
                            s_cutoff_weight  = g;
                            s_cutoff_flow    = s_flow_rate_g_s;
                            s_cutoff_armed   = false;
                            s_cutoff_pending = true;
                            cutoff_confirm   = 0;
                        }
                    }
                    else
                        cutoff_confirm = 0;
                }
                else
                    cutoff_confirm = 0;
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

        if (n > 2)
        {
            /* Layer 1: symmetric trimmed mean — drop the highest AND lowest
             * sample to remove within-block outliers from EMI bursts, which
             * spike in both directions (motor start can pull readings
             * negative just as easily as positive). */
            float avg = (sum - max_g - min_g) / (float)(n - 2);

            /* Layer 2: inter-block delta gate — reject the entire block if
             * the jump is physically impossible (spike from motor start etc.)
             * Bypassed during calibration: the user intentionally places a
             * large weight on the scale and the motor is off, so there is no
             * EMI to filter and the display must follow the real reading. */
            if (s_calibration_mode
                || fabsf(avg - s_latest_weight) < SPIKE_REJECT_DELTA_G)
            {
                s_latest_weight = HX711_EMA_ALPHA * avg + (1.0f - HX711_EMA_ALPHA) * s_latest_weight;

                /* Flow rate: weight delta over the fixed block period.
                 * Block period = HX711_AVG_SAMPLES / HX711_POLL_HZ (seconds).
                 * Hard-capped at MAX_PLAUSIBLE_FLOW_G_S so a single block that
                 * sneaks past the spike gate (e.g. a 9 g jump just under the
                 * 10 g threshold) can't blow up coast_g and cut the shot at
                 * <1 s.  Real espresso grinders peak around 5–10 g/s.
                 * The EMA then smooths the clamped rate: raw block deltas
                 * jitter by ±0.2–0.4 g/s, which used to wobble stop_at every
                 * block and bias the cutoff early. */
                float dt_s = (float)HX711_AVG_SAMPLES / (float)HX711_POLL_HZ;
                float rate = (avg - s_prev_block_weight) / dt_s;
                if (rate < 0.0f) rate = 0.0f;
                if (rate > MAX_PLAUSIBLE_FLOW_G_S) rate = MAX_PLAUSIBLE_FLOW_G_S;
                s_flow_rate_g_s = FLOW_EMA_ALPHA * rate
                                + (1.0f - FLOW_EMA_ALPHA) * (float)s_flow_rate_g_s;
                s_prev_block_weight = avg;

                /* Settling detection buffer: push latest block average. */
                s_settle_buf[s_settle_buf_idx] = s_latest_weight;
                s_settle_buf_idx++;
                if (s_settle_buf_idx >= SETTLE_BUF_SIZE)
                {
                    s_settle_buf_idx = 0;
                    s_settle_buf_full = true;
                }

                /* Refresh the stop threshold for the fast cutoff path.
                 * coast_g = motor_latency × flow; motor latency is autotuned
                 * from the observed coast each shot, so it also absorbs the
                 * cutoff confirmation window — nothing else to model here. */
                if (s_cutoff_armed)
                {
                    float flow = (float)s_flow_rate_g_s;
                    float coast_g = (flow > 0.01f)
                                        ? (s_motor_latency_ms / 1000.0f) * flow * COAST_RATIO
                                        : COAST_FALLBACK_G;
                    if (coast_g > MAX_COAST_G) coast_g = MAX_COAST_G;
                    s_stop_at_g = s_target - coast_g - s_offset;
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
static bool  s_autotune_enabled = true;   /* user-toggle; persisted to NVS */
static bool  s_manual_stop = false;       /* set by grind_ctrl_stop, cleared each start */
static float s_weight = 0.0f;
static float s_result = 0.0f;
static lv_timer_t *s_timer = NULL;

/* Settle tick counter (counts up after SSR cutoff).
 * Real mode: advance until is_settled() or SETTLE_TIMEOUT_TICKS.
 * Demo mode: use SETTLE_FIXED_TICKS as a simple fixed delay. */
#define SETTLE_TIMEOUT_TICKS 30    /* 30 × 100 ms = 3 s max wait (real)                */
#define SETTLE_FIXED_TICKS 2       /* 2 × 100 ms = 200 ms fixed (demo)                 */
#define MAIN_SETTLE_MIN_TICKS 8    /* 8 × 100 ms = 0.8 s minimum post-main-stop settle */
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
    uint8_t en;
    if (nvs_get_u8(h, GRIND_KEY_AT_EN, &en) == ESP_OK) {
        s_autotune_enabled = (en != 0);
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
    nvs_set_u8(h, GRIND_KEY_AT_EN, s_autotune_enabled ? 1 : 0);

    nvs_commit(h);
    nvs_close(h);
}

static void run_autotune(void)
{
    /* User toggle — once they've found their sweet spot they can lock the
     * offset by disabling autotune from the web UI. */
    if (!s_autotune_enabled)
        return;

    /* Manual stop is not a normal completion; the user aborted mid-shot,
     * so the result doesn't reflect what the controller would have produced. */
    if (s_manual_stop)
        return;

    /* Gate: skip anomalous shots (too short, flow never established) so a
     * single bad reading doesn't corrupt tuning. */
    if (s_grind_ms < AUTOTUNE_MIN_GRIND_MS)
        return;
    if (s_pulse_flow_rate < AUTOTUNE_MIN_FLOW_G_S)
        return;

    /* Sensor-spike gate: if the settled weight is BELOW the weight that
     * triggered cutoff, the cutoff was caused by a transient spike (motor
     * EMI), not real coffee.  The "result" is meaningless for tuning. */
    if (s_weight_before_pulses + 0.05f < s_weight_at_cutoff)
        return;

    /* Implausibly high flow rate at cutoff → almost certainly a sensor
     * glitch.  Real espresso grinders top out around 10 g/s. */
    if (s_pulse_flow_rate > MAX_PLAUSIBLE_FLOW_G_S)
        return;

    /* Anomalous result magnitude — far below or far above target indicates
     * a starved hopper, jammed grinder, or a sensor anomaly that survived
     * the gates above.  Tuning from these shots wrecks convergence. */
    float abs_delta = s_result - s_target;
    if (abs_delta < 0.0f) abs_delta = -abs_delta;
    float max_delta = s_target * AUTOTUNE_MAX_DELTA_FRAC;
    if (max_delta < AUTOTUNE_MAX_DELTA_FLOOR_G)
        max_delta = AUTOTUNE_MAX_DELTA_FLOOR_G;
    if (abs_delta > max_delta)
        return;

#if !GRIND_DEMO_MODE
    /* Motor-latency autotune: derive the loop latency from the observed
     * coast and EMA it.  The cutoff is per-sample now, so there is no
     * measurement staleness to subtract — coast/flow IS the latency the
     * stop threshold must compensate (motor spin-down + confirmation
     * window).  This keeps the coast prediction self-correcting so the
     * user doesn't have to tune motor_latency_ms by hand. */
    float actual_coast_g = s_weight_before_pulses - s_weight_at_cutoff;
    if (actual_coast_g > 0.02f) {
        float observed_ms = (actual_coast_g / s_pulse_flow_rate) * 1000.0f;
        s_motor_latency_ms = clampf(
            (1.0f - MOTOR_LATENCY_ALPHA) * s_motor_latency_ms +
            MOTOR_LATENCY_ALPHA * observed_ms,
            10.0f, 500.0f);
    }
#endif

    /* Pre-stop offset autotune — tuned from the MAIN-STOP error (settled
     * weight before any pulse), not the final result.  Pulses pull the
     * final weight back to target, so tuning on s_result would let a
     * persistent main-stop undershoot hide inside the deadband forever,
     * paying the multi-second pulse penalty on every shot.  Converging the
     * main stop itself makes pulses the exception, not the routine.
     * Capped per shot so the offset cannot swing to the clamp floor in a
     * single step — even a noisy outlier nudges by at most
     * AUTOTUNE_MAX_STEP_G. */
    float main_delta = s_weight_before_pulses - s_target;
    if (fabsf(main_delta) > AUTOTUNE_DEADBAND) {
        float step = main_delta * AUTOTUNE_FACTOR;
        if (step >  AUTOTUNE_MAX_STEP_G) step =  AUTOTUNE_MAX_STEP_G;
        if (step < -AUTOTUNE_MAX_STEP_G) step = -AUTOTUNE_MAX_STEP_G;
        s_offset = clampf(s_offset + step, OFFSET_MIN_G, OFFSET_MAX_G);
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
    /* If flow was never measured during the main grind (cutoff fired before
     * any coffee flowed), adopt the rate the pulse just produced so the next
     * pulse is sized from real data instead of the fallback assumption. */
    if (s_pulse_flow_rate <= 0.01f)
        s_pulse_flow_rate = (float)s_flow_rate_g_s;

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

    if (shortfall > PULSE_MIN_G && s_pulse_attempts < PULSE_MAX_ATTEMPTS)
    {
        /* Flow can be unmeasured when the cutoff fired before any coffee
         * flowed (tiny targets push stop_at ≤ 0).  Assume a typical rate so
         * the shot is still dosed up by pulses instead of ending at ~0 g;
         * pulse_done_cb adopts the real observed rate after each pulse. */
        if (flow <= 0.01f)
            flow = PULSE_FALLBACK_FLOW_G_S;

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
 * hx711_task) and advances the state machine.  Settle / pulse decisions run
 * here at ~10 Hz — adequate, since the SSR is already off by then.  The
 * cut-off itself fires per-sample in hx711_task (see s_cutoff_pending).
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
        /* Minimum waits before the std-dev gate may end settling: after a
         * correction pulse the grinder coasts ~250 ms and the scale needs
         * time to stabilise; after the main stop we wait even though the
         * settle buffer was invalidated at cutoff — the pre-pulse weight
         * feeds offset autotune and the pulse decision, so grabbing it
         * while coffee is still falling corrupts both. */
        int min_ticks = (s_pulse_attempts > 0) ? PULSE_SETTLE_MIN_TICKS
                                               : MAIN_SETTLE_MIN_TICKS;
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

#if GRIND_DEMO_MODE
    /* Demo mode has no hx711_task, so the stop threshold is still evaluated
     * here at 10 Hz.  Real mode cuts off per-sample in hx711_task. */
    float flow = (float)s_flow_rate_g_s;
    float coast_g = (flow > 0.01f)
                        ? (s_motor_latency_ms / 1000.0f) * flow * COAST_RATIO
                        : COAST_FALLBACK_G;
    if (coast_g > MAX_COAST_G) coast_g = MAX_COAST_G;
    float stop_at = s_target - coast_g - s_offset;

    if (s_weight >= stop_at)
    {
        s_weight_at_cutoff = s_weight;
        s_grind_ms = lv_tick_get() - s_grind_start_ms;
        ssr_set(0);
        s_pulse_attempts = 0;
        s_settle_ticks = 0;
        s_state = GRIND_SETTLING;

        /* Simulate mechanical overshoot so the settled weight is realistic */
        s_weight = stop_at + coast_g + DEMO_OVERSHOOT_G + lcg_noise() * 0.05f;
        s_pulse_flow_rate = DEMO_RAMP_G_PER_SEC;
        if (s_weight < 0.0f)
            s_weight = 0.0f;
    }
#else
    /* The fast cutoff fired in hx711_task — advance the state machine.
     * SSR is already off, so the timer latency here costs nothing. */
    if (s_cutoff_pending)
    {
        s_cutoff_pending = false;
        s_pulse_flow_rate  = s_cutoff_flow;
        s_weight_at_cutoff = s_cutoff_weight;
        s_grind_ms = lv_tick_get() - s_grind_start_ms;
        s_pulse_attempts = 0;
        s_settle_ticks = 0;

        /* Invalidate the settle buffer: it still holds mid-grind readings,
         * which would let is_settled() fire while coffee is coasting onto
         * the scale.  The settled weight captured next feeds offset autotune
         * and the pulse decision — it must be a genuinely settled value. */
        s_settle_buf_full = false;
        s_settle_buf_idx = 0;

        s_state = GRIND_SETTLING;
        return;
    }

    /* Safety backstop: sensor died mid-grind or runaway duration — cut the
     * SSR and end the shot.  Flagged as a manual stop so autotune skips it. */
    if (!s_hx711_healthy ||
        lv_tick_get() - s_grind_start_ms > MAX_GRIND_MS)
    {
        s_cutoff_armed = false;
        ssr_set(0);
        s_result = s_weight;
        s_manual_stop = true;
        s_state = GRIND_DONE;
    }
#endif
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
#if !GRIND_DEMO_MODE
    /* Arm the fast cutoff AFTER the SSR is energised, so a fire can never be
     * undone by the ssr_set(1) above.  The initial threshold uses the
     * fallback coast; hx711_task refreshes it every block once flow data
     * exists.  For tiny targets this can be ≤ 0 — the cutoff then fires
     * almost immediately and the pulse path doses the shot up instead. */
    s_stop_at_g = s_target - COAST_FALLBACK_G - s_offset;
    s_cutoff_pending = false;
    s_cutoff_armed = true;
#endif
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
    /* Only from rest: starting during SETTLING/PULSING would orphan the
     * pulse timer and corrupt the in-flight shot's bookkeeping. */
    if (s_state != GRIND_IDLE && s_state != GRIND_DONE)
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
    s_manual_stop          = false;
#if !GRIND_DEMO_MODE
    s_cutoff_armed         = false;
    s_cutoff_pending       = false;
#endif
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

#if !GRIND_DEMO_MODE
    /* Disarm the fast cutoff so hx711_task stops evaluating the threshold. */
    s_cutoff_armed   = false;
    s_cutoff_pending = false;
#endif

    /* Turn off SSR if it was on */
    if (s_state == GRIND_RUNNING || s_state == GRIND_PULSING)
        ssr_set(0);

    if (s_state == GRIND_RUNNING || s_state == GRIND_SETTLING || s_state == GRIND_PULSING)
    {
        s_result = s_weight;
        s_manual_stop = true;   /* flag the abort so run_autotune skips */
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
    /* Only from a fully idle controller — energising the SSR while taring,
     * settling or pulsing would corrupt an in-flight shot. */
    if (s_state != GRIND_IDLE || s_purging)
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
float grind_ctrl_get_target(void) { return s_target; }
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

void grind_ctrl_set_calibration_mode(bool on)
{
#if !GRIND_DEMO_MODE
    s_calibration_mode = on;
#else
    (void)on;
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

bool grind_ctrl_get_autotune_enabled(void) { return s_autotune_enabled; }

void grind_ctrl_set_autotune_enabled(bool on)
{
    s_autotune_enabled = on;
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
