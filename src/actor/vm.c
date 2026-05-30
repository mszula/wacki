/* src/actor/vm.c — per-entity script interpreter and walker tick.
 *
 * Every entity in the scene carries a small bytecode program that
 * drives its animation, position, and timing. The "per-entity VM"
 * dispatches one frame's worth of those instructions for every
 * registered entity each game tick:
 *
 *   EntityWalkerTick()
 *     → walks the render list (kind=0)
 *     → for each entity with an atlas bound: ExecEntityScript()
 *
 * Bytecode format is `[op:u8 +0][dlt:u8 +1][operand:u16 +2]…` with a
 * stride of `dlt * 2` bytes (HALF of the main script VM's `len * 4`).
 * Opcodes 0x00..0x24 cover anchor moves, frame stepping, walker setup,
 * stops, oscillators, atlas swaps, and the click-event enqueue. END is
 * 0x21; LABEL targets are 0x0A.
 *
 * The frame-delta gate at the top of ExecEntityScript yields to the
 * next tick when +0x3C (delay) is still positive — this is what makes
 * animation timing scale with frame pacing.
 *
 * Post-execution always runs the "tidy-up" block: mirrors anchor →
 * drawn position, stashes atlas frame W/H, clamps scale, and applies
 * foot-anchor compensation.
 */

#include "wacki.h"
#include "internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern uint32_t g_frame_delta_ms;
extern uint32_t g_tick_counter;
extern uint16_t g_frame_delta_ticks;

/* Scan one bytecode block for a 0x16 LABEL with matching arg, OR for a
 * matching subroutine entry (op==0x0A label). 1:1 with the inner scan loop
 * that opcodes 0x13/0x19/0x1A use in the original (cases share the same
 * `cVar13/uVar11/uVar14` walking pattern).
 *
 * Returns the new pc (in halfwords from base), or 0 if not found (the
 * caller then sets pc=0 = re-start). If `id == 0xFFFF` matches the first
 * 0x0A encountered (1:1 with the original's `uVar19 == 0xffff` shortcut). */
static uint16_t scan_for_label(const uint8_t *bytecode, uint16_t target_id)
{
    uint16_t pc = 0;
    for (int safety = 0; safety < 2048; ++safety) {
        const uint8_t *p = bytecode + (size_t)pc * 2;
        uint8_t  op  = p[0];
        if (op == '!')  /* 0x21 END */
            return 0;
        if (op == '\n') { /* 0x0A LABEL */
            uint16_t arg = (uint16_t)(p[2] | (p[3] << 8));
            if (target_id == 0xFFFF || arg == target_id)
                return pc;
        }
        uint8_t dlt = p[1];
        if (dlt == 0) return 0;
        pc = (uint16_t)(pc + dlt);
    }
    return 0;
}

