/*
 * actor.c — Entities (sprites with state) and movement of the two
 * controllable actors (Ebek and Fjej).
 *
 * Original addresses:
 *   AllocEntity                0x00405A00
 *   InitEntityBitmap           0x00405920
 *   FreeEntity                 0x004058F0
 *   RegisterEntityForUpdate    0x00405E30
 *   UpdateActorMovement        0x004061D0
 *   ResolveAnimByName          0x00401240 (FindKeyInTaggedTable)
 */
#include "wacki.h"
#include <SDL.h>
#include <string.h>
#include <stdlib.h>

extern void *xmalloc(uint32_t sz);
extern void  xfree  (void *p);

/* ent_ptr_intern / ent_ptr_resolve moved to src/actor/intern.c.
 * Registration table + Find/Register/Unregister moved to
 * src/actor/registration.c (storage there, declarations in
 * src/actor/internal.h). */

#include "actor/internal.h"

/* Entity list management (Link/Unlink/ClearAll, iterators, head globals)
 * moved to src/actor/list.c. */

/* Per-entity VM (scan_for_label, ExecEntityScript, EntityWalkerTick)
 * moved to src/actor/vm.c. EOFF / EOFF8 macros moved to src/actor/internal.h. */


/* ========================================================================= *
 * EntityRenderAll — 1:1 with FUN_00406040 @ 0x00406040.
 *
 * The original allocates a working pointer table at DAT_0045047C, fills it
 * with all linked entities, qsorts by foot_y (LAB_00406190 = cmp callback),
 * then walks it back-to-front. For each:
 *   - flags2 bit 0x1 must be set (visible)
 *   - flags2 bit 0x40 (= 0x40 / 'fade-out') → unlink + free
 *   - flags2 bit 0x4000 = kind3 RLE; 0x8000 = full free path
 *   - else: BlitSpriteScaled* into back buffer at (x,y)
 * ========================================================================= */
static int cmp_entity_y(const void *a, const void *b)
{
    Entity *ea = *(Entity *const *)a;
    Entity *eb = *(Entity *const *)b;
    /* Sort by foot_y at +0x26 — 1:1 with cmp callback LAB_00406190 in
     * the original (disassembled: `mov dx,[ecx+0x26]; mov ax,[ecx+0x26]; sub`).
     * Field +0x26 is `drawn_y + height` (set in ExecEntityScript post-exec
     * when flags & 0x200 == 0), i.e. the *bottom* of the drawn sprite =
     * foot_y. Earlier we sorted by +0x0C, which is the *drawn anchor*: for
     * FLAG_2 (foot-anchored) sprites that's the top-left of the bitmap,
     * giving wrong z-order. */
    int ay = (int)EOFF(ea, 0x26, int16_t);
    int by = (int)EOFF(eb, 0x26, int16_t);
    /* Fallback for entities whose +0x26 hasn't been computed yet
     * (flag 0x200 set, or freshly-spawned before first VM tick) —
     * approximate from anchor Y at +0x24 (the foot coord). */
    if (ay == 0) ay = (int)EOFF(ea, 0x24, int16_t);
    if (by == 0) by = (int)EOFF(eb, 0x24, int16_t);
    return ay - by;
}

/* Actor-paint scratch mask. 1 byte per backbuffer pixel; set to 1 when
 * an ACTOR (g_actor[0] / g_actor[1]) writes a non-transparent pixel.
 * Walk-behind BG-through-mask blits ONLY over these — so kioskb.msk's
 * shape covers Ebek/Fjej walking behind kiosk but doesn't repaint over
 * static entities (banan1, kiosk worker, etc.) that happen to be at
 * higher z-depth than the mask's foot_y. 1:1 conceptual match to the
 * original engine's walk-behind which only affects moving actors, not
 * baked-in scene props. */
static uint8_t *s_actor_paint_mask = NULL;
static int      s_actor_paint_sz = 0;
static void ensure_actor_paint_mask(void)
{
    int need = (int)g_screen_w * g_screen_h;
    if (need > s_actor_paint_sz) {
        free(s_actor_paint_mask);
        s_actor_paint_mask = (uint8_t *)malloc((size_t)need);
        s_actor_paint_sz = s_actor_paint_mask ? need : 0;
    }
    if (s_actor_paint_mask)
        memset(s_actor_paint_mask, 0, (size_t)s_actor_paint_sz);
}

