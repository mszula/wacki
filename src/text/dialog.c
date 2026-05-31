/* src/text/dialog.c — op 0x52 DialogBegin / op 0x53 DialogEnd.
 *
 * Two main jobs:
 *
 *   1. Per-conversation stack management. op 0x52 pushes a slot
 *      capturing the speaker entity's current atlas + bytecode +
 *      walker state; op 0x53 pops the stack and restores them. Nested
 *      dialogs are supported (DIALOG_STACK_MAX deep).
 *
 *   2. Per-line dispatch. After op 0x53 the runner walks the
 *      [rozmowa]<result> section of Gadki.scr, extracts each [sampl]
 *      block (WAV filename + optional [eb]/[fj]/[nic] speaker tag +
 *      speech text), plays the line via PlayDialogLine + ScriptCall
 *      ShowText, and waits for both audio + text to finish before
 *      moving on. While the line is playing the speaker entity's
 *      atlas + bytecode are swapped to the dialog's talk-anim chain
 *      (so the speaker's mouth animates from real bytecode rather
 *      than a hard-coded frame toggle) and restored after.
 *
 * The speech balloon itself (ScriptCallShowText + TickSpeechBalloon)
 * lives in src/text/balloon.c — this module reuses it for the per-line
 * text display.
 *
 * Globals defined here that other modules read:
 *   g_dialog_active     — non-zero between op 0x52 push and op 0x53 pop
 *                         (HandleSceneInput + DispatchClickEvent annotate
 *                         logs with [dlg] when set)
 *   g_subtitles_on /    — Solund-menu gates; mirrored from g_save.settings
 *   g_dialogues_on        on boot by ApplySavedSettings */

#include "wacki.h"
#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern uint32_t       ent_ptr_intern(void *p);
extern const void    *xlat_binary_ptr(uint32_t addr);
extern AnimAsset     *LoadAssetFromDtaBase(const char *name);

/* ScriptCallDialogBegin — partial port of (DialogPush) +
 * (DialogRunner) ops 0x52 / 0x53.
 *
 * Full original flow:
 * op 0x52 = — push dialog onto an internal stack with:
 * op 0x53 = — run the interactive loop:
 * FindSection(Gadki.scr, "[rozmowa]", dialog_name, "[animacja]")
 * for each text line in section:
 * allocate speech bubble, play audio, ProcessGameFrameTick wait
 * while audio playing; process per-frame events; advance
 * on user choice (panel click): return result_key
 *
 * Partial port goal: at least make the dialog NAME look up the section
 * in Gadki.scr and display the first text line as a speech balloon so
 * the user sees real dialog text instead of just the dialog name.
 *
 * The full interactive loop (with audio, options, multi-line scroll)
 * stays a stub until D2 is fully completed. */
extern int ScriptObjFindSection(void *self_, const char *tag,
                                const char *param, const char *altparam);
extern const uint8_t *ScriptObjGetSectionStart(void *self);
extern const uint8_t *ScriptObjGetSectionEnd  (void *self);

/* Extract the next [sampl] block from a [rozmowa]<name> section.
 *
 * Gadki.scr [rozmowa] sections have NO inline text — the audio file IS
 * the dialog content. Each line is structured as:
 *
 * [sampl] fjgut04a.wav
 * [fj] Optional spoken text (speaker = fj = Fjej)
 * [sampl] fjgut04b.wav
 * [eb] Another line (speaker = eb = Ebek)
 * [nic] ← silent / no text
 *
 * outer loop, which:
 * 1. Finds next [sampl] tag
 * 2. Copies filename after it (skipping whitespace, until next ws)
 * 3. Plays the WAV
 * 4. Inside playback wait loop, scans forward for [eb]/[fj]/[nic]
 * and renders a speech bubble per tag found
 *
 * Earlier port treated lines as inline text — bailed on the first '['
 * character. That's why `[dialog] section 'X' played 0 lines` —
 * the very first line of every section is `[sampl]`, instantly
 * terminating the scan.
 *
 * Returns 1 if a [sampl] block was extracted (out_wav populated; speaker
 * = 'e'/'f'/0 = none; out_text = matching speech bubble or empty).
 * Returns 0 at end of section. */
