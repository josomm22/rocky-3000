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
#define AUTOTUNE_FACTOR             0.3f
#define AUTOTUNE_DEADBAND           0.1f
#define AUTOTUNE_MAX_STEP_G         0.15f
#define AUTOTUNE_MAX_DELTA_FRAC     0.25f
#define AUTOTUNE_MAX_DELTA_FLOOR_G  1.5f
#define MAX_PLAUSIBLE_FLOW_G_S      15.0f
#define OFFSET_MIN_G               -2.0f
#define OFFSET_MAX_G                5.0f

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
 * Mirrors run_autotune() in grind_controller.c exactly — including the
 * per-shot AUTOTUNE_MAX_STEP_G cap that prevents a single noisy shot from
 * driving the offset to the clamp floor.
 */
static float autotune_step(float offset, float target, float result)
{
    float delta = result - target;
    if (fabsf(delta) <= AUTOTUNE_DEADBAND)
        return offset;  /* deadband: no change */
    float step = delta * AUTOTUNE_FACTOR;
    if (step >  AUTOTUNE_MAX_STEP_G) step =  AUTOTUNE_MAX_STEP_G;
    if (step < -AUTOTUNE_MAX_STEP_G) step = -AUTOTUNE_MAX_STEP_G;
    return clampf(offset + step, OFFSET_MIN_G, OFFSET_MAX_G);
}

/*
 * Full validity gate — mirrors all early-return guards at the top of
 * run_autotune().  Returns true when the shot is healthy enough for
 * both offset and motor-latency adjustment.
 *
 *   autotune_enabled  — user master switch (NVS-backed)
 *   manual_stop       — user pressed STOP mid-shot
 *   grind_ms          — main SSR-on duration
 *   flow_g_s          — flow rate captured at cutoff
 *   weight_at_cutoff  — scale reading when SSR cut
 *   weight_settled    — settled weight before any pulse
 *   target            — requested dose
 *   result            — final dispensed weight
 */
static bool should_autotune(bool autotune_enabled, bool manual_stop,
                            uint32_t grind_ms, float flow_g_s,
                            float weight_at_cutoff, float weight_settled,
                            float target, float result)
{
    if (!autotune_enabled) return false;
    if (manual_stop)       return false;
    if (grind_ms < AUTOTUNE_MIN_GRIND_MS) return false;
    if (flow_g_s < AUTOTUNE_MIN_FLOW_G_S) return false;
    if (weight_settled + 0.05f < weight_at_cutoff) return false;  /* spike-induced cutoff */
    if (flow_g_s > MAX_PLAUSIBLE_FLOW_G_S) return false;
    float abs_delta = fabsf(result - target);
    float max_delta = target * AUTOTUNE_MAX_DELTA_FRAC;
    if (max_delta < AUTOTUNE_MAX_DELTA_FLOOR_G) max_delta = AUTOTUNE_MAX_DELTA_FLOOR_G;
    if (abs_delta > max_delta) return false;
    return true;
}

/* Convenience wrappers for tests that only care about one or two inputs. */
static bool gate_basic(uint32_t grind_ms, float flow_g_s)
{
    return should_autotune(true, false, grind_ms, flow_g_s, 0.0f, 0.5f, 18.0f, 18.0f);
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
    /* result > target → offset should grow so we cut off earlier next time.
     * delta = +0.5g, raw step = 0.5*0.3 = 0.15g → at cap (no further clamp). */
    float new_off = autotune_step(0.3f, 18.0f, 18.5f);
    TEST_ASSERT_GREATER_THAN_FLOAT(0.3f, new_off);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 0.45f, new_off);
}

void test_undershoot_decreases_offset(void)
{
    /* delta = -0.4g, raw step = 0.3*(-0.4) = -0.12g → within cap. */
    float new_off = autotune_step(0.5f, 18.0f, 17.6f);
    TEST_ASSERT_LESS_THAN_FLOAT(0.5f, new_off);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 0.38f, new_off);
}

void test_step_capped_on_large_overshoot(void)
{
    /* delta = +5g, raw step = 1.5g → must be capped to AUTOTUNE_MAX_STEP_G */
    float new_off = autotune_step(0.0f, 18.0f, 23.0f);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, AUTOTUNE_MAX_STEP_G, new_off);
}

void test_step_capped_on_large_undershoot(void)
{
    /* delta = -3g, raw step = -0.9g → must be capped to -AUTOTUNE_MAX_STEP_G */
    float new_off = autotune_step(0.0f, 18.0f, 15.0f);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, -AUTOTUNE_MAX_STEP_G, new_off);
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
    /* Even with the per-step cap, repeated overshoots eventually push the
     * offset to OFFSET_MAX_G.  Start near the ceiling so a single capped
     * step (+0.15g) crosses it. */
    float new_off = autotune_step(4.9f, 18.0f, 28.0f);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, OFFSET_MAX_G, new_off);
}

void test_clamp_min(void)
{
    /* Same idea on the floor — start near OFFSET_MIN_G so the capped step
     * crosses it.  In practice the anomaly gates should reject huge-delta
     * shots before this happens, but the OFFSET_MIN_G clamp is the last line. */
    float new_off = autotune_step(-1.9f, 18.0f, 8.0f);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, OFFSET_MIN_G, new_off);
}

void test_convergence(void)
{
    /*
     * Simulate repeated shots with a fixed mechanical overshoot of 0.18 g
     * (same as DEMO_OVERSHOOT_G).  The per-step cap slows convergence but
     * the offset still settles within one deadband of the true overshoot.
     */
    const float target   = 18.0f;
    const float overshoot = 0.18f;
    float offset = 0.3f;

    for (int i = 0; i < 20; i++) {
        float result = (target - offset) + overshoot;
        offset = autotune_step(offset, target, result);
    }

    TEST_ASSERT_FLOAT_WITHIN(AUTOTUNE_DEADBAND, overshoot, offset);
}

