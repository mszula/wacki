/*
 * graphics.c — portable 8-bpp software rasteriser.
 *
 * The original engine drove a DirectDraw primary surface; this version
 * draws into a flat shadow buffer (g_back_shadow) and asks the platform
 * layer to present it. The blitter API is preserved verbatim so that
 * BlitSpriteToBackbuffer / PaintImageToBackbuffer continue to match the
 * Ghidra decompile of WACKI.EXE @ 0x00411480 / 0x00411730.
 */
#include "wacki.h"
#include <string.h>
#include <stdlib.h>

/* ---- shadow buffer + palette ------------------------------------------- */
uint8_t  *g_back_shadow = NULL;
uint8_t   g_palette_rgb[256*3];
uint16_t  g_screen_w = WACKI_SCREEN_W;
uint16_t  g_screen_h = WACKI_SCREEN_H;
uint16_t  g_screen_w_dim = WACKI_SCREEN_W;
uint16_t  g_screen_h_dim = WACKI_SCREEN_H;

/* Scene-BG atlas copy — stub-pic komnaty (e.g. magaz3j where the table
 * .pic is a 1×1 palette placeholder) spawn their real BG as a kind=2
 * atlas entity with flag-0x60 in their enter_va. The one-shot blit
 * paths it to the backbuffer ONCE, then second_va destroys the entity
 * — freeing the atlas. So we copy the atlas frame's pixels here on the
 * one-shot path and own the copy ourselves; per-frame paint repaints
 * the BG image (not a backbuffer snapshot — earlier impl snapshot
 * captured whatever else was on backbuffer at the moment, including
 * leftover sprites from the prior komnata → "static silhouettes"
 * behind every moving thing). Freed on komnata transition. */
uint8_t  *g_scene_bg_atlas_copy = NULL;
uint16_t  g_scene_bg_atlas_w    = 0;
uint16_t  g_scene_bg_atlas_h    = 0;
int16_t   g_scene_bg_atlas_dx   = 0;
int16_t   g_scene_bg_atlas_dy   = 0;

void SaveSceneBgAtlas(int16_t dx, int16_t dy,
                      uint16_t w, uint16_t h, const uint8_t *src)
{
    if (!src || !w || !h) return;
    size_t n = (size_t)w * h;
    if (g_scene_bg_atlas_copy) { xfree(g_scene_bg_atlas_copy); g_scene_bg_atlas_copy = NULL; }
    g_scene_bg_atlas_copy = (uint8_t *)xmalloc((uint32_t)n);
    if (!g_scene_bg_atlas_copy) return;
    memcpy(g_scene_bg_atlas_copy, src, n);
    g_scene_bg_atlas_w  = w;
    g_scene_bg_atlas_h  = h;
    g_scene_bg_atlas_dx = dx;
    g_scene_bg_atlas_dy = dy;
}

void PaintSceneBgAtlasIfAny(void)
{
    if (!g_scene_bg_atlas_copy) return;
    PaintImageToBackbuffer(g_scene_bg_atlas_dx, g_scene_bg_atlas_dy,
                           g_scene_bg_atlas_w, g_scene_bg_atlas_h,
                           g_scene_bg_atlas_copy);
}

void FreeSceneBgAtlas(void)
{
    if (g_scene_bg_atlas_copy) { xfree(g_scene_bg_atlas_copy); g_scene_bg_atlas_copy = NULL; }
    g_scene_bg_atlas_w = g_scene_bg_atlas_h = 0;
    g_scene_bg_atlas_dx = g_scene_bg_atlas_dy = 0;
}

/* ---- dirty-rect tracking (kept for fidelity, not used by SDL preset) --- */
typedef struct { int16_t l, t, r, b; } GRect;
static GRect    s_dirty_this[WACKI_MAX_DIRTY_RECTS];
static uint16_t s_dirty_this_count = 0;

