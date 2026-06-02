/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_click_queue.c — deferred click-event queue.
 *
 * Per-entity VM op 0x22 doesn't dispatch click events synchronously
 * (entity-list mutations mid-iteration would corrupt the walker).
 * Instead it enqueues (obj, verb) pairs via EnqueueClickEvent;
 * FlushQueuedClicks drains them at the end of ProcessGameFrameTick
 * by invoking DispatchClickEvent on each.
 *
 * The queue is 1:1 with original DAT_0044A1A0 (slot array) and
 * DAT_0044A1C8 (count). Size cap is 10 entries; overflow silently
 * drops (matches Ghidra's bounded array semantics).
 *
 * Refactor-safety: any rework of EntityWalkerTick / ProcessGameFrameTick
 * must preserve the enqueue → drain ordering. If the drain runs DURING
 * entity iteration, scripts mutating the entity list crash the walker.
 *
 * Reference: src/stubs.c:1298 (EnqueueClickEvent), :1306 (FlushQueuedClicks).
 */

#include "test.h"
#include "wacki.h"
#include "test_engine_stubs.h"

#include <stdint.h>
#include <string.h>

extern void EnqueueClickEvent(uint16_t obj, uint16_t verb);
extern void FlushQueuedClicks(void);

/* Capture (declared in test_engine_stubs.c). */
#define DISPATCH_CAPTURE_MAX 16
extern uint16_t g_stub_dispatch_obj [DISPATCH_CAPTURE_MAX];
extern uint16_t g_stub_dispatch_verb[DISPATCH_CAPTURE_MAX];
extern int      g_stub_dispatch_count;

static void reset_capture(void)
{
    memset(g_stub_dispatch_obj,  0, sizeof g_stub_dispatch_obj);
    memset(g_stub_dispatch_verb, 0, sizeof g_stub_dispatch_verb);
    g_stub_dispatch_count = 0;
    /* Drain any leftover queue from a prior test. */
    FlushQueuedClicks();
    g_stub_dispatch_count = 0;
    memset(g_stub_dispatch_obj,  0, sizeof g_stub_dispatch_obj);
    memset(g_stub_dispatch_verb, 0, sizeof g_stub_dispatch_verb);
}

/* ---- basic enqueue + flush ------------------------------------------ */

TEST(queue_flush_empty_dispatches_nothing)
{
    reset_capture();
    FlushQueuedClicks();
    ASSERT_EQ(g_stub_dispatch_count, 0);
}

TEST(queue_single_enqueue_flush_dispatches_once)
{
    reset_capture();
    EnqueueClickEvent(/*obj=*/0x42, /*verb=*/0x05);
    /* Before flush: dispatch must not have fired. */
    ASSERT_EQ(g_stub_dispatch_count, 0);

    FlushQueuedClicks();
    ASSERT_EQ(g_stub_dispatch_count, 1);
    ASSERT_EQ(g_stub_dispatch_obj [0], 0x42);
    ASSERT_EQ(g_stub_dispatch_verb[0], 0x05);
}

TEST(queue_multiple_drain_in_fifo_order)
{
    /* FIFO drain (1:1 with original tail loop: for i in 0..count). */
    reset_capture();
    EnqueueClickEvent(0xAA, 1);
    EnqueueClickEvent(0xBB, 2);
    EnqueueClickEvent(0xCC, 3);

    FlushQueuedClicks();
    ASSERT_EQ(g_stub_dispatch_count, 3);
    ASSERT_EQ(g_stub_dispatch_obj [0], 0xAA);
    ASSERT_EQ(g_stub_dispatch_verb[0], 1);
    ASSERT_EQ(g_stub_dispatch_obj [1], 0xBB);
    ASSERT_EQ(g_stub_dispatch_verb[1], 2);
    ASSERT_EQ(g_stub_dispatch_obj [2], 0xCC);
    ASSERT_EQ(g_stub_dispatch_verb[2], 3);
}

/* ---- queue resets to empty after flush ------------------------------ */

TEST(queue_resets_to_empty_after_flush)
{
    reset_capture();
    EnqueueClickEvent(0x11, 1);
    EnqueueClickEvent(0x22, 2);
    FlushQueuedClicks();
    ASSERT_EQ(g_stub_dispatch_count, 2);

    /* Second flush must dispatch NOTHING — queue was zeroed. */
    g_stub_dispatch_count = 0;
    FlushQueuedClicks();
    ASSERT_EQ(g_stub_dispatch_count, 0);
}

TEST(queue_reusable_across_frames)
{
    /* Frame 1: enqueue + flush. Frame 2: enqueue different events + flush.
     * The drain pattern repeats indefinitely. */
    reset_capture();
    EnqueueClickEvent(0xF1, 0x10);
    FlushQueuedClicks();
    ASSERT_EQ(g_stub_dispatch_obj[0], 0xF1);

    /* Reset capture (simulating new frame) and run a new event. */
    g_stub_dispatch_count = 0;
    EnqueueClickEvent(0xF2, 0x20);
    FlushQueuedClicks();
    ASSERT_EQ(g_stub_dispatch_count, 1);
    ASSERT_EQ(g_stub_dispatch_obj[0], 0xF2);
    ASSERT_EQ(g_stub_dispatch_verb[0], 0x20);
}

/* ---- overflow guard (CLICK_QUEUE_MAX = 10) -------------------------- */

TEST(queue_caps_at_10_slots_extras_dropped)
{
    /* First 10 enqueued; 11th+ silently dropped. */
    reset_capture();
    for (int i = 0; i < 15; ++i) {
        EnqueueClickEvent(/*obj=*/(uint16_t)(0x100 + i), /*verb=*/(uint16_t)i);
    }
    FlushQueuedClicks();

    /* Exactly 10 dispatches — overflow events 10..14 dropped. */
    ASSERT_EQ(g_stub_dispatch_count, 10);
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(g_stub_dispatch_obj [i], (uint16_t)(0x100 + i));
        ASSERT_EQ(g_stub_dispatch_verb[i], (uint16_t)i);
    }
}

