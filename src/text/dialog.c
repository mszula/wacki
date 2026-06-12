/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/text/dialog.c — op 0x52 DialogBegin / op 0x53 DialogEnd.
 *
 * Two main jobs:
 *
 *   1. Per-conversation stack management. op 0x52 pushes a slot
 *      capturing the speaker entity's current atlas + bytecode +
 *      walker state; op 0x53 pops the stack and restores them.
 *      Nested dialogs are supported (DIALOG_STACK_MAX deep).
 *
 *   2. Per-line dispatch. After op 0x53 the runner walks the
 *      [rozmowa]<result> section of Gadki.scr, extracts each [sampl]
 *      block (WAV filename + optional [eb]/[fj]/[nic] speaker tag +
 *      speech text), plays the line via PlayDialogLine +
 *      ScriptCallShowText, and waits for both audio + text to finish
 *      before moving on. While the line is playing the speaker
 *      entity's atlas + bytecode are swapped to the dialog's
 *      talk-anim chain so the per-entity VM can run the mouth
 *      animation natively; both are restored after.
 *
 * The speech balloon itself (ScriptCallShowText + TickSpeechBalloon)
 * lives in src/text/balloon.c — this module reuses it for the per-
 * line text display.
 *
 * Globals defined here that other modules read:
 *   g_dialog_active   — non-zero between op 0x52 push and op 0x53 pop
 *                       (HandleSceneInput + DispatchClickEvent annotate
 *                       logs with [dlg] when set)
 *   g_subtitles_on /  — Solund-menu gates; mirrored from g_save.settings
 *   g_dialogues_on      on boot by ApplySavedSettings.
 */

#include "wacki.h"
#include "wacki/log.h"
#include "entity_offsets.h"

#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern uint32_t       ent_ptr_intern(void *p);
extern const void    *xlat_binary_ptr(uint32_t addr);
extern AnimAsset     *LoadAssetFromDtaBase(const char *name);

extern int           ScriptObjFindSection(void *self_, const char *tag,
                                          const char *param,
                                          const char *altparam);
extern const uint8_t *ScriptObjGetSectionStart(void *self);
extern const uint8_t *ScriptObjGetSectionEnd  (void *self);

extern void          ScriptCallShowText(uint16_t actor, const char *text);
extern Entity       *FindEntityByVerbId(uint16_t verb);

/* ---- constants ---------------------------------------------------- */

#define DIALOG_STACK_MAX            8
#define DIALOG_MAX_LINES_PER_SECTION 64
#define DIALOG_WAV_NAME_MAX         64
#define DIALOG_TEXT_LINE_MAX        256

/* Per-line wait-loop budget in frames (loop sleeps ~33 ms / iter so
 * 1800 ≈ 60 s of audio safety, 600 ≈ 20 s of text-only safety). */
#define DIALOG_AUDIO_FRAME_BUDGET   1800
#define DIALOG_TEXT_FRAME_BUDGET    600
/* Grace before checking IsDialogLinePlaying — mixer needs a few frames
 * to transition the channel into "playing" after PlayDialogLine
 * returns. Without this the audio-only path breaks out on frame 0. */
#define DIALOG_AUDIO_GRACE_FRAMES   5
/* Main-loop pacing — matches play_demo_scene's ~30 Hz cadence. */
#define DIALOG_FRAME_DELAY_MS       33

/* ---- [sampl] block extractor -------------------------------------- */

/* Extract the next [sampl] block from a [rozmowa]<name> section.
 *
 * Gadki.scr [rozmowa] sections have NO inline dialog text — the audio
 * file IS the dialog content. Each line is structured as:
 *
 *   [sampl] fjgut04a.wav
 *   [fj]    Optional spoken text (speaker = fj = Fjej)
 *   [sampl] fjgut04b.wav
 *   [eb]    Another line (speaker = eb = Ebek)
 *   [nic]   ← silent / no text
 *
 * 1. Find next [sampl] tag
 * 2. Copy filename after it (skipping whitespace, until next ws)
 * 3. Scan forward up to the *next* [sampl] for [eb]/[fj]/[nic] and
 *    pull the matching speech text
 *
 * An earlier port treated lines as inline text and bailed on the
 * first '[' character. That's why log lines used to read
 * "section 'X' played 0 lines" — every section opens with [sampl],
 * which instantly terminated the scan.
 *
 * Returns 1 if a [sampl] block was extracted (out_wav populated;
 * out_speaker = 'e' / 'f' / 0; out_text = matching bubble or empty).
 * Returns 0 at end of section. */
