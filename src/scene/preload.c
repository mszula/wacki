/* src/scene/preload.c — boot-time asset preloading.
 *
 * PreloadCommonAssets runs once at game start to load the assets that
 * persist across stages: the actor atlases, the inventory item icons,
 * the panel cursor, and the default panel layout. These stay in
 * memory and are referenced by g_actor[], g_items_atlas, etc.; scene
 * transitions don't reload them.
 */

#include "wacki.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern AnimAsset *g_ebek_atlas;
extern AnimAsset *g_fjej_atlas;
extern AnimAsset *g_ebfj_atlas;
extern AnimAsset *g_cursor_atlas[8];

extern void BuildStageTable(void);

int PreloadCommonAssets(void)
{
    /* T26: populate g_stage_table[] + g_stage_va_table[] from
 * PTR_PTR_00442FA8 before anything else might consult them
 * (LoadStage from RunGameStageLoop / save.c). */
    BuildStageTable();

    static const struct { const char *name; AnimAsset **slot; } resident[] = {
        { "ebfj.wyc",     &g_ebfj_atlas },  /* DAT_00453748 — actor portraits */
        { "ebek.wyc",     &g_ebek_atlas },  /* T4: singleton, persists scenes */
        { "fjej.wyc",     &g_fjej_atlas },  /* T4: singleton, persists scenes */
        { "przedm.wyc",   &g_items_atlas },  /* DAT_0044E6AC — inventory items */
        /* T31 — cursor state atlases (DAT_00451488..0x004514A4 in PE).
 * Each state maps to a sprite atlas indexed by g_cursor_state. */
        { "olowek1.wyc",  &g_cursor_atlas[0] },  /* state 0 + 6: default arrow */
        { "kaseta.wyc",   &g_cursor_atlas[1] },  /* state 1: idle anim */
        { "magnes1a.wyc", &g_cursor_atlas[2] },  /* state 2: held-item hover */
        { "magnes1.wyc",  &g_cursor_atlas[3] },  /* state 3: ??? */
        { "drzwi1l.wyc",  &g_cursor_atlas[4] },  /* state 4: exit-left/walk */
        { "drzwi1p.wyc",  &g_cursor_atlas[5] },  /* state 5: exit-right */
    };
    for (size_t i = 0; i < sizeof resident / sizeof resident[0]; ++i) {
        AnimAsset *a = LoadAssetFromDtaBase(resident[i].name);
        if (!a) return 0;
        if (resident[i].slot) *resident[i].slot = a;
    }
    /* 1:1 with PreloadCommonAssets @ 0x00403850 trailing block:
 * LoadFileFromDta("Futura.30", &DAT_0044E440);
 * DAT_0044E598 = ParseFutFontFile;
 * The bitmap font is used by RenderTextLineToBuffer (op 0x09 / dialog). */
    void *fbuf = NULL; uint32_t fsz = 0;
    if (LoadFileFromDta("Futura.30", &fbuf, &fsz) && fbuf) {
        g_default_font = ParseFutFontFile((const uint8_t *)fbuf);
        if (!g_default_font) {
            fprintf(stderr, "[init] Futura.30 parse failed\n");
            xfree(fbuf);
        } else {
            fprintf(stderr, "[init] Futura.30 loaded (%u bytes)\n", fsz);
            /* fbuf is referenced by the parsed FontHandle — keep alive */
        }
    } else {
        fprintf(stderr, "[init] Futura.30 not found in archive\n");
    }

    /* 1:1 with PreloadCommonAssets @ 0x004038F4..0x00403924 — initialise
 * the per-item entity_state table. The original zeroes
 * the 0x470-byte block then walks 8-byte strides writing
 * entity_state[idx].panel_verb_id = idx + 0x29
 * for idx in 1..0x8D (entry 0 left zero — verb 0x29 is reserved /
 * never used as a real inventory item). Without this seed,
 * (InventoryAddItem) reads panel_verb_id = 0 and writes
 * 0 into g_panel_verb_tab[], and PaintHudOverlay then skips paint
 * because (0 - 0x29) wraps to >= frame_count. Net effect before the
 * fix: items appeared "added" to inventory but never rendered. */
    memset(g_entity_state, 0, sizeof g_entity_state);
    {
        uint16_t *es = (uint16_t *)g_entity_state;
        for (int idx = 1; idx < 0x8E; ++idx) {
            es[idx * 4 + 0] = (uint16_t)(idx + 0x29);
        }
    }
    return 1;
}