void EntityRenderAll(Entity *head)
{
    (void)head;
    Entity *list[128];
    int n = 0;
    int total = EntityListCount(0);
    for (int i = 0; i < total && n < 128; ++i) {
        Entity *e = EntityListAt(0, i);
        if (e) list[n++] = e;
    }
    if (n > 1) qsort(list, n, sizeof(Entity *), cmp_entity_y);

    /* Reset actor-paint mask for this frame. */
    ensure_actor_paint_mask();

    /* TEMP DIAG: dump render-order when an action sprite is present
     * (atlas name contains 'kwiat'/'hustawk'/etc). Helps trace
     * "double character" z-sort issue. */
    {
        static uint32_t s_last_dump = 0;
        uint32_t now = SDL_GetTicks();
        int has_action = 0;
        for (int j = 0; j < n; ++j) {
            AnimAsset *aa = (AnimAsset *)ent_ptr_resolve(EOFF(list[j], 0x28, uint32_t));
            if (aa && (strstr(aa->name, "kwiat") || strstr(aa->name, "hustawk") ||
                       strstr(aa->name, "cool") || strstr(aa->name, "gadp"))) {
                has_action = 1;
                break;
            }
        }
        if (has_action && now - s_last_dump > 500) {
            extern Entity *g_actor[2];
            s_last_dump = now;
            fprintf(stderr, "[render-order] (action sprite present):\n");
            for (int j = 0; j < n; ++j) {
                Entity *ee = list[j];
                AnimAsset *aa = (AnimAsset *)ent_ptr_resolve(EOFF(ee, 0x28, uint32_t));
                int aidx = (ee == g_actor[0]) ? 0 : (ee == g_actor[1]) ? 1 : -1;
                fprintf(stderr, "  [%2d] +0x26=%d anchor=(%d,%d) draw=(%d,%d) wh=(%d,%d) "
                        "scale=%u flags=0x%04X atlas=%s%s\n",
                        j,
                        (int)EOFF(ee, 0x26, int16_t),
                        (int)EOFF(ee, 0x22, int16_t), (int)EOFF(ee, 0x24, int16_t),
                        (int)EOFF(ee, 0x0A, int16_t), (int)EOFF(ee, 0x0C, int16_t),
                        (int)EOFF(ee, 0x0E, int16_t), (int)EOFF(ee, 0x10, int16_t),
                        (unsigned)EOFF(ee, 0x58, uint16_t),
                        (unsigned)EOFF(ee, 8, uint16_t),
                        aa ? aa->name : "(none)",
                        aidx == 0 ? " <EBEK>" : aidx == 1 ? " <FJEJ>" : "");
            }
        }
    }

    for (int i = 0; i < n; ++i) {
        Entity *e = list[i];
        uint16_t flags = EOFF(e, 8, uint16_t);
        /* Visibility gates — 1:1 with the original FUN_00406040 (renderer)
         * + the per-entity interpreter FUN_004012E0 logic:
         *
         *   bit 0x0001 = renderable (set by AllocEntity → FUN_00405920).
         *                Original renderer's outer-most guard is `flags & 1`.
         *                We don't set it on AllocEntity (legacy), so we just
         *                gate on the hide bits instead.
         *   bit 0x0080 = HIDDEN (op 0x3E HIDE_ENTITY sets, op 0x3F SHOW
         *                clears). EntityRenderAll skips these.
         *   bit 0x2000 = "wait-for-enable" / alpha-plane spawn. In the
         *                original, the per-entity interpreter SKIPS pixel-
         *                buffer prep when this bit is set (entity has a
         *                private pixel buffer that stays zero/transparent
         *                until the script ops fill it). Since our renderer
         *                blits from the atlas directly (not the entity's
         *                private buffer), reading would expose the asset's
         *                full first frame — visible garbage. So skip them
         *                here until the per-entity script enables them via
         *                op 0x3F or a SET_POS_FROM_FRAME path.
         *   bit 0x0040 = fading-out, bit 0x8000 = pending-free.
         */
        if (flags & 0x80)   continue;      /* hidden (op 0x3E) */
        if (flags & 0x40) {
            /* 1:1 with FUN_00406040 @ 0x004060f5: flag 0x40 has a
             * sub-branch on bit 0x20:
             *
             *   flags & 0x20 set   → ONE-SHOT BG BLIT. The original
             *                        renderer clears bit 0x20, calls
             *                        FUN_00411730 (PaintImageToBackbuffer
             *                        — opaque memcpy + dirty-rect), then
             *                        FlushFrameToPrimary. Subsequent
             *                        ticks see only bit 0x40 → skip.
             *
             *   flags & 0x20 clear → already blitted (or genuine
             *                        fade-out) → skip render.
             *
             * Used in komnaty whose entry in the stage table names a
             * stub .pic (e.g. magaz3j.pic = 1×1 palette-only, komnata 5):
             * the real background is spawned as a kind=2 atlas entity
             * via op 0x30 with flags=0x0060 (e.g. magaz3c.wyc), painted
             * once via this branch, then second_va @ 0x0042D828 destroys
             * the entity (id=6 destroy in logs). Earlier port skipped
             * unconditionally on 0x40 → BG never painted → komnata
             * showed the prior scene's framebuffer. */
            if (flags & 0x20) {
                AnimAsset *atlas = (AnimAsset *)ent_ptr_resolve(EOFF(e, 0x28, uint32_t));
                if (atlas && atlas->frame_count && atlas->pixel_ptrs) {
                    uint16_t f = EOFF(e, 0x30, uint16_t);
                    if (f >= atlas->frame_count) f = 0;
                    uint8_t *px = atlas->pixel_ptrs[f];
                    if (px) {
                        uint16_t fw = atlas->off_widths [f];
                        uint16_t fh = atlas->off_heights[f];
                        int16_t bx = (int16_t)EOFF(e, 0x0A, int16_t);
                        int16_t by = (int16_t)EOFF(e, 0x0C, int16_t);
                        PaintImageToBackbuffer(bx, by, fw, fh, px);
                        /* Clear bit 0x20 — mirror of FUN_00406040
                         * `*(ushort *)(piVar2 + 2) = uVar1 & 0xffdf;` */
                        EOFF(e, 8, uint16_t) = (uint16_t)(flags & ~0x20u);
                        FlushFrameToPrimary();
                        /* Stash atlas pixels as the scene's persistent
                         * BG — but ONLY if this sprite is BG-sized.
                         * Original FUN_00406040 doesn't discriminate
                         * (both BG sprites and HUD panel buttons use
                         * flag 0x60 → one-shot blit + persist in
                         * backbuffer); but our port takes a per-frame
                         * full-BG repaint shortcut, so we need to know
                         * which atlas to repaint. HUD buttons (id=100
                         * guziki, id=101 pasek) are 30×30 / 172×3 in
                         * the panel area (Y ≥ 400) and would overwrite
                         * the BG save. Heuristic: ≥ half play-area
                         * width AND tops out above the panel. */
                        if (fw >= 320 && (by < 200 || (by + fh) >= 380))
                            SaveSceneBgAtlas(bx, by, fw, fh, px);
                    }
                }
            }
            continue;
        }
        if (flags & 0x8000) continue;      /* pending free */
        /* NOTE: bit 0x2000 is NOT a visibility flag per the original
         * renderer (FUN_00406040 only checks bit 0x01 / 0x40 / 0xC000).
         * It's an "alpha-plane / static" flag that tells the per-entity
         * interpreter (FUN_004012E0) to skip pixel-buffer prep for
         * non-kind-3 entities. For kind-3 (RLE) entities, the original
         * always decodes — and our port blits directly from atlas so the
         * flag doesn't affect us. Earlier code skipped 0x2000 entirely;
         * that hid the HUD avatar (e1lataje id=100/101 — kind=3 with
         * 0x2000 set permanently per script) plus rob1/szczel HUD-glue.
         * Position validity (`dx <= 0 || dy <= 0`) is the natural gate:
         * entities that never get a positive position via their script
         * (e.g. unresolved SUBSCRIPT_CALL targets) skip on that check. */

        AnimAsset *atlas = (AnimAsset *)ent_ptr_resolve(EOFF(e, 0x28, uint32_t));
        /* kind=1 manual-buffer entities (speech balloons from op 0x09):
         * no atlas binding — pixels live in e->pixels with width/height
         * in e->width/e->height. Direct color-key blit (transparent=0).
         * NOTE: op 0x09 SHOW_TEXT is dead code in the shipped game (audio-
         * only dialogue), so this branch never fires in practice — kept
         * 1:1 with FUN_00406040 in case a future stage's scripts use it. */
        if (!atlas) {
            if (!e->pixels || !e->width || !e->height) continue;
            int16_t bx = (int16_t)EOFF(e, 0x0A, int16_t);
            int16_t by = (int16_t)EOFF(e, 0x0C, int16_t);
            BlitSpriteToBackbuffer((uint16_t)bx, (uint16_t)by,
                                   0, 0, e->width, e->height,
                                   e->width, e->height,
                                   e->pixels, 0);
            continue;
        }
        if (atlas->frame_count == 0) continue;
        uint16_t f = EOFF(e, 0x30, uint16_t);
        if (f >= atlas->frame_count) f = 0;
        uint8_t *px = atlas->pixel_ptrs ? atlas->pixel_ptrs[f] : NULL;
        if (!px) continue;
        uint16_t fw = atlas->off_widths [f];
        uint16_t fh = atlas->off_heights[f];
        /* Position: SET_POS_FROM_FRAME (opcode 0x05) normally writes the
         * atlas drawX/drawY[frame] into e+0xA/0xC; with the walker disabled
         * those stay at 0. Fall back to the atlas hot-spot directly — which
         * for prop atlases (drut/barstoi/pies) IS the scene-local position. */
        int16_t  dx = (int16_t)EOFF(e, 0x0A, int16_t);
        int16_t  dy = (int16_t)EOFF(e, 0x0C, int16_t);
        int       fallback_used = 0;
        if (dx == 0 && dy == 0 && atlas->off_drawX && atlas->off_drawY) {
            dx = (int16_t)atlas->off_drawX[f];
            dy = (int16_t)atlas->off_drawY[f];
            fallback_used = 1;
        }
        (void)fallback_used;
        /* Position validity:
         *   - dx,dy <= 0 → atlas hot-spot is a foot-anchor (negative because
         *     it offsets the sprite ABOVE the foot point). These entities
         *     MUST be positioned by the per-entity script (op 0x05/0x15/0x16)
         *     before they're meaningful on-screen. Without the walker, we
         *     can't synthesise a sensible position — render at (320, 380)
         *     would just plant the actor mid-screen, regardless of whether
         *     they're supposed to be there. Skip them entirely; the user
         *     will notice "missing" rather than "wrong-place" garbage.
         *   - dx,dy > 0 → scene-baked position (drut, barstoi, pies, …) —
         *     render at that hot-spot. */
        /* Off-screen skip — see game.c unified-draw comment. Generous
         * on the right edge so e1lataje (glizda) renders its full
         * leftward flight from x=700 down past x=0. */
        if ((int)dx + (int)fw <= 0) continue;
        if ((int)dy + (int)fh <= 0) continue;
        if (dx >= 640 + 200) continue;
        if (dy >= 400 + 200) continue;
        uint16_t scale = EOFF(e, 0x58, uint16_t);

        /* RLE-decode kind=3 frames into per-call scratch. */
        if (atlas->kind == 3) {
            int need = (int)fw * (int)fh;
            static uint8_t *s_scratch = NULL;
            static int      s_scratch_sz = 0;
            if (need > s_scratch_sz) {
                free(s_scratch);
                s_scratch = (uint8_t *)malloc((size_t)need);
                s_scratch_sz = s_scratch ? need : 0;
            }
            if (!s_scratch) continue;
            DepackRleFrame(px, s_scratch, need);
            px = s_scratch;
        }

        /* T-walk-behind: 1bpp packed MASK shape → BG-through-mask blit.
         * Detection: asset is .msk (kind == 0) AND flag_22 bit 1 clear
         * (= NOT 8bpp visible pixels). The mask's pixel data is a 1bpp
         * packed shape (stride = (w+7)/8 bytes per row, MSB-first).
         *
         * Effect: where the mask bit is set, copy the BG pixel at the
         * same global (x, y) onto the backbuffer. This re-covers any
         * actor pixels with the tree / building / foreground BG art that
         * the scene's .pic carries. Sort order (foot_y at +0x26) ensures
         * we run AFTER actors whose foot_y is smaller (= further from
         * camera than the mask) → actor walks BEHIND the mask region.
         *
         * Without this, walk-behind .msk entities would either not
         * render (their entity layer wasn't linked into the render list)
         * or render as 8bpp garbage (current bug — masks are 1bpp shapes
         * but blitted as opaque 8bpp). 1:1-style with the original engine:
         * FUN_00406040 iterates a single render list including walk-
         * behind masks and FUN_004120c0 does the actual blit; the mask
         * blit path consults the BG pixel buffer for source data instead
         * of asset pixels for the shape-only case.
         *
         * Skipped at scale != 100 because perspective-scaled masks need
         * additional plumbing (none of the walk-behind .msk files we've
         * inspected so far carry a scale flag, so this is a safe skip). */
        extern void  *g_scene_bg_raw;
        extern uint32_t g_scene_bg_size;
        if (atlas->kind == 0 && (atlas->flag_22 & 2) == 0 &&
            g_scene_bg_raw && g_scene_bg_size > 776 &&
            (scale == 0 || scale == 100)) {
            const uint8_t *bg = (const uint8_t *)g_scene_bg_raw;
            uint16_t bg_w = (uint16_t)(bg[4] | (bg[5] << 8));
            uint16_t bg_h = (uint16_t)(bg[6] | (bg[7] << 8));
            const uint8_t *bg_pixels = bg + 776;
            int mask_stride = (fw + 7) / 8;
            int dx_i = (int)dx, dy_i = (int)dy;
            for (int my = 0; my < (int)fh; ++my) {
                int gy = dy_i + my;
                if (gy < 0 || gy >= (int)bg_h) continue;
                if (gy >= (int)g_screen_h) break;
                const uint8_t *row_src = px + (size_t)my * mask_stride;
                const uint8_t *bg_row = bg_pixels + (size_t)gy * bg_w;
                uint8_t *dst_row = g_back_shadow
                                 + (size_t)gy * g_screen_w;
                const uint8_t *paint_row = s_actor_paint_mask
                    ? s_actor_paint_mask + (size_t)gy * g_screen_w : NULL;
                for (int mx = 0; mx < (int)fw; ++mx) {
                    int gx = dx_i + mx;
                    if (gx < 0) continue;
                    if (gx >= (int)bg_w || gx >= (int)g_screen_w) break;
                    /* MSB-first packing (matches .fld convention). */
                    uint8_t bit = (uint8_t)(row_src[mx >> 3]
                                            & (0x80 >> (mx & 7)));
                    if (!bit) continue;
                    /* Walk-behind only affects actors — skip BG restore
                     * if no actor pixel was painted here. */
                    if (paint_row && !paint_row[gx]) continue;
                    dst_row[gx] = bg_row[gx];
                }
            }
            continue;   /* skip normal blit — mask handled. */
        }
        int blit_w, blit_h;
        /* PORT SHORTCUT — perspective-scaled actor blit. Original engine
         * gated this on flag 0x400; our actors don't get that flag, but
         * we want the perspective-shrink effect (sprite shrinks as actor
         * walks into the scene depth, smaller Y). Apply scaling whenever
         * +0x58 != 0 && != 100, AND recompute drawn from anchor +
         * scaled atlas hot-spot using SAME scale as blit — so foot stays
         * at script anchor regardless of frame transitions or scale
         * changes between EntityWalkerTick (uses OLD scale) and
         * UpdateActorMovement (sets NEW scale). Without this recompute,
         * sprite position jumps when scale changes ("animation goes
         * wild during action"). */
        if (scale && scale != 100) {
            uint16_t dw = (uint16_t)((fw * scale) / 100);
            uint16_t dh = (uint16_t)((fh * scale) / 100);
            if (dw == 0) dw = 1;
            if (dh == 0) dh = 1;
            /* Timing-safe foot anchor: override post-exec's pre-computed
             * drawn pos with one based on CURRENT anchor + CURRENT scale
             * × atlas hot-spot. Only for FLAG_2 (foot-anchored) entities. */
            if (EOFF(e, 0x3A, uint16_t) & 2 &&
                atlas->off_drawX && atlas->off_drawY &&
                f < atlas->frame_count) {
                int16_t anchor_x = EOFF(e, 0x22, int16_t);
                int16_t anchor_y = EOFF(e, 0x24, int16_t);
                int16_t hx = (int16_t)atlas->off_drawX[f];
                int16_t hy = (int16_t)atlas->off_drawY[f];
                dx = (int16_t)(anchor_x + ((int32_t)hx * scale) / 100);
                dy = (int16_t)(anchor_y + ((int32_t)hy * scale) / 100);
            }
            if ((flags & 0x100) && !(flags & 0x4)) {
                BlitAlphaScaledToBackbuffer(dx, dy, fw, fh, dw, dh, px, 2);
            } else {
                BlitSpriteScaledColorKeyFlip(dx, dy, fw, fh, dw, dh, px,
                                             (flags & 0x4) ? 1 : 0);
            }
            blit_w = dw; blit_h = dh;
        } else {
            BlitSpriteToBackbuffer((uint16_t)dx, (uint16_t)dy, 0, 0,
                                   fw, fh, fw, fh, px, 0);
            blit_w = fw; blit_h = fh;
        }

        /* Walk-behind paint-mask: mark destination pixels for entities
         * that should be occluded by walk-behind masks. Two cases:
         *   1. Actors (g_actor[0/1]) — always marked. Ebek/Fjej walk
         *      behind buildings via the standard walk-behind effect.
         *   2. "Sky" entities — those whose entire sprite sits ABOVE
         *      the walkable floor (entity_bottom < walk_top). These are
         *      flying things (airplane, ufo, witch, birds) that should
         *      be occluded by tall buildings between camera and sky.
         *
         * Static foreground props (banan on dumpster, kioskar1 worker)
         * have foot_y INSIDE or BELOW the walk area — not marked, so
         * walk-behind mask doesn't erase them (Fix #30 use-case still
         * preserved without the actor-only restriction).
         *
         * Fix #43 — sky gate now requires BOTH geometric position AND
         * spawn flag 0x2000 (set on genuine sky entities like
         * samolot3/ufo via op 0x30 SPAWN; cleared on foreground props).
         * Without the flag check, foreground props that happen to be
         * positioned above the walk area (e.g. foto.wyc camera lifted
         * to (356, 18) during use-camera anim in foto3.pic) get
         * incorrectly painted over by foto3.msk's walk-behind blit —
         * the lifted camera disappears, leaving the BG-baked photo-
         * machine visible. The combined gate still excludes HUD glue
         * (e1lataje carries 0x2000 too but lives at y>300 so
         * entity_bottom_y > walk_fld_oy = 304 in stage 2). */
        int is_actor = (e == g_actor[0] || e == g_actor[1]);
        extern int16_t g_walk_fld_oy;       /* top Y of walkable area */
        int entity_bottom_y = dy + blit_h;
        int is_sky = (g_walk_fld_oy > 0 && entity_bottom_y <= (int)g_walk_fld_oy
                      && (flags & 0x2000));
        if (s_actor_paint_mask && (is_actor || is_sky)) {
            int x0 = dx, y0 = dy;
            int x1 = dx + blit_w, y1 = dy + blit_h;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > (int)g_screen_w) x1 = g_screen_w;
            if (y1 > (int)g_screen_h) y1 = g_screen_h;
            for (int yy = y0; yy < y1; ++yy) {
                uint8_t *m = s_actor_paint_mask + (size_t)yy * g_screen_w;
                for (int xx = x0; xx < x1; ++xx) {
                    m[xx] = 1;
                }
            }
        }
    }
}

