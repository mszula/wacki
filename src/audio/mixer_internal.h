/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/audio/mixer_internal.h — private API shared between audio TUs.
 *
 * NOT a public engine header — only audio modules (mixer, music, sfx,
 * dialog playback) include this. The struct + s_mix + s_mix_dev are
 * defined in src/audio.c (mixer kernel); the SFX dispatcher in
 * src/audio/sfx.c reads them for its replay-guard checks and uses the
 * helpers below to assign / stop mixer channels.
 *
 * If you ever build the legacy Win32 path, this header is replaced by
 * the equivalent DSound-internal definitions. */

#ifndef WACKI_AUDIO_MIXER_INTERNAL_H
#define WACKI_AUDIO_MIXER_INTERNAL_H

#include <SDL.h>
#include <stdint.h>

/* ---- channel layout ----------------------------------------------- */

#define MIX_CHANNEL_COUNT       8
#define MIX_CHAN_MUSIC          0   /* reserved: looped music */
#define MIX_CHAN_DIALOG         1   /* reserved: dialog speech */
#define MIX_CHAN_SFX_START      2   /* SFX takes [2..MIX_CHANNEL_COUNT) */

/* ---- output spec (fixed; every source is converted to this) ------- */

#define MIX_OUT_FREQ          22050
#define MIX_OUT_CHANS         2          /* stereo for max compatibility */
#define MIX_OUT_FORMAT        AUDIO_S16SYS
#define MIX_OUT_SAMPLE_BYTES  (2 * MIX_OUT_CHANS)   /* S16 stereo = 4 bytes */

/* ---- per-channel state ------------------------------------------- */

struct MixChannel {
    Uint8   *buf;          /* converted to output spec, in BYTES */
    Uint32   len;          /* total bytes */
    Uint32   pos;          /* current play position (bytes) */
    int      loop;         /* 1 = loop back to 0; 0 = one-shot */
    int      active;       /* 1 = currently playing */
    uint32_t start_tick;   /* for SFX age-based stealing */
    /* T36 — per-channel stereo gain. 0..255 each, 128 = unity. */
    uint8_t  gain_l;
    uint8_t  gain_r;
    char     name[64];     /* debug name + asset-key for replay guard */
};

/* Shared mixer state (defined in audio.c). */
extern SDL_AudioDeviceID  s_mix_dev;
extern struct MixChannel  s_mix[MIX_CHANNEL_COUNT];

/* Channel-array mutex. On desktop/handheld it's SDL's audio-device lock
 * (serialises against SDL's callback thread). On PS2 there is no SDL audio
 * device — audio runs through a native audsrv thread (platform_ps2.c), so
 * the lock is an EE semaphore (g_ps2_audio_sema) shared with that thread. */
#ifdef WACKI_PS2
extern int g_ps2_audio_sema;          /* created in platform_ps2_audio_init */
extern int WaitSema(int sema_id);     /* ps2sdk <kernel.h> */
extern int SignalSema(int sema_id);
#define MIX_DEV_LOCK()   do { if (g_ps2_audio_sema >= 0) WaitSema(g_ps2_audio_sema);   } while (0)
#define MIX_DEV_UNLOCK() do { if (g_ps2_audio_sema >= 0) SignalSema(g_ps2_audio_sema); } while (0)
#else
#define MIX_DEV_LOCK()   SDL_LockAudioDevice(s_mix_dev)
#define MIX_DEV_UNLOCK() SDL_UnlockAudioDevice(s_mix_dev)
#endif

/* ---- mixer kernel API (defined in audio.c) ----------------------- */

/* Open the SDL audio device on demand. Returns 1 on success / already
 * open, 0 if SDL refused to initialize the device. */
int  mixer_ensure_open(void);

/* Load a WAV by name (root search list + .DTA archive fallback), convert
 * to the mixer's output format, and write the converted PCM to *out_buf
 * with byte length *out_len. Caller owns the returned buffer via
 * SDL_free. Returns 1 on success, 0 on failure. */
int  mixer_load_wav(const char *name, Uint8 **out_buf, Uint32 *out_len);

/* Assign a pre-converted PCM buffer to mixer channel `idx`. The mixer
 * takes ownership of `buf` (SDL_free's it when the channel is reused).
 * loop = 1 makes the channel wrap pos back to 0 instead of going
 * inactive on drain. */
void mixer_assign(int idx, Uint8 *buf, Uint32 len, int loop,
                  const char *name);

/* Stop and tear down channel `idx`. Frees the converted buffer and
 * marks the channel inactive. */
void mixer_stop_channel(int idx);

#endif /* WACKI_AUDIO_MIXER_INTERNAL_H */
