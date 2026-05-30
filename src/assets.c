/*
 * assets.c — ANIM (.wyc), MASK (.msk), FILD (.fld) loaders.
 *
 * Original address:
 * LoadAssetFromDtaBase 0x00405AA0
 *
 * All assets share the same in-file layout:
 *
 * +0 DWORD magic ("ANIM" / "MASK" / "FILD")
 * +4 WORD count
 * +6 WORD off_width_table (relative)
 * +8 WORD off_height_table
 * +10 WORD off_drawX_table
 * +12 WORD off_drawY_table
 * +14 WORD off_pixel_table (DWORD-of-offsets table; each → frame bitmap)
 *
 * For FILD the trailing data after the tables is a sequence of (Δx, Δy)
 * pairs that get appended to the global perspective profile g_persp_profile.
 */
#include "wacki.h"
#include <string.h>

extern void *xmalloc(uint32_t sz);
extern void  xfree  (void *p);

/* Scripts-class lookup (for [animacja]<filename> binding) */
extern void *g_scripts_obj;                          /* */
extern void *FindAnimationScript(void *scripts_obj, const char *name); /* */

/* Global perspective profile (filled by .fld loads). */
int16_t g_persp_profile[0x22*2];   /* g_persp_profile — pairs (dx, dy) */
int     g_persp_band_count = 0;    /* perspective_band_count_alt */

extern uint32_t g_tick_counter;    /* g_tick_counter */

/* CD anti-piracy watchdog REMOVED — rule #7 (no CD checks, no copy
 * protection). Original re-verified WACKI_1 volume label every ~150k
 * ticks via GetVolumeInformationA; we ship as a fully portable binary
 * that doesn't care where the .dta files came from. */

/* ------------------------------------------------------------------------- *
 * LoadAssetFromDtaBase — 0x00405AA0
 * ------------------------------------------------------------------------- */
AnimAsset *LoadAssetFromDtaBase(const char *name)
{
    void    *raw = NULL;
    uint32_t size = 0;
    if (!LoadFileFromDta(name, &raw, &size)) return NULL;

    uint32_t magic = *(uint32_t *)raw;
    if (magic != ASSET_MAGIC_ANIM &&
        magic != ASSET_MAGIC_MASK &&
        magic != ASSET_MAGIC_FILD) { xfree(raw); return NULL; }

    AnimAsset *a = (AnimAsset *)xmalloc(sizeof *a);
    if (!a) { xfree(raw); return NULL; }
    memset(a, 0, sizeof *a);

    a->raw_buffer = raw;
    a->raw_size   = size;
    /* Stash basename for per-frame sound-trigger lookup (PlaySfx). */
    if (name) {
        size_t nl = strlen(name);
        if (nl >= sizeof a->name) nl = sizeof a->name - 1;
        memcpy(a->name, name, nl);
        a->name[nl] = '\0';
    }
    a->frame_count = *(uint16_t *)((uint8_t *)raw + 4);
    a->off_widths  = (uint16_t *)((uint8_t *)raw + *(uint16_t *)((uint8_t *)raw + 6));
    a->off_heights = (uint16_t *)((uint8_t *)raw + *(uint16_t *)((uint8_t *)raw + 8));
    a->off_drawX   = (uint16_t *)((uint8_t *)raw + *(uint16_t *)((uint8_t *)raw + 10));
    a->off_drawY   = (uint16_t *)((uint8_t *)raw + *(uint16_t *)((uint8_t *)raw + 12));

    /* The pixel-offset table is 32-bit offsets relative to raw. On the
 * 64-bit host we cannot relocate them in-place (a 32-bit slot can't
 * hold a 64-bit pointer); allocate a separate array of real pointers.
 * T43b: pix_off_table base isn't guaranteed 4-byte aligned (its
 * offset is read from a u16 in the header), so use memcpy to avoid
 * UB on strict-alignment archs. */
    uint8_t *pix_off_base = (uint8_t *)raw + *(uint16_t *)((uint8_t *)raw + 14);
    a->pixel_ptrs = (uint8_t **)xmalloc(a->frame_count * (uint32_t)sizeof(uint8_t *));
    if (!a->pixel_ptrs) { xfree(raw); xfree(a); return NULL; }
    for (uint16_t i = 0; i < a->frame_count; ++i) {
        uint32_t off;
        memcpy(&off, pix_off_base + i * 4u, 4);
        a->pixel_ptrs[i] = (uint8_t *)raw + off;
    }

    /* compute bbox */
    a->max_w = a->max_h = 0;
    for (uint16_t i = 0; i < a->frame_count; ++i) {
        uint16_t w = a->off_widths[i];
        uint16_t h = a->off_heights[i];
        if (w > a->max_w) a->max_w = w;
        if (h > a->max_h) a->max_h = h;
    }

    /* kind 2 = passive (raw 8bpp frames), 3 = rich (RLE-compressed per
 * ), 0 = mask/fild. The kind flag is a uint16 at byte
 * offset 16 of the raw filepiVar2[4] != 0`):
 * 0 = raw frames, non-zero = RLE.
 * The off_widths table starts at byte 18 by convention, so the test
 * `off_widths_offset > 16` from Ghidra is effectively always true and
 * was the reason an earlier port misread the flag — using
 * `off_widths[0]` (first frame width) instead of the actual marker. */
    if (magic == ASSET_MAGIC_ANIM) {
        uint16_t rich_flag = *(uint16_t *)((uint8_t *)raw + 16);
        a->kind = rich_flag != 0 ? 3 : 2;
        a->flag_22 = rich_flag;    /* preserve raw bits for case 0x30 + 0x2E */
        if (g_scripts_obj)
            a->anim_script = FindAnimationScript(g_scripts_obj, name);
    } else if (magic == ASSET_MAGIC_FILD) {
        a->kind = 0;
        a->flag_22 = 0;
        /* FILD body: z (LoadAssetFromDtaBase FILD branch).
 * Body offset = ushort at raw[16] (NOT raw[6] — wcześniejszy bug).
 * Layout w body:
 * [count:short][X[0]..X[count-1]:short][Y[0]..Y[count-1]:short]
 * Wartości X/Y są **signed shorts ujemne** (offset wstecz od asset
 * origin); formuła `band_pos = off_drawX[0] - raw` w efekcie DODAJE
 * magnitudę → sensowne pozycje (sprawdzone na wartow2.fld:
 * off_drawX[0]=6, raw_X[0]=-173 → band_X=179). */
        uint16_t body_off = *(uint16_t *)((const uint8_t *)raw + 16);
        const int16_t *body = (const int16_t *)((const uint8_t *)raw + body_off);
        int16_t band_cnt = body[0];
        int old = g_persp_band_count;
        g_persp_band_count += (int)band_cnt;
        if (g_persp_band_count > 0x22) g_persp_band_count = 0x22;
        int add = g_persp_band_count - old;
        const int16_t *p_x = &body[1];
        const int16_t *p_y = &body[1 + band_cnt];   /* Y after full X array */
        for (int b = 0; b < add; ++b) {
            g_persp_profile[(old + b) * 2 + 0] = (int16_t)(a->off_drawX[0] - p_x[b]);
            g_persp_profile[(old + b) * 2 + 1] = (int16_t)(a->off_drawY[0] - p_y[b]);
        }
    } else { /* MASK */
        a->kind = 0;
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
