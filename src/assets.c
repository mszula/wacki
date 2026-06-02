/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/assets.c — ANIM (.wyc), MASK (.msk), FILD (.fld) loaders.
 *
 * All assets share the same in-file layout:
 *
 *   +0   DWORD magic ("ANIM" / "MASK" / "FILD")
 *   +4   WORD  frame count
 *   +6   WORD  off_width_table  (offset relative to file base)
 *   +8   WORD  off_height_table
 *   +10  WORD  off_drawX_table
 *   +12  WORD  off_drawY_table
 *   +14  WORD  off_pixel_table  (DWORD-of-offsets table; each entry →
 *                                a frame bitmap)
 *
 * For FILD the trailing data after the tables is a sequence of
 * (Δx, Δy) pairs that get appended to the global perspective profile
 * g_persp_profile. */

#include "wacki.h"
#include <string.h>

extern void *xmalloc(uint32_t sz);
extern void  xfree  (void *p);

/* Scripts object — used to bind [animacja]<filename> sections in
 * Wacky.scr to the loaded asset. */
extern void *g_scripts_obj;
extern void *FindAnimationScript(void *scripts_obj, const char *name);

/* Global perspective profile filled by .fld loads — pairs of (dx, dy)
 * deltas indexed by band. */
int16_t g_persp_profile[0x22*2];
int     g_persp_band_count = 0;

extern uint32_t g_tick_counter;

/* ---- AnimAsset header layout (.wyc / .fld) ------------------------ */

/* Offsets inside the raw .wyc / .fld header. The pointers at offsets
 * 6/8/10/12/14 each store a u16 byte offset (relative to `raw`) of
 * the corresponding table; the kind/body flag at 16 is a u16. */
#define ASSET_HEADER_OFF_FRAME_COUNT    4
#define ASSET_HEADER_OFF_WIDTH_TABLE    6
#define ASSET_HEADER_OFF_HEIGHT_TABLE   8
#define ASSET_HEADER_OFF_DRAWX_TABLE    10
#define ASSET_HEADER_OFF_DRAWY_TABLE    12
#define ASSET_HEADER_OFF_PIX_OFF_TABLE  14
#define ASSET_HEADER_OFF_KIND_OR_BODY   16
#define ASSET_PIX_OFF_ENTRY_BYTES       4

/* AnimAsset.kind enum:
 *   ANIM_KIND_MASK_OR_FILD = 0 = mask atlas (.wyc kind=0) or FILD
 *                               perspective band table (.fld)
 *   ANIM_KIND_RAW           = 2 = raw 8bpp frames (kind-flag == 0)
 *   ANIM_KIND_RICH          = 3 = RLE-compressed frames (kind-flag != 0) */
#define ANIM_KIND_MASK_OR_FILD          0
#define ANIM_KIND_RAW                   2
#define ANIM_KIND_RICH                  3

/* Perspective profile cap — g_persp_band_count is clamped to this so
 * multi-FILD merges can't overflow g_persp_profile[]. */
#define PERSP_BAND_MAX_COUNT            0x22

/* ---- helpers ------------------------------------------------------ */

/* Read a u16 table offset from the header at `off`, return a pointer
 * into raw at that offset. */
static uint16_t *anim_header_table(const void *raw, uint32_t off)
{
    uint16_t table_off = *(const uint16_t *)((const uint8_t *)raw + off);
    return (uint16_t *)((uint8_t *)raw + table_off);
}

/* Stash the source filename in a->name (truncated to fit). The basename
 * is used later for per-frame sound-trigger lookup (PlaySfx). */
static void anim_stash_name(AnimAsset *a, const char *name)
{
    if (!name) return;
    size_t nl = strlen(name);
    if (nl >= sizeof a->name) nl = sizeof a->name - 1;
    memcpy(a->name, name, nl);
    a->name[nl] = '\0';
}

/* Build pixel_ptrs[] from the 32-bit pix_off_table inside `raw`. The
 * pix-off table itself isn't guaranteed 4-byte aligned (its base is a
 * u16 in the header), so we memcpy each entry to avoid UB on strict-
 * alignment archs. The 32-bit offsets are widened to host pointers via
 * `raw + off`, which sidesteps the 32-bit slot vs 64-bit pointer
 * mismatch we'd hit if we tried to relocate them in place. */
static int anim_build_pixel_ptrs(AnimAsset *a)
{
    const uint8_t *pix_off_base = (const uint8_t *)a->raw_buffer
        + *(const uint16_t *)((const uint8_t *)a->raw_buffer
                              + ASSET_HEADER_OFF_PIX_OFF_TABLE);
    a->pixel_ptrs = (uint8_t **)xmalloc(
        a->frame_count * (uint32_t)sizeof(uint8_t *));
    if (!a->pixel_ptrs) return 0;
    for (uint16_t i = 0; i < a->frame_count; ++i) {
        uint32_t off;
        memcpy(&off, pix_off_base + i * ASSET_PIX_OFF_ENTRY_BYTES,
               ASSET_PIX_OFF_ENTRY_BYTES);
        a->pixel_ptrs[i] = (uint8_t *)a->raw_buffer + off;
    }
    return 1;
}

