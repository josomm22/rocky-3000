/*
 * test_grind_by_weight.c
 *
 * End-to-end simulation of the grind-by-weight cycle.
 *
 * grind_controller.c depends on LVGL timers, FreeRTOS, and GPIO so it
 * cannot be linked directly in a host-native build.  Instead this file
 * replicates the demo-mode poll logic as a pure C state machine and
 * drives it through complete grind cycles:
 *
 *   start → weight rises → SSR cuts off at stop_at → settle ticks
 *         → (correction pulses if undershoot) → DONE
 *
 * All constants are mirrored from grind_controller.c.  If a constant
 * changes there, update the matching define below.
 *
 * Run with: pio test -e native
 */

#include <unity.h>
#include <math.h>
#include <stdbool.h>

/* ── Constants mirrored from grind_controller.c ───────────── */

#define DEMO_RAMP_G_PER_SEC      3.0f
#define DEMO_OVERSHOOT_G         0.18f   /* fixed mechanical coast in demo mode */
#define UI_POLL_MS               100
#define SETTLE_TICKS             2
#define COAST_RATIO              1.0f
#define COAST_FALLBACK_G         0.3f
#define MOTOR_LATENCY_MS_DEFAULT 100.0f
#define AUTOTUNE_FACTOR          0.5f
#define AUTOTUNE_DEADBAND        0.1f
#define OFFSET_MIN_G            -2.0f
#define OFFSET_MAX_G             2.0f
#define PULSE_MIN_G              0.15f
#define PULSE_MAX_ATTEMPTS       3
#define PULSE_MIN_MS             30
#define PULSE_MAX_MS             500
#define PULSE_FACTOR             0.8f

/* ── Simulation state ─────────────────────────────────────── */

typedef enum {
    SIM_IDLE = 0,
    SIM_RUNNING,
    SIM_SETTLING,
    SIM_DONE,
} sim_state_t;

typedef struct {
    sim_state_t state;
    float       weight;
    float       target;
    float       offset;         /* auto-tuned residual bias; persists across shots */
    float       flow_rate_g_s;  /* coast prediction input; default = DEMO_RAMP    */
    float       motor_latency_ms;
    float       result;         /* final weight; valid when state == SIM_DONE      */
    int         settle_ticks;
    int         pulse_attempts;
    int         pulse_count;    /* total correction pulses fired this cycle        */
    int         tick_count;     /* total poll ticks elapsed this cycle             */
    float       weight_at_ssr_off; /* scale reading the instant the SSR cut off   */
} sim_t;

/* ── Pure helpers ─────────────────────────────────────────── */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void sim_autotune(sim_t *s)
{
    float delta = s->result - s->target;
    if (fabsf(delta) <= AUTOTUNE_DEADBAND)
        return;
    s->offset = clampf(s->offset + delta * AUTOTUNE_FACTOR, OFFSET_MIN_G, OFFSET_MAX_G);
}

/* ── Simulation engine ────────────────────────────────────── */

/*
 * Decide: fire a correction pulse or declare DONE.
 * Mirrors settle_complete() in grind_controller.c.
 *
 * A pulse of pulse_ms adds weight analytically:
 *   weight_added = flow_rate * (pulse_ms / 1000)
 *                = flow_rate * (shortfall / flow_rate) * PULSE_FACTOR
 *                = shortfall * PULSE_FACTOR
 * This matches the demo-mode weight ramp exactly.
 */
static void sim_settle_complete(sim_t *s)
{
    float shortfall = s->target - s->weight;

    if (shortfall > PULSE_MIN_G
        && s->pulse_attempts < PULSE_MAX_ATTEMPTS
        && s->flow_rate_g_s > 0.01f)
    {
        float pulse_ms = (shortfall / s->flow_rate_g_s) * 1000.0f * PULSE_FACTOR;
        pulse_ms = clampf(pulse_ms, (float)PULSE_MIN_MS, (float)PULSE_MAX_MS);
        s->weight      += s->flow_rate_g_s * (pulse_ms / 1000.0f);
        s->pulse_attempts++;
        s->pulse_count++;
        s->settle_ticks = SETTLE_TICKS;
        /* Remain in SIM_SETTLING for the re-settle countdown */
    }
    else
    {
        s->result = s->weight;
        sim_autotune(s);
        s->state = SIM_DONE;
    }
}