/* perspective globals owned by assets.c */
extern int16_t g_persp_profile[];
extern int     g_persp_band_count;
extern uint16_t g_cursor_speed;            /* DAT_0044A198 — undefined2 in Ghidra */
extern uint16_t g_perspective_min;
extern uint16_t g_perspective_step;
extern uint16_t g_active_actor;
extern uint16_t g_active_target_y;        /* DAT_0044E5A8 — last mouse Y */

/* shared "is there a click this tick?" flag (graphics.c sets it via WndProc) */
extern uint8_t g_lmb_clicked;
extern uint8_t g_lmb_handled;             /* DAT_0044E5A4 */
extern uint16_t g_settings_anim_active;   /* DAT_0044E448 (T121) */
/* g_game_over_code = macro alias for g_script_vars[14]; defined in wacki.h. */

/* ------------------------------------------------------------------------- *
 * InitEntityBitmap — 0x00405920
 * Allocates the backing pixel buffer based on (group_flags) bits:
 *   bit 0x01 = primary image plane
 *   bit 0x04 = secondary plane (mask / shadow)
 * ------------------------------------------------------------------------- */
static Entity *InitEntityBitmap(Entity *e, uint16_t w, uint16_t h)
{
    if (!e) return NULL;
    uint32_t pixels = (uint32_t)w * h;
    uint32_t prim = (e->group_flags & 0x0001) ? pixels : 0;
    uint32_t sec  = (e->group_flags & 0x0004) ? pixels : 0;
    if (prim + sec) {
        if (e->pixels) xfree(e->pixels);
        uint8_t *buf = (uint8_t *)xmalloc(prim + sec);
        if (!buf) { xfree(e); return NULL; }
        e->pixels = buf;
    }
    e->flags1   |= 0x01;
    e->width     = w;
    e->height    = h;
    return e;
}