static void ExecEntityScript(Entity *e)
{
    if (!e) return;
    AnimAsset *atlas = (AnimAsset *)ent_ptr_resolve(EOFF(e, 0x28, uint32_t));
    if (!atlas) return;
    const uint8_t *bytecode = (const uint8_t *)ent_ptr_resolve(EOFF(e, 0x2C, uint32_t));
    if (!bytecode) return;

    uint16_t pc = EOFF(e, 0x32, uint16_t);
    int bVar3 = 1, bVar4 = 0;
    const int16_t *local_18 = NULL;        /* opcode 3 target table */
    const int16_t *local_14 = NULL;        /* opcode 4 target table */

    /* Frame-delta gate — 1:1 with the lead-in code that decrements +0x3C
     * by DAT_0044e578 and only executes when it reaches <= 0. Same field
     * is then optionally re-loaded from +0x3E (period/delay_reset).
     *
     * Uses g_frame_delta_ticks (10 ms units = DAT_0044E578) not raw ms.
     * The +0x3E period is set by op 0x09 from a script literal — script
     * authors specified it in ticks (the only unit +0x3C is ever compared
     * against in the PE). Subtracting raw ms burned through periods 10×
     * too fast and that's why entity animations (props, NPCs, walking
     * cycles) ran accelerated. */
    int16_t delay = (int16_t)EOFF(e, 0x3C, uint16_t);
    delay = (int16_t)(delay - (int16_t)g_frame_delta_ticks);
    EOFF(e, 0x3C, uint16_t) = (uint16_t)delay;
    if (delay > 0) goto post_exec;
    {
        int16_t reset = (int16_t)EOFF(e, 0x3E, uint16_t);
        EOFF(e, 0x3C, uint16_t) = (uint16_t)(delay + reset);
        if (reset == 0) EOFF(e, 0x3C, uint16_t) = 0;
    }

    /* ------ main interpreter loop --------------------------------------- */
    /* Safety counter — bail after 4096 iterations to guarantee we never
     * wedge the whole game on a malformed script (e.g. unresolved
     * SUBSCRIPT_CALL or backward jump with no STOP). The original VM
     * has no such bound, but it also can rely on every script having a
     * reachable STOP_TICK; ours can't (some referenced targets aren't
     * embedded yet → they'd loop). */
    int safety = 4096;
    while (bVar3 && safety-- > 0) {
        /* T-bytecode-refresh: 1:1 with original FUN_004012E0 which re-reads
         * pcVar7 = *(char **)(param_1 + 0x2c) at TOP of each loop iter.
         * After op 0x1E SUBSCRIPT_CALL changes +0x2C to the new bytecode,
         * the NEXT iter must read the NEW bytecode. Earlier port cached
         * `bytecode` once at function entry — loop continued with OLD
         * bytecode after SUBSCRIPT_CALL, advancing pc through OLD ops
         * until STOP_TICK / safety bail. Resulting pc value (e.g. 12
         * after entry [5]'s STOP_TICK) then landed mid-instruction in
         * the NEW bytecode on the following tick — skipping the entire
         * preamble (SET_DELAY 8 + SET_DELAY_AND_PAUSE 6000 + SET_FRAME 72
         * in the idle bytecode) and jumping straight into the cycling
         * frame-advance section. Hence "actors walk in place facing
         * camera instead of standing still". */
        bytecode = (const uint8_t *)ent_ptr_resolve(EOFF(e, 0x2C, uint32_t));
        if (!bytecode) break;
        const uint8_t *p = bytecode + (size_t)pc * 2;
        uint8_t  op   = p[0];
        uint8_t  dlt  = p[1];
        uint16_t arg  = (uint16_t)(p[2] | (p[3] << 8));
        uint16_t next_pc = (uint16_t)(pc + dlt);

        #define ACT_LOG(label, ny) ((void)0)
        switch (op) {
        case 0x00:                                  /* SET_ANCHOR_XY */
            ACT_LOG("0x00 SET_ANCHOR_XY", (uint16_t)(p[4] | (p[5] << 8)));
            EOFF(e, 0x22, uint16_t) = arg;
            EOFF(e, 0x24, uint16_t) = (uint16_t)(p[4] | (p[5] << 8));
            bVar4 = 1; break;
        case 0x01:                                  /* SET_ANCHOR_X */
            EOFF(e, 0x22, uint16_t) = arg; bVar4 = 1; break;
        case 0x02:                                  /* SET_ANCHOR_Y */
            ACT_LOG("0x02 SET_ANCHOR_Y", arg);
            EOFF(e, 0x24, uint16_t) = arg; bVar4 = 1; break;

        case 0x03: {                                /* X_OSCILLATE — 1:1 with
            * Ghidra case 3:
            *   local_18 = *(short **)(pc + 4);        // PE VA
            *   if (entity[+0x40] < *local_18) entity[+0x40]++;
            *   else                            entity[+0x40] = 1;
            *
            * Table is a PE binary pointer — must be resolved through
            * the PE loader's xlat_binary_ptr() because the binary image
            * isn't loaded at its original VA on macOS. Earlier port
            * cast the VA as a raw pointer and would deref invalid
            * memory (or crash) for any script using oscillation. */
            extern const void *xlat_binary_ptr(uint32_t addr);
            extern int  PeLoaderContainsVA(uint32_t va);
            uint32_t tbl_addr; memcpy(&tbl_addr, p + 4, 4);
            local_18 = PeLoaderContainsVA(tbl_addr)
                       ? (const int16_t *)xlat_binary_ptr(tbl_addr)
                       : (const int16_t *)(uintptr_t)tbl_addr;
            uint16_t idx40 = EOFF(e, 0x40, uint16_t);
            uint16_t first = local_18 ? (uint16_t)local_18[0] : 0;
            if ((int)idx40 < (int)first) EOFF(e, 0x40, uint16_t) = idx40 + 1;
            else                          EOFF(e, 0x40, uint16_t) = 1;
            break;
        }
        case 0x04: {                                /* Y_OSCILLATE — see 0x03 */
            extern const void *xlat_binary_ptr(uint32_t addr);
            extern int  PeLoaderContainsVA(uint32_t va);
            uint32_t tbl_addr; memcpy(&tbl_addr, p + 4, 4);
            local_14 = PeLoaderContainsVA(tbl_addr)
                       ? (const int16_t *)xlat_binary_ptr(tbl_addr)
                       : (const int16_t *)(uintptr_t)tbl_addr;
            uint16_t idx42 = EOFF(e, 0x42, uint16_t);
            uint16_t first = local_14 ? (uint16_t)local_14[0] : 0;
            if ((int)idx42 < (int)first) EOFF(e, 0x42, uint16_t) = idx42 + 1;
            else                          EOFF(e, 0x42, uint16_t) = 1;
            break;
        }
        case 0x05: {                                /* SET_POS_FROM_FRAME
             * 1:1 with FUN_004012E0 case 5:
             *   entity[+0x22] = drawX[entity[+0x30]];
             *   entity[+0x24] = drawY[entity[+0x30]];
             *
             * Bounds check `fid < frame_count` is defensive — op 0x23
             * SWAP_ATLAS_BY_ID preserves entity[+0x30] across atlas swap
             * (1:1 with FUN_00407600 save/restore), so a previous
             * high-frame index on a smaller-frame new atlas could OOB
             * read `off_drawX[fid]`. Original 32-bit engine had same
             * potential issue; we clamp defensively on 64-bit. */
            uint16_t fid = EOFF(e, 0x30, uint16_t);
            if (atlas->frame_count && fid >= atlas->frame_count)
                fid = (uint16_t)(atlas->frame_count - 1);
            if (atlas->off_drawY) ACT_LOG("0x05 SET_POS_FROM_FRAME", atlas->off_drawY[fid]);
            if (atlas->off_drawX) EOFF(e, 0x22, uint16_t) = atlas->off_drawX[fid];
            if (atlas->off_drawY) EOFF(e, 0x24, uint16_t) = atlas->off_drawY[fid];
            break;
        }
        case 0x07:                                  /* IF_FRAME — sets bit 0 */
            if (arg == EOFF(e, 0x30, uint16_t)) EOFF8(e, 0x3A) |= 1;
            else                                EOFF(e, 0x3A, uint16_t) &= ~1u;
            break;
        case 0x08: {                                /* FRAME_RANGE_CHECK */
            uint16_t fid = EOFF(e, 0x30, uint16_t);
            uint16_t lo = (uint16_t)(p[4] | (p[5] << 8));
            uint16_t hi = (uint16_t)(p[6] | (p[7] << 8));
            if (lo <= fid && fid <= hi) arg = fid;
        } /* FALLTHRU */
        /* fallthrough */
        case 0x06: {                                /* SET_FRAME
             * 1:1 with original case 6 (reached also via case 8 fall-through):
             *   if (asset->frame_count <= arg) arg = asset->frame_count - 1;
             *   entity[+0x30] = arg;
             *   bVar4 = true;
             *   if (asset->anim_script != NULL)
             *       FUN_00401100(asset->anim_script, arg);  // per-frame SFX
             *
             * The FUN_00401100 call dispatches to FUN_0040A1F0 (sample
             * trigger) using the asset's [sampl]-table. Our port mirrors
             * this with TriggerFrameSfx by asset name. */
            uint16_t fc = atlas->frame_count;
            if (fc && arg >= fc) arg = (uint16_t)(fc - 1);
            EOFF(e, 0x30, uint16_t) = arg;
            bVar4 = 1;
            /* PORT SHORTCUT (refer FUN_004012E0 case 6): original gates the
             * sampl-trigger on `asset->anim_script != NULL` because each
             * asset has a per-asset [sampl] table attached at load time.
             * Our port keeps a SCENE-WIDE g_dynamic_sfx[] (populated by
             * ParseSamplTagsForKomnata) keyed by asset name + frame, so
             * atlas->anim_script is unused. Calling TriggerFrameSfx
             * unconditionally — it's a no-op when the asset+frame combo
             * has no pool entries. Without this, frame-0 SFX set by SET_FRAME
             * in the asset's first per-entity script tick (e.g. desok1.wyc
             * frame 0 = Dskfik/Dsk_fik pool) silently dropped. */
            extern void TriggerFrameSfx(const char *asset_name, int frame);
            TriggerFrameSfx(atlas->name, (int)arg);
            break;
        }
        case 0x09:                                  /* SET_DELAY */
            EOFF(e, 0x3C, uint16_t) = arg;
            EOFF(e, 0x3E, uint16_t) = arg;
            break;
        case 0x0B:                                  /* CLEAR_LOOP_CTRS */
            EOFF(e, 0x34, uint16_t) = 0;
            EOFF(e, 0x36, uint16_t) = 0;
            EOFF(e, 0x38, uint16_t) = 0;
            break;
        case 0x0C: case 0x0D: case 0x0E: {          /* LOOP_A / B / C */
            uint16_t off = (op == 0x0C) ? 0x34 : (op == 0x0D) ? 0x36 : 0x38;
            uint16_t cnt = EOFF(e, off, uint16_t);
            if ((uint32_t)(cnt + 1) < arg) {
                /* T122 — 1:1 with FUN_004012E0 case 0x0C/D/E loop body:
                 *   uVar18 = scan_label(want);  // 0 if not found
                 *   ...
                 *   uVar14 = uVar18;              // unconditional assign
                 *
                 * Earlier port had `if (found != 0 || want < 0) next_pc = found;`
                 * — only assigning when found OR want<0. Original always
                 * assigns (so not-found → wraps to pc=0). Wrap-on-miss is
                 * the intended behavior for malformed scripts. */
                int16_t want = (int16_t)(p[4] | (p[5] << 8));
                uint16_t found = scan_for_label(bytecode,
                                                want < 0 ? 0xFFFF : (uint16_t)want);
                next_pc = found;
                EOFF(e, off, uint16_t) = (uint16_t)(cnt + 1);
            } else {
                EOFF(e, off, uint16_t) = 0;
            }
            break;
        }
        case 0x0F: {                                /* SET_RAND_FRAME
             * 1:1 with FUN_004012E0 case 0x0F:
             *   if (asset->frame_count < arg) arg = asset->frame_count;
             *   frame = FUN_00410F50(arg);
             *   entity[+0x30] = frame;
             *   bVar4 = true;
             *   if (asset->anim_script != NULL)
             *       FUN_00401100(asset->anim_script, frame);  // per-frame SFX
             *
             * The SFX trigger fires when frame changes randomly (e.g.
             * pies.wyc dog barking at random frames). Was missing —
             * frame-set ops would skip the bark sound. */
            uint16_t fc = atlas->frame_count;
            if (fc && arg > fc) arg = fc;
            uint16_t f = (uint16_t)WackiRand(arg);
            EOFF(e, 0x30, uint16_t) = f;
            bVar4 = 1;
            /* Same gate-removal rationale as op 0x06 above — call
             * TriggerFrameSfx unconditionally; it's a no-op when no
             * pool entries match. */
            extern void TriggerFrameSfx(const char *asset_name, int frame);
            TriggerFrameSfx(atlas->name, (int)f);
            break;
        }
        case 0x10:                                  /* SET_DELAY_AND_PAUSE */
            EOFF(e, 0x3C, uint16_t) = arg;
            /* fallthrough */
        case 0x1D:                                  /* STOP_TICK */
            bVar3 = 0;
            break;
        case 0x11: {                                /* SET_RAND_DELAY — 1:1
             * with FUN_004012E0 case 0x11:
             *   delay = FUN_00410F50(arg);
             *   entity[+0x3C] = delay;
             *   bVar3 = false;
             * Picks a random delay in [0, arg) then yields. */
            uint16_t d = (uint16_t)WackiRand(arg);
            EOFF(e, 0x3C, uint16_t) = d;
            bVar3 = 0;
            break;
        }
        case 0x12: {                                /* ADVANCE_FRAME */
            EOFF8(e, 0x3A) |= 1;
            uint16_t f = (uint16_t)(EOFF(e, 0x30, uint16_t) + arg);
            EOFF(e, 0x30, uint16_t) = f;
            if (atlas->frame_count && f >= atlas->frame_count) {
                if (arg < 0x80) EOFF(e, 0x30, uint16_t) = 0;
                else            EOFF(e, 0x30, uint16_t) = (uint16_t)(atlas->frame_count - 1);
                EOFF(e, 0x3A, uint16_t) &= ~1u;
            }
            bVar4 = 1;
            /* Per-frame sound trigger — 1:1 with the original's
             * FUN_00401100 → FUN_0040A1F0 path that fires the [sampl]
             * tag-matched .wav for the new frame. */
            extern void TriggerFrameSfx(const char *asset_name, int frame);
            TriggerFrameSfx(atlas->name, (int)EOFF(e, 0x30, uint16_t));
            break;
        }
        case 0x13: {                                /* WAIT_FRAME (jump-by-label) */
            uint16_t found = scan_for_label(bytecode, arg);
            next_pc = found;
            break;
        }
        case 0x14: {                                /* RAND_GATE — 1:1 with
            * Ghidra case 0x14: `if (FUN_00410F50(2) == 0) jump_to_label`.
            * WackiRand is the 1:1 port (ROL3 + 0x3D8A479C, time-seeded). */
            if (WackiRand(2) == 0) {
                uint16_t found = scan_for_label(bytecode, arg);
                next_pc = found;
            }
            break;
        }
        case 0x15:                                  /* WALK_TO_X (+0x44 path)
             * Instruction width verified via Ghidra byte-pattern search
             * (B32 audit): every op 0x15 and 0x16 instance in PE bytecode
             * (range 0x00423000-0x00440000) carries `len=4` (= 8 bytes).
             *
             *   op 0x15 layout:  [op:1][len=4:1][X:2][Y:2][step:2][pad:2]
             *   op 0x16 layout:  [op:1][len=4:1][X:2][step:2][pad:4]
             *                                              ^^^^^^^^^^^^^
             *                          op 0x16 reads Y from entity[+0x24]
             *
             * Reading p[4..5] / p[6..7] is in-bounds for all observed
             * instructions. No len<4 variants in shipped scripts. */
        case 0x16: {                                /* WALK_TO_XY */
            EOFF8(e, 0x3A) |= 1;
            /* If no path yet, plant a new one via FUN_00401150 (Bresenham). */
            uint32_t dxr = EOFF(e, 0x4C, uint32_t);
            uint32_t dyr = EOFF(e, 0x50, uint32_t);
            if (dxr == 0 && dyr == 0) {
                int16_t tx = (int16_t)arg;
                int16_t ty = (op == 0x15) ? (int16_t)(p[4] | (p[5] << 8))
                                          : (int16_t)EOFF(e, 0x24, uint16_t);
                int16_t cx = (int16_t)EOFF(e, 0x22, uint16_t);
                int16_t cy = (int16_t)EOFF(e, 0x24, uint16_t);
                int16_t sdx = tx - cx, sdy = ty - cy;
                int16_t adx = sdx < 0 ? -sdx : sdx;
                int16_t ady = sdy < 0 ? -sdy : sdy;
                int16_t maxlen = adx > ady ? adx : ady;
                int32_t inc_x = 0, inc_y = 0;
                if (maxlen) {
                    inc_x = (int32_t)((tx - cx) * 0x10000) / maxlen;
                    inc_y = (int32_t)((ty - cy) * 0x10000) / maxlen;
                }
                /* uint32 shift — 1:1 bit pattern with original
                 * `param_1 << 0x10` @ FUN_00401150, but well-defined for
                 * cx/cy < 0 (off-screen entry). See stubs.c BindActorWalker. */
                EOFF(e, 0x44, int32_t) = (int32_t)((uint32_t)(uint16_t)cx << 16);
                EOFF(e, 0x48, int32_t) = (int32_t)((uint32_t)(uint16_t)cy << 16);
                EOFF(e, 0x4C, int32_t) = inc_x;
                EOFF(e, 0x50, int32_t) = inc_y;
                EOFF(e, 0x54, int16_t) = tx;
                EOFF(e, 0x56, int16_t) = ty;
                EOFF(e, 0x3A, uint16_t) &= ~4u;
            }
            uint16_t step = (op == 0x15) ? (uint16_t)(p[6] | (p[7] << 8))
                                         : (uint16_t)(p[4] | (p[5] << 8));
            if (EOFF8(e, 9) & 4) step = (uint16_t)((EOFF(e, 0x58, uint16_t) * step) / 100);
            if ((int16_t)step == 0) step = 1;
            /* PORT — aliasing-safe step loop. EOFF(e, 0x4A, int16_t) reads
             * upper 16 bits of int32 at +0x48; same memory aliased through
             * different types. Compiler under -fstrict-aliasing may assume
             * `int32_t += ...` followed by `int16_t == ...` reads stale
             * value → equality check fires on wrong iteration → walker
             * overshoots target by 1px each tick (Fjej-weź-kwiatka bug:
             * target Y=323 reached as Y=322 due to compiler reordering,
             * walker then re-plants because +0x50 wasn't zeroed, and
             * continues stepping past target). Force per-iter local
             * variable copy so each comparison reads CURRENT state. */
            for (uint16_t k = 0; k < step; ++k) {
                int16_t pre_x = EOFF(e, 0x46, int16_t);
                int16_t tgt_x = EOFF(e, 0x54, int16_t);
                if (pre_x == tgt_x)
                    EOFF(e, 0x4C, uint32_t) = 0;
                else
                    EOFF(e, 0x44, int32_t) += EOFF(e, 0x4C, int32_t);
                int16_t pre_y = EOFF(e, 0x4A, int16_t);
                int16_t tgt_y = EOFF(e, 0x56, int16_t);
                if (pre_y == tgt_y)
                    EOFF(e, 0x50, uint32_t) = 0;
                else
                    EOFF(e, 0x48, int32_t) += EOFF(e, 0x50, int32_t);
            }
            EOFF(e, 0x24, uint16_t) = EOFF(e, 0x4A, uint16_t);
            EOFF(e, 0x22, uint16_t) = EOFF(e, 0x46, uint16_t);
            if (EOFF(e, 0x4C, uint32_t) == 0 && EOFF(e, 0x50, uint32_t) == 0) {
                EOFF(e, 0x3A, uint16_t) &= ~1u;
            }
            /* T123 — original FUN_004012E0 case 0x15/0x16 tail @
             * LAB_00401779 unconditionally sets bVar4 = true (= "frame
             * was touched"). Earlier port only set it on walk-done;
             * walking-mid path missed the post-exec block triggered by
             * bVar4 (clearing +0x26 when flag 0x40 set — minor UX
             * glitch on fading entities). */
            bVar4 = 1;
            break;
        }
        case 0x17:                                  /* ADD_X */
            EOFF(e, 0x22, uint16_t) = (uint16_t)(EOFF(e, 0x22, uint16_t) + arg);
            break;
        case 0x18:                                  /* ADD_Y */
            ACT_LOG("0x18 ADD_Y", (int16_t)(EOFF(e, 0x24, int16_t) + (int16_t)arg));
            EOFF(e, 0x24, uint16_t) = (uint16_t)(EOFF(e, 0x24, uint16_t) + arg);
            break;
        case 0x19:                                  /* JUMP_IF_BIT0 */
            if (EOFF8(e, 0x3A) & 1) {
                uint16_t found = scan_for_label(bytecode, arg);
                next_pc = found;
            }
            break;
        case 0x1A:                                  /* JUMP_IF_NOT_BIT0 */
            if (!(EOFF8(e, 0x3A) & 1)) {
                uint16_t found = scan_for_label(bytecode, arg);
                next_pc = found;
            }
            break;
        case 0x1B:                                  /* SET_FLAG_2 */
            EOFF8(e, 0x3A) |= 2; break;
        case 0x1C:                                  /* CLEAR_FLAG_2 */
            EOFF(e, 0x3A, uint16_t) &= ~2u; break;
        case 0x1E: {                                /* SUBSCRIPT_CALL —
            * 1:1 with FUN_00402500 + entity[+0x2c] = new_bc + pc reset.
            *
            * Unresolved-target safety: if xlat returns NULL (the subroutine
            * isn't embedded in our binary_data table yet), we MUST NOT keep
            * the old bytecode + reset pc to 0 — that re-executes this same
            * instruction next iteration → game hangs. Terminate the tick
            * cleanly instead (bVar3=0), leaving the entity's state intact
            * so the next walker tick re-tries from the same point. */
            uint32_t addr; memcpy(&addr, p + 4, 4);
            extern const void *xlat_binary_ptr(uint32_t);
            const void *new_bc = xlat_binary_ptr(addr);
            if (!new_bc) {
                /* Subroutine isn't embedded — terminate tick (script stays
                 * pointed at current bytecode). Avoids infinite-loop hang
                 * when entity script tail-calls an unembedded address. */
                bVar3 = 0;
                next_pc = pc;
                break;
            }
            /* FUN_00402500 reset, then load new bytecode from operand. */
            EOFF(e, 0x3A, uint16_t) &= ~5u;
            EOFF(e, 0x38, uint16_t) = 0;
            EOFF(e, 0x36, uint16_t) = 0;
            EOFF(e, 0x34, uint16_t) = 0;
            EOFF(e, 0x3C, uint16_t) = 0;
            EOFF(e, 0x42, uint16_t) = 0;
            EOFF(e, 0x40, uint16_t) = 0;
            EOFF(e, 0x32, uint16_t) = 0;
            EOFF(e, 0x50, uint32_t) = 0;
            EOFF(e, 0x4C, uint32_t) = 0;
            EOFF(e, 0x2C, uint32_t) = ent_ptr_intern((void *)new_bc);
            next_pc = 0;
            break;
        }
        case 0x1F: case 0x21:                       /* STOP / END_RESET */
            bVar3 = 0;
            next_pc = 0;
            break;
        case 0x20:                                  /* STOP_KEEP_PC */
            bVar3 = 0;
            next_pc = pc;
            break;
        case 0x22: {
            /* 1:1 with FUN_004012E0 case 0x22:
             *
             *   DAT_0044a1a0[DAT_0044a1c8].obj  = arg1;   // pc+2
             *   DAT_0044a1a0[DAT_0044a1c8].verb = arg2;   // pc+4
             *   ++DAT_0044a1c8;
             *
             * ProcessGameFrameTick later drains this queue (up to 10
             * entries) by calling DispatchClickEvent(obj, verb) on each.
             *
             * We use a 1-slot deferred queue so we don't reenter the
             * main VM mid-entity-tick (which can mutate the entity list
             * we're iterating over). FlushQueuedClicks drains at end of
             * the frame, mirroring the original drain at the tail of
             * ProcessGameFrameTick. */
            extern void EnqueueClickEvent(uint16_t obj, uint16_t verb);
            uint16_t obj  = arg;
            uint16_t verb = (uint16_t)(p[4] | (p[5] << 8));
            EnqueueClickEvent(obj, verb);
            break;
        }
        case 0x23: {
            /* SWAP_ATLAS_BY_ID — 1:1 with per-entity FUN_004012E0 case 0x23:
             *   FUN_004076b0(entity, arg):
             *     iVar1 = FUN_00405d80(1, arg);              // FindUpdateRegistration(1, id)
             *     if (iVar1) FUN_00407600(entity, iVar1);    // bind new atlas
             *   FUN_00402500(entity);                         // reset state
             *
             * FUN_00407600 (binder): writes new atlas ptr into entity[+0x28]
             * and resets the per-entity script pc + delay counters.
             *
             * Earlier port did only the FUN_00402500 reset and missed the
             * atlas swap entirely — scripts using op 0x23 to change atlas
             * at runtime had no visible effect (entity kept old atlas).
             *
             * pc/delay reset overlaps the FUN_00407600 atlas-bind partial
             * reset; we just do the FULL FUN_00402500 reset after binding. */
            extern void *FindUpdateRegistration(uint16_t kind, uint16_t id);
            extern uint32_t ent_ptr_intern(void *p);
            AnimAsset *new_atlas = (AnimAsset *)FindUpdateRegistration(1, arg);
            if (new_atlas) {
                /* FUN_00407600 atlas swap: entity[+0x28] = asset slot;
                 * entity[+0x32] = 0 (pc), +0x36/+0x3C = 0 (delays). */
                EOFF(e, 0x28, uint32_t) = ent_ptr_intern((void *)new_atlas);
                EOFF(e, 0x32, uint16_t) = 0;
                EOFF(e, 0x36, uint16_t) = 0;
                EOFF(e, 0x3C, uint16_t) = 0;
            }
            /* FUN_00402500 reset — same block used by op 0x1E SUBSCRIPT_CALL. */
            EOFF(e, 0x3A, uint16_t) &= ~5u;
            EOFF(e, 0x38, uint16_t) = 0;
            EOFF(e, 0x36, uint16_t) = 0;
            EOFF(e, 0x34, uint16_t) = 0;
            EOFF(e, 0x3C, uint16_t) = 0;
            EOFF(e, 0x42, uint16_t) = 0;
            EOFF(e, 0x40, uint16_t) = 0;
            EOFF(e, 0x32, uint16_t) = 0;
            EOFF(e, 0x50, uint32_t) = 0;
            EOFF(e, 0x4C, uint32_t) = 0;
            break;
        }
        case 0x24:                                  /* SET_FADE */
            EOFF8(e, 9) |= 2;
            EOFF(e, 0x26, uint16_t) = arg;
            bVar4 = 1;
            break;

        default:
            /* unknown opcode: just advance */
            break;
        }

        pc = next_pc;
    }

    /* ------ post-exec: store frame deltas, compute drawn anchor ---------- */
post_exec:
    /* CLEAR +0x26 if frame-changed AND flag 0x40 set — 1:1 with original
     * LAB_00401a91 first block (Ghidra @ FUN_004012E0):
     *
     *   if (bVar4 && (entity[+8] & 0x40)) {
     *       entity[+0x26] = 0;
     *       entity[+8] |= 0x20;
     *   }
     *
     * Runs at TOP of post-exec, BEFORE the later "+0x26 = +0x10 + +0x0c"
     * computation, so the override at the bottom (if flag 0x200 == 0)
     * wins over this clear when both conditions are true. Earlier port
     * had this AFTER the override — wrong order, would clear +0x26 even
     * for normal entities. */
    if (bVar4 && (EOFF(e, 8, uint16_t) & 0x40)) {
        EOFF(e, 0x26, uint16_t) = 0;
        EOFF(e, 8, uint16_t) |= 0x20;
    }
    EOFF(e, 0x32, uint16_t) = pc;

    /* Mirror anchor → drawn position (a/c) */
    {
        int16_t ax = (int16_t)EOFF(e, 0x22, uint16_t);
        int16_t ay = (int16_t)EOFF(e, 0x24, uint16_t);
        EOFF(e, 0x0A, int16_t) = ax;
        EOFF(e, 0x0C, int16_t) = ay;
        if (local_18) {
            uint16_t idx = EOFF(e, 0x40, uint16_t);
            EOFF(e, 0x0A, int16_t) = (int16_t)(local_18[idx] + ax);
        }
        if (local_14) {
            uint16_t idx = EOFF(e, 0x42, uint16_t);
            EOFF(e, 0x0C, int16_t) = (int16_t)(local_14[idx] + ay);
        }
    }
    /* Stash current frame's w/h from atlas tables. Defensive bounds
     * check — op 0x23 SWAP_ATLAS_BY_ID preserves entity[+0x30] across
     * atlas swap, so stale frame index on a smaller new atlas could OOB. */
    {
        uint16_t fid = EOFF(e, 0x30, uint16_t);
        if (atlas->frame_count && fid >= atlas->frame_count)
            fid = (uint16_t)(atlas->frame_count - 1);
        if (atlas->off_widths)  EOFF(e, 0x0E, uint16_t) = atlas->off_widths [fid];
        if (atlas->off_heights) EOFF(e, 0x10, uint16_t) = atlas->off_heights[fid];
    }
    /* Clamp scale — 1:1 with original `if (0xa0 < entity[+0x58]) entity[+0x58] = 0xa0`. */
    if (EOFF(e, 0x58, uint16_t) > 0xA0) EOFF(e, 0x58, uint16_t) = 0xA0;

    /* Foot-anchor compensation — 1:1 with Ghidra FUN_004012E0 post-exec
     * (after the clamp):
     *
     *   if (entity[+0x3a] & 2) {                  // SET_FLAG_2 active
     *       if ((flags & 0x400) == 0) {            // not perspective-scaled
     *           if ((flags & 4) == 0) {            // not 2x doubled
     *               drawn += atlas_off[frame];     // ×1
     *           } else {
     *               drawn += atlas_off[frame] * 2; // ×2
     *           }
     *       } else {                                // perspective-scaled
     *           drawn += atlas_off[frame] * scale / 100;
     *       }
     *   }
     *
     * Single if/else chain — earlier port had TWO overlapping blocks
     * that BOTH fired for entities with flag 2 set and 0x400 clear,
     * applying foot compensation TWICE. Fixed by merging into one
     * chain matching the original's structure.
     *
     * For sprites like rob1 (the glizda/worm) the per-frame hot-x
     * SHIFTS between frames (frame 0 hot=-6, frame 2 hot=-36) — the
     * sprite slides 30 px to keep the FOOT at the script-set scene
     * anchor while the body cycles through poses. */
    if (EOFF(e, 0x3A, uint16_t) & 2) {
        uint16_t fid   = EOFF(e, 0x30, uint16_t);
        uint16_t flags = EOFF(e, 8, uint16_t);
        if (atlas->off_drawX && atlas->off_drawY && fid < atlas->frame_count) {
            int16_t hx = (int16_t)atlas->off_drawX[fid];
            int16_t hy = (int16_t)atlas->off_drawY[fid];
            uint16_t scale58 = EOFF(e, 0x58, uint16_t);
            /* PORT SHORTCUT (refer FUN_004012E0 post-exec foot-anchor block):
             * Original gates scaled-offset path on flag 0x400 (perspective-
             * scaled entity). Actors in stage 1 don't get flag 0x400 set in
             * the path we've RE'd yet, so they'd always take the ×1 branch
             * → foot-offset at natural size while EntityRenderAll scales the
             * blit (via +0x58 from UpdateActorMovement) → sprite top in
             * wrong place (e.g. Ebek climbing maluch at y=267 scaled to 20%
             * but offset still -108 → sprite drawn 108px above foot anchor
             * = floating in the sky above horizon mask).
             *
             * Compromise: whenever +0x58 != 0 and != 100, the renderer
             * applies scale to the blit — so the foot offset must match.
             * Covers both shrink (scale<100, e.g. Ebek climbing distance)
             * AND grow (scale>100, e.g. partner-freeze leftover scale=160
             * during another actor's action) → without grow-side coverage,
             * sprite blit extends past anchor on the FOOT side, dropping
             * the actor below the HUD line. */
            if (flags & 0x400) {                   /* perspective scaled (1:1) */
                EOFF(e, 0x0A, int16_t) = (int16_t)(EOFF(e, 0x0A, int16_t)
                    + ((int32_t)hx * scale58) / 100);
                EOFF(e, 0x0C, int16_t) = (int16_t)(EOFF(e, 0x0C, int16_t)
                    + ((int32_t)hy * scale58) / 100);
            } else if (flags & 4) {                /* ×2 doubled */
                EOFF(e, 0x0A, int16_t) = (int16_t)(EOFF(e, 0x0A, int16_t) + hx * 2);
                EOFF(e, 0x0C, int16_t) = (int16_t)(EOFF(e, 0x0C, int16_t) + hy * 2);
            } else {                                /* ×1 */
                EOFF(e, 0x0A, int16_t) = (int16_t)(EOFF(e, 0x0A, int16_t) + hx);
                EOFF(e, 0x0C, int16_t) = (int16_t)(EOFF(e, 0x0C, int16_t) + hy);
            }
        }
    }
    /* Bake in the foot_y (+0x26) when not gated by 0x200 flag.
     * 1:1 with original LAB_00401a91 final block:
     *   if ((entity[+8] & 0x200) == 0)
     *       entity[+0x26] = entity[+0x10] + entity[+0x0C];
     *
     * This runs AFTER the top-of-post-exec clear (`bVar4 && flag 0x40`)
     * so when both conditions hit the same tick, this override wins —
     * matching original behavior. */
    if (!(EOFF(e, 8, uint16_t) & 0x200)) {
        EOFF(e, 0x26, int16_t) = (int16_t)(EOFF(e, 0x10, int16_t)
            + EOFF(e, 0x0C, int16_t));
    }
}