static int dialog_extract_line(const uint8_t **pss, const uint8_t *se,
                               char *out_wav, size_t out_wav_sz,
                               char *out_text, size_t out_text_sz,
                               char *out_speaker)
{
    const uint8_t *ss = *pss;
    if (out_wav)     out_wav[0] = 0;
    if (out_text)    out_text[0] = 0;
    if (out_speaker) *out_speaker = 0;

    /* ---- find next [sampl] tag --------------------------------------- */
    const char *sampl_tag = "[sampl]";
    const size_t sampl_len = 7;
    const uint8_t *sampl_at = NULL;
    while (ss + sampl_len <= se) {
        if (ss[0] == '[' && memcmp(ss, sampl_tag, sampl_len) == 0) {
            sampl_at = ss;
            break;
        }
        ++ss;
    }
    if (!sampl_at) { *pss = se; return 0; }

    /* Copy WAV filename right after [sampl] — skip ws, take until ws/EOL. */
    const uint8_t *p = sampl_at + sampl_len;
    while (p < se && (*p == ' ' || *p == '\t')) ++p;
    size_t n = 0;
    while (p < se && *p != ' ' && *p != '\t' &&
           *p != '\r' && *p != '\n' && n + 1 < out_wav_sz) {
        out_wav[n++] = (char)*p++;
    }
    out_wav[n] = 0;

    /* Lowercase for DTA lookup case-insensitivity (DTA names live as
 * sent in archive; the archive lookup is case-sensitive but assets
 * are stored lowercase. Original GetFileBySection does its own
 * normalisation; we mirror just enough of it for the .wav path). */
    for (size_t i = 0; i < n; ++i) {
        if (out_wav[i] >= 'A' && out_wav[i] <= 'Z') out_wav[i] += 32;
    }

    /* ---- find next speaker tag ([eb] / [fj] / [nic]) up to next [sampl] - */
    const uint8_t *block_end = se;
    {
        const uint8_t *q = p;
        while (q + sampl_len <= se) {
            if (q[0] == '[' && memcmp(q, sampl_tag, sampl_len) == 0) {
                block_end = q;
                break;
            }
            ++q;
        }
    }

    while (p < block_end) {
        /* Skip ws / newlines between tags. */
        while (p < block_end && (*p == ' ' || *p == '\t' ||
                                 *p == '\r' || *p == '\n')) ++p;
        if (p >= block_end) break;
        if (*p != '[') { ++p; continue; }

        /* Match [eb] / [fj] / [nic] */
        char sp = 0;
        size_t tag_len = 0;
        if (p + 4 <= block_end && memcmp(p, "[eb]", 4) == 0) {
            sp = 'e'; tag_len = 4;
        } else if (p + 4 <= block_end && memcmp(p, "[fj]", 4) == 0) {
            sp = 'f'; tag_len = 4;
        } else if (p + 5 <= block_end && memcmp(p, "[nic]", 5) == 0) {
            /* Silent block — no speaker, no text. Consume tag + bail. */
            p += 5;
            *pss = block_end;
            return 1;
        } else {
            /* Some other tag — skip it. */
            while (p < block_end && *p != ']') ++p;
            if (p < block_end) ++p;
            continue;
        }

        p += tag_len;
        /* Skip ws after the speaker tag. */
        while (p < block_end && (*p == ' ' || *p == '\t')) ++p;
        /* Copy text until EOL (or next tag). */
        size_t tn = 0;
        while (p < block_end && *p != '\r' && *p != '\n' &&
               *p != '[' && tn + 1 < out_text_sz) {
            out_text[tn++] = (char)*p++;
        }
        while (tn > 0 && (out_text[tn-1] == ' ' || out_text[tn-1] == '\t'))
            --tn;
        out_text[tn] = 0;
        if (out_speaker) *out_speaker = sp;
        break;
    }

    *pss = block_end;
    return 1;
}