TEST(queue_overflow_then_new_frame_works)
{
    /* After overflow + flush, queue is empty again so a new frame
     * can re-fill from 0. */
    reset_capture();
    for (int i = 0; i < 15; ++i) {
        EnqueueClickEvent((uint16_t)(0x200 + i), (uint16_t)i);
    }
    FlushQueuedClicks();
    ASSERT_EQ(g_stub_dispatch_count, 10);

    /* Next frame. */
    g_stub_dispatch_count = 0;
    EnqueueClickEvent(0x999, 0x55);
    FlushQueuedClicks();
    ASSERT_EQ(g_stub_dispatch_count, 1);
    ASSERT_EQ(g_stub_dispatch_obj [0], 0x999);
    ASSERT_EQ(g_stub_dispatch_verb[0], 0x55);
}

/* ---- enqueue does NOT call DispatchClickEvent ----------------------- */

TEST(queue_enqueue_alone_does_not_dispatch)
{
    /* Critical invariant: enqueue is async, dispatch happens ONLY in
     * flush. If a refactor moves dispatch into enqueue, scripts
     * mutating the entity list mid-walker-tick would crash. */
    reset_capture();
    for (int i = 0; i < 5; ++i) {
        EnqueueClickEvent((uint16_t)i, (uint16_t)i);
    }
    /* No flush — dispatch count must still be 0. */
    ASSERT_EQ(g_stub_dispatch_count, 0);

    /* Cleanup so next test doesn't see leftover queue. */
    FlushQueuedClicks();
}

SUITE(click_queue)
{
    RUN_TEST(queue_flush_empty_dispatches_nothing);
    RUN_TEST(queue_single_enqueue_flush_dispatches_once);
    RUN_TEST(queue_multiple_drain_in_fifo_order);
    RUN_TEST(queue_resets_to_empty_after_flush);
    RUN_TEST(queue_reusable_across_frames);
    RUN_TEST(queue_caps_at_10_slots_extras_dropped);
    RUN_TEST(queue_overflow_then_new_frame_works);
    RUN_TEST(queue_enqueue_alone_does_not_dispatch);
}
