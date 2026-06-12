/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/audio/sfx.c — frame-trigger SFX dispatch + Wacky.scr [sampl] parser.
 *
 * Drives the per-frame sound-trigger system. Every per-entity script
 * call to op 0x12 ADVANCE_FRAME hits the asset's [sampl] table (parsed
 * from Wacky.scr by ParseSamplTagsForKomnata at LoadKomnata time);
 * if a match is found, TriggerFrameSfx fires the WAV through the
 * shared mixer (channels MIX_CHAN_SFX_START..MIX_CHANNEL_COUNT).
 *
 * Per-(asset, frame, wav) state lives in g_sfx_state[], mirroring the
 * original engine's SampleTable layout (playing_flag + currently-
 * playing-channel). The replay guard at the bottom of TriggerFrameSfx
 * skips re-firing a WAV that's both sticky-playing AND still active
 * on its mixer channel — matching the original's
 * `byte+2 == 0 || byte+3 == 0` semantics (see the invariant note above
 * TriggerFrameSfx).
 *
 * PlaySfx / PlaySfxPanned / PlaySfxAndGetChannel /
 * PlaySfxLoopAndGetChannel are the fire-and-forget public API used by
 * the SoundQueue (sound_queue.c) and other audio callers.
 */

#include "wacki.h"
#include "wacki/log.h"
#include "mixer_internal.h"

#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int      g_audio_sfx_enabled;
extern int      g_audio_sound_enabled;
extern uint32_t g_tick_counter;

void            PlaySfx(const char *wav_name);   /* fwd */
extern int      PlaySfxLoopAndGetChannel(const char *wav_name, int loop);
extern void     PlaySfxPanned(const char *wav_name,
                              uint8_t gain_l, uint8_t gain_r);
int             PlaySfxPannedAndGetChannel(const char *wav_name,
                                           uint8_t gain_l, uint8_t gain_r);

/* ======================================================================== *
 * Per-asset frame-trigger sound table — Wacky.scr `[sampl]` tags.
 *
 * The original engine stores these per asset by parsing Wacky.scr at
 * LoadAssetFromDtaBase time and then, per ADVANCE_FRAME, the walker
 * looks up the current frame and plays the matching WAV via
 * DirectSound. Our port doesn't store [sampl] tables per-asset; we
 * instead parse the current [komnata] section into a flat
 * g_dynamic_sfx[] table at LoadKomnata time and key TriggerFrameSfx
 * lookups by `(asset_name, frame)`.
 *
 * frame_end semantics:
 *   0xFFFF (= -1) → "(N,)" single-shot, no looping
 *   real index   → "(N,M)" loop WAV from frame_start; stop at end-frame
 * ====================================================================== */

#define DYNAMIC_SFX_MAX             256
#define DYNAMIC_SFX_STRPOOL_BYTES   8192
#define DYNAMIC_SFX_ASSET_NAME_MAX  32
#define DYNAMIC_SFX_WAVS_PER_SAMPL  8

struct FrameSfxEntry {
    const char *asset;
    int         frame_start;
    int         frame_end;
    const char *wav;
};

static struct FrameSfxEntry g_dynamic_sfx[DYNAMIC_SFX_MAX];
static int                  g_dynamic_sfx_count = 0;
static char                 g_dynamic_sfx_strpool[DYNAMIC_SFX_STRPOOL_BYTES];
static int                  g_dynamic_sfx_strpool_used = 0;

/* Intern a string into the per-komnata pool. Dedups so equal strings
 * compare pointer-equal — this matters for the TriggerFrameSfx debounce
 * key, which is keyed by interned wav pointer identity. */
static const char *strpool_intern(const char *s, size_t n)
{
    int i = 0;
    while (i < g_dynamic_sfx_strpool_used) {
        const char *cur = g_dynamic_sfx_strpool + i;
        size_t cur_n = strlen(cur);
        if (cur_n == n && memcmp(cur, s, n) == 0) return cur;
        i += (int)cur_n + 1;
    }
    if (g_dynamic_sfx_strpool_used + (int)n + 1 >
        (int)sizeof g_dynamic_sfx_strpool)
        return NULL;
    char *dst = g_dynamic_sfx_strpool + g_dynamic_sfx_strpool_used;
    memcpy(dst, s, n);
    dst[n] = 0;
    g_dynamic_sfx_strpool_used += (int)n + 1;
    return dst;
}

