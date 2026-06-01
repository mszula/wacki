/* src/menu/menu_loop.c — RunMenuScene + the menu's per-frame helpers.
 *
 * RunMenuScene is the generic loop that drives every SceneDef-based
 * screen — title, opszyns, sub-menus (Solund / Grafika / Pytanie /
 * Sejw / Load), chapter-select, etc. Each frame:
 *
 *   1. Pump events.
 *   2. Hit-test the cursor against the mask atlas's button rects.
 *   3. Paint the background (full-screen .pic or restored gameplay
 *      snapshot for sub-fullscreen overlays).
 *   4. Fire the SceneDef's on_click handler (with trigger=button id
 *      on LMB, or -1 for idle pass).
 *   5. Paint default-state button sprites then the hover overlay.
 *   6. Optional after_paint hook (slot-picker uses this for slot text).
 *   7. Paint the cursor sprite + flush.
 *   8. Tick menu music (BGM continuity).
 *   9. Poll ESC / platform-quit.
 *  10. Sleep ~16 ms for 60 fps pacing.
 *
 * Returns when the on_click handler signals close (non-zero rc), ESC
 * pressed (DISABLE_ESC opt-out), or the platform requested quit.
 *
 * The mask atlas pointer is also published to the global g_menu_asset_10
 * so the on_click handler can fetch it for its title-animation block
 * (the original engine wires it via the "asset (kind=1, id=10)" hook).
 *
 * SnapshotBackbufferForMenu lives here too — paint_menu_background
 * restores from that snapshot under sub-fullscreen overlay scenes. */

#include "wacki.h"
#include "wacki/log.h"
#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- constants ---------------------------------------------------- */

/* ~60 fps pacing — SDL_Delay 16 ms leaves headroom for the per-tick cb
 * + paint pass. */
#define MENU_FRAME_DELAY_MS         16

/* RunMenuScene return codes shared with the rest of the engine. ESC
 * inside a menu returns MENU_ESC_RC (= MAIN_MENU_RC_QUIT_CONFIRM_A);
 * the outer caller treats it as "user wants to back out". Hard-quit
 * (platform shutdown / Cmd-Q) returns 99. */
#define MENU_RC_NONE                0
#define MENU_ESC_RC                 4
#define MENU_RC_HARD_QUIT           99

/* .pic RAWB header layout — width and height live at offsets 4..7 as
 * little-endian 16-bit values. */
#define RAWB_HEADER_OFF_WIDTH       4
#define RAWB_HEADER_OFF_HEIGHT      6

/* A .pic shorter than this y-threshold AND not full-width is treated
 * as a sub-fullscreen overlay (Pytanie quit-confirm, Solund options,
 * etc.) — see paint_menu_background. */
#define MENU_OVERLAY_HEIGHT_THRESH  400

/* The engine's "neutral / no verb" sentinel used to clear the hover-
 * verb globals while a menu is up. */
#define SCENE_NEUTRAL_VERB          0x26

/* Win32 keyboard constant the engine uses for HasPendingKey / WaitForKey. */
#define VK_ESCAPE                   0x1B

/* "INIT" trigger value passed to on_click once at scene entry so the
 * cb can do its one-time setup (palette install, BGM start). */
#define MENU_INIT_TRIGGER           0

/* ---- module state ------------------------------------------------- */

/* Full backbuffer snapshot captured before sub-fullscreen overlay menus
 * paint. paint_menu_background restores from this so cursor trails /
 * closed-submenu remnants don't accumulate in the margins. Marked
 * invalid for menus reached BEFORE gameplay starts (e.g. main-menu
 * Load). */
static uint8_t s_menu_bg_snapshot[WACKI_SCREEN_W * WACKI_SCREEN_H];
static int     s_menu_bg_snapshot_valid = 0;

/* ---- externs ------------------------------------------------------ */

extern int16_t  s_mouse_x;
extern int16_t  s_mouse_y;
extern uint8_t *g_back_shadow;
extern int      paint_rawb_pic(const void *blob, uint32_t size, int as_overlay);
extern void     paint_anim_button_at(AnimAsset *atlas, uint16_t frame,
                                     int16_t dx, int16_t dy, int opaque);
extern AnimAsset *g_menu_asset_10;

/* ---- helpers ------------------------------------------------------ */

/* Hit-test: which scene button (if any) does (mx, my) fall on? Each
 * button has two atlas frames — def_anim (always drawn) and hover_anim
 * (drawn on hover). A def_anim of 0xFFFF means "no rest sprite" (used
 * by Pytanie where the dialog only shows TAK/NIE highlighted on hover);
 * in that case fall back to hover_anim's rect so the cursor still hits.
 *
 * Some buttons store a 1×1 placeholder in def_anim (e.g. Grafika.wyc
 * exit) and the real clickable rect lives only in the hover frame —
 * test both and take the union of the two bounding boxes. */
