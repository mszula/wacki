/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_vm_script_parser.c — .scr text-tagged parser.
 *
 * Wacky.scr / Gadki.scr are text-tagged files with sections like:
 *   [etap]N
 *   [komnata]M
 *   <binary bytecode>
 *   [rozmowa]NAME
 *   <dialog data>
 *   [sampl]
 *   <sound-trigger table>
 *
 * src/script.c exposes three public functions for parsing them:
 *   - LoadScriptFile           — read .scr into a ScriptObj buffer
 *   - FindScriptByStageAndRoom — locate [etap]N + [komnata]M section
 *   - ScriptObjFindSection     — locate any tag/param pair
 *   - ScriptObjGetSection{Start,End} — slice the located section
 *
 * Tests write a hand-crafted .scr fixture to /tmp, load it, exercise
 * lookups, verify the returned slice contains the expected bytes.
 *
 * Reference: src/script.c lines 110-202.
 */

#include "test.h"
#include "wacki.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Public API exposed by src/script.c. */
extern int LoadScriptFile(void *self_, const char *name);
extern int FindScriptByStageAndRoom(void *self_, const char *etap,
                                    const char *komnata);
extern int ScriptObjFindSection(void *self_, const char *tag,
                                const char *param, const char *altparam);
extern const uint8_t *ScriptObjGetSectionStart(void *self_);
extern const uint8_t *ScriptObjGetSectionEnd(void *self_);

/* ScriptObj is private — opaque. The struct as-of script.c:113 holds 4
 * pointers + 1 uint32 = up to 40 bytes on 64-bit. Use a generous 128-byte
 * zero-initialized stack buffer to back it. */
#define SCRIPT_OBJ_BYTES 128

static const char kTmpScr[] = "/tmp/wacki-test-script.scr";

static void write_fixture(const char *content)
{
    FILE *fp = fopen(kTmpScr, "wb");
    if (!fp) return;
    fwrite(content, 1, strlen(content), fp);
    fclose(fp);
}

static void cleanup_fixture(void)
{
    remove(kTmpScr);
}

/* ---- LoadScriptFile via filesystem path -------------------------------- */

TEST(load_from_filesystem_path)
{
    const char *body = "[etap]1\n[komnata]A\nHELLO";
    write_fixture(body);

    uint8_t obj[SCRIPT_OBJ_BYTES] = { 0 };
    int rc = LoadScriptFile(obj, kTmpScr);
    ASSERT_EQ(rc, 1);

    cleanup_fixture();
}

TEST(load_from_nonexistent_path_returns_zero)
{
    /* When fopen fails AND no DTA archive is mounted with this name,
     * LoadScriptFile returns 0. */
    uint8_t obj[SCRIPT_OBJ_BYTES] = { 0 };
    int rc = LoadScriptFile(obj, "/tmp/wacki-no-such-script.scr");
    ASSERT_EQ(rc, 0);
}

/* ---- FindScriptByStageAndRoom: stage+room lookup ---------------------- */

TEST(find_stage_and_room_returns_section)
{
    /* Layout: 3 stage/komnata pairs back-to-back.
     *
     *   [etap]1[komnata]A<BODY-A>
     *   [komnata]B<BODY-B>
     *   [etap]2[komnata]A<BODY-2A>
     */
    const char *body =
        "[etap]1\n[komnata]A\nABCDE\n"
        "[komnata]B\nFGHIJ\n"
        "[etap]2\n[komnata]A\nKLMNO";
    write_fixture(body);

    uint8_t obj[SCRIPT_OBJ_BYTES] = { 0 };
    ASSERT_EQ(LoadScriptFile(obj, kTmpScr), 1);

    /* Find [etap]1 [komnata]B → body "FGHIJ". */
    ASSERT_EQ(FindScriptByStageAndRoom(obj, "1", "B"), 1);
    const uint8_t *start = ScriptObjGetSectionStart(obj);
    const uint8_t *end   = ScriptObjGetSectionEnd(obj);
    ASSERT_NOT_NULL(start);
    ASSERT_NOT_NULL(end);

    /* The section should contain "FGHIJ". start points right AFTER
     * the param "B", end points to the next [komnata] or [etap]. */
    /* Skip leading whitespace. */
    while (start < end && (*start == '\n' || *start == '\r' || *start == ' '))
        ++start;
    /* Compare first 5 chars. */
    ASSERT_TRUE(end - start >= 5);
    ASSERT_EQ(start[0], 'F');
    ASSERT_EQ(start[1], 'G');
    ASSERT_EQ(start[2], 'H');
    ASSERT_EQ(start[3], 'I');
    ASSERT_EQ(start[4], 'J');

    cleanup_fixture();
}

