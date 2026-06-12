/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * include/wacki/platform/audio.h — platform audio-output HAL.
 *
 * The engine's mixer (src/audio.c) is a pure channel mixer: it produces one
 * stream of interleaved S16 PCM through a single pull callback and knows
 * nothing about how that PCM reaches the speakers. Each platform drives the
 * pull from whatever its audio output is and implements these entry points:
 *
 *   desktop / handheld  src/platform/sdl/audio_sdl.c  (SDL_OpenAudioDevice +
 *                                                      its callback thread)
 *   PS2                 src/platform_ps2.c            (audsrv + a dedicated
 *                                                      EE feeder thread; an
 *                                                      SDL audio device wedges
 *                                                      the IOP)
 *
 * This replaces the old s_mix_dev / mixer_is_open() / MIX_DEV_LOCK
 * special-casing: the mixer calls these and never #ifdefs on the platform. */
#ifndef WACKI_PLATFORM_AUDIO_H
#define WACKI_PLATFORM_AUDIO_H

/* The mixer's pull callback: fill `len` bytes of interleaved S16 PCM at
 * `stream`. The platform invokes it from whatever drives audio output (SDL's
 * callback thread, or the PS2 audsrv feeder thread), holding the channel
 * lock around the call. */
typedef void (*plat_audio_pull_fn)(void *stream, int len);

/* Open the audio output at `freq` Hz, S16, with `channels` requested, and
 * start pulling PCM from `pull`. Returns the channel count actually obtained
 * (>= 1 — some backends only do mono, and the mixer downmixes to match), or
 * 0 on failure. Idempotent: called while already open, it returns the
 * current channel count without reopening. */
int  plat_audio_open(int freq, int channels, plat_audio_pull_fn pull);

/* Release the mixer's hold on the audio output. On a backend whose device is
 * exclusive (the mmiyoo single audio slot) this closes it so an AVI can take
 * over; where the device is shared/persistent (PS2 audsrv also feeds cutscene
 * audio) it is a no-op. After it returns the mixer re-opens lazily on the
 * next play. Idempotent. */
void plat_audio_close(void);

/* Is the audio output currently up? */
int  plat_audio_is_open(void);

/* Serialise channel-array mutation against the pull callback. On SDL that's
 * the audio-device lock; on PS2 the audsrv-thread semaphore. */
void plat_audio_lock(void);
void plat_audio_unlock(void);

/* ---- cutscene (AVI) audio — a separate PUSH device ---------------- *
 *
 * Distinct from the mixer above: each cutscene opens this at its own PCM
 * format, pushes decoded chunks, and closes it at the end. On SDL it's a
 * second queue-mode SDL_AudioDevice (on a single-slot backend like mmiyoo it
 * borrows the hardware from the mixer for the duration — begin() releases the
 * mixer first); on PS2 it routes through the same audsrv feeder thread as the
 * mixer, switched to a cutscene ring. */

/* Open the cutscene audio device at the source PCM format (a no-op reopen if
 * the format is unchanged). */
void plat_avi_audio_begin(int rate, int channels, int bits);

/* Queue one decoded PCM chunk (in the source format from begin). The backend
 * converts as its device needs — e.g. the SDL backend folds stereo→mono in
 * place when it negotiated a mono device, so `pcm` may be modified. */
void plat_avi_audio_push(void *pcm, int len);

/* Close the cutscene audio device (the mixer re-opens lazily afterwards). */
void plat_avi_audio_end(void);

/* Is the cutscene audio device currently open? */
int  plat_avi_audio_is_open(void);

/* Whether less than `ms` milliseconds of audio remain queued in the device
 * FIFO — drives the decoder's read-ahead. Always 0 where the backend
 * self-paces by blocking on a tiny ring (PS2 audsrv). */
int  plat_avi_audio_below_cushion(unsigned ms);

/* Drop all queued audio (the cutscene was skipped mid-play). */
void plat_avi_audio_flush(void);

/* Whether the decoder must keep feeding audio during the inter-frame wait (1
 * on PS2 — audsrv's ring is smaller than a frame interval, so a plain sleep
 * would underrun it; 0 on SDL, whose FIFO already holds the cushion). */
int  plat_avi_audio_needs_pump(void);

#endif /* WACKI_PLATFORM_AUDIO_H */
