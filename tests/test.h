/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test.h — minimal header-only test framework for wacki-src.
 *
 * Why custom (and not Unity / cmocka / criterion):
 *  - Zero external deps — same philosophy as the rest of the port
 *    (only SDL2 is allowed in the engine build; tests don't link SDL2
 *    so we want zero deps period).
 *  - Single header, single TU per test file — predictable build.
 *  - Survives -Wpedantic / -fsanitize=address,undefined cleanly.
 *  - 100 LOC instead of vendor-ing 2-5 KLOC.
 *
 * Usage in a test file:
 *
 *     #include "test.h"
 *
 *     TEST(my_thing_does_what_it_should) {
 *         ASSERT_EQ(2 + 2, 4);
 *         ASSERT_TRUE(strstr("hello world", "world"));
 *         ASSERT_STREQ("abc", "abc");
 *         ASSERT_MEMEQ(a, b, 16);
 *     }
 *
 *     SUITE(my_suite) {
 *         RUN_TEST(my_thing_does_what_it_should);
 *     }
 *
 * Then in tests/test_main.c add `extern void run_suite_my_suite(int *, int *);`
 * and call it from main(). The suite returns (passed, failed) counts via
 * the two out-pointers.
 *
 * Assertion macros print location + values on failure and call longjmp
 * back to the test runner so a single ASSERT_* failure aborts just that
 * test, not the whole binary. Subsequent tests still run.
 */

#ifndef WACKI_TEST_H
#define WACKI_TEST_H

#include <inttypes.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- runner internals ---------------------------------------------------- */

extern jmp_buf g_test_jmp;        /* set up by RUN_TEST, jumped to on fail */
extern const char *g_test_name;   /* current test name, for error prints */
extern int g_test_failed;         /* incremented per failed test */
extern int g_test_passed;         /* incremented per passed test */

/* The runner's longjmp value is irrelevant; we only care that we abort. */
#define TEST_JMP_VAL 1

/* ---- TEST / SUITE / RUN_TEST -------------------------------------------- */

/* A TEST(name) declares `static void test_<name>(void)`. */
#define TEST(name)  static void test_##name(void)

/* SUITE(name) opens `void run_suite_<name>(int *passed_out, int *failed_out)`.
 * The body lists RUN_TEST(...) calls. */
#define SUITE(name)                                                            \
    void run_suite_##name(int *passed_out, int *failed_out);                   \
    void run_suite_##name(int *passed_out, int *failed_out)

/* RUN_TEST sets the longjmp target, calls the test, then bumps counters. */
#define RUN_TEST(name)                                                         \
    do {                                                                       \
        g_test_name = #name;                                                   \
        if (setjmp(g_test_jmp) == 0) {                                         \
            test_##name();                                                     \
            ++g_test_passed;                                                   \
            if (passed_out) ++*passed_out;                                     \
            fprintf(stdout, "  \033[32mPASS\033[0m  %s\n", #name);             \
        } else {                                                               \
            ++g_test_failed;                                                   \
            if (failed_out) ++*failed_out;                                     \
        }                                                                      \
    } while (0)

/* ---- ASSERT macros ------------------------------------------------------ */

#define TEST_FAIL_(fmt, ...)                                                   \
    do {                                                                      \
        fprintf(stderr,                                                       \
                "  \033[31mFAIL\033[0m  %s  (%s:%d)\n         " fmt "\n",     \
                g_test_name, __FILE__, __LINE__, __VA_ARGS__);                \
        longjmp(g_test_jmp, TEST_JMP_VAL);                                    \
    } while (0)

#define ASSERT_TRUE(expr)                                                      \
    do {                                                                      \
        if (!(expr)) TEST_FAIL_("expected true: %s", #expr);                  \
    } while (0)

#define ASSERT_FALSE(expr)                                                     \
    do {                                                                      \
        if ((expr)) TEST_FAIL_("expected false: %s", #expr);                  \
    } while (0)

#define ASSERT_EQ(a, b)                                                        \
    do {                                                                      \
        long long _av = (long long)(a);                                       \
        long long _bv = (long long)(b);                                       \
        if (_av != _bv)                                                       \
            TEST_FAIL_("%s == %s  (got %lld, want %lld)",                     \
                       #a, #b, _av, _bv);                                     \
    } while (0)

#define ASSERT_NE(a, b)                                                        \
    do {                                                                      \
        long long _av = (long long)(a);                                       \
        long long _bv = (long long)(b);                                       \
        if (_av == _bv)                                                       \
            TEST_FAIL_("%s != %s  (both = %lld)", #a, #b, _av);               \
    } while (0)

#define ASSERT_STREQ(a, b)                                                     \
    do {                                                                      \
        const char *_a = (a);                                                 \
        const char *_b = (b);                                                 \
        if (!_a || !_b || strcmp(_a, _b) != 0)                                \
            TEST_FAIL_("strcmp(%s, %s)  (got \"%s\", want \"%s\")",           \
                       #a, #b, _a ? _a : "(null)", _b ? _b : "(null)");      \
    } while (0)

#define ASSERT_MEMEQ(a, b, n)                                                  \
    do {                                                                      \
        const void *_a = (a);                                                 \
        const void *_b = (b);                                                 \
        size_t _n = (size_t)(n);                                              \
        if (memcmp(_a, _b, _n) != 0) {                                        \
            /* On mismatch find first differing byte to aid debugging. */    \
            size_t _i;                                                        \
            const unsigned char *_pa = (const unsigned char *)_a;             \
            const unsigned char *_pb = (const unsigned char *)_b;             \
            for (_i = 0; _i < _n && _pa[_i] == _pb[_i]; ++_i) { /*scan*/ }   \
            TEST_FAIL_("memcmp(%s, %s, %zu) differs at byte %zu "             \
                       "(got 0x%02X, want 0x%02X)",                           \
                       #a, #b, _n, _i, _pa[_i], _pb[_i]);                    \
        }                                                                     \
    } while (0)

#define ASSERT_NULL(p)                                                         \
    do {                                                                      \
        const void *_p = (p);                                                 \
        if (_p != NULL) TEST_FAIL_("expected NULL: %s (= %p)", #p, _p);       \
    } while (0)

#define ASSERT_NOT_NULL(p)                                                     \
    do {                                                                      \
        const void *_p = (p);                                                 \
        if (_p == NULL) TEST_FAIL_("expected non-NULL: %s", #p);              \
    } while (0)

/* POSIX mkdir takes two args (path, mode); mingw's variant takes only
 * the path. Tests that need to spin up a temp directory go through
 * test_mkdir() so they stay portable. */
#ifdef _WIN32
#  include <direct.h>
#  define test_mkdir(path)  _mkdir(path)
#else
#  include <sys/stat.h>
#  define test_mkdir(path)  mkdir((path), 0700)
#endif

#endif /* WACKI_TEST_H */
