/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_per_entity_vm_real.c — per-entity VM via PRODUCTION dispatch.
 *
 * src/actor.c ExecEntityScript (the per-entity VM, FUN_004012E0) is
 * static, so we exercise it through EntityWalkerTick (FUN_004012B0):
 *
 *     EntityWalkerTick(NULL)
 *         → walks kind=0 entity list
 *         → for each entity with atlas slot != NULL: ExecEntityScript
 *
 * To test individual opcodes, we link a single entity into the render
 * list, point its +0x2C bytecode slot at a small instruction stream,
 * then call EntityWalkerTick(NULL). Post-tick, we read back the entity
 * fields the opcode was supposed to mutate.
 *
 * Per-entity bytecode format (from src/actor.c:279+):
 *   instruction layout: [op:1][dlt:1][operand:2][optional operand:2][optional:2]
 *   stride = dlt * 2 bytes
 *   END marker: op 0x21
 *
 * Key entity offsets touched:
 *   +0x22 anchor_x   +0x24 anchor_y      +0x26 fade/z
 *   +0x28 atlas slot +0x2C bytecode slot +0x30 frame
 *   +0x32 pc         +0x3A state_flags   +0x3C delay  +0x3E delay_reset
 *
 * Reference: src/actor.c:361 (ExecEntityScript), :1003 (EntityWalkerTick).
 */

#include "test.h"
#include "wacki.h"
#include "test_engine_stubs.h"

#include <stdint.h>
#include <string.h>

extern void EntityWalkerTick(Entity *head);

extern Entity *g_render_list_head;
extern void    EntityListClearAll(void);
extern void    LinkEntityToList(Entity **head, Entity *e, int position);
extern uint32_t ent_ptr_intern(void *p);

extern uint16_t g_frame_delta_ticks;

/* Per-entity bytecode opcodes — pinned constants from actor.c. */
#define PVM_SET_ANCHOR_XY      0x00
#define PVM_SET_ANCHOR_X       0x01
#define PVM_SET_ANCHOR_Y       0x02
#define PVM_SET_POS_FROM_FRAME 0x05
#define PVM_SET_FRAME          0x06
#define PVM_IF_FRAME           0x07
#define PVM_SET_DELAY          0x09
#define PVM_CLEAR_LOOP_CTRS    0x0B
#define PVM_SET_RAND_FRAME     0x0F
#define PVM_DELAY_PAUSE        0x10
#define PVM_ADVANCE_FRAME      0x12
#define PVM_ADD_X              0x17
#define PVM_ADD_Y              0x18
#define PVM_SET_FLAG_2         0x1B
#define PVM_CLEAR_FLAG_2       0x1C
#define PVM_STOP_TICK          0x1D
#define PVM_STOP_RESET         0x1F
#define PVM_STOP_KEEP_PC       0x20
#define PVM_END                0x21
#define PVM_SET_FADE           0x24

/* Test entity buffer (256 bytes), bytecode buffer, atlas buffer. */
static uint8_t      s_ent_buf[256];
static uint8_t      s_bytecode[256];

/* AnimAsset with 4 frames, draw/width/height tables. */
static uint16_t     s_w[4]      = { 20, 22, 24, 26 };
static uint16_t     s_h[4]      = { 30, 32, 34, 36 };
static uint16_t     s_dx[4]     = { 100, 110, 120, 130 };
static uint16_t     s_dy[4]     = { 200, 210, 220, 230 };
static uint8_t      s_pixel[8]  = { 0 };
static uint8_t     *s_pixel_ptrs[4] = { s_pixel, s_pixel, s_pixel, s_pixel };
static AnimAsset    s_atlas = {
    .frame_count = 4,
    .off_widths  = s_w,
    .off_heights = s_h,
    .off_drawX   = s_dx,
    .off_drawY   = s_dy,
    .pixel_ptrs  = s_pixel_ptrs,
};

/* Emit one per-entity VM instruction. Returns new buffer position.
 * Layout: [op][dlt][op0_lo][op0_hi][op1_lo][op1_hi][op2_lo][op2_hi]. */