/* ------------------------------------------------------------------------- *
 * AllocEntity — 0x00405A00
 * ------------------------------------------------------------------------- */
Entity *AllocEntity(uint16_t w, uint16_t h, uint16_t kind, uint16_t flags)
{
    Entity *e = (Entity *)xmalloc(sizeof *e);
    if (!e) return NULL;
    memset(e, 0, sizeof *e);
    e->kind        = kind;
    e->group_flags = flags;
    return InitEntityBitmap(e, w, h);
}

/* ------------------------------------------------------------------------- *
 * FreeEntity — 0x004058F0
 * ------------------------------------------------------------------------- */
void FreeEntity(Entity *e)
{
    if (!e) return;
    if (e->pixels) xfree(e->pixels);
    xfree(e);
}

/* Register/Unregister/UnregisterFirstKindIdMatch/UnregisterEntityByPtr
 * moved to src/actor/registration.c. */

/* ------------------------------------------------------------------------- *
 * Per-actor walk-anim selector (directional sprite table is at
 * StageDef::ebek_wyc + 0xC, [0]=L, [1]=R, [2]=U, [3]=D in the original).
 *
 * 1:1 with UpdateActorMovement tail @ 0x004061D0 line 1054+:
 *     sVar11 = psVar3[2] - *(short *)(iVar2 + 0x22);   // target - anchor X
 *     sVar8  = psVar3[3] - *(short *)(iVar2 + 0x24);   // target - anchor Y
 *     if (sVar8 < sVar11) horizontal else vertical.
 *
 * Earlier port read `actor->target_anim_x` (named field in trailing zone,
 * never initialised) so dx/dy always reduced to (target - 0) → ALWAYS picked
 * dir_anims[1] (right) for positive target — actors visibly walked-right
 * regardless of click position.                                            */
/* (select_walk_anim removed — same algorithm lives inline in
 * BindActorWalker below, the sole consumer after UpdateActorMovement's
 * auto-bind branch was removed in fix #10.) */

/* helpers from script.c / game.c */
extern uint16_t FindKeyInTaggedTable(const char *table, char tag, int16_t key); /* 0x00401240 */
extern void     PlayActorAnimByPath(Entity *e, const char *path,
                                    int16_t target_x, int16_t target_y);

