/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/scene/stage.c — stage descriptor table + per-stage actor anims.
 *
 * Wacki ships five stages, each with a PE-binary descriptor that lists
 * the actor atlas filenames, HUD panel, palette, starting komnata,
 * intro AVI, and a couple of alternate cutscenes. BuildStageTable
 * reads the descriptor pointer table once at game start and populates
 * g_stage_table[] / g_stage_va_table[].
 *
 * LoadActorWalkAnims is called per-stage transition: it reads the
 * directional walk-anim pointer tables from the stage descriptor
 * (L/R/U/D + aux + idle) and stores them in g_actor_walk_anim[] for
 * the walker to pick from.
 */

#include "wacki.h"
#include "wacki/log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern const void *PeLoaderRead(uint32_t va);

/* ---- constants ---------------------------------------------------- */

#define STAGE_COUNT                 5    /* Wacki ships five stages */
#define ACTOR_COUNT                 2    /* Ebek + Fjej */
#define WALK_ANIM_SLOTS_PER_ACTOR   6    /* L, R, U, D, aux, idle */
#define BYTES_PER_VA                4    /* PE VAs are 32-bit */

/* PE virtual address of the stage descriptor pointer table (5 dwords
 * pointing at per-stage descriptors, terminated by a NULL sentinel). */
#define STAGE_PTR_TABLE_VA          0x00442FA8

/* Stage descriptor field offsets (verified for stage 1 @ PE VA 0x00428220).
 *
 *   +0x00..+0x13  komnata table ptr + dispatch tables + actor anim
 *                 tables (consumed via PE directly by LoadKomnata /
 *                 DispatchClickEvent / LoadActorWalkAnims)
 *   +0x14..       per-stage assets (filenames) and cutscene VAs
 */
#define STAGE_OFF_ACTOR_ANIM_TABLES 0x0C   /* actor 0/1 anim-table ptrs (8 bytes) */
#define STAGE_OFF_EBEK_WYC          0x14
#define STAGE_OFF_FJEJ_WYC          0x18
#define STAGE_OFF_PANEL_WYC         0x1C
#define STAGE_OFF_PALETA_PAL        0x20
#define STAGE_OFF_START_KOMNATA     0x24
#define STAGE_OFF_INTRO_AVI         0x26
#define STAGE_OFF_ALT_AVI           0x2A
#define STAGE_OFF_ALT3_AVI          0x2E

/* ---- module state ------------------------------------------------- */

const char *g_actor_walk_anim[ACTOR_COUNT][WALK_ANIM_SLOTS_PER_ACTOR] = {
    { NULL, NULL, NULL, NULL, NULL, NULL },
    { NULL, NULL, NULL, NULL, NULL, NULL },
};

StageDef *g_stage_table[STAGE_COUNT];
uint32_t  g_stage_va_table[STAGE_COUNT];

static StageDef s_stage_storage[STAGE_COUNT];

/* ---- helpers ------------------------------------------------------ */

/* Read a u32 little-endian from a byte buffer. */
static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

/* Read a u16 little-endian. */
static uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Resolve a PE-VA stored at `sd + offset` to its loaded char* (or NULL
 * if the VA is zero / unmappable). */
static char *resolve_pe_string(const uint8_t *sd, int offset)
{
    uint32_t va = read_u32_le(sd + offset);
    return va ? (char *)PeLoaderRead(va) : NULL;
}

/* Reset a single actor's anim slots to NULL. */
static void clear_actor_walk_anims(int actor)
{
    for (int j = 0; j < WALK_ANIM_SLOTS_PER_ACTOR; ++j) {
        g_actor_walk_anim[actor][j] = NULL;
    }
}

/* Read one actor's directional anim pointer table from the stage
 * descriptor and resolve each entry through the PE loader. */
static void load_actor_anim_table(const uint8_t *sd, int actor)
{
    int      off    = STAGE_OFF_ACTOR_ANIM_TABLES + actor * BYTES_PER_VA;
    uint32_t tab_va = read_u32_le(sd + off);
    if (!tab_va) {
        clear_actor_walk_anims(actor);
        return;
    }
    const uint8_t *tab = (const uint8_t *)PeLoaderRead(tab_va);
    if (!tab) return;

    for (int slot = 0; slot < WALK_ANIM_SLOTS_PER_ACTOR; ++slot) {
        uint32_t bc_va = read_u32_le(tab + slot * BYTES_PER_VA);
        g_actor_walk_anim[actor][slot] = bc_va ? (const char *)PeLoaderRead(bc_va)
                                               : NULL;
    }
}

