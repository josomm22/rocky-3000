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
#include <stdint.h>

/* ── Build-time config ──────────────────────────────────────── */

#ifndef GRIND_DEMO_MODE
#define GRIND_DEMO_MODE 0
#endif

/* Pre-stop offset defaults & clamps */
#define DEFAULT_OFFSET_G  0.3f
#define AUTOTUNE_FACTOR   0.5f
#define AUTOTUNE_DEADBAND 0.1f   /* ignore deltas smaller than this (g) */
#define OFFSET_MIN_G      0.0f
#define OFFSET_MAX_G      5.0f

/* UI display refresh via LVGL timer (does NOT affect HX711 sample rate) */
#define UI_POLL_MS  100

/* Declared here so hx711_task (real mode) can read it before the rest of
 * the module state block. */
static volatile float s_cal_factor = 1.0f;

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

#define GPIO_SSR          GPIO_NUM_43   /* freed from UART0 TXD by uart_set_pin remap */
#define GPIO_HX711_DATA   GPIO_NUM_4
#define GPIO_HX711_CLK    GPIO_NUM_44   /* UART0 RXD — safe to fully reconfigure */

#define HX711_POLL_HZ   80              /* module output data rate (Hz)   */
#define HX711_POLL_MS   (1000 / HX711_POLL_HZ)  /* 12 ms between samples */
#define HX711_TASK_STACK  2048
#define HX711_TASK_PRIO   5

/*
 * Shared between hx711_task (writer) and the LVGL poll timer (reader).
 * A single float write on ESP32 is atomic at the hardware level, so no
 * mutex is needed — worst case the LVGL timer reads a sample one cycle
 * stale, which is 12.5 ms and completely irrelevant.
 */
static volatile float s_latest_weight = 0.0f;

/* EMA smoothing: α=0.2 → τ ≈ 4 samples (50 ms at 80 Hz).
 * Reduces per-sample noise ~2.2× while staying fast enough for
 * threshold detection and the 100 ms display refresh. */
#define HX711_EMA_ALPHA  0.2f

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
        if (hx711_read_grams(s_cal_factor, &g))
            s_latest_weight = HX711_EMA_ALPHA * g
                              + (1.0f - HX711_EMA_ALPHA) * s_latest_weight;
        vTaskDelay(pdMS_TO_TICKS(HX711_POLL_MS));
    }
}
#endif  /* !GRIND_DEMO_MODE */

/* ── Module state ───────────────────────────────────────────── */

static grind_state_t  s_state      = GRIND_IDLE;
static float          s_target     = 18.0f;
static float          s_offset  = DEFAULT_OFFSET_G;
static float          s_weight  = 0.0f;   /* snapshot read by UI / grind logic */
static float          s_result  = 0.0f;
static lv_timer_t    *s_timer   = NULL;

/* Coast settle (real mode only): ticks remaining after SSR cutoff before
 * reading the final weight.  2 × UI_POLL_MS = 200 ms, avoids blocking
 * the LVGL task with vTaskDelay. */
#define SETTLE_TICKS  2
static int            s_settle_ticks = 0;

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

/* ── LVGL poll timer (UI_POLL_MS, ~10 Hz) ───────────────────── */
/*
 * In real mode this just snapshots s_latest_weight, which the 80 Hz
 * hx711_task has been keeping up-to-date independently.
 * The SSR cut-off decision is therefore also made here at ~10 Hz — fine
 * for espresso doses where the grinder coasts ~0.1–0.3 g after cutoff.
 * If you need sub-100 ms cut-off precision, move the threshold check into
 * hx711_task and signal via an EventGroup bit.
 */
static void poll_cb(lv_timer_t *t)
{
    (void)t;

#if !GRIND_DEMO_MODE
    /* Waiting for coast-settle after SSR cutoff: count down ticks then
     * record the final weight.  s_state stays RUNNING during this window
     * so the display keeps refreshing. */
    if (s_settle_ticks > 0) {
        s_weight = (float)s_latest_weight;
        if (--s_settle_ticks == 0) {
            s_result = s_weight;
            run_autotune();
            s_state = GRIND_DONE;
        }
        return;
    }
#endif

    if (s_state != GRIND_RUNNING)
        return;

#if GRIND_DEMO_MODE
    float inc   = DEMO_RAMP_G_PER_SEC * (UI_POLL_MS / 1000.0f);
    float noise = lcg_noise() * DEMO_NOISE_AMP;
    s_weight += inc + noise;
    if (s_weight < 0.0f) s_weight = 0.0f;
#else
    /* Snapshot the latest reading from the 80 Hz task */
    s_weight = (float)s_latest_weight;
#endif

    float stop_at = s_target - s_offset;
    if (s_weight >= stop_at) {
        ssr_set(0);
#if GRIND_DEMO_MODE
        s_result = stop_at + DEMO_OVERSHOOT_G + lcg_noise() * 0.05f;
        if (s_result < 0.0f) s_result = 0.0f;
        run_autotune();
        s_state = GRIND_DONE;
#else
        /* Start coast-settle countdown; result recorded after SETTLE_TICKS */
        s_settle_ticks = SETTLE_TICKS;
#endif
    }
}

/* ── Public API ─────────────────────────────────────────────── */

void grind_ctrl_init(void)
{
    s_state  = GRIND_IDLE;
    s_weight = 0.0f;
    s_offset = DEFAULT_OFFSET_G;

#if !GRIND_DEMO_MODE
    /* Remap UART0 TXD/RXD off their default pins so GPIO43 is a free GPIO.
     * ESP_LOG output continues via USB Serial/JTAG (secondary console). */
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
    if (s_state == GRIND_RUNNING)
        return;

    s_target       = target_g;
    s_weight       = 0.0f;
    s_result       = 0.0f;
    s_settle_ticks = 0;
    s_state        = GRIND_RUNNING;

    ssr_set(1);
    lv_timer_resume(s_timer);
}

void grind_ctrl_stop(void)
{
    if (s_state != GRIND_RUNNING)
        return;

    ssr_set(0);
    s_result = s_weight;
    run_autotune();
    s_state = GRIND_DONE;
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
    s_cal_factor = clampf(f, 0.1f, 10.0f);
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

bool grind_ctrl_is_demo(void)
{
#if GRIND_DEMO_MODE
    return true;
#else
    return false;
#endif
}

void grind_ctrl_ack_done(void)
{
    if (s_state != GRIND_DONE)
        return;

    s_state  = GRIND_IDLE;
    s_weight = 0.0f;
    lv_timer_pause(s_timer);
}
