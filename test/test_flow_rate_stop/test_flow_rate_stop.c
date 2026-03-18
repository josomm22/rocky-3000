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

/* ── Mirror of constants in grind_controller.c ────────────── */
#define COAST_RATIO          1.0f
#define COAST_FALLBACK_G     0.3f
#define MOTOR_LATENCY_MS_DEFAULT  100.0f

/* ── Replicated helpers ────────────────────────────────────── */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/*
 * Compute coast distance in grams given the current flow rate and motor latency.
 * Falls back to COAST_FALLBACK_G when flow_rate is not yet established.
 */
static float compute_coast_g(float flow_rate_g_s, float motor_latency_ms)
{
    if (flow_rate_g_s > 0.01f)
        return (motor_latency_ms / 1000.0f) * flow_rate_g_s * COAST_RATIO;
    return COAST_FALLBACK_G;
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

    return UNITY_END();
}
