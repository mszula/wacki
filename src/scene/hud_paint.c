/* src/scene/hud_paint.c — per-frame HUD overlay paint.
 *
 * Called once per frame from ProcessGameFrameTickInner, after the
 * scene render but before the cursor and back-buffer flush.
 * Composites three things in order:
 *
 *   1. The verb panel itself (panel.wyc): blit the panel atlas at
 *      its scene-baked position.
 *   2. Inventory item icons: for each non-empty slot on the current
 *      page, blit the corresponding icon from g_items_atlas.
 *   3. The held-item drag-ghost: if the player is currently dragging
 *      a verb (g_held_item != 0x26), interpolate its position toward
 *      the cursor and draw a dimmed copy of the icon there.
 */

#include "wacki.h"
#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern AnimAsset *g_ebfj_atlas;

/* Forward decls for helpers still owned by game.c. */
extern void paint_anim_button_at(AnimAsset *atlas, uint16_t frame,
                                 int16_t base_x, int16_t base_y, int paint);

/* PaintHudOverlay — panel + inventory icons + held-item ghost paint.
 * Runs AFTER EntityRenderAll inside ProcessGameFrameTickInner so HUD
 * sits on top of scene entities. Reads globals: g_panel_asset (panel
 * atlas), g_items_atlas (inventory icon atlas), g_panel_verb_tab[6]
 * (verb in each panel slot), g_held_item, g_settings_anim_active. */
