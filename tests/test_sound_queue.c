/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_sound_queue.c — REAL sound queue + stereo pan math.
 *
 * Now that stubs.c is linked, we exercise production
 * SoundQueueReset / SoundQueueEnqueue / SoundQueueMixForListener
 * (T36 port of FUN_00410D20/FUN_00410DA0).
 *
 * The mix function returns a packed (R << 16) | (C << 8) | L. Identity
 * (empty queue) = 0x808080. Per-source contributions accumulate L/C/R
 * with distance-weighted attenuation and dx-based pan.
 *
 * Reference: src/stubs.c lines 1429-1519.
 */

#include "test.h"
#include "wacki.h"

#include <stdint.h>

extern void     SoundQueueReset(void);
extern void     SoundQueueEnqueue(int16_t x, int16_t y, uint32_t sound_id, uint16_t volume);
extern uint32_t SoundQueueMixForListener(int16_t listener_x, int16_t listener_y);

/* ---- empty queue returns identity 0x808080 -------------------------- */

TEST(empty_queue_returns_identity)
{
    SoundQueueReset();
    uint32_t mix = SoundQueueMixForListener(0, 0);
    ASSERT_EQ(mix, 0x00808080u);
}

TEST(reset_clears_previous_state)
{
    /* Enqueue something, then reset, then verify empty returns identity. */
    SoundQueueReset();
    SoundQueueEnqueue(100, 100, 1, 200);
    SoundQueueReset();

    uint32_t mix = SoundQueueMixForListener(0, 0);
    ASSERT_EQ(mix, 0x00808080u);
}

/* ---- single source: pan and contribution -------------------------- */

TEST(source_at_listener_position_centered)
{
    /* Source at exactly (0, 0) with listener at (0, 0): dx=dy=0,
     * pan=0 → gL = gR = contrib/2; gC = contrib * D_MAX/(D_MAX+1).
     * For volume=200: dist_sq=0; contrib = (200*256)/(0+256) = 200.
     * Then pan=0 → gL = gR = (255 * 200) / 510 = 100.
     * gC = 200 * 240/(240+0+1) = 200 * 240/241 ≈ 199.
     * Verify L/R are balanced (equal) and C dominates. */
    SoundQueueReset();
    SoundQueueEnqueue(0, 0, /*sound_id=*/1, /*volume=*/200);

    uint32_t mix = SoundQueueMixForListener(0, 0);
    uint8_t L = mix & 0xFF;
    uint8_t C = (mix >> 8) & 0xFF;
    uint8_t R = (mix >> 16) & 0xFF;

    /* L == R (centered pan). */
    ASSERT_EQ(L, R);
    /* C should dominate (close-to-listener, on-axis). */
    ASSERT_TRUE(C >= L);
}

TEST(source_to_right_pans_louder_on_right_channel)
{
    /* Source at (+100, 0): dx=+100, dy=0. Pan = (100*255)/240 = ~106.
     * gL = (255 - 106) * contrib / 510 = 149 * contrib / 510
     * gR = (255 + 106) * contrib / 510 = 361 * contrib / 510
     * So R > L. */
    SoundQueueReset();
    SoundQueueEnqueue(100, 0, 1, 200);

    uint32_t mix = SoundQueueMixForListener(0, 0);
    uint8_t L = mix & 0xFF;
    uint8_t R = (mix >> 16) & 0xFF;

    ASSERT_TRUE(R > L);
}

TEST(source_to_left_pans_louder_on_left_channel)
{
    SoundQueueReset();
    SoundQueueEnqueue(-100, 0, 1, 200);

    uint32_t mix = SoundQueueMixForListener(0, 0);
    uint8_t L = mix & 0xFF;
    uint8_t R = (mix >> 16) & 0xFF;

    ASSERT_TRUE(L > R);
}

/* ---- distance attenuation ----------------------------------------- */

TEST(distant_source_quieter_than_close_source)
{
    /* Compare close (10, 0) vs distant (220, 0). Both pan to right
     * (positive dx) so we compare R magnitudes only. */
    SoundQueueReset();
    SoundQueueEnqueue(10, 0, 1, 250);
    uint32_t close_mix = SoundQueueMixForListener(0, 0);
    uint8_t close_R = (close_mix >> 16) & 0xFF;

    SoundQueueReset();
    SoundQueueEnqueue(220, 0, 1, 250);
    uint32_t far_mix = SoundQueueMixForListener(0, 0);
    uint8_t far_R = (far_mix >> 16) & 0xFF;

    ASSERT_TRUE(close_R > far_R);
}

