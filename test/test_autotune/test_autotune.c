/*
 * Unit tests for the auto-tune offset algorithm in grind_controller.c.
 *
 * run_autotune() is static, so we replicate its logic here as a pure
 * function and test the mathematical contract directly.  If the constants
 * change in grind_controller.c, update the defines below to match.
 *
 * Run on host with: pio test -e native
 */

#include <unity.h>
#include <math.h>      /* fabsf */
#include <stdbool.h>
#include <stdint.h>

/* ── Mirror of constants in grind_controller.c ────────────── */
#define AUTOTUNE_FACTOR        0.3f
#define AUTOTUNE_DEADBAND      0.1f
#define OFFSET_MIN_G          -2.0f
#define OFFSET_MAX_G           5.0f

/* Motor-latency autotune mirror */
#define MOTOR_LATENCY_ALPHA    0.3f
#define MOTOR_LATENCY_MIN_MS  10.0f
#define MOTOR_LATENCY_MAX_MS 500.0f
/* Measurement latency: half the HX711 block period
 * = (HX711_AVG_SAMPLES / HX711_POLL_HZ) * 1000 * 0.5
 * = (8 / 80) * 1000 * 0.5 = 50 ms */
#define MEAS_LATENCY_MS       50.0f
/* Coast must be at least this many g for the latency math to be trusted —
 * smaller readings are dominated by scale noise. */
#define COAST_MIN_G           0.02f

/* Validity gates */
#define AUTOTUNE_MIN_GRIND_MS  500
#define AUTOTUNE_MIN_FLOW_G_S  0.5f

/* ── Replicated helpers (pure functions, no hardware deps) ─── */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/*
 * Returns the new offset after one auto-tune step.
 * Mirrors run_autotune() in grind_controller.c exactly.
 */
static float autotune_step(float offset, float target, float result)
{
    float delta = result - target;
    if (fabsf(delta) <= AUTOTUNE_DEADBAND)
        return offset;  /* deadband: no change */
    return clampf(offset + delta * AUTOTUNE_FACTOR, OFFSET_MIN_G, OFFSET_MAX_G);
}

/*
 * Validity gate — mirrors the early-return guards at the top of
 * run_autotune().  Returns true when the shot is healthy enough
 * for both offset and motor-latency adjustment.
 */
static bool should_autotune(uint32_t grind_ms, float flow_g_s)
{
    if (grind_ms < AUTOTUNE_MIN_GRIND_MS) return false;
    if (flow_g_s < AUTOTUNE_MIN_FLOW_G_S) return false;
    return true;
}

/*
 * Returns the new motor latency (ms) after one EMA step from observed coast.
 * Mirrors the motor-latency block inside run_autotune().
 *   actual_coast_g  = weight_before_pulses - weight_at_cutoff
 *   total_latency   = actual_coast_g / flow_g_s * 1000
 *   motor_observed  = total_latency - MEAS_LATENCY_MS
 *   new_latency     = (1-α)·current + α·motor_observed   (clamped)
 *
 * Returns the unchanged input when the coast is too small to trust.
 */
static float motor_latency_step(float current_ms, float coast_g, float flow_g_s)
{
    if (coast_g <= COAST_MIN_G) return current_ms;
    float total_ms    = (coast_g / flow_g_s) * 1000.0f;
    float observed_ms = total_ms - MEAS_LATENCY_MS;
    float ema = (1.0f - MOTOR_LATENCY_ALPHA) * current_ms +
                MOTOR_LATENCY_ALPHA * observed_ms;
    return clampf(ema, MOTOR_LATENCY_MIN_MS, MOTOR_LATENCY_MAX_MS);
}

/* ── setUp / tearDown ─────────────────────────────────────── */

void setUp(void)    {}
void tearDown(void) {}

/* ── tests ────────────────────────────────────────────────── */

#define FLOAT_DELTA 0.0001f

void test_overshoot_increases_offset(void)
{
    /* result > target → offset should grow so we cut off earlier next time */
    float new_off = autotune_step(0.3f, 18.0f, 18.5f);
    TEST_ASSERT_GREATER_THAN_FLOAT(0.3f, new_off);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 0.45f, new_off);   /* 0.3 + 0.3*0.5 */
}

void test_undershoot_decreases_offset(void)
{
    /* result < target → offset should shrink (stop later next time) */
    float new_off = autotune_step(0.5f, 18.0f, 17.6f);
    TEST_ASSERT_LESS_THAN_FLOAT(0.5f, new_off);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 0.38f, new_off);   /* 0.5 + 0.3*(-0.4) */
}

