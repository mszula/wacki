/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * assets-explorer/src/vflic.h — AVI cutscene preview (Dane_30/40/50).
 *
 * The cutscene files are AVI containers (RIFF…AVI) whose video chunks ("00dc")
 * are FLC frames. We demux the AVI ourselves (index every 00dc) and decode
 * each frame with the engine's real FLIC decoder (flic_decode_frame), which
 * writes into g_back_shadow + g_palette_rgb. Decoding is stateful (delta
 * frames), so frames are produced sequentially; seeking back rewinds to 0. */

#ifndef WACKI_VIEWER_VFLIC_H
#define WACKI_VIEWER_VFLIC_H

#include <stdint.h>
#include "render.h"   /* ViewImage */

typedef struct VFlic VFlic;

/* Cheap header sniff: is `path` a RIFF…AVI cutscene? */
int     vflic_is_avi(const char *path);

VFlic  *vflic_open(const char *path);   /* parse + index 00dc frames; NULL on fail */
void    vflic_close(VFlic *vf);

int     vflic_frame_count(const VFlic *vf);
int     vflic_width(const VFlic *vf);
int     vflic_height(const VFlic *vf);
double  vflic_fps(const VFlic *vf);

/* Concatenated PCM audio track (all '01wb' chunks) + its format, or NULL/0. */
const uint8_t *vflic_audio_data(const VFlic *vf);
uint32_t       vflic_audio_len(const VFlic *vf);
int            vflic_audio_hz(const VFlic *vf);
int            vflic_audio_ch(const VFlic *vf);
int            vflic_audio_bits(const VFlic *vf);

/* Decode up to frame `idx` (sequential; rewinds to 0 if idx < current) and
 * render the resulting 8bpp buffer + FLIC palette into RGBA. Returns 1/0. */
int     vflic_render_frame(VFlic *vf, int idx, ViewImage *out);

#endif /* WACKI_VIEWER_VFLIC_H */