/* Play a single dialog line as a speech bubble, then wait for it to
 * dismiss before returning. Mirrors the original per-line loop inside
 * (which also pumps audio + timer until the line is done). */
extern void ScriptCallShowText(uint16_t actor, const char *text);

/* dialog_make_audio_name — retired in T20b. The WAV name is now taken
 * straight from the [sampl] tag inside Gadki.scr (see dialog_extract_line),
 * not built from `<section>_idx.wav` which was a guess. Names like
 * `fj_gut04` → wav `fjgut04a.wav` don't follow the old pattern. */

static void dialog_play_line_indexed(uint16_t actor, const char *text,
                                     const char *dialog_base, int line_idx);

/* T20: retired — no producer after dialog_play_section_lines stopped
 * using placeholder fallback. Kept under #if 0 in case interactive
 * fallback comes back. */
#if 0
static void dialog_play_line(uint16_t actor, const char *text)
{
    dialog_play_line_indexed(actor, text, NULL, 0);
}
#endif

/* dialog_play_line_indexed — T6 / T20b: dialog with per-line audio.
 *
 * `wav_name` is the .wav filename extracted from a [sampl] tag (
 * — it copies the name into then calls
 * to load + play). NULL or empty = silent line (text-only,
 * matches the [nic] tag case). The earlier port built names from
 * `<section_name><idx>.wav` which was an unverified guess — actual
 * WAV filenames in Gadki.scr [sampl] tags don't match that convention
 * (e.g. section `fj_gut04` → wav `fjgut04a.wav`).
 *
 * `text` can be empty for silent ([nic]) lines or [sampl]-only entries.
 * If both are absent, the function returns without action.
 *
 * Wait loop polls both: text dismiss-tick AND dialog audio playing flag. */
/* Forward decls — definitions follow further down (next to the
 * DialogStackPush/Pop block, which they share state with). */
static void DialogActivateTopSpeaker(void);
static void DialogRestoreTopSpeaker(void);
/* T108 — DialogTickMouth retired; mouth anim now driven by per-entity VM
 * via talk_anim bytecode bound in DialogActivateTopSpeaker. */

