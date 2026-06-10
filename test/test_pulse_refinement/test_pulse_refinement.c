/*
 * Unit tests for the post-stop pulse refinement logic in grind_controller.c.
 *
 * After the main grind stops, if the settled weight is short of target by
 * more than PULSE_MIN_G, a correction pulse is fired:
 *
 *   flow_used = (flow > 0.01) ? flow : PULSE_FALLBACK_FLOW_G_S
 *   pulse_ms  = clamp(motor_latency_ms
 *                     + (shortfall / flow_used) * 1000 * PULSE_FACTOR,
 *                     PULSE_MIN_MS, PULSE_MAX_MS)
 *
 * A pulse is fired when:
 *   - shortfall > PULSE_MIN_G
 *   - attempts < PULSE_MAX_ATTEMPTS
 *
 * Unmeasured flow no longer suppresses the pulse (the pre-fix behaviour let
 * a tiny-target shot end at ~0 g): the fallback flow assumption is used
 * instead, and pulse_done_cb adopts the real rate after the first pulse.
 *
 * Run on host with: pio test -e native
 */

#include <unity.h>
#include <math.h>
#include <stdbool.h>

/* ── Mirror of constants in grind_controller.c ────────────── */
#define PULSE_MIN_G             0.05f
#define PULSE_MAX_ATTEMPTS      3
#define PULSE_MIN_MS            30
#define PULSE_MAX_MS            500
#define PULSE_FACTOR            0.8f
#define PULSE_FALLBACK_FLOW_G_S 3.0f
#define MOTOR_LATENCY_MS        250.0f  /* MOTOR_LATENCY_MS_DEFAULT */

/* ── Replicated helpers ────────────────────────────────────── */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Returns true if a correction pulse should be fired. */
static bool should_pulse(float shortfall, int attempts)
{
    return shortfall > PULSE_MIN_G
        && attempts < PULSE_MAX_ATTEMPTS;
}

/* Flow used for pulse sizing: measured when available, fallback otherwise. */
static float pulse_flow(float measured_g_s)
{
    return measured_g_s > 0.01f ? measured_g_s : PULSE_FALLBACK_FLOW_G_S;
}

/* Returns the pulse duration in ms (only call when should_pulse is true).
 * pulse = motor startup time + productive grinding time × undershoot factor */
static float compute_pulse_ms(float shortfall, float flow_rate_g_s,
                              float motor_latency_ms)
{
    float productive_ms = (shortfall / pulse_flow(flow_rate_g_s))
                        * 1000.0f * PULSE_FACTOR;
    return clampf(motor_latency_ms + productive_ms,
                  (float)PULSE_MIN_MS, (float)PULSE_MAX_MS);
}

/* ── setUp / tearDown ─────────────────────────────────────── */

void setUp(void)    {}
void tearDown(void) {}

/* ── should_pulse tests ───────────────────────────────────── */

#define FLOAT_DELTA 0.1f   /* ms-level tolerance is fine for pulse duration */

void test_pulse_fires_on_undershoot(void)
{
    /* Clear undershoot, first attempt → should pulse */
    TEST_ASSERT_TRUE(should_pulse(0.3f, 0));
}

void test_pulse_skipped_below_min_g(void)
{
    /* shortfall == PULSE_MIN_G is NOT > PULSE_MIN_G → no pulse */
    TEST_ASSERT_FALSE(should_pulse(PULSE_MIN_G, 0));
}

void test_pulse_skipped_well_below_min_g(void)
{
    TEST_ASSERT_FALSE(should_pulse(0.02f, 0));
}

void test_pulse_skipped_on_overshoot(void)
{
    /* shortfall < 0 → result > target → definitely no pulse */
    TEST_ASSERT_FALSE(should_pulse(-0.2f, 0));
}

void test_pulse_skipped_at_max_attempts(void)
{
    TEST_ASSERT_FALSE(should_pulse(0.3f, PULSE_MAX_ATTEMPTS));
}

void test_pulse_fires_before_max_attempts(void)
{
    TEST_ASSERT_TRUE(should_pulse(0.3f, PULSE_MAX_ATTEMPTS - 1));
}

void test_pulse_fires_just_above_min_g(void)
{
    TEST_ASSERT_TRUE(should_pulse(PULSE_MIN_G + 0.01f, 0));
}

/* ── pulse_flow fallback tests ────────────────────────────── */

void test_pulse_flow_uses_measured_when_available(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 2.5f, pulse_flow(2.5f));
}

void test_pulse_flow_falls_back_when_unmeasured(void)
{
    /* Tiny targets cut the SSR before any coffee flows: flow stays 0.
     * The pre-fix code skipped the pulse entirely and the shot ended at
     * ~0 g; now the fallback assumption lets pulses dose the shot up. */
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, PULSE_FALLBACK_FLOW_G_S, pulse_flow(0.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, PULSE_FALLBACK_FLOW_G_S, pulse_flow(0.01f));
}

/* ── compute_pulse_ms tests ───────────────────────────────── */