static int hit_test_buttons(SceneDef *scene, AnimAsset *atlas,
                            int mx, int my)
{
    if (!atlas || !atlas->pixel_ptrs) return -1;
    for (int i = 0; i < scene->button_count; ++i) {
        uint16_t a_def = scene->buttons[i].def_anim;
        uint16_t a_hov = scene->buttons[i].hover_anim;
        for (int pass = 0; pass < 2; ++pass) {
            uint16_t a = (pass == 0) ? a_def : a_hov;
            if (a >= atlas->frame_count) continue;
            int w = atlas->off_widths[a], h = atlas->off_heights[a];
            if (w < 2 || h < 2) continue;       /* skip 1×1 placeholders */
            int x = atlas->off_drawX[a], y = atlas->off_drawY[a];
            if (mx >= x && mx < x + w && my >= y && my < y + h)
                return i;
        }
    }
    return -1;
}

/* Convenience wrapper around paint_anim_button_at that honours the
 * atlas frame's drawX/drawY hot-spot (dx/dy = 0). */
static void paint_anim_button(AnimAsset *atlas, uint16_t frame)
{
    paint_anim_button_at(atlas, frame, 0, 0, 0);
}

/* Load the SceneDef's BG .pic + button mask atlas. Returns 1 if bg
 * was loaded, 0 otherwise; writes the atlas pointer to *out_buttons
 * AND publishes it to the global g_menu_asset_10 for the on_click cb. */
static int load_menu_scene_assets(SceneDef *scene,
                                  void **out_bg_raw, uint32_t *out_bg_size,
                                  AnimAsset **out_buttons)
{
    int bg_loaded = 0;
    *out_bg_raw   = NULL;
    *out_bg_size  = 0;
    *out_buttons  = NULL;

    if (scene->background_pic) {
        bg_loaded = LoadFileFromDta(scene->background_pic,
                                    out_bg_raw, out_bg_size);
    }
    if (scene->mask_file) {
        *out_buttons = LoadAssetFromDtaBase(scene->mask_file);
    }
    g_menu_asset_10 = *out_buttons;
    return bg_loaded;
}

/* Decide whether a .pic is a sub-fullscreen overlay (Pytanie-sized)
 * vs a full-screen scene background. */
static int rawb_is_overlay(const void *bg_raw)
{
    const uint8_t *bp = (const uint8_t *)bg_raw;
    int bg_w = bp[RAWB_HEADER_OFF_WIDTH]
             | (bp[RAWB_HEADER_OFF_WIDTH + 1] << 8);
    int bg_h = bp[RAWB_HEADER_OFF_HEIGHT]
             | (bp[RAWB_HEADER_OFF_HEIGHT + 1] << 8);
    return bg_w < WACKI_SCREEN_W || bg_h < MENU_OVERLAY_HEIGHT_THRESH;
}

/* Paint the menu's BG layer. Full-screen scenes draw the .pic directly;
 * overlay scenes restore the captured gameplay snapshot (or clear to
 * colour 0 if no snapshot exists) so cursor trails / closed-submenu
 * remnants don't accumulate in the margins. */
static void paint_menu_background(const void *bg_raw, uint32_t bg_size)
{
    int overlay = rawb_is_overlay(bg_raw);
    if (overlay) {
        if (s_menu_bg_snapshot_valid && g_back_shadow) {
            memcpy(g_back_shadow, s_menu_bg_snapshot,
                   (size_t)WACKI_SCREEN_W * WACKI_SCREEN_H);
        } else {
            FlipBuffersClearWith(0);
        }
    }
    paint_rawb_pic(bg_raw, bg_size, overlay);
}

/* Walk the SceneDef's button table for whichever sprite the mouse is
 * currently over. Returns (button_idx, button_id) via out params;
 * idx = -1 when nothing is hovered (and id falls back to NEUTRAL). */
static void resolve_menu_hover(SceneDef *scene, AnimAsset *buttons,
                               int *out_idx, uint16_t *out_id)
{
    int idx = (buttons && (s_mouse_x | s_mouse_y))
            ? hit_test_buttons(scene, buttons, s_mouse_x, s_mouse_y)
            : -1;
    *out_idx = idx;
    *out_id  = (idx >= 0) ? scene->buttons[idx].id : SCENE_NEUTRAL_VERB;
}

/* Fire the SceneDef's on_click handler with the hovered button's id
 * (or -1 when nothing is hovered — the cb's per-frame logic still runs
 * that way, just no switch-case matches). Returns the cb's rc. */
