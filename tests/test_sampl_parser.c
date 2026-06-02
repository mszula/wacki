/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_sampl_parser.c — Wacky.scr [sampl] tuple parser coverage.
 *
 * The parser in src/audio/sfx.c (ParseSamplTagsForKomnata) walks the
 * section text and registers (asset, frame_start, frame_end, wav)
 * entries in a private dynamic_sfx table. Three tuple shapes from the
 * original engine:
 *
 *   (N,)   = play trigger      → frame_start=N,  frame_end=NONE,  wav=W
 *   (N,M)  = play + loop end   → frame_start=N,  frame_end=M,     wav=W
 *   (,M)   = stop-only trigger → frame_start=-1, frame_end=M,     wav=NULL
 *
 * Stop-only entries are the recent fix that makes Ebek's headphone-
 * rock idle loop work: the (,1) (,25) (,48) (,49) (,61) tuples in the
 * `[komnata] init` section terminate the random Muzik05-09 pool
 * triggered at frame 95. Earlier the parser dropped them and the loop
 * had no stop trigger, so it either played once and stopped (without
 * looping) or kept playing forever (with explicit loop). */

#include "test.h"
#include "wacki.h"

#include <stdint.h>
#include <string.h>

/* Public parser entry (src/audio/sfx.c). */
extern void ParseSamplTagsForKomnata(const uint8_t *start, const uint8_t *end);
extern void ResetDynamicSfxTable(void);

/* Test-only accessors over the parser's output (sfx.c bottom). */
extern int         sfx_test_dynamic_count(void);
extern const char *sfx_test_dynamic_asset(int idx);
extern int         sfx_test_dynamic_frame_start(int idx);
extern int         sfx_test_dynamic_frame_end(int idx);
extern const char *sfx_test_dynamic_wav(int idx);

#define SFX_FRAME_END_NONE  0xFFFF

/* Helper: feed a NUL-terminated literal through the parser. */
static void parse(const char *text)
{
    ResetDynamicSfxTable();
    ParseSamplTagsForKomnata((const uint8_t *)text,
                             (const uint8_t *)text + strlen(text));
}

/* Helper: find the index of the first entry matching (asset, wav,
 * frame_start). Returns -1 on miss. */
static int find_entry(const char *asset, const char *wav,
                      int frame_start)
{
    int n = sfx_test_dynamic_count();
    for (int i = 0; i < n; ++i) {
        const char *a = sfx_test_dynamic_asset(i);
        const char *w = sfx_test_dynamic_wav(i);
        if (!a || strcmp(a, asset) != 0) continue;
        if (sfx_test_dynamic_frame_start(i) != frame_start) continue;
        if (wav == NULL) { if (w == NULL) return i; else continue; }
        if (!w || strcmp(w, wav) != 0) continue;
        return i;
    }
    return -1;
}

/* ---- play triggers ---------------------------------------------------- */

TEST(single_play_trigger_no_end)
{
    parse("[animacja] ebek.wyc\n"
          "[sampl] step.wav (10,)\n");
    ASSERT_EQ(sfx_test_dynamic_count(), 1);
    int i = find_entry("ebek.wyc", "step.wav", 10);
    ASSERT_TRUE(i >= 0);
    ASSERT_EQ(sfx_test_dynamic_frame_end(i), SFX_FRAME_END_NONE);
}

TEST(play_trigger_with_explicit_end_is_loop)
{
    parse("[animacja] rakieta.wyc\n"
          "[sampl] marsz.wav (1,464)\n");
    ASSERT_EQ(sfx_test_dynamic_count(), 1);
    int i = find_entry("rakieta.wyc", "marsz.wav", 1);
    ASSERT_TRUE(i >= 0);
    ASSERT_EQ(sfx_test_dynamic_frame_end(i), 464);
}

TEST(multiple_starts_one_wav_each_gets_an_entry)
{
    parse("[animacja] fjej.wyc\n"
          "[sampl] kr.wav (5,) (29,) (49,) (64,)\n");
    /* 4 start frames × 1 wav = 4 entries. */
    ASSERT_EQ(sfx_test_dynamic_count(), 4);
    ASSERT_TRUE(find_entry("fjej.wyc", "kr.wav", 5)  >= 0);
    ASSERT_TRUE(find_entry("fjej.wyc", "kr.wav", 29) >= 0);
    ASSERT_TRUE(find_entry("fjej.wyc", "kr.wav", 49) >= 0);
    ASSERT_TRUE(find_entry("fjej.wyc", "kr.wav", 64) >= 0);
}

/* ---- random pool: multiple wavs sharing one start frame -------------- */

TEST(random_pool_registers_each_wav_at_same_start)
{
    parse("[animacja] ebek.wyc\n"
          "[sampl] a.wav b.wav c.wav (40,)\n");
    /* 1 start × 3 wavs = 3 entries, all sharing frame_start=40. */
    ASSERT_EQ(sfx_test_dynamic_count(), 3);
    ASSERT_TRUE(find_entry("ebek.wyc", "a.wav", 40) >= 0);
    ASSERT_TRUE(find_entry("ebek.wyc", "b.wav", 40) >= 0);
    ASSERT_TRUE(find_entry("ebek.wyc", "c.wav", 40) >= 0);
}

/* ---- stop-only triggers (the regression fix) ------------------------- */

TEST(stop_only_tuple_registers_with_negative_start_and_null_wav)
{
    parse("[animacja] ebek.wyc\n"
          "[sampl] muzik.wav (95,) (,61)\n");
    /* Expected: 1 play entry + 1 stop-only entry. */
    ASSERT_EQ(sfx_test_dynamic_count(), 2);

    int play = find_entry("ebek.wyc", "muzik.wav", 95);
    ASSERT_TRUE(play >= 0);
    ASSERT_EQ(sfx_test_dynamic_frame_end(play), SFX_FRAME_END_NONE);

    int stop = find_entry("ebek.wyc", NULL, -1);
    ASSERT_TRUE(stop >= 0);
    ASSERT_EQ(sfx_test_dynamic_frame_end(stop), 61);
    ASSERT_NULL(sfx_test_dynamic_wav(stop));
}

