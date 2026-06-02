/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/scene/preload.c — boot-time asset preloading.
 *
 * PreloadCommonAssets runs once at game start, just after
 * InitializeGameSubsystems mounts the data archive. It loads the
 * assets that persist across stages — actor atlases, inventory item
 * icons, cursor-state atlases, the bitmap font — and seeds the
 * per-item entity_state[] table so the inventory panel can render
 * pickups from the first frame.
 *
 * None of the loaded assets are freed at scene transition; they live
 * for the whole game and are referenced through the named globals
 * (g_ebek_atlas, g_items_atlas, g_cursor_atlas[...], g_default_font).
 *
 * BuildStageTable runs FIRST so g_stage_table[] / g_stage_va_table[]
 * are populated before any later code (LoadStage, save.c) consults
 * them. */

#include "wacki.h"
#include "wacki/log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern AnimAsset *g_ebek_atlas;
extern AnimAsset *g_fjej_atlas;
extern AnimAsset *g_ebfj_atlas;
extern AnimAsset *g_cursor_atlas[8];

extern void BuildStageTable(void);

/* ---- constants ---------------------------------------------------- */

/* The bitmap font loaded once at boot for op 0x09 SHOW_TEXT + the
 * dialog runner's per-line text. */
#define FONT_FILENAME                       "Futura.30"

/* Per-item entity_state[] layout. Each item has an 8-byte slot (= 4
 * u16 fields) indexed by (item_id - ITEM_VERB_FIRST). The boot pass
 * seeds entry idx ∈ [1..ITEM_ENTITY_STATE_COUNT) with its panel_verb_
 * id (= idx + ITEM_VERB_FIRST). Entry 0 stays zero because verb 0x29
 * is reserved and never appears as a real inventory pickup. */
#define ITEM_VERB_FIRST                     0x29
#define ITEM_ENTITY_STATE_COUNT             0x8E   /* 142 items */
#define ITEM_ENTITY_STATE_STRIDE_U16        4      /* 8 B per entry */
#define ITEM_ENTITY_STATE_OFF_VERB          0

/* ---- helpers ----------------------------------------------------- */

/* Load the bitmap font. The parsed FontHandle keeps a pointer into
 * the loaded buffer, so we deliberately leak `fbuf` on success — the
 * font lives for the whole game. */
static void preload_default_font(void)
{
    void    *fbuf = NULL;
    uint32_t fsz  = 0;
    if (!LoadFileFromDta(FONT_FILENAME, &fbuf, &fsz) || !fbuf) {
        LOG_INFO("init", "%s not found in archive", FONT_FILENAME);
        return;
    }
    g_default_font = ParseFutFontFile((const uint8_t *)fbuf);
    if (!g_default_font) {
        LOG_INFO("init", "%s parse failed", FONT_FILENAME);
        xfree(fbuf);
        return;
    }
    LOG_INFO("init", "%s loaded (%u bytes)", FONT_FILENAME, fsz);
    /* fbuf intentionally leaked — FontHandle references it. */
}

/* Seed entity_state[idx].panel_verb_id = idx + ITEM_VERB_FIRST for
 * every item slot. Without this, InventoryAddItem reads panel_verb_id
 * = 0 and writes 0 into g_panel_verb_tab[]; PaintHudOverlay then
 * skips paint because (0 - ITEM_VERB_FIRST) wraps to >= frame_count.
 * Net effect before the seed: items appeared "added" to inventory
 * but never rendered. */
static void seed_item_entity_state(void)
{
    memset(g_entity_state, 0, sizeof g_entity_state);
    uint16_t *es = (uint16_t *)g_entity_state;
    for (int idx = 1; idx < ITEM_ENTITY_STATE_COUNT; ++idx) {
        es[idx * ITEM_ENTITY_STATE_STRIDE_U16 + ITEM_ENTITY_STATE_OFF_VERB] =
            (uint16_t)(idx + ITEM_VERB_FIRST);
    }
}

/* ---- public API ------------------------------------------------- */

int PreloadCommonAssets(void)
{
    /* Stage table FIRST so LoadStage / save.c can consult it. */
    BuildStageTable();

    /* Persistent atlases — actor portraits, the two actor sprites
     * (singletons across scene transitions per T4), the inventory
     * item-icon atlas, and the 6 cursor-state atlases. The cursor
     * state index is set by UpdateCursorState every frame. */
    static const struct {
        const char  *name;
        AnimAsset  **slot;
    } resident[] = {
        { "ebfj.wyc",     &g_ebfj_atlas       },  /* actor portraits */
        { "ebek.wyc",     &g_ebek_atlas       },  /* Ebek sprite */
        { "fjej.wyc",     &g_fjej_atlas       },  /* Fjej sprite */
        { "przedm.wyc",   &g_items_atlas      },  /* inventory item icons */
        { "olowek1.wyc",  &g_cursor_atlas[0]  },  /* state 0 / 6: default arrow */
        { "kaseta.wyc",   &g_cursor_atlas[1]  },  /* state 1: idle anim */
        { "magnes1a.wyc", &g_cursor_atlas[2]  },  /* state 2: held-item hover */
        { "magnes1.wyc",  &g_cursor_atlas[3]  },  /* state 3 */
        { "drzwi1l.wyc",  &g_cursor_atlas[4]  },  /* state 4: exit-left */
        { "drzwi1p.wyc",  &g_cursor_atlas[5]  },  /* state 5: exit-right */
    };
    for (size_t i = 0; i < sizeof resident / sizeof resident[0]; ++i) {
        AnimAsset *a = LoadAssetFromDtaBase(resident[i].name);
        if (!a) return 0;
        if (resident[i].slot) *resident[i].slot = a;
    }

    preload_default_font();
    seed_item_entity_state();
    return 1;
}