void PaintHudOverlay(void)
{
    extern AnimAsset *g_panel_asset;
    extern AnimAsset *g_items_atlas;
    extern uint16_t   g_panel_verb_tab[6];
    extern int16_t    s_mouse_x, s_mouse_y;
    extern uint32_t   g_frame_delta_ms;

    AnimAsset *panel = g_panel_asset;
    if (panel) paint_anim_button_at(panel, 0, 0, 400, 1);

    /* Inventory / dialog-choice icon overlay — 1:1 with FUN_00406EB0 @
     * 0x00407045: pixels = przedm.pixel_ptrs[verb - 0x29]; blit at
     * (panel_x + btn_x[i], panel_y + btn_y[i]), 0x28×0x28 cell. */
    if (g_items_atlas && (g_settings_anim_active & 1) && panel
        && panel->off_drawX && panel->off_drawY)
    {
        int16_t panel_x = (int16_t)panel->off_drawX[0];
        int16_t panel_y = (int16_t)panel->off_drawY[0];
        static const int16_t btn_x[6] = { 300, 345, 390, 435, 480, 525 };
        static const int16_t btn_y[6] = {  20,  20,  20,  20,  20,  20 };
        for (int i = 0; i < 6; ++i) {
            uint16_t verb = g_panel_verb_tab[i];
            if (verb == 0x26) continue;
            if ((uint16_t)(verb - 0x29) >= g_items_atlas->frame_count) continue;
            uint16_t idx = (uint16_t)(verb - 0x29);
            uint8_t *px = g_items_atlas->pixel_ptrs
                          ? g_items_atlas->pixel_ptrs[idx] : NULL;
            if (!px) continue;
            uint16_t fw = g_items_atlas->off_widths
                          ? g_items_atlas->off_widths[idx] : 0;
            uint16_t fh = g_items_atlas->off_heights
                          ? g_items_atlas->off_heights[idx] : 0;
            if (!fw || !fh) continue;
            int16_t dx = (int16_t)(panel_x + btn_x[i]);
            int16_t dy = (int16_t)(panel_y + btn_y[i]);
            BlitSpriteToBackbuffer((uint16_t)dx, (uint16_t)dy, 0, 0,
                                   fw, fh, fw, fh, px, 0);
        }
    }

    /* Actor portrait + active-frame indicator — 1:1 port of FUN_00407130 @
     * 0x00407130. The ebfj.wyc atlas has 4 frames stored at fixed positions
     * (each frame's own off_drawX/Y): 0/1 = Ebek/Fjej "active" portraits
     * (with frame/border), 2/3 = "inactive" portraits (without frame).
     *
     * Original gates on a cached active-actor != current — it only repaints
     * when SPACE toggles, because the panel background is sticky in the
     * original's dirty-rect framework. Our PaintHudOverlay repaints the
     * whole panel every frame, so we always paint both portraits.
     *
     * Order matches original: inactive first (uVar2 ^ 3), active on top
     * (uVar2). With active=0 (Ebek) → paint frame 3 (Fjej inactive), then
     * frame 0 (Ebek with frame). With active=1 → frame 2, then frame 1. */
    if (g_ebfj_atlas && g_ebfj_atlas->pixel_ptrs && g_ebfj_atlas->frame_count >= 4
        && (g_settings_anim_active & 1))
    {
        extern uint16_t g_active_actor;
        unsigned a   = g_active_actor & 1u;
        unsigned f_i = (a ^ 1u) | 2u;       /* inactive: 2 or 3 */
        unsigned f_a = a;                   /* active:   0 or 1 */
        for (int pass = 0; pass < 2; ++pass) {
            unsigned f = (pass == 0) ? f_i : f_a;
            uint8_t *px = g_ebfj_atlas->pixel_ptrs[f];
            if (!px) continue;
            uint16_t fw = g_ebfj_atlas->off_widths  ? g_ebfj_atlas->off_widths [f] : 0;
            uint16_t fh = g_ebfj_atlas->off_heights ? g_ebfj_atlas->off_heights[f] : 0;
            uint16_t dx = g_ebfj_atlas->off_drawX   ? g_ebfj_atlas->off_drawX  [f] : 0;
            uint16_t dy = g_ebfj_atlas->off_drawY   ? g_ebfj_atlas->off_drawY  [f] : 0;
            if (!fw || !fh) continue;
            BlitSpriteToBackbuffer(dx, dy, 0, 0, fw, fh, fw, fh, px, 0);
        }
    }

    /* Fjej portrait glasses-on/off overlay — state-driven paint.
     *
     * The original verb-script @ 0x00423EE0 / 0x00432C40 SPAWNS an entity
     * (id=87, asset=fjbezoku.wyc or fjzoku.wyc) at (172,427) when the user
     * takes/returns the glasses. Our entity-render path doesn't keep that
     * entity alive long enough — something (likely a per-frame Item.scr
     * trigger for verb 0x67 or stage 3's verb-script chain) destroys the
     * spawned entity in the same tick. Render-list scan saw the entity
     * for only 1 frame.
     *
     * Workaround: read the script variable the verb-script writes
     * (var[0x67] = 1 when glasses taken, 0 when on portrait) and paint
     * the matching atlas directly. Skips the SPAWN/DESTROY race entirely.
     * Cached load on first use; both atlases stay resident (small —
     * ~2KB each). PORT SHORTCUT (refer FUN_004094a0 + script 0x00423EE0):
     * we drive the visual from the script var rather than the spawned
     * entity, since the entity lifetime is currently broken. */
    {
        extern uint32_t g_script_vars[0x129];
        static AnimAsset *s_fjbezoku = NULL;       /* without glasses */
        static int s_load_attempted = 0;
        if (!s_load_attempted) {
            s_load_attempted = 1;
            s_fjbezoku = LoadAssetFromDtaBase("fjbezoku.wyc");
        }
        if (s_fjbezoku && s_fjbezoku->pixel_ptrs
            && s_fjbezoku->off_widths && s_fjbezoku->off_heights
            && s_fjbezoku->frame_count > 0
            && g_script_vars[0x67] == 1)
        {
            uint8_t *px = s_fjbezoku->pixel_ptrs[0];
            uint16_t fw = s_fjbezoku->off_widths [0];
            uint16_t fh = s_fjbezoku->off_heights[0];
            uint16_t dx = s_fjbezoku->off_drawX  ? s_fjbezoku->off_drawX [0] : 172;
            uint16_t dy = s_fjbezoku->off_drawY  ? s_fjbezoku->off_drawY [0] : 427;
            if (px && fw && fh)
                BlitSpriteToBackbuffer(dx, dy, 0, 0, fw, fh, fw, fh, px, 0);
        }
    }

    /* Health bar overlay — pasek#N.wyc entity, drawn near the TOP of the
     * HUD panel at atlas-native (drawX, drawY) = (7, 403). The stage
     * enter-script spawns this entity (id=101, asset="pasek#1.wyc" for
     * stage 1); EntityRenderAll draws it at the same position BEFORE
     * the panel paint, so panel.wyc frame 0 covers it. We re-paint it
     * here AFTER panel so it stays visible.
     *
     * Frame index = health level 0..23 (0=full green, ~12=yellow, ~18=red,
     * 23=empty). The entity's per-tick script (0x00423528) advances frame
     * from a script var we haven't identified yet; until wired, bar shows
     * whatever frame the entity script last set (typically 0 = full).
     *
     * Gated on `g_settings_anim_active & 1` (= HUD panel visible). Stage 5
     * (Monter finale) sets this bit to 0 in play_demo_scene, so the pasek
     * leftover from the previous stage (if any survived EntityListClearAll
     * via the entry-chain script re-spawning) doesn't show during the
     * ACME assembly cutscene. Same gate as the portrait + inventory paints
     * above — pasek is a panel-scope overlay. */
    if (g_settings_anim_active & 1)
    {
        extern Entity *EntityListAt(int, int);
        extern int     EntityListCount(int);
        extern void   *ent_ptr_resolve(uint32_t);
        int n = EntityListCount(0);
        for (int i = 0; i < n; ++i) {
            Entity *e = EntityListAt(0, i);
            if (!e) continue;
            uint8_t *eb = (uint8_t *)e;
            AnimAsset *atlas = (AnimAsset *)ent_ptr_resolve(*(uint32_t *)(eb + 0x28));
            if (!atlas || !atlas->name[0]) continue;
            if (strncmp(atlas->name, "pasek", 5) != 0) continue;
            if (!atlas->pixel_ptrs || !atlas->off_widths || !atlas->off_heights)
                break;
            uint16_t f = *(uint16_t *)(eb + 0x30);
            if (f >= atlas->frame_count) f = 0;
            uint8_t *px = atlas->pixel_ptrs[f];
            if (!px) break;
            uint16_t fw = atlas->off_widths[f];
            uint16_t fh = atlas->off_heights[f];
            if (!fw || !fh) break;
            uint16_t dx = atlas->off_drawX ? atlas->off_drawX[f] : 7;
            uint16_t dy = atlas->off_drawY ? atlas->off_drawY[f] : 403;
            BlitSpriteToBackbuffer(dx, dy, 0, 0, fw, fh, fw, fh, px, 0);
            break;
        }
    }

    /* Held-item ghost — 1:1 port of FUN_004067C0 ghost branch. */
    static int16_t  s_ghost_x = 0, s_ghost_y = 0;
    static uint16_t s_ghost_item = 0xFFFF;
    if (g_held_item != 0x26 && g_items_atlas &&
        (uint16_t)(g_held_item - 0x29) < g_items_atlas->frame_count)
    {
        uint16_t idx = (uint16_t)(g_held_item - 0x29);
        uint8_t *px = g_items_atlas->pixel_ptrs
                      ? g_items_atlas->pixel_ptrs[idx] : NULL;
        if (px && g_items_atlas->off_widths && g_items_atlas->off_heights) {
            uint16_t fw = g_items_atlas->off_widths [idx];
            uint16_t fh = g_items_atlas->off_heights[idx];
            int16_t tx = (int16_t)(s_mouse_x - 0x22);
            int16_t ty = (int16_t)(s_mouse_y - 7);
            if (g_held_item != s_ghost_item) {
                s_ghost_x = tx; s_ghost_y = ty;
                s_ghost_item = g_held_item;
            } else if ((s_ghost_x != tx || s_ghost_y != ty) &&
                       g_frame_delta_ms != 0) {
                int steps = (int)g_frame_delta_ms;
                if (steps > 64) steps = 64;
                for (int i = 0; i < steps && (s_ghost_x != tx || s_ghost_y != ty); ++i) {
                    int dx = (int)tx - (int)s_ghost_x;
                    int dy = (int)ty - (int)s_ghost_y;
                    int adx = dx < 0 ? -dx : dx;
                    int ady = dy < 0 ? -dy : dy;
                    int stepx = 0, stepy = 0;
                    if (adx >= ady) {
                        stepx = (dx > 0) ? 1 : -1;
                        if (ady > 0) {
                            int ratio_num = ady;
                            if (((i * ratio_num) % (adx ? adx : 1)) <
                                (((i + 1) * ratio_num) % (adx ? adx : 1)))
                                stepy = (dy > 0) ? 1 : (dy < 0 ? -1 : 0);
                        }
                    } else {
                        stepy = (dy > 0) ? 1 : -1;
                        if (adx > 0) {
                            int ratio_num = adx;
                            if (((i * ratio_num) % (ady ? ady : 1)) <
                                (((i + 1) * ratio_num) % (ady ? ady : 1)))
                                stepx = (dx > 0) ? 1 : (dx < 0 ? -1 : 0);
                        }
                    }
                    s_ghost_x = (int16_t)(s_ghost_x + stepx);
                    s_ghost_y = (int16_t)(s_ghost_y + stepy);
                }
            }
            BlitSpriteToBackbuffer((uint16_t)s_ghost_x, (uint16_t)s_ghost_y,
                                   0, 0, fw, fh, fw, fh, px, 0);
        }
    } else {
        s_ghost_item = 0xFFFF;
    }
}