/*
 * Advance the simulation by one UI_POLL_MS tick.
 * Mirrors poll_cb() in grind_controller.c (demo mode).
 */
static void sim_tick(sim_t *s)
{
    if (s->state == SIM_DONE || s->state == SIM_IDLE)
        return;

    s->tick_count++;

    if (s->state == SIM_SETTLING) {
        if (--s->settle_ticks == 0)
            sim_settle_complete(s);
        return;
    }

    if (s->state != SIM_RUNNING)
        return;

    /* Weight increases at the demo ramp rate regardless of flow_rate_g_s */
    s->weight += DEMO_RAMP_G_PER_SEC * (UI_POLL_MS / 1000.0f);

    /* Dynamic stop threshold */
    float flow    = s->flow_rate_g_s;
    float coast_g = (flow > 0.01f)
                    ? (s->motor_latency_ms / 1000.0f) * flow * COAST_RATIO
                    : COAST_FALLBACK_G;
    float stop_at = s->target - coast_g - s->offset;

    if (s->weight >= stop_at) {
        s->weight_at_ssr_off = s->weight;
        s->pulse_attempts    = 0;
        s->settle_ticks      = SETTLE_TICKS;
        s->state             = SIM_SETTLING;
        /* Demo-mode deterministic overshoot (no noise — tests are deterministic) */
        s->weight = stop_at + coast_g + DEMO_OVERSHOOT_G;
    }
}

/*
 * Initialise for a new grind cycle.  The offset is NOT reset here so
 * auto-tune state carries across shots (as it does in the real firmware).
 */
static void sim_start(sim_t *s, float target_g)
{
    s->state             = SIM_RUNNING;
    s->weight            = 0.0f;
    s->target            = target_g;
    s->flow_rate_g_s     = DEMO_RAMP_G_PER_SEC;
    s->motor_latency_ms  = MOTOR_LATENCY_MS_DEFAULT;
    s->settle_ticks      = 0;
    s->pulse_attempts    = 0;
    s->pulse_count       = 0;
    s->result            = 0.0f;
    s->tick_count        = 0;
    s->weight_at_ssr_off = 0.0f;
}

/* Run until DONE or max_ticks.  Returns true when DONE was reached. */
static bool sim_run(sim_t *s, int max_ticks)
{
    while (s->state != SIM_DONE && s->tick_count < max_ticks)
        sim_tick(s);
    return s->state == SIM_DONE;
}

/* ── Test fixtures ─────────────────────────────────────────── */

static sim_t g;   /* one sim instance, offset reset to 0 in setUp */

#define MAX_TICKS 1000  /* safety cap; ~100 s of simulated grind */

void setUp(void)
{
    g.offset          = 0.0f;
    g.state           = SIM_IDLE;
    g.motor_latency_ms = MOTOR_LATENCY_MS_DEFAULT;
}

void tearDown(void) {}

/* ── Tests ─────────────────────────────────────────────────── */

#define F_DELTA 0.01f   /* tight tolerance for exact arithmetic */

/*
 * Basic sanity: a standard 18 g shot reaches DONE and the result
 * lands within 1 g of target (default offset, no prior auto-tune).
 */
void test_grind_18g_completes(void)
{
    sim_start(&g, 18.0f);
    TEST_ASSERT_TRUE(sim_run(&g, MAX_TICKS));
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 18.0f, g.result);
}

void test_grind_20g_completes(void)
{
    sim_start(&g, 20.0f);
    TEST_ASSERT_TRUE(sim_run(&g, MAX_TICKS));
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 20.0f, g.result);
}

void test_grind_12g_completes(void)
{
    sim_start(&g, 12.0f);
    TEST_ASSERT_TRUE(sim_run(&g, MAX_TICKS));
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 12.0f, g.result);
}

/*
 * The SSR must cut off before the target weight is reached so the
 * coast distance absorbs the remaining grams.
 */
