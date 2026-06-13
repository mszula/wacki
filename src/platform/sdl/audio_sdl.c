/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/sdl/audio_sdl.c — audio-output HAL, SDL backend.
 *
 * Desktop + handheld implementation of plat_audio_* (wacki/platform/audio.h):
 * one SDL audio device whose callback pulls mixed PCM from the engine mixer.
 * The PS2 backend (audsrv + an EE feeder thread) lives in src/platform/ps2/audio_ps2.c. */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/audio.h"
#include "wacki/platform/system.h"   /* plat_restore_system_volume */

#include <SDL.h>

#define SDL_AUDIO_OPEN_SAMPLES   2048      /* device buffer in frames */

static SDL_AudioDeviceID  s_dev  = 0;
static SDL_AudioSpec      s_spec;
static plat_audio_pull_fn s_pull = NULL;

/* Fires on SDL's audio thread — hand straight to the mixer's pull. */
static void sdl_audio_cb(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    if (s_pull) s_pull(stream, len);
    else        SDL_memset(stream, 0, (size_t)len);
}

int plat_audio_open(int freq, int channels, plat_audio_pull_fn pull)
{
    if (s_dev) return s_spec.channels;          /* already open */

    s_pull = pull;

    SDL_AudioSpec want;
    SDL_memset(&want, 0, sizeof want);
    want.freq     = freq;
    want.format   = AUDIO_S16SYS;
    want.channels = (Uint8)channels;
    want.samples  = SDL_AUDIO_OPEN_SAMPLES;
    want.callback = sdl_audio_cb;
    want.userdata = NULL;

    /* SDL_AUDIO_ALLOW_CHANNELS_CHANGE + SAMPLES_CHANGE — embedded SDL2
     * backends (mmiyoo on Miyoo Mini Plus) only do mono S16 at a fixed
     * buffer size. With allowed_changes=0 SDL2 silently fails to set up its
     * stereo→mono conversion stream and leaves the backend wedged in
     * "device-already-open" state, so every subsequent play bounces off it.
     * Allowing channel + sample count to flex means we take whatever we got;
     * the mixer downmixes if we ended up mono. Frequency stays pinned because
     * every source is pre-converted to 22 050 Hz. */
    s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &s_spec,
                                SDL_AUDIO_ALLOW_CHANNELS_CHANGE |
                                SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if (!s_dev) {
        /* Log only the first attempt — on Miyoo / OnionOS the audioserver
         * kill is async, so the first dozen-ish attempts race the dying
         * process and fail with "Audio device already open". Repeating the
         * line each frame just floods wacki.log. */
        static int s_logged_open_failure = 0;
        if (!s_logged_open_failure) {
            LOG_INFO("audio", "SDL_OpenAudioDevice failed: %s "
                              "(will retry silently on next play)",
                     SDL_GetError());
            s_logged_open_failure = 1;
        }
        return 0;
    }
    SDL_PauseAudioDevice(s_dev, 0);             /* unpause */
    LOG_INFO("audio", "opened: %d Hz, %d ch, %d-bit, %d samples",
             s_spec.freq, s_spec.channels,
             SDL_AUDIO_BITSIZE(s_spec.format), s_spec.samples);
    /* Some backends (mmiyoo) reset the kernel mixer to max on every
     * SDL_OpenAudioDevice — the platform re-applies its saved volume here. */
    plat_restore_system_volume();
    return s_spec.channels;
}

void plat_audio_close(void)
{
    if (!s_dev) return;
    SDL_PauseAudioDevice(s_dev, 1);
    SDL_CloseAudioDevice(s_dev);
    s_dev = 0;
    LOG_INFO("audio", "released (next play re-opens lazily)");
}

int  plat_audio_is_open(void) { return s_dev != 0; }

void plat_audio_lock(void)    { SDL_LockAudioDevice(s_dev);   }
void plat_audio_unlock(void)  { SDL_UnlockAudioDevice(s_dev); }

/* ---- cutscene (AVI) audio — a second queue-mode device ----------- */

#define AVI_AUDIO_OPEN_SAMPLES   4096   /* ~185 ms @ 22 kHz — bridges the gap
                                         * between per-video-frame audio chunks
                                         * (10 fps = 100 ms) so a slower frame
                                         * can't underrun a smaller buffer */

static SDL_AudioDeviceID s_avi_dev = 0;
static SDL_AudioSpec     s_avi_spec;
static int               s_avi_open = 0;

/* Explicit resampler: source PCM → obtained device spec. Built only when the
 * device negotiated something other than the source — Windows/WASAPI runs at the
 * system mix rate (44.1/48 kHz), never the AVI's 22 kHz, and SDL's implicit
 * queue-time conversion doesn't kick in there (queued 22 kHz bytes played ~2×
 * too fast: "sped up, then silence"). We resample explicitly into the obtained
 * spec and queue that. Subsumes the old manual stereo→mono downmix — the stream
 * folds channels too. NULL when the device opened at exactly the source spec. */
static SDL_AudioStream  *s_avi_cvt         = NULL;
static uint8_t          *s_avi_cvt_out     = NULL;
static int               s_avi_cvt_out_cap = 0;

static void avi_cvt_free(void)
{
    if (s_avi_cvt) { SDL_FreeAudioStream(s_avi_cvt); s_avi_cvt = NULL; }
    free(s_avi_cvt_out);
    s_avi_cvt_out     = NULL;
    s_avi_cvt_out_cap = 0;
}

