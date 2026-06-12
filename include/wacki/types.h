/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * include/wacki/types.h — core types + format constants.
 *
 * Pulled out of include/wacki.h. The umbrella header still includes
 * us, so call sites continue to write `#include "wacki.h"`.
 *
 * Contains:
 *   - Screen / save / scene-routing constants and sentinels
 *   - DTA / Pkv2 / asset-magic constants
 *   - Engine-wide structs (DtaIndexEntry, AnimAsset, Entity,
 *     StageDef, WackiSettings, WackiSlot, WackiSaveFile, DemoScene,
 *     SceneDef, FontHandle (forward), TextRenderTarget, WackiStats)
 *   - Scene-flag bit masks (SCENE_FLAG_*)
 *
 * No function declarations and no extern globals — those live in
 * include/wacki/api.h and include/wacki/globals.h. */

#ifndef WACKI_TYPES_H
#define WACKI_TYPES_H

#include <stddef.h>
#include <stdint.h>

/* ---- screen + save file layout ----------------------------------- */

#define WACKI_SCREEN_W 640
#define WACKI_SCREEN_H 480
#define WACKI_SCREEN_BPP 8
#define WACKI_MAX_DIRTY_RECTS 256

/* Save-slot thumbnail dimensions (sub-sampled gameplay backbuffer
 * captured before opszyns opens, then stored per-slot in the .sav). */
#define SAVE_THUMB_W 126
#define SAVE_THUMB_H 78

#define WACKI_SAVE_MAGIC 0x45564153u /* "SAVE" */
#define WACKI_SAVE_FILE "Wacki.sav"

/* Full game title for the OS window title bar / taskbar / Alt-Tab. The
 * short "Wacki" stays the macOS menu-bar app name + dialog titles; the
 * window itself carries the complete name. ASCII only — no Polish
 * diacritics in the subtitle, so it needs no special encoding. */
#define WACKI_WINDOW_TITLE "Wacki: Kosmiczna rozgrywka"
#define WACKI_SAVE_SLOTS 10
#define WACKI_SLOT_SIZE 0x3012
#define WACKI_SAVE_SIZE 0x1E0C0
#define WACKI_DEFAULT_SLOT_NAME "Pusty"

#define WACKI_CD_LABEL "WACKI_1"

/* ---- scene routing constants ------------------------------------- */

/* The engine's "neutral / no verb" sentinel — sprinkled through every
 * SceneDef-walking helper, the cursor state machine, and the click-
 * dispatch path. 0x26 is the original engine's default fall-through
 * value for verb dispatch. */
#define SCENE_NEUTRAL_VERB 0x26

/* RunGameStageLoop flag bits. FULL_RESET (new game) zeroes script_vars
 * + entity_state + ResetInventory then LoadStage(1). SAVE_LOAD (came
 * from a save load) trusts LoadSaveSlot's prior g_stage_va /
 * g_cur_komnata restore but falls back to stage-1 defaults if either
 * was missed. */
#define STAGE_LOAD_FLAG_FULL_RESET 0x02
#define STAGE_LOAD_FLAG_SAVE_LOAD 0x10

/* g_game_over_code progress signals. NONE / DEATH / CHAPTER_PICK /
 * STAGE_END_AVI are the script-progress codes the engine epilogue
 * branches on; USER_QUIT (2) is the "user-confirmed quit to main
 * menu" sentinel set by ESC / F12→TAK / OPCJE→Quit. The dev-flow
 * treats any value not in this list as a user-quit intent. */
#define GAME_OVER_NONE 0
#define GAME_OVER_DEATH 1
#define GAME_OVER_USER_QUIT 2
#define GAME_OVER_CHAPTER_PICK 3
#define GAME_OVER_STAGE_END_AVI 4

/* sel_tlo chapter-select "Monter finale" pick value. */
#define DEV_PICK_FINALE 5

/* RunMenuScene hard-quit return code — set when the platform requests
 * shutdown (Cmd-Q / window close). */
#define MAIN_MENU_RC_HARD_QUIT 99

/* ---- DTA archive + PKv2 compression ------------------------------ */

#define DTA_MAGIC_BASE 0x45534142u /* "BASE" */
#define DTA_MAGIC_SPIS 0x53495053u /* "SPIS" */
#define DTA_NAME_LEN 12
#define DTA_DIR_ENTRY_SZ 16
#define PKV2_MAGIC 0x32764B50u /* "PKv2" */

typedef struct DtaIndexEntry
{
    char name[DTA_NAME_LEN];
    uint32_t file_offset;
} DtaIndexEntry;

typedef struct DtaFileHeader
{
    uint32_t magic;
    uint32_t compressed_size;
    uint32_t unpacked_size;
} DtaFileHeader;