static void dialog_play_line_indexed(uint16_t actor, const char *text,
                                     const char *wav_name, int line_idx)
{
    /* Either we have audio, or we have text, or both — silent + textless
 * means there's nothing to do (this happens for orphan [sampl] entries
 * that point at missing files plus an empty bubble). */
    int has_text = text && *text;
    int has_audio = wav_name && *wav_name;
    if (!has_text && !has_audio) return;
    (void)line_idx;

    if (has_text) ScriptCallShowText(actor, text);

    int audio_started = 0;
    if (has_audio) {
        uint32_t len = PlayDialogLine(wav_name);
        if (len > 0) audio_started = 1;
    }

    /* T108 — bind the speaker entity to dialog atlas + mouth-cycle
 * bytecode via DialogActivateTopSpeaker. Per-entity VM
 * (ExecEntityScript) advances frames natively from the bytecode
 * each tick — no manual DialogTickMouth needed any more. On line
 * end DialogRestoreTopSpeaker swaps back to the actor's pre-dialog
 * idle anim. */
    DialogActivateTopSpeaker();

    extern Entity *g_speech_balloon;
    extern uint8_t g_lmb_clicked;
    extern uint16_t g_speech_dismiss_ticks;
    /* When audio is playing, allow up to ~30s safety (longer than any
 * realistic dialog line). Without audio, ~10s. Without text bubble
 * the loop simply waits on audio completion. We arm a grace period
 * (~5 frames ≈ 165 ms) before checking IsDialogLinePlaying so the
 * mixer has time to actually enqueue the WAV — without this the
 * audio-only path can break out on frame 0 because PlayDialogLine
 * returns synchronously but the audio thread hasn't transitioned
 * the channel into the "playing" state yet. */
    int safety = audio_started ? 1800 : 600;
    int audio_grace = audio_started ? 5 : 0;
    while (safety-- > 0) {
        if (has_text && !g_speech_balloon) break;
        if (!has_text && audio_grace == 0 && !IsDialogLinePlaying()) break;
        if (audio_grace > 0) --audio_grace;
        /* T108 — mouth animation is now bytecode-driven. The per-entity
 * VM tick (called inside ProcessGameFrameTick → EntityWalkerTick →
 * ExecEntityScript) cycles frames per the bound talk_anim
 * bytecode automatically. No manual DialogTickMouth needed. */
        ProcessGameFrameTick();
        extern int PlatformShouldQuit(void);
        if (PlatformShouldQuit()) break;
        if (g_game_over_code) break;
        if (g_lmb_clicked) {
            g_lmb_clicked = 0;
            g_speech_dismiss_ticks = 0;
            StopDialogLine();              /* T6: cancel mid-line speech */
            ProcessGameFrameTick();
            break;
        }
        /* T6 lip-sync: if audio is the timing source, wait until BOTH
 * (a) audio finishes AND (b) text dismiss-timer expires.
 * If audio finished but text still has time, that's fine —
 * the loop naturally exits when text expires. */
        if (audio_started && !IsDialogLinePlaying()) {
            /* Audio done. Fast-forward text dismiss so the line snaps
 * away on next tick ( behavior where
 * progress-poll returns 0 then text clears). */
            g_speech_dismiss_ticks = 0;
            ProcessGameFrameTick();
            break;
        }
        /* Throttle to ~60 fps so the loop doesn't spin CPU + so the
 * safety iteration counter matches a realistic time budget. */
        SDL_Delay(33);  /* T-anim-speed: match main loop pacing */
    }
    /* T20c — line over; flip atlas back to the speaker's original pose
 * so they don't stay frozen in talking-head form between lines. */
    DialogRestoreTopSpeaker();
}

/* T20b — Activate / restore the speaker's dialog atlas during line
 * playback. Original toggles entity[+0x28] (atlas) and
 * entity[+0x2C] (bytecode) per-slot:
 * ACTIVATE (mode 1): atlas → slot.asset (e.g. fjgadap.wyc),
 * bytecode → slot.talk_anim_va (mouth-cycling
 * bytecode ptr)
 * RESTORE (mode 0): atlas → slot.atlas_backup,
 * bytecode → slot.bytecode_backup
 *
 * Port shortcut: we don't read the original mouth-cycling bytecode
 * (param_4 of op 0x52) — that requires deeper RE of the slot[+0x0C]
 * pointer layout. Instead we:
 * - Swap atlas only (visible "talking pose"). Entity VM continues
 * running the backed-up bytecode, which references frame indices
 * that may not all exist in the dialog atlas. We CLAMP frame
 * index to atlas->frame_count in the renderer (existing safety
 * net), so the worst case is a static talking face.
 * - Drive mouth open/closed frame cycling manually: toggle frame 0
 * ↔ 1 (or 0↔frame_count-1 if 2-frame atlas) every ~150 ms while
 * audio is playing. Matches the visual cadence the original gets
 * from its mouth-cycle bytecode. */
/* Definitions of DialogActivateTopSpeaker / DialogRestoreTopSpeaker /
 * DialogTickMouth moved AFTER the DialogStackSlot type and
 * s_dialog_stack array decl below — they need that struct visible. */

