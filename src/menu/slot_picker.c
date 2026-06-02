/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/menu/slot_picker.c — Sejw.pic + Load.pic save/load slot UI.
 *
 * Two SceneDefs share the same button layout (12 buttons, ids 0x12..
 * 0x1d):
 *   - 0x12       Anuluj (cancel)               → CLICK_RET_USER_CANCELLED
 *   - 0x13       Zapisz / Wczytaj (commit)     → CLICK_RET_LOAD_COMPLETED + action
 *   - 0x14..0x1d 10 slot rows                  → select slot / start edit
 *
 * Sejw.pic (SAVE picker) → write current state to slot
 * Load.pic (LOAD picker) → read slot into current state
 *
 * The renderer integration relies on RunMenuScene loading the slot
 * mask atlas (Load.wyc) into g_menu_asset_10. Hover frames 2..11
 * correspond to slot rows 0..9 — drawX/drawY of each frame gives the
 * row position. We paint slot names every frame inside the on_click
 * cb's idle branch (trigger == -1), so they refresh as the user
 * selects different slots. SceneDef buttons have def_anim = FRAME_NONE
 * (no default sprite), so the text isn't overdrawn except by the
 * actively-hovered hover sprite (acceptable — the highlight indicates
 * focus).
 *
 * SAVE menu supports inline name editing: typed chars accumulate into
 * s_edit_buf, Backspace pops the last char, Enter commits the name +
 * triggers save. */

#include "wacki.h"
#include "wacki/log.h"
#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- constants ---------------------------------------------------- *
 *
 * Button trigger codes (verb ids attached to each SceneDef button). */
#define SLOT_BTN_CANCEL              0x12
#define SLOT_BTN_COMMIT              0x13
#define SLOT_BTN_FIRST_SLOT_TRIGGER  0x14   /* slot 0 → 0x14, slot 9 → 0x1d */

/* SceneDef button-frame index for slot N (Load.wyc atlas frames 2..11). */
#define SLOT_BUTTON_FRAME_OFFSET     2
#define SLOT_FRAME_NONE              0xFFFF

/* Inline-edit keyboard handling. */
#define KEY_ENTER                    0x0D
#define KEY_BACKSPACE                0x08
#define KEY_PRINTABLE_FIRST          0x20   /* ' ' */
#define KEY_PRINTABLE_LAST           0x7F   /* exclusive — DEL excluded */
#define EDIT_NAME_MAX_CHARS          20

/* Slot-row text layout. */
#define SLOT_ROW_INSET_PX            4      /* fill+text inset from row border */
#define SLOT_ROW_TEXT_X_INSET_PX     4      /* extra text X offset past the fill rect */
#define SLOT_ROW_BG_FILL_COLOR       0x01   /* indexed-palette grey/brown */
#define SLOT_ROW_TEXT_COLOR          0x12   /* normal slot-name colour */
#define SLOT_ROW_EDIT_TEXT_COLOR     0xFD   /* highlight palette slot used while editing */

/* Cursor blink rate: ~2 Hz (toggle every 500 ms). */
#define EDIT_CURSOR_BLINK_MS         500

/* RGB for the yellow we force into palette slot 0xFD while editing —
 * the sejw.pic palette doesn't normally have a bright colour there,
 * which would make edit text near-black + unreadable. */
#define EDIT_CURSOR_PALETTE_R        0xFF
#define EDIT_CURSOR_PALETTE_G        0xE0
#define EDIT_CURSOR_PALETTE_B        0x00

/* Click-handler return codes. */
#define CLICK_RET_STAY               0      /* stay on this menu */
#define CLICK_RET_LOAD_COMPLETED     3      /* close menu + resume gameplay */
#define CLICK_RET_USER_CANCELLED     4      /* close menu w/o action */

/* Thumbnail preview pane is at frame 1's draw rect, inset 3 px from
 * the top-left corner. */
#define SLOT_THUMB_FRAME_INDEX       1
#define SLOT_THUMB_INSET_PX          3

/* ---- module state + externs --------------------------------------- */

static int s_slot_selected = -1;        /* 0..9 = chosen slot; -1 = none */
static int s_edit_slot     = -1;
static char s_edit_buf[30];             /* matches WackiSlot.name capacity */
static int  s_edit_len     = 0;