static void push_dirty(int x, int y, int w, int h)
{
    if (s_dirty_this_count >= WACKI_MAX_DIRTY_RECTS) {
        s_dirty_this[0] = (GRect){0, 0, g_screen_w, g_screen_h};
        s_dirty_this_count = 1;
        return;
    }
    s_dirty_this[s_dirty_this_count++] = (GRect){
        (int16_t)x, (int16_t)y, (int16_t)(x+w), (int16_t)(y+h)
    };
}

static void ensure_shadow(void)
{
    if (g_back_shadow) return;
    g_back_shadow = (uint8_t *)xmalloc((uint32_t)g_screen_w * g_screen_h);
    if (g_back_shadow) memset(g_back_shadow, 0, (size_t)g_screen_w * g_screen_h);
}

/* ------------------------------------------------------------------------- *
 * BlitSpriteToBackbuffer — 0x00411480
 * ------------------------------------------------------------------------- */
void BlitSpriteToBackbuffer(uint16_t dx, uint16_t dy,
                            uint16_t sx, uint16_t sy,
                            uint16_t cw, uint16_t ch,
                            uint16_t pw, uint16_t ph,
                            uint8_t *src, int16_t mode)
{
    ensure_shadow();
    if (!src || !g_back_shadow) return;

    int destX = (int16_t)dx, destY = (int16_t)dy;
    int cw_i = cw, ch_i = ch;
    int sx_off = sx, sy_off = sy;

    if (destX < 0) { sx_off -= destX; cw_i += destX; destX = 0; }
    if (destY < 0) { sy_off -= destY; ch_i += destY; destY = 0; }
    if (destX + cw_i > g_screen_w) cw_i = g_screen_w - destX;
    if (destY + ch_i > g_screen_h) ch_i = g_screen_h - destY;
    /* T136 — clip against source surface extent. Earlier `(void)ph;`
 * ignored the surface height entirely; sub-atlas blits with
 * (sx, sy) pointing past surface bounds would read garbage from
 * adjacent memory. Now: bail if requested sx/sy + extent exceeds
 * source surface (pw × ph). */
    if (ph != 0) {
        if (sy_off < 0) { ch_i += sy_off; destY -= sy_off; sy_off = 0; }
        if (sy_off + ch_i > (int)ph) ch_i = (int)ph - sy_off;
    }
    if (pw != 0) {
        if (sx_off < 0) { cw_i += sx_off; destX -= sx_off; sx_off = 0; }
        if (sx_off + cw_i > (int)pw) cw_i = (int)pw - sx_off;
    }
    if (cw_i <= 0 || ch_i <= 0) return;

    push_dirty(destX, destY, cw_i, ch_i);

    uint8_t *dst       = g_back_shadow + (size_t)destY * g_screen_w + destX;
    const uint8_t *ssrc= src + (size_t)sy_off * pw + sx_off;

    /* T104 audit (2026-05-27): all known port callers pass mode == 0.
 * Modes 1 (opaque memcpy) and 2 (palette-index avg) were unused dead
 * code — mode 2 in particular produced garbage colors because palette
 * indices don't blend linearly in an 8bpp paletted scheme. Original
 * binary doesn't use mode 2 either — (the "translucent"
 * blit) is actually a fill-transparent path (`if dst==0 dst=src`),
 * not averaging. Kept here only as gated bail-outs with logging so
 * any future caller is flagged. */
    for (int row = 0; row < ch_i; ++row) {
        switch (mode) {
        case 0: /* color-key 0 — the only used mode */
            for (int i = 0; i < cw_i; ++i)
                if (ssrc[i]) dst[i] = ssrc[i];
            break;
        case 1: /* opaque memcpy — historically used; kept for callers */
            memcpy(dst, ssrc, (size_t)cw_i);
            break;
        case 2:
            /* NOTE: mode 2 in port was a
 * palette-index avg approximation. Original is
 * actually fill-transparent (dst=src where dst==0). Until a
 * caller actually needs it, mirror that semantic. */
            for (int i = 0; i < cw_i; ++i)
                if (dst[i] == 0) dst[i] = ssrc[i];
            break;
        }
        dst  += g_screen_w;
        ssrc += pw;
    }
}

