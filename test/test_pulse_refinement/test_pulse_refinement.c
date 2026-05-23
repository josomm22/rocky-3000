/*
 * Unit tests for the post-stop pulse refinement logic in grind_controller.c.
 *
 * After the main grind stops and settles, if the weight is short of target,
 * a correction pulse may be fired.  A pulse fires when ALL of these hold:
 *
 *   1. shortfall * PULSE_FACTOR > coast_g  (coast alone won't overshoot)
 *   2. shortfall > PULSE_MIN_G
 *   3. attempts < PULSE_MAX_ATTEMPTS
 *   4. flow_rate > 0.01 g/s
 *
 * The SSR-on duration accounts for the coast that naturally follows, so the
 * net delivered weight equals shortfall × PULSE_FACTOR:
 *
 *   coast_g  = (motor_latency_ms / 1000) × flow_rate
 *   pulse_ms = clamp(((shortfall × PULSE_FACTOR − coast_g) / flow) × 1000,
 *                    PULSE_MIN_MS, PULSE_MAX_MS)
 *
 * Run on host with: pio test -e native
 */

#include <unity.h>
#include <math.h>
#include <stdbool.h>

/* ── Mirror of constants in grind_controller.c ────────────── */
#define PULSE_MIN_G        0.05f
#define PULSE_MAX_ATTEMPTS 3
#define PULSE_MIN_MS       30
#define PULSE_MAX_MS       500
#define PULSE_FACTOR       0.8f

/* ── Replicated helpers ────────────────────────────────────── */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/*
 * True when the post-pulse coast alone would deliver >= shortfall * PULSE_FACTOR.
 * In that case, any pulse duration causes overshoot and we should skip.
 */
static bool coast_would_overshoot(float shortfall, float flow_g_s, float motor_latency_ms)
{
    float coast_g = (motor_latency_ms / 1000.0f) * flow_g_s;
    return shortfall * PULSE_FACTOR <= coast_g;
}

/* Returns true if a correction pulse should be fired. */
static bool should_pulse(float shortfall, int attempts, float flow_g_s,
                         float motor_latency_ms)
{
    return !coast_would_overshoot(shortfall, flow_g_s, motor_latency_ms)
        && shortfall > PULSE_MIN_G
        && attempts < PULSE_MAX_ATTEMPTS
        && flow_g_s > 0.01f;
}

/*
 * Returns the pulse duration in ms (only call when should_pulse is true).
 * Subtracts the coast contribution so motor_latency_ms is not double-counted.
 */
static float compute_pulse_ms(float shortfall, float flow_g_s, float motor_latency_ms)
{
    float coast_g = (motor_latency_ms / 1000.0f) * flow_g_s;
    float ms = ((shortfall * PULSE_FACTOR - coast_g) / flow_g_s) * 1000.0f;
    return clampf(ms, (float)PULSE_MIN_MS, (float)PULSE_MAX_MS);
}

/* ── setUp / tearDown ─────────────────────────────────────── */

void setUp(void)    {}
void tearDown(void) {}

/* ── coast_would_overshoot tests ─────────────────────────── */

#define FLOAT_DELTA 0.1f   /* ms-level tolerance is fine for pulse duration */

void test_coast_overshoots_small_shortfall(void)
{
    /* User's exact bug: 0.15 g short at 2.5 g/s, motor_lat=200 ms
     * coast_g = 0.5 g; shortfall*FACTOR = 0.12 <= 0.5 → would overshoot */
    TEST_ASSERT_TRUE(coast_would_overshoot(0.15f, 2.5f, 200.0f));
}

void test_coast_overshoots_exact_boundary(void)
{
    /* shortfall * PULSE_FACTOR == coast_g exactly → still overshoots (uses <=) */
    /* coast_g = 0.1*3 = 0.3; shortfall*0.8 = 0.375*0.8 = 0.3 exactly */
    TEST_ASSERT_TRUE(coast_would_overshoot(0.375f, 3.0f, 100.0f));
}

void test_coast_does_not_overshoot_large_shortfall(void)
{
    /* shortfall=1.0g at 3g/s, motor_lat=100ms → coast=0.3, 0.8>0.3 → OK to pulse */
    TEST_ASSERT_FALSE(coast_would_overshoot(1.0f, 3.0f, 100.0f));
}

