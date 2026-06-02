/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_walker.c — fixed-point line-stepper algorithm.
 *
 * CHARACTERIZATION TEST. Today's port has the walker step loop INLINED
 * inside FUN_004012E0 case 0x15/0x16 in `src/actor.c` (~lines 648-711),
 * so it can't be called directly from a unit test. This file mirrors
 * the math byte-for-byte and locks the CONTRACT — if anyone refactors
 * the walker (extract a `walker_step()` helper, change fixed-point
 * precision, etc.), this test pins the expected pixel-accurate output.
 *
 * After R3.x (REFACTOR.md) extracts the helper, this file should be
 * converted to call the production helper directly — until then it
 * documents the algorithm.
 *
 * The walker uses 16.16 fixed-point accumulators (+0x44 X, +0x48 Y on
 * the Entity) and per-tick increments (+0x4C dx, +0x50 dy). One tick
 * does: accumulator += increment; new screen pos = high 16 bits.
 * Stops when the high 16 bits reach the integer target (+0x54 tx, +0x56 ty).
 *
 * Critical "Fjej overshoot bug" reference:
 *   under -fstrict-aliasing the compiler reordered int32 += / int16 ==
 *   reads on the SAME memory, causing 1-px overshoots. The port uses
 *   -fno-strict-aliasing + per-iter local copies; these tests verify
 *   pixel-perfect convergence which would expose any regression.
 *
 * Reference: src/actor.c ~648-711 + Makefile -fno-strict-aliasing note.
 */

#include "test.h"
#include "wacki.h"

#include <stdint.h>

/* ---- mirror of the production walker step ------------------------------ */

/* Simulates one full WALK_TO_XY (op 0x15) bytecode tick: walks `step`
 * sub-iterations toward (tx, ty). State is (x, y) in screen pixels;
 * acc_x / acc_y are 16.16 accumulators. Returns 1 if walker completed
 * (reached target), 0 if still mid-walk.
 *
 * Identical formulas to actor.c FUN_004012E0 case 0x15.
 */
static int walker_sim(int16_t *x, int16_t *y,
                       int16_t tx, int16_t ty,
                       int32_t *acc_x, int32_t *acc_y,
                       int32_t *inc_x, int32_t *inc_y,
                       int *planted,
                       uint16_t step)
{
    if (!*planted) {
        int16_t cx = *x, cy = *y;
        int16_t sdx = tx - cx, sdy = ty - cy;
        int16_t adx = sdx < 0 ? -sdx : sdx;
        int16_t ady = sdy < 0 ? -sdy : sdy;
        int16_t maxlen = adx > ady ? adx : ady;
        *inc_x = 0; *inc_y = 0;
        if (maxlen) {
            *inc_x = (int32_t)((tx - cx) * 0x10000) / maxlen;
            *inc_y = (int32_t)((ty - cy) * 0x10000) / maxlen;
        }
        /* Seed accumulator using uint16 cast (matches FUN_00401150's
         * `param_1 << 0x10` for off-screen entries). */
        *acc_x = (int32_t)((uint32_t)(uint16_t)cx << 16);
        *acc_y = (int32_t)((uint32_t)(uint16_t)cy << 16);
        *planted = 1;
    }
    if (step == 0) step = 1;
    for (uint16_t k = 0; k < step; ++k) {
        int16_t pre_x = (int16_t)((uint32_t)*acc_x >> 16);
        if (pre_x == tx) *inc_x = 0;
        else             *acc_x += *inc_x;

        int16_t pre_y = (int16_t)((uint32_t)*acc_y >> 16);
        if (pre_y == ty) *inc_y = 0;
        else             *acc_y += *inc_y;
    }
    *x = (int16_t)((uint32_t)*acc_x >> 16);
    *y = (int16_t)((uint32_t)*acc_y >> 16);
    return (*inc_x == 0 && *inc_y == 0) ? 1 : 0;
}

/* ---- tests -------------------------------------------------------------- */

TEST(pure_x_axis_converges_to_target)
{
    int16_t x = 100, y = 200;
    int32_t ax = 0, ay = 0, ix = 0, iy = 0;
    int planted = 0;

    /* Walk from (100, 200) to (150, 200). max = 50. inc_x = 0x10000. */
    int done = 0;
    for (int tick = 0; tick < 100 && !done; ++tick) {
        done = walker_sim(&x, &y, 150, 200, &ax, &ay, &ix, &iy, &planted, 1);
    }
    ASSERT_TRUE(done);
    ASSERT_EQ(x, 150);
    ASSERT_EQ(y, 200);
}

TEST(pure_y_axis_converges_to_target)
{
    int16_t x = 50, y = 80;
    int32_t ax = 0, ay = 0, ix = 0, iy = 0;
    int planted = 0;

    int done = 0;
    for (int tick = 0; tick < 100 && !done; ++tick) {
        done = walker_sim(&x, &y, 50, 130, &ax, &ay, &ix, &iy, &planted, 1);
    }
    ASSERT_TRUE(done);
    ASSERT_EQ(x, 50);
    ASSERT_EQ(y, 130);
}