extern AnimAsset  *g_menu_asset_10;
extern FontHandle *g_default_font;
extern uint8_t    *g_back_shadow;
extern uint8_t     g_palette_rgb[256 * 3];
extern uint8_t     g_save_thumb_pending[SAVE_THUMB_W * SAVE_THUMB_H];
extern void LoadKomnataScene(uint16_t id);

/* ---- helpers ------------------------------------------------------ */

/* Hover-frame index per slot row in Load.wyc — slot N → frame N+2. */
static uint16_t slot_hover_frame(int slot)
{
    return (uint16_t)(slot + SLOT_BUTTON_FRAME_OFFSET);
}

/* Compose a display string for a slot. Empty (never-saved) slots
 * already carry WACKI_DEFAULT_SLOT_NAME ("Pusty") from
 * LoadSaveStateOrInitialize, so they render naturally. No row prefix —
 * the original lays slots out vertically and the user identifies them
 * by position. */
static void slot_display_name(int slot, char *out, size_t out_sz)
{
    const WackiSlot *s = &g_save.slots[slot];
    if (s->stage_indicator == 0 && !s->name[0]) {
        snprintf(out, out_sz, "%s", WACKI_DEFAULT_SLOT_NAME);
    } else if (s->name[0]) {
        snprintf(out, out_sz, "%s", s->name);
    } else {
        snprintf(out, out_sz, "etap %u k%u",
                 (unsigned)s->etap_id, (unsigned)s->stage_indicator);
    }
}

/* Blit the thumbnail preview for the currently-selected slot (or slot
 * 0 if nothing is selected) into the frame-1 box of the slot-list
 * atlas. Source: the slot's saved world_default_snapshot — captured
 * scene for filled slots, TV-test pattern for empty slots. */
static void paint_slot_thumbnail(AnimAsset *atlas)
{
    if (atlas->frame_count <= SLOT_THUMB_FRAME_INDEX) return;

    int16_t tx = (int16_t)(atlas->off_drawX[SLOT_THUMB_FRAME_INDEX]
                           + SLOT_THUMB_INSET_PX);
    int16_t ty = (int16_t)(atlas->off_drawY[SLOT_THUMB_FRAME_INDEX]
                           + SLOT_THUMB_INSET_PX);
    int show_slot = (s_slot_selected >= 0 && s_slot_selected < WACKI_SAVE_SLOTS)
                  ? s_slot_selected : 0;
    const uint8_t *thumb_src = g_save.slots[show_slot].world_default_snapshot;

    if (tx >= 0 && ty >= 0 &&
        tx + SAVE_THUMB_W <= WACKI_SCREEN_W &&
        ty + SAVE_THUMB_H <= WACKI_SCREEN_H)
    {
        PaintImageToBackbuffer((uint16_t)tx, (uint16_t)ty,
                               SAVE_THUMB_W, SAVE_THUMB_H, thumb_src);
    }
}

/* Compute the inner fill rectangle (background colour) for slot row
 * `slot`. Returns 0 if the row geometry is out of bounds. */
static int slot_row_fill_rect(AnimAsset *atlas, int slot,
                              int *rx, int *ry, int *rw, int *rh)
{
    uint16_t f = slot_hover_frame(slot);
    if (f >= atlas->frame_count) return 0;

    int x = (int)atlas->off_drawX[f] + SLOT_ROW_INSET_PX;
    int y = (int)atlas->off_drawY[f] + SLOT_ROW_INSET_PX;
    int w = (int)atlas->off_widths [f] - 2 * SLOT_ROW_INSET_PX;
    int h = (int)atlas->off_heights[f] - 2 * SLOT_ROW_INSET_PX;

    if (x < 0 || y < 0 || x >= WACKI_SCREEN_W || y >= WACKI_SCREEN_H) return 0;
    if (w <= 0 || h <= 0) return 0;
    if (x + w > WACKI_SCREEN_W) w = WACKI_SCREEN_W - x;
    if (y + h > WACKI_SCREEN_H) h = WACKI_SCREEN_H - y;

    *rx = x;  *ry = y;  *rw = w;  *rh = h;
    return 1;
}

/* Fill the slot row's inner rectangle with the configured background
 * colour (palette slot 0x01 = visible grey/brown that contrasts with
 * the row border). */
