#include "../unity/unity.h"
#include "weight_progress.h"

void setUp(void) {}
void tearDown(void) {}

static void zero_and_negative_readings_time_out(void)
{
    weight_progress_t p = {0.0f, 100};
    TEST_ASSERT_EQUAL_INT(0, weight_progress_expired(&p, 0.0f, 3099));
    TEST_ASSERT_EQUAL_INT(1, weight_progress_expired(&p, 0.0f, 3100));
    p.last_progress_ms = 100;
    TEST_ASSERT_EQUAL_INT(1, weight_progress_expired(&p, -4.0f, 3100));
}

static void rising_weight_extends_deadline_but_stall_stops(void)
{
    weight_progress_t p = {0.0f, 0};
    TEST_ASSERT_EQUAL_INT(0, weight_progress_expired(&p, 1.0f, 2500));
    TEST_ASSERT_EQUAL_INT(0, weight_progress_expired(&p, 2.0f, 5000));
    TEST_ASSERT_EQUAL_INT(0, weight_progress_expired(&p, 2.0f, 7999));
    TEST_ASSERT_EQUAL_INT(1, weight_progress_expired(&p, 2.0f, 8000));
}

static void noise_and_rebounds_do_not_count_as_progress(void)
{
    weight_progress_t p = {0.0f, 0};
    weight_progress_expired(&p, 0.1f, 1000);
    weight_progress_expired(&p, -0.1f, 2000);
    TEST_ASSERT_EQUAL_INT(1, weight_progress_expired(&p, 0.1f, 3000));
    p.weight = 2.0f;
    p.last_progress_ms = 0;
    weight_progress_expired(&p, 1.0f, 2000);
    TEST_ASSERT_EQUAL_INT(1, weight_progress_expired(&p, 2.0f, 3000));
}

static void clock_wrap_is_handled(void)
{
    weight_progress_t p = {0.0f, UINT32_MAX - 999u};
    TEST_ASSERT_EQUAL_INT(0, weight_progress_expired(&p, 0.0f, 1999));
    TEST_ASSERT_EQUAL_INT(1, weight_progress_expired(&p, 0.0f, 2000));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(zero_and_negative_readings_time_out);
    RUN_TEST(rising_weight_extends_deadline_but_stall_stops);
    RUN_TEST(noise_and_rebounds_do_not_count_as_progress);
    RUN_TEST(clock_wrap_is_handled);
    return UNITY_END();
}