void test_deadband_no_change_exact(void)
{
    /*
     * Use target=0.0f so that result-target is exact in IEEE 754
     * (subtracting 0 is a no-op).  This guarantees delta == AUTOTUNE_DEADBAND
     * exactly, so the <= boundary in the production code is exercised cleanly.
     */
    float new_off = autotune_step(0.3f, 0.0f, AUTOTUNE_DEADBAND);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 0.3f, new_off);
}

void test_deadband_no_change_below(void)
{
    float new_off = autotune_step(0.3f, 18.0f, 18.05f);  /* delta = 0.05 */
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 0.3f, new_off);
}

void test_above_deadband_changes(void)
{
    float new_off = autotune_step(0.3f, 18.0f, 18.11f);  /* delta = 0.11 > deadband */
    TEST_ASSERT_NOT_EQUAL_FLOAT(0.3f, new_off);
}

void test_clamp_max(void)
{
    /* Large overshoot must not push offset above OFFSET_MAX_G */
    float new_off = autotune_step(4.9f, 18.0f, 28.0f);   /* would be 4.9 + 0.3*10 = 7.9 */
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, OFFSET_MAX_G, new_off);
}

void test_clamp_min(void)
{
    /* Large undershoot must not pull offset below OFFSET_MIN_G */
    float new_off = autotune_step(0.1f, 18.0f, 8.0f);    /* would be 0.1 + 0.3*(-10) = -2.9 */
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, OFFSET_MIN_G, new_off);
}

void test_convergence(void)
{
    /*
     * Simulate repeated shots with a fixed mechanical overshoot of 0.18 g
     * (same as DEMO_OVERSHOOT_G).  The offset should converge and stabilise
     * within a few iterations.
     */
    const float target   = 18.0f;
    const float overshoot = 0.18f;   /* grinder always coasts this much */
    float offset = 0.3f;

    for (int i = 0; i < 10; i++) {
        float result = (target - offset) + overshoot;
        offset = autotune_step(offset, target, result);
    }

    /*
     * After convergence the offset settles within one deadband of the true
     * overshoot.  Once |result - target| <= AUTOTUNE_DEADBAND the algorithm
     * stops adjusting, so the tightest reachable bound is:
     *   |offset - overshoot| <= AUTOTUNE_DEADBAND
     */
    TEST_ASSERT_FLOAT_WITHIN(AUTOTUNE_DEADBAND, overshoot, offset);
}

void test_perfect_shot_no_change(void)
{
    /* result == target exactly → delta == 0 → no change */
    float new_off = autotune_step(0.3f, 18.0f, 18.0f);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 0.3f, new_off);
}

/* ── Motor-latency autotune tests ─────────────────────────── */

void test_motor_latency_increases_when_coast_larger(void)
{
    /* current=200ms, observed coast=0.6g at 2g/s → total=300ms,
     * motor=250ms → EMA = 0.7×200 + 0.3×250 = 215 */
    float new_lat = motor_latency_step(200.0f, 0.6f, 2.0f);
    TEST_ASSERT_GREATER_THAN_FLOAT(200.0f, new_lat);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 215.0f, new_lat);
}

void test_motor_latency_decreases_when_coast_smaller(void)
{
    /* current=300ms, observed coast=0.3g at 2g/s → total=150ms,
     * motor=100ms → EMA = 0.7×300 + 0.3×100 = 240 */
    float new_lat = motor_latency_step(300.0f, 0.3f, 2.0f);
    TEST_ASSERT_LESS_THAN_FLOAT(300.0f, new_lat);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 240.0f, new_lat);
}

void test_motor_latency_skipped_on_tiny_coast(void)
{
    /* coast below COAST_MIN_G (0.02g) is dominated by scale noise — leave latency alone */
    float new_lat = motor_latency_step(200.0f, 0.01f, 2.0f);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 200.0f, new_lat);
}

void test_motor_latency_skipped_on_negative_coast(void)
{
    /* Negative coast = noise dip (settled weight read below cutoff weight).
     * Treat as untrustworthy and leave latency unchanged. */
    float new_lat = motor_latency_step(200.0f, -0.05f, 2.0f);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 200.0f, new_lat);
}