static void fill_slot_row_bg(int rx, int ry, int rw, int rh)
{
    for (int yy = 0; yy < rh; ++yy) {
        uint8_t *row = g_back_shadow + (size_t)(ry + yy) * WACKI_SCREEN_W + rx;
        memset(row, SLOT_ROW_BG_FILL_COLOR, (size_t)rw);
    }
}

/* Force a known-bright yellow into the high palette slot the editing
 * text uses. Non-persistent — the next scene's InstallPalette refresh
 * overwrites it. */
static void install_edit_cursor_palette(void)
{
    g_palette_rgb[SLOT_ROW_EDIT_TEXT_COLOR * 3 + 0] = EDIT_CURSOR_PALETTE_R;
    g_palette_rgb[SLOT_ROW_EDIT_TEXT_COLOR * 3 + 1] = EDIT_CURSOR_PALETTE_G;
    g_palette_rgb[SLOT_ROW_EDIT_TEXT_COLOR * 3 + 2] = EDIT_CURSOR_PALETTE_B;
}

/* Compose the line of text for slot `slot` — either the display name
 * or, if the slot is in inline-edit mode, the editing buffer plus a
 * blinking cursor. Returns the colour-base index for the text render. */
static uint8_t compose_slot_row_text(int slot, char *out, size_t out_sz)
{
    if (slot != s_edit_slot) {
        slot_display_name(slot, out, out_sz);
        return SLOT_ROW_TEXT_COLOR;
    }

    install_edit_cursor_palette();
    int cursor_on = ((SDL_GetTicks() / EDIT_CURSOR_BLINK_MS) & 1u) == 0;
    snprintf(out, out_sz, "%.*s%c",
             s_edit_len, s_edit_buf,
             cursor_on ? '_' : ' ');
    return SLOT_ROW_EDIT_TEXT_COLOR;
}

/* Paint slot row `slot`: fill the inner rectangle then render the
 * display name (or editing buffer) into it. */
static void paint_slot_row(AnimAsset *atlas, int slot)
{
    int rx, ry, rw, rh;
    if (!slot_row_fill_rect(atlas, slot, &rx, &ry, &rw, &rh)) return;

    fill_slot_row_bg(rx, ry, rw, rh);

    char    line[40];
    uint8_t color_base = compose_slot_row_text(slot, line, sizeof line);

    TextRenderTarget t = {
        .stride     = WACKI_SCREEN_W,
        .x          = (uint16_t)(rx + SLOT_ROW_TEXT_X_INSET_PX),
        .color_base = color_base,
        .pixels     = g_back_shadow + (size_t)ry * WACKI_SCREEN_W,
        .font       = g_default_font,
    };
    RenderTextLineToBuffer(&t, (const uint8_t *)line);
}

/* paint_slot_list — every-frame after_paint hook for both SceneDefs.
 * Paints the thumbnail then each slot row's text. */
static void paint_slot_list(void)
{
    AnimAsset *atlas = g_menu_asset_10;
    if (!atlas || !g_default_font || !g_back_shadow) return;
    if (!atlas->off_drawX || !atlas->off_drawY) return;

    paint_slot_thumbnail(atlas);
    for (int i = 0; i < WACKI_SAVE_SLOTS; ++i) {
        paint_slot_row(atlas, i);
    }
}

/* ---- inline-edit (Save menu only) -------------------------------- */

/* Commit the in-flight edit buffer into the slot's name field. */
static void end_edit_commit(void)
{
    if (s_edit_slot < 0) return;
    WackiSlot *s = &g_save.slots[s_edit_slot];
    memset(s->name, 0, sizeof s->name);
    if (s_edit_len > 0) {
        size_t n = (size_t)s_edit_len;
        if (n > sizeof s->name - 1) n = sizeof s->name - 1;
        memcpy(s->name, s_edit_buf, n);
        s->name[n] = 0;
    }
    PlatformSetTextInput(0);
    s_edit_slot = -1;
    s_edit_len  = 0;
}

/* Enter edit mode on `slot` — seed buffer with current name (or empty
 * for default "Pusty" slots so typing replaces the placeholder). */