static int run_menu_click_callback(SceneDef *scene,
                                   int hover_btn, uint16_t hover_id)
{
    int trigger = -1;
    if (g_lmb_clicked && hover_btn >= 0) {
        trigger = (int)hover_id;
    }
    g_lmb_clicked = 0;
    return scene->on_click(trigger);
}

/* Paint the SceneDef's button layer: every button's def sprite, then
 * the hovered button's hover sprite on top. */
static void paint_menu_buttons(SceneDef *scene, AnimAsset *buttons,
                               int hover_btn)
{
    if (!buttons) return;
    for (int i = 0; i < scene->button_count; ++i) {
        paint_anim_button(buttons, scene->buttons[i].def_anim);
    }
    if (hover_btn >= 0) {
        paint_anim_button(buttons, scene->buttons[hover_btn].hover_anim);
    }
}

/* Paint the cursor on top of the menu. Menus have no scene-hover-verb
 * and no held item, so the cursor state machine settles on state 0
 * (default arrow); the call still drives frame advance so the cursor
 * is animated the moment we re-enter gameplay. */
static void paint_menu_cursor(void)
{
    g_hover_scene_verb = SCENE_NEUTRAL_VERB;
    g_hover_panel_verb = SCENE_NEUTRAL_VERB;
    UpdateCursorState();
    PaintCursor();
}

/* Poll ESC / platform-quit. ESC sets the menu-back rc unless the scene
 * opted out via SCENE_FLAG_DISABLE_ESC. */
static int poll_menu_keyboard_quit(SceneDef *scene)
{
    if (HasPendingKey()) {
        uint16_t k = WaitForKey();
        if (k == VK_ESCAPE && !(scene->flags & SCENE_FLAG_DISABLE_ESC)) {
            return MENU_ESC_RC;
        }
    }
    if (PlatformShouldQuit()) return MENU_RC_HARD_QUIT;
    return MENU_RC_NONE;
}

/* ---- public API --------------------------------------------------- */

/* Snapshot whatever is currently in the backbuffer so the next overlay
 * RunMenuScene can paint its .pic on top of it (margins stay coherent).
 * Called before opening sub-fullscreen overlay menus (OpenOptionsMenu
 * in options.c, the Load sub-menu from HandleMainMenuClick). No-op
 * until the backbuffer is allocated. */
void SnapshotBackbufferForMenu(void)
{
    if (!g_back_shadow) return;
    memcpy(s_menu_bg_snapshot, g_back_shadow,
           (size_t)WACKI_SCREEN_W * WACKI_SCREEN_H);
    s_menu_bg_snapshot_valid = 1;
}

int RunMenuScene(int transition_mode, SceneDef *scene)
{
    (void)transition_mode;

    void      *bg_raw  = NULL;
    uint32_t   bg_size = 0;
    AnimAsset *buttons = NULL;
    int bg_loaded = load_menu_scene_assets(scene, &bg_raw, &bg_size, &buttons);

    if (!bg_loaded) FlipBuffersClearWith(0);

    LOG_TRACE("menu", "entered: bg='%s' mask='%s' atlas-frames=%d btns=%d", scene->background_pic ? scene->background_pic : "(none)", scene->mask_file      ? scene->mask_file      : "(none)", buttons ? buttons->frame_count : 0, scene->button_count);

    /* One-time INIT call so HandleMainMenuClick can do its first-frame
     * setup (palette install, BGM start). */
    if (scene->on_click) scene->on_click(MENU_INIT_TRIGGER);

    int rc = MENU_RC_NONE;
    do {
        PumpEvents();

        int      hover_btn;
        uint16_t hover_id;
        resolve_menu_hover(scene, buttons, &hover_btn, &hover_id);

        if (bg_loaded) paint_menu_background(bg_raw, bg_size);

        if (scene->on_click) {
            int r = run_menu_click_callback(scene, hover_btn, hover_id);
            if (r > 0) rc = r;
        }

        paint_menu_buttons(scene, buttons, hover_btn);
        if (scene->after_paint) scene->after_paint();
        paint_menu_cursor();
        FlushFrameToPrimary();
        TickMenuMusic();

        int quit_rc = poll_menu_keyboard_quit(scene);
        if (quit_rc != MENU_RC_NONE) rc = quit_rc;

        EnginePaceFrame(MENU_FRAME_DELAY_MS);
    } while (rc == MENU_RC_NONE);

    if (buttons) FreeAsset(buttons);
    if (bg_raw)  xfree(bg_raw);
    g_menu_asset_10 = NULL;
    return rc;
}