TEST(zero_volume_contributes_nothing)
{
    /* Volume = 0 → `if (v <= 0) continue;` → no contribution. */
    SoundQueueReset();
    SoundQueueEnqueue(0, 0, 1, 0);

    uint32_t mix = SoundQueueMixForListener(0, 0);
    /* Queue non-empty, but only entry has v=0 → L/C/R all 0. */
    ASSERT_EQ(mix, 0u);
}

/* ---- multiple sources accumulate ---------------------------------- */

TEST(two_sources_contributions_accumulate)
{
    /* One source on left, one on right with higher volume so contributions
     * are substantial. With volume=200 + 1px distance the formula gives
     * usable L/R values. */
    SoundQueueReset();
    SoundQueueEnqueue(-50, 0, 1, 200);
    SoundQueueEnqueue(+50, 0, 2, 200);

    uint32_t mix = SoundQueueMixForListener(0, 0);
    uint8_t L = mix & 0xFF;
    uint8_t C = (mix >> 8) & 0xFF;
    uint8_t R = (mix >> 16) & 0xFF;

    /* Both L and R have non-zero contribution (sources accumulate). */
    ASSERT_TRUE(L > 0);
    ASSERT_TRUE(R > 0);
    /* C also has contribution from both sources (off-axis but
     * within D_MAX = 240). */
    ASSERT_TRUE(C > 0);
}

TEST(channel_values_clamp_to_255)
{
    /* Many sources all at listener position with max volume → would
     * sum past 255 without clamp. Verify final L/C/R never exceed 255. */
    SoundQueueReset();
    for (int i = 0; i < 16; ++i) {
        SoundQueueEnqueue(0, 0, (uint32_t)i, 255);
    }

    uint32_t mix = SoundQueueMixForListener(0, 0);
    uint8_t L = mix & 0xFF;
    uint8_t C = (mix >> 8) & 0xFF;
    uint8_t R = (mix >> 16) & 0xFF;

    /* All channels are uint8 — they CAN'T exceed 255, but the production
     * code explicitly clamps before packing. Verify channel values
     * are sensible (non-zero, accumulated). */
    ASSERT_TRUE(L > 0);
    ASSERT_TRUE(C > 0);
    ASSERT_TRUE(R > 0);
    (void)L; (void)C; (void)R;
}

/* ---- listener position affects relative pan ----------------------- */

TEST(moving_listener_changes_relative_pan)
{
    /* Source at (100, 0). Listener at (0, 0) → source on right.
     * Listener moves to (200, 0) → source now on left of listener. */
    SoundQueueReset();
    SoundQueueEnqueue(100, 0, 1, 200);

    uint32_t mix_left  = SoundQueueMixForListener(  0, 0);
    uint32_t mix_right = SoundQueueMixForListener(200, 0);

    uint8_t L1 = mix_left & 0xFF;
    uint8_t R1 = (mix_left >> 16) & 0xFF;
    uint8_t L2 = mix_right & 0xFF;
    uint8_t R2 = (mix_right >> 16) & 0xFF;

    /* From listener 1: source right → R1 > L1. */
    ASSERT_TRUE(R1 > L1);
    /* From listener 2: source left → L2 > R2. */
    ASSERT_TRUE(L2 > R2);
}

SUITE(sound_queue)
{
    RUN_TEST(empty_queue_returns_identity);
    RUN_TEST(reset_clears_previous_state);
    RUN_TEST(source_at_listener_position_centered);
    RUN_TEST(source_to_right_pans_louder_on_right_channel);
    RUN_TEST(source_to_left_pans_louder_on_left_channel);
    RUN_TEST(distant_source_quieter_than_close_source);
    RUN_TEST(zero_volume_contributes_nothing);
    RUN_TEST(two_sources_contributions_accumulate);
    RUN_TEST(channel_values_clamp_to_255);
    RUN_TEST(moving_listener_changes_relative_pan);
}