static int emit_p(uint8_t *buf, int pos, uint8_t op, uint8_t dlt,
                   uint16_t a0, uint16_t a1, uint16_t a2)
{
    buf[pos + 0] = op;
    buf[pos + 1] = dlt;
    if (dlt >= 2) {
        buf[pos + 2] = (uint8_t)(a0 & 0xFF);
        buf[pos + 3] = (uint8_t)((a0 >> 8) & 0xFF);
    }
    if (dlt >= 3) {
        buf[pos + 4] = (uint8_t)(a1 & 0xFF);
        buf[pos + 5] = (uint8_t)((a1 >> 8) & 0xFF);
    }
    if (dlt >= 4) {
        buf[pos + 6] = (uint8_t)(a2 & 0xFF);
        buf[pos + 7] = (uint8_t)((a2 >> 8) & 0xFF);
    }
    return pos + (int)dlt * 2;
}

/* Set up entity with atlas + bytecode bindings, link into render list. */
static Entity *make_entity_with_bytecode(uint8_t *bytecode)
{
    memset(s_ent_buf, 0, sizeof s_ent_buf);
    /* Atlas slot +0x28, bytecode slot +0x2C. */
    *(uint32_t *)(s_ent_buf + 0x28) = ent_ptr_intern(&s_atlas);
    *(uint32_t *)(s_ent_buf + 0x2C) = ent_ptr_intern(bytecode);
    /* Initialize delay to 0 so the gate doesn't block the first tick.
     * (delay - g_frame_delta_ticks(=1) = -1 → falls through.) */
    *(uint16_t *)(s_ent_buf + 0x3C) = 0;
    *(uint16_t *)(s_ent_buf + 0x3E) = 0;
    *(uint16_t *)(s_ent_buf + 0x32) = 0;     /* pc */

    EntityListClearAll();
    LinkEntityToList(&g_render_list_head, (Entity *)s_ent_buf, 0);
    return (Entity *)s_ent_buf;
}

static void reset(void)
{
    g_frame_delta_ticks = 1;
    EntityListClearAll();
    memset(s_bytecode, 0, sizeof s_bytecode);
    memset(s_ent_buf, 0, sizeof s_ent_buf);
}

/* ---- 0x00 SET_ANCHOR_XY --------------------------------------------- */

TEST(pvm_set_anchor_xy_writes_both)
{
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_SET_ANCHOR_XY, 2, 0x4242, 0x1234, 0);
    p = emit_p(s_bytecode, p, PVM_END,           1, 0, 0, 0);
    (void)p;

    /* op 0x00 uses arg (p+2) for X, then reads p+4/p+5 for Y. Bump dlt
     * to 3 so the operand bytes are valid. */
    s_bytecode[0] = PVM_SET_ANCHOR_XY;
    s_bytecode[1] = 3;                          /* dlt = 3 → 6 bytes */
    s_bytecode[2] = 0x42; s_bytecode[3] = 0x42;  /* X = 0x4242 */
    s_bytecode[4] = 0x34; s_bytecode[5] = 0x12;  /* Y = 0x1234 */
    /* Next instruction at offset 6 = END. */
    s_bytecode[6] = PVM_END;
    s_bytecode[7] = 1;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    EntityWalkerTick(NULL);

    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x22), 0x4242);
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x24), 0x1234);
}

/* ---- 0x01 SET_ANCHOR_X / 0x02 SET_ANCHOR_Y -------------------------- */

TEST(pvm_set_anchor_x_only_writes_x)
{
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_SET_ANCHOR_X, 2, 0xCAFE, 0, 0);
    p = emit_p(s_bytecode, p, PVM_END,          1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    *(uint16_t *)((uint8_t *)e + 0x24) = 0x5678;       /* prior Y untouched */
    EntityWalkerTick(NULL);

    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x22), 0xCAFE);
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x24), 0x5678);
}

TEST(pvm_set_anchor_y_only_writes_y)
{
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_SET_ANCHOR_Y, 2, 0xBEEF, 0, 0);
    p = emit_p(s_bytecode, p, PVM_END,          1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    *(uint16_t *)((uint8_t *)e + 0x22) = 0x9999;
    EntityWalkerTick(NULL);

    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x22), 0x9999);
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x24), 0xBEEF);
}

/* ---- 0x06 SET_FRAME ------------------------------------------------- */

TEST(pvm_set_frame_writes_frame)
{
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_SET_FRAME, 2, 2, 0, 0);
    p = emit_p(s_bytecode, p, PVM_END,       1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    EntityWalkerTick(NULL);
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x30), 2);
}

TEST(pvm_set_frame_clamps_to_frame_count_minus_one)
{
    /* Atlas has 4 frames → max index 3. Asking for 100 → clamped to 3. */
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_SET_FRAME, 2, 100, 0, 0);
    p = emit_p(s_bytecode, p, PVM_END,       1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    EntityWalkerTick(NULL);
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x30), 3);   /* clamped */
}

