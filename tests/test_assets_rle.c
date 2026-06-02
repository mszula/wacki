/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_assets_rle.c — ANIM kind=3 (RLE-compressed frames) integration.
 *
 * test_assets.c covers ANIM/MASK/FILD header parsing with flat (kind=2)
 * pixel data. This file exercises the kind=3 path: ANIM with flag_22 != 0
 * → asset->kind = 3, and asset->pixel_ptrs[i] points at RLE-encoded
 * pixel data (3-byte header + stream). DepackRleFrame then decodes
 * those bytes into a destination buffer.
 *
 * This is the integration that LoadAssetFromDtaBase + the per-frame
 * render path in actor.c rely on for actor / prop atlases like
 * ebek.wyc / fjej.wyc / przedm.wyc.
 *
 * Reference: src/assets.c lines 106-111, src/graphics.c DepackRleFrame.
 */

#include "test.h"
#include "wacki.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers (mirrored from test_assets.c / test_archive.c) ---------- */

static void write_pkv2_literal(uint8_t *out, const uint8_t *payload, uint32_t unp)
{
    Pkv2Header h;
    h.magic = PKV2_MAGIC;
    h.compressed_size = 12 + unp;
    h.unpacked_size   = unp;
    memcpy(out, &h, 12);
    memcpy(out + 12, payload, unp);
}

static int write_file_bytes(const char *path, const void *buf, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t w = fwrite(buf, 1, n, f);
    fclose(f);
    return w == n;
}

static size_t build_dta_with_asset(uint8_t *out, const char *name,
                                    const uint8_t *payload, uint32_t payload_len)
{
    uint32_t base = DTA_MAGIC_BASE;
    memcpy(out + 0, &base, 4);
    write_pkv2_literal(out + 4, payload, payload_len);
    uint32_t after_file = 4 + 12 + payload_len;

    uint32_t spis = DTA_MAGIC_SPIS;
    memcpy(out + after_file, &spis, 4);

    uint8_t dir[32];
    memset(dir, 0, sizeof dir);
    size_t namelen = strlen(name);
    if (namelen > DTA_NAME_LEN) namelen = DTA_NAME_LEN;
    memcpy(dir, name, namelen);
    uint32_t off = 4;
    memcpy(dir + 12, &off, 4);

    write_pkv2_literal(out + after_file + 4, dir, 32);
    uint32_t after_spis = after_file + 4 + 12 + 32;

    uint32_t spis_size = 12 + 32;
    memcpy(out + after_spis, &spis_size, 4);

    return after_spis + 4;
}

static const char kTmpDta[] = "/tmp/wacki-test-assets-rle.dta";

/* Build a kind=3 ANIM asset with 1 frame containing RLE-encoded pixels.
 * The RLE header is fill_value=0, marker_A=0xFD, marker_B=0xFE — chosen
 * to be distinct and visible in fixtures.
 *
 * Returns total asset size, populates `out_decoded` with what the frame
 * should decode to (for the test's later DepackRleFrame check). */
static uint32_t build_kind3_anim(uint8_t *out,
                                   const uint8_t *rle_stream, size_t stream_len,
                                   uint8_t fill_value,
                                   uint8_t marker_A,
                                   uint8_t marker_B)
{
    /* ANIM layout (one frame): same header as kind=2 in test_assets.c,
     * with flag_22 != 0 to trigger kind=3 path.
     *
     *   +0  DWORD magic 'ANIM'
     *   +4  WORD frame_count = 1
     *   +6  WORD off_widths
     *   +8  WORD off_heights
     *   +10 WORD off_drawX
     *   +12 WORD off_drawY
     *   +14 WORD off_pixel_table
     *   +16 WORD flag_22 = 0x0001 (anything non-zero)
     *   +18 padding
     *   per-frame arrays (1 entry each):
     *     off_widths = 0x14, off_heights = 0x16, off_drawX = 0x18, off_drawY = 0x1A
     *   off_pixel_tab @ 0x1C (1 × u32 = 4 bytes) — points to pixel block
     *   pixel block @ 0x20 = 3-byte RLE header + stream
     */
    memset(out, 0, 256);

    uint32_t magic = ASSET_MAGIC_ANIM;
    memcpy(out + 0, &magic, 4);
    uint16_t fc = 1;
    memcpy(out + 4, &fc, 2);

    uint16_t off_widths    = 0x14;
    uint16_t off_heights   = 0x16;
    uint16_t off_drawX     = 0x18;
    uint16_t off_drawY     = 0x1A;
    uint16_t off_pixel_tab = 0x1C;
    uint16_t flag_22       = 0x0001;     /* kind=3 trigger */
    memcpy(out + 6,  &off_widths, 2);
    memcpy(out + 8,  &off_heights, 2);
    memcpy(out + 10, &off_drawX, 2);
    memcpy(out + 12, &off_drawY, 2);
    memcpy(out + 14, &off_pixel_tab, 2);
    memcpy(out + 16, &flag_22, 2);

    /* Per-frame metadata (1 entry each). */
    uint16_t w = 8, h = 4, dx = 10, dy = 20;
    memcpy(out + off_widths,  &w,  2);
    memcpy(out + off_heights, &h,  2);
    memcpy(out + off_drawX,   &dx, 2);
    memcpy(out + off_drawY,   &dy, 2);

    /* pixel_off_table[0] = offset of pixel block within asset. */
    uint32_t pixel_block_off = 0x20;
    memcpy(out + off_pixel_tab, &pixel_block_off, 4);

    /* Pixel block: 3-byte RLE header + stream. */
    out[pixel_block_off + 0] = fill_value;
    out[pixel_block_off + 1] = marker_A;
    out[pixel_block_off + 2] = marker_B;
    memcpy(out + pixel_block_off + 3, rle_stream, stream_len);

    return pixel_block_off + 3 + (uint32_t)stream_len;
}