static int dialog_extract_line(const uint8_t **pss, const uint8_t *se,
                               char *out_wav, size_t out_wav_sz,
                               char *out_text, size_t out_text_sz,
                               char *out_speaker)
{
    const uint8_t *ss = *pss;
    if (out_wav)     out_wav[0]     = 0;
    if (out_text)    out_text[0]    = 0;
    if (out_speaker) *out_speaker   = 0;

    static const char  SAMPL_TAG[] = "[sampl]";
    static const size_t SAMPL_LEN  = sizeof SAMPL_TAG - 1;

    /* ---- find next [sampl] tag ------------------------------------ */
    const uint8_t *sampl_at = NULL;
    while (ss + SAMPL_LEN <= se) {
        if (ss[0] == '[' && memcmp(ss, SAMPL_TAG, SAMPL_LEN) == 0) {
            sampl_at = ss;
            break;
        }
        ++ss;
    }
    if (!sampl_at) { *pss = se; return 0; }

    /* ---- copy WAV filename ---------------------------------------- *
     * Skip whitespace, take until ws/EOL. Lowercase for DTA lookup —
     * archive names are stored lowercase even though the lookup is
     * case-sensitive. */
    const uint8_t *p = sampl_at + SAMPL_LEN;
    while (p < se && (*p == ' ' || *p == '\t')) ++p;
    size_t n = 0;
    while (p < se && *p != ' ' && *p != '\t' &&
           *p != '\r' && *p != '\n' && n + 1 < out_wav_sz) {
        out_wav[n++] = (char)*p++;
    }
    out_wav[n] = 0;
    for (size_t i = 0; i < n; ++i) {
        if (out_wav[i] >= 'A' && out_wav[i] <= 'Z') out_wav[i] += 32;
    }

    /* ---- look for [eb] / [fj] / [nic] up to the next [sampl] ------ */
    const uint8_t *block_end = se;
    {
        const uint8_t *q = p;
        while (q + SAMPL_LEN <= se) {
            if (q[0] == '[' && memcmp(q, SAMPL_TAG, SAMPL_LEN) == 0) {
                block_end = q;
                break;
            }
            ++q;
        }
    }

    while (p < block_end) {
        while (p < block_end && (*p == ' ' || *p == '\t' ||
                                 *p == '\r' || *p == '\n')) ++p;
        if (p >= block_end) break;
        if (*p != '[') { ++p; continue; }

        char   sp      = 0;
        size_t tag_len = 0;
        if (p + 4 <= block_end && memcmp(p, "[eb]", 4) == 0) {
            sp = 'e'; tag_len = 4;
        } else if (p + 4 <= block_end && memcmp(p, "[fj]", 4) == 0) {
            sp = 'f'; tag_len = 4;
        } else if (p + 5 <= block_end && memcmp(p, "[nic]", 5) == 0) {
            /* Silent block — no speaker, no text. Consume + bail. */
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
        while (p < block_end && (*p == ' ' || *p == '\t')) ++p;
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

/* ---- DialogStackSlot ---------------------------------------------- */

/* Original layout: each pushed slot is 0x18 bytes inside a dynamically
 * grown vector. Layout per slot:
 *   +0x00 entity ptr          — speaker entity
 *   +0x04 opts_hash           — horner-folded opts bytes (used by an
 *                               internal lookup; unused in our partial
 *                               port but stored for fidelity)
 *   +0x08 loaded asset ptr    — LoadAssetFromDtaBase(dialog_name)
 *   +0x0C talk_anim_va        — PE virtual addr of mouth-cycle bytecode
 *   +0x10 atlas backup        — entity[+ATLAS_SLOT] at push time
 *   +0x14 bytecode backup     — entity[+BYTECODE_SLOT] at push time
 *
 * The original engine's dialog stack lives in RunScriptInterpreter's
 * local_15c (per-invocation). Our port uses a process-global stack —
 * ScriptCallDialogBegin and ScriptCallDialogEnd are split across RSI
 * invocations because the port's runner sequencer is more linear.
 * Nested dialogs ARE supported (push twice, pop pops the top one). */
typedef struct DialogStackSlot {
    Entity    *entity;          /* speaker entity (NULL if not resolved) */
    AnimAsset *asset;           /* loaded dialog asset (FreeAsset on pop) */
    uint32_t   opts_hash;       /* horner fold of opts bytes — port fidelity */
    uint32_t   talk_anim_va;    /* PE VA of mouth-cycle bytecode (op 0x52's
                                 * 4th arg). Bound to entity[+BYTECODE_SLOT]
                                 * by Activate; restored by Restore. */
    uint32_t   atlas_backup;    /* entity[+ATLAS_SLOT] at push time */
    uint32_t   bytecode_backup; /* entity[+BYTECODE_SLOT] at push time */
} DialogStackSlot;

static DialogStackSlot s_dialog_stack[DIALOG_STACK_MAX];
static int             s_dialog_stack_n = 0;

/* ---- walker / VM reset helper ------------------------------------- *
 * Mirrors the original walker-state reset that both Activate, Restore,
 * and Pop perform on the speaker entity. Clears walker accumulators,
 * targets, delays, and the FRAME_READY + WALKER_FRESH state bits so
 * the entity's per-entity VM restarts from pc=0 cleanly. */
static void dialog_reset_speaker_state(Entity *e)
{
    EOFF(e, ENT_OFF_STATE_FLAGS, uint16_t) &=
        (uint16_t)~(ESTATE_FRAME_READY | ESTATE_WALKER_FRESH);
    EOFF(e, ENT_OFF_LOOP_C,        uint16_t) = 0;
    EOFF(e, ENT_OFF_LOOP_B,        uint16_t) = 0;
    EOFF(e, ENT_OFF_LOOP_A,        uint16_t) = 0;
    EOFF(e, ENT_OFF_DELAY,         uint16_t) = 0;
    EOFF(e, ENT_OFF_LOOP_E,        uint16_t) = 0;
    EOFF(e, ENT_OFF_LOOP_D,        uint16_t) = 0;
    EOFF(e, ENT_OFF_PC,            uint16_t) = 0;
    EOFF(e, ENT_OFF_WALKER_DY_REM, uint32_t) = 0;
    EOFF(e, ENT_OFF_WALKER_DX_REM, uint32_t) = 0;
}

/* ---- per-line audio + balloon playback ---------------------------- */

/* Forward decls — definitions sit next to DialogStackSlot since they
 * share the per-slot state. */
static void DialogActivateTopSpeaker(void);
static void DialogRestoreTopSpeaker(void);

/* Play one dialog line:
 *  - `wav_name` is the .wav filename extracted from a [sampl] tag.
 *    NULL or empty = silent line (text-only).
 *  - `text` can be empty for silent ([nic]) lines or [sampl]-only
 *    entries.  If both are absent we return immediately.
 *
 * Wait loop polls both: text dismiss-tick AND dialog audio playing
 * flag. LMB during playback cancels the line.
 *
 * Earlier ports built the WAV name as `<section_name><idx>.wav`, but
 * actual Gadki.scr names don't follow that convention (e.g. section
 * `fj_gut04` → wav `fjgut04a.wav`), so the .wav is now taken straight
 * from the [sampl] tag. */
static void dialog_play_line(uint16_t actor, const char *text,
                             const char *wav_name)
{
    int has_text  = text     && *text;
    int has_audio = wav_name && *wav_name;
    if (!has_text && !has_audio) return;

    if (has_text) ScriptCallShowText(actor, text);

    int audio_started = 0;
    if (has_audio) {
        uint32_t len = PlayDialogLine(wav_name);
        if (len > 0) audio_started = 1;
    }

    /* Bind the speaker entity to the dialog atlas + mouth-cycle
     * bytecode. The per-entity VM (ExecEntityScript) advances frames
     * natively from the bytecode each tick — no manual mouth toggle
     * needed. DialogRestoreTopSpeaker swaps back on line end. */
    DialogActivateTopSpeaker();

    extern Entity   *g_speech_balloon;
    extern uint8_t   g_lmb_clicked;
    extern uint16_t  g_speech_dismiss_ticks;

    int safety      = audio_started ? DIALOG_AUDIO_FRAME_BUDGET
                                    : DIALOG_TEXT_FRAME_BUDGET;
    int audio_grace = audio_started ? DIALOG_AUDIO_GRACE_FRAMES : 0;

    while (safety-- > 0) {
        if (has_text && !g_speech_balloon) break;
        if (!has_text && audio_grace == 0 && !IsDialogLinePlaying()) break;
        if (audio_grace > 0) --audio_grace;

        ProcessGameFrameTick();
        if (PlatformShouldQuit()) break;
        if (g_game_over_code) break;
        if (g_lmb_clicked) {
            g_lmb_clicked = 0;
            g_speech_dismiss_ticks = 0;
            StopDialogLine();              /* cancel mid-line speech */
            ProcessGameFrameTick();
            break;
        }
        /* Lip-sync: if audio is the timing source, wait until both
         * audio finishes AND the text dismiss-timer expires. If audio
         * finished first, fast-forward the text so the line snaps
         * away on next tick. */
        if (audio_started && !IsDialogLinePlaying()) {
            g_speech_dismiss_ticks = 0;
            ProcessGameFrameTick();
            break;
        }
        EnginePaceFrame(DIALOG_FRAME_DELAY_MS);
    }
    /* Flip atlas back to the speaker's original pose so they don't
     * stay frozen in talking-head form between lines. */
    DialogRestoreTopSpeaker();
}

/* ---- Activate / Restore top speaker ------------------------------- *
 *
 * Activate: bind the loaded dialog asset to entity[+ATLAS_SLOT] and
 * the mouth-cycle bytecode (talk_anim_va, resolved via xlat_binary_ptr)
 * to entity[+BYTECODE_SLOT]. The per-entity VM then animates the
 * mouth frames at the original cadence.
 *
 * Restore: reverse — write the atlas + bytecode backups taken at push
 * time back into the entity. */
static void DialogActivateTopSpeaker(void)
{
    if (s_dialog_stack_n == 0) return;
    DialogStackSlot *slot = &s_dialog_stack[s_dialog_stack_n - 1];
    Entity *e = slot->entity;
    if (!e || !slot->asset) return;

    EOFF(e, ENT_OFF_ATLAS_SLOT, uint32_t) = ent_ptr_intern(slot->asset);
    dialog_reset_speaker_state(e);
    if (slot->talk_anim_va) {
        const void *bc = xlat_binary_ptr(slot->talk_anim_va);
        if (bc) {
            EOFF(e, ENT_OFF_BYTECODE_SLOT, uint32_t) =
                ent_ptr_intern((void *)bc);
        }
    }
    EOFF(e, ENT_OFF_FRAME, uint16_t) = 0;
}

static void DialogRestoreTopSpeaker(void)
{
    if (s_dialog_stack_n == 0) return;
    DialogStackSlot *slot = &s_dialog_stack[s_dialog_stack_n - 1];
    Entity *e = slot->entity;
    if (!e) return;

    EOFF(e, ENT_OFF_ATLAS_SLOT, uint32_t) = slot->atlas_backup;
    dialog_reset_speaker_state(e);
    EOFF(e, ENT_OFF_BYTECODE_SLOT, uint32_t) = slot->bytecode_backup;
    EOFF(e, ENT_OFF_FRAME,         uint16_t) = 0;
}

/* ---- DialogStackPush / Pop ---------------------------------------- */

/* Push: allocate asset, store speaker entity backups. Returns the
 * stack depth after push, or -1 on overflow / asset load failure
 * (matches original: only commits the slot if LoadAssetFromDtaBase
 * succeeded). */
static int DialogStackPush(Entity *speaker, const char *dialog_name,
                           const uint8_t *opts_bytes,
                           uint32_t talk_anim_va)
{
    if (s_dialog_stack_n >= DIALOG_STACK_MAX) {
        LOG_TRACE("dialog", "push: stack full (%d)", s_dialog_stack_n);
        return -1;
    }
    if (!speaker) {
        LOG_TRACE("dialog", "push: NULL speaker — skip");
        return -1;
    }

    DialogStackSlot *slot = &s_dialog_stack[s_dialog_stack_n];
    slot->entity       = speaker;
    slot->talk_anim_va = talk_anim_va;

    /* Horner fold over opts (original: `h = h*2 + b`). */
    uint32_t h = 0;
    if (opts_bytes) {
        for (const uint8_t *p = opts_bytes; *p; ++p) h = h * 2u + *p;
    }
    slot->opts_hash = h;

    /* Back up atlas + bytecode slots (intern handles — match the
     * script-byte layout). Even if some are 0 (no current atlas) we
     * still record. */
    slot->atlas_backup    = EOFF(speaker, ENT_OFF_ATLAS_SLOT,    uint32_t);
    slot->bytecode_backup = EOFF(speaker, ENT_OFF_BYTECODE_SLOT, uint32_t);

    /* Original commits the slot ONLY if load succeeded; we mirror
     * that — on failure, don't bump count. */
    AnimAsset *a = LoadAssetFromDtaBase(dialog_name);
    if (!a) {
        LOG_TRACE("dialog", "push: asset '%s' load failed — skip", dialog_name ? dialog_name : "(null)");
        return -1;
    }
    slot->asset = a;

    ++s_dialog_stack_n;
    return s_dialog_stack_n;
}

/* Pop: iterate slots from BOTTOM to TOP (matches original loop
 * direction) restoring entity state + freeing per-slot asset. Resets
 * stack count to 0 (clear all).
 *
 * Atlas restore skips the size-realloc branch the original does
 * (check new_atlas dims vs entity bitmap bytes, realloc if larger):
 * on restore the backup atlas was the speaker's original, so dims
 * match what the entity already carries — no realloc needed in
 * practice. If the dialog asset was larger and the bitmap was grown
 * during dialog, the restored atlas references a smaller frame range,
 * which still works (the backing buffer is just over-allocated). */
static void DialogStackPop(void)
{
    for (int i = 0; i < s_dialog_stack_n; ++i) {
        DialogStackSlot *slot = &s_dialog_stack[i];
        Entity *e = slot->entity;
        if (e) {
            EOFF(e, ENT_OFF_ATLAS_SLOT, uint32_t) = slot->atlas_backup;
            dialog_reset_speaker_state(e);
            EOFF(e, ENT_OFF_FRAME,         uint16_t) = 0;
            EOFF(e, ENT_OFF_BYTECODE_SLOT, uint32_t) = slot->bytecode_backup;
        }
        if (slot->asset) {
            FreeAsset(slot->asset);
            slot->asset = NULL;
        }
        slot->entity = NULL;
    }
    s_dialog_stack_n = 0;
}

/* ---- per-section line runner -------------------------------------- *
 *
 * op 0x52       = push only (entity backups + asset load)
 * op 0x53 (+..) = play [sampl] WAVs + pop
 *
 * Lines come from the Gadki.scr [rozmowa]<result_key> section. Each
 * entry is a [sampl] tag with the WAV filename, optionally followed
 * by [eb] / [fj] / [nic] speaker + text for the speech bubble.
 * `result_key` is the op 0x53 argument, chosen by the script's
 * IF-chain on var[4]. */
static void dialog_play_section_lines(uint16_t actor,
                                      const char *section_name)
{
    if (!section_name || !*section_name) return;
    if (!g_dialogues_obj) return;
    if (!ScriptObjFindSection(g_dialogues_obj, "[rozmowa]",
                              section_name, "[animacja]"))
    {
        LOG_TRACE("dialog", "section '%s' not in Gadki.scr — skip", section_name);
        return;
    }
    const uint8_t *ss = ScriptObjGetSectionStart(g_dialogues_obj);
    const uint8_t *se = ScriptObjGetSectionEnd  (g_dialogues_obj);
    if (!ss || !se) return;

    char wav[DIALOG_WAV_NAME_MAX];
    char text[DIALOG_TEXT_LINE_MAX];
    char speaker = 0;
    int  played  = 0;
    for (int i = 0; i < DIALOG_MAX_LINES_PER_SECTION; ++i) {
        if (!dialog_extract_line(&ss, se, wav, sizeof wav,
                                 text, sizeof text, &speaker)) break;
        /* TODO: the speaker actor verb (Ebek=0x29? Fjej=0x2A?) isn't
         * threaded through DialogStackPush yet, so we pass actor=0
         * and the speech balloon centres on screen rather than over
         * the speaker. Future: pull it from the stack-top entity's
         * click payload. */
        (void)speaker;
        LOG_TRACE("dialog", "line %d wav='%s' speaker=%c text='%s'", i + 1, wav[0] ? wav : "(none)", speaker ? speaker : '-', text[0] ? text : "(none)");
        dialog_play_line(actor, text, wav);
        ++played;
    }
    LOG_TRACE("dialog", "section '%s' played %d sampl blocks", section_name, played);
}

/* ---- public state + ops ------------------------------------------- */

/* Set while a dialog is open (between op 0x52 push and op 0x53 pop).
 * HandleSceneInput + DispatchClickEvent annotate logs with [dlg] tag
 * when this is set, so we can see exactly which click the user makes
 * during the choice picker. */
uint8_t g_dialog_active = 0;

/* T103 — Solund-menu non-audio gates. Set by SolundClick + restored
 * by ApplySavedSettings on boot. */
uint8_t g_subtitles_on = 1;       /* fade_step mirror */
uint8_t g_dialogues_on = 1;       /* fade_progress mirror — gates op 0x52/0x53 */

int ScriptCallDialogBegin(uint16_t actor, const char *dialog_name,
                          const uint8_t *opts, uint32_t talk_anim_va)
{
    /* T103 — gate on g_dialogues_on. Original op 0x52 wraps the call
     * in `if (fade_progress != 0)`. If dialogues are disabled in
     * Solund, no-op the entire op. */
    if (!g_dialogues_on) {
        LOG_TRACE("dlg", "op 0x52 BEGIN suppressed (dialogues_on=0)");
        return 0;
    }
    LOG_TRACE("dlg", "op 0x52 BEGIN actor=0x%04X asset=%s talk_anim_va=0x%08X", actor, dialog_name ? dialog_name : "(null)", talk_anim_va);
    g_stats.total_dialogs++;
    g_dialog_active = 1;

    if (!dialog_name || !*dialog_name) return 0;

    Entity *speaker = FindEntityByVerbId(actor);
    DialogStackPush(speaker, dialog_name, opts, talk_anim_va);
    return 0;
}

void ScriptCallDialogEnd(const char *result)
{
    if (!g_dialogues_on) {
        LOG_TRACE("dlg", "op 0x53 END suppressed (dialogues_on=0)");
        return;
    }
    LOG_TRACE("dlg", "op 0x53 END result=%s (stack=%d) var[4]=0x%04X", result ? result : "(null)", s_dialog_stack_n, (unsigned)(g_script_vars[4] & 0xFFFF));

    /* Pass actor=0 so the balloon centres on screen — see the TODO
     * in dialog_play_section_lines. */
    dialog_play_section_lines(0, result);
    DialogStackPop();
    if (s_dialog_stack_n == 0) g_dialog_active = 0;
}
