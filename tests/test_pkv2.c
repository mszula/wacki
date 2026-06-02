/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_pkv2.c — PKv2 depack roundtrip.
 *
 * PKv2 is the LZ77 + Huffman-prefix compression used inside DTA archives.
 * The depack contract is verified at integration level by
 * `tools/dta-validate.sh` (1782 entries SHA-256). This file pins the
 * UNIT-level contract: header parsing + literal-mode fast path + sanity
 * checks (bad magic, undersize input).
 *
 * Why "literal mode" is enough for unit tests:
 *   The PKv2 format has two modes — literal (last byte == 0, payload
 *   is raw bytes after the 12-byte header) and compressed (LZ77 stream
 *   read backwards). Constructing a valid compressed-mode fixture by
 *   hand is impractical — that's what dta-validate.sh covers via real
 *   archive data. Literal mode catches all the header / sanity logic.
 *
 * Reference: src/depack.c DepackPkv2Buffer + docs/asset-format.md §2.
 */

#include "test.h"
#include "wacki.h"

#include <stdlib.h>
#include <string.h>

/* Build a literal-mode PKv2 blob in `out_buf` (caller-allocated).
 * Header (12 bytes) + payload (`unp` bytes ending in 0) = `comp` total.
 * Returns total size written. */
static size_t build_literal_pkv2(uint8_t *out, const uint8_t *payload,
                                  uint32_t unp)
{
    /* Sanity check from depack: comp >= 32 && unp > 0. We use comp = 12 + unp,
     * so caller must supply unp >= 20 for the comp >= 32 check to pass. */
    uint32_t comp = 12 + unp;

    /* Write little-endian header. */
    Pkv2Header h;
    h.magic = PKV2_MAGIC;
    h.compressed_size = comp;
    h.unpacked_size   = unp;
    memcpy(out, &h, 12);

    /* Payload — caller must ensure last byte is 0 (literal-mode marker). */
    memcpy(out + 12, payload, unp);

    return comp;
}

/* ---- happy path: literal mode roundtrip --------------------------------- */

TEST(literal_mode_roundtrip)
{
    /* 24 bytes of literal payload, last byte 0. */
    uint8_t payload[24];
    for (int i = 0; i < 23; ++i) payload[i] = (uint8_t)('A' + (i % 26));
    payload[23] = 0; /* literal-mode terminator */

    uint8_t src[12 + 24];
    size_t comp = build_literal_pkv2(src, payload, 24);
    ASSERT_EQ(comp, 36);  /* 12 + 24 */

    uint8_t dst[24];
    memset(dst, 0xAA, sizeof dst);
    DepackPkv2Buffer(src, dst, NULL);

    /* Output should equal payload exactly. */
    ASSERT_MEMEQ(dst, payload, 24);
}

TEST(literal_mode_minimum_size)
{
    /* Smallest legal payload: unp = 20 (so comp = 32, hits the >= 32 floor). */
    uint8_t payload[20];
    for (int i = 0; i < 19; ++i) payload[i] = (uint8_t)(0x80 + i);
    payload[19] = 0;

    uint8_t src[12 + 20];
    build_literal_pkv2(src, payload, 20);

    uint8_t dst[20] = { 0 };
    DepackPkv2Buffer(src, dst, NULL);

    ASSERT_MEMEQ(dst, payload, 20);
}

