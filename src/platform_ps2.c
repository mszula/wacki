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
#include <kernel.h>
#include <audsrv.h>

#include "iomanX_irx.c"
#include "fileXio_irx.c"
#include "cdfs_irx.c"
#include "audsrv_irx.c"

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

    /* Audio: the SPU2 driver (LIBSD, in rom0) + audsrv. Used by the
     * mixer→audsrv thread in platform_ps2_audio_init(). */
    SifLoadModule("rom0:LIBSD", 0, NULL);
    SifExecModuleBuffer(audsrv_irx, size_audsrv_irx, 0, NULL, &ret);

    /* NOTE: do NOT call init_joystick_driver() here — SDL2-PS2's
     * SDL_InitSubSystem(GAMECONTROLLER) brings the pad up itself (loads
     * padman), and loading it a second time here conflicts and kills pad
     * input entirely. The pad already works via SDL. */
}

/* ---- native audsrv audio ----------------------------------------- *
 *
 * SDL2-PS2's audio backend wedges the IOP, so audio goes through audsrv
 * directly. The engine's SDL-style pull mixer (audio.c mixer_callback) is
 * driven from a dedicated EE thread that fills a chunk and pushes it with
 * audsrv_play_audio() (which blocks when audsrv's ring is full — natural
 * pacing). g_ps2_audio_sema guards the channel array against the game
 * thread's SFX assigns (mixer_assign/release take it on PS2). */

/* audsrv's internal ring is only ~4700 bytes (~53 ms). wait_audio(CHUNK)
 * refills whenever CHUNK bytes are free, so a small CHUNK keeps the ring
 * topped up near-full (~3700/4700 ≈ 41 ms cushion) instead of letting it
 * swing down to a few ms before each big refill — that low point is when an
 * IOP-busy moment (game asset reads over the SIF) drains it to 0 and SPU2
 * repeats its last buffer (the "looping sample" glitch). 1024 B = 256 frames
 * = ~12 ms feed granularity, ~86 SIF feeds/s. */
#define PS2_AUD_CHUNK 1024                 /* 256 stereo S16 frames */
int g_ps2_audio_sema = -1;                 /* shared with audio.c */
static char s_aud_buf[PS2_AUD_CHUNK]   __attribute__((aligned(64)));
static char s_aud_stack[16 * 1024]     __attribute__((aligned(16)));
static volatile int s_aud_run = 0;

extern void mixer_fill_ps2(void *buf, int len);   /* audio.c */
extern void SDL_Delay(unsigned int ms);           /* SDL2 */

/* AVI cutscene audio. audsrv stays at 22050/16/stereo; the cutscene's PCM
 * is converted to that on the way in and handed to the SAME audio thread
 * through a decoupling ring. Feeding straight from the AVI pump (game
 * thread) couldn't keep up — a slow per-frame FLC decode blocks that
 * thread far longer than audsrv's ~53 ms ring, draining it (the looping
 * sample). The audio thread, by contrast, plays from the ring at a steady
 * real-time rate regardless of decode, and silence-pads a momentary
 * shortfall instead of repeating. */