TEST(find_stage_and_room_unknown_room_returns_zero)
{
    const char *body = "[etap]1\n[komnata]A\nABC";
    write_fixture(body);

    uint8_t obj[SCRIPT_OBJ_BYTES] = { 0 };
    ASSERT_EQ(LoadScriptFile(obj, kTmpScr), 1);
    /* Wrong komnata name → 0. */
    ASSERT_EQ(FindScriptByStageAndRoom(obj, "1", "X"), 0);

    cleanup_fixture();
}

TEST(find_stage_and_room_unknown_stage_returns_zero)
{
    const char *body = "[etap]1\n[komnata]A\nABC";
    write_fixture(body);

    uint8_t obj[SCRIPT_OBJ_BYTES] = { 0 };
    ASSERT_EQ(LoadScriptFile(obj, kTmpScr), 1);
    ASSERT_EQ(FindScriptByStageAndRoom(obj, "99", "A"), 0);

    cleanup_fixture();
}

/* ---- ScriptObjFindSection: any tag/param lookup ---------------------- */

TEST(find_section_with_altparam_terminator)
{
    /* Gadki.scr layout has [rozmowa]NAME and [sampl] tags. The optional
     * altparam parameter to FindTagInScript / ScriptObjFindSection tells
     * the parser to terminate the section at EITHER the next matching
     * tag OR the altparam, whichever comes first.
     *
     * Verify: when altparam="[sampl]" is closer than the next "[rozmowa]",
     * the section's end is at the [sampl] marker.
     */
    const char *body =
        "[rozmowa]GREET\nHELLO\n[sampl]\nXYZ\n[rozmowa]BYE\nGOODBYE";
    write_fixture(body);

    uint8_t obj[SCRIPT_OBJ_BYTES] = { 0 };
    ASSERT_EQ(LoadScriptFile(obj, kTmpScr), 1);

    /* Find [rozmowa]GREET, altparam="[sampl]". Section should end at
     * [sampl] (which comes BEFORE the next [rozmowa]). */
    int rc = ScriptObjFindSection(obj, "[rozmowa]", "GREET", "[sampl]");
    ASSERT_EQ(rc, 1);

    const uint8_t *start = ScriptObjGetSectionStart(obj);
    const uint8_t *end   = ScriptObjGetSectionEnd(obj);
    ASSERT_NOT_NULL(start);
    ASSERT_NOT_NULL(end);

    /* The slice should contain "HELLO" but NOT "XYZ" or "BYE". */
    size_t slice_len = (size_t)(end - start);
    /* Convert to a NUL-terminated buffer for easy strstr. */
    char slice[64] = { 0 };
    size_t copy = slice_len < sizeof slice - 1 ? slice_len : sizeof slice - 1;
    memcpy(slice, start, copy);
    ASSERT_TRUE(strstr(slice, "HELLO") != NULL);
    ASSERT_TRUE(strstr(slice, "XYZ") == NULL);
    ASSERT_TRUE(strstr(slice, "BYE") == NULL);

    cleanup_fixture();
}

