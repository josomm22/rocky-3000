/*
 * Minimal Unity-compatible test framework for GBWUI host-native tests.
 *
 * Implements the subset of Unity macros used in this project:
 *   UNITY_BEGIN / UNITY_END / RUN_TEST
 *   TEST_ASSERT_EQUAL_INT
 *   TEST_ASSERT_FLOAT_WITHIN
 *   TEST_ASSERT_GREATER_THAN_FLOAT / TEST_ASSERT_LESS_THAN_FLOAT
 *   TEST_ASSERT_NOT_EQUAL_FLOAT
 *
 * Header-only; no .c file required.
 *
 * Output format matches Unity so tooling that parses Unity output works too:
 *   test/test_foo.c:42:test_bar:PASS
 *   test/test_foo.c:55:test_baz:FAIL: Expected X but was Y
 */

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <setjmp.h>
#include <string.h>

/* ── internal state ───────────────────────────────────────── */

static int         _u_tests     = 0;
static int         _u_failures  = 0;
static const char *_u_test_name = "";
static jmp_buf     _u_jmp;

/* ── public macros ────────────────────────────────────────── */

#define UNITY_BEGIN() \
    do { _u_tests = 0; _u_failures = 0; \
         printf("\n"); } while (0)

#define UNITY_END() \
    ( printf("-----------------------\n" \
             "%d Tests %d Failures 0 Ignored\n" \
             "%s\n", _u_tests, _u_failures, _u_failures ? "FAIL" : "OK"), \
      _u_failures )

#define RUN_TEST(fn) \
    do { \
        _u_tests++; \
        _u_test_name = #fn; \
        setUp(); \
        if (setjmp(_u_jmp) == 0) { \
            fn(); \
            tearDown(); \
            printf("%s:%d:%s:PASS\n", __FILE__, __LINE__, #fn); \
        } else { \
            tearDown(); \
        } \
    } while (0)

/* ── internal failure helper ──────────────────────────────── */

/* _u_fail prints file:line:testname:FAIL: msg and long-jumps out of test */
#define _U_FAIL(file, line, msg) \
    do { \
        _u_failures++; \
        printf("%s:%d:%s:FAIL: %s\n", file, line, _u_test_name, msg); \
        longjmp(_u_jmp, 1); \
    } while (0)

/* ── assertion macros ─────────────────────────────────────── */

#define TEST_ASSERT_EQUAL_INT(expected, actual) \
    do { \
        int _e = (int)(expected); \
        int _a = (int)(actual); \
        if (_e != _a) { \
            char _msg[128]; \
            snprintf(_msg, sizeof(_msg), "Expected %d Was %d", _e, _a); \
            _U_FAIL(__FILE__, __LINE__, _msg); \
        } \
    } while (0)

#define TEST_ASSERT_FLOAT_WITHIN(delta, expected, actual) \
    do { \
        float _d = (float)(delta); \
        float _e = (float)(expected); \
        float _a = (float)(actual); \
        if (fabsf(_a - _e) > _d) { \
            char _msg[128]; \
            snprintf(_msg, sizeof(_msg), \
                     "Expected %f within +/-%f Was %f", (double)_e, (double)_d, (double)_a); \
            _U_FAIL(__FILE__, __LINE__, _msg); \
        } \
    } while (0)

#define TEST_ASSERT_GREATER_THAN_FLOAT(threshold, actual) \
    do { \
        float _t = (float)(threshold); \
        float _a = (float)(actual); \
        if (_a <= _t) { \
            char _msg[128]; \
            snprintf(_msg, sizeof(_msg), \
                     "Expected > %f Was %f", (double)_t, (double)_a); \
            _U_FAIL(__FILE__, __LINE__, _msg); \
        } \
    } while (0)

#define TEST_ASSERT_LESS_THAN_FLOAT(threshold, actual) \
    do { \
        float _t = (float)(threshold); \
        float _a = (float)(actual); \
        if (_a >= _t) { \
            char _msg[128]; \
            snprintf(_msg, sizeof(_msg), \
                     "Expected < %f Was %f", (double)_t, (double)_a); \
            _U_FAIL(__FILE__, __LINE__, _msg); \
        } \
    } while (0)

#define TEST_ASSERT_NOT_EQUAL_FLOAT(unexpected, actual) \
    do { \
        if ((float)(actual) == (float)(unexpected)) { \
            char _msg[128]; \
            snprintf(_msg, sizeof(_msg), \
                     "Expected not equal to %f", (double)(float)(unexpected)); \
            _U_FAIL(__FILE__, __LINE__, _msg); \
        } \
    } while (0)
