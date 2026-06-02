/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/vm/opcodes.h — main script VM opcode constants.
 *
 * The Wacki bytecode VM (RunScriptInterpreter) handles 78 opcodes in
 * the range 0x00..0x57. Names match the engine's documented behaviour
 * of each instruction.
 *
 * Instruction stride: each instruction is `len * 4` bytes, where `len`
 * (in DWORDS) lives at byte +1 of the instruction. Operands follow
 * the header word: `a0` at byte +2, `a1` at +4, `a2` at +6.
 *
 * This header is included only by src/vm/main.c. Other code that
 * mentions specific opcode numbers (the per-entity VM, click dispatch
 * helpers, etc.) reaches them by name through the same pattern.
 */
#ifndef WACKI_VM_OPCODES_H
#define WACKI_VM_OPCODES_H

/* ---- conditionals (0x00..0x07) ------------------------------------ */
#define OP_SKIP_TO_END          0x00    /* unconditional skip to ENDIF */
#define OP_IF_NE                0x01    /* skip if var == imm  */
#define OP_IF_GT                0x02    /* skip if var <= imm  */
#define OP_IF_LT                0x03    /* skip if var >= imm  */
#define OP_IF_EQ                0x04    /* skip if var != imm  */
#define OP_IF_ALL_BITS_SET      0x05    /* skip if (var & imm) != imm */
#define OP_ENDIF                0x06
#define OP_ELSE                 0x07

/* ---- entity / actor I/O (0x09..0x12) ------------------------------ */
#define OP_SHOW_TEXT            0x09    /* speech balloon (mostly dead code) */
#define OP_VAR_OR               0x0A
#define OP_SET_ACTOR_FLAG2      0x0B
#define OP_VAR_AND_NOT          0x0C
#define OP_VAR_SET              0x0D
#define OP_SET_ENTITY_SCRIPT    0x0E
#define OP_SET_ENTITY_ANIM      0x0F
#define OP_WALK_EBEK            0x10
#define OP_WALK_FJEJ            0x11
#define OP_WALK_BOTH            0x12

/* ---- wait family (0x13..0x16, 0x26, 0x3D) ------------------------- */
#define OP_FRAME_TICK           0x13    /* advance one frame */
#define OP_WAIT_MS              0x14    /* wait N ms ticks */
#define OP_WAIT_ENTITY_IDLE     0x15
#define OP_LABEL                0x16    /* jump target for op 0x17 */
#define OP_JUMP_LABEL           0x17
#define OP_LOOP_COUNTED         0x18
#define OP_QUEUE_DIALOG         0x19    /* op 0x19 — dialog choice queue */
#define OP_DIALOG_SHOW          0x1A
#define OP_DIALOG_CLEAR         0x1B
#define OP_DIALOG_CHOICE        0x1C
#define OP_DIALOG_CLOSE         0x1D
#define OP_DIALOG_TBD           0x1F
#define OP_GO_EXIT              0x20
#define OP_RET_REG_DEFAULT      0x21    /* g_return_reg = default-actor */
#define OP_RET_REG_THAT_ID      0x22
#define OP_INV_HAS_ITEM         0x23    /* g_return_reg = InventoryHasItem(...) */
#define OP_CALL_SUB             0x24    /* tail-call into PE-resolved bytecode */
#define OP_TAILCALL             0x25
#define OP_WAIT_ANIM_FRAME      0x26    /* wait until entity[+0x30] == target */
#define OP_SET_TAGGED_FIELD     0x27
#define OP_SET_ENTITY_XY        0x28
#define OP_GET_CUR_ROOM         0x29    /* g_return_reg = current komnata id */
#define OP_WACKI_RAND           0x2A    /* g_return_reg = WackiRand(a0) */
#define OP_SECONDARY_SCRIPT     0x2B
#define OP_BG_MASK_SETUP        0x2C    /* room walk-area + walk-behind */
#define OP_LOAD_ASSET           0x2D
#define OP_REG_MASK_LIST        0x2E    /* op 0x2E — mask list register */
#define OP_REG_VERB_MASK        0x2F    /* op 0x2F — verb-table mask list */
#define OP_SPAWN_ENTITY         0x30
#define OP_DESTROY_ENT_A        0x31
#define OP_DESTROY_ENT_B        0x32
#define OP_RESET_ACTORS         0x33
#define OP_GET_ACTIVE_ACTOR     0x34    /* g_return_reg = g_active_actor + 1 */
#define OP_WALK_MODE_A          0x35
#define OP_WALK_MODE_B          0x37
#define OP_WALK_TO_BY_ID        0x38
#define OP_WALK_TO_SELF         0x3A
#define OP_ATTACH_PROP_A        0x3B
#define OP_ATTACH_PROP_B        0x3C
#define OP_HIDE_ENTITY          0x3E
#define OP_SHOW_ENTITY          0x3F
#define OP_SET_PERSPECTIVE      0x40    /* op 0x40 — script-driven persp bias */
#define OP_SOUND_PLAY           0x41
#define OP_SOUND_STOP           0x42
#define OP_NOP_E6C4_A           0x43    /* dead state, kept for opcode-count fidelity */
#define OP_NOP_E6C4_B           0x44    /* dead state */
#define OP_TIMER_RESET          0x45
#define OP_TIMER_READ           0x46
#define OP_MOVE_ENTITY_DELTA    0x47
#define OP_PAL_LOAD_FADE        0x48
#define OP_PAL_FADE_STEP        0x49
#define OP_PAL_LOAD_INSTANT     0x4A
#define OP_QUERY_ENTITY_X       0x4B
#define OP_QUERY_ENTITY_Y       0x4C
#define OP_VAR_ADD              0x4D
#define OP_VAR_SUB              0x4E
#define OP_RETURN_FALSE         0x4F
#define OP_SUBANIM_HIDE_TOGGLE  0x50
#define OP_SUBANIM_SET_PARAM    0x51
#define OP_DIALOG_PUSH          0x52
#define OP_DIALOG_PLAY          0x53
#define OP_SHOW_PICTURE         0x54
#define OP_END_HARD             0x55    /* terminates execution but NOT scanning */
#define OP_END                  0x56    /* terminates execution AND scanning */
#define OP_WAIT_KIND2_FRAME     0x3D    /* declared late: op 0x3D — wait variant */

/* ---- magic verb sentinel ------------------------------------------ */
#define NEUTRAL_VERB            0x26    /* "no-op" verb id; default hover value */

/* ---- script-var register conventions ------------------------------ */
#define SCRIPT_VAR_RETURN_REG   4       /* var[4] = g_return_reg alias */
#define SCRIPT_VAR_WALK_INTR    4       /* var[4] = lmb_clicked walk-interrupt */
#define SCRIPT_VAR_GAME_OVER    14      /* var[14] = g_game_over_code */
#define SCRIPT_VAR_COMPLETED    17      /* var[17] = stage-completion bitmap */
#define SCRIPT_VAR_HOVER_VERB   0x0F    /* var[0x0F] = hover panel verb (world-pickup) */

#endif /* WACKI_VM_OPCODES_H */
