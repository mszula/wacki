/* src/text/balloon.c — speech balloon rendering + dialog text.
 *
 * The largest single domain extracted from stubs.c. Three concerns:
 *
 * 1. Text-translation LUT (TextTranslationLutInit): build a glyph
 * remap table that converts Polish accented characters to their
 * ASCII fallbacks for fonts that don't carry the full character
 * set.
 *
 * 2. Speech balloon (ScriptCallShowText + TickSpeechBalloon):
 * script opcode 0x09 SHOW_TEXT path. Builds a kind=1 entity that
 * carries the rendered glyphs, positions it above the speaker,
 * and arms a dismiss timer based on text length. The per-frame
 * TickSpeechBalloon advances the dismiss countdown and tears
 * down the entity when it expires.
 *
 * 3. Dialog end (ScriptCallDialogEnd): op 0x53 tail — pops the
 * dialog stack, restores the panel verb table, and signals the
 * result_key to the calling script.
 *
 * The balloon and dialog state live in module-static globals
 * (g_speech_balloon, g_speech_tick, g_speech_dismiss_ticks,
 * g_dialog_active, etc.).
 */

#include "wacki.h"
#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void   *xmalloc(uint32_t sz);
extern void    xfree  (void *p);
extern uint32_t ent_ptr_intern(void *p);
extern const void *xlat_binary_ptr(uint32_t addr);

/* Auto-dismiss timer derived from the original engine's wait-loop
 * counter. Ticked down by frame delta each ProcessGameFrameTick. */
char     g_speech_text[256]      = {0};
uint16_t g_speech_actor          = 0;
uint32_t g_speech_tick           = 0;
uint16_t g_speech_dismiss_ticks  = 0;

/* Active speech balloon entity (kind=1 with manual pixel buffer).
 * NULL = no active balloon. EntityListClearAll resets via the same
 * path as g_actor[]. */
Entity *g_speech_balloon = NULL;

/* Speaker animation unbind state — mirrors the original op 0x09 epilogue
 * (RunScriptInterpreter case 9, line ~480 of ):
 *
 * if (local_164 < dlg_count) {
 * (slot[local_164].speaker, slot[local_164].DATA);
 * }
 *
 * The original re-binds the speaker entity to the dialog slot's `data`
 * bytecode (idle animation) AFTER the balloon dismisses. Without it the
 * speaker stays frozen in the "talking" frame.
 *
 * Port stores the bind args here at op 0x09 time; TickSpeechBalloon
 * applies them when the timer hits zero (= original wait-loop exit). */
uint16_t g_speech_unbind_speaker = 0;
uint32_t g_speech_unbind_data    = 0;

/* T117 — Polish-diacritic → Futura-glyph translation table (1:1 with
 * init @ 0x0040C740). The original engine maps 18 CP-1250
 * Polish characters to custom Futura.30 glyph slots @ indices 0xC2..0xFB.
 * Identity for all other bytes. Source pairs lifted from
 * DAT_00445E40 (source CP-1250) + DAT_00445E58 (target Futura slot) in PE.
 *
 * Without this LUT, op 0x09 SHOW_TEXT would either:
 * (a) render Polish chars as wrong glyphs (whatever lives at CP-1250
 * codepoint in Futura.30 — possibly garbage), or
 * (b) render them as blanks (if outside font's first..last_char range).
 *
 * Called once at engine boot (PreloadCommonAssets tail) so it's ready
 * before any op 0x09 fires. */
static uint8_t g_text_translation_lut[256];
static int     g_text_lut_built = 0;