TEST(literal_mode_writes_full_unpacked_size)
{
    /* Confirm the decoder writes exactly `unp` bytes — not one more,
     * not one less. We overrun-detect by surrounding dst with a guard. */
    uint8_t payload[32];
    for (int i = 0; i < 31; ++i) payload[i] = (uint8_t)(i + 1);
    payload[31] = 0;

    uint8_t src[12 + 32];
    build_literal_pkv2(src, payload, 32);

    uint8_t dst_with_guard[64];
    memset(dst_with_guard, 0xCC, sizeof dst_with_guard);
    uint8_t *dst = dst_with_guard + 16;  /* 16-byte guard either side */

    DepackPkv2Buffer(src, dst, NULL);

    /* Guard bytes BEFORE dst must be untouched. */
    for (int i = 0; i < 16; ++i) ASSERT_EQ(dst_with_guard[i], 0xCC);
    /* Guard bytes AFTER dst+32 must be untouched. */
    for (int i = 48; i < 64; ++i) ASSERT_EQ(dst_with_guard[i], 0xCC);
    /* Payload area must match. */
    ASSERT_MEMEQ(dst, payload, 32);
}

/* ---- progress callback fires on completion ----------------------------- */

static int g_progress_calls;
static int g_progress_last;
static void progress_cb(int pct) { ++g_progress_calls; g_progress_last = pct; }

TEST(progress_callback_fires_at_100)
{
    uint8_t payload[20];
    memset(payload, 'X', 19);
    payload[19] = 0;

    uint8_t src[12 + 20];
    build_literal_pkv2(src, payload, 20);

    uint8_t dst[20];
    g_progress_calls = 0; g_progress_last = -1;
    DepackPkv2Buffer(src, dst, progress_cb);

    ASSERT_TRUE(g_progress_calls >= 1);
    ASSERT_EQ(g_progress_last, 100);
}

/* ---- defensive: bad magic, undersized input ---------------------------- */

TEST(bad_magic_does_not_write_output)
{
    /* If magic is wrong, decoder logs and returns without touching dst. */
    uint8_t src[12 + 20];
    Pkv2Header h;
    h.magic = 0xDEADBEEFu;  /* not PKV2_MAGIC */
    h.compressed_size = 32;
    h.unpacked_size = 20;
    memcpy(src, &h, 12);
    memset(src + 12, 'Z', 19);
    src[12 + 19] = 0;

    uint8_t dst[20];
    memset(dst, 0xAB, sizeof dst);
    DepackPkv2Buffer(src, dst, NULL);

    /* Output buffer should be entirely untouched. */
    for (int i = 0; i < 20; ++i) ASSERT_EQ(dst[i], 0xAB);
}

TEST(undersize_input_does_not_write_output)
{
    /* comp < 32 → sanity bail, no output. */
    uint8_t src[12 + 8];
    Pkv2Header h;
    h.magic = PKV2_MAGIC;
    h.compressed_size = 20;   /* < 32, must bail */
    h.unpacked_size = 8;
    memcpy(src, &h, 12);
    memset(src + 12, 'Q', 7);
    src[12 + 7] = 0;

    uint8_t dst[8];
    memset(dst, 0x55, sizeof dst);
    DepackPkv2Buffer(src, dst, NULL);

    for (int i = 0; i < 8; ++i) ASSERT_EQ(dst[i], 0x55);
}

TEST(zero_unpacked_size_does_not_write_output)
{
    /* unp == 0 → sanity bail. */
    uint8_t src[64];
    Pkv2Header h;
    h.magic = PKV2_MAGIC;
    h.compressed_size = 64;
    h.unpacked_size = 0;
    memcpy(src, &h, 12);
    memset(src + 12, 0, 52);

    uint8_t dst[32];
    memset(dst, 0x77, sizeof dst);
    DepackPkv2Buffer(src, dst, NULL);

    for (int i = 0; i < 32; ++i) ASSERT_EQ(dst[i], 0x77);
}

SUITE(pkv2)
{
    RUN_TEST(literal_mode_roundtrip);
    RUN_TEST(literal_mode_minimum_size);
    RUN_TEST(literal_mode_writes_full_unpacked_size);
    RUN_TEST(progress_callback_fires_at_100);
    RUN_TEST(bad_magic_does_not_write_output);
    RUN_TEST(undersize_input_does_not_write_output);
    RUN_TEST(zero_unpacked_size_does_not_write_output);
}