/* T21 — DialogPush/Pop stack ( + ).
 *
 * Original layout: each pushed slot is 0x18 bytes inside a dynamically
 * grown vector ( grow helper). Layout per slot:
 * +0x00 entity ptr — speaker entity
 * +0x04 opts_hash — horner-folded opts bytes (used by some
 * internal lookup; unused in our partial
 * port but stored for fidelity)
 * +0x08 loaded asset ptr — LoadAssetFromDtaBase(dialog_name)
 * +0x0C opts count — number of choice entries
 * +0x10 entity[+0x28] backup — atlas at push time
 * +0x14 entity[+0x2C] backup — bytecode at push time
 *
 * On pop, for each pushed slot, in iteration order:
 * - Restore entity[+0x28] to backup via (which also
 * reallocs entity bitmap if the restored atlas needs more space).
 * - Run walker-state reset on entity.
 * - Clear entity[+0x30] (script pc) to 0.
 * - Restore entity[+0x2C] from backup.
 * - Free the loaded asset ( = FreeAsset).
 *
 * Original dialog stack lives in RunScriptInterpreter's local_15c (per-
 * invocation). Our port uses a process-global stack since
 * ScriptCallDialogBegin and ScriptCallDialogEnd are split across
 * RSI calls (the port's runner sequencer is more linear). Nested
 * dialogs ARE supported (push twice, pop pops the top one). */
typedef struct DialogStackSlot {
    Entity   *entity;            /* speaker entity (NULL if not resolved) */
    AnimAsset *asset;            /* loaded dialog asset (FreeAsset on pop) */
    uint32_t  opts_hash;         /* horner fold of opts bytes — port fidelity */
    uint32_t  talk_anim_va;      /* T108: PE VA of mouth-cycle bytecode
 * (= op 0x52's 4th arg / slot+0x0C in
 * the original 0x18-byte slot). Bound to
 * entity[+0x2C] by ACTIVATE, restored
 * from bytecode_backup by RESTORE. Earlier
 * field was misnamed `opts_count`. */
    uint32_t  atlas_backup;      /* entity[+0x28] at push time (intern slot) */
    uint32_t  bytecode_backup;   /* entity[+0x2C] at push time (intern slot) */
} DialogStackSlot;

#define DIALOG_STACK_MAX 8
static DialogStackSlot s_dialog_stack[DIALOG_STACK_MAX];
static int             s_dialog_stack_n = 0;

/* T108 — Dialog ACTIVATE/RESTORE helpers.
 * Per Ghidra:
 * (entity, slot[+0x10]); // restore atlas_backup
 * (entity); // walker reset
 * } else { // ACTIVATE
 * (entity, slot[+0x08]); // bind dialog asset
 * (entity); // walker reset
 *
 * Earlier port (T20c) only swapped the atlas and drove a hardcoded
 * frame 0↔1 toggle via DialogTickMouth — that was a port shortcut
 * needed because we didn't yet have the bytecode pointer (op 0x52's
 * 4th arg, threaded through in T108). Now with talk_anim_va resolved
 * via xlat_binary_ptr we can bind real bytecode; the per-entity VM
 * cycles the mouth frames naturally via its standard frame-advance
 * timer, matching original cadence + animation curve exactly.
 *
 * DialogTickMouth retired — bytecode drives the animation now. */
extern uint32_t ent_ptr_intern(void *);
extern const void *xlat_binary_ptr(uint32_t addr);

static void DialogActivateTopSpeaker(void)
{
    if (s_dialog_stack_n == 0) return;
    DialogStackSlot *slot = &s_dialog_stack[s_dialog_stack_n - 1];
    Entity *e = slot->entity;
    if (!e || !slot->asset) return;
    uint8_t *eb = (uint8_t *)e;
    /* Atlas: bind loaded dialog asset (= slot[+0x08]). */
    *(uint32_t *)(eb + 0x28) = ent_ptr_intern(slot->asset);
    /* Walker reset partial reset (clears walker
 * busy + delays so the new bytecode runs fresh from pc=0). */
    *(uint16_t *)(eb + 0x3A) &= (uint16_t)~5u;
    *(uint16_t *)(eb + 0x38) = 0;
    *(uint16_t *)(eb + 0x36) = 0;
    *(uint16_t *)(eb + 0x34) = 0;
    *(uint16_t *)(eb + 0x3C) = 0;
    *(uint16_t *)(eb + 0x42) = 0;
    *(uint16_t *)(eb + 0x40) = 0;
    *(uint16_t *)(eb + 0x32) = 0;
    *(uint32_t *)(eb + 0x50) = 0;
    *(uint32_t *)(eb + 0x4C) = 0;
    /* Bytecode: bind talk_anim_va — the mouth-cycle program from PE.
 * Per-entity VM ( / ExecEntityScript) advances frames
 * automatically per its op 0x06/op 0x08/etc. instructions. */
    if (slot->talk_anim_va) {
        const void *bc = xlat_binary_ptr(slot->talk_anim_va);
        if (bc) *(uint32_t *)(eb + 0x2C) = ent_ptr_intern((void *)bc);
    }
    /* Reset frame pc — VM starts from frame 0. */
    *(uint16_t *)(eb + 0x30) = 0;
}

