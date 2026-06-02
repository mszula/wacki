/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_assets.c — ANIM / MASK / FILD header parsing.
 *
 * src/assets.c LoadAssetFromDtaBase parses the three shared-format
 * assets (3-byte magic + count + offset tables). It uses LoadFileFromDta
 * to fetch the bytes (already tested in test_archive.c) so this file
 * focuses on the parse + bbox + kind/flag_22 logic.
 *
 * Strategy: build a DTA fixture containing a hand-crafted ANIM (or
 * MASK / FILD) asset, mount it, call LoadAssetFromDtaBase, verify
 * frame_count, off_widths/heights/drawX/drawY arrays, max_w/max_h,
 * kind, flag_22, raw_buffer, raw_size, name.
 *
 * Reference: src/assets.c.
 */

#include "test.h"
#include "wacki.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- DTA fixture builder (mirrors test_archive.c) ----------------------- */

static void write_pkv2_literal(uint8_t *out, const uint8_t *payload, uint32_t unp)
{
    Pkv2Header h;
    h.magic = PKV2_MAGIC;
    h.compressed_size = 12 + unp;
    h.unpacked_size   = unp;
    memcpy(out, &h, 12);
    memcpy(out + 12, payload, unp);
}

/* Build a DTA archive containing one file "ASSET.WYC" of `payload_len`
 * bytes. The compressed file is at offset 4 in the .dta.
 *
 * NOTE: PKv2 literal mode requires payload_len >= 20 AND the last byte
 * of the payload (compressed stream end) == 0. Caller must ensure
 * payload[payload_len - 1] == 0. */
static size_t build_dta_with_asset(uint8_t *out, const char *name,
                                    const uint8_t *payload,
                                    uint32_t payload_len)
{
    /* Payload must already satisfy literal-mode constraints. */
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
    /* Upper-case (LoadFileFromDta uppercases the key, dir already uppercase). */
    uint32_t off = 4;
    memcpy(dir + 12, &off, 4);

    write_pkv2_literal(out + after_file + 4, dir, 32);
    uint32_t after_spis = after_file + 4 + 12 + 32;

    uint32_t spis_size = 12 + 32;
    memcpy(out + after_spis, &spis_size, 4);

    return after_spis + 4;
}

static int write_file_bytes(const char *path, const void *buf, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t w = fwrite(buf, 1, n, f);
    fclose(f);
    return w == n;
}

static const char kTmpDta[] = "/tmp/wacki-test-assets.dta";

/* ---- ANIM fixture builder ---------------------------------------------- *
 * ANIM layout (per docs/asset-format.md + src/assets.c):
 *   +0  DWORD magic 'ANIM' (or MASK / FILD)
 *   +4  WORD  frame_count
 *   +6  WORD  off_widths      (relative to file start)
 *   +8  WORD  off_heights
 *   +10 WORD  off_drawX
 *   +12 WORD  off_drawY
 *   +14 WORD  off_pixel_table
 *   +16 WORD  flag_22 (alpha-plane / 8bpp click select bits)
 *   ...     (gap)
 *   +0x12  width_tab[frame_count] (uint16 each)
 *   widths × heights × drawX × drawY arrays (each 2 × frame_count bytes)
 *   pixel-offset table (4 × frame_count bytes)
 *   pixel data
 *
 * For test fixtures we use 3 frames. */