void test_motor_latency_clamp_max(void)
{
    /* Pathological coast → derived motor latency would blow past 500ms;
     * must clamp to MOTOR_LATENCY_MAX_MS */
    float new_lat = motor_latency_step(400.0f, 2.0f, 1.0f);  /* total=2000, obs=1950 */
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, MOTOR_LATENCY_MAX_MS, new_lat);
}

void test_motor_latency_clamp_min(void)
{
    /* observed=-45 (coast smaller than measurement latency) → EMA goes
     * negative; must clamp to MOTOR_LATENCY_MIN_MS */
    float new_lat = motor_latency_step(15.0f, 0.05f, 10.0f);  /* total=5, obs=-45 */
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, MOTOR_LATENCY_MIN_MS, new_lat);
}

void test_motor_latency_converges(void)
{
    /* Simulate a grinder whose true motor latency is 200ms, flow 2g/s.
     * True coast = (200+50)/1000 × 2 = 0.5g.  Starting from a wildly wrong
     * 400ms estimate, the EMA should approach 200 within 10 shots. */
    const float true_latency = 200.0f;
    const float coast        = 0.5f;
    const float flow         = 2.0f;
    float latency = 400.0f;

    for (int i = 0; i < 10; i++)
        latency = motor_latency_step(latency, coast, flow);

    /* After 10 shots the EMA should be within 10 ms of the true value. */
    TEST_ASSERT_FLOAT_WITHIN(10.0f, true_latency, latency);
}

/* ── Validity-gate tests ──────────────────────────────────── */

void test_gate_passes_normal_shot(void)
{
    TEST_ASSERT_TRUE(should_autotune(3000, 2.5f));
}

void test_gate_skips_short_grind(void)
{
    /* Manual stop or instant-completion glitch → grind_ms < 500 → skip */
    TEST_ASSERT_FALSE(should_autotune(200, 2.5f));
    TEST_ASSERT_FALSE(should_autotune(499, 2.5f));
}

void test_gate_passes_at_min_grind_ms(void)
{
    /* Boundary: exactly AUTOTUNE_MIN_GRIND_MS should pass (the gate uses <) */
    TEST_ASSERT_TRUE(should_autotune(AUTOTUNE_MIN_GRIND_MS, 2.5f));
}

void test_gate_skips_low_flow(void)
{
    /* Beans ran out mid-grind / motor never spun up → flow < 0.5 → skip */
    TEST_ASSERT_FALSE(should_autotune(3000, 0.3f));
    TEST_ASSERT_FALSE(should_autotune(3000, 0.0f));
}

void test_gate_passes_at_min_flow(void)
{
    /* Boundary: exactly AUTOTUNE_MIN_FLOW_G_S should pass (the gate uses <) */
    TEST_ASSERT_TRUE(should_autotune(3000, AUTOTUNE_MIN_FLOW_G_S));
}

void test_gate_skips_zero_grind_ms(void)
{
    /* Manual STOP during GRIND_RUNNING never sets s_grind_ms → stays 0 */
    TEST_ASSERT_FALSE(should_autotune(0, 2.5f));
}

/* ── runner ───────────────────────────────────────────────── */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_overshoot_increases_offset);
    RUN_TEST(test_undershoot_decreases_offset);
    RUN_TEST(test_deadband_no_change_exact);
    RUN_TEST(test_deadband_no_change_below);
    RUN_TEST(test_above_deadband_changes);
    RUN_TEST(test_clamp_max);
    RUN_TEST(test_clamp_min);
    RUN_TEST(test_convergence);
    RUN_TEST(test_perfect_shot_no_change);

    RUN_TEST(test_motor_latency_increases_when_coast_larger);
    RUN_TEST(test_motor_latency_decreases_when_coast_smaller);
    RUN_TEST(test_motor_latency_skipped_on_tiny_coast);
    RUN_TEST(test_motor_latency_skipped_on_negative_coast);
    RUN_TEST(test_motor_latency_clamp_max);
    RUN_TEST(test_motor_latency_clamp_min);
    RUN_TEST(test_motor_latency_converges);

    RUN_TEST(test_gate_passes_normal_shot);
    RUN_TEST(test_gate_skips_short_grind);
    RUN_TEST(test_gate_passes_at_min_grind_ms);
    RUN_TEST(test_gate_skips_low_flow);
    RUN_TEST(test_gate_passes_at_min_flow);
    RUN_TEST(test_gate_skips_zero_grind_ms);

    return UNITY_END();
}
