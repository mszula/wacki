/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_vm_show_text_bind.c — op 0x09 SHOW_TEXT speech-bind path.
 *
 * After op 0x19 QUEUE_DIALOG pushes (speaker, ptr, data) into the
 * VM-local dlg_speaker[] / dlg_ptr[] / dlg_data[] arrays, a subsequent
 * op 0x09 SHOW_TEXT with reg_id matching one of the queued speakers
 * triggers a SPEECH-BIND path:
 *
 *   1. Scan dlg_speaker[] for first match with reg_id.
 *   2. Call FindEntityByVerbId(dlg_speaker[found]) → get speaker entity.
 *   3. If found AND xlat_binary_ptr(dlg_ptr[found]) resolves:
 *        - Reset entity walker state (10 fields).
 *        - entity[+0x2C] = ent_ptr_intern(resolved_bc).
 *        - entity[+0x30] = 0 (frame reset).
 *   4. Stash dlg_speaker[found] / dlg_data[found] into g_speech_unbind_*.
 *
 * This is what drives mouth-animation during dialog lines. Earlier port
 * skipped the bind → speakers were silent and static during dialog.
 *
 * Reference: src/script.c case 0x09 lines 736-785.
 */

#include "test.h"
#include "wacki.h"
#include "test_engine_stubs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int RunScriptInterpreter(uint16_t this_id, uint16_t that_id, uint8_t *bytecode);
extern uint32_t g_script_vars[0x129];
extern int   PeLoaderInit(const char *exe_path);
extern void  PeLoaderFree(void);
extern uint32_t ent_ptr_intern(void *p);
extern void    *ent_ptr_resolve(uint32_t slot);

/* Globals set by op 0x09's speech-bind path. */
extern uint16_t g_speech_unbind_speaker;
extern uint32_t g_speech_unbind_data;
extern void *g_speech_balloon;

static const char kTmpPe[] = "/tmp/wacki-test-show-text-bind.exe";

/* Mini PE builder with caller-controlled .data section bytes at VA 0x00401000. */
static void build_pe(uint8_t *out, const uint8_t *section, size_t n)
{
    if (n > 0x40) n = 0x40;
    memset(out, 0, 0x200);
    out[0x00] = 'M'; out[0x01] = 'Z';
    uint32_t e_lfanew = 0x80;
    memcpy(out + 0x3C, &e_lfanew, 4);
    out[0x80] = 'P'; out[0x81] = 'E';
    uint16_t machine = 0x014C, nsec = 1, opt_hdr_size = 0x60, chars = 0x0103;
    memcpy(out + 0x84, &machine, 2);
    memcpy(out + 0x86, &nsec, 2);
    memcpy(out + 0x94, &opt_hdr_size, 2);
    memcpy(out + 0x96, &chars, 2);
    uint16_t opt_magic = 0x010B;
    memcpy(out + 0x98, &opt_magic, 2);
    uint32_t image_base = 0x00400000u;
    memcpy(out + 0xB4, &image_base, 4);
    memcpy(out + 0xF8, ".data", 5);
    uint32_t vsize = 0x40, va = 0x1000, rsize = 0x40, rptr = 0x140;
    memcpy(out + 0xF8 + 0x08, &vsize, 4);
    memcpy(out + 0xF8 + 0x0C, &va, 4);
    memcpy(out + 0xF8 + 0x10, &rsize, 4);
    memcpy(out + 0xF8 + 0x14, &rptr, 4);
    memcpy(out + 0x140, section, n);
}

static void setup_pe(const uint8_t *section, size_t n)
{
    uint8_t blob[512];
    build_pe(blob, section, n);
    FILE *fp = fopen(kTmpPe, "wb");
    fwrite(blob, 1, 0x200, fp);
    fclose(fp);
    PeLoaderFree();
    PeLoaderInit(kTmpPe);
}

static void teardown_pe(void)
{
    PeLoaderFree();
    remove(kTmpPe);
}

static size_t emit(uint16_t *buf, size_t pos, uint8_t op, uint8_t len,
                    uint16_t a0, uint16_t a1, uint16_t a2)
{
    buf[pos + 0] = (uint16_t)op | (uint16_t)((uint16_t)len << 8);
    buf[pos + 1] = a0;
    if (len >= 2) {
        buf[pos + 2] = a1;
        buf[pos + 3] = a2;
    }
    return pos + (size_t)len * 2;
}

static uint8_t s_ent_buf[256];
static Entity *make_entity_clear(void)
{
    memset(s_ent_buf, 0, sizeof s_ent_buf);
    return (Entity *)s_ent_buf;
}

