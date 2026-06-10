/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * audio.c — SDL audio mixer + music/sfx/dialog dispatch.
 *
 * The cutscene playback shim (PlaySceneCutsceneAvi + InitializeDirect
 * Sound) lives in src/audio/cutscene.c — it's the AVI/FLIC entry point
 * and has no direct dependency on the mixer below.
 *
 * What stays here (for now):
 *   - The SDL_AudioDevice mixer core + per-channel WAV loader
 *   - Music API   (PlayMenuMusic / StopMenuMusic / TickMenuMusic)
 *   - SFX dispatch (Wacky.scr [sampl] parser + TriggerFrameSfx +
 *                   PlaySfx / PlaySfxLoopAndGetChannel / ...)
 *   - Dialog line (PlayDialogLine)
 *   - The per-flag gates (g_audio_music_enabled, _sfx_enabled, etc.)
 *
 * Pulled-apart split into src/audio/{mixer,music,sfx}.c is feasible
 * but blocked on exposing the s_mix array + helpers across TUs. */

#include "wacki.h"
#include "wacki/log.h"
#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- *
 * Audio mixer (T6).
 *
 * One SDL audio device with a callback that mixes N channels into stereo
 * S16 22050 Hz output. Each channel owns a pre-converted PCM buffer
 * (SDL_BuildAudioCVT + SDL_ConvertAudio at load time) and a play
 * position. Channels can loop (music) or one-shot (SFX, dialog).
 *
 * Replaces the earlier hack of opening a separate SDL_AudioDevice per
 * SFX slot (4 devices) + a music device (5 total) — wasteful, prone
 * to format mismatch surprises. Mixer fixes both: single device,
 * uniform output spec, all streams normalized at load time.
 *
 * Public API preserved (PlaySfx, PlayMenuMusic, StopMenuMusic,
 * TickMenuMusic, TickSfx all keep their signatures + semantics).
 * New: PlayDialogLine — dedicated channel for per-line dialog audio.
 * ------------------------------------------------------------------------- */
/* MIX_CHANNEL_COUNT, MIX_CHAN_*, MixChannel, s_mix[], and s_mix_dev
 * are declared in audio/mixer_internal.h so src/audio/sfx.c can read
 * the channel array for its replay-guard check. */
#include "audio/mixer_internal.h"
#include "audio/music_stream.h"

/* MIX_OUT_* output-spec macros live in mixer_internal.h (shared with the
 * music-stream TU). */
#define MIX_GAIN_IDENTITY     128        /* per-channel gain: 128 = unity */
#define MIX_OPEN_SAMPLES      2048       /* device buffer in frames */

SDL_AudioDeviceID s_mix_dev = 0;
static SDL_AudioSpec     s_mix_spec;
struct MixChannel s_mix[MIX_CHANNEL_COUNT];

/* Some embedded SDL2 backends (notably mmiyoo on Miyoo Mini Plus)
 * only support mono output. We always mix internally in stereo for
 * positional pan; if the backend ended up mono after the OpenAudio
 * call, the callback downmixes L+R into a single channel at the
 * very end. s_dev_chans tracks what we actually got. */
#define MIX_MAX_FRAMES_PER_CB   4096
static int      s_dev_chans = MIX_OUT_CHANS;
static int16_t  s_mix_tmp[MIX_MAX_FRAMES_PER_CB * MIX_OUT_CHANS];

/* Audio callback — fires on SDL's audio thread. Mix all active channels
 * into a stereo intermediate, then write to the device buffer either
 * as stereo (memcpy) or mono (L+R downmix) depending on what the
 * backend gave us. */
