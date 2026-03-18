/*
 * Unit tests for the post-stop pulse refinement logic in grind_controller.c.
 *
 * After the main grind stops, if the settled weight is short of target by
 * more than PULSE_MIN_G, a correction pulse is fired:
 *
 *   pulse_ms = clamp((shortfall / flow_rate) * 1000 * PULSE_FACTOR,
 *                    PULSE_MIN_MS, PULSE_MAX_MS)
 *
 * A pulse is only fired when:
 *   - shortfall > PULSE_MIN_G
 *   - attempts < PULSE_MAX_ATTEMPTS
 *   - flow_rate > 0.01 g/s
 *
 * Run on host with: pio test -e native
 */

#include <unity.h>
#include <math.h>
#include <stdbool.h>

/* ── Mirror of constants in grind_controller.c ────────────── */
#define PULSE_MIN_G        0.15f
#define PULSE_MAX_ATTEMPTS 3
#define PULSE_MIN_MS       30
#define PULSE_MAX_MS       500
#define PULSE_FACTOR       0.8f

/* ── Replicated helpers ────────────────────────────────────── */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Returns true if a correction pulse should be fired. */
static bool should_pulse(float shortfall, int attempts, float flow_rate_g_s)
{
    return shortfall > PULSE_MIN_G
        && attempts < PULSE_MAX_ATTEMPTS
        && flow_rate_g_s > 0.01f;
}

/* Returns the pulse duration in ms (only call when should_pulse is true). */
static float compute_pulse_ms(float shortfall, float flow_rate_g_s)
{
    float ms = (shortfall / flow_rate_g_s) * 1000.0f * PULSE_FACTOR;
    return clampf(ms, (float)PULSE_MIN_MS, (float)PULSE_MAX_MS);
}

/* ── setUp / tearDown ─────────────────────────────────────── */

void setUp(void)    {}
void tearDown(void) {}

/* ── should_pulse tests ───────────────────────────────────── */

#define FLOAT_DELTA 0.1f   /* ms-level tolerance is fine for pulse duration */

void test_pulse_fires_on_undershoot(void)
{
    /* Clear undershoot, first attempt, good flow rate → should pulse */
    TEST_ASSERT_TRUE(should_pulse(0.3f, 0, 3.0f));
}

void test_pulse_skipped_below_min_g(void)
{
    /* shortfall == PULSE_MIN_G is NOT > PULSE_MIN_G → no pulse */
    TEST_ASSERT_FALSE(should_pulse(PULSE_MIN_G, 0, 3.0f));
}

void test_pulse_skipped_well_below_min_g(void)
{
    TEST_ASSERT_FALSE(should_pulse(0.05f, 0, 3.0f));
}

void test_pulse_skipped_on_overshoot(void)
{
    /* shortfall < 0 → result > target → definitely no pulse */
    TEST_ASSERT_FALSE(should_pulse(-0.2f, 0, 3.0f));
}

void test_pulse_skipped_at_max_attempts(void)
{
    TEST_ASSERT_FALSE(should_pulse(0.3f, PULSE_MAX_ATTEMPTS, 3.0f));
}

void test_pulse_fires_before_max_attempts(void)
{
    TEST_ASSERT_TRUE(should_pulse(0.3f, PULSE_MAX_ATTEMPTS - 1, 3.0f));
}

void test_pulse_skipped_on_zero_flow(void)
{
    TEST_ASSERT_FALSE(should_pulse(0.3f, 0, 0.0f));
}

void test_pulse_skipped_on_low_flow(void)
{
    /* flow_rate == 0.01 is NOT > 0.01 → no pulse */
    TEST_ASSERT_FALSE(should_pulse(0.3f, 0, 0.01f));
}

void test_pulse_fires_just_above_min_g(void)
{
    TEST_ASSERT_TRUE(should_pulse(PULSE_MIN_G + 0.01f, 0, 3.0f));
}

/* ── compute_pulse_ms tests ───────────────────────────────── */

void test_pulse_duration_typical(void)
{
    /* shortfall=0.3g, flow=3 g/s → (0.3/3)*1000*0.8 = 80 ms */
    float ms = compute_pulse_ms(0.3f, 3.0f);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 80.0f, ms);
}

void test_pulse_duration_intentional_undershoot(void)
{
    /* PULSE_FACTOR=0.8 means we intentionally aim to add only 80% of shortfall */
    float shortfall = 0.5f;
    float flow      = 2.0f;
    /* (0.5/2)*1000*0.8 = 200 ms */
    float ms = compute_pulse_ms(shortfall, flow);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 200.0f, ms);
}

void test_pulse_duration_clamps_to_min(void)
{
    /* Very small shortfall + fast flow → tiny raw duration → clamp to PULSE_MIN_MS */
    float ms = compute_pulse_ms(0.16f, 10.0f);  /* raw = (0.16/10)*1000*0.8 = 12.8 ms */
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, (float)PULSE_MIN_MS, ms);
}

void test_pulse_duration_clamps_to_max(void)
{
    /* Large shortfall + very slow flow → huge raw duration → clamp to PULSE_MAX_MS */
    float ms = compute_pulse_ms(2.0f, 0.05f);  /* raw = (2/0.05)*1000*0.8 = 32000 ms */
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, (float)PULSE_MAX_MS, ms);
}

void test_pulse_duration_scales_with_shortfall(void)
{
    /* Larger shortfall → longer pulse */
    float ms_small = compute_pulse_ms(0.2f, 3.0f);
    float ms_large = compute_pulse_ms(0.4f, 3.0f);
    TEST_ASSERT_GREATER_THAN_FLOAT(ms_small, ms_large);
}

void test_pulse_duration_scales_with_flow(void)
{
    /* Faster flow → shorter pulse needed for the same shortfall */
    float ms_slow = compute_pulse_ms(0.3f, 1.0f);
    float ms_fast = compute_pulse_ms(0.3f, 5.0f);
    TEST_ASSERT_GREATER_THAN_FLOAT(ms_fast, ms_slow);
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
    float flow    = 3.0f;
    int   attempts = 0;

    while (should_pulse(target - weight, attempts, flow)
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
    RUN_TEST(test_pulse_skipped_on_zero_flow);
    RUN_TEST(test_pulse_skipped_on_low_flow);
    RUN_TEST(test_pulse_fires_just_above_min_g);
    RUN_TEST(test_pulse_duration_typical);
    RUN_TEST(test_pulse_duration_intentional_undershoot);
    RUN_TEST(test_pulse_duration_clamps_to_min);
    RUN_TEST(test_pulse_duration_clamps_to_max);
    RUN_TEST(test_pulse_duration_scales_with_shortfall);
    RUN_TEST(test_pulse_duration_scales_with_flow);
    RUN_TEST(test_three_pulses_converge);

    return UNITY_END();
}