typedef struct Pkv2Header
{
    uint32_t magic;
    uint32_t compressed_size;
    uint32_t unpacked_size;
} Pkv2Header;

/* ---- asset (.wyc / .msk / .fld) ---------------------------------- */

#define ASSET_MAGIC_ANIM 0x4D494E41u /* "ANIM" — .wyc */
#define ASSET_MAGIC_MASK 0x4B53414Du /* "MASK" — .msk */
#define ASSET_MAGIC_FILD 0x444C4946u /* "FILD" — .fld */

typedef struct AnimAsset
{
    uint16_t frame_count;
    uint16_t pad;
    uint16_t *off_widths;
    uint16_t *off_heights;
    uint16_t *off_drawX;
    uint16_t *off_drawY;
    uint8_t **pixel_ptrs;
    uint16_t max_w;
    uint16_t max_h;
    void *raw_buffer;
    uint32_t raw_size;
    uint16_t kind;
    /* flag_22 — raw ushort at byte +0x16 of the original ANIM file
     * header (= 16 bytes in). The original engine stores this at
     * AnimAsset+0x22 (hence the name). Used by case 0x30 SPAWN bit 0
     * → alpha-plane and case 0x2E/2F RegMaskList bit 1 → 8bpp click
     * test. The port's `kind` collapses non-zero values into 2/3;
     * flag_22 preserves the bits. */
    uint16_t flag_22;
    void *anim_script; /* per-asset sound-trigger table */
    char name[24];     /* basename used for [sampl] lookup */
} AnimAsset;

/* Entity is the per-actor / per-prop runtime struct. The original
 * engine stored it as a 102-byte (0x66) flat buffer with all pointer
 * fields as 4-byte slots; the byte offsets are absolute and the per-
 * entity script interpreter references them directly (e[+0x22]
 * anchor_x, e[+0x28] current_anim ptr, e[+0x2C] bytecode ptr,
 * e[+0x30] kind, etc.).
 *
 * On a 64-bit host we can't preserve those offsets AND fit real C
 * pointers (8 bytes) in the original 4-byte slots. The full 1:1 port
 * stores 4-byte SLOT IDs in the entity and resolves to real pointers
 * via ent_ptr_intern / ent_ptr_resolve.
 *
 * Critical port note: the script VM (RunScriptInterpreter + per-
 * entity interpreter) writes to RAW byte offsets via
 * `*(T *)((uint8_t *)e + N)`. Those offsets are the ORIGINAL 32-bit
 * engine's layout (anchor X at +0x22, atlas handle at +0x28, walker
 * target at +0x54, scale at +0x58, ... up to ~+0x66). Named C
 * pointer fields (8 bytes) DRIFT relative to those raw offsets on
 * 64-bit hosts; if a C-named pointer lands within the script-byte
 * range, script writes partially overwrite it and CORRUPT the
 * pointer.
 *
 * The trailing-zone fields below (pixels, kind, group_flags) sit
 * past byte 0x88 and are port-internal only — read/written via the
 * named accessors, NEVER via raw `*(T *)(eb + OFFSET)` script-byte
 * semantics. */
typedef struct Entity
{
    uint8_t _r0[8];           /* +0x00..+0x07 — header bytes */
    uint8_t flags1;           /* +0x08 — script byte: flags low */
    uint8_t flags2;           /* +0x09 — script byte: flags high */
    uint16_t cur_anim_id;     /* +0x0A — script byte: drawn X */
    uint16_t cur_anim_y;      /* +0x0C — script byte: drawn Y */
    uint16_t width;           /* +0x0E — script byte: width */
    uint16_t height;          /* +0x10 — script byte: height */
    uint8_t _r1[0xE0 - 0x12]; /* padding past full script-byte
                               * range. All script writes up to
                               * ~+0x66 land inside this region. */
    uint8_t *pixels;          /* +0xE0 — 8B aligned, alloc'd by
                               *         InitEntityBitmap, freed
                               *         by FreeEntity. */
    uint16_t kind;            /* +0xE8 — port-internal
                               *         (AllocEntity arg, NOT
                               *         script kind byte). */
    uint16_t group_flags;     /* +0xEA — port-internal (bit 0 =
                               *         primary plane, bit 2 =
                               *         secondary plane). */
} Entity;

/* Byte-offset accessors + Entity field offset constants. */
#include "entity_offsets.h"

/* ---- stage + save layout ----------------------------------------- */

#pragma pack(push, 1)
typedef struct StageDef
{
    void *unknown[5];
    char *ebek_wyc;
    char *fjej_wyc;
    char *panel_wyc;
    char *paleta_pal;
    uint16_t start_komnata;
    char *intro_avi;
    char *alt_avi;
    char *alt3_avi;
} StageDef;