static void mixer_callback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    /* len is in bytes. Convert to per-device-frame count given the
     * actual channel count we negotiated. */
    int dev_chans = s_dev_chans > 0 ? s_dev_chans : MIX_OUT_CHANS;
    int n_samples = len / 2;          /* S16 = 2 bytes per sample slot */
    int n_frames  = n_samples / dev_chans;
    if (n_frames > MIX_MAX_FRAMES_PER_CB) n_frames = MIX_MAX_FRAMES_PER_CB;

    /* Mix everything as stereo into the scratch buffer; positional pan
     * applies cleanly regardless of what the device wants. */
    int16_t *mix = s_mix_tmp;
    SDL_memset(mix, 0, (size_t)(n_frames * MIX_OUT_CHANS * 2));

    for (int c = 0; c < MIX_CHANNEL_COUNT; ++c) {
        struct MixChannel *ch = &s_mix[c];
        if (!ch->active || !ch->buf || ch->len == 0) continue;
        int16_t *src           = (int16_t *)ch->buf;
        Uint32   src_frame     = ch->pos / MIX_OUT_SAMPLE_BYTES;
        Uint32   src_frame_end = ch->len / MIX_OUT_SAMPLE_BYTES;
        /* T36: per-channel gain. MIX_GAIN_IDENTITY = no attenuation;
         * anything else multiplies each sample. Cast to int so the
         * intermediate (src * gain) doesn't overflow at gain=255. */
        int      gain_l        = ch->gain_l;
        int      gain_r        = ch->gain_r;
        for (int f = 0; f < n_frames; ++f) {
            if (src_frame >= src_frame_end) {
                if (ch->loop) {
                    src_frame = 0;
                } else {
                    ch->active = 0;
                    break;
                }
            }
            int16_t sl = src[src_frame * 2 + 0];
            int16_t sr = src[src_frame * 2 + 1];
            ++src_frame;
            int ml = (int)mix[f * 2 + 0] + (sl * gain_l) / MIX_GAIN_IDENTITY;
            int mr = (int)mix[f * 2 + 1] + (sr * gain_r) / MIX_GAIN_IDENTITY;
            if (ml >  32767) ml =  32767;
            if (ml < -32768) ml = -32768;
            if (mr >  32767) mr =  32767;
            if (mr < -32768) mr = -32768;
            mix[f * 2 + 0] = (int16_t)ml;
            mix[f * 2 + 1] = (int16_t)mr;
        }
        ch->pos = src_frame * MIX_OUT_SAMPLE_BYTES;
    }

    /* Write to the device buffer in whatever channel layout we got. */
    int16_t *out = (int16_t *)stream;
    if (dev_chans == 1) {
        /* Downmix L+R → 1 channel. Average rather than sum so a
         * full-scale stereo pair doesn't clip on output. */
        for (int f = 0; f < n_frames; ++f) {
            int l = mix[f * 2 + 0];
            int r = mix[f * 2 + 1];
            out[f] = (int16_t)((l + r) / 2);
        }
    } else {
        /* Native stereo (or higher — first 2 channels carry the mix,
         * any extras stay silent from the memset above). */
        if (dev_chans == 2) {
            SDL_memcpy(out, mix, (size_t)(n_frames * 4));
        } else {
            for (int f = 0; f < n_frames; ++f) {
                out[f * dev_chans + 0] = mix[f * 2 + 0];
                out[f * dev_chans + 1] = mix[f * 2 + 1];
                for (int x = 2; x < dev_chans; ++x)
                    out[f * dev_chans + x] = 0;
            }
        }
    }
}