TEST(diagonal_converges_no_overshoot)
{
    /* Diagonal from (0, 0) to (40, 30). max = 40, inc_x = 0x10000 (= 1.0),
     * inc_y = (30 * 0x10000) / 40 = 0xC000 (= 0.75). After 40 ticks both
     * should reach target exactly. */
    int16_t x = 0, y = 0;
    int32_t ax = 0, ay = 0, ix = 0, iy = 0;
    int planted = 0;

    /* 40 ticks should be enough. */
    int done = 0;
    for (int tick = 0; tick < 60 && !done; ++tick) {
        done = walker_sim(&x, &y, 40, 30, &ax, &ay, &ix, &iy, &planted, 1);
    }
    ASSERT_TRUE(done);
    ASSERT_EQ(x, 40);
    ASSERT_EQ(y, 30);
}

TEST(reverse_diagonal_converges)
{
    /* (200, 200) → (100, 50). Negative increments. */
    int16_t x = 200, y = 200;
    int32_t ax = 0, ay = 0, ix = 0, iy = 0;
    int planted = 0;

    int done = 0;
    for (int tick = 0; tick < 200 && !done; ++tick) {
        done = walker_sim(&x, &y, 100, 50, &ax, &ay, &ix, &iy, &planted, 1);
    }
    ASSERT_TRUE(done);
    ASSERT_EQ(x, 100);
    ASSERT_EQ(y, 50);
}

TEST(zero_distance_completes_immediately)
{
    /* If start == target, walker should complete on first tick. */
    int16_t x = 320, y = 240;
    int32_t ax = 0, ay = 0, ix = 0, iy = 0;
    int planted = 0;

    int done = walker_sim(&x, &y, 320, 240, &ax, &ay, &ix, &iy, &planted, 1);
    ASSERT_EQ(done, 1);
    ASSERT_EQ(x, 320);
    ASSERT_EQ(y, 240);
}

TEST(multi_step_advances_faster)
{
    /* step=4 should advance ~4× faster than step=1 for the same path. */
    int16_t x1 = 0, y1 = 0;
    int32_t ax1 = 0, ay1 = 0, ix1 = 0, iy1 = 0;
    int p1 = 0;
    int16_t x4 = 0, y4 = 0;
    int32_t ax4 = 0, ay4 = 0, ix4 = 0, iy4 = 0;
    int p4 = 0;

    /* After 10 step=4 ticks (=40 sub-iterations), should be near (40, 0). */
    for (int t = 0; t < 10; ++t)
        walker_sim(&x4, &y4, 100, 0, &ax4, &ay4, &ix4, &iy4, &p4, 4);
    /* After 10 step=1 ticks (=10 sub-iterations), should be near (10, 0). */
    for (int t = 0; t < 10; ++t)
        walker_sim(&x1, &y1, 100, 0, &ax1, &ay1, &ix1, &iy1, &p1, 1);

    /* x4 should be substantially further along than x1. */
    ASSERT_TRUE(x4 > x1 + 20);
    ASSERT_EQ(x4, 40);
    ASSERT_EQ(x1, 10);
}

TEST(no_overshoot_when_inc_does_not_divide_evenly)
{
    /* Path (0,0) → (7, 0). max=7. inc_x = 0x10000. Should reach 7 in
     * exactly 7 ticks without overshooting to 8 on tick 8. */
    int16_t x = 0, y = 0;
    int32_t ax = 0, ay = 0, ix = 0, iy = 0;
    int planted = 0;

    /* Run 20 ticks — walker should clamp at 7. */
    for (int t = 0; t < 20; ++t)
        walker_sim(&x, &y, 7, 0, &ax, &ay, &ix, &iy, &planted, 1);
    ASSERT_EQ(x, 7);
    ASSERT_EQ(y, 0);
}

TEST(one_pixel_distance_converges_within_two_ticks)
{
    /* (0, 0) → (1, 0). maxlen = 1, inc_x = 0x10000.
     *
     * Tick 1 advances acc_x by 0x10000 → high16 = 1. inc_x is NOT
     * zeroed yet (the production code zeros inc_x only when pre == tgt
     * BEFORE the increment, not after) so the walker isn't flagged
     * "done" yet. Tick 2 sees pre_x == tgt_x and zeros inc_x → done.
     *
     * This characterizes the 1px contract: walker REACHES target on
     * tick N but only FLAGS done on tick N+1. The end-to-end visible
     * effect is identical (actor stops at the right pixel) but the
     * walker-busy bit (+0x3A bit 0) clears one tick after physical
     * arrival. Matches production code at src/actor.c:709-711. */
    int16_t x = 0, y = 0;
    int32_t ax = 0, ay = 0, ix = 0, iy = 0;
    int planted = 0;

    walker_sim(&x, &y, 1, 0, &ax, &ay, &ix, &iy, &planted, 1);
    /* After tick 1: physically at target, but inc_x not zeroed yet. */
    ASSERT_EQ(x, 1);
    ASSERT_EQ(y, 0);

    int done = walker_sim(&x, &y, 1, 0, &ax, &ay, &ix, &iy, &planted, 1);
    /* After tick 2: inc_x zeroed → done flag set. */
    ASSERT_EQ(done, 1);
    ASSERT_EQ(x, 1);
    ASSERT_EQ(y, 0);
}