typedef struct WackiSettings
{
    uint8_t video_mode, sound_on, music_on, pad0;
    uint8_t voice_on, subtitles_on, dialogues_on, pad1;
} WackiSettings;

/* Slot layout exactly fills WACKI_SLOT_SIZE (0x3012) — checked via
 * static assert below. */
typedef struct WackiSlot
{
    uint16_t stage_indicator;
    uint16_t etap_id;
    char name[30];
    uint32_t script_vars[0x129];
    uint32_t entity_state[0x11C];
    uint32_t scene_snapshot[0x1E];
    uint8_t world_default_snapshot[0x2664];
} WackiSlot;

typedef struct WackiSaveFile
{
    uint32_t magic;
    WackiSettings settings;
    WackiSlot slots[WACKI_SAVE_SLOTS];
} WackiSaveFile;
#pragma pack(pop)

/* Sanity: WackiSlot must occupy exactly WACKI_SLOT_SIZE bytes for
 * the on-disk Wacki.sav layout to remain compatible with the
 * original. */
typedef char wacki_slot_size_check
    [(sizeof(WackiSlot) == WACKI_SLOT_SIZE) ? 1 : -1];

/* ---- scene system ------------------------------------------------ */

/* DemoScene — per-room descriptor (bg .pic + .fld walkability + music
 * wav + walkable bbox fallback). The synthesised version is built by
 * LoadKomnataScene; per-stage tables are emitted by BuildStageTable. */
typedef struct DemoScene
{
    const char *name;
    const char *bg_pic;
    const char *fld_file;
    const char *music_wav;
    int walk_x0, walk_y0, walk_x1, walk_y1;
} DemoScene;

/* SceneDef — 1:1 with the on-disk SceneDef layout:
 *   +0   const char *background_pic       (.pic filename or NULL)
 *   +4   const char *mask_file            (.wyc atlas filename)
 *   +8   int (*on_click)(int trigger)     called every tick + on click
 *   +12  int  button_count
 *   +16  int  flags                        (see SCENE_FLAG_*)
 *   +20  struct { u16 id, def_anim, hover_anim; } buttons[N]
 *
 *  id          = the value passed as `trigger` to on_click when the
 *                button is clicked (and the value g_hover_scene_verb
 *                is set to while hovered).
 *  def_anim    = atlas frame drawn always (button at rest).
 *  hover_anim  = atlas frame drawn on top while the mouse is over
 *                the button's def_anim rect (mouse-over highlight). */
typedef struct SceneDef
{
    const char *background_pic;
    const char *mask_file;
    int (*on_click)(int trigger);
    int button_count;
    uint32_t flags;
    struct
    {
        uint16_t id, def_anim, hover_anim;
    } buttons[40];
    /* Optional — called every frame AFTER the default + hover
     * sprites have been painted. Use for overlays that must sit on
     * top of the hover highlight (e.g. save-slot inline-edit text).
     * NULL = skip. */
    void (*after_paint)(void);
} SceneDef;

#define SCENE_FLAG_REDRAW 0x01u
#define SCENE_FLAG_MOUSE_ONLY 0x02u
#define SCENE_FLAG_FORCE_CB 0x04u
#define SCENE_FLAG_FADE 0x08u
#define SCENE_FLAG_DISABLE_ESC 0x10u
#define SCENE_FLAG_KEEP_IMAGE 0x20u

/* ---- font + text render target ----------------------------------- */

typedef struct FontHandle FontHandle;

/* Pointer-bearing target descriptor for RenderTextLineToBuffer. The
 * original 32-bit engine used a 5-element uint32_t array (stride,
 * pixels-ptr, font-ptr, x, color). On 64-bit hosts pointer slots
 * must be uintptr_t to avoid truncation — using uint32_t crashed
 * when font/pixel allocations landed above the 4 GB boundary. */
typedef struct TextRenderTarget
{
    uint16_t stride;
    uint16_t x;
    uint8_t color_base;
    uint8_t _pad[3];
    uint8_t *pixels;
    FontHandle *font;
} TextRenderTarget;

/* ---- playthrough stats ------------------------------------------- */

/* T56 — incremented from game.c + stubs.c hot spots. StatsDump prints
 * a summary to stderr (and HUD overlay if wired). */
typedef struct WackiStats
{
    uint32_t boot_tick;           /* tick when game started */
    uint32_t total_clicks;        /* DispatchClickEvent count */
    uint32_t total_dialogs;       /* ScriptCallDialogBegin count */
    uint32_t total_komnata_loads; /* LoadKomnata count */
    uint32_t total_quicksaves;    /* F5 */
    uint32_t total_quickloads;    /* F9 */
} WackiStats;

/* The CygFile file-I/O shim moved to wacki/platform/storage.h (the storage
 * HAL); the umbrella header pulls that in alongside this one. */

#endif /* WACKI_TYPES_H */