void test_ssr_cuts_off_before_target(void)
{
    sim_start(&g, 18.0f);
    sim_run(&g, MAX_TICKS);
    TEST_ASSERT_LESS_THAN_FLOAT(18.0f, g.weight_at_ssr_off);
}

/*
 * At 3 g/s with 100 ms ticks the ramp takes ~60 ticks plus 2 settle
 * ticks.  Even with a few correction pulses the grind must finish in
 * well under 200 ticks.
 */
void test_grind_finishes_in_finite_ticks(void)
{
    sim_start(&g, 18.0f);
    sim_run(&g, MAX_TICKS);
    TEST_ASSERT_LESS_THAN_FLOAT(200.0f, (float)g.tick_count);
}

/*
 * With the default offset (0 g) the demo-mode overshoot of 0.18 g puts
 * the result above target — no undershoot, so no correction pulse fires.
 */
void test_no_pulse_on_default_offset(void)
{
    sim_start(&g, 18.0f);
    sim_run(&g, MAX_TICKS);
    TEST_ASSERT_EQUAL_INT(0, g.pulse_count);
}

/*
 * A large positive offset makes the SSR cut off too early, leaving an
 * undershoot bigger than PULSE_MIN_G.  The correction pulse must fire.
 *
 * With offset=0.5 the settled weight is target - 0.5 + 0.18 = 17.68 g
 * (shortfall 0.32 g > PULSE_MIN_G 0.15 g).
 */
void test_pulse_fires_on_aggressive_offset(void)
{
    g.offset = 0.5f;
    sim_start(&g, 18.0f);
    sim_run(&g, MAX_TICKS);
    TEST_ASSERT_GREATER_THAN_FLOAT(0.0f, (float)g.pulse_count);
}

/*
 * Even when a pulse fires, the final result must be within 0.5 g of
 * target (the pulse recovers most of the shortfall in one burst).
 */
void test_pulse_improves_accuracy(void)
{
    g.offset = 0.5f;
    sim_start(&g, 18.0f);
    sim_run(&g, MAX_TICKS);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 18.0f, g.result);
}

/*
 * Correction pulses must never exceed PULSE_MAX_ATTEMPTS per cycle,
 * even with a pathologically large offset that causes deep undershoot.
 */
void test_pulse_count_capped_at_max_attempts(void)
{
    g.offset = 2.0f;   /* maximum allowed offset */
    sim_start(&g, 18.0f);
    sim_run(&g, MAX_TICKS);
    TEST_ASSERT_LESS_OR_EQUAL(PULSE_MAX_ATTEMPTS, g.pulse_count);
}

/*
 * Auto-tune step: default offset produces an overshoot → offset
 * increases after the shot so the next stop fires earlier.
 */
void test_autotune_increases_offset_on_overshoot(void)
{
    float before = g.offset;   /* 0.0 from setUp */
    sim_start(&g, 18.0f);
    sim_run(&g, MAX_TICKS);
    TEST_ASSERT_GREATER_THAN_FLOAT(before, g.offset);
}

/*
 * Auto-tune step: an undershoot result must decrease the offset so the
 * next stop fires later.
 *
 * We force a raw undershoot by zeroing the flow rate so no correction
 * pulses fire and the settled weight lands well below target.
 * With offset=1.5 and no flow:
 *   coast = COAST_FALLBACK_G = 0.3
 *   stop_at = 18 - 0.3 - 1.5 = 16.2 → settled = 16.2 + 0.3 + 0.18 = 16.68
 *   result = 16.68, delta = -1.32  → offset goes down
 */
void test_autotune_decreases_offset_on_undershoot(void)
{
    g.offset = 1.5f;
    sim_start(&g, 18.0f);
    g.flow_rate_g_s = 0.0f;   /* disable pulses; expose raw undershoot to autotune */
    float before = 1.5f;
    sim_run(&g, MAX_TICKS);
    TEST_ASSERT_LESS_THAN_FLOAT(before, g.offset);
}

