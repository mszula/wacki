/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * assets-explorer/src/vaudio.c — WAV playback + waveform (see vaudio.h).
 *
 * .wav assets are RIFF/WAVE, so SDL_LoadWAV_RW decodes them straight off the
 * depacked DTA buffer. Playback (re)opens an audio device matching the clip's
 * own spec and queues the samples — no resampling, no engine mixer. */

#include "vaudio.h"

#include <SDL.h>
#include <stdlib.h>
#include <string.h>

static SDL_AudioDeviceID s_dev = 0;

int vaudio_init(void)
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        SDL_Log("vaudio: SDL_INIT_AUDIO failed: %s", SDL_GetError());
        return 0;
    }
    return 1;
}

void vaudio_stop(void)
{
    if (s_dev) { SDL_CloseAudioDevice(s_dev); s_dev = 0; }
}

void vaudio_shutdown(void)
{
    vaudio_stop();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void vaudio_play(const void *wav, uint32_t sz)
{
    vaudio_stop();
    if (!wav || sz == 0) return;

    SDL_AudioSpec spec;
    Uint8  *abuf = NULL;
    Uint32  alen = 0;
    if (!SDL_LoadWAV_RW(SDL_RWFromConstMem(wav, sz), 1, &spec, &abuf, &alen))
        return;

    s_dev = SDL_OpenAudioDevice(NULL, 0, &spec, NULL, 0);
    if (s_dev) {
        SDL_QueueAudio(s_dev, abuf, alen);
        SDL_PauseAudioDevice(s_dev, 0);
    }
    SDL_FreeWAV(abuf);
}

void vaudio_play_pcm(const void *data, uint32_t len, int freq, int channels, int bits)
{
    vaudio_stop();
    if (!data || len == 0 || freq <= 0 || channels <= 0) return;

    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.freq     = freq;
    spec.format   = (bits == 8) ? AUDIO_U8 : AUDIO_S16LSB;
    spec.channels = (Uint8)channels;
    spec.samples  = 4096;

    s_dev = SDL_OpenAudioDevice(NULL, 0, &spec, NULL, 0);
    if (s_dev) {
        SDL_QueueAudio(s_dev, data, len);
        SDL_PauseAudioDevice(s_dev, 0);
    }
}

int vaudio_is_playing(void)
{
    return s_dev && SDL_GetQueuedAudioSize(s_dev) > 0;
}

int vaudio_info(const void *wav, uint32_t sz, int *hz, int *channels,
                int *bits, double *seconds)
{
    SDL_AudioSpec spec;
    Uint8  *abuf = NULL;
    Uint32  alen = 0;
    if (!SDL_LoadWAV_RW(SDL_RWFromConstMem(wav, sz), 1, &spec, &abuf, &alen))
        return 0;

    int b  = SDL_AUDIO_BITSIZE(spec.format);
    int fb = (b / 8) * spec.channels;           /* bytes per frame */
    if (hz)       *hz       = spec.freq;
    if (channels) *channels = spec.channels;
    if (bits)     *bits     = b;
    if (seconds)  *seconds  = (fb && spec.freq)
                            ? (double)alen / (double)fb / (double)spec.freq : 0.0;
    SDL_FreeWAV(abuf);
    return 1;
}

/* One channel-0 sample at frame `f`, normalised to signed 16-bit range. */
static int sample_s16(const Uint8 *p, SDL_AudioFormat fmt)
{
    switch (fmt) {
        case AUDIO_S16LSB: return (int16_t)(p[0] | (p[1] << 8));
        case AUDIO_S16MSB: return (int16_t)((p[0] << 8) | p[1]);
        case AUDIO_U8:     return ((int)p[0] - 128) * 256;
        case AUDIO_S8:     return (int)(int8_t)p[0] * 256;
        case AUDIO_S32LSB: {
            int32_t v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
            return v >> 16;
        }
        default:           return 0;
    }
}

static void put_rgba(uint8_t *o, uint8_t r, uint8_t g, uint8_t b)
{
    o[0] = r; o[1] = g; o[2] = b; o[3] = 255;
}

int vaudio_waveform(const void *wav, uint32_t sz, int w, int h, ViewImage *out)
{
    if (w <= 0 || h <= 0) return 0;

    SDL_AudioSpec spec;
    Uint8  *abuf = NULL;
    Uint32  alen = 0;
    if (!SDL_LoadWAV_RW(SDL_RWFromConstMem(wav, sz), 1, &spec, &abuf, &alen))
        return 0;

    out->rgba = (uint8_t *)malloc((size_t)w * h * 4);
    if (!out->rgba) { SDL_FreeWAV(abuf); return 0; }
    out->w = w; out->h = h;

    /* Dark background + a faint zero line. */
    for (int i = 0; i < w * h; ++i) put_rgba(out->rgba + (size_t)i * 4, 20, 20, 26);
    for (int x = 0; x < w; ++x) put_rgba(out->rgba + ((size_t)(h / 2) * w + x) * 4, 50, 50, 60);

    int      bits  = SDL_AUDIO_BITSIZE(spec.format);
    int      fb    = (bits / 8) * spec.channels;          /* bytes per frame */
    uint32_t total = fb ? alen / (uint32_t)fb : 0;

    if (total > 0) {
        for (int x = 0; x < w; ++x) {
            uint32_t a = (uint32_t)((uint64_t)x * total / w);
            uint32_t b = (uint32_t)((uint64_t)(x + 1) * total / w);
            if (b <= a) b = a + 1;
            int mn = 32767, mx = -32768;
            for (uint32_t f = a; f < b && f < total; ++f) {
                int v = sample_s16(abuf + (size_t)f * fb, spec.format);
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            int y0 = h / 2 - (mx * (h / 2)) / 32768;
            int y1 = h / 2 - (mn * (h / 2)) / 32768;
            if (y0 < 0) y0 = 0;
            if (y1 >= h) y1 = h - 1;
            for (int y = y0; y <= y1; ++y)
                put_rgba(out->rgba + ((size_t)y * w + x) * 4, 90, 200, 170);
        }
    }
    SDL_FreeWAV(abuf);
    return 1;
}
