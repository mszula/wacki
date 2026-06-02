/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* include/entity_offsets.h — shared Entity byte-offset constants.
 *
 * The script VMs (both main and per-entity) address Entity fields by
 * raw byte offset rather than by named C struct field — the engine's
 * bytecode hard-codes these offsets and we mirror that faithfully.
 *
 * These constants are also used by code that bridges between script
 * and runtime (click hit-test, mask-list registration, walker bind,
 * speech balloon, etc.). Defining them once in one shared header
 * avoids the same numeric soup being repeated across a dozen
 * translation units.
 *
 * Include this header anywhere `EOFF(e, ENT_OFF_X, T)` is wanted.
 */
#ifndef WACKI_ENTITY_OFFSETS_H
#define WACKI_ENTITY_OFFSETS_H

#include <stdint.h>

/* ---- generic byte-offset accessors -------------------------------- */
#define EOFF(e, o, T) (*(T *)((uint8_t *)(e) + (o)))
#define EOFF8(e, o)   (*((uint8_t *)(e) + (o)))

/* ---- Entity field offsets ----------------------------------------- *
 *
 * Layout taken from the original engine's flat 102-byte Entity buffer.
 * Trailing-zone offsets (≥ 0x5E) are not exposed here — they're for
 * runtime-only port additions like cached SDL surfaces. */

#define ENT_OFF_FLAGS1          0x08    /* primary flags byte */
#define ENT_OFF_FLAGS2          0x09    /* secondary flags byte */
#define ENT_OFF_DRAWN_X         0x0A    /* drawn anchor X (post-VM tick) */
#define ENT_OFF_DRAWN_Y         0x0C    /* drawn anchor Y */
#define ENT_OFF_WIDTH           0x0E    /* current frame width */
#define ENT_OFF_HEIGHT          0x10    /* current frame height */
#define ENT_OFF_CLICK_FOOT_Y    0x12    /* hit-test owner foot_y (≠ ENT_OFF_FOOT_Y below) */
#define ENT_OFF_PIXELS_SLOT     0x14    /* pixels ptr / mask flag (kind=3) */
#define ENT_OFF_PIXEL_SLOT_ALT  0x16    /* pixel data ptr (kind=3 mask) */
#define ENT_OFF_ANCHOR_X        0x22    /* script-writable anchor X */
#define ENT_OFF_ANCHOR_Y        0x24    /* script-writable anchor Y */
#define ENT_OFF_FOOT_Y          0x26    /* foot_y after VM post-exec bake (z-sort key) */
#define ENT_OFF_FADE_Z          ENT_OFF_FOOT_Y   /* alias — same byte; fade state when EFLAG_FADE_OR_BG set */
#define ENT_OFF_ATLAS_SLOT      0x28    /* AnimAsset * (interned) */
#define ENT_OFF_BYTECODE_SLOT   0x2C    /* per-entity script bytecode (interned) */
#define ENT_OFF_FRAME           0x30    /* current frame index */
#define ENT_OFF_PC              0x32    /* script pc (halfwords) */
#define ENT_OFF_LOOP_A          0x34
#define ENT_OFF_LOOP_B          0x36
#define ENT_OFF_LOOP_C          0x38
#define ENT_OFF_STATE_FLAGS     0x3A    /* bit 0=frame_ready, 1=anim_active, 4=walker_fresh */
#define ENT_OFF_DELAY           0x3C
#define ENT_OFF_DELAY_RESET     0x3E
#define ENT_OFF_LOOP_D          0x40
#define ENT_OFF_LOOP_E          0x42
#define ENT_OFF_WALKER_X        0x44    /* walker accumulator X (16.16 fixed) */
#define ENT_OFF_WALKER_X_HI     0x46    /* upper half of WALKER_X */
#define ENT_OFF_WALKER_Y        0x48
#define ENT_OFF_WALKER_Y_HI     0x4A
#define ENT_OFF_WALKER_DX_REM   0x4C    /* non-zero = walker busy */
#define ENT_OFF_WALKER_DY_REM   0x50
#define ENT_OFF_WALKER_TGT_X    0x54
#define ENT_OFF_WALKER_TGT_Y    0x56
#define ENT_OFF_SCALE_PCT       0x58    /* clamped to ≤ 0xA0 */
#define ENT_OFF_GROUP_FLAGS     0x5E

/* Click descriptor field offsets (separate kind of entity used by
 * hit-test). Numerically overlaps Entity layout, kept separately for
 * documentation. */
#define CLICK_OFF_KIND              0x08    /* 1 = sprite, 2 = mask */
#define CLICK_OFF_OWNER_SLOT        0x0A
#define CLICK_OFF_VERB_TABLE_SLOT   0x0E
#define CLICK_OFF_CACHED_VERB       0x12

/* ---- common entity flag bits -------------------------------------- */

/* Primary flags (byte at ENT_OFF_FLAGS1 / +0x08, accessed as u16). */
#define EFLAG_RENDERABLE        0x0001
#define EFLAG_DOUBLED           0x0004    /* sprite drawn at 2× */
#define EFLAG_ONESHOT_BG_PEND   0x0020    /* paired with FADE_OR_BG for one-shot blit */
#define EFLAG_FOOT_BAKED        EFLAG_ONESHOT_BG_PEND    /* alias — set when fade-out clears +0x26 */
#define EFLAG_FADE_OR_BG        0x0040    /* fade-out or one-shot BG paint */
#define EFLAG_HIDDEN            0x0080
#define EFLAG_ALPHA_PLANE       0x0100
#define EFLAG_NO_FOOT_BAKE      0x0200
#define EFLAG_PERSPECTIVE       0x0400
#define EFLAG_SKY               0x2000
#define EFLAG_PENDING_FREE      0x8000

/* Subset accessed via byte ENT_OFF_FLAGS1 (8-bit). */
#define EFLAGS1_HIDDEN          0x80

/* State flags (byte at ENT_OFF_STATE_FLAGS / +0x3A). */
#define ESTATE_FRAME_READY      0x01
#define ESTATE_ANIM_ACTIVE      0x02
#define ESTATE_FOOT_ANCHORED    ESTATE_ANIM_ACTIVE   /* alias used by render path */
#define ESTATE_WALKER_FRESH     0x04

/* Scale clamp: drawn scale_pct at +0x58 may not exceed 160 %. */
#define ENT_SCALE_MAX           0xA0

#endif /* WACKI_ENTITY_OFFSETS_H */
