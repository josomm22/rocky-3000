/*
 * Unit tests for src/core/grind_history.c
 *
 * Run on host with: pio test -e native
 *
 * Coverage:
 *   - Empty init (NVS absent)
 *   - Single record
 *   - Ordering: oldest → newest
 *   - Circular wrap at HISTORY_MAX
 *   - Clear
 *   - get() with max < count
 */

#include <unity.h>
#include "grind_history.h"

/* ── helpers ──────────────────────────────────────────────── */

#define FLOAT_DELTA 0.001f

static void record_n(int n)
{
    for (int i = 0; i < n; i++)
        grind_history_record((float)(i + 1), (float)(i + 1) + 0.1f);
}

/* ── setUp / tearDown ─────────────────────────────────────── */

void setUp(void)
{
    /* Each test starts with a clean slate.
     * grind_history_clear() resets RAM state; NVS stub is always empty. */
    grind_history_clear();
    grind_history_init();   /* re-init: NVS stub returns NOT_FOUND → no-op */
}

void tearDown(void) {}

/* ── tests ────────────────────────────────────────────────── */

void test_init_empty(void)
{
    TEST_ASSERT_EQUAL_INT(0, grind_history_count());
}

void test_record_single(void)
{
    grind_history_record(18.0f, 18.2f);
    TEST_ASSERT_EQUAL_INT(1, grind_history_count());

    grind_record_t out[1];
    int n = grind_history_get(out, 1);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 18.0f, out[0].target_g);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 18.2f, out[0].result_g);
}

void test_record_order(void)
{
    /* Record A then B; get should return [A, B] */
    grind_history_record(18.0f, 18.1f);
    grind_history_record(20.0f, 20.3f);

    grind_record_t out[2];
    int n = grind_history_get(out, 2);
    TEST_ASSERT_EQUAL_INT(2, n);

    /* Oldest first */
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 18.0f, out[0].target_g);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 20.0f, out[1].target_g);
}

void test_count_fills_to_max(void)
{
    record_n(HISTORY_MAX);
    TEST_ASSERT_EQUAL_INT(HISTORY_MAX, grind_history_count());
}

void test_circular_wrap(void)
{
    /* Fill to capacity, then add one more — oldest should be overwritten */
    record_n(HISTORY_MAX);

    /* This 51st record overwrites the oldest (target=1) */
    grind_history_record(99.0f, 99.9f);

    TEST_ASSERT_EQUAL_INT(HISTORY_MAX, grind_history_count());

    grind_record_t out[HISTORY_MAX];
    int n = grind_history_get(out, HISTORY_MAX);
    TEST_ASSERT_EQUAL_INT(HISTORY_MAX, n);

    /* First entry should now be what was originally the 2nd entry (target=2) */
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 2.0f, out[0].target_g);

    /* Last entry should be the one we just added */
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 99.0f, out[HISTORY_MAX - 1].target_g);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 99.9f, out[HISTORY_MAX - 1].result_g);
}

void test_clear_resets_count(void)
{
    record_n(5);
    grind_history_clear();
    TEST_ASSERT_EQUAL_INT(0, grind_history_count());
}

void test_clear_get_returns_zero(void)
{
    record_n(5);
    grind_history_clear();

    grind_record_t out[5];
    int n = grind_history_get(out, 5);
    TEST_ASSERT_EQUAL_INT(0, n);
}

void test_get_limited(void)
{
    record_n(5);

    grind_record_t out[3];
    int n = grind_history_get(out, 3);
    TEST_ASSERT_EQUAL_INT(3, n);

    /* Should return the 3 oldest */
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 1.0f, out[0].target_g);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 2.0f, out[1].target_g);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, 3.0f, out[2].target_g);
}

void test_get_with_max_zero(void)
{
    record_n(3);
    grind_record_t out[1];
    TEST_ASSERT_EQUAL_INT(0, grind_history_get(out, 0));
}

void test_multiple_wraps(void)
{
    /* Write 3× capacity; only the last HISTORY_MAX entries should survive */
    record_n(HISTORY_MAX * 3);
    TEST_ASSERT_EQUAL_INT(HISTORY_MAX, grind_history_count());

    grind_record_t out[HISTORY_MAX];
    int n = grind_history_get(out, HISTORY_MAX);
    TEST_ASSERT_EQUAL_INT(HISTORY_MAX, n);

    /* The newest entry should be the last one recorded: target = HISTORY_MAX*3 */
    float expected_last = (float)(HISTORY_MAX * 3);
    TEST_ASSERT_FLOAT_WITHIN(FLOAT_DELTA, expected_last, out[HISTORY_MAX - 1].target_g);
}

/* ── runner ───────────────────────────────────────────────── */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_empty);
    RUN_TEST(test_record_single);
    RUN_TEST(test_record_order);
    RUN_TEST(test_count_fills_to_max);
    RUN_TEST(test_circular_wrap);
    RUN_TEST(test_clear_resets_count);
    RUN_TEST(test_clear_get_returns_zero);
    RUN_TEST(test_get_limited);
    RUN_TEST(test_get_with_max_zero);
    RUN_TEST(test_multiple_wraps);

    return UNITY_END();
}