#undef ACT_LOG

/* ========================================================================= *
 * EntityWalkerTick — 1:1 with FUN_004012B0 @ 0x004012B0.
 *
 *   FUN_004024d0();                           // refresh DAT_0044e578 (g_frame_delta_ms)
 *   for (e = head; e; e = e->next)
 *       if (e->script_pc_or_kind10 != 0) FUN_004012E0(e);
 *
 * The check `param_1[10] != 0` reads the entity's `kind` field at +0x28
 * (which holds AnimAsset *). If the entity has no atlas, skip it.
 * ========================================================================= */
void EntityWalkerTick(Entity *head)
{
    /* g_frame_delta_ms / g_frame_delta_ticks are now refreshed at the
     * TOP of ProcessGameFrameTickInner, BEFORE this runs, so we read
     * them as-is (1:1 with the original where FUN_004024d0 ran first
     * inside FUN_004025C0). Earlier version did a duplicate refresh
     * here from g_tick_counter, which:
     *   a) re-wrote g_frame_delta_ms with a stale value (last frame's
     *      g_tick_counter end-of-frame snapshot),
     *   b) didn't update g_frame_delta_ticks at all, so post-EntityVM
     *      readers of ticks (UpdateCursorState) got the previous
     *      frame's value. */

    (void)head;
    int n = EntityListCount(0);
    for (int i = 0; i < n; ++i) {
        Entity *e = EntityListAt(0, i);
        if (!e) continue;
        AnimAsset *a = (AnimAsset *)ent_ptr_resolve(EOFF(e, 0x28, uint32_t));
        if (a != NULL)
            ExecEntityScript(e);
    }
}