/* ---- tests ----------------------------------------------------------- */

TEST(kind_3_asset_pixel_ptrs_decode_via_rle)
{
    /* RLE stream that should decode to: 0,0,0,0,0,0xAA,0xBB,0xBB,0xBB
     * = 9 output bytes.
     *
     * Encoded:
     *   marker_A=0xFD followed by count-1=4 → emit 5 × fill_value (0)
     *   literal 0xAA
     *   marker_B=0xFE followed by count-1=2, value=0xBB → emit 3 × 0xBB
     */
    uint8_t stream[] = {
        0xFD, 0x04,           /* AAAAA */
        0xAA,                 /* literal */
        0xFE, 0x02, 0xBB,     /* BBB */
    };
    uint8_t expected[9] = { 0, 0, 0, 0, 0, 0xAA, 0xBB, 0xBB, 0xBB };

    uint8_t asset[256];
    uint32_t asset_len = build_kind3_anim(asset, stream, sizeof stream,
                                            /*fill=*/0, /*A=*/0xFD, /*B=*/0xFE);

    /* Pad to >= 20 bytes for PKv2 literal mode + ensure last byte 0. */
    /* PKv2 literal mode requires last byte of compressed stream == 0.
     * Add one trailing zero byte PAST the asset bytes so we don't
     * overwrite the asset's actual last byte (the RLE stream's last
     * meaningful byte). */
    uint8_t payload[256] = { 0 };
    memcpy(payload, asset, asset_len);
    uint32_t pl_len = asset_len + 1;
    if (pl_len < 20) pl_len = 20;
    payload[pl_len - 1] = 0;

    uint8_t blob[512];
    memset(blob, 0, sizeof blob);
    size_t n = build_dta_with_asset(blob, "RICH.WYC", payload, pl_len);
    ASSERT_TRUE(write_file_bytes(kTmpDta, blob, n));

    ASSERT_EQ(OpenDtaArchiveFile(kTmpDta), 1);
    AnimAsset *a = LoadAssetFromDtaBase("RICH.WYC");
    ASSERT_NOT_NULL(a);

    /* Kind = 3 (RLE), flag_22 = 0x0001 (preserved). */
    ASSERT_EQ(a->kind, 3);
    ASSERT_EQ(a->flag_22, 0x0001);
    ASSERT_EQ(a->frame_count, 1);
    ASSERT_EQ(a->off_widths[0], 8);
    ASSERT_EQ(a->off_heights[0], 4);

    /* pixel_ptrs[0] points at the 3-byte header followed by the RLE stream.
     * Verify the header bytes match what we wrote. */
    const uint8_t *p = a->pixel_ptrs[0];
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p[0], 0);      /* fill */
    ASSERT_EQ(p[1], 0xFD);   /* marker_A */
    ASSERT_EQ(p[2], 0xFE);   /* marker_B */

    /* DepackRleFrame against the pixel block, decoding sizeof expected bytes. */
    uint8_t decoded[16];
    memset(decoded, 0x55, sizeof decoded);   /* sentinel guard */
    DepackRleFrame(p, decoded, (int)sizeof expected);

    ASSERT_MEMEQ(decoded, expected, sizeof expected);
    /* Guard bytes past sizeof expected must still hold 0x55. */
    ASSERT_EQ(decoded[9],  0x55);
    ASSERT_EQ(decoded[15], 0x55);

    FreeAsset(a);
    remove(kTmpDta);
}