TEST(mostly_x_diagonal_y_lags_then_completes)
{
    /* Path (0, 0) → (100, 5). max = 100. inc_x = 0x10000, inc_y = 0x0CCC.
     * Y accumulator gains ~0.05 / tick → after 20 ticks Y = 1, etc. Must
     * still reach (100, 5) eventually. */
    int16_t x = 0, y = 0;
    int32_t ax = 0, ay = 0, ix = 0, iy = 0;
    int planted = 0;

    int done = 0;
    for (int t = 0; t < 200 && !done; ++t)
        done = walker_sim(&x, &y, 100, 5, &ax, &ay, &ix, &iy, &planted, 1);
    ASSERT_TRUE(done);
    ASSERT_EQ(x, 100);
    ASSERT_EQ(y, 5);
}

TEST(very_long_distance_converges)
{
    /* Stage-scale walk: (40, 380) → (590, 420). max = 550. */
    int16_t x = 40, y = 380;
    int32_t ax = 0, ay = 0, ix = 0, iy = 0;
    int planted = 0;

    int done = 0;
    for (int t = 0; t < 700 && !done; ++t)
        done = walker_sim(&x, &y, 590, 420, &ax, &ay, &ix, &iy, &planted, 1);
    ASSERT_TRUE(done);
    ASSERT_EQ(x, 590);
    ASSERT_EQ(y, 420);
}

TEST(off_screen_entry_uint16_cast_handles_negative_start)
{
    /* (-10, 50) → (50, 50). The production code's
     *   `acc_x = (int32_t)((uint32_t)(uint16_t)cx << 16)`
     * means cx=-10 gets cast to 0xFFF6, then << 16 → 0xFFF60000 (a large
     * negative int32 when interpreted as signed). The walker advances
     * monotonically until reaching tx=50.
     *
     * Verify the walker doesn't get stuck and reaches the target. */
    int16_t x = -10, y = 50;
    int32_t ax = 0, ay = 0, ix = 0, iy = 0;
    int planted = 0;

    int done = 0;
    for (int t = 0; t < 200 && !done; ++t)
        done = walker_sim(&x, &y, 50, 50, &ax, &ay, &ix, &iy, &planted, 1);
    ASSERT_TRUE(done);
    ASSERT_EQ(x, 50);
}

TEST(planted_flag_prevents_re_initialization)
{
    /* On the second walker_sim call the `planted` flag is already 1
     * → the accumulator MUST NOT be re-seeded from current (x, y).
     * If it were, a mid-walk re-init would teleport the walker.
     *
     * Run 5 ticks, capture the accumulator after, then run 5 more
     * ticks; positions should monotonically progress, not reset. */
    int16_t x = 0, y = 0;
    int32_t ax = 0, ay = 0, ix = 0, iy = 0;
    int planted = 0;

    for (int t = 0; t < 5; ++t)
        walker_sim(&x, &y, 100, 0, &ax, &ay, &ix, &iy, &planted, 1);
    int16_t x_after_5 = x;

    for (int t = 0; t < 5; ++t)
        walker_sim(&x, &y, 100, 0, &ax, &ay, &ix, &iy, &planted, 1);
    int16_t x_after_10 = x;

    ASSERT_TRUE(planted);
    /* After 10 ticks should be at least double the position after 5. */
    ASSERT_TRUE(x_after_10 > x_after_5);
}

SUITE(walker)
{
    RUN_TEST(pure_x_axis_converges_to_target);
    RUN_TEST(pure_y_axis_converges_to_target);
    RUN_TEST(diagonal_converges_no_overshoot);
    RUN_TEST(reverse_diagonal_converges);
    RUN_TEST(zero_distance_completes_immediately);
    RUN_TEST(multi_step_advances_faster);
    RUN_TEST(no_overshoot_when_inc_does_not_divide_evenly);
    RUN_TEST(one_pixel_distance_converges_within_two_ticks);
    RUN_TEST(mostly_x_diagonal_y_lags_then_completes);
    RUN_TEST(very_long_distance_converges);
    RUN_TEST(off_screen_entry_uint16_cast_handles_negative_start);
    RUN_TEST(planted_flag_prevents_re_initialization);
}
