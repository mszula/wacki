/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/graphics.c — portable 8-bpp software rasteriser.
 *
 * The original engine drove a DirectDraw primary surface; this port
 * draws into a flat shadow buffer (g_back_shadow) and asks the
 * platform layer to present it. The blitter API is preserved
 * verbatim so that BlitSpriteToBackbuffer / PaintImageToBackbuffer
 * continue to match the original semantics for every caller. */
#include "wacki.h"
#include <string.h>
#include <stdlib.h>

/* ---- constants --------------------------------------------------- */

/* Sanity cap on the destination size accepted by the scaled-blit
 * paths — anything larger is almost certainly a scale-pct overflow.
 * 0x400 = 1024 px, far above any actor sprite. The static LUT arrays
 * (x_step, y_extra) are sized to SCALED_BLIT_MAX_DIM + 1. */
#define SCALED_BLIT_MAX_DIM     0x400
#define SCALED_BLIT_LUT_SIZE    (SCALED_BLIT_MAX_DIM + 1)

/* ---- shadow buffer + palette ------------------------------------- */
uint8_t  *g_back_shadow = NULL;
uint8_t   g_palette_rgb[256*3];
uint16_t  g_screen_w = WACKI_SCREEN_W;
uint16_t  g_screen_h = WACKI_SCREEN_H;
uint16_t  g_screen_w_dim = WACKI_SCREEN_W;
uint16_t  g_screen_h_dim = WACKI_SCREEN_H;

/* ---- persistent one-shot-BG bake layer ----------------------------- *
 *
 * Entities flagged EFLAG_FADE_OR_BG | EFLAG_ONESHOT_BG_PEND (flag-0x60
 * spawns, plus per-entity fade bakes) paint their current frame into the
 * scene background ONCE, then the source entity is usually destroyed. The
 * original engine accumulates every such bake into one persistent BG page
 * (FUN_00411730) and repaints from it, so any number of bakes coexist
 * permanently — a stub .pic's full background, the korlab5 airlock door,
 * the card-reader buttons.
 *
 * We mirror that with a screen-sized pixel buffer + 1-byte coverage mask.
 * SaveSceneBgAtlas copies the (clean) atlas pixels in and marks the rect;
 * PaintSceneBgAtlasIfAny overlays the marked pixels onto the
 * freshly-painted .pic every frame. Storing clean atlas pixels (never a
 * backbuffer snapshot) avoids the "static silhouette" artefact a snapshot
 * baked in. A re-bake over the same area overwrites in place, so
 * open→close→open toggles always show the latest state. A dirty bbox
 * keeps the per-frame overlay cheap. Freed on komnata transition.
 *
 * NOTE: replaces an earlier single-atlas slot that held only ONE bake and
 * gated saves on width ≥ 320 px — which dropped narrow scene props, most
 * visibly the korlab5 airlock door (~165 px), so it reverted to the
 * .pic's closed state the instant the per-frame repaint ran. The width
 * gate only existed to stop a small prop from evicting the real BG out of
 * that single slot; the layer makes both the gate and the slot moot,
 * matching the original's un-gated accumulation. */
static uint8_t *s_bg_bake_px   = NULL;   /* screen-sized accumulated pixels */
static uint8_t *s_bg_bake_mask = NULL;   /* 1 where a bake covers the pixel */
static int      s_bg_bake_x0, s_bg_bake_y0;   /* dirty bbox of baked area;  */
static int      s_bg_bake_x1, s_bg_bake_y1;   /* x1<=x0 ⇒ nothing baked yet */

static int ensure_bg_bake_layer(void)
{
    size_t n = (size_t)g_screen_w * g_screen_h;
    if (!s_bg_bake_px) s_bg_bake_px = (uint8_t *)xmalloc((uint32_t)n);
    if (!s_bg_bake_mask) {
        s_bg_bake_mask = (uint8_t *)xmalloc((uint32_t)n);
        if (s_bg_bake_mask) memset(s_bg_bake_mask, 0, n);
    }
    return s_bg_bake_px && s_bg_bake_mask;
}