/* ------------------------------------------------------------------------- *
 * UpdateActorMovement — 0x004061D0
 *
 * Earlier port used NAMED Entity struct fields (target_anim_x/y,
 * z_perspective_off, walk_dx_remaining, walk_dy_remaining, state_flags) —
 * all of which sit in the trailing zone (byte 0xE0+) since #100 and are
 * NEVER initialised by the rest of the port. Reads returned 0, writes
 * went to dead memory. Effect:
 *   - perspective scale at +0x58 never updated (renderer fell back to
 *     constant scale, perspective Y rescaling broken)
 *   - walker mid-walk re-triggered on every click (no skip)
 *   - script-bound walk path (+0x3A bit 2) never honoured
 *   - direction selector saw dx = target-0, dy = target-0 → right-walk
 *     anim for every click position
 *
 * Fix: switch to raw byte access via EOFF() matching original FUN_004061D0.
 * ------------------------------------------------------------------------- */
void UpdateActorMovement(int16_t target_x, int16_t target_y)
{
    extern Entity *g_actor[2];                /* DAT_0044E724/0728 */
    extern const char *g_actor_walk_anim[2][6]; /* dir + aux + idle  */

    if (!(g_settings_anim_active & 0x02)) return;

    for (int i = 0; i < 2; ++i) {
        Entity *a = g_actor[i];
        if (!a) continue;
        /* flags1 (script byte +0x08) bit 0x80 = hidden — original tests
         * `*(byte *)(iVar2 + 8) & 0x80`. a->flags1 happens to sit at +0x08
         * which matches, so direct read is safe. */
        if (a->flags1 & 0x80) continue;

        /* PORT SHORTCUT (refer FUN_004061D0 perspective block + op 0x3E
         * hide-partner suppression in stubs.c ScriptCallHideEnt):
         * Original verb scripts paired op 0x40 SET_PERSPECTIVE (re-bias
         * perspective so the ACTIVE actor at far distance scales toward
         * 0) with op 0x3E HIDE_ENTITY on BOTH actors (clearing the stage
         * for a cinematic action shot). The partner-hide is unwanted UX
         * in our port (suppressed), but then the active-actor's
         * perspective bias ALSO applies to the partner (same formula
         * loops over both actors) → at street level y=380 with cs=265,
         * z=265-36=229→clamp 160 = 160% size = partner balloons. Mute
         * the perspective update for the PARTNER (the actor NOT performing
         * the action) when perspective globals are non-default so partner
         * stays at her last computed scale (typically ~100% baseline)
         * while active actor shrinks into the distance as intended. */
        extern uint16_t g_active_actor;
        int is_partner = ((int)(g_active_actor & 1u)) != i;
        int persp_modified = (g_cursor_speed != 0x78 ||
                              g_perspective_min != 4 ||
                              g_perspective_step != 7);
        if (is_partner && persp_modified) {
            /* Keep partner's +0x58 frozen at last value (don't update). */
        } else {
            /* Perspective Y bias → byte +0x58 (script scale slot) — 1:1 with
             *   *(short *)(iVar2 + 0x58) = (short)iVar10;
             * Source is anchor Y at +0x24, not the trailing-zone alias. */
            int anchor_y = EOFF(a, 0x24, int16_t);
            int z = (int)g_cursor_speed -
                    ((400 - anchor_y) * (int)g_perspective_min) /
                    (int)g_perspective_step;
            if (z < 0) z = 0;
            if (z > 0xA0) z = 0xA0;
            EOFF(a, 0x58, int16_t) = (int16_t)z;
        }

        /* Skip when "frozen" via script flag bit 0x8000 of (script_var[i+1]).
         * Original tests `(&DAT_00449880)[*local_c]` where local_c walks 0/1.
         * g_script_vars layout matches: per-actor flags at index 1 / 2. */
        if (g_script_vars[i + 1] & 0x8000) continue;

        /* T-input-order followup: the original engine's auto-walker-bind
         * branch (FUN_004061D0 line 1054+) lived here, gated on
         * DAT_0044E5A4 (g_lmb_handled). In our port HandleSceneInput at
         * the top of PGFT Inner is now the SOLE click-driven walker-bind
         * path (item-click → verb script + its own op 0x10/0x11/0x12;
         * free-walk click → BindActorWalker(mouse_pos)).
         *
         * Keeping the auto-bind here turned out to be ACTIVELY harmful:
         * if the user clicks during a long blocking-wait pump inside
         * DispatchClickEvent (e.g., 5+ second banana animation), the
         * stray click sets g_lmb_clicked=1, HandleSceneInput's re-entry
         * guard silently rejects it BUT leaves the flag set, snapshot
         * in the next inner pump sees g_lmb_handled=1, and THIS branch
         * rebound the walker to wherever the cursor happened to be.
         * Empirically: target=(23,0) "from=(436,-1611)" walker binds
         * mid-script that teleported Fjej off-screen.
         *
         * Removed. Perspective scale update above stays — that's the
         * meaningful per-frame work of UpdateActorMovement. */
        (void)target_x; (void)target_y;
    }
}

/* ------------------------------------------------------------------------- *
 * BindActorWalker — public helper that binds the per-entity VM walker to
 * an actor at a resolved (target_x, target_y). 1:1 with the walker-bind
 * tail of UpdateActorMovement (FUN_004061D0 lines ~1054-1064 of decompile)
 * but callable directly from play_demo_scene's click handler, which is the
 * shortest path to T3 (S3 phase 3) walker unification without needing T1
 * full activation (g_lmb_handled gate, currently dormant due to walker
 * conflict — see #14.3 in REVIEW).
 *
 * Picks direction anim from g_actor_walk_anim[actor_idx][0..3] (L/R/U/D)
 * based on |dx| vs |dy| dominance, then patches op 0x15 in the bytecode
 * with the resolved target and binds entity[+0x2C].
 *
 * Returns: 1 if walker bound, 0 if no bytecode available for that direction. */
extern int is_walkable_at(int sx, int sy);
extern int16_t g_persp_profile[0x22 * 2];
extern int     g_persp_band_count;

/* ==========================================================================
 *  Waypoint Dijkstra path-finder — 1:1 port of FUN_00404600 / FUN_004046b0 /
 *  FUN_00404840 / FUN_00406510 epilogue. Each actor owns a 3486-byte
 *  waypoint graph; nodes are scene perspective bands (loaded from FILD body
 *  by LoadAssetFromDtaBase, see assets.c). Edges connect band pairs whose
 *  straight line is fully walkable. BFS from TARGET fills bfs_dist[]; we
 *  pick the SOURCE-connected band with min total cost and reconstruct the
 *  hop chain via bfs_back[].
 *
 *  Walker consumption (matches original D9C decrement logic):
 *    - selected_path[0] = source-adjacent band (high BFS level)
 *    - selected_path[last] = target-adjacent band (low BFS level)
 *    - path_index starts at last → walker walks to TARGET-SIDE band first
 *      ("most ambitious leg"); if walker drains short (clip at wall),
 *      next tick decrements path_index → walker tries SHORTER leg toward
 *      a closer-to-source band. Continues until walker reaches a band
 *      or path_index reaches -1 → walker is bound to the final (original)
 *      target.                                                            */

#define WP_MAX_BANDS 0x22   /* 34 slots — caps DAT_0044A200 (LoadAssetFromDtaBase) */
#define WP_MAX_EDGES 0x200  /* 512 edges — matches FUN_004046b0 cap */
#define WP_BAND_SRC  0xFE   /* edge.from/to marker for source pseudo-node */
#define WP_BAND_TGT  0xFF   /* edge.from/to marker for target pseudo-node */