static void reset_vm(void)
{
    memset(g_script_vars, 0, sizeof g_script_vars);
    vm_stubs_reset();
    g_stub_entity_for_verb = NULL;
    g_speech_balloon = NULL;
    g_speech_unbind_speaker = 0;
    g_speech_unbind_data = 0;
}

/* ---- speech-bind path: QUEUE then SHOW_TEXT matching speaker --------- */

TEST(queue_then_show_text_matching_speaker_binds_bytecode)
{
    /* PE section has "fake bytecode" at VA 0x00401000. */
    uint8_t section[16] = { 0xCA, 0xFE, 0xBA, 0xBE,
                             0xDE, 0xAD, 0xBE, 0xEF };
    setup_pe(section, sizeof section);

    reset_vm();
    Entity *speaker = make_entity_clear();
    test_inject_entity_for_verb(speaker, 42);

    /* Build:
     *   op 0x19 QUEUE_DIALOG speaker=42 ptr=0x00401000 data=0xDEADCAFE
     *   op 0x09 SHOW_TEXT reg=42 (matches)
     *   END_FORCE
     */
    uint16_t prog[32] = { 0 };
    size_t pos = 0;

    /* QUEUE_DIALOG op 0x19 len=3:
     *   word[1] = speaker (42)
     *   word[2..3] = ptr u32
     *   word[4..5] = data u32
     */
    prog[pos + 0] = (uint16_t)0x19 | (3 << 8);
    prog[pos + 1] = 42;
    prog[pos + 2] = (uint16_t)(0x00401000u & 0xFFFF);
    prog[pos + 3] = (uint16_t)((0x00401000u >> 16) & 0xFFFF);
    prog[pos + 4] = (uint16_t)(0xDEADCAFEu & 0xFFFF);
    prog[pos + 5] = (uint16_t)((0xDEADCAFEu >> 16) & 0xFFFF);
    pos += 3 * 2;

    /* SHOW_TEXT op 0x09 len=2:
     *   word[1] = reg=42
     *   word[2..3] = text VA (use NULL VA → text resolves to NULL, fine)
     */
    prog[pos + 0] = (uint16_t)0x09 | (2 << 8);
    prog[pos + 1] = 42;
    prog[pos + 2] = 0; prog[pos + 3] = 0;
    pos += 2 * 2;

    emit(prog, pos, 0x55, 1, 0, 0, 0);

    RunScriptInterpreter(0, 0, (uint8_t *)prog);

    /* Speaker entity should have its bytecode slot bound. */
    uint8_t *eb = (uint8_t *)speaker;
    uint32_t slot = *(uint32_t *)(eb + 0x2C);
    ASSERT_TRUE(slot != 0);
    /* Resolved pointer should point at PE memory (first byte 0xCA). */
    void *resolved = ent_ptr_resolve(slot);
    ASSERT_NOT_NULL(resolved);
    ASSERT_EQ(((uint8_t *)resolved)[0], 0xCA);

    /* Frame index reset. */
    ASSERT_EQ(*(uint16_t *)(eb + 0x30), 0);

    /* Walker state cleared (spot-check). */
    ASSERT_EQ(*(uint16_t *)(eb + 0x3A) & 5u, 0u);
    ASSERT_EQ(*(uint32_t *)(eb + 0x4C), 0u);
    ASSERT_EQ(*(uint32_t *)(eb + 0x50), 0u);

    /* Unbind state stashed. */
    ASSERT_EQ(g_speech_unbind_speaker, 42);
    ASSERT_EQ(g_speech_unbind_data,    0xDEADCAFEu);

    /* SHOW_TEXT was also dispatched. */

    teardown_pe();
}

TEST(show_text_no_queued_speaker_no_bind)
{
    /* SHOW_TEXT with reg=42 but no QUEUE_DIALOG entry → no bind path,
     * but SHOW_TEXT still dispatches normally and unbind globals stay 0. */
    setup_pe((const uint8_t *)"\0", 1);

    reset_vm();
    Entity *speaker = make_entity_clear();
    test_inject_entity_for_verb(speaker, 42);

    uint16_t prog[8] = { 0 };
    size_t pos = 0;
    prog[pos + 0] = (uint16_t)0x09 | (2 << 8);
    prog[pos + 1] = 42;
    pos += 2 * 2;
    emit(prog, pos, 0x55, 1, 0, 0, 0);

    RunScriptInterpreter(0, 0, (uint8_t *)prog);

    /* No bind → entity[+0x2C] unchanged (= 0 from make_entity_clear). */
    uint8_t *eb = (uint8_t *)speaker;
    ASSERT_EQ(*(uint32_t *)(eb + 0x2C), 0u);

    /* Unbind globals NOT set. */
    ASSERT_EQ(g_speech_unbind_speaker, 0);
    ASSERT_EQ(g_speech_unbind_data,    0u);

    /* SHOW_TEXT still dispatched. */

    teardown_pe();
}

