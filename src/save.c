/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/save.c — Wacki.sav read / write / slot restore.
 *
 * The on-disk file is a verbatim dump of the in-memory `g_save`
 * (0x1E0C0 bytes). LoadSaveStateOrInitialize either re-reads it at
 * boot (and accepts it iff WACKI_SAVE_MAGIC matches) or initialises
 * a fresh slot table with default settings. LoadSaveSlot / QuickLoad
 * pour a slot's contents back into the live globals and call
 * LoadStage to rebuild the world. WriteSaveFile + QuickSaveToSlot
 * are the inverse — both go through a tmp+rename so a crash between
 * open and write doesn't leave a zero-byte Wacki.sav. */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/storage.h"   /* plat_save_read / plat_save_write */
#include <string.h>

WackiSaveFile g_save;       /* in-memory image, paged to storage by WriteSaveFile */

/* World-state default template — copied into every fresh slot at
 * first launch so loading slot N right after install doesn't see
 * uninitialised script state. */
extern const uint8_t g_default_world_state[0x2664];

extern uint32_t  g_script_vars[];
extern uint16_t  g_active_actor;
extern uint16_t  g_cur_etap;
extern uint16_t  g_cur_komnata;
extern uint32_t  g_entity_state[0x11C];
extern uint32_t  g_scene_snapshot[0x1E];
extern int       LoadStage(uint16_t stage);

/* ---- LoadSaveStateOrInitialize ----------------------------------- */
void LoadSaveStateOrInitialize(void)
{
    int loaded = 0;

    /* The save image is read through the storage HAL — a file on desktop/
     * handheld, the memory card (libmc) on PS2. */
    if (plat_save_read(&g_save, (int)sizeof g_save) == (int)sizeof g_save &&
        g_save.magic == WACKI_SAVE_MAGIC)
        loaded = 1;

    if (!loaded) {
        memset(&g_save, 0, sizeof g_save);
        g_save.magic = WACKI_SAVE_MAGIC;
        /* First-launch defaults. T129 originally seeded sound_on=0 to
         * match the original engine (the 1997 build shipped with SFX
         * muted on first launch and required the user to toggle Solund
         * → SFX-on to hear them). The port has NO Solund button wired
         * to sound_on (only music/voice/subtitles/dialogues/extra),
         * so a fresh-install user has no way to enable SFX from inside
         * the game. Default to 1 so SFX work out of the box; users who
         * want the original mute behavior can edit Wacki.sav byte 5. */
        g_save.settings = (WackiSettings){
            .video_mode = 1, .sound_on = 1, .music_on = 1, .pad0 = 0,
            .voice_on = 1, .subtitles_on = 1, .dialogues_on = 1, .pad1 = 0
        };
        for (int i = 0; i < WACKI_SAVE_SLOTS; ++i) {
            WackiSlot *s = &g_save.slots[i];
            memcpy(s->world_default_snapshot,
                   g_default_world_state, sizeof s->world_default_snapshot);
            memcpy(s->name, WACKI_DEFAULT_SLOT_NAME,
                   sizeof WACKI_DEFAULT_SLOT_NAME);
        }
    }
}

/* ---- LoadSaveSlot ------------------------------------------------ *
 *
 * Pour the slot's contents back into the live globals and call
 * LoadStage(etap) to rebuild the world. */
int LoadSaveSlot(uint16_t idx)
{
    if (idx >= WACKI_SAVE_SLOTS) return 0;
    WackiSlot *s = &g_save.slots[idx];
    if (s->stage_indicator == 0) return 0;

    /* T102 call order matters:
     *   1. g_cur_etap = slot.etap_id
     *   2. LoadStage(etap)   ← BEFORE the memcpy
     *   3. g_cur_komnata = slot.stage_indicator
     *   4. memcpy script_vars / entity_state / scene_snapshot
     *
     * Earlier port had LoadStage AFTER the memcpy — LoadStage's
     * stage init + entry_script ran AFTER restoring vars and
     * clobbered them back to defaults. Quickload (F9) and menu Load
     * silently dropped all progress flags every time. */
    g_cur_etap = s->etap_id;
    LoadStage(g_cur_etap);
    g_cur_komnata = s->stage_indicator;
    memcpy(g_script_vars,    s->script_vars,    sizeof s->script_vars);
    memcpy(g_entity_state,   s->entity_state,   sizeof s->entity_state);
    memcpy(g_scene_snapshot, s->scene_snapshot, sizeof s->scene_snapshot);
    return 1;
}

/* ---- WriteSaveFile ----------------------------------------------- *
 *
 * Originally embedded in RunGameStageLoop; extracted here. */
void WriteSaveFile(void)
{
    /* Written through the storage HAL — an atomic tmp+rename file commit on
     * desktop/handheld, the memory card (libmc) on PS2. Either way the engine
     * never observes a half-written save. */
    g_save.magic = WACKI_SAVE_MAGIC;
    if (!plat_save_write(&g_save, (int)sizeof g_save))
        LOG_INFO("save", "save write failed — not persisted");
}

/* ---- T53 quicksave / quickload (port extension) ------------------ *
 *
 * Capture the current live globals (etap, komnata, script vars,
 * entity state, scene snapshot) into slot N, then write the file.
 * The slot's display name is set by the caller (the Save menu uses
 * the user's inline-edit input or a default; the F5 path stamps
 * "Quick%u"). Slot 0 is conventionally reserved for the F5/F9
 * cycle. */
int QuickSaveToSlot(uint16_t idx)
{
    if (idx >= WACKI_SAVE_SLOTS) return 0;
    /* Refuse to save when the game isn't in-progress (cur_etap == 0
     * means we're still in the menu). Otherwise the slot would be
     * written with stage_indicator=0 and LoadSaveSlot would reject
     * it. */
    if (g_cur_etap == 0 || g_cur_komnata == 0) return 0;

    WackiSlot *s = &g_save.slots[idx];
    s->stage_indicator = g_cur_komnata;
    s->etap_id         = g_cur_etap;
    memcpy(s->script_vars,    g_script_vars,    sizeof s->script_vars);
    memcpy(s->entity_state,   g_entity_state,   sizeof s->entity_state);
    memcpy(s->scene_snapshot, g_scene_snapshot, sizeof s->scene_snapshot);
    /* Slot name is owned by the caller — don't clobber it here. */

    WriteSaveFile();
    g_stats.total_quicksaves++;             /* T56 */
    LOG_INFO("save", "quicksave → slot %u (etap=%u komnata=%u)", idx, g_cur_etap, g_cur_komnata);
    return 1;
}

int QuickLoadFromSlot(uint16_t idx)
{
    if (idx >= WACKI_SAVE_SLOTS) return 0;
    WackiSlot *s = &g_save.slots[idx];
    if (s->stage_indicator == 0) {
        LOG_INFO("save", "quickload slot %u empty — skip", idx);
        return 0;
    }
    g_stats.total_quickloads++;             /* T56 */
    LOG_INFO("save", "quickload ← slot %u (etap=%u komnata=%u)", idx, s->etap_id, s->stage_indicator);
    return LoadSaveSlot(idx);
}