static void DialogRestoreTopSpeaker(void)
{
    if (s_dialog_stack_n == 0) return;
    DialogStackSlot *slot = &s_dialog_stack[s_dialog_stack_n - 1];
    Entity *e = slot->entity;
    if (!e) return;
    uint8_t *eb = (uint8_t *)e;
    /* Restore atlas + bytecode from backups taken at push time. */
    *(uint32_t *)(eb + 0x28) = slot->atlas_backup;
    /* Walker reset ( — same as ACTIVATE path). */
    *(uint16_t *)(eb + 0x3A) &= (uint16_t)~5u;
    *(uint16_t *)(eb + 0x38) = 0;
    *(uint16_t *)(eb + 0x36) = 0;
    *(uint16_t *)(eb + 0x34) = 0;
    *(uint16_t *)(eb + 0x3C) = 0;
    *(uint16_t *)(eb + 0x42) = 0;
    *(uint16_t *)(eb + 0x40) = 0;
    *(uint16_t *)(eb + 0x32) = 0;
    *(uint32_t *)(eb + 0x50) = 0;
    *(uint32_t *)(eb + 0x4C) = 0;
    *(uint32_t *)(eb + 0x2C) = slot->bytecode_backup;
    *(uint16_t *)(eb + 0x30) = 0;
}

/* T108 — DialogTickMouth retired entirely. The T20c port shortcut
 * (manual frame 0↔1 toggle every 150 ms) is no longer needed: per-entity
 * VM cycles mouth frames natively via the talk_anim bytecode bound by
 * DialogActivateTopSpeaker. Function deleted; remove the forward decl
 * up near dialog_play_line_indexed if linkage breaks. */

/* DialogStackPush Allocates asset, stores
 * speaker entity backups. Returns the stack depth after push, or
 * -1 on overflow / asset load failure (mirrors original: original
 * only commits the slot if LoadAssetFromDtaBase succeeded). */
static int DialogStackPush(Entity *speaker, const char *dialog_name,
                           const uint8_t *opts_bytes, uint32_t talk_anim_va)
{
    if (s_dialog_stack_n >= DIALOG_STACK_MAX) {
        fprintf(stderr, "[dialog] push: stack full (%d)\n", s_dialog_stack_n);
        return -1;
    }
    if (!speaker) {
        fprintf(stderr, "[dialog] push: NULL speaker — skip\n");
        return -1;
    }

    DialogStackSlot *slot = &s_dialog_stack[s_dialog_stack_n];
    slot->entity        = speaker;
    slot->talk_anim_va  = talk_anim_va;

    /* Horner fold ( inner loop: `h = h*2 + b`). */
    uint32_t h = 0;
    if (opts_bytes) {
        for (const uint8_t *p = opts_bytes; *p; ++p)
            h = h * 2u + *p;
    }
    slot->opts_hash = h;

    /* Backup atlas + bytecode slots (intern handles — match script-byte
 * layout). Even if some are 0 (no current atlas) we still record. */
    uint8_t *eb = (uint8_t *)speaker;
    slot->atlas_backup    = *(uint32_t *)(eb + 0x28);
    slot->bytecode_backup = *(uint32_t *)(eb + 0x2C);

    /* Load dialog asset. Original commits the slot ONLY if load
 * succeeded; we mirror that: on failure, don't bump count. */
    AnimAsset *a = LoadAssetFromDtaBase(dialog_name);
    if (!a) {
        fprintf(stderr, "[dialog] push: asset '%s' load failed — skip\n",
                dialog_name ? dialog_name : "(null)");
        return -1;
    }
    slot->asset = a;

    ++s_dialog_stack_n;
    return s_dialog_stack_n;
}

