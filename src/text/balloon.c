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

/* ---- module constants --------------------------------------------- *
 *
 * Where a number appears more than once in this file, it gets a name. */

#define SPEECH_TEXT_MAX        256       /* working buffer for translated text */
#define SPEECH_LINES_MAX        10       /* max `|`-delimited lines per balloon */
#define SPEECH_FONT_COLOR     0xFD       /* indexed-palette colour for glyph fg */
#define SPEECH_LINE_HEIGHT      30       /* Futura.30 line advance, px */
#define SPEECH_MIN_WIDTH        32       /* tiny balloons need a base width */
#define SPEECH_SCREEN_MARGIN     8       /* keep balloon away from screen edges */
#define SPEECH_DEFAULT_Y      0x50       /* fallback Y when no speaker found */

/* Auto-dismiss timer is `text_chars * MS_PER_CHAR + (lines + 4) *
 * LINE_PADDING_MS`, clamped to [MIN_DUR_MS, MAX_DUR_MS]. */
#define SPEECH_MS_PER_CHAR      10
#define SPEECH_LINE_PADDING_MS  0x19     /* = 25 ms (per Ghidra formula) */
#define SPEECH_LINE_PADDING_LINES 4      /* magic +4 in the formula */
#define SPEECH_MIN_DUR_MS       60
#define SPEECH_MAX_DUR_MS     5000

/* Polish CP-1250 → Futura.30 glyph slot translation table size. */
#define POLISH_DIACRITIC_COUNT  18

/* Auto-dismiss timer derived from the original engine's wait-loop
 * counter. Ticked down by frame delta each ProcessGameFrameTick. */
char     g_speech_text[SPEECH_TEXT_MAX] = {0};
uint16_t g_speech_actor                 = 0;
uint32_t g_speech_tick                  = 0;
uint16_t g_speech_dismiss_ticks         = 0;

/* Active speech balloon entity (kind=1 with manual pixel buffer).
 * NULL = no active balloon. EntityListClearAll resets via the same
 * path as g_actor[]. */
Entity *g_speech_balloon = NULL;

/* Speaker animation unbind state — mirrors the original op 0x09
 * epilogue. After the balloon dismisses, the original re-binds the
 * speaker entity to its dialog slot's idle-animation bytecode;
 * without this re-bind the speaker stays frozen in the "talking"
 * frame.
 *
 * The port stores the bind args here at op 0x09 time;
 * TickSpeechBalloon applies them when the dismiss timer hits zero
 * (= original wait-loop exit). */
uint16_t g_speech_unbind_speaker = 0;
uint32_t g_speech_unbind_data    = 0;

/* T117 — Polish-diacritic → Futura-glyph translation table.
 *
 * The original engine maps 18 CP-1250 Polish characters to custom
 * Futura.30 glyph slots at indices 0xC2..0xFB. All other bytes pass
 * through identity. Source pairs lifted byte-for-byte from the PE
 * (CP-1250 source table → Futura slot target table).
 *
 * Without this LUT, op 0x09 SHOW_TEXT would either render Polish
 * chars as wrong glyphs (whatever lives at the CP-1250 codepoint in
 * Futura.30 — possibly garbage) or as blanks (if outside the font's
 * first..last_char range).
 *
 * Called once at engine boot (PreloadCommonAssets tail) so it's
 * ready before any op 0x09 fires. */
static uint8_t g_text_translation_lut[256];
static int     g_text_lut_built = 0;