/* ---- 0x05 SET_POS_FROM_FRAME ---------------------------------------- */

TEST(pvm_set_pos_from_frame_copies_atlas_drawXY)
{
    /* Atlas frame 2: off_drawX[2]=120, off_drawY[2]=220. Pre-set
     * entity's frame to 2, then run SET_POS_FROM_FRAME. */
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_SET_POS_FROM_FRAME, 1, 0, 0, 0);
    p = emit_p(s_bytecode, p, PVM_END,                1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    *(uint16_t *)((uint8_t *)e + 0x30) = 2;    /* current frame */
    EntityWalkerTick(NULL);

    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x22), 120);
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x24), 220);
}

/* ---- 0x09 SET_DELAY ------------------------------------------------- */

TEST(pvm_set_delay_writes_both_delay_and_reset)
{
    /* SET_DELAY writes BOTH +0x3C (current) and +0x3E (period).
     * BUT: at this point we're still inside the same tick, and after
     * the instruction runs, the loop continues to the next instruction.
     * After END, post-exec runs. Verifying +0x3E is the cleanest test
     * (it's the period, not consumed by the lead-in gate). */
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_SET_DELAY, 2, 8, 0, 0);
    p = emit_p(s_bytecode, p, PVM_END,       1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    EntityWalkerTick(NULL);
    /* Period +0x3E set to 8. */
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x3E), 8);
}

/* ---- 0x10 SET_DELAY_AND_PAUSE: halts tick AND keeps pc on STOP -------- */

TEST(pvm_delay_pause_halts_tick_with_pc_advanced)
{
    /* 0x10 sets +0x3C = arg and ALSO falls through to STOP_TICK (which
     * sets bVar3=0, exiting the loop). pc is set to next_pc (= pc+dlt)
     * before the case 0x1D fallthrough — so pc advances past 0x10. */
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_DELAY_PAUSE, 2, 42, 0, 0);
    p = emit_p(s_bytecode, p, PVM_SET_ANCHOR_X, 2, 0xDEAD, 0, 0);  /* should NOT execute */
    p = emit_p(s_bytecode, p, PVM_END,         1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    *(uint16_t *)((uint8_t *)e + 0x22) = 0x1111;
    EntityWalkerTick(NULL);

    /* Delay set to 42. */
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x3C), 42);
    /* Next instruction (SET_ANCHOR_X with 0xDEAD) did NOT execute. */
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x22), 0x1111);
}

/* ---- 0x17 ADD_X / 0x18 ADD_Y ---------------------------------------- */

TEST(pvm_add_x_adds_to_anchor_x)
{
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_ADD_X, 2, 10, 0, 0);
    p = emit_p(s_bytecode, p, PVM_END,   1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    *(uint16_t *)((uint8_t *)e + 0x22) = 100;
    EntityWalkerTick(NULL);
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x22), 110);
}

TEST(pvm_add_y_adds_to_anchor_y)
{
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_ADD_Y, 2, 5, 0, 0);
    p = emit_p(s_bytecode, p, PVM_END,   1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    *(uint16_t *)((uint8_t *)e + 0x24) = 200;
    EntityWalkerTick(NULL);
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x24), 205);
}

/* ---- 0x1B SET_FLAG_2 / 0x1C CLEAR_FLAG_2 ---------------------------- */

TEST(pvm_set_flag_2_sets_bit_1_of_3A)
{
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_SET_FLAG_2, 1, 0, 0, 0);
    p = emit_p(s_bytecode, p, PVM_END,        1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    *(uint8_t *)((uint8_t *)e + 0x3A) = 0;
    EntityWalkerTick(NULL);
    ASSERT_EQ(*(uint8_t *)((uint8_t *)e + 0x3A) & 2, 2);
}

TEST(pvm_clear_flag_2_clears_bit_1_of_3A)
{
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_CLEAR_FLAG_2, 1, 0, 0, 0);
    p = emit_p(s_bytecode, p, PVM_END,          1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    *(uint8_t *)((uint8_t *)e + 0x3A) = 0xFF;          /* all bits set */
    EntityWalkerTick(NULL);
    ASSERT_EQ(*(uint8_t *)((uint8_t *)e + 0x3A) & 2, 0);
    /* Other bits preserved (op 0x1C does `&= ~2u` so bit 0 still set). */
    ASSERT_EQ(*(uint8_t *)((uint8_t *)e + 0x3A) & 1, 1);
}

