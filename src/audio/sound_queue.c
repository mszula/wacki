/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/audio/sound_queue.c — positional sound queue + script bridges.
 *
 * Two concerns in one module:
 *
 * 1. Positional queue. The script VM op 0x41 SOUND_PLAY pushes a
 * (source_x, source_y, sound_id, volume) tuple onto a 16-entry
 * queue. Per frame, SoundQueueMixForListener walks the queue,
 * computes distance + pan against the listener position, and
 * returns an aggregate (L, C, R) gain packet that the mixer can
 * use to set master volume.
 *
 * The original engine doesn't trigger direct playback from op 0x41
 * either — actual SFX firing is frame-driven via [sampl] tags in
 * Wacky.scr (handled by TriggerFrameSfx). Op 0x41 just contributes
 * POSITIONAL state to the aggregate gain calculation.
 *
 * 2. Script bridges. ScriptCallSoundPlay enqueues a positional source
 * and logs the computed pan; ScriptCallSoundStop resets the queue
 * and stops menu music.
 *
 * The pan calculation here is a portable approximation of the
 * original's distance + ftol triple. The shape (left-of-listener is
 * quieter on the right channel etc.) and identity-output behaviour
 * (empty queue → 0x808080) match exactly.
 */

#include "wacki.h"
#include "wacki/log.h"

#include <stdint.h>
#include <stdio.h>

extern Entity   *g_actor[2];
extern uint16_t  g_active_actor;

extern void StopMenuMusic(void);

/* ---- queue storage ------------------------------------------------- */

#define SOUND_QUEUE_MAX  16

typedef struct SoundQueueEntry {
    int16_t  x, y;
    uint16_t volume;
    uint8_t  sound_id[3];
    uint8_t  pad;
} SoundQueueEntry;

static SoundQueueEntry s_sound_queue[SOUND_QUEUE_MAX];
static uint16_t        s_sound_queue_count = 0;

void SoundQueueReset(void)
{
    s_sound_queue_count = 0;
}

void SoundQueueEnqueue(int16_t x, int16_t y, uint32_t sound_id, uint16_t volume)
{
    if (s_sound_queue_count >= SOUND_QUEUE_MAX) return;

    SoundQueueEntry *e = &s_sound_queue[s_sound_queue_count++];
    e->x       = x;
    e->y       = y;
    e->volume  = volume;
    e->sound_id[0] = (uint8_t)(sound_id      );
    e->sound_id[1] = (uint8_t)(sound_id >>  8);
    e->sound_id[2] = (uint8_t)(sound_id >> 16);
}

/* ---- mixer-facing aggregate gain ----------------------------------- */

/* Identity gain packet: each channel at 0x80 (= no attenuation). */
#define GAIN_IDENTITY_PACKET   0x00808080u

/* Distance clamp. Sources further than D_MAX pixels from the listener
 * contribute zero. Tuned for a 640×480 stage — anything more than
 * half the diagonal collapses to silence. */
#define SOUND_DISTANCE_MAX     240

/* Pack three 8-bit channel gains into a u32: R << 16 | C << 8 | L. */
static uint32_t pack_lcr(int L, int C, int R)
{
    if (L < 0)   L = 0;
    if (C < 0)   C = 0;
    if (R < 0)   R = 0;
    if (L > 255) L = 255;
    if (C > 255) C = 255;
    if (R > 255) R = 255;
    return (uint32_t)((R << 16) | (C << 8) | L);
}

uint32_t SoundQueueMixForListener(int16_t listener_x, int16_t listener_y)
{
    if (s_sound_queue_count == 0) return GAIN_IDENTITY_PACKET;

    int L = 0, C = 0, R = 0;

    for (uint16_t i = 0; i < s_sound_queue_count; ++i) {
        int dx = (int)s_sound_queue[i].x - (int)listener_x;
        int dy = (int)s_sound_queue[i].y - (int)listener_y;
        int abs_dx = dx < 0 ? -dx : dx;
        int abs_dy = dy < 0 ? -dy : dy;
        int dist_sq = abs_dx * abs_dx + 2 * abs_dy * abs_dy;
        int v       = (int)s_sound_queue[i].volume;

        if (v <= 0) continue;
        if (dist_sq > SOUND_DISTANCE_MAX * SOUND_DISTANCE_MAX) {
            dist_sq = SOUND_DISTANCE_MAX * SOUND_DISTANCE_MAX;
        }

        int contrib = (v * 256) / (dist_sq + 256);
        if (contrib > 255) contrib = 255;

        int pan = (dx * 255) / SOUND_DISTANCE_MAX;          /* -255..+255 */
        if (pan < -255) pan = -255;
        if (pan >  255) pan =  255;

        int gL = ((255 - pan) * contrib) / 510;
        int gR = ((255 + pan) * contrib) / 510;
        int gC = (contrib * (SOUND_DISTANCE_MAX - abs_dy)) /
                 (SOUND_DISTANCE_MAX + abs_dy + 1);

        L += gL;
        C += gC;
        R += gR;
    }
    return pack_lcr(L, C, R);
}

/* ---- script bridges ------------------------------------------------- */

void ScriptCallSoundPlay(uint16_t id, uint16_t a, uint32_t b, uint16_t c)
{
    /* Op 0x41 maps to: source_x=reg_id, source_y=a1, sound_id=u32, volume=a2.
 * Source coords are 16-bit signed. */
    SoundQueueEnqueue((int16_t)id, (int16_t)a, b, c);

    /* Diagnostic: compute the current aggregate pan relative to the
 * active actor's position. The result is informational — actual
 * SFX playback fires frame-driven via TriggerFrameSfx, not here. */
    int16_t lx = WACKI_SCREEN_W / 2;
    int16_t ly = WACKI_SCREEN_H / 2;
    if (g_actor[g_active_actor & 1]) {
        uint8_t *eb = (uint8_t *)g_actor[g_active_actor & 1];
        lx = *(int16_t *)(eb + 0x22);
        ly = *(int16_t *)(eb + 0x24);
    }
    uint32_t lcr = SoundQueueMixForListener(lx, ly);
    LOG_TRACE("script", "sound enqueue id=0x%04x src=(%d,%d) vol=%u "
            "pan=L%u/C%u/R%u", id, (int)id, (int)a, c, lcr & 0xFF, (lcr >> 8) & 0xFF, (lcr >> 16) & 0xFF);
}

void ScriptCallSoundStop(void)
{
    SoundQueueReset();
    StopMenuMusic();
}