void ResetDynamicSfxTable(void)
{
    g_dynamic_sfx_count        = 0;
    g_dynamic_sfx_strpool_used = 0;
}

/* Lenient `[keyword]` matcher — tolerates the `[ keyword]` and
 * `[keyword ]` typos found 13× in shipped Wacky.scr (e.g.
 * `[ sampl] CamKlik2.wav (31,)` in the foto3.pic camera animation).
 * On success advances *pp past the closing `]` and returns 1;
 * otherwise leaves *pp unchanged and returns 0. */
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

/* ParseSamplTagsForKomnata — Wacky.scr [sampl] tag parser.
 *
 * Walks a buffer between `start` and `end` (current Wacky.scr
 * [komnata] section per FindScriptByStageAndRoom). Tracks the
 * current [animacja] asset name; for each [sampl] line, parses
 * `WAV1 WAV2 .. (F1,M1) (F2,M2) ...` and registers (asset, frame,
 * wav) triples in g_dynamic_sfx[].
 *
 * Token rules:
 *   "[animacja] NAME.wyc"        — set current asset (lowercased)
 *   "[sampl] WAV1 WAV2 ... (F,)" — register each WAV at frame F
 *   "(F,M)"                      — same, with end-frame M for looping
 *   end of section / file        — stop
 *
 * The original encodes a random pool via multiple entries with the
 * same (asset, frame). The M end-frame in (N,M) is captured (used to
 * stop looping WAVs at the right tick).
 */