/* Walk frames and compute the bounding-box max(w, h) across all of
 * them — used by entity allocators to pick atlas slot sizes. */
static void anim_compute_bbox(AnimAsset *a)
{
    a->max_w = 0;
    a->max_h = 0;
    for (uint16_t i = 0; i < a->frame_count; ++i) {
        uint16_t w = a->off_widths[i];
        uint16_t h = a->off_heights[i];
        if (w > a->max_w) a->max_w = w;
        if (h > a->max_h) a->max_h = h;
    }
}

/* Decode the .wyc kind flag (u16 at offset 16): 0 = raw frames, non-
 * zero = RLE-compressed. Also looks up the asset's anim_script in the
 * global scripts object so per-frame VM dispatch finds it later. */
static void anim_decode_kind(AnimAsset *a, const char *name)
{
    uint16_t rich_flag = *(const uint16_t *)
        ((const uint8_t *)a->raw_buffer + ASSET_HEADER_OFF_KIND_OR_BODY);
    a->kind    = rich_flag != 0 ? ANIM_KIND_RICH : ANIM_KIND_RAW;
    a->flag_22 = rich_flag;
    if (g_scripts_obj) {
        a->anim_script = FindAnimationScript(g_scripts_obj, name);
    }
}

/* Parse the FILD body (perspective band table). Body layout:
 *   [count:i16][X[0..count-1]:i16][Y[0..count-1]:i16]
 * X/Y are signed (typically negative — back-offset from the asset
 * origin); we publish them as (off_drawX[0] - X) into the global
 * g_persp_profile[] so the band positions land at the correct screen
 * Y. */
static void anim_parse_fild_bands(AnimAsset *a)
{
    a->kind    = ANIM_KIND_MASK_OR_FILD;
    a->flag_22 = 0;

    uint16_t body_off = *(const uint16_t *)
        ((const uint8_t *)a->raw_buffer + ASSET_HEADER_OFF_KIND_OR_BODY);
    const int16_t *body = (const int16_t *)
        ((const uint8_t *)a->raw_buffer + body_off);
    int16_t band_cnt = body[0];

    int old = g_persp_band_count;
    g_persp_band_count += (int)band_cnt;
    if (g_persp_band_count > PERSP_BAND_MAX_COUNT) {
        g_persp_band_count = PERSP_BAND_MAX_COUNT;
    }
    int add = g_persp_band_count - old;

    const int16_t *p_x = &body[1];
    const int16_t *p_y = &body[1 + band_cnt];
    for (int b = 0; b < add; ++b) {
        g_persp_profile[(old + b) * 2 + 0] =
            (int16_t)(a->off_drawX[0] - p_x[b]);
        g_persp_profile[(old + b) * 2 + 1] =
            (int16_t)(a->off_drawY[0] - p_y[b]);
    }
}

/* ---- LoadAssetFromDtaBase ---------------------------------------- *
 *
 * Load a .wyc / .fld / .pic-like asset out of the .DTA archive,
 * parse its header, and build the runtime AnimAsset (pixel pointers,
 * bbox, kind, perspective bands). */
AnimAsset *LoadAssetFromDtaBase(const char *name)
{
    void    *raw  = NULL;
    uint32_t size = 0;
    if (!LoadFileFromDta(name, &raw, &size)) return NULL;

    uint32_t magic = *(uint32_t *)raw;
    if (magic != ASSET_MAGIC_ANIM &&
        magic != ASSET_MAGIC_MASK &&
        magic != ASSET_MAGIC_FILD)
    {
        xfree(raw);
        return NULL;
    }

    AnimAsset *a = (AnimAsset *)xmalloc(sizeof *a);
    if (!a) { xfree(raw); return NULL; }
    memset(a, 0, sizeof *a);

    a->raw_buffer  = raw;
    a->raw_size    = size;
    anim_stash_name(a, name);

    a->frame_count = *(uint16_t *)((uint8_t *)raw + ASSET_HEADER_OFF_FRAME_COUNT);
    a->off_widths  = anim_header_table(raw, ASSET_HEADER_OFF_WIDTH_TABLE);
    a->off_heights = anim_header_table(raw, ASSET_HEADER_OFF_HEIGHT_TABLE);
    a->off_drawX   = anim_header_table(raw, ASSET_HEADER_OFF_DRAWX_TABLE);
    a->off_drawY   = anim_header_table(raw, ASSET_HEADER_OFF_DRAWY_TABLE);

    if (!anim_build_pixel_ptrs(a)) { xfree(raw); xfree(a); return NULL; }
    anim_compute_bbox(a);

    if (magic == ASSET_MAGIC_ANIM) {
        anim_decode_kind(a, name);
    } else if (magic == ASSET_MAGIC_FILD) {
        anim_parse_fild_bands(a);
    } else {
        a->kind    = ANIM_KIND_MASK_OR_FILD;
        a->flag_22 = 0;
    }

    return a;
}

void FreeAsset(AnimAsset *a)
{
    if (!a) return;
    if (a->pixel_ptrs) xfree(a->pixel_ptrs);
    if (a->raw_buffer) xfree(a->raw_buffer);
    xfree(a);
}
