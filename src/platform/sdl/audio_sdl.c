/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/sdl/audio_sdl.c — audio-output HAL, SDL backend.
 *
 * Desktop + handheld implementation of plat_audio_* (wacki/platform/audio.h):
 * one SDL audio device whose callback pulls mixed PCM from the engine mixer.
 * The PS2 backend (audsrv + an EE feeder thread) lives in platform_ps2.c. */

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
static int               s_avi_src_channels = 2;   /* source PCM, from begin() */
static int               s_avi_src_bits     = 16;

void plat_avi_audio_begin(int rate, int channels, int bits)
{
    s_avi_src_channels = channels;
    s_avi_src_bits     = bits;

    if (s_avi_open &&
        s_avi_spec.freq == rate &&
        s_avi_spec.channels == channels &&
        s_avi_spec.format == (SDL_AudioFormat)(bits == 8 ? AUDIO_U8 : AUDIO_S16LSB))
        return;                                  /* same format — reuse */

    if (s_avi_open) { SDL_CloseAudioDevice(s_avi_dev); s_avi_open = 0; }

    /* mmiyoo holds a single audio device slot — release the SFX/music mixer so
     * SDL_OpenAudioDevice doesn't bounce off "device already open". The mixer
     * re-opens lazily on the first play after the cutscene ends. */
    extern void mixer_release(void);
    mixer_release();

    SDL_AudioSpec want = {0};
    want.freq     = rate;
    want.format   = (bits == 8) ? AUDIO_U8 : AUDIO_S16LSB;
    want.channels = (Uint8)channels;
    want.samples  = AVI_AUDIO_OPEN_SAMPLES;
    /* ALLOW_CHANNELS/SAMPLES_CHANGE — mmiyoo only does mono + a fixed buffer;
     * without these a stereo AVI fails to open. We downmix in push() if the
     * device came back mono. */
    s_avi_dev = SDL_OpenAudioDevice(NULL, 0, &want, &s_avi_spec,
                                    SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
                                    SDL_AUDIO_ALLOW_CHANNELS_CHANGE |
                                    SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if (!s_avi_dev) {
        LOG_INFO("audio", "SDL_OpenAudioDevice (avi): %s", SDL_GetError());
        return;
    }
    s_avi_open = 1;
    SDL_PauseAudioDevice(s_avi_dev, 0);
    LOG_INFO("audio", "AVI audio: %d Hz, %d ch, %d samples",
             s_avi_spec.freq, s_avi_spec.channels, s_avi_spec.samples);
    plat_restore_system_volume();
}

void plat_avi_audio_push(void *pcm, int len)
{
    if (!s_avi_open) return;
    /* Source stereo but device negotiated mono (mmiyoo) → fold L+R → (L+R)/2
     * in place before queuing, else the mono device reads stereo bytes as a
     * mono stream and plays at double speed with channel-alternation garble. */
    if (s_avi_src_channels == 2 && s_avi_spec.channels == 1 &&
        s_avi_src_bits == 16 && (len & 3) == 0) {
        int16_t *a = (int16_t *)pcm;
        int frames = len / 4;
        for (int i = 0; i < frames; ++i) {
            int v = (int)a[i * 2] + (int)a[i * 2 + 1];
            a[i] = (int16_t)(v / 2);
        }
        SDL_QueueAudio(s_avi_dev, pcm, (Uint32)(frames * 2));
    } else {
        SDL_QueueAudio(s_avi_dev, pcm, (Uint32)len);
    }
}

void plat_avi_audio_end(void)
{
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
    /* Pause + drop the queue so a skipped cutscene stops audio immediately. */
    SDL_PauseAudioDevice(s_avi_dev, 1);
    SDL_ClearQueuedAudio(s_avi_dev);
    SDL_PauseAudioDevice(s_avi_dev, 0);
}

/* The SDL FIFO holds the whole cushion, so the decoder just sleeps the
 * inter-frame wait — no need to keep feeding. */
int plat_avi_audio_needs_pump(void) { return 0; }