/* ------------------------------------------------------------------------- *
 * PaintImageToBackbuffer — 0x00411730
 * ------------------------------------------------------------------------- */
void PaintImageToBackbuffer(int16_t dx, int16_t dy,
                            uint16_t cw, uint16_t ch,
                            const uint8_t *src)
{
    ensure_shadow();
    if (!src || !g_back_shadow) return;

    int destX = dx, destY = dy;
    int cw_i = cw, ch_i = ch;
    int sx_off = 0, sy_off = 0;

    if (destX < 0) { sx_off = -destX; cw_i += destX; destX = 0; }
    if (destY < 0) { sy_off = -destY; ch_i += destY; destY = 0; }
    if (destX + cw_i > g_screen_w) cw_i = g_screen_w - destX;
    if (destY + ch_i > g_screen_h) ch_i = g_screen_h - destY;
    if (cw_i <= 0 || ch_i <= 0) return;

    push_dirty(destX, destY, cw_i, ch_i);

    uint8_t *dst       = g_back_shadow + (size_t)destY * g_screen_w + destX;
    const uint8_t *ssrc= src + (size_t)sy_off * cw + sx_off;
    for (int row = 0; row < ch_i; ++row) {
        memcpy(dst, ssrc, (size_t)cw_i);
        dst  += g_screen_w;
        ssrc += cw;
    }
}

void FlipBuffersClearWith(uint8_t v)
{
    ensure_shadow();
    if (g_back_shadow)
        memset(g_back_shadow, v, (size_t)g_screen_w * g_screen_h);
    s_dirty_this[0] = (GRect){0,0,g_screen_w,g_screen_h};
    s_dirty_this_count = 1;
}

/* ------------------------------------------------------------------------- *
 * FlushFrameToPrimary — 0x004119B0
 * Sends the shadow to the screen via the platform layer.
 * ------------------------------------------------------------------------- */
void FlushFrameToPrimary(void)
{
    ensure_shadow();
    if (g_back_shadow)
        PlatformPresent(g_back_shadow, g_palette_rgb, g_screen_w, g_screen_h);
    s_dirty_this_count = 0;
}

/* ------------------------------------------------------------------------- *
 * RestorePrevFrameRects — 0x00412410
 * No-op in the SDL build (we redraw the whole shadow each frame).
 * ------------------------------------------------------------------------- */
void RestorePrevFrameRects(void) { /* nop */ }

/* ------------------------------------------------------------------------- *
 * BlitSpriteScaledColorKey — nearest-neighbor scaled colour-key blit.
 *
 * Used for perspective-scaled actor rendering. The original engine does
 * this inside the per-entity render path ( case scaled-actor
 * branch + stretched-blit) controlled by entity->scale_pct
 * (= entity+0x58, computed each tick by UpdateActorMovement from the
 * actor's foot-Y and the stage perspective parameters g_cursor_speed /
 * g_perspective_min / g_perspective_step).
 *
 * Parameters:
 * dx, dy — destination top-left (already adjusted for scaled size)
 * sw, sh — source frame size (raw atlas)
 * dw, dh — destination size (= sw*scale/100, sh*scale/100)
 * src — source pixels (sw × sh, 8 bpp)
 * Skips palette index 0 (transparent).
 * ------------------------------------------------------------------------- */
void BlitSpriteScaledColorKey(int16_t dx, int16_t dy,
                              uint16_t sw, uint16_t sh,
                              uint16_t dw, uint16_t dh,
                              const uint8_t *src)
{
    BlitSpriteScaledColorKeyFlip(dx, dy, sw, sh, dw, dh, src, 0);
}

/* Same as above but with horizontal mirror flag (used for actors walking
 * right — the original engine stores ebek/fjej atlases facing LEFT only
 * and mirrors at render time via + entity flags). */