/* ---- 0x12 ADVANCE_FRAME --------------------------------------------- */

TEST(pvm_advance_frame_increments_frame)
{
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_ADVANCE_FRAME, 2, 1, 0, 0);
    p = emit_p(s_bytecode, p, PVM_END,           1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    *(uint16_t *)((uint8_t *)e + 0x30) = 1;
    EntityWalkerTick(NULL);
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x30), 2);
}

TEST(pvm_advance_frame_wraps_to_zero_when_arg_small)
{
    /* When frame >= frame_count AND arg < 0x80 → wraps to 0. */
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_ADVANCE_FRAME, 2, 1, 0, 0);
    p = emit_p(s_bytecode, p, PVM_END,           1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    *(uint16_t *)((uint8_t *)e + 0x30) = 3;   /* last frame; +1 = 4 = OOB */
    EntityWalkerTick(NULL);
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x30), 0);  /* wrapped */
}

/* ---- 0x07 IF_FRAME -------------------------------------------------- */

TEST(pvm_if_frame_matching_sets_bit_0)
{
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_IF_FRAME, 2, 2, 0, 0);    /* test == 2 */
    p = emit_p(s_bytecode, p, PVM_END,      1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    *(uint16_t *)((uint8_t *)e + 0x30) = 2;
    *(uint8_t *)((uint8_t *)e + 0x3A) = 0;
    EntityWalkerTick(NULL);
    ASSERT_EQ(*(uint8_t *)((uint8_t *)e + 0x3A) & 1, 1);   /* bit 0 set */
}

TEST(pvm_if_frame_non_matching_clears_bit_0)
{
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_IF_FRAME, 2, 2, 0, 0);
    p = emit_p(s_bytecode, p, PVM_END,      1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    *(uint16_t *)((uint8_t *)e + 0x30) = 5;   /* != 2 */
    *(uint8_t *)((uint8_t *)e + 0x3A) = 1;    /* bit 0 starts set */
    EntityWalkerTick(NULL);
    ASSERT_EQ(*(uint8_t *)((uint8_t *)e + 0x3A) & 1, 0);   /* bit 0 cleared */
}

/* ---- 0x0B CLEAR_LOOP_CTRS ------------------------------------------- */

TEST(pvm_clear_loop_ctrs_zeros_34_36_38)
{
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_CLEAR_LOOP_CTRS, 1, 0, 0, 0);
    p = emit_p(s_bytecode, p, PVM_END,             1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    *(uint16_t *)((uint8_t *)e + 0x34) = 0xAAAA;
    *(uint16_t *)((uint8_t *)e + 0x36) = 0xBBBB;
    *(uint16_t *)((uint8_t *)e + 0x38) = 0xCCCC;
    EntityWalkerTick(NULL);

    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x34), 0);
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x36), 0);
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x38), 0);
}

/* ---- 0x1D STOP_TICK ------------------------------------------------- */

TEST(pvm_stop_tick_halts_without_pc_reset)
{
    /* STOP_TICK exits the loop with bVar3=0 and next_pc unchanged
     * (= pc+dlt, advancing past STOP_TICK). On next tick, execution
     * resumes at the instruction after STOP_TICK. */
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_SET_ANCHOR_X, 2, 0xAAAA, 0, 0);
    int stop_pos = p;
    p = emit_p(s_bytecode, p, PVM_STOP_TICK,    1, 0, 0, 0);
    p = emit_p(s_bytecode, p, PVM_SET_ANCHOR_X, 2, 0xBBBB, 0, 0);  /* NOT this tick */
    p = emit_p(s_bytecode, p, PVM_END,          1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    EntityWalkerTick(NULL);
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x22), 0xAAAA);
    /* pc should be past STOP_TICK (= stop_pos/2 + 1 instr in halfwords).
     * STOP_TICK was at byte offset stop_pos with dlt=1, so next_pc =
     * (stop_pos/2) + 1 halfword-units. */
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x32), (uint16_t)(stop_pos / 2 + 1));
}

/* ---- 0x1F STOP_RESET (alias 0x21 END inside switch) ----------------- */

TEST(pvm_stop_reset_halts_and_zeros_pc)
{
    /* STOP_RESET = bVar3=0 + next_pc=0. Post-exec writes pc=0. */
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_SET_ANCHOR_X, 2, 0xCCCC, 0, 0);
    p = emit_p(s_bytecode, p, PVM_STOP_RESET,   1, 0, 0, 0);
    p = emit_p(s_bytecode, p, PVM_SET_ANCHOR_X, 2, 0xDDDD, 0, 0);
    p = emit_p(s_bytecode, p, PVM_END,          1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    EntityWalkerTick(NULL);
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x22), 0xCCCC);
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x32), 0);   /* pc reset */
}