/* DialogStackPop Iterates slots from BOTTOM
 * to TOP (matches original loop direction) restoring entity state +
 * freeing per-slot asset. Resets stack count to 0 (clear all). */
static void DialogStackPop(void)
{
    for (int i = 0; i < s_dialog_stack_n; ++i) {
        DialogStackSlot *slot = &s_dialog_stack[i];
        Entity *e = slot->entity;
        if (e) {
            uint8_t *eb = (uint8_t *)e;
            /* atlas restore — write atlas backup into
 * entity[+0x28]. We skip the size-realloc branch (original
 * checks new_atlas dims vs entity bitmap bytes and reallocs
 * via ); on restore the backup atlas was the
 * speaker's original, so dims match what the entity already
 * carries — no realloc needed in 99% of cases. If the dialog
 * asset was larger and the bitmap was grown during dialog,
 * the restored atlas now references a smaller frame range —
 * still works, just over-allocated buffer (benign). */
            *(uint32_t *)(eb + 0x28) = slot->atlas_backup;
            /* walker-state reset (). */
            *(uint16_t *)(eb + 0x3A) &= (uint16_t)0xFFFAu;
            *(uint16_t *)(eb + 0x38) = 0;
            *(uint16_t *)(eb + 0x36) = 0;
            *(uint16_t *)(eb + 0x34) = 0;
            *(uint16_t *)(eb + 0x3C) = 0;
            *(uint16_t *)(eb + 0x42) = 0;
            *(uint16_t *)(eb + 0x40) = 0;
            *(uint16_t *)(eb + 0x32) = 0;
            *(uint32_t *)(eb + 0x50) = 0;
            *(uint32_t *)(eb + 0x4C) = 0;
            /* Clear pc + restore bytecode. */
            *(uint16_t *)(eb + 0x30) = 0;
            *(uint32_t *)(eb + 0x2C) = slot->bytecode_backup;
        }
        if (slot->asset) {
            FreeAsset(slot->asset);
            slot->asset = NULL;
        }
        slot->entity = NULL;
    }
    s_dialog_stack_n = 0;
}

/* T20 —:
 * op 0x52 = PUSH only (entity backups + asset load)
 * op 0x53 (+ ) = play [sampl] WAVs + pop
 *
 * Lines come from Gadki.scr [rozmowa]<result_key> section. Each entry
 * is a [sampl] tag with the WAV filename, optionally followed by [eb]/
 * [fj]/[nic] speaker + text for the speech bubble. result_key is the
 * op 0x53 arg, chosen by script's IF-chain on var[4]. */
static void dialog_play_section_lines(uint16_t actor, const char *section_name)
{
    if (!section_name || !*section_name) return;
    if (!g_dialogues_obj) return;
    if (!ScriptObjFindSection(g_dialogues_obj, "[rozmowa]",
                              section_name, "[animacja]"))
    {
        /*: silent return if section missing. */
        fprintf(stderr, "[dialog] section '%s' not in Gadki.scr — skip\n",
                section_name);
        return;
    }
    const uint8_t *ss = ScriptObjGetSectionStart(g_dialogues_obj);
    const uint8_t *se = ScriptObjGetSectionEnd  (g_dialogues_obj);
    if (!ss || !se) return;

    /* Iterate over [sampl] blocks ( outer loop).
 * Each block: WAV filename + optional speaker text bubble. */
    char wav[64];
    char text[256];
    char speaker = 0;
    int played = 0;
    for (int i = 0; i < 64; ++i) {
        if (!dialog_extract_line(&ss, se, wav, sizeof wav,
                                 text, sizeof text, &speaker)) break;
        /* Speaker actor verb — Ebek=0x29? Fjej=0x2A? — placeholder
 * "actor" arg keeps the existing speech-balloon positioning
 * (centred on screen) since we don't yet thread the speaker
 * verb through DialogStackPush. T20c future: pass speaker
 * verb from g_dialog_stack[top].entity click-payload. */
        (void)speaker;
        fprintf(stderr, "[dialog] line %d wav='%s' speaker=%c text='%s'\n",
                i + 1, wav[0] ? wav : "(none)",
                speaker ? speaker : '-',
                text[0] ? text : "(none)");
        dialog_play_line_indexed(actor, text, wav, i + 1);
        ++played;
    }
    fprintf(stderr, "[dialog] section '%s' played %d sampl blocks\n",
            section_name, played);
}