void BlitSpriteScaledColorKeyFlip(int16_t dx, int16_t dy,
                                  uint16_t sw, uint16_t sh,
                                  uint16_t dw, uint16_t dh,
                                  const uint8_t *src, int flip_h)
{
    ensure_shadow();
    if (!src || !g_back_shadow || dw == 0 || dh == 0 || sw == 0 || sh == 0) return;
    if (dw > 0x400 || dh > 0x400) return;

    int destX0 = dx, destY0 = dy;
    int destX1 = dx + dw, destY1 = dy + dh;
    if (destX0 < 0) destX0 = 0;
    if (destY0 < 0) destY0 = 0;
    if (destX1 > g_screen_w) destX1 = g_screen_w;
    if (destY1 > g_screen_h) destY1 = g_screen_h;
    if (destX0 >= destX1 || destY0 >= destY1) return;

    push_dirty(destX0, destY0, destX1 - destX0, destY1 - destY0);

    /* T33: mode 0 — x-step LUT +
 * y-extra-row LUT. Bresenham-style accumulator. */
    static uint32_t x_step[0x401];
    static uint8_t  y_extra[0x401];
    {
        uint32_t base = sw / dw, rem = sw % dw, acc = rem, cur = base;
        for (uint32_t i = 0; i < dw; ++i) {
            x_step[i] = cur;
            acc += rem; cur = base;
            if ((int32_t)acc >= (int32_t)dw) { acc -= dw; cur += 1; }
        }
    }
    {
        uint32_t rem = sh % dh, acc = rem;
        for (uint32_t i = 0; i < dh; ++i) {
            int extra = ((int32_t)acc >= (int32_t)dh);
            y_extra[i] = (uint8_t)extra;
            if (extra) acc -= dh;
            acc += rem;
        }
    }

    /* Walk src rows via base step + y_extra (mirrors mode 0 inner loop
 * in ). For each dst row, x_step[dx] is how many src
 * columns to advance after sampling. flip_h inverts the x-offset
 * (sw-1-x) — original engine flips at render time, atlases face L. */
    const uint8_t *srow      = src;
    int            row_step  = (int)(sh / dh) * (int)sw;
    /* Skip src rows above clipped destY0. */
    for (int dy_off = dy; dy_off < destY0; ++dy_off) {
        srow += row_step;
        if (y_extra[dy_off - dy]) srow += sw;
    }
    for (int dy_off = destY0; dy_off < destY1; ++dy_off) {
        uint8_t       *drow = g_back_shadow + (size_t)dy_off * g_screen_w;
        const uint8_t *sp   = srow;
        /* Skip src cols left of clipped destX0. */
        for (int dx_off = dx; dx_off < destX0; ++dx_off)
            sp += x_step[dx_off - dx];
        for (int dx_off = destX0; dx_off < destX1; ++dx_off) {
            int sx = (int)(sp - srow);
            if (flip_h) sx = (int)sw - 1 - sx;
            if (sx >= 0 && sx < (int)sw) {
                uint8_t v = srow[sx];
                if (v) drow[dx_off] = v;
            }
            sp += x_step[dx_off - dx];
        }
        srow += row_step;
        if (y_extra[dy_off - dy]) srow += sw;
    }
}

/* ------------------------------------------------------------------------- *
 * DepackRleFrame
 *
 * The "rich" ANIM encoding (asset kind=3 in LoadAssetFromDtaBase, i.e.
 * frame_count > 16 and first-frame's first param non-zero) stores each
 * frame as an RLE stream with a 3-byte header:
 * header[0] = fill_value (the colour to emit for skip runs;
 * usually 0 = palette idx 0 = transparent)
 * header[1] = marker_A (any input byte equal to A introduces a
 * run of fill_value)
 * header[2] = marker_B (introduces a run of an arbitrary literal)
 *
 * Stream (bytes after the header):
 * for each input byte b:
 * if b == marker_A: count = next_byte + 1;
 * emit `count` copies of fill_value
 * if b == marker_B: count = next_byte + 1; value = next_byte;
 * emit `count` copies of value
 * else: emit b once
 * Loop until `dst_len` output bytes written. Returns nothing.
 * ------------------------------------------------------------------------- */