/* Log the populated walk-anim slot pointers for both actors. */
static void log_actor_walk_anims(void)
{
    for (int i = 0; i < ACTOR_COUNT; ++i) {
        LOG_INFO("stage", "actor walk anims: a%d LRUD=%p,%p,%p,%p aux=%p idle=%p", i, (void *)g_actor_walk_anim[i][0], (void *)g_actor_walk_anim[i][1], (void *)g_actor_walk_anim[i][2], (void *)g_actor_walk_anim[i][3], (void *)g_actor_walk_anim[i][4], (void *)g_actor_walk_anim[i][5]);
    }
}

/* Decode one StageDef from a PE-loaded stage descriptor block. */
static void decode_stage_def(StageDef *out, const uint8_t *sd)
{
    memset(out, 0, sizeof *out);
    out->ebek_wyc      = resolve_pe_string(sd, STAGE_OFF_EBEK_WYC);
    out->fjej_wyc      = resolve_pe_string(sd, STAGE_OFF_FJEJ_WYC);
    out->panel_wyc     = resolve_pe_string(sd, STAGE_OFF_PANEL_WYC);
    out->paleta_pal    = resolve_pe_string(sd, STAGE_OFF_PALETA_PAL);
    out->start_komnata = read_u16_le(sd + STAGE_OFF_START_KOMNATA);
    out->intro_avi     = resolve_pe_string(sd, STAGE_OFF_INTRO_AVI);
    out->alt_avi       = resolve_pe_string(sd, STAGE_OFF_ALT_AVI);
    out->alt3_avi      = resolve_pe_string(sd, STAGE_OFF_ALT3_AVI);
}

/* ---- world-state baseline (saved-game template) ------------------ */

const uint8_t g_default_world_state[0x2664] = { 0 };

/* ---- public API --------------------------------------------------- */

void LoadActorWalkAnims(uint32_t stage_va)
{
    if (!stage_va) {
        for (int i = 0; i < ACTOR_COUNT; ++i) clear_actor_walk_anims(i);
        return;
    }
    const uint8_t *sd = (const uint8_t *)PeLoaderRead(stage_va);
    if (!sd) return;

    for (int actor = 0; actor < ACTOR_COUNT; ++actor) {
        load_actor_anim_table(sd, actor);
    }
    log_actor_walk_anims();
}

void BuildStageTable(void)
{
    const uint8_t *tab = (const uint8_t *)PeLoaderRead(STAGE_PTR_TABLE_VA);
    if (!tab) {
        LOG_INFO("stage", "stage pointer table unreadable — stage table empty");
        for (int i = 0; i < STAGE_COUNT; ++i) {
            g_stage_table[i]    = NULL;
            g_stage_va_table[i] = 0;
        }
        return;
    }

    for (int i = 0; i < STAGE_COUNT; ++i) {
        uint32_t sva = read_u32_le(tab + i * BYTES_PER_VA);
        g_stage_va_table[i] = sva;
        if (!sva) { g_stage_table[i] = NULL; continue; }

        const uint8_t *sd = (const uint8_t *)PeLoaderRead(sva);
        if (!sd) { g_stage_table[i] = NULL; continue; }

        StageDef *out = &s_stage_storage[i];
        decode_stage_def(out, sd);
        g_stage_table[i] = out;

        LOG_INFO("stage", "%d @ 0x%08X: ebek=%s fjej=%s panel=%s pal=%s "
                "start=%u intro=%s alt=%s", i + 1, sva, out->ebek_wyc    ? out->ebek_wyc    : "(null)", out->fjej_wyc    ? out->fjej_wyc    : "(null)", out->panel_wyc   ? out->panel_wyc   : "(null)", out->paleta_pal  ? out->paleta_pal  : "(null)", out->start_komnata, out->intro_avi   ? out->intro_avi   : "(null)", out->alt_avi     ? out->alt_avi     : "(null)");
    }
}
