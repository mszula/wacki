/*
 * audio.c — portable audio stubs.
 *
 * The original engine drove DirectSound (for SFX), MCI cdaudio (for music
 * straight off CD-DA tracks), and MCI AVIVideo (for cutscenes). None of
 * those exist on Mac/Linux, and the engine doesn't have anywhere to play
 * sound from without the original CD anyway, so this file is reduced to
 * no-op implementations matching the Ghidra prototypes.
 *
 * If you ever build the legacy Win32 path (WACKI_WITH_WIN32 + ddraw/dsound),
 * the original audio.c (kept in version control as audio_win32.c) can be
 * dropped in to restore real MCI playback.
 */
#include "wacki.h"
#include <stdio.h>

/* ------------------------------------------------------------------------- *
 * InitializeDirectSound — 0x0040D0A0
 * In the SDL build we always succeed; nothing actually plays.
 * ------------------------------------------------------------------------- */
int InitializeDirectSound(void) { return 0; }

/* ------------------------------------------------------------------------- *
 * PlaySceneCutsceneAvi — 0x0040A430
 *
 * The original called MCI AVIVideo to play Dane_10.dta (intro), Dane_14.dta
 * (death), etc. Those files are encoded as Autodesk FLIC (AFLC fourCC)
 * inside an AVI container — a custom decoder we don't ship. For now show
 * a 1-second placeholder splash so the user knows a cutscene would play.
 *
 * To get real playback, drop in a FLIC decoder (or use libavformat) and
 * blit frames into the back-buffer via PaintImageToBackbuffer.
 */
#include "wacki.h"
#include <SDL.h>

extern int PlayFlicAviFile(const char *path);   /* flic.c */
extern char g_cd_path[260];

/* Try to open `name` relative to several roots; on macOS auto-uppercase
 * the file part on the second pass. */
static int try_play_at(const char *root, const char *name)
{
    char p[1024];
    if (root && *root) snprintf(p, sizeof p, "%s/%s", root, name);
    else               snprintf(p, sizeof p, "%s", name);
    if (PlayFlicAviFile(p)) return 1;
    /* uppercase the last path component */
    size_t l = strlen(p);
    size_t i = l;
    while (i > 0 && p[i-1] != '/' && p[i-1] != '\\') --i;
    for (size_t j = i; j < l; ++j)
        if (p[j] >= 'a' && p[j] <= 'z') p[j] &= 0xDF;
    return PlayFlicAviFile(p);
}

void PlaySceneCutsceneAvi(const char *avi_name)
{
    if (!avi_name) return;
    if (try_play_at(NULL,       avi_name)) return;
    if (try_play_at(g_cd_path,  avi_name)) return;
    if (try_play_at("./data",   avi_name)) return;
}

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
#define MIX_CHANNEL_COUNT   8           /* total mixer channels */
#define MIX_CHAN_MUSIC      0           /* reserved: looped music */
#define MIX_CHAN_DIALOG     1           /* reserved: dialog speech */
#define MIX_CHAN_SFX_START  2           /* SFX takes channels [2..MIX_CHANNEL_COUNT) */

#define MIX_OUT_FREQ        22050
#define MIX_OUT_CHANS       2           /* stereo for max compatibility */
#define MIX_OUT_FORMAT      AUDIO_S16SYS
#define MIX_OUT_SAMPLE_BYTES (2 * MIX_OUT_CHANS)   /* S16 stereo = 4 bytes */

struct MixChannel {
    Uint8   *buf;          /* converted to output spec, in BYTES */
    Uint32   len;          /* total bytes */
    Uint32   pos;          /* current play position (bytes) */
    int      loop;         /* 1 = loop back to 0; 0 = one-shot */
    int      active;       /* 1 = currently playing */
    uint32_t start_tick;   /* for SFX age-based stealing */
    /* T36 — per-channel stereo gain. 0..255 each, 128 = unity (1.0).
 * Callback multiplies sample by gain/128, allowing per-source
 * stereo pan derived from positional source/listener distance
 * (SoundQueueMixForListener). Music + dialog default to 128/128
 * (no spatial pan); SFX get gains computed at PlaySfx time. */
    uint8_t  gain_l;
    uint8_t  gain_r;
    char     name[64];     /* debug name */
};

static SDL_AudioDeviceID s_mix_dev = 0;
static SDL_AudioSpec     s_mix_spec;
static struct MixChannel s_mix[MIX_CHANNEL_COUNT];

/* Audio callback — fires on SDL's audio thread. Mix all active channels
 * into the output buffer with saturation clipping. */