void DepackRleFrame(const uint8_t *src, uint8_t *dst, int dst_len)
{
    if (!src || !dst || dst_len <= 0) return;
    uint8_t  fill     = src[0];
    uint8_t  marker_A = src[1];
    uint8_t  marker_B = src[2];
    const uint8_t *p   = src + 3;
    uint8_t       *d   = dst;
    uint8_t       *end = dst + dst_len;
    while (d < end) {
        uint8_t b = *p++;
        if (b == marker_A) {
            int count = (int)(*p++) + 1;
            uint8_t v = fill;
            while (count-- > 0 && d < end) *d++ = v;
        } else if (b == marker_B) {
            int count = (int)(*p++) + 1;
            uint8_t v = *p++;
            while (count-- > 0 && d < end) *d++ = v;
        } else {
            *d++ = b;
        }
    }
}

/* ------------------------------------------------------------------------- *
 * InstallPalette — 0x00412D10
 * Copies bytes into g_palette_rgb (BGR triplets in the original; we keep
 * the same byte order so .pal files load unmodified).
 *
 * T7: also rebuilds the RGB12 quantization LUTs used by the alpha-plane
 * scaled blit ( mode 1/2 box-filter paths).
 * ------------------------------------------------------------------------- */
extern void RebuildAlphaQuantLuts(void);
void InstallPalette(const uint8_t *rgb, uint16_t first)
{
    if (!rgb || first >= 256) return;
    int count = 256 - first;
    memcpy(g_palette_rgb + first*3, rgb, (size_t)count * 3);
    /* T7: rebuild alpha-plane RGB12 quantization LUTs. */
    RebuildAlphaQuantLuts();
}

/* ========================================================================= *
 * Alpha-plane scaled blit
 * + + .
 *
 * Used by per-entity VM for entities with flag 0x100 + 0x400 (alpha-plane
 * + perspective scaled). Mode 0 = nearest-neighbor (also handled by the
 * existing BlitSpriteScaledColorKey path). Mode 1 = 1D horizontal box
 * filter with RGB12 quantization. Mode 2 = 2D box filter (full alpha).
 *
 * Original engine caches the 4096-entry inverse LUT to disk via the
 * palette path; we rebuild from scratch each InstallPalette (still fast
 * enough at < 100ms for a one-time cost).
 * ========================================================================= */

/* : palette → RGB12 nibble triplets (256 × 4 bytes;
 * byte 0=R, byte 1=G, byte 2=B — matches g_palette_rgb byte order
 * (.PAL files are RGB-ordered)). */
static uint8_t  s_pal_rgb12[256][4];
/* : RGB12 → palette idx. RGB12 index layout (per original
 * : `idx = sum_r + (sum_b * 0x10 + sum_g) * 0x10`):
 * bits 0-3 = R nibble (LOW)
 * bits 4-7 = G nibble (MIDDLE)
 * bits 8-11 = B nibble (HIGH)
 * (R-low, B-high nibble layout — NOT a typical RGB565-style ordering.) */
static uint8_t  s_rgb12_to_pal[4096];
/* Tint state. Original = 0x808080 = identity. Encoding
 * matches : `tint_r = param & 0xff; tint_g = (param >> 8)
 * & 0xff; tint_b = (param >> 16) & 0xff` — so the param is "RGB-encoded"
 * uint32 with R in LOW byte. */
static uint32_t s_tint_color = 0x808080;
static uint32_t s_tint_r = 0x80, s_tint_g = 0x80, s_tint_b = 0x80;

/* RebuildAlphaQuantLuts head + brute-force build.
 *
 * Distance weights from original ( inner loop):
 * R diff × 900 (= 0x384)
 * G diff × 3481 (= 0xD99)
 * B diff × 121 (= 0x79)
 * These approximate BT.601 perceived luminance — G strongest, B weakest.
 *
 * T116 reverted (2026-05-27) — earlier port added a `Wacki.444` disk
 * cache mirroring the original ( head reads `.444` file
 * via ). The cache made sense on a 1997 Pentium 90 where
 * brute-forcing 4096 × 256 = 1M iterations took ~50ms. Modern CPU
 * benchmark: **1.66 ms / build** — perceptibly free.
 *
 * Original's disk cache was a perf optimization for the era, not a
 * functional requirement. Removed to drop the extra disk file +
 * stale-cache risk + complexity. Brute-force every InstallPalette call;
 * the output bytes are byte-identical to what the cache would yield. */