void ParseSamplTagsForKomnata(const uint8_t *start, const uint8_t *end)
{
    if (!start || !end || start >= end) return;
    const uint8_t *p = start;
    char cur_asset[DYNAMIC_SFX_ASSET_NAME_MAX] = {0};
    int  cur_asset_set = 0;
    int  added         = 0;

    while (p < end) {
        /* Skip whitespace + newlines. */
        while (p < end && (*p == ' ' || *p == '\t' ||
                           *p == '\r' || *p == '\n')) ++p;
        if (p >= end) break;
        /* Stop at next major section. */
        if (p + 9 <= end && memcmp(p, "[komnata]", 9) == 0) break;

        /* [animacja] NAME.wyc — record current asset. */
        if (match_bracket_tag(&p, end, "animacja")) {
            while (p < end && (*p == ' ' || *p == '\t')) ++p;
            const uint8_t *name_start = p;
            while (p < end && *p != ' ' && *p != '\t' &&
                   *p != '\r' && *p != '\n') ++p;
            size_t n = (size_t)(p - name_start);
            if (n >= sizeof cur_asset) n = sizeof cur_asset - 1;
            memcpy(cur_asset, name_start, n);
            cur_asset[n] = 0;
            /* Lowercase — Wacky.scr uses mixed case but AnimAsset.name
             * from LoadAssetFromDtaBase is normalised lowercase. */
            for (size_t i = 0; i < n; ++i) {
                if (cur_asset[i] >= 'A' && cur_asset[i] <= 'Z')
                    cur_asset[i] = (char)(cur_asset[i] + 32);
            }
            cur_asset_set = 1;
            continue;
        }

        /* [sampl] WAV1 WAV2 ... (F,M) (F,) — must follow an [animacja]. */
        if (cur_asset_set && match_bracket_tag(&p, end, "sampl")) {
            /* Collect WAV tokens until '(' (= first frame trigger). */
            const char *wavs[DYNAMIC_SFX_WAVS_PER_SAMPL];
            char        wav_buf[DYNAMIC_SFX_WAVS_PER_SAMPL][32];
            int         wav_count = 0;
            while (p < end && *p != '(' && *p != '\r' && *p != '\n') {
                while (p < end && (*p == ' ' || *p == '\t')) ++p;
                if (p >= end || *p == '(' ||
                    *p == '\r' || *p == '\n') break;
                const uint8_t *tok_start = p;
                while (p < end && *p != ' ' && *p != '\t' &&
                       *p != '(' && *p != '\r' && *p != '\n') ++p;
                size_t tn = (size_t)(p - tok_start);
                if (tn == 0) break;
                if (wav_count < DYNAMIC_SFX_WAVS_PER_SAMPL) {
                    if (tn >= sizeof wav_buf[0]) tn = sizeof wav_buf[0] - 1;
                    memcpy(wav_buf[wav_count], tok_start, tn);
                    wav_buf[wav_count][tn] = 0;
                    for (size_t i = 0; i < tn; ++i) {
                        if (wav_buf[wav_count][i] >= 'A' &&
                            wav_buf[wav_count][i] <= 'Z')
                        {
                            wav_buf[wav_count][i] =
                                (char)(wav_buf[wav_count][i] + 32);
                        }
                    }
                    wavs[wav_count] = wav_buf[wav_count];
                    ++wav_count;
                }
            }
            /* Parse each `(N,M)` tuple — digits → N (start), comma +
             * digits → M (end). Missing M → 0xFFFF (no end frame).
             * The original requires the comma to add the entry at all
             * but shipped Wacky.scr never uses `(N)`, so we mirror the
             * original-equivalent path. */
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
                /* Three tuple shapes from the original engine:
                 *   (N,)   = play trigger      → n_start>=0, n_end<0
                 *   (N,M)  = play + loop end   → n_start>=0, n_end>=0
                 *   (,M)   = stop-only trigger → n_start<0,  n_end>=0
                 *
                 * Stop-only tuples don't pin to a specific wav — when
                 * the entity's frame reaches M, the engine stops
                 * whichever wav from this [sampl] group is currently
                 * playing. Used by e.g. ebek.wyc idle frame 95 →
                 * Muzik* random pick, with (,1) (,25) (,48) (,49)
                 * (,61) as alternate stop frames. Without registering
                 * these our parser dropped them and the muffled rock
                 * music never had a stop trigger; the start trigger
                 * still worked but the SFX kept playing once started.
                 *
                 * Stop-only entries encode as frame_start=-1 (signed
                 * sentinel) so callers know they are not playable. */
                const char *asset_interned =
                    strpool_intern(cur_asset, strlen(cur_asset));
                if (!asset_interned) {
                    while (p < end && (*p == ' ' || *p == '\t')) ++p;
                    continue;
                }
                if (n_start < 0 && n_end >= 0) {
                    /* Stop-only — one entry per [sampl] group is
                     * enough; the trigger looks up by asset alone
                     * and stops every state that asset owns. wav=NULL
                     * marks the entry as stop-only. */
                    if (g_dynamic_sfx_count < DYNAMIC_SFX_MAX) {
                        g_dynamic_sfx[g_dynamic_sfx_count++] =
                            (struct FrameSfxEntry){
                                asset_interned, -1, n_end, NULL};
                        ++added;
                    }
                    while (p < end && (*p == ' ' || *p == '\t')) ++p;
                    continue;
                }
                if (n_start < 0) {
                    /* `()` with no useful contents — skip. */
                    while (p < end && (*p == ' ' || *p == '\t')) ++p;
                    continue;
                }
                int frame_end = (n_end < 0) ? 0xFFFF : n_end;
                /* Register each WAV at frame n_start. */
                for (int wi = 0; wi < wav_count; ++wi) {
                    if (g_dynamic_sfx_count >= DYNAMIC_SFX_MAX) break;
                    const char *wav_interned =
                        strpool_intern(wavs[wi], strlen(wavs[wi]));
                    if (!wav_interned) continue;
                    g_dynamic_sfx[g_dynamic_sfx_count++] =
                        (struct FrameSfxEntry){
                            asset_interned, n_start,
                            frame_end, wav_interned};
                    ++added;
                }
                while (p < end && (*p == ' ' || *p == '\t')) ++p;
            }
            continue;
        }

        /* Unrecognised — skip to next newline. */
        while (p < end && *p != '\n') ++p;
    }
    LOG_INFO("sampl", "parser: %d entries from this [komnata]", added);
}

/* ======================================================================== *
 * Per-(asset, frame, wav) playback state.
 *
 * The original engine's replay guard is `byte+3 == 0 || byte+2 == 0`:
 *   byte+2 (playing_flag) is sticky-on-play, cleared only by an
 *          end-frame trigger or random-pool swap.
 *   byte+3 (channel)      is set when DSound starts the buffer and
 *          cleared when the channel drains.
 *
 * Net effect: a WAV retriggers at the same frame iff its previous
 * instance has finished (drained channel) or a different pool member
 * is picked. An earlier port mis-read this as "byte+2 alone gates
 * retrigger", which silenced every single-pool (N,) entry (the
 * majority: slimak.wyc, footsteps, ambient loops) after first play.
 *
 * Our port keys per `(asset, frame, wav)` tuple in g_sfx_state[]; the
 * `channel` field mirrors byte+3 by storing the mixer channel the WAV
 * was last assigned to. "Drained" is detected via s_mix[channel].active
 * flipping to 0 in the mixer pull.
 * ====================================================================== */

