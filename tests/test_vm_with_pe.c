/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_vm_with_pe.c — VM ops that require PE-loaded fixtures.
 *
 * These opcodes resolve operands via xlat_binary_ptr(VA), so their
 * "happy path" needs a PE image mapped with the right bytes at the
 * right VAs. Earlier suites cover the unresolved-VA fallback (returns
 * NULL → op no-ops). Here we exercise the actual fixture-driven paths.
 *
 * Covers:
 *   - op 0x0E SET_ENTITY_SCRIPT  → resolved bytecode pointer bound to entity
 *   - op 0x27 SET_OBJ_PROP       → write to tagged-table position via FindKey
 *   - op 0x28 SET_ENT_POS        → foot-anchor compensation (atlas + flag bit 1)
 *
 * Reference: src/script.c case 0x0E / 0x27 / 0x28.
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

static const char kTmpPe[] = "/tmp/wacki-test-vm-with-pe.exe";

/* Build a minimal PE with a writable .data section at VA 0x00401000.
 * Section contains caller-supplied bytes. */
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

static size_t emit_imm32(uint16_t *buf, size_t pos, uint8_t op, uint8_t len,
                          uint16_t a0, uint32_t imm32)
{
    buf[pos + 0] = (uint16_t)op | (uint16_t)((uint16_t)len << 8);
    buf[pos + 1] = a0;
    buf[pos + 2] = (uint16_t)(imm32 & 0xFFFF);
    buf[pos + 3] = (uint16_t)((imm32 >> 16) & 0xFFFF);
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
    g_stub_update_registration_for_kind_id = NULL;
}

/* ---- op 0x0E SET_ENTITY_SCRIPT — bytecode resolved + bound ----------- */

TEST(op_0E_resolved_bytecode_bound_to_entity)
{
    /* PE section contains 8 bytes of "bytecode" starting at VA 0x00401000.
     * Op 0x0E with i32_at4 = 0x00401000 → xlat_binary_ptr resolves
     * → ent_ptr_intern slots the pointer → entity[+0x2C] = slot id. */
    uint8_t section[16] = { 0xCA, 0xFE, 0xBA, 0xBE, 0x12, 0x34, 0x56, 0x78 };
    setup_pe(section, sizeof section);

    reset_vm();
    Entity *e = make_entity_clear();
    test_inject_entity_for_verb(e, 7);

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit_imm32(prog, p, 0x0E, 2, 7, 0x00401000u);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);

    /* entity[+0x2C] should hold a non-zero slot id. */
    uint32_t slot = *(uint32_t *)((uint8_t *)e + 0x2C);
    ASSERT_TRUE(slot != 0);

    /* ent_ptr_resolve(slot) should yield the PE-resolved pointer
     * (PE image start + 0x1000). */
    void *resolved = ent_ptr_resolve(slot);
    ASSERT_NOT_NULL(resolved);
    /* First bytes should match what we wrote into PE. */
    ASSERT_EQ(((uint8_t *)resolved)[0], 0xCA);
    ASSERT_EQ(((uint8_t *)resolved)[1], 0xFE);

    /* Frame index should be reset to 0. */
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x30), 0);

    teardown_pe();
}

/* ---- op 0x28 SET_ENT_POS — foot-anchor compensation ------------------ *
 *
 * When the entity has:
 *   - atlas at +0x28 (non-zero slot)
 *   - flags3a (+0x3A) bit 1 set
 *   - atlas->off_drawX / off_drawY non-NULL
 *   - frame index (+0x30) < atlas->frame_count
 *
 * the drawn position (+0x0A/+0x0C) gets the atlas hot-spot offset
 * added, while anchor (+0x22/+0x24) stays at the raw (x, y).
 *
 * This is the "foot anchor" compensation — the script positions the
 * actor by its foot, but the sprite needs to be offset so the foot
 * lands on the requested pixel.
 */

TEST(op_28_set_ent_pos_with_foot_anchor_adjusts_drawn)
{
    reset_vm();
    Entity *e = make_entity_clear();
    uint8_t *eb = (uint8_t *)e;

    /* Set flag bit 1 (foot-anchor active). */
    *(uint16_t *)(eb + 0x3A) = 2;

    /* Set frame index to 0. */
    *(uint16_t *)(eb + 0x30) = 0;

    /* Build an AnimAsset on the stack and intern it into the atlas slot. */
    static uint16_t draw_x_table[2] = { (uint16_t)(int16_t)-5, 99 };
    static uint16_t draw_y_table[2] = { (uint16_t)(int16_t)-10, 88 };

    AnimAsset atlas;
    memset(&atlas, 0, sizeof atlas);
    atlas.frame_count = 2;
    atlas.off_drawX   = draw_x_table;
    atlas.off_drawY   = draw_y_table;

    uint32_t slot = ent_ptr_intern(&atlas);
    ASSERT_TRUE(slot != 0);
    *(uint32_t *)(eb + 0x28) = slot;

    test_inject_entity_for_verb(e, 7);

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x28, 2, 7, 100, 200);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);

    /* Anchor (+0x22/+0x24) = raw (x, y). */
    ASSERT_EQ(*(uint16_t *)(eb + 0x22), 100);
    ASSERT_EQ(*(uint16_t *)(eb + 0x24), 200);
    /* Drawn (+0x0A/+0x0C) = (x + atlas.draw_x[0], y + atlas.draw_y[0])
     *                     = (100 + -5, 200 + -10) = (95, 190). */
    ASSERT_EQ((int16_t)*(uint16_t *)(eb + 0x0A), 95);
    ASSERT_EQ((int16_t)*(uint16_t *)(eb + 0x0C), 190);
}