void test_coast_does_not_overshoot_just_above_threshold(void)
{
    /* 0.38g shortfall: 0.38*0.8=0.304 > 0.3 coast → just safe to pulse */
    TEST_ASSERT_FALSE(coast_would_overshoot(0.38f, 3.0f, 100.0f));
}

/* ── should_pulse tests ───────────────────────────────────── */

void test_pulse_fires_on_clear_undershoot(void)
{
    /* shortfall=1.0g well above coast threshold, first attempt */
    TEST_ASSERT_TRUE(should_pulse(1.0f, 0, 3.0f, 100.0f));
}

void test_pulse_skipped_when_coast_alone_overshoots(void)
{
    /* 0.2g shortfall at 3g/s, motor_lat=100ms: coast=0.3, 0.16<=0.3 → skip */
    TEST_ASSERT_FALSE(should_pulse(0.2f, 0, 3.0f, 100.0f));
}

void test_pulse_skipped_second_pulse_realistic(void)
{
    /* Regression: pulse 2 after first pulse left 0.15g short.
     * motor_lat=200ms, flow=2.5g/s → coast=0.5; 0.12<=0.5 → skip. */
    TEST_ASSERT_FALSE(should_pulse(0.15f, 1, 2.5f, 200.0f));
}

void test_pulse_skipped_below_min_g(void)
{
    /* shortfall == PULSE_MIN_G is NOT > PULSE_MIN_G → no pulse */
    TEST_ASSERT_FALSE(should_pulse(PULSE_MIN_G, 0, 3.0f, 100.0f));
}

void test_pulse_skipped_well_below_min_g(void)
{
    TEST_ASSERT_FALSE(should_pulse(0.02f, 0, 3.0f, 100.0f));
}

void test_pulse_skipped_on_overshoot(void)
{
    /* shortfall < 0 → result > target → no pulse */
    TEST_ASSERT_FALSE(should_pulse(-0.2f, 0, 3.0f, 100.0f));
}

void test_pulse_skipped_at_max_attempts(void)
{
    TEST_ASSERT_FALSE(should_pulse(1.0f, PULSE_MAX_ATTEMPTS, 3.0f, 100.0f));
}

void test_pulse_fires_before_max_attempts(void)
{
    TEST_ASSERT_TRUE(should_pulse(1.0f, PULSE_MAX_ATTEMPTS - 1, 3.0f, 100.0f));
}

void test_pulse_skipped_on_zero_flow(void)
{
    TEST_ASSERT_FALSE(should_pulse(1.0f, 0, 0.0f, 100.0f));
}

void test_pulse_skipped_on_low_flow(void)
{
    /* flow_rate == 0.01 is NOT > 0.01 → no pulse */
    TEST_ASSERT_FALSE(should_pulse(1.0f, 0, 0.01f, 100.0f));
}

/* ── compute_pulse_ms tests ───────────────────────────────── */

void test_pulse_duration_typical(void)
{
    /* shortfall=1.0g, flow=3g/s, motor_lat=100ms:
     * coast_g=0.3; ((1.0*0.8 - 0.3) / 3) * 1000 = (0.5/3)*1000 ≈ 166.7 ms */
    float ms = compute_pulse_ms(1.0f, 3.0f, 100.0f);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 166.7f, ms);
}

void test_pulse_duration_coast_subtracted(void)
{
    /* Verify coast is subtracted from the SSR-on time — if motor_latency doubles
     * the pulse must shrink, not grow. */
    float ms_low_lat  = compute_pulse_ms(1.0f, 3.0f, 50.0f);
    float ms_high_lat = compute_pulse_ms(1.0f, 3.0f, 150.0f);
    TEST_ASSERT_GREATER_THAN_FLOAT(ms_high_lat, ms_low_lat);
}

void test_pulse_duration_net_delivery_equals_factor_times_shortfall(void)
{
    /* The net grams delivered = SSR-on grams + coast grams
     *   = flow*(pulse_ms/1000) + coast_g
     *   = flow*((shortfall*FACTOR - coast_g)/flow) + coast_g
     *   = shortfall * FACTOR
     * Verify numerically. */
    float shortfall = 1.0f, flow = 3.0f, latency = 100.0f;
    float coast_g   = (latency / 1000.0f) * flow;
    float pulse_ms  = compute_pulse_ms(shortfall, flow, latency);
    float net       = flow * (pulse_ms / 1000.0f) + coast_g;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, shortfall * PULSE_FACTOR, net);
}

