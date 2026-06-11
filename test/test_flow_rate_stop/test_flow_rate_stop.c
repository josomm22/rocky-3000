/*
 * Unit tests for the dynamic flow-rate-based stop prediction in grind_controller.c.
 *
 * The key formula is:
 *   coast_g = (motor_latency_ms / 1000) * flow_rate_g_s * COAST_RATIO
 *   stop_at  = target - coast_g - offset  (offset is residual bias, default 0)
 *
 * When flow_rate_g_s == 0 the code falls back to COAST_FALLBACK_G.
 *
 * These are pure-math helpers replicated from grind_controller.c.
 * Run on host with: pio test -e native
 */

#include <unity.h>
#include <math.h>
#include <stdbool.h>

/* ── Mirror of constants in grind_controller.c ────────────── */
#define COAST_RATIO          1.0f
#define COAST_FALLBACK_G     0.3f
#define MAX_COAST_G          3.0f
#define MOTOR_LATENCY_MS_DEFAULT  100.0f
#define MAX_PLAUSIBLE_FLOW_G_S    15.0f
#define FLOW_EMA_ALPHA       0.3f
#define CUTOFF_CONFIRM_SAMPLES 3

/* ── Replicated helpers ────────────────────────────────────── */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/*
 * Compute coast distance in grams given the current flow rate and motor latency.
 * Falls back to COAST_FALLBACK_G when flow_rate is not yet established;
 * hard-capped at MAX_COAST_G so corrupt inputs can't cut the shot at <1 s.
 */
static float compute_coast_g(float flow_rate_g_s, float motor_latency_ms)
{
    float coast = COAST_FALLBACK_G;
    if (flow_rate_g_s > 0.01f)
        coast = (motor_latency_ms / 1000.0f) * flow_rate_g_s * COAST_RATIO;
    if (coast > MAX_COAST_G)
        coast = MAX_COAST_G;
    return coast;
}

/*
 * One flow-rate update as performed per block in hx711_task:
 * clamp the raw block delta to [0, MAX_PLAUSIBLE_FLOW_G_S], then EMA it.
 */
static float flow_ema_step(float prev_flow, float raw_rate)
{
    if (raw_rate < 0.0f) raw_rate = 0.0f;
    if (raw_rate > MAX_PLAUSIBLE_FLOW_G_S) raw_rate = MAX_PLAUSIBLE_FLOW_G_S;
    return FLOW_EMA_ALPHA * raw_rate + (1.0f - FLOW_EMA_ALPHA) * prev_flow;
}

/*
 * One per-sample cutoff-confirmation step as performed in hx711_task.
 * Returns true when the SSR should be cut: CUTOFF_CONFIRM_SAMPLES
 * consecutive samples at/above the threshold.  A sample below the
 * threshold resets the counter.
 */
static bool cutoff_step(int *confirm, float sample_g, float stop_at_g)
{
    if (sample_g >= stop_at_g) {
        if (++(*confirm) >= CUTOFF_CONFIRM_SAMPLES)
            return true;
    } else {
        *confirm = 0;
    }
    return false;
}

/* Full stop threshold. */
static float compute_stop_at(float target, float flow_rate_g_s,
                              float motor_latency_ms, float offset)
{
    float coast_g = compute_coast_g(flow_rate_g_s, motor_latency_ms);
    return target - coast_g - offset;
}

/* ── setUp / tearDown ─────────────────────────────────────── */

void setUp(void)    {}
void tearDown(void) {}

/* ── Tests ────────────────────────────────────────────────── */

#define FLOAT_DELTA 0.0001f

void test_coast_g_zero_flow_uses_fallback(void)
{
    /* No flow rate yet → fall back to fixed 0.3 g */
    float coast = compute_coast_g(0.0f, MOTOR_LATENCY_MS_DEFAULT);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, COAST_FALLBACK_G, coast);
}

void test_coast_g_below_threshold_uses_fallback(void)
{
    /* flow_rate == 0.005 is below the 0.01 guard → fallback */
    float coast = compute_coast_g(0.005f, MOTOR_LATENCY_MS_DEFAULT);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, COAST_FALLBACK_G, coast);
}