void RebuildAlphaQuantLuts(void)
{
    /* Step 1: copy palette to s_pal_rgb12 (byte order R/G/B/0). */
    for (int i = 0; i < 256; ++i) {
        s_pal_rgb12[i][0] = g_palette_rgb[i*3 + 0] >> 4;  /* R nibble */
        s_pal_rgb12[i][1] = g_palette_rgb[i*3 + 1] >> 4;  /* G nibble */
        s_pal_rgb12[i][2] = g_palette_rgb[i*3 + 2] >> 4;  /* B nibble */
        s_pal_rgb12[i][3] = 0;
    }
    /* Step 2: brute-force inverse LUT — 256 candidates per 4096 RGB12 keys.
 * ~1.66 ms on modern CPU; no caching needed. */
    for (int rgb12 = 0; rgb12 < 4096; ++rgb12) {
        int r = (rgb12 >> 0) & 0xf;   /* LOW nibble = R */
        int g = (rgb12 >> 4) & 0xf;   /* MIDDLE nibble = G */
        int b = (rgb12 >> 8) & 0xf;   /* HIGH nibble = B */
        int best_idx = 1, best_dist = 0x7FFFFFFF;
        for (int p = 1; p < 256; ++p) {
            int dr = r - s_pal_rgb12[p][0];
            int dg = g - s_pal_rgb12[p][1];
            int db = b - s_pal_rgb12[p][2];
            int dist = dr*dr*900 + dg*dg*3481 + db*db*121;
            if (dist < best_dist) { best_dist = dist; best_idx = p; }
            if (dist == 0) break;
        }
        s_rgb12_to_pal[rgb12] = (uint8_t)best_idx;
    }
    /* Reset tint to identity. */
    s_tint_color = 0x808080;
    s_tint_r = 0x80;
    s_tint_g = 0x80;
    s_tint_b = 0x80;
}

/* SetAlphaTint Sets tint multiplier as
 * RGB-encoded uint32 (R=LOW byte, B=HIGH byte). 0x808080 = identity
 * (no tint). Values < 0x80 darken, > 0x80 brighten the corresponding
 * channel. */
void SetAlphaTint(uint32_t rgb)
{
    s_tint_color = rgb;
    s_tint_r = rgb & 0xff;
    s_tint_g = (rgb >> 8) & 0xff;
    s_tint_b = (rgb >> 16) & 0xff;
}

/* sample_box_1d Averages src pixels across a
 * row span; non-zero pixels contribute to running R/G/B totals. Returns
 * the nearest palette index for the averaged RGB12 value. */
static uint8_t sample_box_1d(const uint8_t *p, int count)
{
    int sum_r = 0, sum_g = 0, sum_b = 0, n = 0;
    if (count < 1) count = 1;
    for (int i = 0; i < count; ++i) {
        uint8_t v = p[i];
        if (v) {
            sum_r += s_pal_rgb12[v][0];
            sum_g += s_pal_rgb12[v][1];
            sum_b += s_pal_rgb12[v][2];
            ++n;
        }
    }
    if (n == 0) return 0;
    if (n > 1) {
        sum_r = (sum_r / n) & 0xf;
        sum_g = (sum_g / n) & 0xf;
        sum_b = (sum_b / n) & 0xf;
    }
    if (s_tint_color != 0x808080) {
        sum_r = (sum_r * (int)s_tint_r) >> 7; if (sum_r > 0xf) sum_r = 0xf;
        sum_g = (sum_g * (int)s_tint_g) >> 7; if (sum_g > 0xf) sum_g = 0xf;
        sum_b = (sum_b * (int)s_tint_b) >> 7; if (sum_b > 0xf) sum_b = 0xf;
    }
    /* rgb12 layout: R=LOW, G=MIDDLE, B=HIGH (matches lookup). */
    return s_rgb12_to_pal[sum_r | (sum_g << 4) | (sum_b << 8)];
}