static void begin_edit(int slot)
{
    s_edit_slot = slot;
    const WackiSlot *s = &g_save.slots[slot];
    if (s->stage_indicator == 0 ||
        strcmp(s->name, WACKI_DEFAULT_SLOT_NAME) == 0)
    {
        s_edit_len    = 0;
        s_edit_buf[0] = 0;
    } else {
        size_t n = strnlen(s->name, sizeof s->name);
        if (n >= sizeof s_edit_buf) n = sizeof s_edit_buf - 1;
        memcpy(s_edit_buf, s->name, n);
        s_edit_buf[n] = 0;
        s_edit_len    = (int)n;
    }
    PlatformSetTextInput(1);
}

/* Commit a save to the selected slot. Shared between the Zapisz button
 * and the Enter-in-edit-mode path. Returns CLICK_RET_LOAD_COMPLETED on
 * success (= close menu), CLICK_RET_STAY if no slot selected, and
 * CLICK_RET_USER_CANCELLED if not in gameplay. */
static int save_commit_selected(void)
{
    end_edit_commit();
    if (s_slot_selected < 0) {
        LOG_TRACE("save-menu", "commit ignored (no slot selected)");
        return CLICK_RET_STAY;
    }
    if (g_cur_etap == 0 || g_cur_komnata == 0) {
        LOG_TRACE("save-menu", "cannot save outside gameplay");
        return CLICK_RET_USER_CANCELLED;
    }
    WackiSlot *s = &g_save.slots[s_slot_selected];
    /* Auto-name fallback when the user committed without typing. */
    if (!s->name[0] || strcmp(s->name, WACKI_DEFAULT_SLOT_NAME) == 0) {
        snprintf(s->name, sizeof s->name, "etap %u k%u",
                 (unsigned)g_cur_etap, (unsigned)g_cur_komnata);
    }
    memcpy(s->world_default_snapshot, g_save_thumb_pending,
           sizeof s->world_default_snapshot);
    QuickSaveToSlot((uint16_t)s_slot_selected);
    LOG_TRACE("save-menu", "saved slot %d (%s)", s_slot_selected, s->name);
    s_slot_selected = -1;
    return CLICK_RET_LOAD_COMPLETED;
}

/* Trigger code → slot index conversion (or -1 if not a slot trigger). */
static int slot_index_from_trigger(int trigger)
{
    int slot = trigger - SLOT_BTN_FIRST_SLOT_TRIGGER;
    if (slot < 0 || slot >= WACKI_SAVE_SLOTS) return -1;
    return slot;
}

/* Drain typed characters into the edit buffer. Returns 1 if the user
 * pressed Enter (signalling commit-now), 0 otherwise. */
static int process_typed_chars(void)
{
    uint8_t c;
    while ((c = PlatformPollTypedChar()) != 0) {
        if (c == KEY_ENTER) return 1;

        if (c == KEY_BACKSPACE) {
            if (s_edit_len > 0) s_edit_buf[--s_edit_len] = 0;
        } else if (c >= KEY_PRINTABLE_FIRST && c < KEY_PRINTABLE_LAST) {
            if (s_edit_len < EDIT_NAME_MAX_CHARS) {
                s_edit_buf[s_edit_len++] = (char)c;
                s_edit_buf[s_edit_len]   = 0;
            }
        }
    }
    return 0;
}

/* ---- click handlers ----------------------------------------------- */

static int SaveSlotClick(int trigger)
{
    int slot = slot_index_from_trigger(trigger);

    if (slot >= 0) {
        /* Single click selects + enters edit mode (slight deviation from
         * the original "click then click again" UX — user prefers
         * immediate typing). Switching to another slot mid-edit commits
         * the prior edit first. */
        if (s_edit_slot >= 0 && s_edit_slot != slot) end_edit_commit();
        s_slot_selected = slot;
        if (s_edit_slot != slot) begin_edit(slot);
        LOG_TRACE("save-menu", "slot %d editing", slot);
        return CLICK_RET_STAY;
    }

    if (trigger == SLOT_BTN_CANCEL) {
        /* Drop any in-progress edit without committing. */
        PlatformSetTextInput(0);
        s_edit_slot     = -1;
        s_edit_len      = 0;
        s_slot_selected = -1;
        return CLICK_RET_USER_CANCELLED;
    }

    if (trigger == SLOT_BTN_COMMIT) {
        return save_commit_selected();
    }

    /* Idle frame — drain typed chars (Enter commits + closes), then
     * repaint slot rows so the editing slot's cursor blinks. */
    if (s_edit_slot >= 0 && process_typed_chars()) {
        return save_commit_selected();
    }
    paint_slot_list();
    return CLICK_RET_STAY;
}