void SaveSceneBgAtlas(int16_t dx, int16_t dy,
                      uint16_t w, uint16_t h, const uint8_t *src)
{
    if (!src || !w || !h || !ensure_bg_bake_layer()) return;

    /* Clip (dx,dy,w,h) to the screen, tracking the source offset so a
     * partially off-screen bake still copies the right pixels. */
    int x0 = dx, y0 = dy, x1 = dx + (int)w, y1 = dy + (int)h;
    int sx = 0, sy = 0;
    if (x0 < 0) { sx = -x0; x0 = 0; }
    if (y0 < 0) { sy = -y0; y0 = 0; }
    if (x1 > (int)g_screen_w) x1 = (int)g_screen_w;
    if (y1 > (int)g_screen_h) y1 = (int)g_screen_h;
    if (x0 >= x1 || y0 >= y1) return;

    for (int y = y0; y < y1; ++y) {
        const uint8_t *s   = src + (size_t)(sy + (y - y0)) * w + sx;
        size_t         off = (size_t)y * g_screen_w + x0;
        memcpy(s_bg_bake_px   + off, s, (size_t)(x1 - x0));
        memset(s_bg_bake_mask + off, 1, (size_t)(x1 - x0));
    }

    if (s_bg_bake_x1 <= s_bg_bake_x0) {        /* first bake this scene */
        s_bg_bake_x0 = x0; s_bg_bake_y0 = y0;
        s_bg_bake_x1 = x1; s_bg_bake_y1 = y1;
    } else {
        if (x0 < s_bg_bake_x0) s_bg_bake_x0 = x0;
        if (y0 < s_bg_bake_y0) s_bg_bake_y0 = y0;
        if (x1 > s_bg_bake_x1) s_bg_bake_x1 = x1;
        if (y1 > s_bg_bake_y1) s_bg_bake_y1 = y1;
    }
}

void PaintSceneBgAtlasIfAny(void)
{
    if (!s_bg_bake_px || !s_bg_bake_mask || !g_back_shadow) return;
    if (s_bg_bake_x1 <= s_bg_bake_x0) return;          /* nothing baked yet */

    for (int y = s_bg_bake_y0; y < s_bg_bake_y1; ++y) {
        size_t         row = (size_t)y * g_screen_w;
        const uint8_t *mk  = s_bg_bake_mask + row;
        const uint8_t *px  = s_bg_bake_px   + row;
        uint8_t       *bb  = g_back_shadow  + row;
        for (int x = s_bg_bake_x0; x < s_bg_bake_x1; ++x)
            if (mk[x]) bb[x] = px[x];
    }
}

void FreeSceneBgAtlas(void)
{
    if (s_bg_bake_px)   { xfree(s_bg_bake_px);   s_bg_bake_px   = NULL; }
    if (s_bg_bake_mask) { xfree(s_bg_bake_mask); s_bg_bake_mask = NULL; }
    s_bg_bake_x0 = s_bg_bake_y0 = s_bg_bake_x1 = s_bg_bake_y1 = 0;
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

/* ---- BlitSpriteToBackbuffer -------------------------------------- */
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
    /* T136 — clip against source surface extent. An earlier `(void)
     * ph;` ignored the surface height entirely; sub-atlas blits with
     * (sx, sy) pointing past surface bounds would read garbage from
     * adjacent memory. Now: bail if the requested sx/sy + extent
     * exceeds the source surface (pw × ph). */
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
     * Modes 1 (opaque memcpy) and 2 used to do palette-index
     * averaging, but mode 2 produced garbage colours (palette
     * indices don't blend linearly in 8bpp paletted) and the
     * original binary uses mode 2 as a fill-transparent path
     * (`if dst==0 dst=src`), not averaging. Modes 1 and 2 are kept
     * here only to match any future caller; the semantic for mode 2
     * is now the original's fill-transparent. */
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
            /* Fill-transparent — original binary's semantic.  Earlier
             * port did palette-index averaging which produced garbage
             * colour. */
            for (int i = 0; i < cw_i; ++i)
                if (dst[i] == 0) dst[i] = ssrc[i];
            break;
        }
        dst  += g_screen_w;
        ssrc += pw;
    }
}