void TextTranslationLutInit(void)
{
    if (g_text_lut_built) return;
    /* Identity mapping for all 256 bytes (1:1 z head). */
    for (int i = 0; i < 256; ++i) g_text_translation_lut[i] = (uint8_t)i;
    /* Override 18 entries — Polish diacritics → Futura slots. Table
 * lifted byte-for-byte from PE: source @ 0x00445E40, target @
 * 0x00445E58 (each 18 bytes). */
    static const uint8_t src[18] = {
        0xA5, 0xC6, 0xCA, 0xA3, 0xD1, 0xD3, 0x8C, 0xAF, 0x8F,   /* Ą Ć Ę Ł Ń Ó Ś Ż Ź */
        0xB9, 0xE6, 0xEA, 0xB3, 0xF1, 0xF3, 0x9C, 0x9F, 0xBF    /* ą ć ę ł ń ó ś ź ż */
    };
    static const uint8_t dst[18] = {
        0xC2, 0xCA, 0xCB, 0xCE, 0xCF, 0xD3, 0xD4, 0xDB, 0xDA,
        0xE2, 0xEA, 0xEB, 0xEE, 0xEF, 0xF3, 0xF4, 0xFA, 0xFB
    };
    for (int i = 0; i < 18; ++i) g_text_translation_lut[src[i]] = dst[i];
    g_text_lut_built = 1;
}

/* T117 — translate input text via LUT into output buffer. 1:1 with
 *. Stops on NUL byte in TRANSLATED stream
 * (an override mapping a char to 0x00 would terminate early — the
 * Polish-diacritic LUT never does this so we're safe in practice). */
static void translate_script_text(const char *in, char *out, size_t out_sz)
{
    if (!in || !out || out_sz < 1) { if (out_sz) out[0] = 0; return; }
    TextTranslationLutInit();
    size_t n = 0;
    while (n + 1 < out_sz) {
        uint8_t c = (uint8_t)in[n];
        if (c == 0) break;
        uint8_t t = g_text_translation_lut[c];
        if (t == 0) break;       /* mirrors original early-out on translated NUL */
        out[n++] = (char)t;
    }
    out[n] = 0;
}

/* DEAD CODE IN SHIPPED GAME (verified empirically 2026-05-28).
 *
 * Op 0x09 SHOW_TEXT is fully implemented in WACKI.EXE (RunScriptInterpreter
 * case 9 @ 0x00407820) but no script in any of the 5 stages actually emits
 * the opcode. All in-game character dialogue goes through op 0x52/0x53
 * (DialogStackPush / DialogRunner reading [sampl] audio-only entries from
 * Gadki.scr — Polish voice acting, no subtitles).
 *
 * Likely a leftover from pre-dub development when text placeholders were
 * shown over speakers. Kept 1:1 for fidelity + in case any unreached
 * scripts in stages 4/5 actually fire it. If you ever see a "[say]" log
 * line, the kind=1 entity render path in EntityRenderAll will display it. */
