/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform_ps2.c — PlayStation 2 device glue.
 *
 * PS2 file I/O bring-up. ps2sdk's newlib fopen() reaches no device on its
 * own, so the engine's file access goes through fileXio instead (see the
 * cygio.c shim). For that to work the IOP fileio stack must be brought up
 * first — confirmed on-target (PCSX2 via PINE):
 *
 *   SifInitRpc → reset the IOP (so the real iomanX/fileXio don't fight
 *   PCSX2's HLE default modules — that conflict hangs fileXioInit) → sbv
 *   patches (allow loading modules from EE RAM) → load iomanX + fileXio +
 *   cdfs (bin2c-embedded) → fileXioInit → sceCdInit.
 *
 * After this, fileXioOpen("host:...") (dev, bare ELF + ./data) and
 * fileXioOpen("cdfs:/DATA/...") (the ISO) both open. Audio is left off
 * (audsrv wedges the IOP); SDL_Init for video happens later and brings up
 * its own modules.
 *
 * Built only for TARGET=ps2; the build (tools/build-ps2.sh) generates the
 * iomanX_irx.c / fileXio_irx.c / cdfs_irx.c includes with bin2c.
 */

#include <stdint.h>

#ifdef WACKI_PS2

#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <libcdvd.h>
#include <gsKit.h>
#include <dmaKit.h>

#include "iomanX_irx.c"
#include "fileXio_irx.c"
#include "cdfs_irx.c"

/* Hand-declared to dodge ps2sdk's guarded fileXio_rpc.h ("don't mix
 * fio/fileXio with the newlib port"): the engine uses ONLY fileXio for
 * file access (cygio.c), never newlib fopen, so this is safe. */
extern int fileXioInit(void);

/* ---- bring-up trace (read over PINE) ----------------------------- */

volatile uint32_t g_ps2_trace[32];
volatile uint32_t g_ps2_trace_n = 0;

/* Frame profiling (read over PINE). exp = 8bpp->ARGB expansion + texture
 * update; draw = RenderClear/Copy/Present (GS + vsync); frame = full
 * present-to-present. "engine software blit" ≈ frame - exp - draw. */
volatile uint32_t g_ps2_exp_ms   = 0;
volatile uint32_t g_ps2_draw_ms  = 0;
volatile uint32_t g_ps2_frame_ms = 0;
volatile uint32_t g_ps2_present_n = 0;

void ps2_mark(uint32_t code)
{
    uint32_t i = g_ps2_trace_n;
    if (i < 32) g_ps2_trace[i] = code;
    g_ps2_trace_n = i + 1;
}

void ps2_spin_forever(void)
{
    for (;;) { }
}

/* ---- file I/O bring-up ------------------------------------------- */

/* Called first thing in WackiMain, before any file access. */
void platform_ps2_io_init(void)
{
    SifInitRpc(0);

    /* Clean IOP — real iomanX/fileXio mustn't fight PCSX2's HLE defaults. */
    while (!SifIopReset("", 0)) { }
    while (!SifIopSync())       { }
    SifInitRpc(0);
    SifLoadFileInit();

    /* Allow loading IRX modules from EE RAM buffers on the reset IOP. */
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();

    int ret;
    SifExecModuleBuffer(iomanX_irx,  size_iomanX_irx,  0, NULL, &ret);
    SifExecModuleBuffer(fileXio_irx, size_fileXio_irx, 0, NULL, &ret);
    fileXioInit();

    sceCdInit(SCECdINIT);
    SifExecModuleBuffer(cdfs_irx, size_cdfs_irx, 0, NULL, &ret);
}

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

    s_fbtex.Mem = (u32 *)shadow;
    gsKit_clear(s_gs, 0);
    gsKit_TexManager_invalidate(s_gs, &s_fbtex);   /* shadow changed → re-upload */
    gsKit_TexManager_bind(s_gs, &s_fbtex);
    gsKit_prim_sprite_texture_3d(s_gs, &s_fbtex,
        0.0f,            0.0f,            1, 0.0f,     0.0f,
        (float)s_gs->Width, (float)s_gs->Height, 1, (float)w, (float)h,
        0x80808080);
    gsKit_queue_exec(s_gs);
    gsKit_sync_flip(s_gs);
    gsKit_TexManager_nextFrame(s_gs);

    g_ps2_present_n++;   /* frame counter — read over PINE to measure fps */
}

#endif /* WACKI_PS2 */