void test_pulse_duration_typical(void)
{
    /* shortfall=0.3g, flow=3 g/s, latency=250 ms
     * → 250 + (0.3/3)*1000*0.8 = 330 ms */
    float ms = compute_pulse_ms(0.3f, 3.0f, MOTOR_LATENCY_MS);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 330.0f, ms);
}

void test_pulse_duration_intentional_undershoot(void)
{
    /* PULSE_FACTOR=0.8 means we intentionally aim to add only 80% of
     * shortfall: 100 + (0.5/2)*1000*0.8 = 300 ms */
    float ms = compute_pulse_ms(0.5f, 2.0f, 100.0f);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 300.0f, ms);
}

void test_pulse_duration_clamps_to_min(void)
{
    /* Tiny shortfall + fast flow + minimal motor latency → raw duration
     * below PULSE_MIN_MS: 10 + (0.06/10)*1000*0.8 = 14.8 → clamp to 30 */
    float ms = compute_pulse_ms(0.06f, 10.0f, 10.0f);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, (float)PULSE_MIN_MS, ms);
}

void test_pulse_duration_clamps_to_max(void)
{
    /* Large shortfall + very slow flow → huge raw duration → clamp to
     * PULSE_MAX_MS: 250 + (2/0.05)*1000*0.8 = 32250 → 500 */
    float ms = compute_pulse_ms(2.0f, 0.05f, MOTOR_LATENCY_MS);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, (float)PULSE_MAX_MS, ms);
}

void test_pulse_duration_scales_with_shortfall(void)
{
    /* Larger shortfall → longer pulse */
    float ms_small = compute_pulse_ms(0.2f, 3.0f, MOTOR_LATENCY_MS);
    float ms_large = compute_pulse_ms(0.4f, 3.0f, MOTOR_LATENCY_MS);
    TEST_ASSERT_GREATER_THAN_FLOAT(ms_small, ms_large);
}

void test_pulse_duration_scales_with_flow(void)
{
    /* Faster flow → shorter pulse needed for the same shortfall */
    float ms_slow = compute_pulse_ms(0.3f, 1.0f, MOTOR_LATENCY_MS);
    float ms_fast = compute_pulse_ms(0.3f, 5.0f, MOTOR_LATENCY_MS);
    TEST_ASSERT_GREATER_THAN_FLOAT(ms_fast, ms_slow);
}

void test_pulse_duration_finite_on_zero_flow(void)
{
    /* The zero-flow shot must still produce a bounded, sane pulse via the
     * fallback flow: 250 + (0.3/3)*1000*0.8 = 330 ms */
    float ms = compute_pulse_ms(0.3f, 0.0f, MOTOR_LATENCY_MS);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 330.0f, ms);
}

void test_three_pulses_converge(void)
{
    /*
     * Simulate up to PULSE_MAX_ATTEMPTS correction pulses.
     * Each pulse adds (shortfall * PULSE_FACTOR) to the weight
     * (intentional undershoot per pulse).  After all attempts the
     * remaining shortfall must be less than the original.
     */
    float target  = 18.0f;
    float weight  = 17.5f;   /* 0.5 g short */
    int   attempts = 0;

    while (should_pulse(target - weight, attempts)
           && attempts < PULSE_MAX_ATTEMPTS)
    {
        float shortfall = target - weight;
        /* Simulate: pulse adds shortfall * PULSE_FACTOR grams */
        weight += shortfall * PULSE_FACTOR;
        attempts++;
    }

    /* After all pulses the weight should be closer to target than we started */
    TEST_ASSERT_GREATER_THAN_FLOAT(17.5f, weight);
    /* And total attempts must not exceed the cap */
    TEST_ASSERT_LESS_OR_EQUAL(PULSE_MAX_ATTEMPTS, attempts);
}

/* ── runner ───────────────────────────────────────────────── */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_pulse_fires_on_undershoot);
    RUN_TEST(test_pulse_skipped_below_min_g);
    RUN_TEST(test_pulse_skipped_well_below_min_g);
    RUN_TEST(test_pulse_skipped_on_overshoot);
    RUN_TEST(test_pulse_skipped_at_max_attempts);
    RUN_TEST(test_pulse_fires_before_max_attempts);
    RUN_TEST(test_pulse_fires_just_above_min_g);
    RUN_TEST(test_pulse_flow_uses_measured_when_available);
    RUN_TEST(test_pulse_flow_falls_back_when_unmeasured);
    RUN_TEST(test_pulse_duration_typical);
    RUN_TEST(test_pulse_duration_intentional_undershoot);
    RUN_TEST(test_pulse_duration_clamps_to_min);
    RUN_TEST(test_pulse_duration_clamps_to_max);
    RUN_TEST(test_pulse_duration_scales_with_shortfall);
    RUN_TEST(test_pulse_duration_scales_with_flow);
    RUN_TEST(test_pulse_duration_finite_on_zero_flow);
    RUN_TEST(test_three_pulses_converge);

    return UNITY_END();
}