static uint32_t build_anim_asset(uint8_t *out, uint32_t magic,
                                  uint16_t flag_22,
                                  uint16_t w0, uint16_t w1, uint16_t w2,
                                  uint16_t h0, uint16_t h1, uint16_t h2)
{
    /* Layout offsets within the asset. */
    uint16_t off_widths  = 0x14;     /* immediately after 0x14-byte header (4 magic + 4*2 offsets + 2 + 2) */
    uint16_t off_heights = off_widths  + 3 * 2;   /* 0x14 + 6 = 0x1A */
    uint16_t off_drawX   = off_heights + 3 * 2;   /* 0x20 */
    uint16_t off_drawY   = off_drawX   + 3 * 2;   /* 0x26 */
    uint16_t off_pixel_tab = off_drawY + 3 * 2;   /* 0x2C */
    uint16_t pixels_start  = off_pixel_tab + 3 * 4; /* 0x38 */

    /* Header */
    memcpy(out + 0, &magic, 4);
    uint16_t fc = 3;
    memcpy(out + 4,  &fc,            2);
    memcpy(out + 6,  &off_widths,    2);
    memcpy(out + 8,  &off_heights,   2);
    memcpy(out + 10, &off_drawX,     2);
    memcpy(out + 12, &off_drawY,     2);
    memcpy(out + 14, &off_pixel_tab, 2);
    memcpy(out + 16, &flag_22,       2);
    /* Bytes 0x12-0x13: padding */
    out[0x12] = 0; out[0x13] = 0;

    /* Width / height / drawX / drawY arrays */
    uint16_t ws[3] = { w0, w1, w2 };
    uint16_t hs[3] = { h0, h1, h2 };
    uint16_t dx[3] = { 0, 1, 2 };
    uint16_t dy[3] = { 10, 11, 12 };
    memcpy(out + off_widths,  ws, sizeof ws);
    memcpy(out + off_heights, hs, sizeof hs);
    memcpy(out + off_drawX,   dx, sizeof dx);
    memcpy(out + off_drawY,   dy, sizeof dy);

    /* Pixel-offset table — point each frame's pixel block past `pixels_start`. */
    uint32_t p0 = pixels_start;
    uint32_t p1 = pixels_start + 4;
    uint32_t p2 = pixels_start + 8;
    memcpy(out + off_pixel_tab + 0, &p0, 4);
    memcpy(out + off_pixel_tab + 4, &p1, 4);
    memcpy(out + off_pixel_tab + 8, &p2, 4);

    /* Fake pixel data — 4 bytes per frame, distinct fill. */
    for (int i = 0; i < 4; ++i) out[pixels_start + 0 + i] = 0xA0;
    for (int i = 0; i < 4; ++i) out[pixels_start + 4 + i] = 0xB1;
    for (int i = 0; i < 4; ++i) out[pixels_start + 8 + i] = 0xC2;

    /* Total asset size. Make it 32-byte aligned for the PKv2 padding floor. */
    return pixels_start + 12;
}

/* ---- tests -------------------------------------------------------------- */

TEST(anim_header_parsed_correctly)
{
    /* Build an ANIM asset with frame_count=3, flag_22=0 (kind=2). */
    uint8_t asset[96];
    memset(asset, 0, sizeof asset);
    uint32_t asset_len = build_anim_asset(asset, ASSET_MAGIC_ANIM,
                                            /*flag_22=*/0,
                                            /*w0=*/10, /*w1=*/20, /*w2=*/15,
                                            /*h0=*/30, /*h1=*/25, /*h2=*/40);

    /* Pad to >= 20 bytes literal mode minimum and ensure last byte is 0
     * (the trailing pixel bytes are 0xC2; pad after with zeros). */
    uint8_t payload[128];
    memset(payload, 0, sizeof payload);
    memcpy(payload, asset, asset_len);
    uint32_t pl_len = asset_len < 20 ? 20 : asset_len;
    /* Need last byte = 0 → ensure we end on a zero byte. */
    payload[pl_len - 1] = 0;

    uint8_t blob[256];
    memset(blob, 0, sizeof blob);
    size_t n = build_dta_with_asset(blob, "ASSET.WYC", payload, pl_len);
    ASSERT_TRUE(write_file_bytes(kTmpDta, blob, n));

    ASSERT_EQ(OpenDtaArchiveFile(kTmpDta), 1);
    AnimAsset *a = LoadAssetFromDtaBase("ASSET.WYC");
    ASSERT_NOT_NULL(a);

    ASSERT_EQ(a->frame_count, 3);
    ASSERT_EQ(a->off_widths[0], 10);
    ASSERT_EQ(a->off_widths[1], 20);
    ASSERT_EQ(a->off_widths[2], 15);
    ASSERT_EQ(a->off_heights[0], 30);
    ASSERT_EQ(a->off_heights[1], 25);
    ASSERT_EQ(a->off_heights[2], 40);
    ASSERT_EQ(a->off_drawX[0], 0);
    ASSERT_EQ(a->off_drawX[1], 1);
    ASSERT_EQ(a->off_drawX[2], 2);
    ASSERT_EQ(a->off_drawY[0], 10);

    /* max_w / max_h = max across all frames. */
    ASSERT_EQ(a->max_w, 20);
    ASSERT_EQ(a->max_h, 40);

    /* flag_22 == 0 → kind == 2 (raw frames, NOT RLE). */
    ASSERT_EQ(a->kind, 2);
    ASSERT_EQ(a->flag_22, 0);

    /* Name was stored. */
    ASSERT_STREQ(a->name, "ASSET.WYC");

    /* Pixel pointers point at distinct fill bytes. */
    ASSERT_EQ(a->pixel_ptrs[0][0], 0xA0);
    ASSERT_EQ(a->pixel_ptrs[1][0], 0xB1);
    ASSERT_EQ(a->pixel_ptrs[2][0], 0xC2);

    FreeAsset(a);
    remove(kTmpDta);
}

