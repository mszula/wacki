/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/ps2/video_ps2.c — PS2 video HAL. The engine's 8-bpp shadow +
 * palette go to the GS via gsKit (a PSMT8 texture + hardware CLUT — the GS
 * does the palette lookup), avoiding a per-pixel EE expansion. Provides
 * plat_video_* + the frame-profiling counters read over PINE. Built only for
 * TARGET=ps2. */

#include <stdint.h>

#ifdef WACKI_PS2

#include <string.h>
#include <gsKit.h>
#include <dmaKit.h>

#include "wacki/platform/video.h"   /* plat_video_* */
#include "wacki/platform/input.h"   /* plat_pad_menu_nav — picker input */
#include "ps2_internal.h"           /* platform_ps2_audio_init (plat_video_init) */

#include "font8x8.inc"              /* PS2_FONT8X8 — menu text as solid sprites */

/* Frame profiling (read over PINE). exp = 8bpp->ARGB expansion + texture
 * update; draw = RenderClear/Copy/Present (GS + vsync); frame = full
 * present-to-present. "engine software blit" ≈ frame - exp - draw. */
volatile uint32_t g_ps2_exp_ms   = 0;
volatile uint32_t g_ps2_draw_ms  = 0;
volatile uint32_t g_ps2_frame_ms = 0;
volatile uint32_t g_ps2_present_n = 0;
/* ---- native gsKit video (hardware palette) ----------------------- *
 *
 * The engine renders into a 640×480 8-bpp shadow + a 256-entry palette.
 * SDL2-PS2's renderer can only take RGB textures, forcing a per-pixel
 * 8→ARGB expansion on the EE — profiled at ~135 ms/frame (6 fps). Here we
 * own the GS via gsKit instead: upload the raw 8-bpp shadow as a PSMT8
 * texture + the palette as a CLUT and let the GS do the lookup in
 * hardware during rasterisation. No EE expansion, 307 KB upload instead
 * of 1.2 MB. SDL is kept only for input (SDL_GameController) + timing. */

static GSGLOBAL *s_gs = 0;
static GSTEXTURE s_fbtex;
static u32       s_clut[256] __attribute__((aligned(64)));

/* ---- video mode: runtime, chosen by the boot-time picker --------- *
 *
 * Pure runtime choice (ps2_video_picker below): the engine boots in NTSC and
 * the picker switches the screen if the player picks PAL or 480p. */
typedef enum { PS2_VIDEO_NTSC = 0, PS2_VIDEO_PAL, PS2_VIDEO_480P } ps2_video_mode_t;

static int s_video_mode = PS2_VIDEO_NTSC;

/* Set the gsGlobal mode + geometry for a picked mode. NTSC keeps gsKit's
 * auto-detected geometry (overriding it leaves MagV=-1 = top-half only); PAL
 * and 480p set the FULL geometry, as the old per-mode #ifdef chain did. */
static void apply_video_mode(GSGLOBAL *gs, int mode)
{
    switch (mode) {
    case PS2_VIDEO_PAL:
        /* PAL 640×512 interlaced — region-authentic for this Polish game; the
         * 640×480 shadow letterboxes (16px bars) instead of squashing. 50 Hz
         * vs the 30 fps present isn't a clean 2:1, so motion is a touch less
         * smooth than NTSC. */
        gs->Mode = GS_MODE_PAL; gs->Interlace = GS_INTERLACED; gs->Field = GS_FIELD;
        gs->Width = 640; gs->Height = 512;
        break;
    case PS2_VIDEO_480P:
        /* Progressive 640×480 (VGA 60 Hz) — 1:1, no interlace flicker. Needs a
         * VGA/component display on real HW; PCSX2's VGA window may sit a little
         * high (StartY needs interactive tuning). */
        gs->Mode = GS_MODE_VGA_640_60; gs->Interlace = GS_NONINTERLACED; gs->Field = GS_FRAME;
        gs->Width = 640; gs->Height = 480;
        break;
    case PS2_VIDEO_NTSC:
    default:
        /* gsKit auto NTSC (640×448 interlaced). Do NOT override geometry. */
        break;
    }
}