/* LoadSlotClick (Load.pic handler). Reads the selected slot via
 * LoadSaveSlot then triggers LoadKomnataScene to re-enter the room
 * the save was made in. */
static int LoadSlotClick(int trigger)
{
    int slot = slot_index_from_trigger(trigger);

    if (slot >= 0) {
        s_slot_selected = slot;
        LOG_TRACE("load-menu", "slot %d selected", slot);
        return CLICK_RET_STAY;
    }

    if (trigger == SLOT_BTN_CANCEL) {
        s_slot_selected = -1;
        return CLICK_RET_USER_CANCELLED;
    }

    if (trigger == SLOT_BTN_COMMIT) {
        if (s_slot_selected < 0) {
            LOG_TRACE("load-menu", "commit ignored (no slot selected)");
            return CLICK_RET_STAY;
        }
        if (g_save.slots[s_slot_selected].stage_indicator == 0) {
            LOG_TRACE("load-menu", "slot %d is empty", s_slot_selected);
            return CLICK_RET_STAY;
        }
        if (LoadSaveSlot((uint16_t)s_slot_selected)) {
            LoadKomnataScene(g_cur_komnata);
            LOG_TRACE("load-menu", "loaded slot %d → etap %u k%u", s_slot_selected, (unsigned)g_cur_etap, (unsigned)g_cur_komnata);
            s_slot_selected = -1;
            return CLICK_RET_LOAD_COMPLETED;
        }
        return CLICK_RET_STAY;
    }

    paint_slot_list();
    return CLICK_RET_STAY;
}

/* ---- SceneDefs ---------------------------------------------------- */

/* Sejw.pic — SAVE picker. FORCE_CB so paint_slot_list refreshes every
 * frame; after_paint runs the slot-text overlay on top of the hover
 * sprite so it isn't obscured. */
SceneDef g_save_menu_scene = {
    .background_pic = "Sejw.pic",
    .mask_file      = "Load.wyc",
    .on_click       = SaveSlotClick,
    .button_count   = 12,
    .flags          = SCENE_FLAG_FORCE_CB,
    .buttons = {
        { 0x12, SLOT_FRAME_NONE,  0 }, { 0x13, SLOT_FRAME_NONE,  1 },
        { 0x14, SLOT_FRAME_NONE,  2 }, { 0x15, SLOT_FRAME_NONE,  3 },
        { 0x16, SLOT_FRAME_NONE,  4 }, { 0x17, SLOT_FRAME_NONE,  5 },
        { 0x18, SLOT_FRAME_NONE,  6 }, { 0x19, SLOT_FRAME_NONE,  7 },
        { 0x1a, SLOT_FRAME_NONE,  8 }, { 0x1b, SLOT_FRAME_NONE,  9 },
        { 0x1c, SLOT_FRAME_NONE, 10 }, { 0x1d, SLOT_FRAME_NONE, 11 },
    },
    .after_paint = paint_slot_list,
};

/* Load.pic — LOAD picker. Same SceneDef layout as Sejw.pic. */
SceneDef g_load_menu_scene = {
    .background_pic = "Load.pic",
    .mask_file      = "Load.wyc",
    .on_click       = LoadSlotClick,
    .button_count   = 12,
    .flags          = SCENE_FLAG_FORCE_CB,
    .buttons = {
        { 0x12, SLOT_FRAME_NONE,  0 }, { 0x13, SLOT_FRAME_NONE,  1 },
        { 0x14, SLOT_FRAME_NONE,  2 }, { 0x15, SLOT_FRAME_NONE,  3 },
        { 0x16, SLOT_FRAME_NONE,  4 }, { 0x17, SLOT_FRAME_NONE,  5 },
        { 0x18, SLOT_FRAME_NONE,  6 }, { 0x19, SLOT_FRAME_NONE,  7 },
        { 0x1a, SLOT_FRAME_NONE,  8 }, { 0x1b, SLOT_FRAME_NONE,  9 },
        { 0x1c, SLOT_FRAME_NONE, 10 }, { 0x1d, SLOT_FRAME_NONE, 11 },
    },
    .after_paint = paint_slot_list,
};