TEST(find_section_no_altparam_uses_next_same_tag)
{
    /* Without altparam, section ends at the NEXT occurrence of the same
     * tag. Verify [rozmowa]A goes until [rozmowa]B. */
    const char *body =
        "[rozmowa]A\nALPHA\n[rozmowa]B\nBRAVO";
    write_fixture(body);

    uint8_t obj[SCRIPT_OBJ_BYTES] = { 0 };
    ASSERT_EQ(LoadScriptFile(obj, kTmpScr), 1);

    int rc = ScriptObjFindSection(obj, "[rozmowa]", "A", NULL);
    ASSERT_EQ(rc, 1);

    const uint8_t *start = ScriptObjGetSectionStart(obj);
    const uint8_t *end   = ScriptObjGetSectionEnd(obj);
    char slice[64] = { 0 };
    size_t slice_len = (size_t)(end - start);
    size_t copy = slice_len < sizeof slice - 1 ? slice_len : sizeof slice - 1;
    memcpy(slice, start, copy);

    ASSERT_TRUE(strstr(slice, "ALPHA") != NULL);
    ASSERT_TRUE(strstr(slice, "BRAVO") == NULL);

    cleanup_fixture();
}

TEST(find_section_returns_zero_when_tag_missing)
{
    const char *body = "[etap]1\nABC";
    write_fixture(body);

    uint8_t obj[SCRIPT_OBJ_BYTES] = { 0 };
    ASSERT_EQ(LoadScriptFile(obj, kTmpScr), 1);

    /* [rozmowa] doesn't exist in this file. */
    int rc = ScriptObjFindSection(obj, "[rozmowa]", "ANY", NULL);
    ASSERT_EQ(rc, 0);

    cleanup_fixture();
}

TEST(find_section_last_in_file_extends_to_eof)
{
    /* Last section in file has no following tag → end points to EOF. */
    const char *body = "[etap]1\n[komnata]Z\nLASTBODY";
    write_fixture(body);

    uint8_t obj[SCRIPT_OBJ_BYTES] = { 0 };
    ASSERT_EQ(LoadScriptFile(obj, kTmpScr), 1);
    ASSERT_EQ(FindScriptByStageAndRoom(obj, "1", "Z"), 1);

    const uint8_t *start = ScriptObjGetSectionStart(obj);
    const uint8_t *end   = ScriptObjGetSectionEnd(obj);
    /* end should point to start + strlen(body) - position-of-LASTBODY. */
    ASSERT_TRUE(end > start);

    /* Slice should contain "LASTBODY". */
    char slice[64] = { 0 };
    size_t slice_len = (size_t)(end - start);
    size_t copy = slice_len < sizeof slice - 1 ? slice_len : sizeof slice - 1;
    memcpy(slice, start, copy);
    ASSERT_TRUE(strstr(slice, "LASTBODY") != NULL);

    cleanup_fixture();
}

/* ---- accessors with NULL self return NULL ---------------------------- */

TEST(get_section_start_null_returns_null)
{
    ASSERT_NULL(ScriptObjGetSectionStart(NULL));
    ASSERT_NULL(ScriptObjGetSectionEnd(NULL));
}

SUITE(vm_script_parser)
{
    RUN_TEST(load_from_filesystem_path);
    RUN_TEST(load_from_nonexistent_path_returns_zero);
    RUN_TEST(find_stage_and_room_returns_section);
    RUN_TEST(find_stage_and_room_unknown_room_returns_zero);
    RUN_TEST(find_stage_and_room_unknown_stage_returns_zero);
    RUN_TEST(find_section_with_altparam_terminator);
    RUN_TEST(find_section_no_altparam_uses_next_same_tag);
    RUN_TEST(find_section_returns_zero_when_tag_missing);
    RUN_TEST(find_section_last_in_file_extends_to_eof);
    RUN_TEST(get_section_start_null_returns_null);
}