void TextTranslationLutInit(void)
{
    if (g_text_lut_built) return;
    /* Identity mapping for all 256 bytes. */
    for (int i = 0; i < 256; ++i) g_text_translation_lut[i] = (uint8_t)i;
    /* Override 18 entries — Polish diacritics → Futura slots. */
    static const uint8_t cp1250_src[POLISH_DIACRITIC_COUNT] = {
        0xA5, 0xC6, 0xCA, 0xA3, 0xD1, 0xD3, 0x8C, 0xAF, 0x8F,   /* Ą Ć Ę Ł Ń Ó Ś Ż Ź */
        0xB9, 0xE6, 0xEA, 0xB3, 0xF1, 0xF3, 0x9C, 0x9F, 0xBF    /* ą ć ę ł ń ó ś ź ż */
    };
    static const uint8_t futura_dst[POLISH_DIACRITIC_COUNT] = {
        0xC2, 0xCA, 0xCB, 0xCE, 0xCF, 0xD3, 0xD4, 0xDB, 0xDA,
        0xE2, 0xEA, 0xEB, 0xEE, 0xEF, 0xF3, 0xF4, 0xFA, 0xFB
    };
    for (int i = 0; i < POLISH_DIACRITIC_COUNT; ++i) {
        g_text_translation_lut[cp1250_src[i]] = futura_dst[i];
    }
    g_text_lut_built = 1;
}

/* T117 — translate input text via LUT into output buffer. Stops on
 * NUL in the TRANSLATED stream (an override mapping a char to 0x00
 * would terminate early; the Polish-diacritic LUT never does this so
 * we're safe in practice). */
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

/* ScriptCallShowText is the back-end of script opcode 0x09 SHOW_TEXT.
 *
 * NOTE: dead code in the shipped game. Op 0x09 is implemented in the
 * engine but no script in any of the 5 stages emits it; character
 * dialogue runs through op 0x52/0x53 instead (Polish voice-only via
 * Gadki.scr [sampl] tags). Kept for fidelity in case unreached scripts
 * fire it. If you see a "[say]" log line, the entity-render path will
 * display the balloon.
 *
 * Pipeline:
 *   1. Translate CP-1250 → Futura glyph indices
 *   2. Split text on '|' into lines (≤ SPEECH_LINES_MAX)
 *   3. Measure width per line, derive balloon bbox
 *   4. Free any previous balloon, allocate kind=1 entity for new one
 *   5. Render each line into the entity's pixel buffer
 *   6. Position above the speaker (or fallback to centered)
 *   7. Link into render list, arm dismissal timer
 */

/* Split `text` on '|' separators into up to SPEECH_LINES_MAX entries
 * pointing into `buf` (which holds a mutable copy). Returns line count.
 * Each line is NUL-terminated in-place. */
static int split_text_lines(const char *text, char *buf, size_t buf_sz,
                            char **lines)
{
    strncpy(buf, text, buf_sz - 1);
    buf[buf_sz - 1] = 0;

    int   count = 0;
    char *p     = buf;
    while (count < SPEECH_LINES_MAX) {
        lines[count++] = p;
        char *bar = strchr(p, '|');
        if (!bar) break;
        *bar = 0;
        p = bar + 1;
    }
    return count;
}

/* Measure each line's pixel width. Sets *out_max_w / *out_total_h to
 * the balloon's overall dimensions (clamped to screen). */
static void measure_balloon(FontHandle *font, char **lines, int count,
                            int *line_w, int *out_max_w, int *out_total_h)
{
    int max_w = 0;
    for (int i = 0; i < count; ++i) {
        line_w[i] = MeasureTextLine(font, (const uint8_t *)lines[i]);
        if (line_w[i] > max_w) max_w = line_w[i];
    }
    if (max_w < SPEECH_MIN_WIDTH) max_w = SPEECH_MIN_WIDTH;
    if (max_w > WACKI_SCREEN_W - SPEECH_SCREEN_MARGIN) {
        max_w = WACKI_SCREEN_W - SPEECH_SCREEN_MARGIN;
    }

    int total_h = count * SPEECH_LINE_HEIGHT;
    if (total_h < SPEECH_LINE_HEIGHT) total_h = SPEECH_LINE_HEIGHT;

    *out_max_w   = max_w;
    *out_total_h = total_h;
}

/* Free the currently-displayed balloon entity (if any) so a new one
 * can take its place. The engine keeps at most one balloon active. */
static void clear_speech_balloon(void)
{
    if (!g_speech_balloon) return;

    UnlinkEntity(g_speech_balloon);
    if (g_speech_balloon->pixels) xfree(g_speech_balloon->pixels);
    xfree(g_speech_balloon);
    g_speech_balloon = NULL;
}