/* Draw an ASCII string from the embedded 8x8 font as solid GS sprites — the
 * same primitive path as the highlight bar, so it renders with no texture or
 * alpha (gsKit fontm's textured path drew blank here). `px` is the on-screen
 * size of one font pixel; consecutive lit pixels in a row merge into one run
 * sprite to keep the queue small. Advances 8*px per glyph. */
static void draw_text(GSGLOBAL *gs, float x, float y, float px, int z,
                      u64 color, const char *s)
{
    for (; *s; ++s) {
        unsigned ch = (unsigned char)*s;
        if (ch >= 0x20 && ch <= 0x7E) {
            const unsigned char *g = PS2_FONT8X8[ch - 0x20];
            for (int row = 0; row < 8; ++row) {
                unsigned bits = (unsigned)g[row];
                int col = 0;
                while (bits) {
                    if (bits & 1u) {
                        int run = 1;
                        while ((bits >> run) & 1u) ++run;      /* merge lit run */
                        float gx = x + (float)col * px;
                        float gy = y + (float)row * px;
                        gsKit_prim_sprite(gs, gx, gy,
                                          gx + (float)run * px, gy + px, z, color);
                        col += run; bits >>= run;
                    } else { ++col; bits >>= 1; }
                }
            }
        }
        x += 8.0f * px;
    }
}

/* Centred variant — places the string's midpoint at screen-x `cx`, so lines
 * stay on-screen regardless of length (avoids the hand-tuned-x clipping). */
static void draw_text_c(GSGLOBAL *gs, float cx, float y, float px, int z,
                        u64 color, const char *s)
{
    draw_text(gs, cx - 8.0f * px * (float)strlen(s) * 0.5f, y, px, z, color, s);
}

/* ---- boot-time mode picker --------------------------------------- *
 *
 * Drawn in the safe auto-NTSC screen platform_ps2_video_init brings up first
 * (works on every TV + PCSX2). The DualShock D-pad/stick moves the selection,
 * X confirms. With no pad we skip straight to the preselected default so a
 * controller-less PCSX2 launch never soft-locks. Returns the chosen
 * ps2_video_mode_t; the caller re-inits the screen if it differs from NTSC. */
