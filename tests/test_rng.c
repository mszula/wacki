/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_rng.c — WackiRand ROL3 PRNG (FUN_00410F50).
 *
 * The original engine uses a deterministic ROL3-based RNG seeded from
 * time(). Determinism is required by the smoke harness (`--seed N`)
 * and used by:
 *   - actor.c case 6/9 (random animation frame pick)
 *   - audio.c PlaySfx random voice line picker
 *   - script.c op 0x2A IS_SOUND_PLAYING / WackiRand(bound)
 *
 * If we ever refactor the RNG (e.g. switch state width, change
 * advance constant) and lose determinism, the entire smoke harness
 * goes flaky. These tests pin the contract.
 *
 * Reference: src/stubs.c:1371+ (WackiRandSeed / WackiRand).
 */

#include "test.h"
#include "wacki.h"

#include <stdint.h>

/* ---- determinism: same seed → same sequence ---------------------------- */

TEST(seeded_run_is_deterministic)
{
    /* Two seeded runs with bound=1000 must produce the identical sequence. */
    uint32_t a[16], b[16];

    WackiRandSeed(0x12345678u);
    for (int i = 0; i < 16; ++i) a[i] = WackiRand(1000);

    WackiRandSeed(0x12345678u);
    for (int i = 0; i < 16; ++i) b[i] = WackiRand(1000);

    ASSERT_MEMEQ(a, b, sizeof a);
}

TEST(different_seeds_produce_different_sequences)
{
    /* Sanity: changing the seed must change the output (very high
     * probability — for a ROL3 PRNG with 0xFFFFFFFF state, two seeds
     * that diverge by 1 will produce different output within 1-2 steps). */
    uint32_t a[8], b[8];

    WackiRandSeed(1);
    for (int i = 0; i < 8; ++i) a[i] = WackiRand(0xFFFF);

    WackiRandSeed(2);
    for (int i = 0; i < 8; ++i) b[i] = WackiRand(0xFFFF);

    /* It's not guaranteed every element differs, but the buffers as a whole
     * must differ. */
    ASSERT_FALSE(memcmp(a, b, sizeof a) == 0);
}

TEST(zero_seed_remaps_to_nonzero)
{
    /* WackiRandSeed(0) silently re-seeds to 0xDEADBEEF — verify by checking
     * that seeding with 0 produces the SAME sequence as seeding with 0xDEADBEEF. */
    uint32_t a[8], b[8];

    WackiRandSeed(0);
    for (int i = 0; i < 8; ++i) a[i] = WackiRand(100);

    WackiRandSeed(0xDEADBEEFu);
    for (int i = 0; i < 8; ++i) b[i] = WackiRand(100);

    ASSERT_MEMEQ(a, b, sizeof a);
}

/* ---- bound contract: WackiRand(N) returns value in [0, N) -------------- */

TEST(result_within_bound)
{
    WackiRandSeed(42);
    for (int i = 0; i < 4096; ++i) {
        uint32_t r = WackiRand(100);
        ASSERT_TRUE(r < 100);
    }
}

TEST(result_within_bound_power_of_two)
{
    WackiRandSeed(7);
    for (int i = 0; i < 4096; ++i) {
        uint32_t r = WackiRand(64);
        ASSERT_TRUE(r < 64);
    }
}

TEST(result_within_bound_large)
{
    /* Bound near uint16_t cap. */
    WackiRandSeed(99);
    for (int i = 0; i < 4096; ++i) {
        uint32_t r = WackiRand(0xFFFE);
        ASSERT_TRUE(r < 0xFFFE);
    }
}

/* ---- distribution sanity (light) ---------------------------------------- */

TEST(distribution_covers_buckets)
{
    /* Over 10k samples with bound=16, every bucket should be hit at least
     * a handful of times. We use a generous 10× safety margin: each bucket
     * expected ~625, threshold = 50. Won't detect subtle bias but catches
     * "always returns 0" or "always returns same value" regressions. */
    int hits[16] = { 0 };
    WackiRandSeed(0xCAFEBABEu);
    for (int i = 0; i < 10000; ++i) {
        uint32_t r = WackiRand(16);
        ASSERT_TRUE(r < 16);
        ++hits[r];
    }
    for (int i = 0; i < 16; ++i) {
        ASSERT_TRUE(hits[i] >= 50);
    }
}

/* ---- golden vector: lock the exact ROL3 advance ------------------------ */

TEST(golden_vector_seed_1)
{
    /* Pinned outputs for WackiRandSeed(1) → WackiRand(0xFFFF) × 6.
     *
     * The values are CAPTURED from the production WackiRand (formula:
     * state = ROL32(state, 3) + 0x3D8A479C; output = state & 0xFFFF
     * with optional pow2 remap). If anyone changes the advance constant,
     * rotation width, or seed handling, this golden vector breaks
     * immediately and the diff in the assertion output shows exactly
     * which step diverged.
     *
     * Hand-deriving these from the formula is error-prone (off-by-one in
     * the ROL high bit will silently shift every subsequent value). So
     * the test pins what the function actually does — that IS the
     * contract for downstream callers (smoke harness, deterministic
     * replay, etc.). */
    WackiRandSeed(1);
    ASSERT_EQ(WackiRand(0xFFFF), 18340);
    ASSERT_EQ(WackiRand(0xFFFF), 33981);
    ASSERT_EQ(WackiRand(0xFFFF), 28037);
    ASSERT_EQ(WackiRand(0xFFFF), 46024);
    ASSERT_EQ(WackiRand(0xFFFF), 58849);
    ASSERT_EQ(WackiRand(0xFFFF), 30374);
}

SUITE(rng)
{
    RUN_TEST(seeded_run_is_deterministic);
    RUN_TEST(different_seeds_produce_different_sequences);
    RUN_TEST(zero_seed_remaps_to_nonzero);
    RUN_TEST(result_within_bound);
    RUN_TEST(result_within_bound_power_of_two);
    RUN_TEST(result_within_bound_large);
    RUN_TEST(distribution_covers_buckets);
    RUN_TEST(golden_vector_seed_1);
}
