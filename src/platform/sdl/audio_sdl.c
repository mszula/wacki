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
#ifdef WACKI_MIYOO
    /* mmiyoo SDL2 resets the kernel mixer to driver-default (max) on every
     * SDL_OpenAudioDevice — re-apply the user's saved OnionOS volume now so
     * audio doesn't blast at full volume until they mash Vol+/-. */
    extern void platform_restore_system_volume(void);
    platform_restore_system_volume();
#endif
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