static int ps2_video_picker(GSGLOBAL *gs, int preselect)
{
    int sel = (preselect == PS2_VIDEO_PAL || preselect == PS2_VIDEO_480P)
              ? preselect : PS2_VIDEO_NTSC;

    int up, down, confirm;
    if (!plat_pad_menu_nav(&up, &down, &confirm))
        return sel;                              /* no pad — use the default */

    static const char *const ITEMS[3] = {
        "NTSC - 640x448, 60 Hz",
        "PAL - 640x512, 50 Hz",
        "480p - 640x480, progresywny"
    };
    /* "Wacki" palette — warm paper-and-ink: deep-brown ground, red-orange game
     * title, cream heading, parchment options, a gold bar behind the pick.
     * (Kept off a light background — that would clamp to white on the GS.) */
    const u64 BG    = GS_SETREG_RGBAQ(0x22, 0x16, 0x10, 0x80, 0);  /* deep brown    */
    const u64 TITLE = GS_SETREG_RGBAQ(0xE8, 0x46, 0x1A, 0x80, 0);  /* red-orange    */
    const u64 HEAD  = GS_SETREG_RGBAQ(0xF2, 0xD9, 0x8C, 0x80, 0);  /* cream         */
    const u64 ITEM  = GS_SETREG_RGBAQ(0xC9, 0xB8, 0x90, 0x80, 0);  /* parchment     */
    const u64 SELT  = GS_SETREG_RGBAQ(0x22, 0x16, 0x10, 0x80, 0);  /* dark (on bar) */
    const u64 BARC  = GS_SETREG_RGBAQ(0xF2, 0xC0, 0x33, 0x80, 0);  /* gold bar      */
    const u64 HINT  = GS_SETREG_RGBAQ(0x9A, 0x82, 0x60, 0x80, 0);  /* muted warm    */

    for (;;) {
        plat_pad_menu_nav(&up, &down, &confirm);
        if (up)      sel = (sel + 2) % 3;        /* +2 mod 3 == -1 */
        if (down)    sel = (sel + 1) % 3;
        if (confirm) break;

        gsKit_clear(gs, BG);
        draw_text_c(gs, 320.0f,  46.0f, 2.0f, 3, TITLE, "Wacki: Kosmiczna Rozgrywka");
        draw_text_c(gs, 320.0f,  84.0f, 3.0f, 3, HEAD,  "Tryb video");
        for (int i = 0; i < 3; ++i) {
            float y = 162.0f + (float)i * 46.0f;
            if (i == sel)
                gsKit_prim_sprite(gs, 70.0f, y - 7.0f, 570.0f, y + 25.0f, 2, BARC);
            draw_text_c(gs, 320.0f, y, 2.0f, 3, i == sel ? SELT : ITEM, ITEMS[i]);
        }
        draw_text_c(gs, 320.0f, 350.0f, 2.0f, 3, HINT,
                    "Gora/Dol - wybor   X - zatwierdz");
        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);
    }
    /* Drop the confirming X (and any other queued pad input) so it doesn't reach
     * the game's first pump as a click and skip the (click-skippable) intro. */
    plat_input_flush();
    return sel;
}

int platform_ps2_video_init(int w, int h)
{
    /* Bring the GS up in gsKit's auto NTSC first (safe on every display) so the
     * picker has a screen to draw on; switch to the picked mode afterwards.
     * Only the pixel format / buffering are tweaked here — NOT the geometry. */
    s_gs = gsKit_init_global();
    s_gs->PSM             = GS_PSM_CT24;
    s_gs->PSMZ            = GS_PSMZ_16S;
    s_gs->ZBuffering      = GS_SETTING_OFF;
    s_gs->DoubleBuffering = GS_SETTING_ON;
    s_gs->PrimAlphaEnable = GS_SETTING_OFF;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(s_gs);
    gsKit_mode_switch(s_gs, GS_ONESHOT);
    gsKit_TexManager_init(s_gs);                 /* before fontm (matches gsKit's example) */

    /* Let the player choose PAL / NTSC / 480p, then re-init the screen if the
     * pick isn't the NTSC we're already showing. */
    s_video_mode = ps2_video_picker(s_gs, s_video_mode);
    if (s_video_mode != PS2_VIDEO_NTSC) {
        apply_video_mode(s_gs, s_video_mode);
        gsKit_init_screen(s_gs);
        gsKit_mode_switch(s_gs, GS_ONESHOT);
        gsKit_TexManager_init(s_gs);             /* re-init VRAM tracking for the new mode */
    }

    s_fbtex.Width           = (u32)w;
    s_fbtex.Height          = (u32)h;
    s_fbtex.PSM             = GS_PSM_T8;
    s_fbtex.ClutPSM         = GS_PSM_CT32;
    s_fbtex.Filter          = GS_FILTER_LINEAR;   /* smooth the 480→448 scale */
    s_fbtex.ClutStorageMode = GS_CLUT_STORAGE_CSM1;
    s_fbtex.Clut            = s_clut;
    s_fbtex.Mem             = 0;                    /* points at the shadow per frame */
    s_fbtex.Delayed         = 0;
    gsKit_setup_tbw(&s_fbtex);
    return 1;
}

