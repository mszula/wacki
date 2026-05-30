/* src/scene/stage.c — stage descriptor table + per-stage actor anims.
 *
 * Wacki ships five stages, each with a PE-binary descriptor that lists
 * the actor atlas filenames, HUD panel, palette, starting komnata,
 * intro AVI, and a couple of alternate cutscenes. BuildStageTable
 * reads the descriptor pointer table at PE VA 0x00442FA8 once at game
 * start and populates g_stage_table[] / g_stage_va_table[].
 *
 * LoadActorWalkAnims is called per-stage transition: it reads the
 * directional walk-anim pointer tables from the stage descriptor
 * (L/R/U/D + aux + idle) and stores them in g_actor_walk_anim[] for
 * the walker to pick from.
 */

#include "wacki.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern const void *PeLoaderRead(uint32_t va);

/* ---- directional walk-anim table per actor.
 *
 * DAT_0044A19C+0xC (actor 0) and +0x10 (actor 1) —
 * each a pointer to an array of 6 dwords:
 * [0] L walk (op 0x15-driven, patched X/Y)
 * [1] R walk
 * [2] U walk
 * [3] D walk
 * [4] aux script (purpose TBD — possibly stand-with-direction)
 * [5] idle script (no op 0x15, just SET_DELAY → STOP)
 *
 * binds entry [5] (idle) to entity[+0x2C] at room reset:
 * DAT_0044E724[0xB] = *(int *)(DAT_0044A19C+0xC + 0x14);
 * The 0x14 offset = 4*5 bytes = entry 5 = idle. So actors start with
 * idle bound and per-entity VM ticks the idle script (which yields each
 * frame). On click, PlayActorAnimByPath swaps to a directional walker. */
const char *g_actor_walk_anim[2][6] = {
    { NULL, NULL, NULL, NULL, NULL, NULL },
    { NULL, NULL, NULL, NULL, NULL, NULL },
};

/* LoadActorWalkAnims — populate g_actor_walk_anim from PE stage descriptor.
 * Called by LoadStage after g_stage_va is set.
 *
 * Stage descriptor layout (verified for stage 1 @ PE VA 0x00428220):
 * +0x00 = komnata table ptr
 * +0x04 = verb dispatch table
 * +0x08 = object dispatch table
 * +0x0C = actor 0 directional anim table ptr (→ 6 dwords L/R/U/D/aux/idle)
 * +0x10 = actor 1 directional anim table ptr
 * +0x14 = ebek_wyc filename string
 * +0x18 = fjej_wyc filename string
 * +0x1C = panel_wyc filename string
 * ... */
void LoadActorWalkAnims(uint32_t stage_va)
{
    extern const void *PeLoaderRead(uint32_t va);
    if (!stage_va) {
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 6; ++j)
                g_actor_walk_anim[i][j] = NULL;
        return;
    }
    const uint8_t *sd = (const uint8_t *)PeLoaderRead(stage_va);
    if (!sd) return;

    for (int actor = 0; actor < 2; ++actor) {
        /* Read anim table pointer from stage descriptor. */
        uint32_t tab_va = (uint32_t)(
            sd[0xC + actor*4] |
            (sd[0xD + actor*4] << 8) |
            (sd[0xE + actor*4] << 16) |
            (sd[0xF + actor*4] << 24));
        if (!tab_va) {
            for (int j = 0; j < 6; ++j)
                g_actor_walk_anim[actor][j] = NULL;
            continue;
        }
        const uint8_t *tab = (const uint8_t *)PeLoaderRead(tab_va);
        if (!tab) continue;
        for (int slot = 0; slot < 6; ++slot) {
            uint32_t bc_va = (uint32_t)(
                tab[slot*4 + 0] |
                (tab[slot*4 + 1] << 8) |
                (tab[slot*4 + 2] << 16) |
                (tab[slot*4 + 3] << 24));
            g_actor_walk_anim[actor][slot] =
                bc_va ? (const char *)PeLoaderRead(bc_va) : NULL;
        }
    }
    fprintf(stderr,
        "[stage] actor walk anims: a0 LRUD=%p,%p,%p,%p aux=%p idle=%p\n",
        (void*)g_actor_walk_anim[0][0], (void*)g_actor_walk_anim[0][1],
        (void*)g_actor_walk_anim[0][2], (void*)g_actor_walk_anim[0][3],
        (void*)g_actor_walk_anim[0][4], (void*)g_actor_walk_anim[0][5]);
    fprintf(stderr,
        "[stage] actor walk anims: a1 LRUD=%p,%p,%p,%p aux=%p idle=%p\n",
        (void*)g_actor_walk_anim[1][0], (void*)g_actor_walk_anim[1][1],
        (void*)g_actor_walk_anim[1][2], (void*)g_actor_walk_anim[1][3],
        (void*)g_actor_walk_anim[1][4], (void*)g_actor_walk_anim[1][5]);
}

/* ---- default world-state template — DAT_004434F0 (0x999 dwords).
 * The real one is a precomputed game-state baseline; for the portable
 * build it stays zeroed (an empty save). */
const uint8_t g_default_world_state[0x2664] = { 0 };