#define SFX_STATE_MAX           256
#define SFX_RANDOM_POOL_MAX     8       /* matches DYNAMIC_SFX_WAVS_PER_SAMPL */
#define SFX_FRAME_END_NONE      0xFFFF
#define SFX_DEFAULT_GAIN        128     /* identity (centred) */

struct SfxState {
    const char *asset;          /* matched by strcmp — see sfx_state_for */
    int         frame;
    const char *wav;            /* interned — matched by pointer identity */
    uint8_t     playing_flag;   /* mirror of wav_state[].playing_flag */
    int8_t      channel;        /* current mixer channel or -1 if free */
};

static struct SfxState g_sfx_state[SFX_STATE_MAX];
static int             g_sfx_state_count = 0;
static int             g_sfx_cur_wav     = -1;   /* mirror of t->cur_wav */
static const char     *g_sfx_cur_asset   = NULL;

/* Stop `st`'s mixer channel iff it's still playing our WAV (the slot
 * may have been stolen for another sound). Logs the reason. */
static void sfx_stop_channel_if_ours(struct SfxState *st, const char *reason)
{
    if (st->channel < MIX_CHAN_SFX_START ||
        st->channel >= MIX_CHANNEL_COUNT || !st->wav)
        return;
    MIX_DEV_LOCK();
    int ours = s_mix[st->channel].active &&
               strcmp(s_mix[st->channel].name, st->wav) == 0;
    MIX_DEV_UNLOCK();
    if (!ours) return;
    LOG_TRACE("sfx", "stop '%s' (asset=%s ch=%d) — %s", st->wav, st->asset ? st->asset : "(null)", st->channel, reason);
    mixer_stop_channel(st->channel);
}

/* Reset all SFX state. Called on scene transition (the original frees
 * every asset on the komnata, which stops every WAV in each asset's
 * SampleTable).
 *
 * Three responsibilities:
 *  1. Stop any still-playing mixer channels that g_sfx_state[] points
 *     to. Looping (N,M) WAVs whose M was never ticked (player left
 *     the room mid-rakieta) would otherwise bleed into the next
 *     komnata's audio.
 *  2. Zero the slot array. Without this the (asset, wav) pointers in
 *     g_sfx_state[] become dangling — ResetDynamicSfxTable is called
 *     right after us in LoadKomnata and clears g_dynamic_sfx_strpool,
 *     so any retained pointer is unsafe to dereference.
 *  3. Reset the cur_wav/cur_asset mirror of the original's
 *     per-table fields. */
void ResetFrameSfxState(void)
{
    for (int i = 0; i < g_sfx_state_count; ++i) {
        sfx_stop_channel_if_ours(&g_sfx_state[i], "komnata reset");
    }
    g_sfx_state_count = 0;
    g_sfx_cur_wav     = -1;
    g_sfx_cur_asset   = NULL;
}

/* Find-or-create the state slot for (asset_name, frame, wav).
 *
 * Asset names arrive from two paths with different pointers —
 * TriggerFrameSfx passes `atlas->name` (a buffer inside AnimAsset),
 * while sfx_handle_end_frames and StopAllSfxForAsset pass interned
 * pointers from g_dynamic_sfx_strpool. Both strings compare equal but
 * the pointers don't, so we strcmp on asset. `wav` is always interned
 * (both paths derive it from g_dynamic_sfx[].wav) so pointer-equality
 * is fine there. */
static struct SfxState *sfx_state_for(const char *asset, int frame,
                                      const char *wav)
{
    for (int i = 0; i < g_sfx_state_count; ++i) {
        if (g_sfx_state[i].frame == frame &&
            g_sfx_state[i].wav   == wav &&
            g_sfx_state[i].asset && asset &&
            strcmp(g_sfx_state[i].asset, asset) == 0)
            return &g_sfx_state[i];
    }
    if (g_sfx_state_count >= SFX_STATE_MAX) return NULL;
    g_sfx_state[g_sfx_state_count] =
        (struct SfxState){asset, frame, wav, 0, -1};
    return &g_sfx_state[g_sfx_state_count++];
}