TEST(kind_3_decode_full_8x4_frame_via_rle)
{
    /* 8×4 frame = 32 bytes. Encode 32 zeros via marker_A:
     *   0xFD 31  (emit 32 × fill=0)
     */
    uint8_t stream[] = { 0xFD, 31 };

    uint8_t asset[256];
    uint32_t asset_len = build_kind3_anim(asset, stream, sizeof stream,
                                            0, 0xFD, 0xFE);

    /* PKv2 literal mode requires last byte of compressed stream == 0.
     * Add one trailing zero byte PAST the asset bytes so we don't
     * overwrite the asset's actual last byte (the RLE stream's last
     * meaningful byte). */
    uint8_t payload[256] = { 0 };
    memcpy(payload, asset, asset_len);
    uint32_t pl_len = asset_len + 1;
    if (pl_len < 20) pl_len = 20;
    payload[pl_len - 1] = 0;

    uint8_t blob[512];
    memset(blob, 0, sizeof blob);
    size_t n = build_dta_with_asset(blob, "FULL.WYC", payload, pl_len);
    write_file_bytes(kTmpDta, blob, n);

    ASSERT_EQ(OpenDtaArchiveFile(kTmpDta), 1);
    AnimAsset *a = LoadAssetFromDtaBase("FULL.WYC");
    ASSERT_NOT_NULL(a);

    /* Frame area = w × h = 8 × 4 = 32 bytes. */
    int frame_pixels = (int)a->off_widths[0] * (int)a->off_heights[0];
    ASSERT_EQ(frame_pixels, 32);

    uint8_t decoded[64];
    memset(decoded, 0xAA, sizeof decoded);
    DepackRleFrame(a->pixel_ptrs[0], decoded, frame_pixels);

    for (int i = 0; i < frame_pixels; ++i) ASSERT_EQ(decoded[i], 0);
    /* Past frame area sentinels untouched. */
    ASSERT_EQ(decoded[32], 0xAA);
    ASSERT_EQ(decoded[63], 0xAA);

    FreeAsset(a);
    remove(kTmpDta);
}

TEST(kind_3_with_flag_22_bit_zero_set_alpha_plane_marker)
{
    /* flag_22 bit 0 = alpha-plane source marker (per docs/asset-format.md
     * §3.2). LoadAssetFromDtaBase should preserve the raw bits in
     * a->flag_22 even when our internal `kind` collapses non-zero to 3. */
    uint8_t stream[] = { 0xFD, 0 };   /* emit 1 × fill */
    uint8_t asset[256];
    uint32_t asset_len = build_kind3_anim(asset, stream, sizeof stream,
                                            0, 0xFD, 0xFE);
    /* Patch flag_22 to 0x0001 (alpha-plane bit). */
    uint16_t flag22 = 0x0001;
    memcpy(asset + 16, &flag22, 2);

    /* PKv2 literal mode requires last byte of compressed stream == 0.
     * Add one trailing zero byte PAST the asset bytes so we don't
     * overwrite the asset's actual last byte (the RLE stream's last
     * meaningful byte). */
    uint8_t payload[256] = { 0 };
    memcpy(payload, asset, asset_len);
    uint32_t pl_len = asset_len + 1;
    if (pl_len < 20) pl_len = 20;
    payload[pl_len - 1] = 0;

    uint8_t blob[512];
    memset(blob, 0, sizeof blob);
    size_t n = build_dta_with_asset(blob, "ALPHA.WYC", payload, pl_len);
    write_file_bytes(kTmpDta, blob, n);

    ASSERT_EQ(OpenDtaArchiveFile(kTmpDta), 1);
    AnimAsset *a = LoadAssetFromDtaBase("ALPHA.WYC");
    ASSERT_NOT_NULL(a);

    ASSERT_EQ(a->kind, 3);
    ASSERT_EQ(a->flag_22 & 0x0001, 0x0001);   /* alpha-plane bit preserved */

    FreeAsset(a);
    remove(kTmpDta);
}

SUITE(assets_rle)
{
    RUN_TEST(kind_3_asset_pixel_ptrs_decode_via_rle);
    RUN_TEST(kind_3_decode_full_8x4_frame_via_rle);
    RUN_TEST(kind_3_with_flag_22_bit_zero_set_alpha_plane_marker);
}