void test_coast_g_typical_espresso(void)
{
    /* 3 g/s flow, 100 ms latency → coast = 0.1 * 3.0 * 1.0 = 0.3 g */
    float coast = compute_coast_g(3.0f, 100.0f);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 0.3f, coast);
}

void test_coast_g_fast_flow(void)
{
    /* 5 g/s (coarse grind), 100 ms → coast = 0.5 g */
    float coast = compute_coast_g(5.0f, 100.0f);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 0.5f, coast);
}

void test_coast_g_slow_flow(void)
{
    /* 1 g/s (very fine), 100 ms → coast = 0.1 g */
    float coast = compute_coast_g(1.0f, 100.0f);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 0.1f, coast);
}

void test_coast_g_scales_with_latency(void)
{
    /* Higher latency → more coast at the same flow rate */
    float coast_50ms  = compute_coast_g(3.0f,  50.0f);
    float coast_150ms = compute_coast_g(3.0f, 150.0f);
    TEST_ASSERT_GREATER_THAN_FLOAT(coast_50ms, coast_150ms);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 0.15f, coast_50ms);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 0.45f, coast_150ms);
}

void test_stop_at_no_bias(void)
{
    /* target=18, flow=3 g/s, latency=100 ms, offset=0 → stop at 17.7 g */
    float stop = compute_stop_at(18.0f, 3.0f, 100.0f, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 17.7f, stop);
}

void test_stop_at_with_positive_bias(void)
{
    /* Positive residual offset (persistent overshoot) shifts stop earlier */
    float stop_no_bias = compute_stop_at(18.0f, 3.0f, 100.0f, 0.0f);
    float stop_bias    = compute_stop_at(18.0f, 3.0f, 100.0f, 0.1f);
    TEST_ASSERT_LESS_THAN_FLOAT(stop_no_bias, stop_bias);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 17.6f, stop_bias);
}

void test_stop_at_with_negative_bias(void)
{
    /* Negative residual offset (persistent undershoot) shifts stop later */
    float stop = compute_stop_at(18.0f, 3.0f, 100.0f, -0.1f);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 17.8f, stop);
}

void test_stop_at_fallback_when_no_flow(void)
{
    /* Without flow rate, coast = COAST_FALLBACK_G = 0.3, offset = 0 */
    float stop = compute_stop_at(18.0f, 0.0f, 100.0f, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 18.0f - COAST_FALLBACK_G, stop);
}

void test_stop_at_always_less_than_target(void)
{
    /* The stop threshold must always be below target (coast > 0 and offset <= 0.5) */
    float stop = compute_stop_at(18.0f, 3.0f, 100.0f, 0.0f);
    TEST_ASSERT_LESS_THAN_FLOAT(18.0f, stop);
}

void test_coast_g_capped_at_max(void)
{
    /* 15 g/s (spike-gate ceiling) × 500 ms latency → raw coast 7.5 g;
     * must cap at MAX_COAST_G so the shot can't be cut grams early. */
    float coast = compute_coast_g(15.0f, 500.0f);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, MAX_COAST_G, coast);
}

/* ── Flow-rate EMA tests ──────────────────────────────────── */

void test_flow_ema_negative_delta_clamps_to_zero(void)
{
    /* Scale settling after tare can produce negative block deltas. */
    float f = flow_ema_step(2.0f, -1.0f);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 0.7f * 2.0f, f);
}

void test_flow_ema_spike_clamped_then_smoothed(void)
{
    /* A 50 g/s glitch block is first clamped to 15 g/s, then the EMA
     * only lets 30% of it through — the flow used for coast prediction
     * moves from 2.0 to 5.9, not to 50. */
    float f = flow_ema_step(2.0f, 50.0f);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 0.3f * 15.0f + 0.7f * 2.0f, f);
}

void test_flow_ema_attenuates_single_block_noise(void)
{
    /* Steady 2 g/s with one ±1 g/s noisy block: the EMA moves by only
     * 30% of the disturbance instead of passing it straight to stop_at. */
    float up   = flow_ema_step(2.0f, 3.0f);
    float down = flow_ema_step(2.0f, 1.0f);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 2.3f, up);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 1.7f, down);
}