static void mixer_callback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    /* Start with silence. */
    SDL_memset(stream, 0, (size_t)len);
    int16_t *out = (int16_t *)stream;
    int n_samples = len / 2;        /* S16 = 2 bytes per sample slot */

    /* MIX_OUT_CHANS = 2 (stereo); each "frame" is 2 S16 samples.
 * Number of frames = n_samples / 2. */
    int n_frames = n_samples / MIX_OUT_CHANS;

    for (int c = 0; c < MIX_CHANNEL_COUNT; ++c) {
        struct MixChannel *ch = &s_mix[c];
        if (!ch->active || !ch->buf || ch->len == 0) continue;
        int16_t *src = (int16_t *)ch->buf;
        Uint32 src_frame = ch->pos / (2 * MIX_OUT_CHANS);    /* in frames */
        Uint32 src_frame_end = ch->len / (2 * MIX_OUT_CHANS);
        /* T36: per-channel gain. 128 = identity. Anything else applies
 * a multiplicative attenuation per sample. Cast to int so the
 * intermediate (src * gain) doesn't overflow at gain=255. */
        int gain_l = ch->gain_l;
        int gain_r = ch->gain_r;
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
            int ml = (int)out[f * 2 + 0] + (sl * gain_l) / 128;
            int mr = (int)out[f * 2 + 1] + (sr * gain_r) / 128;
            if (ml >  32767) ml =  32767;
            if (ml < -32768) ml = -32768;
            if (mr >  32767) mr =  32767;
            if (mr < -32768) mr = -32768;
            out[f * 2 + 0] = (int16_t)ml;
            out[f * 2 + 1] = (int16_t)mr;
        }
        ch->pos = src_frame * (2 * MIX_OUT_CHANS);
    }
}

