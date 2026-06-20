/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * assets-explorer/src/vaudio.h — WAV playback + waveform for the asset explorer.
 *
 * The game's .wav assets are plain RIFF/WAVE, so the viewer plays them with
 * SDL's own loader straight off the depacked DTA buffer — it does NOT link the
 * engine's mixer (which is tied to the game's channel/queue state). One device
 * is (re)opened per clip to match its format. */

#ifndef WACKI_VIEWER_VAUDIO_H
#define WACKI_VIEWER_VAUDIO_H

#include <stdint.h>
#include "render.h"   /* ViewImage */

int  vaudio_init(void);                       /* SDL_INIT_AUDIO; 1 = ok */
void vaudio_shutdown(void);

void vaudio_play(const void *wav, uint32_t sz);
/* Play raw PCM (e.g. a cutscene's audio track) — not a RIFF/WAV buffer. */
void vaudio_play_pcm(const void *data, uint32_t len, int freq, int channels, int bits);
void vaudio_stop(void);
int  vaudio_is_playing(void);                 /* 1 while samples remain queued */

/* Parse the WAV header for the meta line. Returns 1 + fills any non-NULL out. */
int  vaudio_info(const void *wav, uint32_t sz, int *hz, int *channels,
                 int *bits, double *seconds);

/* Render a min/max waveform of the clip into an RGBA ViewImage. Returns 1/0. */
int  vaudio_waveform(const void *wav, uint32_t sz, int w, int h, ViewImage *out);

#endif /* WACKI_VIEWER_VAUDIO_H */