/* Render every text line into the balloon entity's pixel buffer,
 * centered horizontally per-line. */
static void render_balloon_lines(Entity *e, FontHandle *font,
                                 char **lines, int count,
                                 const int *line_w, int max_w)
{
    for (int i = 0; i < count; ++i) {
        int cx = (max_w - line_w[i]) / 2;
        if (cx < 0) cx = 0;
        TextRenderTarget td = {
            .stride     = (uint16_t)max_w,
            .x          = (uint16_t)cx,
            .color_base = SPEECH_FONT_COLOR,
            .pixels     = e->pixels + (size_t)(i * SPEECH_LINE_HEIGHT) * max_w,
            .font       = font,
        };
        RenderTextLineToBuffer(&td, (const uint8_t *)lines[i]);
    }
}

/* Decide where on screen the balloon goes. If the speaker entity is
 * findable (via verb_id), position above its draw rect. Otherwise
 * fallback to horizontally-centered at a fixed Y. Clamps to screen. */
static void position_balloon(uint16_t actor, int max_w, int total_h,
                             int *out_x, int *out_y)
{
    extern Entity *FindEntityByVerbId(uint16_t verb);
    Entity *spk = FindEntityByVerbId(actor);

    int bx, by;
    if (spk) {
        int16_t  sx_x = EOFF(spk, ENT_OFF_DRAWN_X, int16_t);
        int16_t  sx_y = EOFF(spk, ENT_OFF_DRAWN_Y, int16_t);
        uint16_t sx_w = EOFF(spk, ENT_OFF_WIDTH,   uint16_t);
        bx = (int)sx_x + ((int)sx_w - max_w) / 2;
        by = (int)sx_y - total_h;
    } else {
        bx = (WACKI_SCREEN_W - max_w) / 2;
        by = SPEECH_DEFAULT_Y;
    }
    if (bx < 0) bx = 0;
    if (bx + max_w > WACKI_SCREEN_W) bx = WACKI_SCREEN_W - max_w;
    if (by < 0) by = 0;

    *out_x = bx;
    *out_y = by;
}

/* The dismissal duration in ms is derived from text length + line
 * count, then clamped. The original engine's wait-loop counter has the
 * same shape; we compute the semantic equivalent directly because our
 * buffer addresses don't match the original's fixed address arithmetic. */
static int compute_dismiss_duration(size_t char_count, int line_count)
{
    int dur = (int)char_count * SPEECH_MS_PER_CHAR
            + (line_count + SPEECH_LINE_PADDING_LINES) * SPEECH_LINE_PADDING_MS;
    if (dur < SPEECH_MIN_DUR_MS) dur = SPEECH_MIN_DUR_MS;
    if (dur > SPEECH_MAX_DUR_MS) dur = SPEECH_MAX_DUR_MS;
    return dur;
}