/* Ensure the mixer device + spec is open. Idempotent. */
static int mixer_ensure_open(void)
{
    if (s_mix_dev) return 1;
    SDL_AudioSpec want;
    SDL_memset(&want, 0, sizeof want);
    want.freq     = MIX_OUT_FREQ;
    want.format   = MIX_OUT_FORMAT;
    want.channels = MIX_OUT_CHANS;
    want.samples  = 2048;
    want.callback = mixer_callback;
    want.userdata = NULL;
    s_mix_dev = SDL_OpenAudioDevice(NULL, 0, &want, &s_mix_spec, 0);
    if (!s_mix_dev) {
        fprintf(stderr, "[mixer] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return 0;
    }
    SDL_PauseAudioDevice(s_mix_dev, 0);   /* unpause */
    fprintf(stderr, "[mixer] open: %d Hz, %d ch, %d-bit (8 channels)\n",
            s_mix_spec.freq, s_mix_spec.channels,
            SDL_AUDIO_BITSIZE(s_mix_spec.format));
    return 1;
}

/* Load a WAV (from filesystem or DTA) and convert to output spec.
 * Returns 1 on success with out_buf/out_len owned by caller (must SDL_free
 * after audio callback no longer references it). */
static int mixer_load_wav(const char *name, Uint8 **out_buf, Uint32 *out_len);

/* Lock device, assign to channel `idx`, unlock, unpause. Frees previous
 * buf if any. */
static void mixer_assign(int idx, Uint8 *buf, Uint32 len, int loop,
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
    /* Default to identity gain (no pan). Callers can override via
 * mixer_set_pan after the channel is loaded. */
    s_mix[idx].gain_l = 128;
    s_mix[idx].gain_r = 128;
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

static void mixer_stop_channel(int idx)
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
 * Menu / background WAV music — now backed by mixer channel MIX_CHAN_MUSIC.
 *
 * → (&g_persp_band_count,
 * BuildAssetPath("Dane_01.dta", NULL), 1) → (handle).
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
static int mixer_load_wav(const char *name, Uint8 **out_buf, Uint32 *out_len)
{
    SDL_AudioSpec native;
    Uint8 *native_buf = NULL;
    Uint32 native_len = 0;

    int ok = 0;
    if (!ok) ok = try_load_wav_at(NULL,      name, &native, &native_buf, &native_len);
    if (!ok) ok = try_load_wav_at(g_cd_path, name, &native, &native_buf, &native_len);
    if (!ok) ok = try_load_wav_at("./data",  name, &native, &native_buf, &native_len);
    if (!ok) ok = try_load_wav_from_dta(name, &native, &native_buf, &native_len);
    if (!ok) {
        *out_buf = NULL; *out_len = 0;
        return 0;
    }

    /* Already in target format? Just transfer ownership. */
    if (native.freq == MIX_OUT_FREQ &&
        native.format == MIX_OUT_FORMAT &&
        native.channels == MIX_OUT_CHANS) {
        /* Take a freshly malloc'd copy so caller can SDL_free it (WAV
 * buffer must use SDL_FreeWAV; can't mix). */
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
        fprintf(stderr, "[mixer] SDL_BuildAudioCVT failed for %s: %s\n",
                name, SDL_GetError());
        SDL_FreeWAV(native_buf);
        return 0;
    }
    /* CVT needs a buffer sized native_len * cvt.len_mult. Allocate fresh
 * via SDL_malloc (so SDL_free can release it), copy native into it. */
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
        fprintf(stderr, "[mixer] SDL_ConvertAudio failed for %s: %s\n",
                name, SDL_GetError());
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
/* T103 fix — DAT_004551xx mapping per Ghidra plate comment on
 * LoadSaveStateOrInitialize @ 0x0040A5C0:
 * speech_y_offset — sfx (sound effects) ← g_audio_sfx_enabled
 * speech_text_attr — music ← g_audio_music_enabled
 * speech_color_index — (semantic under RE) — kept as "sound_enabled"
 * = legacy master mute
 * fade_target — voice ← g_audio_voice_enabled (NEW)
 *
 * `g_audio_sound_enabled` is legacy from earlier port; functions as a
 * master mute (was mis-named "sfx" via fade_progress — that's actually
 * dialogues_on). Kept for compatibility with existing call sites. */
int      g_audio_music_enabled = 1;       /* speech_text_attr mirror */
int      g_audio_sfx_enabled   = 1;       /* speech_y_offset mirror — sfx */
int      g_audio_voice_enabled = 1;       /* fade_target mirror — dialog audio */
int      g_audio_sound_enabled = 1;       /* speech_color_index mirror (legacy global) */
static char  s_last_music_name[64] = "";
static int   s_last_music_loop    = 0;

void StopMenuMusic(void)
{
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
    /* Remember the last requested track so AudioSetMusicEnabled(1) can
 * resume play after a mute-toggle. */
    snprintf(s_last_music_name, sizeof s_last_music_name, "%s", dta_name);
    s_last_music_loop = loop ? 1 : 0;

    if (!g_audio_music_enabled || !g_audio_sound_enabled) {
        StopMenuMusic();
        return;
    }
    if (!mixer_ensure_open()) return;
    StopMenuMusic();

    Uint8 *buf = NULL; Uint32 len = 0;
    if (!mixer_load_wav(dta_name, &buf, &len)) {
        fprintf(stderr, "[music] cannot find/decode %s as WAV\n", dta_name);
        return;
    }
    mixer_assign(MIX_CHAN_MUSIC, buf, len, loop ? 1 : 0, dta_name);
    fprintf(stderr, "[music] %s playing: %u bytes converted (loop=%d) on mixer ch %d\n",
            dta_name, len, loop ? 1 : 0, MIX_CHAN_MUSIC);
}

/* Toggle hook — called by Solund handler ( port) when the
 * user clicks the music on/off button in the options menu. If muting
 * mid-play, stops the channel. If un-muting, re-issues PlayMenuMusic
 * with the last remembered track to resume. */
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
    /* Mixer callback handles loop natively — no per-frame top-up needed.
 * Function kept as no-op for API compat (called from play_demo_scene
 * frame loop). */
}

/* ------------------------------------------------------------------------- *
 * PlaySfx — fire-and-forget WAV playback channel.
 *
 * Used by the per-frame sound-trigger system: every per-entity script
 * call to op 0x12 ADVANCE_FRAME consults the asset's [sampl] table
 * (parsed from Wacky.scr) for the just-entered frame; if a match is
 * found, the sound name fires here. Plays in parallel with the room
 * music (separate SDL audio device). Auto-closes when the queue
 * drains.
 *
 * (T6 refactor: SFX now share the mixer channel pool. Legacy per-slot
 * SDL device + WAV-format-per-slot logic removed — see mixer_assign +
 * MIX_CHAN_SFX_START at top of file.) */

void PlaySfx(const char *wav_name);   /* fwd decl */

/* ------------------------------------------------------------------------- *
 * Per-asset frame-trigger sound table —scr `[sampl]` tags.
 *
 * The original engine stores these per asset by parsing Wacky.scr at
 * LoadAssetFromDtaBase time ( → FindAnimationScript) and
 * then per ADVANCE_FRAME the walker calls → 
 * which looks up the current frame and plays the matching WAV via
 * DirectSound.
 *
 * Our port doesn't yet parse `[sampl]` text — hard-code the stage-1
 * triggers extracted from `/tmp/dta-list/wacky.scr`. If the asset
 * lookup hits, we PlaySfx that wav. Each entry: (asset_basename,
 * frame_index, wav_filename). Multiple entries per asset allowed.
 *
 * Source: wacky.scr lines 27-49 (stage-1 maluch.pic [komnata]). */
/* frame_end: 0xFFFF for `(N,)` (single-shot trigger, no looping); a real
 * frame index for `(N,M)` (loop wav from frame_start, stop at frame_end).
 * FrameSfxEntry (start, end) ushort pair stored by
 * — at parse, M-absent becomes sVar11=-1=0xFFFF. */
struct FrameSfxEntry { const char *asset; int frame_start; int frame_end; const char *wav; };

/* Dynamic runtime table populated by ParseSamplTagsForKomnata from the
 * current [komnata]N section of Wacky.scr. Replaces the hand-transcribed
 * table from earlier port — proper
 * tag parser. Each entry: {asset (heap str), frame, wav (heap str)}. */
#define DYNAMIC_SFX_MAX 256
static struct FrameSfxEntry g_dynamic_sfx[DYNAMIC_SFX_MAX];
static int  g_dynamic_sfx_count = 0;
static char g_dynamic_sfx_strpool[8192];
static int  g_dynamic_sfx_strpool_used = 0;

static const char *strpool_intern(const char *s, size_t n)
{
    /* Dedup — return existing entry if matched (= literal pointer
 * equality for stable TriggerFrameSfx debounce keying). */
    int i = 0;
    while (i < g_dynamic_sfx_strpool_used) {
        const char *cur = g_dynamic_sfx_strpool + i;
        size_t cur_n = strlen(cur);
        if (cur_n == n && memcmp(cur, s, n) == 0) return cur;
        i += (int)cur_n + 1;
    }
    if (g_dynamic_sfx_strpool_used + (int)n + 1 > (int)sizeof g_dynamic_sfx_strpool)
        return NULL;
    char *dst = g_dynamic_sfx_strpool + g_dynamic_sfx_strpool_used;
    memcpy(dst, s, n);
    dst[n] = 0;
    g_dynamic_sfx_strpool_used += (int)n + 1;
    return dst;
}

void ResetDynamicSfxTable(void)
{
    g_dynamic_sfx_count = 0;
    g_dynamic_sfx_strpool_used = 0;
}

/* ParseSamplTagsForKomnata — port of 's [sampl] tag parser.
 *
 * Walks a buffer between `start` and `end` (current Wacky.scr [komnata]
 * section per FindScriptByStageAndRoom). Tracks current [animacja] asset
 * name; for each [sampl] line, parses `WAV1 WAV2 .. (F1,M1) (F2,M2) ...`
 * and registers (asset, frame, wav) triples in g_dynamic_sfx[].
 *
 * Token rules:
 * - "[animacja] NAME.wyc" sets current asset (case-preserved, NUL-term).
 * - "[sampl] WAV1 WAV2 ... (F,)" / "(F,M)" — register each WAV at each F.
 * - end of section = stop. End of file = stop.
 *
 *: per-frame random-pool semantics via multiple
 * entries with same (asset, frame). The M (end frame) in (N,M) is
 * IGNORED (our TriggerFrameSfx doesn't honor "stop at M" — the WAV
 * plays naturally). */
/* Lenient `[keyword]` matcher — tolerates the `[ keyword]` and
 * `[keyword ]` typos found 13× in shipped Wacky.scr (e.g. `[ sampl]
 * CamKlik2.wav (31,)` in the foto3.pic camera animation). On success
 * advances *pp past the closing `]`, returns 1; otherwise leaves *pp
 * unchanged and returns 0. */
static int match_bracket_tag(const uint8_t **pp, const uint8_t *end,
                             const char *keyword)
{
    const uint8_t *p = *pp;
    if (p >= end || *p != '[') return 0;
    ++p;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    size_t klen = strlen(keyword);
    if (p + klen > end) return 0;
    if (memcmp(p, keyword, klen) != 0) return 0;
    p += klen;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    if (p >= end || *p != ']') return 0;
    ++p;
    *pp = p;
    return 1;
}

void ParseSamplTagsForKomnata(const uint8_t *start, const uint8_t *end)
{
    if (!start || !end || start >= end) return;
    const uint8_t *p = start;
    char cur_asset[32] = {0};
    int  cur_asset_set = 0;
    int  added = 0;

    while (p < end) {
        /* Skip whitespace + comments. */
        while (p < end && (*p == ' ' || *p == '\t' ||
                           *p == '\r' || *p == '\n')) ++p;
        if (p >= end) break;
        /* Stop at next major section. */
        if (p + 9 <= end && memcmp(p, "[komnata]", 9) == 0) break;

        /* Look for [animacja] tag (tolerant of `[ animacja]` typos). */
        if (match_bracket_tag(&p, end, "animacja")) {
            while (p < end && (*p == ' ' || *p == '\t')) ++p;
            const uint8_t *name_start = p;
            while (p < end && *p != ' ' && *p != '\t' &&
                   *p != '\r' && *p != '\n') ++p;
            size_t n = (size_t)(p - name_start);
            if (n >= sizeof cur_asset) n = sizeof cur_asset - 1;
            memcpy(cur_asset, name_start, n);
            cur_asset[n] = 0;
            /* Normalise to lowercase (Wacky.scr uses mixed case but
 * AnimAsset.name from LoadAssetFromDtaBase is lowercased). */
            for (size_t i = 0; i < n; ++i)
                if (cur_asset[i] >= 'A' && cur_asset[i] <= 'Z')
                    cur_asset[i] = (char)(cur_asset[i] + 32);
            cur_asset_set = 1;
            continue;
        }

        /* Look for [sampl] tag (tolerant of `[ sampl]` typos, must
 * follow an [animacja]). */
        if (cur_asset_set && match_bracket_tag(&p, end, "sampl")) {
            /* Parse rest of line. Collect tokens until '(' (= start
 * of frame trigger list), then per (N,M) tuple register
 * each WAV at frame N. */
            const char *wavs[8];
            char wav_buf[8][32];
            int wav_count = 0;
            while (p < end && *p != '(' && *p != '\r' && *p != '\n') {
                while (p < end && (*p == ' ' || *p == '\t')) ++p;
                if (p >= end || *p == '(' || *p == '\r' || *p == '\n') break;
                const uint8_t *tok_start = p;
                while (p < end && *p != ' ' && *p != '\t' &&
                       *p != '(' && *p != '\r' && *p != '\n') ++p;
                size_t tn = (size_t)(p - tok_start);
                if (tn == 0) break;
                if (wav_count < 8) {
                    if (tn >= sizeof wav_buf[0]) tn = sizeof wav_buf[0] - 1;
                    memcpy(wav_buf[wav_count], tok_start, tn);
                    wav_buf[wav_count][tn] = 0;
                    /* Normalise lowercase. */
                    for (size_t i = 0; i < tn; ++i)
                        if (wav_buf[wav_count][i] >= 'A' &&
                            wav_buf[wav_count][i] <= 'Z')
                            wav_buf[wav_count][i] = (char)(wav_buf[wav_count][i] + 32);
                    wavs[wav_count] = wav_buf[wav_count];
                    ++wav_count;
                }
            }
            /* Parse (N,M) tuples number parser:
 * digits → N (start). Comma then digits → M (end). Missing
 * digits → -1 (= 0xFFFF for ushort). actually
 * REQUIRES the comma to add the entry at all (no comma →
 * skip), but shipped Wacky.scr never uses `(N)` so we keep
 * the original-equivalent path. */
            while (p < end && *p == '(') {
                ++p;
                while (p < end && (*p == ' ' || *p == '\t')) ++p;
                int n_start = -1;
                {
                    const uint8_t *digit0 = p;
                    int n = 0;
                    while (p < end && *p >= '0' && *p <= '9') {
                        n = n * 10 + (*p - '0'); ++p;
                    }
                    if (p > digit0) n_start = n;
                }
                int n_end = -1;
                while (p < end && (*p == ' ' || *p == '\t')) ++p;
                if (p < end && *p == ',') {
                    ++p;
                    while (p < end && (*p == ' ' || *p == '\t')) ++p;
                    const uint8_t *digit0 = p;
                    int n = 0;
                    while (p < end && *p >= '0' && *p <= '9') {
                        n = n * 10 + (*p - '0'); ++p;
                    }
                    if (p > digit0) n_end = n;
                }
                while (p < end && *p != ')' && *p != '\n') ++p;
                if (p < end && *p == ')') ++p;
                if (n_start < 0) {
                    while (p < end && (*p == ' ' || *p == '\t')) ++p;
                    continue;
                }
                int frame_end = (n_end < 0) ? 0xFFFF : n_end;
                /* Register each WAV at frame n_start. */
                const char *asset_interned = strpool_intern(cur_asset, strlen(cur_asset));
                if (asset_interned) {
                    for (int wi = 0; wi < wav_count; ++wi) {
                        if (g_dynamic_sfx_count >= DYNAMIC_SFX_MAX) break;
                        const char *wav_interned =
                            strpool_intern(wavs[wi], strlen(wavs[wi]));
                        if (!wav_interned) continue;
                        g_dynamic_sfx[g_dynamic_sfx_count++] =
                            (struct FrameSfxEntry){asset_interned, n_start, frame_end, wav_interned};
                        ++added;
                    }
                }
                while (p < end && (*p == ' ' || *p == '\t')) ++p;
            }
            continue;
        }

        /* Skip to next newline. */
        while (p < end && *p != '\n') ++p;
    }
    fprintf(stderr, "[sampl] parser: %d entries from this [komnata]\n", added);
}

/* Static fallback table REMOVED (B4 from REVIEW-2026-05) — the D7
 * `Wacky.scr [sampl]` parser (ParseSamplTagsForKomnata, called from
 * LoadKomnata @ ) populates g_dynamic_sfx[] with every
 * (asset, frame, wav) triple needed by the current room. The only
 * code path that triggers per-frame SFX is the per-entity VM tick
 * which can't fire before LoadKomnata has spawned any entities — so
 * the dynamic table is always populated before TriggerFrameSfx fires.
 * No safety net needed. */

/*
 * (t->wav_state[t->cur_wav].hash); // stop
 * t->wav_state[t->cur_wav].playing_flag = 0;
 * // start_frame matched
 * (t->wav_state[t->cur_wav].hash);
 * t->wav_state[t->cur_wav].playing_flag = 0;
 * SampleEntry *w = &t->wav_state[random_idx];
 *
 * Key invariant: the play guard at 0x40A292 is `byte+3 == 0 ||
 * byte+2 == 0`. byte+2 (playing_flag) is sticky-on-play, cleared
 * only by an end-frame trigger or pool swap. byte+3 (channel /
 * "currently-playing") is set when DSound starts the buffer and
 * cleared when it drains. Net effect: a wav RETRIGGERS at the same
 * frame iff its previous instance has finished (drained channel)
 * or a different pool member is picked. Earlier port mis-read this
 * as "byte+2 alone gates retrigger" → single-pool (N,) entries (the
 * majority — slimak.wyc, footsteps, ambient loops) fired exactly
 * once per scene load.
 *
 * Our port doesn't have the original's nested SampleTable list. We
 * track state per (asset, frame, wav) tuple in g_sfx_state[]. The
 * `channel` field mirrors byte+3 by storing the mixer channel a
 * wav was last assigned to; "drained" is detected via
 * s_mix[channel].active flipping to 0 in mixer_callback. */

#define SFX_STATE_MAX 256
struct SfxState {
    const char *asset;        /* keyed by table entry pointer identity */
    int         frame;
    const char *wav;
    uint8_t     playing_flag; /* mirror of wav_state[].playing_flag */
    int8_t      channel;      /* mixer channel this WAV is currently on,
 * or -1 if free / never played. Used to
 * stop in flight when cur_wav swaps to a
 * different pool entry —
 * original's stop call. */
};
static struct SfxState g_sfx_state[SFX_STATE_MAX];
static int             g_sfx_state_count = 0;
static int             g_sfx_cur_wav     = -1;  /* mirror of t->cur_wav */
static const char     *g_sfx_cur_asset   = NULL;

/* Reset all SFX state. Mirrors what the original does on scene transition
 * (+ ): every asset on the komnata is freed →
 * stops every wav in each asset's SampleTable.
 *
 * Three responsibilities:
 * 1. Stop any still-playing mixer channels that g_sfx_state[] points to
 * — looping (N,M) wavs whose end-frame the previous komnata never
 * ticked (e.g. player leaves the room mid-rakieta) would otherwise
 * keep mixing into the next komnata's audio.
 * 2. Zero the slot array. Without this the (asset, wav) pointers in
 * g_sfx_state[] become dangling — ResetDynamicSfxTable is called
 * right after us in LoadKomnata and clears g_dynamic_sfx_strpool,
 * so any retained pointer is unsafe to dereference.
 * 3. Reset the global cur_wav/cur_asset mirror of 's
 * per-table fields (port shortcut). */
void ResetFrameSfxState(void)
{
    for (int i = 0; i < g_sfx_state_count; ++i) {
        struct SfxState *st = &g_sfx_state[i];
        if (st->channel >= MIX_CHAN_SFX_START &&
            st->channel < MIX_CHANNEL_COUNT && st->wav) {
            SDL_LockAudioDevice(s_mix_dev);
            int ours = s_mix[st->channel].active &&
                       strcmp(s_mix[st->channel].name, st->wav) == 0;
            SDL_UnlockAudioDevice(s_mix_dev);
            if (ours) {
                fprintf(stderr, "[sfx] stop '%s' (asset=%s ch=%d) — komnata reset\n",
                        st->wav, st->asset ? st->asset : "(null)", st->channel);
                mixer_stop_channel(st->channel);
            }
        }
    }
    g_sfx_state_count = 0;
    g_sfx_cur_wav     = -1;
    g_sfx_cur_asset   = NULL;
}

/* Get-or-create state slot keyed by (asset_name, frame, wav). Asset names
 * arrive from two paths with different pointers — TriggerFrameSfx passes
 * `atlas->name` (a buffer inside AnimAsset), while sfx_handle_end_frames
 * and StopAllSfxForAsset pass interned pointers from g_dynamic_sfx_strpool.
 * Both strings compare equal but the pointers don't. Use strcmp on asset
 * to bind both paths to the same slot — otherwise the lookup creates a
 * fresh slot with channel=-1 and the mixer_stop_channel call no-ops.
 *
 * `wav` is always interned (both paths derive it from g_dynamic_sfx[].wav)
 * so pointer-equality is fine there. */
static struct SfxState *sfx_state_for(const char *asset, int frame, const char *wav)
{
    for (int i = 0; i < g_sfx_state_count; ++i) {
        if (g_sfx_state[i].frame == frame &&
            g_sfx_state[i].wav   == wav &&
            g_sfx_state[i].asset && asset &&
            strcmp(g_sfx_state[i].asset, asset) == 0)
            return &g_sfx_state[i];
    }
    if (g_sfx_state_count >= SFX_STATE_MAX) return NULL;
    g_sfx_state[g_sfx_state_count] = (struct SfxState){asset, frame, wav, 0, -1};
    return &g_sfx_state[g_sfx_state_count++];
}

extern uint32_t g_tick_counter;

/* Pass 1 — end-frame handling. inner loop @ 0x40A23E:
 * if (entry.end == frame) { STOP cur_wav; flag = 0; cur_wav = -1; return; }
 * `(N,M)` entries in Wacky.scr establish a (start=N, stop=M) range where
 * the wav loops between N and M. Hitting M stops the loop. Without this,
 * marsz.wav / rakiet1a.wav play once and fall silent halfway through
 * the rakieta.wyc animation (frames 1..464 needing ~30s of audio from
 * a 10s wav). */
/* (SampleTable destructor, called from
 * when an animation asset is freed): iterates the wav
 * pool and calls to stop each currently-playing wav.
 *
 * Our port mirror: stop every currently-playing SfxState whose asset
 * matches, and zero its flag. Called when an asset slot is overwritten
 * (ScriptCallLoadAsset) — the per-entity VM may not tick frame_end
 * for looping (N,M) entries if the bytecode skips frames or the
 * script switches assets mid-animation. */
void StopAllSfxForAsset(const char *asset_name)
{
    if (!asset_name) return;
    for (int i = 0; i < g_sfx_state_count; ++i) {
        struct SfxState *st = &g_sfx_state[i];
        if (!st->asset || strcmp(st->asset, asset_name) != 0) continue;
        if (st->channel >= MIX_CHAN_SFX_START &&
            st->channel < MIX_CHANNEL_COUNT) {
            /* Only stop if this channel is still playing OUR wav —
 * the slot may have been stolen for another sound. */
            SDL_LockAudioDevice(s_mix_dev);
            int ours = s_mix[st->channel].active &&
                       strcmp(s_mix[st->channel].name, st->wav) == 0;
            SDL_UnlockAudioDevice(s_mix_dev);
            if (ours) {
                fprintf(stderr, "[sfx] stop '%s' (asset=%s ch=%d) — asset reload\n",
                        st->wav, asset_name, st->channel);
                mixer_stop_channel(st->channel);
            }
        }
        st->channel = -1;
        st->playing_flag = 0;
    }
}

static void sfx_handle_end_frames(const char *asset_name, int frame)
{
    for (int i = 0; i < g_dynamic_sfx_count; ++i) {
        struct FrameSfxEntry *e = &g_dynamic_sfx[i];
        if (e->frame_end == 0xFFFF || e->frame_end != frame) continue;
        if (strcmp(e->asset, asset_name) != 0) continue;
        struct SfxState *st = sfx_state_for(e->asset, e->frame_start, e->wav);
        if (!st) continue;
        fprintf(stderr, "[sfx] stop '%s' at end-frame %d (asset=%s start=%d ch=%d)\n",
                e->wav, frame, asset_name, e->frame_start, st->channel);
        if (st->channel >= MIX_CHAN_SFX_START &&
            st->channel < MIX_CHANNEL_COUNT) {
            mixer_stop_channel(st->channel);
        }
        st->channel = -1;
        st->playing_flag = 0;
    }
}

void TriggerFrameSfx(const char *asset_name, int frame)
{
    if (!asset_name) return;

    /* Pass 1: stop any (N,M) loops whose M matches this frame. */
    sfx_handle_end_frames(asset_name, frame);

    /* Pass 2: collect random pool for (asset, start=frame).
 * `[sampl] WAV1 WAV2 .. (F,)` semantics: at frame F, pick one of
 * WAV1..WAVk at random. Also track frame_end of the picked entry
 * → if non-0xFFFF we play with loop=1 so the wav loops until
 * sfx_handle_end_frames stops it. */
    const char *pool[8];
    int pool_end[8];
    int n = 0;
    for (int i = 0; i < g_dynamic_sfx_count && n < 8; ++i) {
        if (g_dynamic_sfx[i].frame_start == frame &&
            strcmp(g_dynamic_sfx[i].asset, asset_name) == 0) {
            pool[n]     = g_dynamic_sfx[i].wav;
            pool_end[n] = g_dynamic_sfx[i].frame_end;
            ++n;
        }
    }
    if (n == 0) return;

    /*(count) — uniform random in [0, count). */
    int random_idx = (int)WackiRand((uint16_t)n);
    const char *wav = pool[random_idx];

    /*:
 * if a different wav from the pool is currently "playing" for
 * this asset, clear its flag AND stop the mixer channel it's on
 * ('s stop call). */
    if (g_sfx_cur_asset == asset_name && g_sfx_cur_wav != random_idx) {
        if (g_sfx_cur_wav >= 0 && g_sfx_cur_wav < n) {
            struct SfxState *prev = sfx_state_for(asset_name, frame, pool[g_sfx_cur_wav]);
            if (prev) {
                prev->playing_flag = 0;
                if (prev->channel >= 0) {
                    mixer_stop_channel(prev->channel);
                    prev->channel = -1;
                }
            }
        }
    }
    g_sfx_cur_asset = asset_name;
    g_sfx_cur_wav   = random_idx;

    /* Play guard —:
 * MOV CL, [wav_state[i] + 0x3] ; "currently-playing" indicator
 * TEST CL, CL ; JZ → play
 * MOV CL, [wav_state[i] + 0x2] ; sticky play_flag
 * TEST CL, CL ; JNZ → skip; else play
 *
 * Decision: play iff `byte+3 == 0 || byte+2 == 0` — i.e. SKIP only
 * when the wav is BOTH currently still playing AND has fired at
 * least once. The sticky `byte+2` alone does NOT block replay;
 * it just records that we've started this trigger before.
 *
 * In the original, `byte+3` is set by (DSound start)
 * and cleared by DSound notification when the buffer drains.
 * Our port mirror: `byte+3 != 0` ≡ "the same wav is still actively
 * playing on the mixer channel we last assigned it to". After the
 * SFX naturally ends (mixer callback flips `ch->active = 0`), the
 * condition flips and the next frame-trigger replays. */
    struct SfxState *st = sfx_state_for(asset_name, frame, wav);
    if (!st) return;
    int currently_playing = 0;
    if (st->channel >= MIX_CHAN_SFX_START && st->channel < MIX_CHANNEL_COUNT) {
        SDL_LockAudioDevice(s_mix_dev);
        if (s_mix[st->channel].active &&
            strcmp(s_mix[st->channel].name, wav) == 0) {
            currently_playing = 1;
        }
        SDL_UnlockAudioDevice(s_mix_dev);
    }
    if (st->playing_flag && currently_playing) return;
    st->playing_flag = 1;
    /* PlaySfxLoopAndGetChannel returns the channel index so we can
 * (a) stop-in-flight when cur_wav swaps and
 * (b) detect drain via s_mix[channel].active above.
 * loop=1 for (N,M) entries — the wav loops until the M-frame
 * trigger calls sfx_handle_end_frames → mixer_stop_channel. */
    int want_loop = (pool_end[random_idx] != 0xFFFF);
    extern int PlaySfxLoopAndGetChannel(const char *wav_name, int loop);
    st->channel = (int8_t)PlaySfxLoopAndGetChannel(wav, want_loop);
}

/* PlaySfx — fire-and-forget WAV via mixer SFX channel pool.
 * Picks free channel from [MIX_CHAN_SFX_START .. MIX_CHANNEL_COUNT) or
 * steals oldest if all busy. Auto-frees converted buffer when callback
 * drains the channel (TickSfx). */
extern void PlaySfxPanned(const char *wav_name, uint8_t gain_l, uint8_t gain_r);
int PlaySfxPannedAndGetChannel(const char *wav_name,
                               uint8_t gain_l, uint8_t gain_r);   /* fwd */

void PlaySfx(const char *wav_name)
{
    PlaySfxPanned(wav_name, 128, 128);
}

/* T36 — PlaySfxPanned: same as PlaySfx with explicit L/R gain.
 * Caller computes gains from positional source vs listener via
 * SoundQueueMixForListener (which returns packed 0xRRCCLL — split
 * the C contribution equally into L+R for our 2-channel mixer). */
void PlaySfxPanned(const char *wav_name, uint8_t gain_l, uint8_t gain_r)
{
    (void)PlaySfxPannedAndGetChannel(wav_name, gain_l, gain_r);
}

/* Internal — returns the mixer channel the WAV landed on (or -1 if it
 * couldn't be loaded / mixer not open). Used by TriggerFrameSfx so it
 * can stop the previous WAV in flight when cur_wav swaps. */
int PlaySfxPannedAndGetChannel(const char *wav_name,
                               uint8_t gain_l, uint8_t gain_r)
{
    if (!wav_name) return -1;
    if (!g_audio_sfx_enabled || !g_audio_sound_enabled) return -1;
    if (!mixer_ensure_open()) return -1;

    extern uint32_t g_tick_counter;
    int slot = -1;
    uint32_t oldest = 0xFFFFFFFFu;
    int oldest_slot = MIX_CHAN_SFX_START;
    for (int i = MIX_CHAN_SFX_START; i < MIX_CHANNEL_COUNT; ++i) {
        if (!s_mix[i].active) { slot = i; break; }
        if (s_mix[i].start_tick < oldest) {
            oldest = s_mix[i].start_tick;
            oldest_slot = i;
        }
    }
    if (slot < 0) slot = oldest_slot;

    Uint8 *buf = NULL; Uint32 len = 0;
    if (!mixer_load_wav(wav_name, &buf, &len)) {
        fprintf(stderr, "[sfx] cannot load '%s'\n", wav_name);
        return -1;
    }
    mixer_assign(slot, buf, len, 0, wav_name);
    /* mixer_assign sets gain to 128/128 (identity); override here for
 * positional pan. Lock once for atomic L+R update. */
    SDL_LockAudioDevice(s_mix_dev);
    s_mix[slot].gain_l = gain_l;
    s_mix[slot].gain_r = gain_r;
    SDL_UnlockAudioDevice(s_mix_dev);
    fprintf(stderr, "[sfx] play '%s' on mixer ch %d (%u bytes, gain L=%u R=%u)\n",
            wav_name, slot, len, gain_l, gain_r);
    return slot;
}

int PlaySfxAndGetChannel(const char *wav_name)
{
    return PlaySfxPannedAndGetChannel(wav_name, 128, 128);
}

/* Looping variant — same as PlaySfxAndGetChannel but assigns the mixer
 * channel with `loop=1` so the audio callback wraps src_frame back to 0
 * instead of marking the channel inactive. Used by TriggerFrameSfx for
 * `[sampl] WAV (N,M)` entries — caller must stop the channel at frame
 * M via sfx_handle_end_frames. */
int PlaySfxLoopAndGetChannel(const char *wav_name, int loop)
{
    int ch = PlaySfxPannedAndGetChannel(wav_name, 128, 128);
    if (ch >= 0 && loop) {
        SDL_LockAudioDevice(s_mix_dev);
        s_mix[ch].loop = 1;
        SDL_UnlockAudioDevice(s_mix_dev);
    }
    return ch;
}

/* PlayDialogLine — T6: dedicated dialog speech channel.
 * Uses MIX_CHAN_DIALOG (reserved). Returns the byte length of the
 * loaded audio (so caller can compute approximate duration) or 0 on
 * failure. */
uint32_t PlayDialogLine(const char *wav_name)
{
    if (!wav_name) return 0;
    /* T103 — voice gate: if user disabled voice in Solund, drop the
 * sample silently. Caller still proceeds with text/animation. */
    if (!g_audio_voice_enabled || !g_audio_sound_enabled) return 0;
    if (!mixer_ensure_open()) return 0;
    Uint8 *buf = NULL; Uint32 len = 0;
    if (!mixer_load_wav(wav_name, &buf, &len)) {
        fprintf(stderr, "[dialog] cannot load '%s'\n", wav_name);
        return 0;
    }
    mixer_assign(MIX_CHAN_DIALOG, buf, len, 0, wav_name);
    fprintf(stderr, "[dialog] play '%s' on mixer ch %d (%u bytes)\n",
            wav_name, MIX_CHAN_DIALOG, len);
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
