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
#include <math.h>   /* fabsf */

/* ── Mirror of constants in grind_controller.c ────────────── */
#define AUTOTUNE_FACTOR   0.5f
#define AUTOTUNE_DEADBAND 0.1f
#define OFFSET_MIN_G      0.0f
#define OFFSET_MAX_G      5.0f

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
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 0.55f, new_off);   /* 0.3 + 0.5*0.5 */
}

void test_undershoot_decreases_offset(void)
{
    /* result < target → offset should shrink (stop later next time) */
    float new_off = autotune_step(0.5f, 18.0f, 17.6f);
    TEST_ASSERT_LESS_THAN_FLOAT(0.5f, new_off);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 0.3f, new_off);    /* 0.5 + 0.5*(-0.4) */
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
    float new_off = autotune_step(4.9f, 18.0f, 28.0f);   /* would be 4.9 + 5.0 = 9.9 */
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, OFFSET_MAX_G, new_off);
}

void test_clamp_min(void)
{
    /* Large undershoot must not pull offset below OFFSET_MIN_G */
    float new_off = autotune_step(0.1f, 18.0f, 8.0f);    /* would be 0.1 + 0.5*(-10) = -4.9 */
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

    return UNITY_END();
}
