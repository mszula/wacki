/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * tests/test_sampl_parser.c — Wacky.scr [sampl] tuple parser coverage.
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
extern int  FindKomnataBgMusic(const uint8_t *start, const uint8_t *end,
                               char out[][KOMNATA_BG_MUSIC_NAME_MAX], int max);

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

/* ---- room-level BG music: FindKomnataBgMusic ------------------------- *
 * The looping room ambience is the [sampl] track(s) listed BEFORE the
 * komnata section's first [animacja]/[rozmowa] — NOT a Tlo_<etap>_<id>a
 * formula. These cases mirror the real Wacky.scr layouts. */

/* Helper: run FindKomnataBgMusic over a literal section. */
static int find_music(const char *text,
                      char out[][KOMNATA_BG_MUSIC_NAME_MAX], int max)
{
    return FindKomnataBgMusic((const uint8_t *)text,
                              (const uint8_t *)text + strlen(text), out, max);
}

TEST(bg_music_single_room_level_sampl)
{
    /* etap-1 kiosk21: one room track, then prop animations. */
    char m[KOMNATA_BG_MUSIC_MAX_TRACKS][KOMNATA_BG_MUSIC_NAME_MAX];
    int n = find_music("[sampl] Tlo_1_3a.wav\n"
                       "[animacja] kioskarz.wyc\n"
                       "[sampl] mowi.wav (3,)\n", m, KOMNATA_BG_MUSIC_MAX_TRACKS);
    ASSERT_EQ(n, 1);
    ASSERT_STREQ(m[0], "tlo_1_3a.wav");   /* lowercased */
}

TEST(bg_music_multiple_variants_form_the_pool)
{
    /* etap-1 maluch: three room tracks the engine layers + loops together. */
    char m[KOMNATA_BG_MUSIC_MAX_TRACKS][KOMNATA_BG_MUSIC_NAME_MAX];
    int n = find_music("[sampl] Tlo_1_1a.wav\n"
                       "[sampl] Tlo_1_1b.wav\n"
                       "[sampl] Tlo_1_1c.wav\n"
                       "[animacja] baranek.wyc\n"
                       "[sampl] mee.wav (1,)\n", m, KOMNATA_BG_MUSIC_MAX_TRACKS);
    ASSERT_EQ(n, 3);
    ASSERT_STREQ(m[0], "tlo_1_1a.wav");
    ASSERT_STREQ(m[1], "tlo_1_1b.wav");
    ASSERT_STREQ(m[2], "tlo_1_1c.wav");
}

TEST(bg_music_etap2_short_names)
{
    /* The actual bug: etap-2 rooms name their track Tlo1a.wav (no
     * _etap_komnata_ infix) — the old formula's Tlo_2_1a.wav never
     * matched, so the room was silent. The data-driven path picks it up. */
    char m[KOMNATA_BG_MUSIC_MAX_TRACKS][KOMNATA_BG_MUSIC_NAME_MAX];
    int n = find_music("[sampl] Tlo1a.wav\n"
                       "[animacja] kamera.wyc\n"
                       "[ sampl] CamKlik2.wav (31,)\n", m, KOMNATA_BG_MUSIC_MAX_TRACKS);
    ASSERT_EQ(n, 1);
    ASSERT_STREQ(m[0], "tlo1a.wav");
}

TEST(bg_music_stops_at_first_animacja)
{
    /* [animacja]-scoped [sampl] are prop SFX, never room music. */
    char m[KOMNATA_BG_MUSIC_MAX_TRACKS][KOMNATA_BG_MUSIC_NAME_MAX];
    int n = find_music("[sampl] room.wav\n"
                       "[animacja] a.wyc\n"
                       "[sampl] sfx1.wav (1,)\n"
                       "[sampl] sfx2.wav (2,)\n", m, KOMNATA_BG_MUSIC_MAX_TRACKS);
    ASSERT_EQ(n, 1);
    ASSERT_STREQ(m[0], "room.wav");
}

TEST(bg_music_stops_at_rozmowa_boundary)
{
    char m[KOMNATA_BG_MUSIC_MAX_TRACKS][KOMNATA_BG_MUSIC_NAME_MAX];
    int n = find_music("[sampl] amb.wav\n"
                       "[rozmowa] dlg\n"
                       "[sampl] notmusic.wav (1,)\n", m, KOMNATA_BG_MUSIC_MAX_TRACKS);
    ASSERT_EQ(n, 1);
    ASSERT_STREQ(m[0], "amb.wav");
}

TEST(bg_music_none_when_animacja_is_first)
{
    /* A room that opens straight into an [animacja] has no room music. */
    char m[KOMNATA_BG_MUSIC_MAX_TRACKS][KOMNATA_BG_MUSIC_NAME_MAX];
    int n = find_music("[animacja] a.wyc\n"
                       "[sampl] sfx.wav (1,)\n", m, KOMNATA_BG_MUSIC_MAX_TRACKS);
    ASSERT_EQ(n, 0);
}

TEST(bg_music_respects_max_cap)
{
    char m[2][KOMNATA_BG_MUSIC_NAME_MAX];
    int n = find_music("[sampl] one.wav\n"
                       "[sampl] two.wav\n"
                       "[sampl] three.wav\n", m, 2);
    ASSERT_EQ(n, 2);
    ASSERT_STREQ(m[0], "one.wav");
    ASSERT_STREQ(m[1], "two.wav");
}

TEST(bg_music_mixed_prefix_tracks_all_collected)
{
    /* etap-4 chatacz layers two Tlo tracks with a Tamtamy (drum) track —
     * different prefixes, all room-level, all kept. Regression guard for the
     * "play only one track" bug: every layer must survive extraction so the
     * caller can play them together (e.g. klatka2's piano was one such layer). */
    char m[KOMNATA_BG_MUSIC_MAX_TRACKS][KOMNATA_BG_MUSIC_NAME_MAX];
    int n = find_music("[sampl] Tlo_4_4a.wav\n"
                       "[sampl] Tlo_4_4b.wav\n"
                       "[sampl] Tamtamy2.wav\n"
                       "[animacja] tubylec.wyc\n"
                       "[sampl] krzyk.wav (2,)\n", m, KOMNATA_BG_MUSIC_MAX_TRACKS);
    ASSERT_EQ(n, 3);
    ASSERT_STREQ(m[0], "tlo_4_4a.wav");
    ASSERT_STREQ(m[1], "tlo_4_4b.wav");
    ASSERT_STREQ(m[2], "tamtamy2.wav");
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
    RUN_TEST(bg_music_single_room_level_sampl);
    RUN_TEST(bg_music_multiple_variants_form_the_pool);
    RUN_TEST(bg_music_etap2_short_names);
    RUN_TEST(bg_music_stops_at_first_animacja);
    RUN_TEST(bg_music_stops_at_rozmowa_boundary);
    RUN_TEST(bg_music_none_when_animacja_is_first);
    RUN_TEST(bg_music_respects_max_cap);
    RUN_TEST(bg_music_mixed_prefix_tracks_all_collected);
}