/* Stop every currently-playing SfxState whose asset matches, and
 * zero its flag. Called when an asset slot is overwritten
 * (ScriptCallLoadAsset) — the per-entity VM may not tick frame_end
 * for looping (N,M) entries if the bytecode skips frames or the
 * script switches assets mid-animation. */
void StopAllSfxForAsset(const char *asset_name)
{
    if (!asset_name) return;
    for (int i = 0; i < g_sfx_state_count; ++i) {
        struct SfxState *st = &g_sfx_state[i];
        if (!st->asset || strcmp(st->asset, asset_name) != 0) continue;
        sfx_stop_channel_if_ours(st, "asset reload");
        st->channel      = -1;
        st->playing_flag = 0;
    }
}

/* Pass 1 — end-frame handling. `(N,M)` entries in Wacky.scr loop the
 * WAV between frames N and M; hitting M stops the loop. Without this,
 * marsz.wav / rakiet1a.wav play once and fall silent halfway through
 * the rakieta.wyc animation (frames 1..464 needing ~30s of audio from
 * a 10s wav). */
static void sfx_handle_end_frames(const char *asset_name, int frame)
{
    for (int i = 0; i < g_dynamic_sfx_count; ++i) {
        struct FrameSfxEntry *e = &g_dynamic_sfx[i];
        if (e->frame_end == SFX_FRAME_END_NONE) continue;
        /* Exact match — mirrors the original engine. A `frame >=
         * frame_end` test fires stop triggers retroactively on
         * every frame past the end, which immediately wipes the
         * Muzik headphone-rock loop the moment Ebek's frame 95
         * start trigger lands (95 already past all of 1/25/48/49/61).
         * The skateboard-crash overshoot case is handled by
         * StopAllSfxForAsset in ScriptCallDestroyEnt instead. */
        if (frame != e->frame_end) continue;
        if (strcmp(e->asset, asset_name) != 0) continue;

        if (e->frame_start < 0 || e->wav == NULL) {
            /* Stop-only entry (from a `(,M)` tuple). The original
             * engine stops whichever wav from this [sampl] group is
             * currently playing; we don't know which one, so sweep
             * every SfxState matching this asset. */
            for (int j = 0; j < g_sfx_state_count; ++j) {
                struct SfxState *sst = &g_sfx_state[j];
                if (!sst->asset || strcmp(sst->asset, asset_name) != 0)
                    continue;
                if (!sst->playing_flag && sst->channel < 0) continue;
                LOG_TRACE("sfx", "stop-only frame=%d asset=%s wav='%s' ch=%d",
                          frame, asset_name, sst->wav, sst->channel);
                if (sst->channel >= MIX_CHAN_SFX_START &&
                    sst->channel < MIX_CHANNEL_COUNT) {
                    mixer_stop_channel(sst->channel);
                }
                sst->channel      = -1;
                sst->playing_flag = 0;
            }
            continue;
        }

        struct SfxState *st = sfx_state_for(e->asset, e->frame_start, e->wav);
        if (!st) continue;
        if (!st->playing_flag && st->channel < 0) continue;     /* already stopped */
        LOG_TRACE("sfx", "stop '%s' at end-frame %d (asset=%s start=%d ch=%d)",
                  e->wav, frame, asset_name, e->frame_start, st->channel);
        if (st->channel >= MIX_CHAN_SFX_START &&
            st->channel < MIX_CHANNEL_COUNT) {
            mixer_stop_channel(st->channel);
        }
        st->channel      = -1;
        st->playing_flag = 0;
    }
}

/* Collect every dynamic SFX entry whose (asset, frame_start) matches
 * the current trigger. Returns the pool size (0..SFX_RANDOM_POOL_MAX). */
static int collect_random_sfx_pool(const char *asset_name, int frame,
                                   const char **pool, int *pool_end)
{
    int n = 0;
    for (int i = 0;
         i < g_dynamic_sfx_count && n < SFX_RANDOM_POOL_MAX;
         ++i)
    {
        if (g_dynamic_sfx[i].frame_start == frame &&
            strcmp(g_dynamic_sfx[i].asset, asset_name) == 0)
        {
            pool[n]     = g_dynamic_sfx[i].wav;
            pool_end[n] = g_dynamic_sfx[i].frame_end;
            ++n;
        }
    }
    return n;
}

/* Stop the previously-picked WAV from the same asset's pool if it's
 * still active. Used when the random pick swaps mid-stream. */