void test_flow_ema_converges_to_steady_rate(void)
{
    /* From standstill, a steady 3 g/s flow must be tracked within ~1 s
     * (10 blocks): EMA(10) = 3 × (1 - 0.7^10) ≈ 2.92. */
    float f = 0.0f;
    for (int i = 0; i < 10; i++)
        f = flow_ema_step(f, 3.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 3.0f, f);
}

/* ── Cutoff-confirmation tests ────────────────────────────── */

void test_cutoff_fires_after_consecutive_samples(void)
{
    int confirm = 0;
    TEST_ASSERT_FALSE(cutoff_step(&confirm, 17.8f, 17.7f));
    TEST_ASSERT_FALSE(cutoff_step(&confirm, 17.8f, 17.7f));
    TEST_ASSERT_TRUE(cutoff_step(&confirm, 17.9f, 17.7f));
}

void test_cutoff_ignores_single_spike(void)
{
    /* One EMI spike above the threshold must not end the shot. */
    int confirm = 0;
    TEST_ASSERT_FALSE(cutoff_step(&confirm, 25.0f, 17.7f));  /* spike   */
    TEST_ASSERT_FALSE(cutoff_step(&confirm, 12.0f, 17.7f));  /* resets  */
    TEST_ASSERT_FALSE(cutoff_step(&confirm, 12.1f, 17.7f));
    TEST_ASSERT_EQUAL_INT(0, confirm);
}

void test_cutoff_spike_burst_below_confirm_count_rejected(void)
{
    /* Even a two-sample burst (25 ms of EMI) is rejected; only a sustained
     * crossing — real coffee on the scale — cuts the SSR. */
    int confirm = 0;
    TEST_ASSERT_FALSE(cutoff_step(&confirm, 25.0f, 17.7f));
    TEST_ASSERT_FALSE(cutoff_step(&confirm, 25.0f, 17.7f));
    TEST_ASSERT_FALSE(cutoff_step(&confirm, 12.0f, 17.7f));
    TEST_ASSERT_EQUAL_INT(0, confirm);
}

void test_cutoff_fires_immediately_when_threshold_nonpositive(void)
{
    /* Tiny target + offset can push stop_at ≤ 0: the cutoff fires on the
     * first confirmed samples and the pulse path doses the shot up. */
    int confirm = 0;
    TEST_ASSERT_FALSE(cutoff_step(&confirm, 0.0f, -0.1f));
    TEST_ASSERT_FALSE(cutoff_step(&confirm, 0.0f, -0.1f));
    TEST_ASSERT_TRUE(cutoff_step(&confirm, 0.0f, -0.1f));
}

/* ── runner ───────────────────────────────────────────────── */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_coast_g_zero_flow_uses_fallback);
    RUN_TEST(test_coast_g_below_threshold_uses_fallback);
    RUN_TEST(test_coast_g_typical_espresso);
    RUN_TEST(test_coast_g_fast_flow);
    RUN_TEST(test_coast_g_slow_flow);
    RUN_TEST(test_coast_g_scales_with_latency);
    RUN_TEST(test_stop_at_no_bias);
    RUN_TEST(test_stop_at_with_positive_bias);
    RUN_TEST(test_stop_at_with_negative_bias);
    RUN_TEST(test_stop_at_fallback_when_no_flow);
    RUN_TEST(test_stop_at_always_less_than_target);
    RUN_TEST(test_coast_g_capped_at_max);

    RUN_TEST(test_flow_ema_negative_delta_clamps_to_zero);
    RUN_TEST(test_flow_ema_spike_clamped_then_smoothed);
    RUN_TEST(test_flow_ema_attenuates_single_block_noise);
    RUN_TEST(test_flow_ema_converges_to_steady_rate);

    RUN_TEST(test_cutoff_fires_after_consecutive_samples);
    RUN_TEST(test_cutoff_ignores_single_spike);
    RUN_TEST(test_cutoff_spike_burst_below_confirm_count_rejected);
    RUN_TEST(test_cutoff_fires_immediately_when_threshold_nonpositive);

    return UNITY_END();
}
