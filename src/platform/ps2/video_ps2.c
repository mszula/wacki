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
#include "ps2_internal.h"           /* platform_ps2_audio_init (plat_video_init) */

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

int platform_ps2_video_init(int w, int h)
{
    s_gs = gsKit_init_global();
#ifdef WACKI_PS2_PROGRESSIVE
    /* Progressive 640×480 (VGA 60 Hz) — full height, no interlace flicker,
     * 1:1 with the engine framebuffer. Geometry is correct (PINE-confirmed
     * 640×480, MagV=0, non-interlaced), but PCSX2's VGA display window sits
     * a little high (top clipped, black bar at the bottom) — the StartY
     * placement needs interactive tuning. Off by default until a startup
     * mode picker lands; also needs a VGA/component display on real HW. */
    s_gs->Mode           = GS_MODE_VGA_640_60;
    s_gs->Interlace      = GS_NONINTERLACED;
    s_gs->Field          = GS_FRAME;
    s_gs->Width          = 640;
    s_gs->Height         = 480;
#elif defined(WACKI_PS2_576P)
    /* PAL progressive 576p (640×576, 50 Hz) — test build: WACKI_PS2_576P=1.
     * Region-authentic + no interlace flicker. The 640×480 shadow draws 1:1
     * with a 48px letterbox bar top & bottom. Needs a 576p-capable (component/
     * RGB) display on real HW. Full geometry like the others. */
    s_gs->Mode           = GS_MODE_DTV_576P;
    s_gs->Interlace      = GS_NONINTERLACED;
    s_gs->Field          = GS_FRAME;
    s_gs->Width          = 640;
    s_gs->Height         = 576;
#elif defined(WACKI_PS2_PAL)
    /* PAL 640×512 interlaced — test build: WACKI_PS2_PAL=1 ./tools/build-ps2.sh.
     * The 640×480 shadow scales to the taller PAL frame (less vertical squash
     * than NTSC's 480→448) and is region-authentic for this Polish game.
     * Caveat: 50 Hz vs the 30 fps present cadence isn't a clean 2:1, so motion
     * is a touch less smooth than NTSC. Set the FULL geometry like the
     * progressive path — a partial override leaves MagV=-1 (top-half only). */
    s_gs->Mode           = GS_MODE_PAL;
    s_gs->Interlace      = GS_INTERLACED;
    s_gs->Field          = GS_FIELD;
    s_gs->Width          = 640;
    s_gs->Height         = 512;
#endif
    /* Default: gsKit's auto-detected mode (NTSC 640×448 interlaced) — full
     * screen, correct MagV. Do NOT override its geometry (overriding left
     * MagV=-1 = top-half). Only the pixel format / buffering are tweaked. */
    s_gs->PSM            = GS_PSM_CT24;
    s_gs->PSMZ           = GS_PSMZ_16S;
    s_gs->ZBuffering     = GS_SETTING_OFF;
    s_gs->DoubleBuffering = GS_SETTING_ON;
    s_gs->PrimAlphaEnable = GS_SETTING_OFF;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(s_gs);
    gsKit_mode_switch(s_gs, GS_ONESHOT);
    gsKit_TexManager_init(s_gs);

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