/* Ensure the mixer device + spec is open. Idempotent. */
int mixer_ensure_open(void)
{
    if (s_mix_dev) return 1;
#ifdef WACKI_PS2
    /* PS2 runs silent — opening an SDL audio device wedges audsrv and
     * hangs the IOP (see platform_sdl.c). The mixer + every PlaySfx /
     * dialog path already tolerates "no device open", so just report it
     * unavailable and never touch SDL audio. */
    return 0;
#endif
    SDL_AudioSpec want;
    SDL_memset(&want, 0, sizeof want);
    want.freq     = MIX_OUT_FREQ;
    want.format   = MIX_OUT_FORMAT;
    want.channels = MIX_OUT_CHANS;
    want.samples  = MIX_OPEN_SAMPLES;
    want.callback = mixer_callback;
    want.userdata = NULL;
    /* SDL_AUDIO_ALLOW_CHANNELS_CHANGE + SAMPLES_CHANGE — embedded SDL2
     * backends (mmiyoo on Miyoo Mini Plus) only do mono S16 at a fixed
     * buffer size. With allowed_changes=0 SDL2 silently fails to set
     * up its stereo→mono conversion stream and leaves the backend
     * wedged in "device-already-open" state, so every subsequent
     * SFX play retries and bounces off it. Allowing the channel +
     * sample count to flex means we just take whatever we got; the
     * callback downmixes if we ended up mono. Frequency stays pinned
     * because every WAV is pre-converted to 22 050 Hz. */
    s_mix_dev = SDL_OpenAudioDevice(NULL, 0, &want, &s_mix_spec,
                                    SDL_AUDIO_ALLOW_CHANNELS_CHANGE |
                                    SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if (!s_mix_dev) {
        /* Log only the first attempt — on Miyoo / OnionOS the
         * audioserver kill is async, so the first dozen-ish attempts
         * race the dying process and fail with "Audio device already
         * open". Repeating the line each frame just floods wacki.log. */
        static int s_logged_open_failure = 0;
        if (!s_logged_open_failure) {
            LOG_INFO("mixer", "SDL_OpenAudioDevice failed: %s "
                              "(will retry silently on next play)",
                     SDL_GetError());
            s_logged_open_failure = 1;
        }
        return 0;
    }
    s_dev_chans = s_mix_spec.channels;
    SDL_PauseAudioDevice(s_mix_dev, 0);   /* unpause */
    LOG_INFO("mixer", "opened: %d Hz, %d ch, %d-bit, %d samples",
             s_mix_spec.freq, s_mix_spec.channels,
             SDL_AUDIO_BITSIZE(s_mix_spec.format), s_mix_spec.samples);
#ifdef WACKI_MIYOO
    /* mmiyoo SDL2 backend resets the kernel mixer to driver-default
     * (max) on every SDL_OpenAudioDevice — re-apply the user's saved
     * OnionOS volume now so SFX/music doesn't blast at full volume
     * until the user mashes Vol+/- to wake the firmware handler. */
    extern void platform_restore_system_volume(void);
    platform_restore_system_volume();
#endif
    return 1;
}

/* Tear down the SDL mixer device so the mmiyoo single-slot audio
 * hardware is free for an AVI playback. Pauses + closes the device,
 * marks all channels inactive (so any lingering refs don't think
 * playback is still happening) and zeroes s_mix_dev. The next
 * mixer_ensure_open call re-opens lazily on the first SFX/music
 * play after the AVI returns.
 *
 * Needed on mmiyoo specifically: the backend wraps a single hardware
 * audio-out slot and refuses a second SDL_OpenAudioDevice with
 * "Audio device already open". Without this, replaying the intro or
 * Credits cutscene from the menu silently dropped audio because the
 * mixer was holding the slot. */
void mixer_release(void)
{
    if (!s_mix_dev) return;
    SDL_PauseAudioDevice(s_mix_dev, 1);
    SDL_LockAudioDevice(s_mix_dev);
    for (int i = 0; i < MIX_CHANNEL_COUNT; ++i) {
        s_mix[i].active = 0;
        if (s_mix[i].buf) { SDL_free(s_mix[i].buf); s_mix[i].buf = NULL; }
        s_mix[i].len = 0;
        s_mix[i].pos = 0;
    }
    SDL_UnlockAudioDevice(s_mix_dev);
    SDL_CloseAudioDevice(s_mix_dev);
    s_mix_dev = 0;
    LOG_INFO("mixer", "released (next play re-opens lazily)");
}

/* Load a WAV (from filesystem or DTA) and convert to output spec.
 * Returns 1 on success with out_buf/out_len owned by caller (must SDL_free
 * after audio callback no longer references it). */
int mixer_load_wav(const char *name, Uint8 **out_buf, Uint32 *out_len);

/* Lock device, assign to channel `idx`, unlock, unpause. Frees previous
 * buf if any. */
void mixer_assign(int idx, Uint8 *buf, Uint32 len, int loop,
                         const char *name)
{
    if (idx < 0 || idx >= MIX_CHANNEL_COUNT) return;
    SDL_LockAudioDevice(s_mix_dev);
    if (s_mix[idx].buf) SDL_free(s_mix[idx].buf);
    s_mix[idx].buf    = buf;
    s_mix[idx].len    = len;
    s_mix[idx].pos    = 0;
    s_mix[idx].loop   = loop;
    s_mix[idx].active = (buf && len > 0) ? 1 : 0;
    /* Default to identity gain (no pan). Callers can override after
     * the channel is loaded. */
    s_mix[idx].gain_l = MIX_GAIN_IDENTITY;
    s_mix[idx].gain_r = MIX_GAIN_IDENTITY;
    extern uint32_t g_tick_counter;
    s_mix[idx].start_tick = g_tick_counter;
    if (name) {
        size_t n = strlen(name);
        if (n >= sizeof s_mix[idx].name) n = sizeof s_mix[idx].name - 1;
        memcpy(s_mix[idx].name, name, n);
        s_mix[idx].name[n] = 0;
    } else {
        s_mix[idx].name[0] = 0;
    }
    SDL_UnlockAudioDevice(s_mix_dev);
}

void mixer_stop_channel(int idx)
{
    if (idx < 0 || idx >= MIX_CHANNEL_COUNT) return;
    SDL_LockAudioDevice(s_mix_dev);
    s_mix[idx].active = 0;
    if (s_mix[idx].buf) {
        SDL_free(s_mix[idx].buf);
        s_mix[idx].buf = NULL;
    }
    s_mix[idx].len = 0;
    s_mix[idx].pos = 0;
    s_mix[idx].name[0] = 0;
    SDL_UnlockAudioDevice(s_mix_dev);
}

/* ------------------------------------------------------------------------- *
 * Menu / background WAV music — backed by mixer channel MIX_CHAN_MUSIC.
 * ------------------------------------------------------------------------- */

static int try_load_wav_at(const char *root, const char *name,
                           SDL_AudioSpec *out_spec, Uint8 **out_buf,
                           Uint32 *out_len)
{
    char p[1024];
    if (root && *root) snprintf(p, sizeof p, "%s/%s", root, name);
    else               snprintf(p, sizeof p, "%s", name);
    if (SDL_LoadWAV(p, out_spec, out_buf, out_len))
        return 1;
    /* upper-case the basename (CD layout on macOS often) */
    size_t l = strlen(p);
    size_t i = l;
    while (i > 0 && p[i-1] != '/' && p[i-1] != '\\') --i;
    for (size_t j = i; j < l; ++j)
        if (p[j] >= 'a' && p[j] <= 'z') p[j] &= 0xDF;
    return SDL_LoadWAV(p, out_spec, out_buf, out_len) != NULL;
}

/* Try to load the WAV as an entry inside the currently mounted .dta
 * archive (wybuch.wav, items.wav, etc. live there — not on disk). */
static int try_load_wav_from_dta(const char *name, SDL_AudioSpec *out_spec,
                                 Uint8 **out_buf, Uint32 *out_len)
{
    void    *raw = NULL;
    uint32_t sz  = 0;
    if (!LoadFileFromDta(name, &raw, &sz) || !raw || sz < 12) return 0;
    const uint8_t *p = (const uint8_t *)raw;
    if (!(p[0]=='R' && p[1]=='I' && p[2]=='F' && p[3]=='F' &&
          p[8]=='W' && p[9]=='A' && p[10]=='V' && p[11]=='E')) {
        xfree(raw);
        return 0;
    }
    SDL_RWops *rw = SDL_RWFromConstMem(raw, (int)sz);
    int ok = SDL_LoadWAV_RW(rw, 1, out_spec, out_buf, out_len) != NULL;
    xfree(raw);
    return ok;
}

/* mixer_load_wav — combined disk + DTA WAV load, then convert to mixer
 * output spec (S16 stereo 22050 Hz). Returns 1 + sets *out_buf (owned by
 * caller, free via SDL_free) + *out_len. */
int mixer_load_wav(const char *name, Uint8 **out_buf, Uint32 *out_len)
{
    SDL_AudioSpec native;
    Uint8 *native_buf = NULL;
    Uint32 native_len = 0;

    /* Try the mounted DTA archive FIRST. Every WAV the game references
     * lives there — falling through the filesystem paths first means
     * each SFX play does 6 failed fopen() syscalls before reaching the
     * archive. On a slow SD card (Miyoo Mini Plus) that's 30+ ms of
     * dead time per play, plus a flood of "Couldn't open ..." prints
     * from SDL2's RWops layer. Filesystem fallback stays for dev
     * overrides — drop a custom WAV next to the binary or in ./data/
     * and it'll be picked up if the archive misses. */
    int ok = 0;
    if (!ok) ok = try_load_wav_from_dta(name, &native, &native_buf, &native_len);
    if (!ok) ok = try_load_wav_at(NULL,      name, &native, &native_buf, &native_len);
    if (!ok) ok = try_load_wav_at(g_data_root, name, &native, &native_buf, &native_len);
    if (!ok) ok = try_load_wav_at("./data",  name, &native, &native_buf, &native_len);
    if (!ok) {
        *out_buf = NULL; *out_len = 0;
        return 0;
    }

    /* Already in target format? Just transfer ownership. */
    if (native.freq == MIX_OUT_FREQ &&
        native.format == MIX_OUT_FORMAT &&
        native.channels == MIX_OUT_CHANS) {
        /* Take a freshly malloc'd copy so caller can SDL_free it
         * (WAV buffer must use SDL_FreeWAV; can't mix allocators). */
        Uint8 *copy = (Uint8 *)SDL_malloc(native_len);
        if (!copy) {
            SDL_FreeWAV(native_buf);
            *out_buf = NULL; *out_len = 0;
            return 0;
        }
        SDL_memcpy(copy, native_buf, native_len);
        SDL_FreeWAV(native_buf);
        *out_buf = copy;
        *out_len = native_len;
        return 1;
    }

    /* Build CVT and convert in-place via SDL_ConvertAudio. */
    SDL_AudioCVT cvt;
    if (SDL_BuildAudioCVT(&cvt, native.format, native.channels, native.freq,
                          MIX_OUT_FORMAT, MIX_OUT_CHANS, MIX_OUT_FREQ) < 0) {
        LOG_TRACE("mixer", "SDL_BuildAudioCVT failed for %s: %s", name, SDL_GetError());
        SDL_FreeWAV(native_buf);
        return 0;
    }
    /* CVT needs a buffer sized native_len * cvt.len_mult. Allocate
     * fresh via SDL_malloc (so SDL_free can release it), copy native
     * into it. */
    Uint32 buf_size = native_len * (cvt.len_mult ? cvt.len_mult : 1);
    Uint8 *buf = (Uint8 *)SDL_malloc(buf_size);
    if (!buf) {
        SDL_FreeWAV(native_buf);
        return 0;
    }
    SDL_memcpy(buf, native_buf, native_len);
    SDL_FreeWAV(native_buf);
    cvt.buf = buf;
    cvt.len = (int)native_len;
    if (SDL_ConvertAudio(&cvt) != 0) {
        LOG_TRACE("mixer", "SDL_ConvertAudio failed for %s: %s", name, SDL_GetError());
        SDL_free(buf);
        return 0;
    }
    *out_buf = buf;
    *out_len = (Uint32)cvt.len_cvt;
    return 1;
}

/* Options-menu toggles — wired by Solund handler in game.c. Defaults to
 * enabled. When music is toggled off mid-play, we stop the channel and
 * remember the last track so the toggle-back-on path can resume it. */
/* T103 fix — flag mapping inferred from LoadSaveStateOrInitialize:
 *   speech_y_offset    → g_audio_sfx_enabled
 *   speech_text_attr   → g_audio_music_enabled
 *   speech_color_index → g_audio_sound_enabled (legacy master mute)
 *   fade_target        → g_audio_voice_enabled
 *
 * g_audio_sound_enabled is a hold-over master mute (the field was once
 * mis-mapped to "sfx" via fade_progress, which actually drives
 * dialogues_on). Kept so existing call sites continue to compile. */
int      g_audio_music_enabled = 1;       /* speech_text_attr mirror */
int      g_audio_sfx_enabled   = 1;       /* speech_y_offset mirror — sfx */
int      g_audio_voice_enabled = 1;       /* fade_target mirror — dialog audio */
int      g_audio_sound_enabled = 1;       /* speech_color_index mirror (legacy global) */
static char  s_last_music_name[64] = "";
static int   s_last_music_loop    = 0;

void StopMenuMusic(void)
{
    /* Tear down any active stream first (so the producer won't touch the
     * ring), then stop the channel — which frees the ring it owns. */
    music_stream_stop();
    if (s_mix_dev) mixer_stop_channel(MIX_CHAN_MUSIC);
}

void PlayMenuMusic(const char *dta_name, int loop)
{
    if (!dta_name) {
        StopMenuMusic();
        s_last_music_name[0] = 0;
        s_last_music_loop = 0;
        return;
    }
    /* Remember the last requested track so AudioSetMusicEnabled(1)
     * can resume play after a mute-toggle. */
    snprintf(s_last_music_name, sizeof s_last_music_name, "%s", dta_name);
    s_last_music_loop = loop ? 1 : 0;

    if (!g_audio_music_enabled || !g_audio_sound_enabled) {
        StopMenuMusic();
        return;
    }
    if (!mixer_ensure_open()) return;
    StopMenuMusic();

    /* Large standalone BGM (e.g. the ~25 MB menu music) → stream a looping
     * ring instead of loading the whole file into RAM. Small archived clips
     * fall through to the whole-file load below. */
    if (music_stream_try_open(dta_name, loop)) return;

    Uint8 *buf = NULL; Uint32 len = 0;
    if (!mixer_load_wav(dta_name, &buf, &len)) {
        LOG_TRACE("music", "cannot find/decode %s as WAV", dta_name);
        return;
    }
    mixer_assign(MIX_CHAN_MUSIC, buf, len, loop ? 1 : 0, dta_name);
    LOG_TRACE("music", "%s playing: %u bytes converted (loop=%d) on mixer ch %d", dta_name, len, loop ? 1 : 0, MIX_CHAN_MUSIC);
}

/* Toggle hook — called by the Solund options handler when the user
 * clicks the music on/off button. If muting mid-play, stops the
 * channel. If un-muting, re-issues PlayMenuMusic with the last
 * remembered track to resume. */
void AudioSetMusicEnabled(int on)
{
    int was = g_audio_music_enabled;
    g_audio_music_enabled = on ? 1 : 0;
    if (was && !on) {
        StopMenuMusic();
    } else if (!was && on && s_last_music_name[0]) {
        PlayMenuMusic(s_last_music_name, s_last_music_loop);
    }
}

void AudioSetSfxEnabled(int on)
{
    g_audio_sfx_enabled = on ? 1 : 0;
}

/* T103 — voice toggle. Gates PlayDialogLine via g_audio_voice_enabled
 * check at the head. If toggled off mid-line, stops the dialog channel
 * immediately so the user-facing effect is instant. */
void AudioSetVoiceEnabled(int on)
{
    int was = g_audio_voice_enabled;
    g_audio_voice_enabled = on ? 1 : 0;
    if (was && !on && s_mix_dev) mixer_stop_channel(MIX_CHAN_DIALOG);
}

/* Global sound mute — kills both music + SFX while clear,
 * resumes music when set back on. */
void AudioSetSoundEnabled(int on)
{
    int was = g_audio_sound_enabled;
    g_audio_sound_enabled = on ? 1 : 0;
    if (was && !on) {
        StopMenuMusic();
    } else if (!was && on && s_last_music_name[0] && g_audio_music_enabled) {
        PlayMenuMusic(s_last_music_name, s_last_music_loop);
    }
}

void TickMenuMusic(void)
{
    /* Drive the streaming menu BGM. No-op unless a large BGM is currently
     * streaming — whole-loaded music loops natively in the mixer callback.
     * Called once per frame by the menu loop. */
    music_stream_refill();
}


/* PlayDialogLine — T6: dedicated dialog speech channel.
 * Uses MIX_CHAN_DIALOG (reserved). Returns the byte length of the
 * loaded audio (so caller can compute approximate duration) or 0 on
 * failure. */
uint32_t PlayDialogLine(const char *wav_name)
{
    if (!wav_name) return 0;
    /* T103 — voice gate: if the user disabled voice in Solund, drop
     * the sample silently. Caller still proceeds with text /
     * animation. */
    if (!g_audio_voice_enabled || !g_audio_sound_enabled) return 0;
    if (!mixer_ensure_open()) return 0;
    Uint8 *buf = NULL; Uint32 len = 0;
    if (!mixer_load_wav(wav_name, &buf, &len)) {
        LOG_TRACE("dialog", "cannot load '%s'", wav_name);
        return 0;
    }
    mixer_assign(MIX_CHAN_DIALOG, buf, len, 0, wav_name);
    LOG_TRACE("dialog", "play '%s' on mixer ch %d (%u bytes)", wav_name, MIX_CHAN_DIALOG, len);
    return len;
}

/* StopDialogLine — used to cancel mid-line (e.g. user click-to-skip). */
void StopDialogLine(void)
{
    if (!s_mix_dev) return;
    mixer_stop_channel(MIX_CHAN_DIALOG);
}

/* IsDialogLinePlaying — for lip-sync polling. */
int IsDialogLinePlaying(void)
{
    return s_mix_dev && s_mix[MIX_CHAN_DIALOG].active;
}

/* TickSfx — collect drained channels. Callback already sets active=0
 * when channel finishes; just free its buf here. Avoids touching shared
 * state during audio callback. */
void TickSfx(void)
{
    if (!s_mix_dev) return;
    for (int i = MIX_CHAN_SFX_START; i < MIX_CHANNEL_COUNT; ++i) {
        SDL_LockAudioDevice(s_mix_dev);
        if (!s_mix[i].active && s_mix[i].buf) {
            SDL_free(s_mix[i].buf);
            s_mix[i].buf = NULL;
            s_mix[i].len = 0;
        }
        SDL_UnlockAudioDevice(s_mix_dev);
    }
}