/* sample_box_2d Same as 1D but spans `height`
 * rows × `width` cols around the source pixel. `src_stride` is the
 * source row pitch (= src_w). */
static uint8_t sample_box_2d(const uint8_t *p, int width, int src_stride,
                             int height)
{
    int sum_r = 0, sum_g = 0, sum_b = 0, n = 0;
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    for (int y = 0; y < height; ++y) {
        const uint8_t *row = p + y * src_stride;
        for (int x = 0; x < width; ++x) {
            uint8_t v = row[x];
            if (v) {
                sum_r += s_pal_rgb12[v][0];
                sum_g += s_pal_rgb12[v][1];
                sum_b += s_pal_rgb12[v][2];
                ++n;
            }
        }
    }
    if (n == 0) return 0;
    if (n > 1) {
        sum_r = (sum_r / n) & 0xf;
        sum_g = (sum_g / n) & 0xf;
        sum_b = (sum_b / n) & 0xf;
    }
    if (s_tint_color != 0x808080) {
        sum_r = (sum_r * (int)s_tint_r) >> 7; if (sum_r > 0xf) sum_r = 0xf;
        sum_g = (sum_g * (int)s_tint_g) >> 7; if (sum_g > 0xf) sum_g = 0xf;
        sum_b = (sum_b * (int)s_tint_b) >> 7; if (sum_b > 0xf) sum_b = 0xf;
    }
    /* rgb12 layout: R=LOW, G=MIDDLE, B=HIGH (matches lookup). */
    return s_rgb12_to_pal[sum_r | (sum_g << 4) | (sum_b << 8)];
}

/* BlitAlphaScaled Caller passes a packed
 * "blit struct" similar to original ..E0:
 * { src_w, src_h, src_ptr (8B), dst_w, dst_h, dst_ptr (8B), mode }
 * but we take individual args for clarity.
 *
 * Modes:
 * 0 = nearest-neighbor with x-step LUT (each src pixel maps 1→N dst)
 * 1 = 1D horizontal box filter (averages along rows)
 * 2 = 2D box filter (averages over `step_y` rows × `step_x` cols)
 *
 * Returns immediately if any dim is 0 or > 0x400 (matches original guard). */
/* BlitAlphaScaledToBackbuffer — convenience wrapper that allocates a
 * scratch dst buffer of `dw × dh`, runs BlitAlphaScaled (mode 0/1/2),
 * and color-key-blits to the shadow buffer (palette idx 0 transparent).
 *
 * Used by EntityRenderAll for entities with flag 0x100 + 0x400 (alpha +
 * perspective scaled). All three modes shipped:
 * mode 0 = nearest-neighbor with x-step LUT (matches )
 * mode 1 = 1D horizontal box filter + RGB12 quantization
 * mode 2 = 2D box filter + RGB12 quantization
 * Non-alpha scaled actors use BlitSpriteScaledColorKeyFlip directly,
 * which now uses the same x-step LUT (T33). */
void BlitAlphaScaledToBackbuffer(int16_t dx, int16_t dy,
                                 uint16_t sw, uint16_t sh,
                                 uint16_t dw, uint16_t dh,
                                 const uint8_t *src, uint16_t mode)
{
    if (!src || dw == 0 || dh == 0) return;
    /* Scratch buffer for the scaled output. Reuse across calls to
 * avoid per-frame malloc churn. */
    static uint8_t *s_alpha_scratch    = NULL;
    static size_t   s_alpha_scratch_sz = 0;
    size_t need = (size_t)dw * (size_t)dh;
    if (need > s_alpha_scratch_sz) {
        free(s_alpha_scratch);
        s_alpha_scratch = (uint8_t *)malloc(need);
        s_alpha_scratch_sz = s_alpha_scratch ? need : 0;
    }
    if (!s_alpha_scratch) return;
    BlitAlphaScaled(sw, sh, src, dw, dh, s_alpha_scratch, mode);
    /* Blit scratch → shadow buffer with color-key (palette idx 0
 * transparent). Reuse the existing src=8bpp path. */
    BlitSpriteToBackbuffer((uint16_t)dx, (uint16_t)dy, 0, 0, dw, dh,
                           dw, dh, s_alpha_scratch, 0);
}

