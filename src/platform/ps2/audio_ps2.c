/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/ps2/audio_ps2.c — PS2 audio HAL. SDL2-PS2's audio backend
 * wedges the IOP, so audio goes through audsrv directly: a dedicated EE feeder
 * thread pulls the engine mixer (or a cutscene ring) and pushes to audsrv.
 * Provides plat_audio_* + plat_avi_audio_* + platform_ps2_audio_init. Built
 * only for TARGET=ps2. */

#include <stdint.h>

#ifdef WACKI_PS2

#include <string.h>
#include <kernel.h>         /* semaphores + EE threads */
#include <audsrv.h>

#include "wacki/platform/audio.h"   /* plat_audio_* / plat_avi_audio_* */
#include "ps2_internal.h"           /* platform_ps2_audio_init (defined here) */

extern void SDL_Delay(unsigned int ms);

/* ---- native audsrv audio ----------------------------------------- *
 *
 * SDL2-PS2's audio backend wedges the IOP, so audio goes through audsrv
 * directly. The engine's pull mixer (registered via plat_audio_open as
 * mixer_pull) is driven from a dedicated EE thread that fills a chunk and
 * pushes it with audsrv_play_audio() (which blocks when audsrv's ring is
 * full — natural pacing). g_ps2_audio_sema is the audio HAL's channel lock
 * (plat_audio_lock/unlock), guarding the channel array against the game
 * thread's SFX assigns. */

/* audsrv's internal ring is only ~4700 bytes (~53 ms). wait_audio(CHUNK)
 * refills whenever CHUNK bytes are free, so a small CHUNK keeps the ring
 * topped up near-full (~3700/4700 ≈ 41 ms cushion) instead of letting it
 * swing down to a few ms before each big refill — that low point is when an
 * IOP-busy moment (game asset reads over the SIF) drains it to 0 and SPU2
 * repeats its last buffer (the "looping sample" glitch). 1024 B = 256 frames
 * = ~12 ms feed granularity, ~86 SIF feeds/s. */
#define PS2_AUD_CHUNK 1024                 /* 256 stereo S16 frames */
int g_ps2_audio_sema = -1;                 /* the audio HAL channel lock */
static char s_aud_buf[PS2_AUD_CHUNK]   __attribute__((aligned(64)));
static char s_aud_stack[16 * 1024]     __attribute__((aligned(16)));
static volatile int s_aud_run = 0;

/* The mixer's pull, registered via plat_audio_open. NULL until the mixer
 * attaches (the feeder thread starts eagerly for the intro cutscene, which
 * routes through the AVI ring below — so the mixer branch outputs silence
 * until a first SFX/music play registers the pull). */
static plat_audio_pull_fn s_audio_pull = NULL;

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
        } else if (s_audio_pull) {
            WaitSema(g_ps2_audio_sema);
            s_audio_pull(s_aud_buf, PS2_AUD_CHUNK);
            SignalSema(g_ps2_audio_sema);
        } else {
            memset(s_aud_buf, 0, PS2_AUD_CHUNK);   /* mixer not attached yet */
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
void plat_avi_audio_begin(int rate, int channels, int bits)
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
void plat_avi_audio_push(void *buf, int len)
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
void plat_avi_audio_end(void)
{
    s_avi_audio_on = 0;
}

int  plat_avi_audio_is_open(void) { return s_avi_audio_on; }

/* audsrv's ring is tiny and plat_avi_audio_push self-paces (blocks when the
 * ring fills), so there's no SDL-style FIFO to pre-fill — the read-ahead loop
 * is a no-op and the inter-frame wait pumps instead (see needs_pump). */
int  plat_avi_audio_below_cushion(unsigned ms) { (void)ms; return 0; }

/* On skip the cutscene just stops feeding and end() routes the thread back to
 * the mixer; audsrv drains its ~53 ms ring on its own. */
void plat_avi_audio_flush(void) { }

int  plat_avi_audio_needs_pump(void) { return 1; }
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

/* ---- audio-output HAL (storage.h sibling: audio.h) --------------- *
 *
 * The audsrv feeder thread is brought up eagerly (platform_ps2_audio_init,
 * called from platform_sdl.c) so the intro cutscene's audio works before any
 * SFX/music. plat_audio_open just registers the mixer's pull (and starts the
 * thread if it somehow isn't up yet). plat_audio_close is a no-op: audsrv is
 * shared with the cutscene audio path, so it must stay alive. */
int plat_audio_open(int freq, int channels, plat_audio_pull_fn pull)
{
    (void)freq;
    s_audio_pull = pull;
    if (!s_aud_run) platform_ps2_audio_init();
    return channels;                       /* audsrv is fixed 22050/16/stereo */
}

void plat_audio_close(void) { /* audsrv stays up — shared with AVI cutscenes */ }

int  plat_audio_is_open(void) { return s_aud_run; }

void plat_audio_lock(void)
{
    if (g_ps2_audio_sema >= 0) WaitSema(g_ps2_audio_sema);
}

void plat_audio_unlock(void)
{
    if (g_ps2_audio_sema >= 0) SignalSema(g_ps2_audio_sema);
}

#endif /* WACKI_PS2 */