static void stop_previous_pool_pick(const char *asset_name, int frame,
                                    const char **pool, int pool_n)
{
    if (g_sfx_cur_asset != asset_name) return;
    if (g_sfx_cur_wav < 0 || g_sfx_cur_wav >= pool_n) return;
    struct SfxState *prev = sfx_state_for(asset_name, frame,
                                          pool[g_sfx_cur_wav]);
    if (!prev) return;
    prev->playing_flag = 0;
    if (prev->channel >= 0) {
        mixer_stop_channel(prev->channel);
        prev->channel = -1;
    }
}

/* True iff `st` is currently active on its mixer channel playing
 * exactly `wav`. The lock is held only for the channel snapshot. */
static int sfx_is_currently_playing(struct SfxState *st, const char *wav)
{
    if (st->channel < MIX_CHAN_SFX_START ||
        st->channel >= MIX_CHANNEL_COUNT)
        return 0;
    int playing = 0;
    MIX_DEV_LOCK();
    if (s_mix[st->channel].active &&
        strcmp(s_mix[st->channel].name, wav) == 0)
        playing = 1;
    MIX_DEV_UNLOCK();
    return playing;
}

void TriggerFrameSfx(const char *asset_name, int frame)
{
    if (!asset_name) return;

    /* Pass 1: stop any (N,M) loops whose M matches this frame. */
    sfx_handle_end_frames(asset_name, frame);

    /* Pass 2: collect the random pool for (asset, start=frame). */
    const char *pool[SFX_RANDOM_POOL_MAX];
    int         pool_end[SFX_RANDOM_POOL_MAX];
    int n = collect_random_sfx_pool(asset_name, frame, pool, pool_end);
    if (n == 0) return;

    int         random_idx = (int)WackiRand((uint16_t)n);
    const char *wav        = pool[random_idx];

    /* If the previous pick was a different WAV from this asset, stop
     * it before launching the new one. */
    if (g_sfx_cur_asset == asset_name && g_sfx_cur_wav != random_idx)
        stop_previous_pool_pick(asset_name, frame, pool, n);
    g_sfx_cur_asset = asset_name;
    g_sfx_cur_wav   = random_idx;

    /* Replay guard: skip only when the same WAV is BOTH still active
     * on its mixer channel AND has fired at least once. The sticky
     * playing_flag alone never blocks replay — it just records that
     * we've started this trigger before. */
    struct SfxState *st = sfx_state_for(asset_name, frame, wav);
    if (!st) return;
    if (st->playing_flag && sfx_is_currently_playing(st, wav)) return;
    st->playing_flag = 1;

    /* loop=1 when this start-trigger has any explicit end frame
     * (either inline `(N,M)` or a sibling `(,M)` stop-only tuple
     * in the same [sampl] group). Without looping, the muffled rock
     * music on Ebek's idle frame 95 played once and went silent
     * before the (,1) (,25) (,48) (,49) (,61) stop frames could
     * fire — the stop-only siblings imply "loop me until told to
     * quit", so respect that. */
    int has_sibling_stop = 0;
    if (pool_end[random_idx] == SFX_FRAME_END_NONE) {
        for (int i = 0; i < g_dynamic_sfx_count; ++i) {
            struct FrameSfxEntry *de = &g_dynamic_sfx[i];
            if (de->frame_start < 0 && de->wav == NULL &&
                de->asset == asset_name)
            {
                has_sibling_stop = 1;
                break;
            }
        }
    }
    int want_loop = (pool_end[random_idx] != SFX_FRAME_END_NONE) ||
                    has_sibling_stop;
    st->channel = (int8_t)PlaySfxLoopAndGetChannel(wav, want_loop);
}

/* ======================================================================== *
 * Fire-and-forget public API — backed by the shared mixer channel pool
 * [MIX_CHAN_SFX_START .. MIX_CHANNEL_COUNT). Picks a free channel or
 * steals the oldest if all busy. Auto-frees the converted buffer when
 * the callback drains the channel (TickSfx).
 * ====================================================================== */

void PlaySfx(const char *wav_name)
{
    PlaySfxPanned(wav_name, SFX_DEFAULT_GAIN, SFX_DEFAULT_GAIN);
}

/* PlaySfxPanned — same as PlaySfx with explicit L/R gain. The caller
 * supplies the gains (frame-driven [sampl] SFX use the listener-relative
 * pan computed at trigger time). */