#pragma pack(push, 1)
typedef struct WPEdge {
    uint8_t from;
    uint8_t to;
    int32_t dist;
} WPEdge;
#pragma pack(pop)

typedef struct ActorWaypoints {
    WPEdge   edges[WP_MAX_EDGES];          /* original +0x000 */
    int16_t  band_x[WP_MAX_BANDS];         /* original +0xC00 */
    int16_t  band_y[WP_MAX_BANDS];         /* original +0xC44 */
    int16_t  bfs_state[WP_MAX_BANDS];      /* original +0xC88 (-1 = unvisited, ≥0 = BFS level) */
    int32_t  bfs_dist[WP_MAX_BANDS];       /* original +0xCCC */
    uint8_t  bfs_back[WP_MAX_BANDS];       /* original +0xD54 (0xFF = TARGET marker) */
    uint8_t  selected_path[WP_MAX_BANDS];  /* original +0xD76 */
    uint16_t edge_count;                    /* original +0xD98 */
    uint16_t baseline_count;                /* original +0xD9A (saved by SceneInit) */
    int16_t  path_index;                    /* original +0xD9C (decrements from last to -1) */
    int16_t  final_x, final_y;              /* port: original clip target after all waypoints */
    uint8_t  path_active;                   /* port: 1 = walker is on a waypoint chain */
} ActorWaypoints;

static ActorWaypoints s_wp[2];

/* line_reaches — Bresenham line from (sx,sy) to (tx,ty), checks every
 * pixel for walkability. Returns 1 if line reaches (tx,ty) walking only on
 * walkable pixels, 0 if blocked. (sx,sy) is NOT checked (assumed walker
 * is already there). */
static int line_reaches(int sx, int sy, int tx, int ty)
{
    if (sx == tx && sy == ty) return 1;
    int ddx = tx - sx, ddy = ty - sy;
    int adx = ddx < 0 ? -ddx : ddx;
    int ady = ddy < 0 ? -ddy : ddy;
    int maxlen = adx > ady ? adx : ady;
    if (maxlen == 0) return 1;
    int64_t cx_fp = (int64_t)sx << 16;
    int64_t cy_fp = (int64_t)sy << 16;
    int64_t inc_x = ((int64_t)ddx << 16) / maxlen;
    int64_t inc_y = ((int64_t)ddy << 16) / maxlen;
    for (int s = 0; s < maxlen; ++s) {
        cx_fp += inc_x; cy_fp += inc_y;
        int nx = (int)(cx_fp >> 16);
        int ny = (int)(cy_fp >> 16);
        if (!is_walkable_at(nx, ny)) return 0;
    }
    return 1;
}

/* line_clip — like line_reaches but also returns the last walkable pixel
 * along the line. out_clip = sx,sy if even first step is non-walkable. */
static int line_clip(int sx, int sy, int tx, int ty,
                      int *out_clip_x, int *out_clip_y)
{
    int ddx = tx - sx, ddy = ty - sy;
    int adx = ddx < 0 ? -ddx : ddx;
    int ady = ddy < 0 ? -ddy : ddy;
    int maxlen = adx > ady ? adx : ady;
    if (maxlen == 0) {
        if (out_clip_x) *out_clip_x = sx;
        if (out_clip_y) *out_clip_y = sy;
        return 1;
    }
    int64_t cx_fp = (int64_t)sx << 16;
    int64_t cy_fp = (int64_t)sy << 16;
    int64_t inc_x = ((int64_t)ddx << 16) / maxlen;
    int64_t inc_y = ((int64_t)ddy << 16) / maxlen;
    int last_x = sx, last_y = sy;
    int reached = 1;
    for (int s = 0; s < maxlen; ++s) {
        cx_fp += inc_x; cy_fp += inc_y;
        int nx = (int)(cx_fp >> 16);
        int ny = (int)(cy_fp >> 16);
        if (!is_walkable_at(nx, ny)) { reached = 0; break; }
        last_x = nx; last_y = ny;
    }
    if (out_clip_x) *out_clip_x = last_x;
    if (out_clip_y) *out_clip_y = last_y;
    return reached;
}

/* FUN_004046b0 — for each band slot in [start_band, band_count), if a
 * straight line from (cx,cy) to band reaches exactly, append an edge
 * (from_id → band) with Manhattan distance.                              */
static void wp_add_edges_from(ActorWaypoints *wp, int16_t cx, int16_t cy,
                               int start_band, uint8_t from_id)
{
    int total = g_persp_band_count;
    if (total > WP_MAX_BANDS) total = WP_MAX_BANDS;
    for (int idx = start_band; idx < total; ++idx) {
        if (wp->edge_count >= WP_MAX_EDGES) break;
        int16_t bx = wp->band_x[idx], by = wp->band_y[idx];
        if (!line_reaches(cx, cy, bx, by)) continue;
        int dx = bx - cx, dy = by - cy;
        int adx = dx < 0 ? -dx : dx;
        int ady = dy < 0 ? -dy : dy;
        wp->edges[wp->edge_count].from = from_id;
        wp->edges[wp->edge_count].to   = (uint8_t)idx;
        wp->edges[wp->edge_count].dist = adx + ady;
        wp->edge_count++;
    }
}

/* FUN_00404600 — scene init. Called once per scene (from
 * ScriptCallBgMaskSetup after FILD body bands are loaded). Copies scene
 * bands into actor waypoint slots and pre-builds edges between bands
 * 4..N-1 so per-pathfind only adds source/target edges.                  */
void ActorWaypointsSceneInit(int actor_idx)
{
    if (actor_idx < 0 || actor_idx > 1) return;
    ActorWaypoints *wp = &s_wp[actor_idx];
    wp->edge_count = 0;
    wp->path_index = -1;
    wp->path_active = 0;

    /* Zero perimeter slots (0..3) — partner-obstacle perimeter goes here
     * in FUN_00404840 if active; otherwise overwritten by scene bands. */
    for (int i = 0; i < 4; ++i) {
        wp->band_x[i] = 0; wp->band_y[i] = 0;
    }
    /* Copy scene bands into slots 0..N-1 (overwrites perimeter zeros). */
    int n = g_persp_band_count;
    if (n > WP_MAX_BANDS) n = WP_MAX_BANDS;
    for (int b = 0; b < n; ++b) {
        wp->band_x[b] = g_persp_profile[b * 2 + 0];
        wp->band_y[b] = g_persp_profile[b * 2 + 1];
    }
    /* Build edges between bands starting from slot 4 (original loop). */
    if (4 < n - 1) {
        for (int i = 4; i < n - 1; ++i) {
            wp_add_edges_from(wp, wp->band_x[i], wp->band_y[i], i + 1, (uint8_t)i);
        }
    }
    wp->baseline_count = wp->edge_count;
}

/* FUN_00404840 — per-pathfind BFS Dijkstra. Called by BindActorWalker
 * when straight line from actor to target is blocked. Builds source/target
 * pseudo-edges, expands BFS from TARGET frontier, picks SOURCE-side band
 * with min total cost, reconstructs hop chain in selected_path[].
 * Sets path_index = path_len - 1 so consumer reads target-adjacent first.
 * Returns: path length (0 = no path found, walker should use original
 * clipped target instead).                                                */