/*
 * After repeated shots auto-tune converges: the result must land within
 * AUTOTUNE_DEADBAND of the target.
 *
 * With DEMO_OVERSHOOT_G=0.18 and AUTOTUNE_DEADBAND=0.1 the offset
 * settles at ~0.09 after just two shots and stays there.
 */
void test_autotune_converges_over_shots(void)
{
    for (int shot = 0; shot < 10; shot++) {
        sim_start(&g, 18.0f);
        sim_run(&g, MAX_TICKS);
        /* g.offset persists — sim_start does NOT reset it */
    }
    TEST_ASSERT_FLOAT_WITHIN(AUTOTUNE_DEADBAND, 18.0f, g.result);
}

/*
 * The stop threshold must always be strictly below the target weight
 * (coast_g > 0 in all valid configurations).
 */
void test_stop_threshold_below_target(void)
{
    float flow    = DEMO_RAMP_G_PER_SEC;
    float coast_g = (g.motor_latency_ms / 1000.0f) * flow * COAST_RATIO;
    float stop_at = 18.0f - coast_g - g.offset;
    TEST_ASSERT_LESS_THAN_FLOAT(18.0f, stop_at);
}

/*
 * Higher flow rate → larger coast distance → SSR cuts off at a lower
 * weight reading.  Verify weight_at_ssr_off decreases as flow increases.
 */
void test_higher_flow_cuts_off_earlier(void)
{
    /* Slow flow */
    sim_start(&g, 18.0f);
    g.flow_rate_g_s = 1.0f;
    sim_run(&g, MAX_TICKS);
    float ssr_slow = g.weight_at_ssr_off;

    /* Fast flow */
    setUp();
    sim_start(&g, 18.0f);
    g.flow_rate_g_s = 5.0f;
    sim_run(&g, MAX_TICKS);
    float ssr_fast = g.weight_at_ssr_off;

    TEST_ASSERT_LESS_THAN_FLOAT(ssr_slow, ssr_fast);
}

/*
 * The result weight must always be positive (the grinder never removes
 * coffee from the portafilter).
 */
void test_result_is_positive(void)
{
    sim_start(&g, 18.0f);
    sim_run(&g, MAX_TICKS);
    TEST_ASSERT_GREATER_THAN_FLOAT(0.0f, g.result);
}

/*
 * Longer motor latency → more coast → SSR cuts off earlier, so
 * weight_at_ssr_off is lower with high latency than with low latency.
 */
void test_higher_latency_cuts_off_earlier(void)
{
    sim_start(&g, 18.0f);
    g.motor_latency_ms = 50.0f;   /* override after sim_start resets to default */
    sim_run(&g, MAX_TICKS);
    float ssr_short = g.weight_at_ssr_off;

    setUp();
    sim_start(&g, 18.0f);
    g.motor_latency_ms = 200.0f;  /* override after sim_start resets to default */
    sim_run(&g, MAX_TICKS);
    float ssr_long = g.weight_at_ssr_off;

    TEST_ASSERT_LESS_THAN_FLOAT(ssr_short, ssr_long);
}

/* ── Runner ─────────────────────────────────────────────────── */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_grind_18g_completes);
    RUN_TEST(test_grind_20g_completes);
    RUN_TEST(test_grind_12g_completes);
    RUN_TEST(test_ssr_cuts_off_before_target);
    RUN_TEST(test_grind_finishes_in_finite_ticks);
    RUN_TEST(test_no_pulse_on_default_offset);
    RUN_TEST(test_pulse_fires_on_aggressive_offset);
    RUN_TEST(test_pulse_improves_accuracy);
    RUN_TEST(test_pulse_count_capped_at_max_attempts);
    RUN_TEST(test_autotune_increases_offset_on_overshoot);
    RUN_TEST(test_autotune_decreases_offset_on_undershoot);
    RUN_TEST(test_autotune_converges_over_shots);
    RUN_TEST(test_stop_threshold_below_target);
    RUN_TEST(test_higher_flow_cuts_off_earlier);
    RUN_TEST(test_result_is_positive);
    RUN_TEST(test_higher_latency_cuts_off_earlier);

    return UNITY_END();
}
