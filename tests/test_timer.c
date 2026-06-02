/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_timer.c — multimedia timer stubs.
 *
 * src/timer.c is the SDL-build no-op wrapper for the original engine's
 * winmm timer (timeGetDevCaps + timeBeginPeriod + timeSetEvent). The
 * port lets SDL drive frame pacing instead; these functions exist only
 * to keep the original call sites linking cleanly.
 *
 * The tests verify the stubs' documented return values so that nobody
 * "accidentally helpful" wires in real timer logic that would change
 * the demo's tick cadence (which would cascade into RNG sync drift in
 * the smoke harness).
 *
 * Reference: src/timer.c.
 */

#include "test.h"

#include <stdint.h>

extern int InitializeMmTimer(void *self_);
extern int ArmPeriodicCallback(void *self_, uint32_t period_ms,
                                uint32_t flags, void (*fn)(void));

static int s_cb_called;
static void tick_cb(void) { ++s_cb_called; }

TEST(init_mm_timer_returns_one)
{
    /* Stub returns 1 — caller code does `if (!InitializeMmTimer(...))
     * abort()`. Make sure that stays. */
    ASSERT_EQ(InitializeMmTimer(NULL), 1);
    int dummy;
    ASSERT_EQ(InitializeMmTimer(&dummy), 1);
}

TEST(arm_periodic_returns_zero_and_does_not_call_cb)
{
    /* Stub returns 0 (no timer armed) and MUST NOT call the callback —
     * otherwise the original-engine call sites that pass interrupt-time
     * tick functions get invoked from the wrong context. */
    s_cb_called = 0;
    int rc = ArmPeriodicCallback(NULL, 50, 0, tick_cb);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(s_cb_called, 0);
}

TEST(arm_periodic_null_callback_safe)
{
    /* Stub must not crash on a null fn pointer. */
    int rc = ArmPeriodicCallback(NULL, 33, 0, NULL);
    ASSERT_EQ(rc, 0);
}

SUITE(timer)
{
    RUN_TEST(init_mm_timer_returns_one);
    RUN_TEST(arm_periodic_returns_zero_and_does_not_call_cb);
    RUN_TEST(arm_periodic_null_callback_safe);
}