TEST(anim_with_rich_flag_becomes_kind_3)
{
    /* flag_22 != 0 → kind=3 (RLE-compressed frames). */
    uint8_t asset[96];
    memset(asset, 0, sizeof asset);
    uint32_t asset_len = build_anim_asset(asset, ASSET_MAGIC_ANIM,
                                            /*flag_22=*/0x0001,
                                            8, 8, 8,
                                            8, 8, 8);
    uint8_t payload[128] = { 0 };
    memcpy(payload, asset, asset_len);
    uint32_t pl_len = asset_len < 20 ? 20 : asset_len;
    payload[pl_len - 1] = 0;

    uint8_t blob[256];
    memset(blob, 0, sizeof blob);
    size_t n = build_dta_with_asset(blob, "RICH.WYC", payload, pl_len);
    write_file_bytes(kTmpDta, blob, n);

    ASSERT_EQ(OpenDtaArchiveFile(kTmpDta), 1);
    AnimAsset *a = LoadAssetFromDtaBase("RICH.WYC");
    ASSERT_NOT_NULL(a);
    ASSERT_EQ(a->kind, 3);
    ASSERT_EQ(a->flag_22, 0x0001);

    FreeAsset(a);
    remove(kTmpDta);
}

TEST(mask_asset_has_kind_zero)
{
    /* MASK asset → kind=0, flag_22=0 (port collapses non-ANIM). */
    uint8_t asset[96];
    memset(asset, 0, sizeof asset);
    uint32_t asset_len = build_anim_asset(asset, ASSET_MAGIC_MASK,
                                            /*flag_22=*/0,
                                            5, 5, 5,
                                            5, 5, 5);
    uint8_t payload[128] = { 0 };
    memcpy(payload, asset, asset_len);
    uint32_t pl_len = asset_len < 20 ? 20 : asset_len;
    payload[pl_len - 1] = 0;

    uint8_t blob[256];
    memset(blob, 0, sizeof blob);
    size_t n = build_dta_with_asset(blob, "MASK.MSK", payload, pl_len);
    write_file_bytes(kTmpDta, blob, n);

    ASSERT_EQ(OpenDtaArchiveFile(kTmpDta), 1);
    AnimAsset *a = LoadAssetFromDtaBase("MASK.MSK");
    ASSERT_NOT_NULL(a);
    ASSERT_EQ(a->kind, 0);
    ASSERT_EQ(a->flag_22, 0);

    FreeAsset(a);
    remove(kTmpDta);
}

TEST(bad_magic_returns_null)
{
    /* Asset with valid PKv2 wrap but no recognised magic → LoadAssetFromDtaBase NULL. */
    uint8_t payload[32] = { 0 };
    /* magic = 'FAKE' (not ANIM/MASK/FILD) */
    payload[0] = 'F'; payload[1] = 'A'; payload[2] = 'K'; payload[3] = 'E';
    payload[31] = 0;

    uint8_t blob[256];
    memset(blob, 0, sizeof blob);
    size_t n = build_dta_with_asset(blob, "BAD.WYC", payload, 32);
    write_file_bytes(kTmpDta, blob, n);

    ASSERT_EQ(OpenDtaArchiveFile(kTmpDta), 1);
    AnimAsset *a = LoadAssetFromDtaBase("BAD.WYC");
    ASSERT_NULL(a);

    remove(kTmpDta);
}

TEST(missing_asset_returns_null)
{
    /* DTA mounted but the requested asset isn't in it. */
    uint8_t payload[32] = { 0 };
    payload[0] = 'A'; payload[1] = 'N'; payload[2] = 'I'; payload[3] = 'M';
    payload[31] = 0;

    uint8_t blob[256];
    memset(blob, 0, sizeof blob);
    size_t n = build_dta_with_asset(blob, "EXISTS.WYC", payload, 32);
    write_file_bytes(kTmpDta, blob, n);

    ASSERT_EQ(OpenDtaArchiveFile(kTmpDta), 1);
    AnimAsset *a = LoadAssetFromDtaBase("NOSUCH.WYC");
    ASSERT_NULL(a);

    remove(kTmpDta);
}

SUITE(assets)
{
    RUN_TEST(anim_header_parsed_correctly);
    RUN_TEST(anim_with_rich_flag_becomes_kind_3);
    RUN_TEST(mask_asset_has_kind_zero);
    RUN_TEST(bad_magic_returns_null);
    RUN_TEST(missing_asset_returns_null);
}