/* T20b debug — set while a dialog is open (between op 0x52 push and
 * op 0x53 pop). HandleSceneInput + DispatchClickEvent annotate logs
 * with [dlg] tag when this is set, so we can see exactly which click
 * the user makes during the choice picker. */
uint8_t g_dialog_active = 0;

/* T103 — Solund-menu non-audio gates. Set by SolundClick + restored
 * by ApplySavedSettings on boot. */
uint8_t g_subtitles_on = 1;       /* fade_step mirror */
uint8_t g_dialogues_on = 1;       /* fade_progress mirror — gates op 0x52/0x53 */

int ScriptCallDialogBegin(uint16_t actor, const char *dialog_name,
                          const uint8_t *opts, uint32_t talk_anim_va)
{
    /* T103 — gate on g_dialogues_on. Original op 0x52
 * @ 0x00408BC8 wraps the call in `if (fade_progress != 0)`.
 * If dialogues are disabled in Solund, we no-op the entire op. */
    if (!g_dialogues_on) {
        fprintf(stderr, "[dlg] op 0x52 BEGIN suppressed (dialogues_on=0)\n");
        return 0;
    }
    fprintf(stderr, "[dlg] op 0x52 BEGIN actor=0x%04X asset=%s talk_anim_va=0x%08X\n",
            actor, dialog_name ? dialog_name : "(null)", talk_anim_va);
    g_stats.total_dialogs++;                /* T56 */
    g_dialog_active = 1;

    if (!dialog_name || !*dialog_name) return 0;

    /* op 0x52 = PUSH only ( ). T108 — pass talk_anim_va
 * (= op 0x52's 4th arg / PE VA of mouth-cycle bytecode) so
 * DialogActivateTopSpeaker can bind it to entity[+0x2C] later. */
    extern Entity *FindEntityByVerbId(uint16_t verb);
    Entity *speaker = FindEntityByVerbId(actor);
    DialogStackPush(speaker, dialog_name, opts, talk_anim_va);
    return 0;
}

void ScriptCallDialogEnd(const char *result)
{
    /* T103 — gate on g_dialogues_on, z op 0x53 wrap @ 0x00408C28. */
    if (!g_dialogues_on) {
        fprintf(stderr, "[dlg] op 0x53 END suppressed (dialogues_on=0)\n");
        return;
    }
    /* op 0x53 = play lines from [rozmowa]<result> + pop (
 * + ). */
    fprintf(stderr, "[dlg] op 0x53 END result=%s (stack=%d) var[4]=0x%04X\n",
            result ? result : "(null)", s_dialog_stack_n,
            (unsigned)(g_script_vars[4] & 0xFFFF));

    /* Speaker actor verb from the top of dialog stack. The dialog
 * stack stores Entity*, but ScriptCallShowText needs a verb_id
 * for balloon positioning. For now pass actor=0 — balloon centres
 * on screen instead of speaker (acceptable until we extract the
 * verb from the entity's click payload). */
    dialog_play_section_lines(0, result);
    DialogStackPop();
    if (s_dialog_stack_n == 0) g_dialog_active = 0;
}