/* ---- stage descriptor table — PTR_PTR_00442FA8.
 *
 * T26: BuildStageTable reads PTR_PTR_00442FA8 from PE memory (5 dwords
 * → 5 stage descriptor VAs), parses each into a host-native StageDef
 * (pointers resolved through PeLoaderRead), and populates g_stage_table[].
 * g_stage_va_table[] stores the raw PE VAs alongside for LoadStage and
 * LoadActorWalkAnims (which read additional fields directly via PE).
 *
 * After BuildStageTable: g_stage_table[0..4] is non-NULL iff the
 * corresponding stage descriptor existed in the binary. Stage 1 has
 * been the only one tested in interactive play; stages 2-5 are wired
 * but require runtime validation (see TASKS-2/T28). */
StageDef *g_stage_table[5];
uint32_t  g_stage_va_table[5];     /* raw PE VAs alongside g_stage_table */

static StageDef s_stage_storage[5];

void BuildStageTable(void)
{
    extern const void *PeLoaderRead(uint32_t va);
    /* PTR_PTR_00442FA8 — 5 dwords of stage descriptor VAs. The 6th
 * slot in PE is a NULL sentinel (see read_memory dump). */
    const uint8_t *tab = (const uint8_t *)PeLoaderRead(0x00442FA8);
    if (!tab) {
        fprintf(stderr, "[stage] PTR_PTR_00442FA8 unreadable — stage table empty\n");
        for (int i = 0; i < 5; ++i) { g_stage_table[i] = NULL; g_stage_va_table[i] = 0; }
        return;
    }
    for (int i = 0; i < 5; ++i) {
        uint32_t sva = (uint32_t)(tab[i*4 + 0] | (tab[i*4 + 1] << 8) |
                                  (tab[i*4 + 2] << 16) | (tab[i*4 + 3] << 24));
        g_stage_va_table[i] = sva;
        if (!sva) { g_stage_table[i] = NULL; continue; }

        const uint8_t *sd = (const uint8_t *)PeLoaderRead(sva);
        if (!sd) { g_stage_table[i] = NULL; continue; }

        StageDef *out = &s_stage_storage[i];
        memset(out, 0, sizeof *out);
        /* Skip unknown[5] @ +0..+0x13 — komnata table, dispatch tables,
 * actor anim tables; consumed via PE directly elsewhere
 * (LoadKomnata, DispatchClickEvent, LoadActorWalkAnims). */
        uint32_t ebek_va   = (uint32_t)(sd[0x14] | (sd[0x15] << 8) | (sd[0x16] << 16) | (sd[0x17] << 24));
        uint32_t fjej_va   = (uint32_t)(sd[0x18] | (sd[0x19] << 8) | (sd[0x1A] << 16) | (sd[0x1B] << 24));
        uint32_t panel_va  = (uint32_t)(sd[0x1C] | (sd[0x1D] << 8) | (sd[0x1E] << 16) | (sd[0x1F] << 24));
        uint32_t pal_va    = (uint32_t)(sd[0x20] | (sd[0x21] << 8) | (sd[0x22] << 16) | (sd[0x23] << 24));
        uint16_t start_kn  = (uint16_t)(sd[0x24] | (sd[0x25] << 8));
        uint32_t intro_va  = (uint32_t)(sd[0x26] | (sd[0x27] << 8) | (sd[0x28] << 16) | (sd[0x29] << 24));
        uint32_t alt_va    = (uint32_t)(sd[0x2A] | (sd[0x2B] << 8) | (sd[0x2C] << 16) | (sd[0x2D] << 24));
        uint32_t alt3_va   = (uint32_t)(sd[0x2E] | (sd[0x2F] << 8) | (sd[0x30] << 16) | (sd[0x31] << 24));

        out->ebek_wyc      = ebek_va  ? (char *)PeLoaderRead(ebek_va)  : NULL;
        out->fjej_wyc      = fjej_va  ? (char *)PeLoaderRead(fjej_va)  : NULL;
        out->panel_wyc     = panel_va ? (char *)PeLoaderRead(panel_va) : NULL;
        out->paleta_pal    = pal_va   ? (char *)PeLoaderRead(pal_va)   : NULL;
        out->start_komnata = start_kn;
        out->intro_avi     = intro_va ? (char *)PeLoaderRead(intro_va) : NULL;
        out->alt_avi       = alt_va   ? (char *)PeLoaderRead(alt_va)   : NULL;
        out->alt3_avi      = alt3_va  ? (char *)PeLoaderRead(alt3_va)  : NULL;
        g_stage_table[i]   = out;
        fprintf(stderr,
            "[stage] %d @ 0x%08X: ebek=%s fjej=%s panel=%s pal=%s start=%u intro=%s alt=%s\n",
            i + 1, sva,
            out->ebek_wyc   ? out->ebek_wyc   : "(null)",
            out->fjej_wyc   ? out->fjej_wyc   : "(null)",
            out->panel_wyc  ? out->panel_wyc  : "(null)",
            out->paleta_pal ? out->paleta_pal : "(null)",
            start_kn,
            out->intro_avi  ? out->intro_avi  : "(null)",
            out->alt_avi    ? out->alt_avi    : "(null)");
    }
}
