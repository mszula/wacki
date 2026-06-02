/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/hud/items.c — inventory item-name voice-overs.
 *
 * Hovering an inventory item in the bottom panel for ~2 seconds fires
 * a short female-voice WAV that names the item ("magnes", "drut", …).
 *
 * 1. LoadItemNamesTable parses Item.scr at boot, building a table
 * indexed by `item_verb - ITEM_VERB_FIRST`. Each entry stores
 * the WAV filename to play.
 *
 * 2. ItemHoverDwellTick runs once per frame (after PanelHitTest) and
 * implements the dwell state machine:
 *
 * - hover_panel_verb out of item range → reset, arm 2 s timer
 * - in range, new index, dwell expired → play SFX, arm timer
 *
 * The 2-second dwell prevents the voice from re-firing on every frame
 * while the cursor sits over the same item.
 */

#include "wacki.h"
#include "wacki/log.h"

#include <stdint.h>
#include <stdio.h>

extern void     PlaySfx(const char *name);
extern uint16_t g_hover_panel_verb;
extern uint32_t g_tick_counter;

#define ITEM_NAME_MAX        256
#define ITEM_NAME_FILENAME    16        /* matches original 16-byte stride */

#define ITEM_VERB_FIRST      0x2A       /* note: differs from Inventory's 0x29 */
#define ITEM_VERB_LAST       0xB6
#define ITEM_DWELL_MS        2000       /* hover time before voice fires */

static char g_item_wav[ITEM_NAME_MAX][ITEM_NAME_FILENAME] = {{0}};
static int  g_item_wav_count                              = 0;

/* Single-pass parser for Item.scr — for each `[N]filename\n` block,
 * read decimal N (1-based), skip past `]`, then copy non-whitespace
 * filename bytes (max ITEM_NAME_FILENAME - 1) into the table.
 *
 * Returns the highest N seen — also the count of registered slots
 * (gaps in numbering are tolerated; missing slots stay zero-filled). */
int LoadItemNamesTable(void)
{
    void    *raw = NULL;
    uint32_t sz  = 0;
    if (!LoadFileFromDta("Item.scr", &raw, &sz) || !raw || sz == 0) {
        LOG_TRACE("item-name", "Item.scr missing — voice-over disabled");
        return 0;
    }

    const uint8_t *p   = (const uint8_t *)raw;
    const uint8_t *end = p + sz;

    while (p < end) {
        while (p < end && *p != '[') ++p;
        if (p >= end) break;
        ++p;                                /* skip '[' */

        int n = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            n = n * 10 + (*p - '0');
            ++p;
        }
        if (n <= 0 || n > ITEM_NAME_MAX) {
            while (p < end && *p != '[') ++p;       /* skip to next block */
            continue;
        }

        while (p < end && *p != ']') ++p;
        if (p < end) ++p;
        while (p < end && *p <= ' ') ++p;           /* skip whitespace */

        int slot   = n - 1;
        int copied = 0;
        while (p < end && *p > ' ' && copied < ITEM_NAME_FILENAME - 1) {
            g_item_wav[slot][copied++] = (char)*p;
            ++p;
        }
        g_item_wav[slot][copied] = 0;
        if (n > g_item_wav_count) g_item_wav_count = n;
    }

    xfree(raw);
    LOG_TRACE("item-name", "loaded %d entries from Item.scr", g_item_wav_count);
    return g_item_wav_count;
}

void ItemHoverDwellTick(void)
{
    static uint32_t s_dwell_until = 0;
    static int      s_last_item   = -1;

    const uint16_t verb = g_hover_panel_verb;

    if (verb < ITEM_VERB_FIRST || verb > ITEM_VERB_LAST) {
        s_last_item   = -1;
        s_dwell_until = g_tick_counter + ITEM_DWELL_MS;
        return;
    }

    int idx = (int)(verb - ITEM_VERB_FIRST);
    if (idx >= g_item_wav_count)        return;     /* no name registered */
    if (idx == s_last_item)             return;     /* already announced */
    if (g_tick_counter < s_dwell_until) return;     /* still dwelling */

    const char *wav = g_item_wav[idx];
    if (wav[0]) {
        PlaySfx(wav);
        LOG_TRACE("item-name", "hover verb=0x%02X → %s", verb, wav);
    }
    s_dwell_until = g_tick_counter + ITEM_DWELL_MS;
    s_last_item   = idx;
}