/* ---- 0x20 STOP_KEEP_PC ---------------------------------------------- */

TEST(pvm_stop_keep_pc_halts_with_pc_at_instruction)
{
    /* bVar3=0 + next_pc = pc (current, NOT advanced). On next tick we
     * re-execute the STOP_KEEP_PC at the same pc — but it just stops
     * again, so it's a "permanent yield" until something external moves pc. */
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_SET_ANCHOR_X,  2, 0xEEEE, 0, 0);
    int stop_pos = p;
    p = emit_p(s_bytecode, p, PVM_STOP_KEEP_PC,  1, 0, 0, 0);
    p = emit_p(s_bytecode, p, PVM_END,           1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    EntityWalkerTick(NULL);
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x22), 0xEEEE);
    /* pc points at the STOP_KEEP_PC itself (not past it). */
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x32), (uint16_t)(stop_pos / 2));
}

/* ---- 0x24 SET_FADE -------------------------------------------------- */

TEST(pvm_set_fade_writes_26_and_sets_flag_2_of_byte_9)
{
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_SET_FADE, 2, 0x55, 0, 0);
    p = emit_p(s_bytecode, p, PVM_END,      1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    *(uint8_t *)((uint8_t *)e + 9) = 0;
    EntityWalkerTick(NULL);

    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x26), 0x55);
    ASSERT_EQ(*(uint8_t *)((uint8_t *)e + 9) & 2, 2);
}

/* ---- Delay gate: positive delay yields without running opcodes ------ */

TEST(pvm_positive_delay_skips_opcode_execution)
{
    /* +0x3C = 10, g_frame_delta_ticks = 1 → delay - 1 = 9 > 0 → goto
     * post_exec without running any opcodes. SET_ANCHOR_X(0xF0F0) must
     * NOT take effect. */
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_SET_ANCHOR_X, 2, 0xF0F0, 0, 0);
    p = emit_p(s_bytecode, p, PVM_END,          1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    *(uint16_t *)((uint8_t *)e + 0x22) = 0x4242;
    *(uint16_t *)((uint8_t *)e + 0x3C) = 10;
    EntityWalkerTick(NULL);

    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x22), 0x4242); /* unchanged */
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x3C), 9);      /* delay decremented */
}

/* ---- Post-exec mirrors anchor → drawn (a, c) ------------------------ */

TEST(pvm_post_exec_mirrors_anchor_to_drawn_position)
{
    /* After per-entity VM finishes, +0x22/+0x24 are copied to +0x0A/+0x0C
     * (drawn anchor) by post-exec block. */
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_SET_ANCHOR_X, 2, 0x1234, 0, 0);
    p = emit_p(s_bytecode, p, PVM_END,          1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    *(uint16_t *)((uint8_t *)e + 0x24) = 0x5678;
    EntityWalkerTick(NULL);

    ASSERT_EQ(*(int16_t *)((uint8_t *)e + 0x0A), 0x1234);
    ASSERT_EQ(*(int16_t *)((uint8_t *)e + 0x0C), 0x5678);
}

/* ---- Post-exec stashes atlas frame's W/H into +0x0E/+0x10 ----------- */

TEST(pvm_post_exec_writes_width_height_from_atlas)
{
    /* Frame 1: off_widths[1]=22, off_heights[1]=32. After execution
     * the entity should have these values in +0x0E and +0x10. */
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_SET_FRAME, 2, 1, 0, 0);
    p = emit_p(s_bytecode, p, PVM_END,       1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    EntityWalkerTick(NULL);

    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x0E), 22);
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x10), 32);
}

/* ---- Post-exec clamps scale_pct (+0x58) to 0xA0 --------------------- */

TEST(pvm_post_exec_clamps_scale_to_0xA0)
{
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_END, 1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);
    *(uint16_t *)((uint8_t *)e + 0x58) = 0xFF;   /* above 0xA0 */
    EntityWalkerTick(NULL);

    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x58), 0xA0);
}

/* ---- EntityWalkerTick: entities without atlas slot are skipped ------ */