void test_step_cap_protects_against_outlier(void)
{
    /*
     * The pre-fix bug: a single spike-corrupted shot drove offset from 0
     * to OFFSET_MIN_G (-2.0) in one step (delta = -18.8 × 0.3 = -5.6 → clamp).
     * After the fix, even if the shot somehow passes the gates, the worst
     * case is a single AUTOTUNE_MAX_STEP_G nudge.
     */
    float new_off = autotune_step(0.0f, 21.0f, 2.2f);   /* historical bad shot */
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, -AUTOTUNE_MAX_STEP_G, new_off);
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
    TEST_ASSERT_TRUE(gate_basic(3000, 2.5f));
}

void test_gate_skips_short_grind(void)
{
    /* Manual stop or instant-completion glitch → grind_ms < 500 → skip */
    TEST_ASSERT_FALSE(gate_basic(200, 2.5f));
    TEST_ASSERT_FALSE(gate_basic(499, 2.5f));
}

void test_gate_passes_at_min_grind_ms(void)
{
    /* Boundary: exactly AUTOTUNE_MIN_GRIND_MS should pass (the gate uses <) */
    TEST_ASSERT_TRUE(gate_basic(AUTOTUNE_MIN_GRIND_MS, 2.5f));
}

void test_gate_skips_low_flow(void)
{
    /* Beans ran out mid-grind / motor never spun up → flow < 0.5 → skip */
    TEST_ASSERT_FALSE(gate_basic(3000, 0.3f));
    TEST_ASSERT_FALSE(gate_basic(3000, 0.0f));
}

void test_gate_passes_at_min_flow(void)
{
    /* Boundary: exactly AUTOTUNE_MIN_FLOW_G_S should pass (the gate uses <) */
    TEST_ASSERT_TRUE(gate_basic(3000, AUTOTUNE_MIN_FLOW_G_S));
}

void test_gate_skips_zero_grind_ms(void)
{
    /* Manual STOP during GRIND_RUNNING never sets s_grind_ms → stays 0 */
    TEST_ASSERT_FALSE(gate_basic(0, 2.5f));
}

/* ── Gates added in v1.2.13 — anomaly / spike / user-toggle ── */

void test_gate_skips_when_disabled(void)
{
    /* User flipped autotune off in the web UI → never adjust offset. */
    TEST_ASSERT_FALSE(should_autotune(false, false, 3000, 2.5f,
                                      18.0f, 18.0f, 18.0f, 18.0f));
}

void test_gate_skips_on_manual_stop(void)
{
    /* User pressed STOP mid-shot — the result reflects the abort, not
     * what the controller would have produced. */
    TEST_ASSERT_FALSE(should_autotune(true, true, 3000, 2.5f,
                                      18.0f, 18.0f, 18.0f, 18.0f));
}

void test_gate_skips_on_spike_induced_cutoff(void)
{
    /* Cutoff fired at 22.8 g (spike), settled at 14.3 g — the historical
     * ts=1777300619 shot.  Negative apparent coast = sensor glitch. */
    TEST_ASSERT_FALSE(should_autotune(true, false, 12106, 5.0f,
                                      22.82f, 14.25f, 20.5f, 14.3f));
}

void test_gate_skips_on_implausible_flow(void)
{
    /* The historical ts=1777040502 shot: f=187 g/s.  No real grinder
     * does that — it's pure sensor noise. */
    TEST_ASSERT_FALSE(should_autotune(true, false, 724, 187.4f,
                                      0.8f, 1.36f, 21.0f, 2.2f));
}

void test_gate_skips_on_huge_negative_delta(void)
{
    /* Result far below target — bean starvation, jammed grinder, or any
     * anomaly that survived the earlier gates.  Don't tune from this. */
    TEST_ASSERT_FALSE(should_autotune(true, false, 5000, 2.0f,
                                      8.0f, 8.0f, 18.0f, 8.0f));
}

void test_gate_skips_on_huge_positive_delta(void)
{
    /* Symmetric: result far above target should also be rejected. */
    TEST_ASSERT_FALSE(should_autotune(true, false, 5000, 2.0f,
                                      25.0f, 25.0f, 18.0f, 25.0f));
}

void test_gate_floor_protects_small_targets(void)
{
    /* For a 5g target, 25% would be 1.25g — too tight against normal noise.
     * The 1.5g floor takes over. A 1.4g delta on a 5g shot should still tune. */
    TEST_ASSERT_TRUE(should_autotune(true, false, 5000, 1.5f,
                                     5.0f, 5.0f, 5.0f, 6.4f));   /* delta = 1.4g, under 1.5g floor */
}

/* ── runner ───────────────────────────────────────────────── */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_overshoot_increases_offset);
    RUN_TEST(test_undershoot_decreases_offset);
    RUN_TEST(test_step_capped_on_large_overshoot);
    RUN_TEST(test_step_capped_on_large_undershoot);
    RUN_TEST(test_step_cap_protects_against_outlier);
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
    RUN_TEST(test_gate_skips_when_disabled);
    RUN_TEST(test_gate_skips_on_manual_stop);
    RUN_TEST(test_gate_skips_on_spike_induced_cutoff);
    RUN_TEST(test_gate_skips_on_implausible_flow);
    RUN_TEST(test_gate_skips_on_huge_negative_delta);
    RUN_TEST(test_gate_skips_on_huge_positive_delta);
    RUN_TEST(test_gate_floor_protects_small_targets);

    return UNITY_END();
}