void ScriptCallShowText(uint16_t actor, const char *text)
{
    extern uint32_t g_tick_counter;
    extern FontHandle *g_default_font;
    if (!text || !*text || !g_default_font) return;
    /* T103 — gate on g_subtitles_on. When disabled in
 * Solund menu, op 0x09 SHOW_TEXT no-ops (audio still plays via
 * separate path if voice_on is set; only the visible balloon is
 * suppressed). */
    if (!g_subtitles_on) {
        fprintf(stderr, "[say] suppressed (subtitles_on=0): %.60s\n", text);
        return;
    }
    /* T117 — translate raw CP-1250 input via Polish-diacritic LUT before
 * any layout/render. op 0x09 dispatch calling
 * (text) → DAT_00475950 (translated buffer). */
    char translated[256];
    translate_script_text(text, translated, sizeof translated);
    text = translated;
    fprintf(stderr, "[say] actor=%u: %.120s\n", actor, text);

    /* 1:1 with op 0x09 (RunScriptInterpreter case 9):
 *
 * 1. copies the text into work buffer.
 * 2. Split on '|' into up to 10 lines (local_d4[]).
 * 3. Measure each line width via (font, line),
 * track max in uVar24, set total height = lines * font.advance.
 * 4. piVar22 = AllocEntity(maxW, totalH, kind=1, 1).
 * 5. Zero the pixel buffer.
 * 6. Set up render desc DAT_004549E8..:
 * stride=maxW height=totalH font=DAT_0045500C color=0xFD
 * and render each line centred at (maxW - line_w)/2.
 * 7. (speaker_id) → speaker click entity.
 * If kind=1/2: X = drawX + (w - text_w)/2, Y = drawY - text_h.
 * Else: Y=0x50, X centred.
 * 8. Clamp X to [0, screen_w - text_w], Y to >= 0.
 * 9. (DAT_0045147C, balloon) — link to render list.
 * 10. Wait loop ticks down DAT_00455004 by g_frame_delta_ms per
 * ProcessGameFrameTick; exit when zero or DAT_0044E5AC click.
 * 11. Set balloon hidden bit (+0x09 & 0x80) on exit. */

    /* --- 1: copy text + split on '|' --- */
    char buf[256];
    strncpy(buf, text, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    char *lines[10];
    int   line_count = 0;
    char *p = buf;
    while (line_count < 10) {
        lines[line_count++] = p;
        char *bar = strchr(p, '|');
        if (!bar) break;
        *bar = 0;
        p = bar + 1;
    }

    /* --- 2: measure widths via MeasureTextLine (= 
 * 1:1 port — per-glyph width_tab + kern_tab sums). --- */
    int line_w[10] = {0};
    int max_w = 0;
    for (int i = 0; i < line_count; ++i) {
        line_w[i] = MeasureTextLine(g_default_font, (const uint8_t *)lines[i]);
        if (line_w[i] > max_w) max_w = line_w[i];
    }
    if (max_w < 32) max_w = 32;
    if (max_w > WACKI_SCREEN_W - 8) max_w = WACKI_SCREEN_W - 8;
    int total_h = line_count * 30;       /* Futura.30 advance ≈ 30 */
    if (total_h < 30) total_h = 30;

    /* --- 3: AllocEntity kind=1 with primary plane (group_flags=1) --- */
    /* Free any previous balloon — original op 0x09 keeps only one at
 * a time (the previous one is hidden via +0x09 |= 0x80 then GC'd). */
    if (g_speech_balloon) {
        UnlinkEntity(g_speech_balloon);
        if (g_speech_balloon->pixels) xfree(g_speech_balloon->pixels);
        xfree(g_speech_balloon);
        g_speech_balloon = NULL;
    }
    Entity *e = AllocEntity((uint16_t)max_w, (uint16_t)total_h, 1, 1);
    if (!e || !e->pixels) {
        if (e) { if (e->pixels) xfree(e->pixels); xfree(e); }
        return;
    }
    memset(e->pixels, 0, (size_t)max_w * (size_t)total_h);

    /* --- 4: render each line into the buffer ---
 * RenderTextLineToBuffer writes at td.pixels + stride*baseline.
 * Per-line Y advance via offsetting base pointer. Color 0xFD
 * (DAT_004549F8 from Ghidra).
 *
 * Native-pointer TextRenderTarget struct (replaces the original's
 * 32-bit uint32_t[5] descriptor) — see wacki.h. The legacy uint32_t
 * array truncated 64-bit pointers and crashed when malloc returned
 * addresses above the 4 GB boundary. */
    for (int i = 0; i < line_count; ++i) {
        int cx = (max_w - line_w[i]) / 2;
        if (cx < 0) cx = 0;
        TextRenderTarget td = {
            .stride     = (uint16_t)max_w,
            .x          = (uint16_t)cx,
            .color_base = 0xFD,
            .pixels     = e->pixels + (size_t)(i * 30) * max_w,
            .font       = g_default_font,
        };
        RenderTextLineToBuffer(&td, (const uint8_t *)lines[i]);
    }

    /* --- 5: position above speaker — 1:1 with case 9 
 * lookup. Use FindEntityByVerbId; if found, position above its
 * draw rect. Else fallback: Y=0x50, X centred. */
    int bx, by;
    extern Entity *FindEntityByVerbId(uint16_t verb);
    Entity *spk = FindEntityByVerbId(actor);
    if (spk) {
        uint8_t *sb = (uint8_t *)spk;
        int16_t sx_x = *(int16_t  *)(sb + 0x0A);
        int16_t sx_y = *(int16_t  *)(sb + 0x0C);
        uint16_t sx_w = *(uint16_t *)(sb + 0x0E);
        bx = (int)sx_x + ((int)sx_w - max_w) / 2;
        by = (int)sx_y - total_h;
    } else {
        bx = (WACKI_SCREEN_W - max_w) / 2;
        by = 0x50;
    }
    if (bx < 0) bx = 0;
    if (bx + max_w > WACKI_SCREEN_W) bx = WACKI_SCREEN_W - max_w;
    if (by < 0) by = 0;

    uint8_t *eb = (uint8_t *)e;
    *(int16_t *)(eb + 0x0A) = (int16_t)bx;
    *(int16_t *)(eb + 0x0C) = (int16_t)by;
    *(int16_t *)(eb + 0x22) = (int16_t)bx;          /* anchor mirror */
    *(int16_t *)(eb + 0x24) = (int16_t)by;
    /* foot_y at +0x26 = drawY + height for z-sort. */
    *(int16_t *)(eb + 0x26) = (int16_t)(by + total_h);

    /* --- 6: link to render list. --- */
    LinkEntityToList(&g_render_list_head, e, 0);
    g_speech_balloon = e;

    /* --- 7: dismissal timer (Ghidra: DAT_00455004 = chars*10 - 0x7D20
 * + (lines+4)*0x19; ticks down by g_frame_delta_ms each frame). --- */
    size_t n_chars = strlen(text);
    /* Dismiss-timer formula — 1:1 with Ghidra case 9:
 *
 * DAT_00455004 = (short)pcVar5 * 10 + -0x7d20 + (lines+4) * 0x19;
 *
 * `pcVar5` is the NUL-terminator pointer (= buffer_start +
 * text_chars). The `(short)pcVar5 * 10 - 0x7D20` term resolves to
 * `text_chars * 10` IF the buffer starts at the original game's
 * fixed address `0x475950` (whose low word 0x5950 = 22864 satisfies
 * `22864 * 10 mod 65536 == 0x7D20`). That calibration cancels the
 * fixed buffer-base contribution leaving the char count.
 *
 * Our heap-allocated buffer doesn't match the original's address,
 * so we compute the semantic equivalent directly:
 * dur (ms) = text_chars * 10 + (lines+4) * 25
 *
 * For 50 chars, 1 line: 500 + 125 = 625 ms.
 * For 200 chars, 2 lines: 2000 + 150 = 2150 ms.
 * Result is read by TickSpeechBalloon each frame and decremented by
 * g_frame_delta_ms (real ms) — when ≤ 0 the balloon is GC'd. */
    int dur = (int)n_chars * 10 + (line_count + 4) * 0x19;
    if (dur < 60)   dur = 60;       /* safety: ensure at least ~1s show */
    if (dur > 5000) dur = 5000;     /* cap absurdly long text at 5s */
    /* Keep legacy fields for the overlay-renderer transition window
 * — they'll be retired once the entity render path is the sole
 * draw site for kind=1 balloons. */
    g_speech_text[0]        = 0;             /* disable old overlay */
    g_speech_actor          = actor;
    g_speech_tick           = g_tick_counter;
    g_speech_dismiss_ticks  = (uint16_t)dur;
}

/* TickSpeechBalloon — drains the dismissal timer (Ghidra DAT_00455004
 * wait loop in case 9). Called once per ProcessGameFrameTick. */
void TickSpeechBalloon(void)
{
    extern uint32_t g_tick_counter;
    extern const void *xlat_binary_ptr(uint32_t);
    extern uint32_t ent_ptr_intern(void *);
    extern Entity *FindEntityByVerbId(uint16_t verb);
    if (!g_speech_balloon) return;
    if ((g_tick_counter - g_speech_tick) >= g_speech_dismiss_ticks) {
        /* Hide + unlink + free. Mirrors `balloon[+9] |= 0x80` then
 * normal entity GC; we GC explicitly since render list owns
 * the pointer. */
        UnlinkEntity(g_speech_balloon);
        if (g_speech_balloon->pixels) xfree(g_speech_balloon->pixels);
        xfree(g_speech_balloon);
        g_speech_balloon = NULL;

        /* Speaker animation UNBIND — case 9 epilogue:
 *
 * if (local_164 < dlg_count) {
 * (slot[local_164].speaker, slot[local_164].data);
 * }
 *
 * Re-bind the speaker to the dlg_data idle-animation bytecode
 * so they return to neutral pose. Identical to op 0x0E body
 *. */
        if (g_speech_unbind_speaker != 0 && g_speech_unbind_data != 0) {
            Entity *sp = FindEntityByVerbId(g_speech_unbind_speaker);
            if (sp) {
                const void *bc = xlat_binary_ptr(g_speech_unbind_data);
                if (bc) {
                    uint8_t *eb = (uint8_t *)sp;
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
                    *(uint32_t *)(eb + 0x2C) = ent_ptr_intern((void *)bc);
                    *(uint16_t *)(eb + 0x30) = 0;
                }
            }
            g_speech_unbind_speaker = 0;
            g_speech_unbind_data    = 0;
        }
    }
}

/* ScriptCallDialogBegin — partial port of (DialogPush) +
 * (DialogRunner) ops 0x52 / 0x53.
 *
 * Full original flow:
 * op 0x52 = — push dialog onto an internal stack with:
 * slot.entity = speaker entity (looked up by reg_id)
 * slot.atlas_backup = entity[+0x28]
 * slot.script_backup = entity[+0x2C]
 * slot.hash = horner-fold(opts bytes)
 * slot.asset = LoadAssetFromDtaBase(dialog_name)
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
 * `wav_name` is the .wav filename extracted from a [sampl] tag (1:1 with
 * — it copies the name into DAT_004550b8 then calls
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
 *
 * void (stack, int mode) {
 * for each slot in stack:
 * if (mode == 0) { // RESTORE
 * (entity, slot[+0x10]); // restore atlas_backup
 * (entity); // walker reset
 * entity[+0x2C] = slot[+0x14]; // restore bytecode_backup
 * } else { // ACTIVATE
 * (entity, slot[+0x08]); // bind dialog asset
 * (entity); // walker reset
 * entity[+0x2C] = slot[+0x0C]; // bind talk_anim bytecode
 * }
 * entity[+0x30] = 0; // reset frame pc
 * }
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
    /* Walker reset (1:1 — same as ACTIVATE path). */
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
            /* walker-state reset (1:1). */
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

/* T20 — 1:1 with Ghidra dialog flow:
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
uint8_t g_subtitles_on = 1;       /* DAT_00455121 mirror */
uint8_t g_dialogues_on = 1;       /* DAT_00455122 mirror — gates op 0x52/0x53 */

int ScriptCallDialogBegin(uint16_t actor, const char *dialog_name,
                          const uint8_t *opts, uint32_t talk_anim_va)
{
    /* T103 — gate on g_dialogues_on. Original op 0x52
 * @ 0x00408BC8 wraps the call in `if (DAT_00455122 != 0)`.
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

    /* op 0x52 = PUSH only (1:1 ). T108 — pass talk_anim_va
 * (= op 0x52's 4th arg / PE VA of mouth-cycle bytecode) so
 * DialogActivateTopSpeaker can bind it to entity[+0x2C] later. */
    extern Entity *FindEntityByVerbId(uint16_t verb);
    Entity *speaker = FindEntityByVerbId(actor);
    DialogStackPush(speaker, dialog_name, opts, talk_anim_va);
    return 0;
}

void ScriptCallDialogEnd(const char *result)
{
    /* T103 — gate on g_dialogues_on, 1:1 z op 0x53 wrap @ 0x00408C28. */
    if (!g_dialogues_on) {
        fprintf(stderr, "[dlg] op 0x53 END suppressed (dialogues_on=0)\n");
        return;
    }
    /* op 0x53 = play lines from [rozmowa]<result> + pop (1:1
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