TEST(op_28_no_atlas_drawn_equals_anchor)
{
    /* atlas == NULL → no compensation → drawn == anchor == raw (x, y). */
    reset_vm();
    Entity *e = make_entity_clear();
    /* No atlas slot set. */
    test_inject_entity_for_verb(e, 7);

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x28, 2, 7, 333, 444);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);

    uint8_t *eb = (uint8_t *)e;
    ASSERT_EQ(*(uint16_t *)(eb + 0x0A), 333);
    ASSERT_EQ(*(uint16_t *)(eb + 0x0C), 444);
    ASSERT_EQ(*(uint16_t *)(eb + 0x22), 333);
    ASSERT_EQ(*(uint16_t *)(eb + 0x24), 444);
}

TEST(op_28_flag_bit_1_not_set_drawn_equals_raw_xy)
{
    /* atlas exists but flags3a bit 1 NOT set → no compensation. */
    reset_vm();
    Entity *e = make_entity_clear();
    uint8_t *eb = (uint8_t *)e;

    *(uint16_t *)(eb + 0x3A) = 0;   /* bit 1 not set */

    static uint16_t draw_x_table[1] = { (uint16_t)(int16_t)-30 };
    static uint16_t draw_y_table[1] = { (uint16_t)(int16_t)-40 };
    AnimAsset atlas;
    memset(&atlas, 0, sizeof atlas);
    atlas.frame_count = 1;
    atlas.off_drawX = draw_x_table;
    atlas.off_drawY = draw_y_table;
    *(uint32_t *)(eb + 0x28) = ent_ptr_intern(&atlas);

    test_inject_entity_for_verb(e, 7);

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x28, 2, 7, 500, 600);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);

    /* All 4 fields = raw (x, y). */
    ASSERT_EQ(*(uint16_t *)(eb + 0x0A), 500);
    ASSERT_EQ(*(uint16_t *)(eb + 0x22), 500);
    ASSERT_EQ(*(uint16_t *)(eb + 0x0C), 600);
    ASSERT_EQ(*(uint16_t *)(eb + 0x24), 600);
}

TEST(op_28_frame_index_above_frame_count_no_compensation)
{
    /* Defensive: fid >= frame_count → no compensation (safety). */
    reset_vm();
    Entity *e = make_entity_clear();
    uint8_t *eb = (uint8_t *)e;

    *(uint16_t *)(eb + 0x3A) = 2;        /* foot-anchor on */
    *(uint16_t *)(eb + 0x30) = 99;       /* out-of-range frame */

    static uint16_t dx[1] = { (uint16_t)(int16_t)-7 };
    static uint16_t dy[1] = { (uint16_t)(int16_t)-8 };
    AnimAsset atlas;
    memset(&atlas, 0, sizeof atlas);
    atlas.frame_count = 1;     /* fid=99 >= 1 → skip */
    atlas.off_drawX = dx;
    atlas.off_drawY = dy;
    *(uint32_t *)(eb + 0x28) = ent_ptr_intern(&atlas);

    test_inject_entity_for_verb(e, 7);

    uint16_t prog[8] = { 0 };
    size_t p = 0;
    p = emit(prog, p, 0x28, 2, 7, 100, 200);
    p = emit(prog, p, 0x55, 1, 0, 0, 0);
    (void)p;

    RunScriptInterpreter(0, 0, (uint8_t *)prog);

    /* No compensation → drawn == anchor == raw. */
    ASSERT_EQ(*(uint16_t *)(eb + 0x0A), 100);
    ASSERT_EQ(*(uint16_t *)(eb + 0x0C), 200);
}

/* ---- op 0x27 SET_OBJ_PROP — write to tagged-table position ----------- *
 *
 * blk = xlat_binary_ptr(i32_at4)
 * idx = FindKeyInTaggedTable(blk, '\x15', -1)
 * if (idx != 0) {
 *   *(uint16_t *)(blk + 2 + idx * 2) = a2 (X)
 *   *(uint16_t *)(blk + 4 + idx * 2) = pc[5] (Y)
 * }
 *
 * `idx != 0` is a "skip first-record match" guard — if the tag is the
 * FIRST record, idx==0 and the write skips. To trigger the write we
 * need the matching '\x15' to come AFTER at least one other record
 * (so idx accumulates non-zero).
 */