void PlaySfxPanned(const char *wav_name, uint8_t gain_l, uint8_t gain_r)
{
    (void)PlaySfxPannedAndGetChannel(wav_name, gain_l, gain_r);
}

/* Internal — returns the mixer channel the WAV landed on, or -1 if
 * the load failed / mixer not open. Used by TriggerFrameSfx so it
 * can stop the previous WAV in flight when cur_wav swaps. */
int PlaySfxPannedAndGetChannel(const char *wav_name,
                               uint8_t gain_l, uint8_t gain_r)
{
    if (!wav_name) return -1;
    if (!g_audio_sfx_enabled || !g_audio_sound_enabled) return -1;
    if (!mixer_ensure_open()) return -1;

    /* Find a free channel — or steal the oldest if all busy. */
    int      slot        = -1;
    uint32_t oldest      = 0xFFFFFFFFu;
    int      oldest_slot = MIX_CHAN_SFX_START;
    for (int i = MIX_CHAN_SFX_START; i < MIX_CHANNEL_COUNT; ++i) {
        if (!s_mix[i].active) { slot = i; break; }
        if (s_mix[i].start_tick < oldest) {
            oldest      = s_mix[i].start_tick;
            oldest_slot = i;
        }
    }
    if (slot < 0) slot = oldest_slot;

    Uint8  *buf = NULL;
    Uint32  len = 0;
    if (!mixer_load_wav(wav_name, &buf, &len)) {
        LOG_TRACE("sfx", "cannot load '%s'", wav_name);
        return -1;
    }
    mixer_assign(slot, buf, len, 0, wav_name);
    /* mixer_assign sets gain to 128/128 (identity); override here for
     * positional pan. Lock once for an atomic L+R update. */
    MIX_DEV_LOCK();
    s_mix[slot].gain_l = gain_l;
    s_mix[slot].gain_r = gain_r;
    MIX_DEV_UNLOCK();
    LOG_TRACE("sfx", "play '%s' on mixer ch %d (%u bytes, gain L=%u R=%u)", wav_name, slot, len, gain_l, gain_r);
    return slot;
}

int PlaySfxAndGetChannel(const char *wav_name)
{
    return PlaySfxPannedAndGetChannel(wav_name,
                                      SFX_DEFAULT_GAIN, SFX_DEFAULT_GAIN);
}

/* Looping variant — same as PlaySfxAndGetChannel but assigns the
 * mixer channel with `loop=1` so the audio callback wraps src_frame
 * back to 0 instead of marking the channel inactive. Used by
 * TriggerFrameSfx for `[sampl] WAV (N,M)` entries — caller must stop
 * the channel at frame M via sfx_handle_end_frames. */
int PlaySfxLoopAndGetChannel(const char *wav_name, int loop)
{
    int ch = PlaySfxPannedAndGetChannel(wav_name,
                                        SFX_DEFAULT_GAIN, SFX_DEFAULT_GAIN);
    if (ch >= 0 && loop) {
        MIX_DEV_LOCK();
        s_mix[ch].loop = 1;
        MIX_DEV_UNLOCK();
    }
    return ch;
}

/* ---- test-only accessors -----------------------------------------------
 *
 * Tests under tests/test_sampl_parser.c use these to verify
 * ParseSamplTagsForKomnata's output without depending on the (static)
 * g_dynamic_sfx layout. They peek into the same per-komnata table the
 * trigger code reads. No-cost in release builds — never called from
 * production code, just leaks four small functions to the linker. */

int sfx_test_dynamic_count(void)
{
    return g_dynamic_sfx_count;
}

const char *sfx_test_dynamic_asset(int idx)
{
    if (idx < 0 || idx >= g_dynamic_sfx_count) return NULL;
    return g_dynamic_sfx[idx].asset;
}

int sfx_test_dynamic_frame_start(int idx)
{
    if (idx < 0 || idx >= g_dynamic_sfx_count) return 0;
    return g_dynamic_sfx[idx].frame_start;
}

int sfx_test_dynamic_frame_end(int idx)
{
    if (idx < 0 || idx >= g_dynamic_sfx_count) return 0;
    return g_dynamic_sfx[idx].frame_end;
}

const char *sfx_test_dynamic_wav(int idx)
{
    if (idx < 0 || idx >= g_dynamic_sfx_count) return NULL;
    return g_dynamic_sfx[idx].wav;
}