/* ---- PaintImageToBackbuffer -------------------------------------- */
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

/* Set while a komnata is loading (see LoadKomnataScene). The load runs the
 * enter-script's settling ticks, which paint the NEW room's entities into the
 * shadow while it still holds the OLD background — the new BG is only loaded a
 * couple of steps later. Presenting those intermediate ticks flashes "new
 * entities on the old background" for a frame or two (very visible at the PS2's
 * vsync-locked 30 fps). Hold the last good frame instead; the outer game tick
 * presents the clean new scene once the load returns. */
uint8_t g_present_suppressed = 0;

/* ---- FlushFrameToPrimary ----------------------------------------- *
 * Sends the shadow to the screen via the platform layer. */
void FlushFrameToPrimary(void)
{
    if (g_present_suppressed) return;   /* mid-load: don't present partial frames */
    ensure_shadow();
    if (g_back_shadow)
        PlatformPresent(g_back_shadow, g_palette_rgb, g_screen_w, g_screen_h);
    s_dirty_this_count = 0;
}

/* ---- RestorePrevFrameRects --------------------------------------- *
 * No-op in the SDL build (we redraw the whole shadow each frame). */
void RestorePrevFrameRects(void) { /* nop */ }

/* ---- FadeOutToBlack ---------------------------------------------- *
 * Gradual palette fade of the current frame, mirroring the original's
 * game-over transition: LERP every palette entry toward palette entry 0
 * — the scene's index-0 colour, normally black — re-presenting each
 * step. Used before death / chapter / stage-end cutscenes so the screen
 * fades instead of snapping to the AVI.
 *
 * Mutates g_palette_rgb in place and does NOT restore it: every caller
 * is immediately followed by an AVI (sets its own palette) or a scene
 * load (re-installs), so the faded palette is always overwritten next.
 * No-op in headless (nothing is presented). */
#define FADE_OUT_STEPS      16
#define FADE_OUT_FRAME_MS   25
void FadeOutToBlack(void)
{
    if (g_headless || !g_back_shadow) return;

    uint8_t start[sizeof g_palette_rgb];
    memcpy(start, g_palette_rgb, sizeof start);
    const int tr = start[0], tg = start[1], tb = start[2];   /* entry 0 */

    for (int s = 1; s <= FADE_OUT_STEPS; ++s) {
        if (PlatformShouldQuit()) break;
        int t = (s * 256) / FADE_OUT_STEPS;                  /* 0..256 */
        for (int i = 0; i < 256; ++i) {
            int r = start[i * 3 + 0], g = start[i * 3 + 1], b = start[i * 3 + 2];
            g_palette_rgb[i * 3 + 0] = (uint8_t)(r + ((tr - r) * t) / 256);
            g_palette_rgb[i * 3 + 1] = (uint8_t)(g + ((tg - g) * t) / 256);
            g_palette_rgb[i * 3 + 2] = (uint8_t)(b + ((tb - b) * t) / 256);
        }
        FlushFrameToPrimary();
        PumpEvents();
        EnginePaceFrame(FADE_OUT_FRAME_MS);
    }
}

/* ---- BlitSpriteScaledColorKey + Flip ----------------------------- *
 *
 * Nearest-neighbour scaled colour-key blit, used for perspective-
 * scaled actor rendering. The original engine does this inside the
 * per-entity render path; scale comes from entity[+0x58] (scale_pct,
 * computed each tick by UpdateActorMovement from the actor's foot-Y
 * and the stage perspective parameters g_cursor_speed /
 * g_perspective_min / g_perspective_step).
 *
 * Parameters:
 *   dx, dy — destination top-left (already adjusted for scaled size)
 *   sw, sh — source frame size (raw atlas)
 *   dw, dh — destination size (= sw*scale/100, sh*scale/100)
 *   src    — source pixels (sw × sh, 8 bpp)
 * Palette index 0 is transparent. */