static int wp_find_path(ActorWaypoints *wp,
                         int16_t source_x, int16_t source_y,
                         int16_t target_x, int16_t target_y)
{
    int n = g_persp_band_count;
    if (n > WP_MAX_BANDS) n = WP_MAX_BANDS;
    wp->edge_count = wp->baseline_count;

    /* Source edges (band 0xFE) — from start position to walkable bands.
     * Original starts from band 4 when no partner-obstacle (slots 0..3
     * are scene bands but excluded since edges between them were already
     * pre-built in SceneInit starting from slot 4). */
    wp_add_edges_from(wp, source_x, source_y, 4, WP_BAND_SRC);
    /* Target edges (band 0xFF) — from target to all bands (slots 0+). */
    wp_add_edges_from(wp, target_x, target_y, 0, WP_BAND_TGT);

    if (n == 0) {
        wp->path_index = -1;
        return 0;
    }

    /* BFS init: all bands unvisited. */
    for (int b = 0; b < n; ++b) {
        wp->bfs_state[b] = -1;
        wp->bfs_dist[b]  = -1;
        wp->bfs_back[b]  = WP_BAND_TGT;
    }
    /* Mark TARGET-adjacent bands as frontier level 1. */
    for (uint16_t e = 0; e < wp->edge_count; ++e) {
        WPEdge *ed = &wp->edges[e];
        int band = -1;
        if (ed->from == WP_BAND_TGT) band = ed->to;
        else if (ed->to == WP_BAND_TGT) band = ed->from;
        if (band >= 0 && band < n) {
            wp->bfs_state[band] = 1;
            wp->bfs_back[band]  = WP_BAND_TGT;
            wp->bfs_dist[band]  = ed->dist;
        }
    }
    /* BFS expand levels — outer loop iterates until no new band discovered. */
    int level = 0;
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int b = 0; b < n; ++b) {
            if (wp->bfs_state[b] != level + 1) continue;
            for (uint16_t e = 0; e < wp->edge_count; ++e) {
                WPEdge *ed = &wp->edges[e];
                int other = -1;
                if (ed->from == b) other = ed->to;
                else if (ed->to == b) other = ed->from;
                if (other < 0 || other >= n) continue;
                int32_t new_dist = wp->bfs_dist[b] + ed->dist;
                if (wp->bfs_state[other] == -1) {
                    wp->bfs_state[other] = level + 2;
                    wp->bfs_back[other]  = (uint8_t)b;
                    wp->bfs_dist[other]  = new_dist;
                    changed = 1;
                } else if ((uint32_t)new_dist < (uint32_t)wp->bfs_dist[other]) {
                    wp->bfs_dist[other] = new_dist;
                    wp->bfs_back[other] = (uint8_t)b;
                }
            }
        }
        level++;
    }
    /* Find SOURCE-connected band with min total cost (= bfs_dist[band] +
     * source-edge dist). 1:1 with FUN_00404840 final loop. */
    uint32_t best_total = 0xFFFFFFFFu;
    int best_band = -1;
    for (uint16_t e = 0; e < wp->edge_count; ++e) {
        WPEdge *ed = &wp->edges[e];
        int band = -1;
        if (ed->from == WP_BAND_SRC) band = ed->to;
        else if (ed->to == WP_BAND_SRC) band = ed->from;
        if (band < 0 || band >= n) continue;
        if (wp->bfs_dist[band] < 0) continue;
        uint32_t total = (uint32_t)wp->bfs_dist[band] + (uint32_t)ed->dist;
        if (total < best_total) {
            best_total = total;
            best_band  = band;
        }
    }
    if (best_band < 0) {
        wp->path_index = -1;
        return 0;
    }
    /* Reconstruct selected_path[] from best_band → bfs_back chain →
     * target marker. selected_path[0] = best_band (source-side),
     * selected_path[last] = target-adjacent band.                       */
    int idx = 0;
    int cur = best_band;
    while (cur != WP_BAND_TGT && idx < WP_MAX_BANDS) {
        wp->selected_path[idx++] = (uint8_t)cur;
        cur = wp->bfs_back[cur];
    }
    /* Forward progression: walker starts at selected_path[0] = source-side
     * band, then increments toward selected_path[len-1] = target-side band,
     * then final target. Original original used DECREMENT (D9C starts at
     * last, walks to source-side first) — which produces visible "walking
     * backwards" because target was at end of physical chain; intuitive
     * forward direction matches user expectation and walker geometry. */
    wp->path_index = 0;
    return idx;
}

/* Track total path length so AdvanceTick knows when to switch to final. */
static int s_wp_path_len[2] = {0, 0};

/* Per-tick advance — when walker drains, decrement path_index and re-bind
 * walker to next waypoint (or final target if path_index hits -1).
 * 1:1 with FUN_00406510 epilogue's D9C decrement, invoked from
 * UpdateActorMovement's per-tick bVar5 (walker-idle) branch.            */
extern int BindActorWalkerDirect(int actor_idx, int target_x, int target_y);
int BindActorWalker(int actor_idx, int target_x, int target_y);
void PerActorWaypointAdvanceTick(void)
{
    extern Entity *g_actor[2];
    for (int i = 0; i < 2; ++i) {
        ActorWaypoints *wp = &s_wp[i];
        if (!wp->path_active || !g_actor[i]) continue;
        uint8_t *eb = (uint8_t *)g_actor[i];
        uint32_t wdx = *(uint32_t *)(eb + 0x4C);
        uint32_t wdy = *(uint32_t *)(eb + 0x50);
        if (wdx != 0 || wdy != 0) continue;   /* walker still moving */

        /* Walker drained — advance: increment index, pick next waypoint.
         * When path_index >= path_len, switch to final original target. */
        wp->path_index++;
        int16_t next_x, next_y;
        int is_final_leg = 0;
        if (wp->path_index >= s_wp_path_len[i]) {
            next_x = wp->final_x;
            next_y = wp->final_y;
            wp->path_active = 0;
            is_final_leg = 1;
        } else {
            int n = g_persp_band_count;
            uint8_t band = wp->selected_path[wp->path_index];
            if (band >= n) {
                wp->path_active = 0;
                continue;
            }
            next_x = wp->band_x[band];
            next_y = wp->band_y[band];
        }
        int16_t cur_x = EOFF(g_actor[i], 0x22, int16_t);
        int16_t cur_y = EOFF(g_actor[i], 0x24, int16_t);
        if (next_x == cur_x && next_y == cur_y) continue;  /* already there */
        if (is_final_leg)
            BindActorWalker(i, (int)next_x, (int)next_y);
        else
            BindActorWalkerDirect(i, (int)next_x, (int)next_y);
    }
}

/* BindActorWalkerDirect — anim-bind only, NO path-finder. Used by waypoint
 * advance to avoid re-triggering Dijkstra on every leg, and by callers that
 * already know target is reachable. */