TEST(headphone_rock_pattern_full_fidelity)
{
    /* The actual Wacky.scr line that motivated the parser rewrite.
     * 5 wavs share start frame 95; 5 stop-only triggers at 1/25/48/49/61
     * terminate whichever wav from the pool ended up playing. */
    parse("[animacja] ebek.wyc\n"
          "[sampl] Muzik05.wav Muzik06.wav Muzik07.wav Muzik08.wav Muzik09.wav "
          "(95,) (,1) (,25) (,48) (,49) (,61)\n");

    /* 5 play entries (one per wav at frame 95) + 5 stop-only entries. */
    ASSERT_EQ(sfx_test_dynamic_count(), 10);

    /* All five wavs should have a play entry at frame 95.
     * Names lowercase — parser normalises. */
    ASSERT_TRUE(find_entry("ebek.wyc", "muzik05.wav", 95) >= 0);
    ASSERT_TRUE(find_entry("ebek.wyc", "muzik06.wav", 95) >= 0);
    ASSERT_TRUE(find_entry("ebek.wyc", "muzik07.wav", 95) >= 0);
    ASSERT_TRUE(find_entry("ebek.wyc", "muzik08.wav", 95) >= 0);
    ASSERT_TRUE(find_entry("ebek.wyc", "muzik09.wav", 95) >= 0);

    /* Five stop-only triggers, each frame_start=-1, wav=NULL, with the
     * specific end frames. Iterate through ALL stop-only entries to
     * collect the end frames seen. */
    int n = sfx_test_dynamic_count();
    int seen_1 = 0, seen_25 = 0, seen_48 = 0, seen_49 = 0, seen_61 = 0;
    for (int i = 0; i < n; ++i) {
        if (sfx_test_dynamic_frame_start(i) != -1) continue;
        if (sfx_test_dynamic_wav(i) != NULL) continue;
        int fe = sfx_test_dynamic_frame_end(i);
        if (fe ==  1) seen_1  = 1;
        if (fe == 25) seen_25 = 1;
        if (fe == 48) seen_48 = 1;
        if (fe == 49) seen_49 = 1;
        if (fe == 61) seen_61 = 1;
    }
    ASSERT_TRUE(seen_1);
    ASSERT_TRUE(seen_25);
    ASSERT_TRUE(seen_48);
    ASSERT_TRUE(seen_49);
    ASSERT_TRUE(seen_61);
}

TEST(stop_only_without_start_does_not_attach_to_a_wav)
{
    parse("[animacja] x.wyc\n"
          "[sampl] only.wav (,5)\n");
    /* The wav is listed but no play trigger, only a stop-only tuple.
     * The parser still registers the stop-only entry but with wav=NULL
     * (it doesn't bind to a specific wav). The shipped scripts never
     * use this shape, but the parser shouldn't crash on it. */
    ASSERT_EQ(sfx_test_dynamic_count(), 1);
    int i = find_entry("x.wyc", NULL, -1);
    ASSERT_TRUE(i >= 0);
    ASSERT_EQ(sfx_test_dynamic_frame_end(i), 5);
}

/* ---- empty / malformed input ----------------------------------------- */

TEST(parse_no_animacja_section_no_entries)
{
    /* Without a preceding [animacja] tag the parser has no current
     * asset to attach entries to — they're dropped. */
    parse("[sampl] orphan.wav (1,)\n");
    ASSERT_EQ(sfx_test_dynamic_count(), 0);
}

TEST(parse_two_animacja_blocks_each_track_owns_its_entries)
{
    parse("[animacja] ebek.wyc\n"
          "[sampl] a.wav (10,)\n"
          "[animacja] fjej.wyc\n"
          "[sampl] b.wav (20,)\n");
    ASSERT_EQ(sfx_test_dynamic_count(), 2);
    ASSERT_TRUE(find_entry("ebek.wyc", "a.wav", 10) >= 0);
    ASSERT_TRUE(find_entry("fjej.wyc", "b.wav", 20) >= 0);
}

TEST(parse_stops_at_next_komnata_section)
{
    /* Once the parser hits `[komnata]` it bails — entries for the
     * next komnata are not registered into this block's table. */
    parse("[animacja] a.wyc\n"
          "[sampl] x.wav (1,)\n"
          "[komnata] init\n"
          "[animacja] b.wyc\n"
          "[sampl] y.wav (2,)\n");
    ASSERT_EQ(sfx_test_dynamic_count(), 1);
    ASSERT_TRUE(find_entry("a.wyc", "x.wav", 1) >= 0);
}

SUITE(sampl_parser)
{
    RUN_TEST(single_play_trigger_no_end);
    RUN_TEST(play_trigger_with_explicit_end_is_loop);
    RUN_TEST(multiple_starts_one_wav_each_gets_an_entry);
    RUN_TEST(random_pool_registers_each_wav_at_same_start);
    RUN_TEST(stop_only_tuple_registers_with_negative_start_and_null_wav);
    RUN_TEST(headphone_rock_pattern_full_fidelity);
    RUN_TEST(stop_only_without_start_does_not_attach_to_a_wav);
    RUN_TEST(parse_no_animacja_section_no_entries);
    RUN_TEST(parse_two_animacja_blocks_each_track_owns_its_entries);
    RUN_TEST(parse_stops_at_next_komnata_section);
}