TEST(op_27_set_obj_prop_writes_xy_into_tagged_record)
{
    /* Tagged table:
     *   record 0: tag='A' len=2 key=0       (idx → 2 after skip)
     *   record 1: tag=0x15 len=2 key=0      (match → returns idx=2)
     *   record 2: '!' terminator
     *
     * Write target: blk + 2 + 2*2 = blk+6 (X), blk + 4 + 2*2 = blk+8 (Y).
     */
    uint8_t section[64] = { 0 };
    /* record 0 — 4 bytes: tag, len, key_lo, key_hi */
    section[0] = 'A'; section[1] = 2; section[2] = 0; section[3] = 0;
    /* record 1 — 4 bytes: tag=0x15 */
    section[4] = 0x15; section[5] = 2; section[6] = 0; section[7] = 0;
    /* record 2 — terminator */
    section[8] = '!';

    setup_pe(section, sizeof section);

    reset_vm();
    uint16_t prog[16] = { 0 };
    size_t pos = 0;
    /* op=0x27 len=3 → 12 bytes. word[1]=a0 unused, dword@+4 = VA = 0x00401000.
     * word[4] = a2 (X), word[5] = pc[5] (Y). */
    prog[pos + 0] = (uint16_t)0x27 | (3 << 8);
    prog[pos + 1] = 0;
    /* dword @ +4 = VA */
    prog[pos + 2] = (uint16_t)(0x00401000u & 0xFFFF);
    prog[pos + 3] = (uint16_t)((0x00401000u >> 16) & 0xFFFF);
    /* a2 (= pc[4]) = X coordinate */
    prog[pos + 4] = 0x1234;
    /* pc[5] = Y coordinate */
    prog[pos + 5] = 0x5678;
    pos += 3 * 2;
    emit(prog, pos, 0x55, 1, 0, 0, 0);

    RunScriptInterpreter(0, 0, (uint8_t *)prog);

    /* Read back from PE memory at VA 0x00401006 (= blk + 6 = X)
     * and 0x00401008 (= blk + 8 = Y). xlat_binary_ptr resolves. */
    extern const void *xlat_binary_ptr(uint32_t addr);
    const uint8_t *blk = (const uint8_t *)xlat_binary_ptr(0x00401000u);
    ASSERT_NOT_NULL(blk);
    uint16_t written_x = *(const uint16_t *)(blk + 6);
    uint16_t written_y = *(const uint16_t *)(blk + 8);
    ASSERT_EQ(written_x, 0x1234);
    ASSERT_EQ(written_y, 0x5678);

    teardown_pe();
}

TEST(op_27_tag_at_first_record_skips_write_idx_zero)
{
    /* When the matching tag IS the first record, idx == 0 → the
     * write is skipped (port's `if (idx != 0)` guard). Pre-fill the
     * target slots and verify they stay unchanged. */
    uint8_t section[32] = { 0 };
    /* record 0 — tag=0x15 (matches immediately, idx returned = 0) */
    section[0] = 0x15; section[1] = 2; section[2] = 0; section[3] = 0;
    /* record 1 — terminator */
    section[4] = '!';
    /* Pre-fill target slots (blk+2 and blk+4) with sentinels. */
    /* Actually for idx=0 the write would target blk+2 and blk+4 (= section[2..3]
     * and section[4..5]). But idx=0 SKIPS the write. So those stay as we
     * wrote them. section[2..3] = 0 (key); section[4..5] = '!' + 0. */

    setup_pe(section, sizeof section);

    reset_vm();
    uint16_t prog[16] = { 0 };
    size_t pos = 0;
    prog[pos + 0] = (uint16_t)0x27 | (3 << 8);
    prog[pos + 1] = 0;
    prog[pos + 2] = (uint16_t)(0x00401000u & 0xFFFF);
    prog[pos + 3] = (uint16_t)((0x00401000u >> 16) & 0xFFFF);
    prog[pos + 4] = 0xDEAD;
    prog[pos + 5] = 0xBEEF;
    pos += 3 * 2;
    emit(prog, pos, 0x55, 1, 0, 0, 0);

    RunScriptInterpreter(0, 0, (uint8_t *)prog);

    /* No write happened — verify section[2..3] still = 0 (the key bytes
     * we put there), section[4..5] still '!' '\0'. */
    extern const void *xlat_binary_ptr(uint32_t addr);
    const uint8_t *blk = (const uint8_t *)xlat_binary_ptr(0x00401000u);
    ASSERT_EQ(blk[2], 0);
    ASSERT_EQ(blk[3], 0);
    ASSERT_EQ(blk[4], '!');
    ASSERT_EQ(blk[5], 0);

    teardown_pe();
}

SUITE(vm_with_pe)
{
    RUN_TEST(op_0E_resolved_bytecode_bound_to_entity);
    RUN_TEST(op_28_set_ent_pos_with_foot_anchor_adjusts_drawn);
    RUN_TEST(op_28_no_atlas_drawn_equals_anchor);
    RUN_TEST(op_28_flag_bit_1_not_set_drawn_equals_raw_xy);
    RUN_TEST(op_28_frame_index_above_frame_count_no_compensation);
    RUN_TEST(op_27_set_obj_prop_writes_xy_into_tagged_record);
    RUN_TEST(op_27_tag_at_first_record_skips_write_idx_zero);
}