void platform_ps2_present(const uint8_t *shadow, const uint8_t *palette,
                          int w, int h)
{
    if (!s_gs) return;

    /* Palette → GS CLUT. CT32 = R,G,B,A in ascending bytes (GS treats
     * alpha 0x80 as 1.0). 8-bit textures read the CLUT in CSM1 storage
     * order, which swaps entries within each 32-block (8↔16) — gsKit does
     * NOT do this, so apply the swizzle here or the colours scramble. */
    for (int i = 0; i < 256; ++i) {
        const uint8_t *e = palette + i * 3;
        u32 c = (u32)e[0] | ((u32)e[1] << 8) | ((u32)e[2] << 16) | (0x80u << 24);
        int j = i;
        if      ((i & 0x18) == 0x08) j = i + 8;
        else if ((i & 0x18) == 0x10) j = i - 8;
        s_clut[j] = c;
    }

    /* Vertical fit: if the display frame is TALLER than the 640x480 shadow
     * (PAL 512), draw 1:1 centred with black letterbox bars rather than
     * scaling up — sharper, no vertical squash. (PAL 512 → 480 native + a
     * 16px bar top & bottom = 32px total.) NTSC (448 < 480) still scales to
     * fill; progressive (480 == 480) is already 1:1. Width is 640 in every
     * mode, so horizontal is always 1:1. */
    float dy0 = 0.0f, dy1 = (float)s_gs->Height;
    if ((int)s_gs->Height > h) {
        int bar = ((int)s_gs->Height - h) / 2;
        dy0 = (float)bar;
        dy1 = (float)(bar + h);
    }

    s_fbtex.Mem = (u32 *)shadow;
    gsKit_clear(s_gs, 0);                            /* black; fills the bars */
    gsKit_TexManager_invalidate(s_gs, &s_fbtex);   /* shadow changed → re-upload */
    gsKit_TexManager_bind(s_gs, &s_fbtex);
    gsKit_prim_sprite_texture_3d(s_gs, &s_fbtex,
        0.0f,            dy0,            1, 0.0f,     0.0f,
        (float)s_gs->Width, dy1, 1, (float)w, (float)h,
        0x80808080);
    gsKit_queue_exec(s_gs);
    gsKit_sync_flip(s_gs);
    gsKit_TexManager_nextFrame(s_gs);

    g_ps2_present_n++;   /* frame counter — read over PINE to measure fps */
}

/* ---- video-output HAL (wacki/platform/video.h) ------------------- *
 *
 * Thin wrappers over the gsKit display + audsrv set up above. platform_sdl.c
 * (the shared SDL input/event layer, also compiled for PS2) drives these
 * instead of #ifdef'ing WACKI_PS2 in PlatformInit/Present. */

/* SDL_Init flags for the PS2: EVENTS + TIMER only (gsKit owns the GS, audsrv
 * the sound; SDL2-PS2's video/audio backends fight the IOP). These are stable
 * SDL2 ABI values — naming them here avoids pulling the whole SDL.h into this
 * gsKit/ps2sdk TU (which already hand-declares SDL_Delay) for two constants. */
#define PS2_SDL_INIT_TIMER   0x00000001u
#define PS2_SDL_INIT_EVENTS  0x00004000u

unsigned plat_video_sdl_init_flags(void)
{
    return PS2_SDL_INIT_EVENTS | PS2_SDL_INIT_TIMER;
}

int plat_video_init(int w, int h, const char *title)
{
    (void)title;
    if (!platform_ps2_video_init(w, h)) return 0;
    /* Native audsrv audio is brought up eagerly here (the intro cutscene's
     * audio thread must be running before any mixer attach). */
    platform_ps2_audio_init();
    return 1;
}

void plat_video_present(const uint8_t *shadow, const uint8_t *palette_rgb,
                        int w, int h)
{
    platform_ps2_present(shadow, palette_rgb, w, h);
}

/* gsKit needs no explicit teardown before the process exits; there's no
 * windowed mode and no SDL message-box surface on the PS2. */
void plat_video_shutdown(void)                                   { }
void plat_video_toggle_fullscreen(void)                          { }
void plat_video_message_box(const char *t, const char *b)        { (void)t; (void)b; }

#endif /* WACKI_PS2 */