TEST(show_text_queued_but_no_entity_no_crash)
{
    /* QUEUE matches, but FindEntityByVerbId returns NULL → bind path
     * skipped, but unbind globals are reset on entry (not stashed). */
    setup_pe((const uint8_t *)"\0", 1);

    reset_vm();
    g_stub_entity_for_verb = NULL;          /* no entity to bind to */

    uint16_t prog[32] = { 0 };
    size_t pos = 0;
    prog[pos + 0] = (uint16_t)0x19 | (3 << 8);
    prog[pos + 1] = 7;
    prog[pos + 2] = 0; prog[pos + 3] = 0x40; /* ptr = 0x00400000 */
    prog[pos + 4] = 0; prog[pos + 5] = 0;
    pos += 3 * 2;
    prog[pos + 0] = (uint16_t)0x09 | (2 << 8);
    prog[pos + 1] = 7;
    pos += 2 * 2;
    emit(prog, pos, 0x55, 1, 0, 0, 0);

    int rc = RunScriptInterpreter(0, 0, (uint8_t *)prog);
    ASSERT_EQ(rc, 1);
    /* No bind. SHOW_TEXT still dispatched (the bind path doesn't gate
     * the ScriptCallShowText call). */

    teardown_pe();
}

TEST(show_text_queued_unresolved_ptr_still_stashes_unbind_when_sp_found)
{
    /* QUEUE_DIALOG with ptr VA outside PE (xlat returns NULL) → bytecode
     * BIND is skipped (new_bc NULL), BUT the unbind-stash at lines
     * 781-782 is gated only by `if (sp)` (entity found), NOT by
     * `if (new_bc)`. So when FindEntityByVerbId returns non-NULL,
     * the stash STILL fires with the queued speaker/data even though
     * no actual bind occurred.
     *
     * This contract matters for the speech balloon TICK path —
     * TickSpeechBalloon reads g_speech_unbind_speaker/data on balloon
     * dismissal to re-bind the speaker's IDLE script. Even when the
     * dialog line couldn't bind a talking-head anim, the dismissal
     * still needs the speaker id for cleanup. */
    setup_pe((const uint8_t *)"\0", 1);

    reset_vm();
    Entity *speaker = make_entity_clear();
    test_inject_entity_for_verb(speaker, 11);   /* match SHOW_TEXT reg=11 */

    uint16_t prog[32] = { 0 };
    size_t pos = 0;
    prog[pos + 0] = (uint16_t)0x19 | (3 << 8);
    prog[pos + 1] = 11;
    /* ptr VA = 0xCAFE0000 — way outside PE → xlat returns NULL */
    prog[pos + 2] = 0; prog[pos + 3] = 0xCAFE;
    /* data dword */
    prog[pos + 4] = 0x1111; prog[pos + 5] = 0x2222;
    pos += 3 * 2;
    prog[pos + 0] = (uint16_t)0x09 | (2 << 8);
    prog[pos + 1] = 11;
    pos += 2 * 2;
    emit(prog, pos, 0x55, 1, 0, 0, 0);

    RunScriptInterpreter(0, 0, (uint8_t *)prog);

    /* Unbind globals stashed even though bind skipped (sp was found). */
    ASSERT_EQ(g_speech_unbind_speaker, 11);
    ASSERT_EQ(g_speech_unbind_data, 0x22221111u);

    /* Entity bytecode NOT bound (xlat returned NULL → new_bc skipped). */
    uint8_t *eb = (uint8_t *)speaker;
    ASSERT_EQ(*(uint32_t *)(eb + 0x2C), 0u);

    teardown_pe();
}

SUITE(vm_show_text_bind)
{
    RUN_TEST(queue_then_show_text_matching_speaker_binds_bytecode);
    RUN_TEST(show_text_no_queued_speaker_no_bind);
    RUN_TEST(show_text_queued_but_no_entity_no_crash);
    RUN_TEST(show_text_queued_unresolved_ptr_still_stashes_unbind_when_sp_found);
}