void ScriptCallShowText(uint16_t actor, const char *text)
{
    extern uint32_t    g_tick_counter;
    extern FontHandle *g_default_font;

    if (!text || !*text || !g_default_font) return;

    /* Settings can disable visible subtitles (audio still plays via a
     * separate path if voice_on is set). */
    if (!g_subtitles_on) {
        fprintf(stderr, "[say] suppressed (subtitles_on=0): %.60s\n", text);
        return;
    }

    char translated[SPEECH_TEXT_MAX];
    translate_script_text(text, translated, sizeof translated);
    text = translated;
    fprintf(stderr, "[say] actor=%u: %.120s\n", actor, text);

    /* Layout: split into lines, measure, compute bounding box. */
    char  buf[SPEECH_TEXT_MAX];
    char *lines[SPEECH_LINES_MAX];
    int   line_count = split_text_lines(text, buf, sizeof buf, lines);

    int line_w[SPEECH_LINES_MAX] = {0};
    int max_w, total_h;
    measure_balloon(g_default_font, lines, line_count, line_w, &max_w, &total_h);

    /* Free previous balloon; allocate kind=1 entity for this one. */
    clear_speech_balloon();
    Entity *e = AllocEntity((uint16_t)max_w, (uint16_t)total_h, 1, 1);
    if (!e || !e->pixels) {
        if (e) { if (e->pixels) xfree(e->pixels); xfree(e); }
        return;
    }
    memset(e->pixels, 0, (size_t)max_w * (size_t)total_h);

    /* Render lines into the entity's pixel buffer, then place on screen. */
    render_balloon_lines(e, g_default_font, lines, line_count, line_w, max_w);

    int bx, by;
    position_balloon(actor, max_w, total_h, &bx, &by);

    EOFF(e, ENT_OFF_DRAWN_X,  int16_t) = (int16_t)bx;
    EOFF(e, ENT_OFF_DRAWN_Y,  int16_t) = (int16_t)by;
    EOFF(e, ENT_OFF_ANCHOR_X, int16_t) = (int16_t)bx;
    EOFF(e, ENT_OFF_ANCHOR_Y, int16_t) = (int16_t)by;
    EOFF(e, ENT_OFF_FOOT_Y,   int16_t) = (int16_t)(by + total_h);

    LinkEntityToList(&g_render_list_head, e, 0);
    g_speech_balloon = e;

    /* Arm the dismissal timer. */
    g_speech_text[0]        = 0;                       /* disable legacy overlay */
    g_speech_actor          = actor;
    g_speech_tick           = g_tick_counter;
    g_speech_dismiss_ticks  = (uint16_t)compute_dismiss_duration(strlen(text),
                                                                 line_count);
}

/* Re-bind a speaker entity to a fresh bytecode block (e.g. its idle
 * animation) and reset all walker / loop / delay state so the speaker
 * stops mid-talk and returns to neutral. Used by TickSpeechBalloon to
 * close out a dialog line. */
static void rebind_speaker_to_bytecode(Entity *sp, const void *bytecode)
{
    EOFF(sp, ENT_OFF_STATE_FLAGS,   uint16_t) &=
        (uint16_t)~(ESTATE_FRAME_READY | ESTATE_WALKER_FRESH);
    EOFF(sp, ENT_OFF_LOOP_A,        uint16_t) = 0;
    EOFF(sp, ENT_OFF_LOOP_B,        uint16_t) = 0;
    EOFF(sp, ENT_OFF_LOOP_C,        uint16_t) = 0;
    EOFF(sp, ENT_OFF_LOOP_D,        uint16_t) = 0;
    EOFF(sp, ENT_OFF_LOOP_E,        uint16_t) = 0;
    EOFF(sp, ENT_OFF_DELAY,         uint16_t) = 0;
    EOFF(sp, ENT_OFF_PC,            uint16_t) = 0;
    EOFF(sp, ENT_OFF_FRAME,         uint16_t) = 0;
    EOFF(sp, ENT_OFF_WALKER_DX_REM, uint32_t) = 0;
    EOFF(sp, ENT_OFF_WALKER_DY_REM, uint32_t) = 0;
    EOFF(sp, ENT_OFF_BYTECODE_SLOT, uint32_t) = ent_ptr_intern((void *)bytecode);
}

/* TickSpeechBalloon — drains the dismissal timer once per game tick.
 * When the balloon expires, also fires the speaker-unbind handshake
 * armed by op 0x09 so the speaker returns to their idle anim. */
void TickSpeechBalloon(void)
{
    extern uint32_t g_tick_counter;
    extern Entity *FindEntityByVerbId(uint16_t verb);

    if (!g_speech_balloon) return;
    if ((g_tick_counter - g_speech_tick) < g_speech_dismiss_ticks) return;

    /* Time's up — tear down the balloon. The render list owns the
     * pointer, so we have to GC explicitly. */
    clear_speech_balloon();

    /* Optional speaker walker-bytecode unbind, armed by op 0x09. */
    if (g_speech_unbind_speaker != 0 && g_speech_unbind_data != 0) {
        Entity *sp = FindEntityByVerbId(g_speech_unbind_speaker);
        if (sp) {
            const void *bc = xlat_binary_ptr(g_speech_unbind_data);
            if (bc) rebind_speaker_to_bytecode(sp, bc);
        }
        g_speech_unbind_speaker = 0;
        g_speech_unbind_data    = 0;
    }
}