void plat_avi_audio_begin(int rate, int channels, int bits)
{
    SDL_AudioFormat src_fmt = (bits == 8) ? AUDIO_U8 : AUDIO_S16LSB;

    if (s_avi_open &&
        s_avi_spec.freq == rate &&
        s_avi_spec.channels == channels &&
        s_avi_spec.format == src_fmt)
        return;                                  /* same format — reuse */

    if (s_avi_open) { SDL_CloseAudioDevice(s_avi_dev); s_avi_open = 0; }
    avi_cvt_free();

    /* mmiyoo holds a single audio device slot — release the SFX/music mixer so
     * SDL_OpenAudioDevice doesn't bounce off "device already open". The mixer
     * re-opens lazily on the first play after the cutscene ends. */
    mixer_release();

    SDL_AudioSpec want = {0};
    want.freq     = rate;
    want.format   = src_fmt;
    want.channels = (Uint8)channels;
    want.samples  = AVI_AUDIO_OPEN_SAMPLES;
    /* ALLOW_FREQUENCY/CHANNELS/SAMPLES_CHANGE — the device may run at a different
     * rate (WASAPI's system mix rate) or only mono (mmiyoo); we adapt to whatever
     * it returns with the s_avi_cvt resampler below. */
    s_avi_dev = SDL_OpenAudioDevice(NULL, 0, &want, &s_avi_spec,
                                    SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
                                    SDL_AUDIO_ALLOW_CHANNELS_CHANGE |
                                    SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if (!s_avi_dev) {
        LOG_INFO("audio", "SDL_OpenAudioDevice (avi): %s", SDL_GetError());
        return;
    }
    s_avi_open = 1;

    /* Build the resampler whenever the device didn't hand back the exact source
     * spec — folds rate, format AND channels (the old manual downmix). */
    if (s_avi_spec.freq != rate || s_avi_spec.channels != channels ||
        s_avi_spec.format != src_fmt) {
        s_avi_cvt = SDL_NewAudioStream(src_fmt, (Uint8)channels, rate,
                                       s_avi_spec.format, s_avi_spec.channels,
                                       s_avi_spec.freq);
        if (!s_avi_cvt)
            LOG_INFO("audio", "SDL_NewAudioStream (avi): %s", SDL_GetError());
    }

    SDL_PauseAudioDevice(s_avi_dev, 0);
    LOG_INFO("audio", "AVI audio: src %d Hz %d ch %d-bit -> device %d Hz %d ch %d samples%s",
             rate, channels, bits, s_avi_spec.freq, s_avi_spec.channels,
             s_avi_spec.samples, s_avi_cvt ? " [resampling]" : "");
    plat_restore_system_volume();
}

void plat_avi_audio_push(void *pcm, int len)
{
    if (!s_avi_open) return;
    if (!s_avi_cvt) {                          /* device matches source spec */
        SDL_QueueAudio(s_avi_dev, pcm, (Uint32)len);
        return;
    }
    /* Resample the chunk into the device spec, then queue the converted output
     * (already at the device rate, so SDL never converts at queue time). The
     * stream also folds stereo→mono when the device came back mono (mmiyoo). */
    if (SDL_AudioStreamPut(s_avi_cvt, pcm, len) < 0) return;
    for (;;) {
        int avail = SDL_AudioStreamAvailable(s_avi_cvt);
        if (avail <= 0) break;
        if (avail > s_avi_cvt_out_cap) {
            uint8_t *n = (uint8_t *)realloc(s_avi_cvt_out, (size_t)avail);
            if (!n) return;                    /* OOM — drop the tail */
            s_avi_cvt_out     = n;
            s_avi_cvt_out_cap = avail;
        }
        int got = SDL_AudioStreamGet(s_avi_cvt, s_avi_cvt_out, avail);
        if (got <= 0) break;
        SDL_QueueAudio(s_avi_dev, s_avi_cvt_out, (Uint32)got);
    }
}

void plat_avi_audio_end(void)
{
    avi_cvt_free();
    if (!s_avi_open) return;
    SDL_CloseAudioDevice(s_avi_dev);
    s_avi_open = 0;
    s_avi_dev  = 0;
}

int plat_avi_audio_is_open(void) { return s_avi_open; }

int plat_avi_audio_below_cushion(unsigned ms)
{
    if (!s_avi_open) return 0;
    int bytes_per_sample = SDL_AUDIO_BITSIZE(s_avi_spec.format) / 8;
    uint32_t bps = (uint32_t)s_avi_spec.freq * s_avi_spec.channels *
                   (uint32_t)bytes_per_sample;
    uint32_t cushion = bps * ms / 1000;
    return SDL_GetQueuedAudioSize(s_avi_dev) < cushion;
}

void plat_avi_audio_flush(void)
{
    if (!s_avi_open) return;
    /* Pause + drop the queue (and any bytes mid-resample) so a skipped cutscene
     * stops audio immediately. */
    SDL_PauseAudioDevice(s_avi_dev, 1);
    SDL_ClearQueuedAudio(s_avi_dev);
    if (s_avi_cvt) SDL_AudioStreamClear(s_avi_cvt);
    SDL_PauseAudioDevice(s_avi_dev, 0);
}

/* The SDL FIFO holds the whole cushion, so the decoder just sleeps the
 * inter-frame wait — no need to keep feeding. */
int plat_avi_audio_needs_pump(void) { return 0; }