int BindActorWalkerDirect(int actor_idx, int target_x, int target_y);
int BindActorWalker(int actor_idx, int target_x, int target_y)
{
    extern Entity *g_actor[2];
    if (actor_idx < 0 || actor_idx > 1) return 0;
    Entity *a = g_actor[actor_idx];
    if (!a) return 0;

    /* Path-find — 1:1 with FUN_00406510 (clip) + FUN_00404840 (BFS Dijkstra).
     * Phases:
     *   1. Phase 1 (FUN_00406510 lines 11-26): back-step along line from
     *      target toward start to first walkable pixel.
     *   2. Phase 2 (FUN_00406510 lines 37-50): forward-step from anchor
     *      to walkable target, stop at first wall pixel.
     *   3. Phase 3 (FUN_00406510 epilogue + FUN_00404840): if clipped target
     *      ≠ original target, run BFS Dijkstra over waypoint graph; bind
     *      walker to first hop in selected_path (target-side band).
     *      PerActorWaypointAdvanceTick handles subsequent hops as walker
     *      drains. */
    int sx = (int)EOFF(a, 0x22, int16_t);
    int sy = (int)EOFF(a, 0x24, int16_t);
    int tx = target_x, ty = target_y;
    ActorWaypoints *wp = &s_wp[actor_idx];

    /* Reset any in-flight waypoint path — new BindActorWalker = fresh intent. */
    wp->path_active = 0;
    wp->path_index  = -1;

    /* Phase 1 — back-step to walkable. */
    if (!is_walkable_at(tx, ty)) {
        int ddx = sx - tx, ddy = sy - ty;
        int adx0 = ddx < 0 ? -ddx : ddx;
        int ady0 = ddy < 0 ? -ddy : ddy;
        int maxlen0 = adx0 > ady0 ? adx0 : ady0;
        if (maxlen0 > 0) {
            int64_t cx_fp = (int64_t)tx << 16;
            int64_t cy_fp = (int64_t)ty << 16;
            int64_t inc_x = ((int64_t)ddx << 16) / maxlen0;
            int64_t inc_y = ((int64_t)ddy << 16) / maxlen0;
            for (int s = 0; s < maxlen0; ++s) {
                int nx = (int)(cx_fp >> 16);
                int ny = (int)(cy_fp >> 16);
                if (is_walkable_at(nx, ny)) { tx = nx; ty = ny; break; }
                cx_fp += inc_x; cy_fp += inc_y;
            }
        }
    }
    /* Phase 2 — forward-step clip. */
    int clip_x = sx, clip_y = sy;
    int reached = line_clip(sx, sy, tx, ty, &clip_x, &clip_y);

    if (!reached && g_persp_band_count > 0) {
        /* Phase 3 — BFS Dijkstra to find chain of bands. */
        int path_len = wp_find_path(wp, (int16_t)sx, (int16_t)sy,
                                     (int16_t)tx, (int16_t)ty);
        if (path_len > 0) {
            uint8_t band = wp->selected_path[wp->path_index];
            int n = g_persp_band_count;
            if (band < n) {
                wp->final_x = (int16_t)tx;
                wp->final_y = (int16_t)ty;
                wp->path_active = 1;
                s_wp_path_len[actor_idx] = path_len;
                target_x = wp->band_x[band];
                target_y = wp->band_y[band];
                return BindActorWalkerDirect(actor_idx, target_x, target_y);
            }
        }
    }
    /* PORT SHORTCUT (refer FUN_00412b60 case 8 obstacle handling): some
     * scenes have walk-fld bitmaps with walkable pixels UNDER visible
     * buildings/obstacles. Original blocks these via additional walkability
     * list entries (kind=3 walk-behind assets registered with flag 0x8008
     * → case 8 in FUN_00412b60 returns NOT walkable + stops list search).
     * Our port only consults the single bg-mask-setup FLD bitmap, missing
     * these obstacle overlays — BFS finds edges through them, walker takes
     * "shortcut through building" visually. Full fix requires porting the
     * linked-list walkability head DAT_0044E6B0 + per-entity flag setup
     * for kind=3 walk-behind sprites. Rare edge case; documented for
     * future work. */
    return BindActorWalkerDirect(actor_idx, clip_x, clip_y);
}

int BindActorWalkerDirect(int actor_idx, int target_x, int target_y)
{
    extern Entity *g_actor[2];
    extern const char *g_actor_walk_anim[2][6];
    if (actor_idx < 0 || actor_idx > 1) return 0;
    Entity *a = g_actor[actor_idx];
    if (!a) return 0;

    /* Direction selection — 1:1 with select_walk_anim algorithm:
     *   dir 0 = L (target_x < anchor_x AND |dx| > |dy|)
     *   dir 1 = R (target_x > anchor_x AND |dx| > |dy|)
     *   dir 2 = U (target_y < anchor_y AND |dy| >= |dx|)
     *   dir 3 = D (target_y > anchor_y AND |dy| >= |dx|)
     * Set the mirror flag (entity[+8] bit 0x4) when picking R direction —
     * atlases store left-facing native, mirror for right. The per-entity VM
     * walker doesn't write this flag itself (walker bytecode handles only
     * frame cycling + position), so we set it once at bind time. */
    int16_t anchor_x = EOFF(a, 0x22, int16_t);
    int16_t anchor_y = EOFF(a, 0x24, int16_t);
    int dx = target_x - anchor_x;
    int dy = target_y - anchor_y;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int dir;
    if (ady < adx) dir = (dx > 0) ? 1 : 0;   /* horizontal dominant */
    else           dir = (dy > 0) ? 3 : 2;   /* vertical dominant */

    const char *anim_path = g_actor_walk_anim[actor_idx][dir];
    if (!anim_path) return 0;

    /* T-render-bug: bit 0x04 of entity[+8] (= flags1 bit 2) is NOT a mirror
     * flag — Ghidra FUN_004012E0 post-exec uses it as "×2 doubled" for both
     * sprite scaling AND hot-spot offset doubling:
     *
     *   } else if (flags & 4) {     // ×2 doubled
     *       EOFF(e, 0x0A, int16_t) += hx * 2;
     *       EOFF(e, 0x0C, int16_t) += hy * 2;
     *   }
     *
     * AND BlitSpriteScaledColorKeyFlip's flip_h path is also conditioned on
     * this bit (= mirror render) — so an earlier port-shortcut assumed this
     * bit doubled as "render-mirror flag" for R-direction walking. Wrong!
     *
     * Setting it for R-walk made the renderer apply ×2 hot-spot offset
     * (drawn_y = anchor + hy*2 = anchor - 216 for hy=-108) which planted the
     * actor sprite ~110 px above the floor — the visible "Fjej w kosmos /
     * Edek lewituje" bug. The actual Wacki engine uses SEPARATE atlas frames
     * for L vs R walk (no mirror needed); frame range 0..23 for L, 24..47
     * for R (per FRAME_RANGE_CHECK in walker bytecodes).
     *
     * Remove the flag toggling here. R-walk mirror was always wrong: the
     * walker bytecode already cycles through the correct R-walk frames
     * (which face right natively).
     *
     * NOTE: The renderer's flag-4 → flip_h path in EntityRenderAll is also
     * suspect (= probably wrong for atlas data that natively contains both
     * L and R frames). Leaving renderer alone for now; this fix alone
     * resolves the "kosmos" bug since flag is never set anymore. */
    (void)dir;

    PlayActorAnimByPath(a, anim_path, (int16_t)target_x, (int16_t)target_y);
    return 1;
}