void BlitSpriteScaledColorKey(int16_t dx, int16_t dy,
                              uint16_t sw, uint16_t sh,
                              uint16_t dw, uint16_t dh,
                              const uint8_t *src)
{
    BlitSpriteScaledColorKeyFlip(dx, dy, sw, sh, dw, dh, src, 0);
}

/* Same as above with a horizontal mirror flag (used for actors
 * walking right — the original engine stores ebek/fjej atlases
 * facing LEFT only and mirrors at render time). */
void BlitSpriteScaledColorKeyFlip(int16_t dx, int16_t dy,
                                  uint16_t sw, uint16_t sh,
                                  uint16_t dw, uint16_t dh,
                                  const uint8_t *src, int flip_h)
{
    ensure_shadow();
    if (!src || !g_back_shadow || dw == 0 || dh == 0 ||
        sw == 0 || sh == 0) return;
    if (dw > SCALED_BLIT_MAX_DIM || dh > SCALED_BLIT_MAX_DIM) return;

    int destX0 = dx, destY0 = dy;
    int destX1 = dx + dw, destY1 = dy + dh;
    if (destX0 < 0) destX0 = 0;
    if (destY0 < 0) destY0 = 0;
    if (destX1 > g_screen_w) destX1 = g_screen_w;
    if (destY1 > g_screen_h) destY1 = g_screen_h;
    if (destX0 >= destX1 || destY0 >= destY1) return;

    push_dirty(destX0, destY0, destX1 - destX0, destY1 - destY0);

    /* T33: mode 0 — x-step LUT + y-extra-row LUT, Bresenham-style
     * accumulator. */
    static uint32_t x_step[SCALED_BLIT_LUT_SIZE];
    static uint8_t  y_extra[SCALED_BLIT_LUT_SIZE];
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

    /* Walk src rows via base step + y_extra (mirrors mode 0 inner
     * loop in the original). For each dst row, x_step[dx] is how
     * many src columns to advance after sampling. flip_h inverts the
     * x-offset (sw-1-x) — atlases face left, engine flips at render
     * time. */
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

/* ---- DepackRleFrame ---------------------------------------------- *
 *
 * The "rich" ANIM encoding (asset kind=3 in LoadAssetFromDtaBase,
 * i.e. frame_count > 16 and first-frame's first param non-zero)
 * stores each frame as an RLE stream with a 3-byte header:
 *
 *   header[0] = fill_value (usually 0 = palette idx 0 = transparent)
 *   header[1] = marker_A   (any input byte equal to A introduces a
 *                           run of fill_value)
 *   header[2] = marker_B   (introduces a run of an arbitrary literal)
 *
 * Stream (bytes after the header), for each input byte b:
 *   b == marker_A: count = next_byte + 1;
 *                  emit `count` copies of fill_value
 *   b == marker_B: count = next_byte + 1; value = next_byte;
 *                  emit `count` copies of value
 *   else:          emit b once
 *
 * Loop until `dst_len` output bytes written. */
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

/* ---- InstallPalette ---------------------------------------------- *
 *
 * Copies bytes into g_palette_rgb (BGR triplets in the original; we
 * keep the same byte order so .pal files load unmodified).
 *
 * T7: also rebuilds the RGB12 quantization LUTs used by the alpha-
 * plane scaled blit (mode 1 / 2 box-filter paths). */
extern void RebuildAlphaQuantLuts(void);
void InstallPalette(const uint8_t *rgb, uint16_t first)
{
    if (!rgb || first >= 256) return;
    int count = 256 - first;
    memcpy(g_palette_rgb + first*3, rgb, (size_t)count * 3);
    /* T7: rebuild alpha-plane RGB12 quantization LUTs. */
    RebuildAlphaQuantLuts();
}