#define AVI_RING_BYTES   (512 * 1024)     /* ~3.0 s of 22050/16/stereo —
                                           * must exceed the AVI's audio
                                           * chunk size (they're ~0.7 s+) */
static uint8_t      s_avi_ring[AVI_RING_BYTES] __attribute__((aligned(16)));
static volatile uint32_t s_avi_wpos = 0;  /* producer (game thread) */
static volatile uint32_t s_avi_rpos = 0;  /* consumer (audio thread) */
static volatile int s_avi_audio_on = 0;
static int          s_avi_src_ch   = 2;
static int          s_avi_src_bits = 16;

/* Audio thread: pull one chunk of cutscene audio from the ring (silence
 * for any shortfall) — single consumer, lock-free against the producer. */
static void avi_ring_pull(uint8_t *dst, int n)
{
    uint32_t r = s_avi_rpos, w = s_avi_wpos;
    uint32_t avail = (w - r + AVI_RING_BYTES) % AVI_RING_BYTES;
    uint32_t take  = avail < (uint32_t)n ? avail : (uint32_t)n;
    uint32_t first = AVI_RING_BYTES - r;
    if (first > take) first = take;
    memcpy(dst, s_avi_ring + r, first);
    if (take > first) memcpy(dst + first, s_avi_ring, take - first);
    if (take < (uint32_t)n) memset(dst + take, 0, (uint32_t)n - take);
    s_avi_rpos = (r + take) % AVI_RING_BYTES;
}

static void ps2_audio_thread(void *arg)
{
    (void)arg;
    while (s_aud_run) {
        if (s_avi_audio_on) {                 /* a cutscene is playing */
            avi_ring_pull((uint8_t *)s_aud_buf, PS2_AUD_CHUNK);
        } else {
            WaitSema(g_ps2_audio_sema);
            mixer_fill_ps2(s_aud_buf, PS2_AUD_CHUNK);
            SignalSema(g_ps2_audio_sema);
        }
        /* Pace to the SPU2 drain rate: wait_audio() blocks until the ring
         * has room for the whole chunk, then play_audio() queues it in full.
         * Feeding without wait_audio() overruns the ring and stalls SPU2. */
        audsrv_wait_audio(PS2_AUD_CHUNK);
        audsrv_play_audio(s_aud_buf, PS2_AUD_CHUNK);
    }
    ExitThread();
}

/* Begin cutscene audio: remember the source format and route the audio
 * thread to the cutscene ring (reset empty). audsrv format is unchanged. */
void platform_ps2_avi_audio_begin(int rate, int channels, int bits)
{
    (void)rate;                               /* assumed 22050 (mixer rate) */
    if (g_ps2_audio_sema < 0) return;
    s_avi_src_ch   = channels;
    s_avi_src_bits = bits;
    s_avi_wpos = s_avi_rpos = 0;
    s_avi_audio_on = 1;
}

/* Producer (game thread): convert one cutscene PCM chunk to 22050/16/stereo
 * and write it into the ring in room-sized blocks. The AVI's audio chunks
 * are large (~0.7 s+), so a whole chunk usually fits the ring in one pass
 * (no wait); only if the ring momentarily fills does it yield, splitting
 * the chunk and pacing to playback. Unsupported formats play silence. */
void platform_ps2_avi_audio_push(const void *buf, int len)
{
    if (!s_avi_audio_on || len <= 0) return;
    int mono;
    if      (s_avi_src_ch == 1 && s_avi_src_bits == 16) mono = 1;
    else if (s_avi_src_ch == 2 && s_avi_src_bits == 16) mono = 0;
    else return;                              /* unsupported → play silence */

    const int16_t *msp   = (const int16_t *)buf;  /* mono samples */
    const uint8_t *bsp   = (const uint8_t *)buf;  /* stereo bytes */
    uint32_t total_out   = mono ? (uint32_t)len * 2 : (uint32_t)len;
    uint32_t produced    = 0;                 /* output bytes written */
    uint32_t in_sample   = 0;                 /* mono samples consumed */

    while (produced < total_out) {
        uint32_t used = (s_avi_wpos - s_avi_rpos + AVI_RING_BYTES) % AVI_RING_BYTES;
        uint32_t room = AVI_RING_BYTES - used - 1;
        if (room < 4) {                       /* ring full — let it drain */
            SDL_Delay(1);
            if (!s_avi_audio_on) return;
            continue;
        }
        uint32_t w = s_avi_wpos;
        if (mono) {
            uint32_t fit = room / 4;          /* whole stereo frames that fit */
            uint32_t rem = (total_out - produced) / 4;
            if (fit > rem) fit = rem;
            for (uint32_t k = 0; k < fit; ++k) {
                int16_t v = msp[in_sample++];
                *(int16_t *)(s_avi_ring + w) = v; w = (w + 2) % AVI_RING_BYTES;
                *(int16_t *)(s_avi_ring + w) = v; w = (w + 2) % AVI_RING_BYTES;
            }
            produced += fit * 4;
        } else {
            uint32_t can = total_out - produced;
            if (can > room) can = room;
            for (uint32_t k = 0; k < can; ++k) {
                s_avi_ring[w] = bsp[produced + k]; w = (w + 1) % AVI_RING_BYTES;
            }
            produced += can;
        }
        s_avi_wpos = w;
    }
}

/* Cutscene done: route the audio thread back to the mixer. */
void platform_ps2_avi_audio_end(void)
{
    s_avi_audio_on = 0;
}

void platform_ps2_audio_init(void)
{
    if (audsrv_init() != 0) return;
    struct audsrv_fmt_t fmt;
    fmt.freq = 22050; fmt.bits = 16; fmt.channels = 2;
    if (audsrv_set_format(&fmt) != 0) return;
    audsrv_set_volume(MAX_VOLUME);

    ee_sema_t sema;
    sema.init_count = 1; sema.max_count = 1; sema.attr = 0; sema.option = 0;
    g_ps2_audio_sema = CreateSema(&sema);
    if (g_ps2_audio_sema < 0) return;

    /* The game loop busy-waits on the GS vsync (gsKit_sync_flip) and almost
     * never yields the CPU, and the EE main thread runs at priority 1 (the
     * top of the user range). The audio feeder MUST be strictly higher
     * priority (lower number) than the game thread, or it only runs when the
     * game happens to block (e.g. a fileXio read on a player action) — the
     * "audio hangs when idle, advances when I do something" symptom. Since
     * main sits at 1 there's no room above it, so demote the game thread to
     * 40 and put the audio thread at 32. */
    ChangeThreadPriority(GetThreadId(), 40);

    s_aud_run = 1;
    ee_thread_t th;
    th.func             = (void *)ps2_audio_thread;
    th.stack            = s_aud_stack;
    th.stack_size       = sizeof s_aud_stack;
    th.gp_reg           = GetGP();
    th.initial_priority = 32;
    th.attr             = 0;
    th.option           = 0;
    int tid = CreateThread(&th);
    if (tid >= 0) StartThread(tid, NULL);
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