void test_pulse_duration_clamps_to_min(void)
{
    /* Very small correctable shortfall → tiny raw duration → clamp to PULSE_MIN_MS */
    /* shortfall=0.5g, flow=3g/s, lat=100ms: coast=0.3, (0.1/3)*1000=33ms > 30 just barely */
    /* Use a case where it clamps: shortfall=0.39g, flow=3g/s, lat=100ms:
     * coast_g=0.3, ((0.312-0.3)/3)*1000 = (0.012/3)*1000 = 4ms → clamp */
    float ms = compute_pulse_ms(0.39f, 3.0f, 100.0f);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, (float)PULSE_MIN_MS, ms);
}

void test_pulse_duration_clamps_to_max(void)
{
    /* Large shortfall + very slow flow → clamp to PULSE_MAX_MS */
    float ms = compute_pulse_ms(5.0f, 0.05f, 10.0f);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, (float)PULSE_MAX_MS, ms);
}

void test_pulse_duration_scales_with_shortfall(void)
{
    /* Larger shortfall → longer SSR-on time */
    float ms_small = compute_pulse_ms(0.5f, 3.0f, 100.0f);
    float ms_large = compute_pulse_ms(1.5f, 3.0f, 100.0f);
    TEST_ASSERT_GREATER_THAN_FLOAT(ms_small, ms_large);
}

void test_pulse_duration_scales_with_flow(void)
{
    /* Faster flow → shorter pulse needed for the same shortfall */
    float ms_slow = compute_pulse_ms(1.0f, 1.0f, 50.0f);
    float ms_fast = compute_pulse_ms(1.0f, 5.0f, 50.0f);
    TEST_ASSERT_GREATER_THAN_FLOAT(ms_fast, ms_slow);
}

void test_three_pulses_converge(void)
{
    /*
     * Simulate up to PULSE_MAX_ATTEMPTS correction pulses.
     * Each successful pulse adds shortfall * PULSE_FACTOR.
     * The coast_overshoots gate stops firing once the shortfall is small
     * enough that the coast alone would cover it.
     */
    float target  = 18.0f;
    float weight  = 16.5f;   /* 1.5 g short — well above coast threshold */
    float flow    = 3.0f;
    float latency = 100.0f;
    int   attempts = 0;

    while (should_pulse(target - weight, attempts, flow, latency)
           && attempts < PULSE_MAX_ATTEMPTS)
    {
        float shortfall = target - weight;
        weight += shortfall * PULSE_FACTOR;
        attempts++;
    }

    /* Weight must have moved toward target */
    TEST_ASSERT_GREATER_THAN_FLOAT(16.5f, weight);
    /* And never exceeded the attempt cap */
    TEST_ASSERT_LESS_OR_EQUAL(PULSE_MAX_ATTEMPTS, attempts);
}

/* ── runner ───────────────────────────────────────────────── */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_coast_overshoots_small_shortfall);
    RUN_TEST(test_coast_overshoots_exact_boundary);
    RUN_TEST(test_coast_does_not_overshoot_large_shortfall);
    RUN_TEST(test_coast_does_not_overshoot_just_above_threshold);

    RUN_TEST(test_pulse_fires_on_clear_undershoot);
    RUN_TEST(test_pulse_skipped_when_coast_alone_overshoots);
    RUN_TEST(test_pulse_skipped_second_pulse_realistic);
    RUN_TEST(test_pulse_skipped_below_min_g);
    RUN_TEST(test_pulse_skipped_well_below_min_g);
    RUN_TEST(test_pulse_skipped_on_overshoot);
    RUN_TEST(test_pulse_skipped_at_max_attempts);
    RUN_TEST(test_pulse_fires_before_max_attempts);
    RUN_TEST(test_pulse_skipped_on_zero_flow);
    RUN_TEST(test_pulse_skipped_on_low_flow);

    RUN_TEST(test_pulse_duration_typical);
    RUN_TEST(test_pulse_duration_coast_subtracted);
    RUN_TEST(test_pulse_duration_net_delivery_equals_factor_times_shortfall);
    RUN_TEST(test_pulse_duration_clamps_to_min);
    RUN_TEST(test_pulse_duration_clamps_to_max);
    RUN_TEST(test_pulse_duration_scales_with_shortfall);
    RUN_TEST(test_pulse_duration_scales_with_flow);
    RUN_TEST(test_three_pulses_converge);

    return UNITY_END();
}