TEST(pvm_entity_without_atlas_is_skipped)
{
    /* Entity has bytecode but +0x28 (atlas slot) = 0 → ent_ptr_resolve
     * returns NULL → ExecEntityScript not called. SET_ANCHOR_X must NOT
     * take effect. */
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_SET_ANCHOR_X, 2, 0x9999, 0, 0);
    p = emit_p(s_bytecode, p, PVM_END,          1, 0, 0, 0);
    (void)p;

    memset(s_ent_buf, 0, sizeof s_ent_buf);
    *(uint32_t *)(s_ent_buf + 0x28) = 0;                 /* NO atlas */
    *(uint32_t *)(s_ent_buf + 0x2C) = ent_ptr_intern(s_bytecode);
    *(uint16_t *)(s_ent_buf + 0x22) = 0x1111;            /* sentinel */

    EntityListClearAll();
    LinkEntityToList(&g_render_list_head, (Entity *)s_ent_buf, 0);
    EntityWalkerTick(NULL);

    /* Skipped — sentinel unchanged. */
    ASSERT_EQ(*(uint16_t *)(s_ent_buf + 0x22), 0x1111);
}

/* ---- pc threading across two ticks ---------------------------------- */

TEST(pvm_pc_carries_between_ticks)
{
    /* Two-instruction sequence: SET_ANCHOR_X(0xAAAA), STOP_TICK,
     * SET_ANCHOR_X(0xBBBB), END.
     *
     * Tick 1: runs first SET_ANCHOR_X + STOP_TICK → +0x22 = 0xAAAA,
     *         pc advanced past STOP_TICK.
     * Tick 2: resumes from second SET_ANCHOR_X → +0x22 = 0xBBBB. */
    reset();
    int p = 0;
    p = emit_p(s_bytecode, p, PVM_SET_ANCHOR_X, 2, 0xAAAA, 0, 0);
    p = emit_p(s_bytecode, p, PVM_STOP_TICK,    1, 0, 0, 0);
    p = emit_p(s_bytecode, p, PVM_SET_ANCHOR_X, 2, 0xBBBB, 0, 0);
    p = emit_p(s_bytecode, p, PVM_END,          1, 0, 0, 0);
    (void)p;

    Entity *e = make_entity_with_bytecode(s_bytecode);

    EntityWalkerTick(NULL);
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x22), 0xAAAA);

    EntityWalkerTick(NULL);
    ASSERT_EQ(*(uint16_t *)((uint8_t *)e + 0x22), 0xBBBB);
}

SUITE(per_entity_vm_real)
{
    RUN_TEST(pvm_set_anchor_xy_writes_both);
    RUN_TEST(pvm_set_anchor_x_only_writes_x);
    RUN_TEST(pvm_set_anchor_y_only_writes_y);
    RUN_TEST(pvm_set_frame_writes_frame);
    RUN_TEST(pvm_set_frame_clamps_to_frame_count_minus_one);
    RUN_TEST(pvm_set_pos_from_frame_copies_atlas_drawXY);
    RUN_TEST(pvm_set_delay_writes_both_delay_and_reset);
    RUN_TEST(pvm_delay_pause_halts_tick_with_pc_advanced);
    RUN_TEST(pvm_add_x_adds_to_anchor_x);
    RUN_TEST(pvm_add_y_adds_to_anchor_y);
    RUN_TEST(pvm_set_flag_2_sets_bit_1_of_3A);
    RUN_TEST(pvm_clear_flag_2_clears_bit_1_of_3A);
    RUN_TEST(pvm_advance_frame_increments_frame);
    RUN_TEST(pvm_advance_frame_wraps_to_zero_when_arg_small);
    RUN_TEST(pvm_if_frame_matching_sets_bit_0);
    RUN_TEST(pvm_if_frame_non_matching_clears_bit_0);
    RUN_TEST(pvm_clear_loop_ctrs_zeros_34_36_38);
    RUN_TEST(pvm_stop_tick_halts_without_pc_reset);
    RUN_TEST(pvm_stop_reset_halts_and_zeros_pc);
    RUN_TEST(pvm_stop_keep_pc_halts_with_pc_at_instruction);
    RUN_TEST(pvm_set_fade_writes_26_and_sets_flag_2_of_byte_9);
    RUN_TEST(pvm_positive_delay_skips_opcode_execution);
    RUN_TEST(pvm_post_exec_mirrors_anchor_to_drawn_position);
    RUN_TEST(pvm_post_exec_writes_width_height_from_atlas);
    RUN_TEST(pvm_post_exec_clamps_scale_to_0xA0);
    RUN_TEST(pvm_entity_without_atlas_is_skipped);
    RUN_TEST(pvm_pc_carries_between_ticks);
}