void BlitAlphaScaled(uint16_t src_w, uint16_t src_h, const uint8_t *src,
                     uint16_t dst_w, uint16_t dst_h, uint8_t *dst,
                     uint16_t mode)
{
    if (!src || !dst) return;
    if (!src_w || !src_h || !dst_w || !dst_h) return;
    if (dst_w > 0x400 || dst_h > 0x400) return;

    /* Build x-step LUT: each dst column gets src_w/dst_w
 * with Bresenham-style accumulator for the remainder. */
    static uint32_t x_step[0x401];
    {
        uint32_t base = src_w / dst_w;
        uint32_t rem  = src_w % dst_w;
        uint32_t acc  = rem;
        uint32_t cur  = base;
        for (uint32_t i = 0; i < dst_w; ++i) {
            x_step[i] = cur;
            acc += rem;
            cur = base;
            if ((int32_t)acc >= (int32_t)dst_w) {
                acc -= dst_w;
                cur += 1;
            }
        }
    }
    /* Build y-extra-row flag table: each dst row decides
 * whether to advance an extra src row. */
    static uint8_t y_extra[0x401];
    {
        uint32_t rem = src_h % dst_h;
        uint32_t acc = rem;
        for (uint32_t i = 0; i < dst_h; ++i) {
            int extra = ((int32_t)acc >= (int32_t)dst_h);
            y_extra[i] = (uint8_t)extra;
            if (extra) acc -= dst_h;
            acc += rem;
        }
    }
    const uint8_t *srcrow = src;
    uint8_t       *dstrow = dst;
    int            src_step_row = (src_h / dst_h) * src_w;

    if (mode == 0) {
        /* Nearest-neighbor with x-step. */
        for (uint32_t dy = 0; dy < dst_h; ++dy) {
            const uint32_t *step = x_step;
            const uint8_t  *sp   = srcrow;
            for (uint32_t dx = 0; dx < dst_w; ++dx) {
                dstrow[dx] = *sp;
                sp += *step++;
            }
            srcrow += src_step_row;
            if (y_extra[dy]) srcrow += src_w;
            dstrow += dst_w;
        }
    } else if (mode == 1) {
        /* 1D horizontal box filter. */
        for (uint32_t dy = 0; dy < dst_h; ++dy) {
            const uint32_t *step = x_step;
            const uint8_t  *sp   = srcrow;
            for (uint32_t dx = 0; dx < dst_w; ++dx) {
                int sw = (int)*step;
                if (sw < 1) sw = 1;
                dstrow[dx] = sample_box_1d(sp, sw);
                sp += *step++;
            }
            srcrow += src_step_row;
            if (y_extra[dy]) srcrow += src_w;
            dstrow += dst_w;
        }
    } else if (mode == 2) {
        /* 2D box filter — span = step_x × step_y. */
        uint32_t y_base = src_h / dst_h;
        uint32_t y_acc  = src_h % dst_h;
        for (uint32_t dy = 0; dy < dst_h; ++dy) {
            uint32_t sh = y_base + (y_extra[dy] ? 1 : 0);
            if (sh == 0) sh = 1;
            if (dy + sh > dst_h) sh = (uint32_t)(dst_h - dy);
            const uint32_t *step = x_step;
            const uint8_t  *sp   = srcrow;
            for (uint32_t dx = 0; dx < dst_w; ++dx) {
                int sw = (int)*step;
                if (sw == 0) sw = 1;
                dstrow[dx] = sample_box_2d(sp, sw, src_w, (int)sh);
                sp += *step++;
            }
            srcrow += src_step_row;
            if (y_extra[dy]) srcrow += src_w;
            dstrow += dst_w;
            (void)y_acc;
        }
    }
}
