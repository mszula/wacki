/*
 * wacki.h — public API for the reconstructed Wacki engine.
 *
 * Reconstructed from a Ghidra reverse of the original WACKI.EXE
 * (Henryk Cygert, 1997 — Polish point-and-click adventure).
 *
 * The engine is built around a tiny platform layer (platform_sdl.c on
 * Mac/Linux/Win, or a Win32-DirectDraw layer that could be added later).
 * Function and field names match the RE'd binary; original Ghidra
 * addresses are preserved in comments.
 */
#ifndef WACKI_H
#define WACKI_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ----- Optional Win32 path (legacy) -------------------------------------- */
#if defined(WACKI_WITH_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <ddraw.h>
#  include <dsound.h>
#  include <mmsystem.h>
#else
   typedef void *HINSTANCE;
   typedef void *HWND;
   typedef int   BOOL;
#endif

/* ============= Constants ================================================ */
#define WACKI_SCREEN_W        640
#define WACKI_SCREEN_H        480
#define WACKI_SCREEN_BPP      8
#define WACKI_MAX_DIRTY_RECTS 256

#define WACKI_CD_LABEL        "WACKI_1"
/* WACKI_CDCHECK_TICKS removed — rule #7, no CD anti-piracy. */

#define DTA_MAGIC_BASE        0x45534142u    /* "BASE" */
#define DTA_MAGIC_SPIS        0x53495053u    /* "SPIS" */
#define DTA_NAME_LEN          12
#define DTA_DIR_ENTRY_SZ      16
#define PKV2_MAGIC            0x32764B50u    /* "PKv2" */

#define ASSET_MAGIC_ANIM      0x4D494E41u    /* "ANIM" — .wyc */
#define ASSET_MAGIC_MASK      0x4B53414Du    /* "MASK" — .msk */
#define ASSET_MAGIC_FILD      0x444C4946u    /* "FILD" — .fld */

#define WACKI_SAVE_MAGIC      0x45564153u    /* "SAVE" */
#define WACKI_SAVE_FILE       "Wacki.sav"
#define WACKI_SAVE_SLOTS      10
#define WACKI_SLOT_SIZE       0x3012
#define WACKI_SAVE_SIZE       0x1E0C0
#define WACKI_DEFAULT_SLOT_NAME "Pusty"

/* ============= Structures =============================================== */

typedef struct DtaIndexEntry {
    char     name[DTA_NAME_LEN];
    uint32_t file_offset;
} DtaIndexEntry;

typedef struct DtaFileHeader {
    uint32_t magic;
    uint32_t compressed_size;
    uint32_t unpacked_size;
} DtaFileHeader;

typedef struct Pkv2Header {
    uint32_t magic;
    uint32_t compressed_size;
    uint32_t unpacked_size;
} Pkv2Header;

typedef struct AnimAsset {
    uint16_t   frame_count;
    uint16_t   pad;
    uint16_t  *off_widths;
    uint16_t  *off_heights;
    uint16_t  *off_drawX;
    uint16_t  *off_drawY;
    uint8_t  **pixel_ptrs;
    uint16_t   max_w;
    uint16_t   max_h;
    void      *raw_buffer;
    uint32_t   raw_size;
    uint16_t   kind;
    /* flag_22 — raw ushort at byte +0x16 of the original ANIM file header
     * (= 16 bytes in). The original engine stores this at AnimAsset+0x22
     * (hence the name). Used by case 0x30 SPAWN bit 0 → alpha-plane and
     * case 0x2E/2F RegMaskList bit 1 → 8bpp click test. The port's `kind`
     * collapses non-zero values into 2/3; flag_22 preserves the bits. */
    uint16_t   flag_22;
    void      *anim_script;     /* per-asset sound-trigger table (see audio.c) */
    char       name[24];        /* basename used for [sampl]-table lookup */
} AnimAsset;

/* Entity is the per-actor / per-prop runtime struct. The original engine
 * stored it as a 102-byte (0x66) flat buffer with all pointer fields as
 * 4-byte slots; the byte offsets are absolute and the per-entity script
 * interpreter (FUN_004012E0) references them directly (e[+0x22] anchor_x,
 * e[+0x28] current_anim ptr, e[+0x2C] bytecode ptr, e[+0x30] kind, etc.).
 *
 * On a 64-bit host we can't preserve those offsets AND fit real C pointers
 * (8 bytes) in the original 4-byte slots. The full 1:1 port stores 4-byte
 * SLOT IDs in the entity and resolves to real pointers via ent_ptr_intern
 * / ent_ptr_resolve. The current SDL build keeps the demo path running
 * with a richer named-field struct (below) while the slot-based entity
 * walker is staged in actor.c under a feature flag. */
/* Entity layout — critical port note:
 *
 * The script VM (RunScriptInterpreter + per-entity FUN_004012E0) writes
 * to RAW byte offsets via `*(T *)((uint8_t *)e + N)`. Those offsets are
 * the ORIGINAL 32-bit engine's layout (anchor X at +0x22, atlas handle
 * at +0x28, walker target at +0x54, scale at +0x58, ... up to ~+0x66).
 *
 * Named C fields with 8-byte-aligned pointers DRIFT relative to those
 * raw offsets on 64-bit hosts. Critically: if a C-named pointer field
 * (8 bytes) is positioned within the script-byte range, script writes
 * partially overwrite it and CORRUPT the pointer.
 *
 * Specific bug discovered round 11: `pixels` at offset 0x20 was
 * corrupted by anchor X writes at byte 0x22. The fix is to position
 * every NATIVE pointer (uint8_t *, void *) AFTER byte 0x88 — past the
 * script-byte territory.
 *
 * For 64-bit safety we keep all native pointers in a TRAILING zone via
 * explicit padding, and use raw byte offsets (+0x28 atlas, +0x2C
 * bytecode, +0x16 mask pixel) for script-interop fields — those are
 * stored as 4-byte intern handles via ent_ptr_intern().                */
typedef struct Entity {
    uint8_t  _r0[8];               /* +0x00..+0x07 — header bytes */
    uint8_t  flags1;               /* +0x08 — script byte: flags low */
    uint8_t  flags2;               /* +0x09 — script byte: flags high */
    uint16_t cur_anim_id;          /* +0x0A — script byte: drawn X */
    uint16_t cur_anim_y;           /* +0x0C — script byte: drawn Y */
    uint16_t width;                /* +0x0E — script byte: width */
    uint16_t height;               /* +0x10 — script byte: height */
    uint8_t  _r1[0xE0 - 0x12];     /* padding past full script-byte range.
                                    * All script writes up to ~+0x66 land
                                    * inside this region. Trailing-zone
                                    * fields below are port-internal only
                                    * (read/written via named access),
                                    * NEVER by raw `*(T *)(eb + OFFSET)`
                                    * script-byte semantics. */
    /* ----- Trailing zone (port-internal, safe from script-byte writes) -- *
     * After #100 (pixels collision fix) and #106 (UpdateActorMovement
     * raw-byte fix), the only named fields the port still uses are these
     * three. Earlier struct had a long list of DEAD fields (start_x/y,
     * target_anim_x/y, frame_index, current_anim, attached_prop,
     * state_flags, walk_dx/dy_remaining, z_perspective_off) — all
     * uninitialised, never read after #106. Removed in T10 for clarity. */
    uint8_t *pixels;               /* +0xE0 — 8B aligned, alloc'd by
                                    *         InitEntityBitmap / freed
                                    *         by FreeEntity. */
    uint16_t kind;                 /* +0xE8 — port-internal (AllocEntity
                                    *         arg, NOT script kind byte). */
    uint16_t group_flags;          /* +0xEA — port-internal (bit 0 = primary
                                    *         plane, bit 2 = secondary plane;
                                    *         read by InitEntityBitmap). */
} Entity;

/* Byte-offset accessors + Entity field offset constants. Defined in a
 * separate header so any TU that needs to address Entity fields by
 * offset can pull them in without dragging the whole umbrella. */
#include "entity_offsets.h"

/* Pointer-slot helpers (kept for future entity-walker port). */
uint32_t ent_ptr_intern(void *p);
void    *ent_ptr_resolve(uint32_t slot);

/* Entity list management. Two parallel lists — render (drawables) and
 * click (hotspots). `LinkEntityToList` takes the address of a head
 * global (g_render_list_head or g_click_list_head) and routes
 * accordingly. Iterators use a flag (0 = render, 1 = click). */
extern Entity *g_render_list_head;
extern Entity *g_click_list_head;

void   LinkEntityToList(Entity **head, Entity *e, int position);
void   UnlinkEntity(Entity *e);
void   EntityListClearAll(void);
Entity *EntityListFirst(int click_list);
Entity *EntityListAt(int click_list, int idx);
int    EntityListCount(int click_list);

#pragma pack(push, 1)
typedef struct StageDef {
    void    *unknown[5];
    char    *ebek_wyc;
    char    *fjej_wyc;
    char    *panel_wyc;
    char    *paleta_pal;
    uint16_t start_komnata;
    char    *intro_avi;
    char    *alt_avi;
    char    *alt3_avi;
} StageDef;

typedef struct WackiSettings {
    uint8_t video_mode, sound_on, music_on, pad0;
    uint8_t voice_on,   subtitles_on, dialogues_on, pad1;
} WackiSettings;

/* Slot layout exactly fills WACKI_SLOT_SIZE (0x3012) — checked via
 * static assert below. No pad[] needed; the fields above sum to the
 * required size. The pad slot existed earlier when scene_snapshot was
 * sized differently; keeping the assert as a safety net for future
 * resizing. */
typedef struct WackiSlot {
    uint16_t stage_indicator;
    uint16_t etap_id;
    char     name[30];
    uint32_t script_vars[0x129];
    uint32_t entity_state[0x11C];
    uint32_t scene_snapshot[0x1E];
    uint8_t  world_default_snapshot[0x2664];
} WackiSlot;

typedef struct WackiSaveFile {
    uint32_t      magic;
    WackiSettings settings;
    WackiSlot     slots[WACKI_SAVE_SLOTS];
} WackiSaveFile;
#pragma pack(pop)

/* Sanity: WackiSlot must occupy exactly WACKI_SLOT_SIZE bytes for the
 * on-disk Wacki.sav layout to remain compatible with the original. */
typedef char wacki_slot_size_check
    [ (sizeof(WackiSlot) == WACKI_SLOT_SIZE) ? 1 : -1 ];

/* SceneDef — 1:1 with the on-disk SceneDef at DAT_00445CD0:
 *   +0   const char *background_pic       (.pic filename or NULL)
 *   +4   const char *mask_file            (.wyc atlas filename)
 *   +8   int (*on_click)(int trigger)     called every tick + on click
 *   +12  int  button_count
 *   +16  int  flags                        (see SCENE_FLAG_*)
 *   +20  struct { u16 id, def_anim, hover_anim; } buttons[N]
 *
 *  id           = the value passed as `trigger` to on_click when the button
 *                 is clicked (and the value DAT_0044988c is set to while
 *                 hovered, per FUN_00406bb0).
 *  def_anim     = atlas frame drawn always (button at rest).
 *  hover_anim   = atlas frame drawn on top while the mouse is over the
 *                 button's def_anim rect (mouse-over highlight).
 */
typedef struct SceneDef {
    const char *background_pic;
    const char *mask_file;
    int (*on_click)(int trigger);
    int          button_count;
    uint32_t     flags;
    struct { uint16_t id, def_anim, hover_anim; } buttons[40];
    /* Optional — called every frame AFTER the default + hover sprites have
     * been painted. Use for overlays that must sit on top of the hover
     * highlight (e.g. save-slot inline-edit text). NULL = skip. */
    void       (*after_paint)(void);
} SceneDef;

#define SCENE_FLAG_REDRAW       0x01u
#define SCENE_FLAG_MOUSE_ONLY   0x02u
#define SCENE_FLAG_FORCE_CB     0x04u
#define SCENE_FLAG_FADE         0x08u
#define SCENE_FLAG_DISABLE_ESC  0x10u
#define SCENE_FLAG_KEEP_IMAGE   0x20u

/* ============= Platform layer (SDL2 by default) ========================= */

int  PlatformInit(int w, int h, const char *title);
void PlatformShutdown(void);
void PlatformPresent(const uint8_t *shadow,
                     const uint8_t *palette_rgb /* 256*3 */,
                     int w, int h);
void PlatformPumpEvents(void);
int  PlatformShouldQuit(void);
void PlatformShowMessageBox(const char *title, const char *body);
/* Inline-edit typed-char input (save-slot rename). PlatformSetTextInput
 * enables SDL text input; PlatformPollTypedChar drains the queue
 * one byte per call (returns 0 when empty). 0x08 = Backspace,
 * 0x0D = Enter, otherwise printable single-byte char. */
void    PlatformSetTextInput(int on);
uint8_t PlatformPollTypedChar(void);
void    PlatformPushTypedChar(uint8_t c);

/* ============= Module APIs ============================================== */

int  CheckCdRomDrive(void);
int  CheckDirectSoundVersion(void);

/* graphics.c */
void BlitSpriteToBackbuffer(uint16_t dx, uint16_t dy,
                            uint16_t sx, uint16_t sy,
                            uint16_t cw, uint16_t ch,
                            uint16_t pw, uint16_t ph,
                            uint8_t *src, int16_t mode);
void PaintImageToBackbuffer(int16_t dx, int16_t dy,
                            uint16_t cw, uint16_t ch,
                            const uint8_t *src);
void FlushFrameToPrimary(void);
void RestorePrevFrameRects(void);
void FlipBuffersClearWith(uint8_t value);

/* Scene-BG atlas copy — see graphics.c. Save() copies a kind=2 atlas
 * frame's pixels as the scene's persistent BG; Paint() blits that copy
 * to the backbuffer at the top of each frame (only when a one-shot
 * flag-0x60 BG entity ran for this scene). Free() runs on komnata
 * transition. Used by komnaty whose table .pic is a stub (e.g. magaz3j
 * = 1×1) and the real BG comes from an atlas entity (magaz3c.wyc). */
extern uint8_t *g_scene_bg_atlas_copy;
void SaveSceneBgAtlas(int16_t dx, int16_t dy,
                      uint16_t w, uint16_t h, const uint8_t *src);
void PaintSceneBgAtlasIfAny(void);
void FreeSceneBgAtlas(void);
void InstallPalette(const uint8_t *rgb, uint16_t first);
/* Decompress one RLE-encoded "rich" ANIM frame (asset kind=3) into a flat
 * (w*h)-byte raw pixel buffer. 1:1 with FUN_00410cb0. The src buffer is
 * AnimAsset.pixel_ptrs[frame], dst must be at least dst_len bytes. */
void DepackRleFrame(const uint8_t *src, uint8_t *dst, int dst_len);

/* Nearest-neighbor scaled color-key blit (palette idx 0 = transparent).
 * Used for perspective-scaled actors. See graphics.c for the full notes. */
void BlitSpriteScaledColorKey(int16_t dx, int16_t dy,
                              uint16_t sw, uint16_t sh,
                              uint16_t dw, uint16_t dh,
                              const uint8_t *src);
/* Same as above + horizontal mirror flag for right-facing actors. */
void BlitSpriteScaledColorKeyFlip(int16_t dx, int16_t dy,
                                  uint16_t sw, uint16_t sh,
                                  uint16_t dw, uint16_t dh,
                                  const uint8_t *src, int flip_h);

/* T7: Alpha-plane scaled blit — 1:1 port of FUN_00410960 + helpers.
 * Used for entities with flag 0x100 + 0x400 (alpha + perspective scaled).
 * Mode 0 = nearest-neighbor with x-step LUT (equivalent to
 *          BlitSpriteScaledColorKey but uses precomputed step table).
 * Mode 1 = 1D horizontal box filter with RGB12 quantization.
 * Mode 2 = 2D box filter with RGB12 quantization (full alpha).
 *
 * Caller responsibilities:
 *   - Call RebuildAlphaQuantLuts when palette changes (InstallPalette
 *     does this automatically).
 *   - SetAlphaTint(0x808080) for identity; <0x80 darkens, >0x80
 *     brightens the corresponding BGR channel.
 *   - src/dst are 8bpp paletted buffers, palette idx 0 = transparent
 *     (skipped during box-filter accumulation). */
void RebuildAlphaQuantLuts(void);
void SetAlphaTint(uint32_t bgr);
void BlitAlphaScaled(uint16_t src_w, uint16_t src_h, const uint8_t *src,
                     uint16_t dst_w, uint16_t dst_h, uint8_t *dst,
                     uint16_t mode);
/* Convenience wrapper: BlitAlphaScaled + color-key shadow blit, similar
 * API to BlitSpriteScaledColorKey but with alpha-plane mode selection. */
void BlitAlphaScaledToBackbuffer(int16_t dx, int16_t dy,
                                 uint16_t sw, uint16_t sh,
                                 uint16_t dw, uint16_t dh,
                                 const uint8_t *src, uint16_t mode);

/* audio.c */
int  InitializeDirectSound(void);
void PlaySceneCutsceneAvi(const char *avi_name);
/* Menu / background WAV music. The original engine opened a CD-DA track via
 * MCI (FUN_0040cab0 + FUN_0040d380); we accept a Dane_XX.dta filename that
 * is a plain RIFF/WAVE file on disk and stream it looped on a dedicated
 * SDL audio device. PlayMenuMusic("Dane_01.dta") => menu music.  */
void PlayMenuMusic(const char *dta_name, int loop);
void StopMenuMusic(void);
/* Per-frame tick — no-op since T6 mixer (callback handles loop natively).
 * Kept for API compatibility. */
void TickMenuMusic(void);

/* Options-menu toggles — mirror DAT_0045511E/22/1F flags. When music or
 * the global sound flag flips off mid-play, the music channel is stopped;
 * when flipped back on, the last-requested track resumes. SFX flag just
 * gates new PlaySfx calls. */
extern int g_audio_music_enabled;
extern int g_audio_sfx_enabled;
extern int g_audio_voice_enabled;
extern int g_audio_sound_enabled;
/* T103 — Solund-menu non-audio gates. Set/cleared by SolundClick. */
extern uint8_t g_subtitles_on;       /* DAT_00455121 — gates op 0x09 SHOW_TEXT */
extern uint8_t g_dialogues_on;       /* DAT_00455122 — gates op 0x52/0x53 */
extern int g_audio_sfx_enabled;
extern int g_audio_sound_enabled;
void AudioSetMusicEnabled(int on);
void AudioSetSfxEnabled  (int on);
void AudioSetVoiceEnabled(int on);    /* T103 — dialog audio toggle */
void AudioSetSoundEnabled(int on);

/* T6 audio mixer — single SDL device + callback mixing 8 channels:
 *   ch 0 = music (looped)
 *   ch 1 = dialog speech (one-shot)
 *   ch 2..7 = SFX pool (one-shot, age-based stealing)
 * All streams converted to 22050 Hz S16 stereo at load time. */
void     PlaySfx(const char *wav_name);
void     TickSfx(void);
uint32_t PlayDialogLine(const char *wav_name);  /* returns byte length */
void     StopDialogLine(void);
int      IsDialogLinePlaying(void);

/* archive.c */
int  OpenDtaArchiveFile(const char *path);
int  LoadFileFromDta(const char *name, void **out_buf, uint32_t *out_size);

/* depack.c */
void DepackPkv2Buffer(void *src, void *dst, void (*progress)(int));

/* assets.c */
AnimAsset *LoadAssetFromDtaBase(const char *name);
void       FreeAsset(AnimAsset *a);

/* script.c */
int  RunScriptInterpreter(uint16_t this_id, uint16_t that_id, uint8_t *bytecode);
int  LoadScriptFile(void *script_obj, const char *name);
int  FindScriptByStageAndRoom(void *script_obj, const char *etap, const char *komnata);
int  ScriptObjFindSection(void *script_obj, const char *tag,
                          const char *param, const char *altparam);
const uint8_t *ScriptObjGetSectionStart(void *self);
const uint8_t *ScriptObjGetSectionEnd  (void *self);

/* actor.c */
Entity *AllocEntity(uint16_t w, uint16_t h, uint16_t kind, uint16_t flags);
void    RegisterEntityForUpdate(Entity *e, uint16_t kind, uint16_t id);
void    UpdateActorMovement(int16_t target_x, int16_t target_y);
void    FreeEntity(Entity *e);

/* save.c */
void LoadSaveStateOrInitialize(void);
int  LoadSaveSlot(uint16_t slot);
void WriteSaveFile(void);
/* T53 — F5/F9 helpers. Slot 0 reserved for the quicksave cycle. */
int  QuickSaveToSlot(uint16_t slot);
int  QuickLoadFromSlot(uint16_t slot);
/* T34 — boot-time hook: push Wacki.sav settings (music/sound/voice/video)
 * into the in-memory opt flags + audio mixer state. Called once from
 * RunMainGameLoop after LoadSaveStateOrInitialize. */
void ApplySavedSettings(void);

/* font.c */
typedef struct FontHandle FontHandle;
FontHandle *ParseFutFontFile(const uint8_t *raw);
int         MeasureTextLine(FontHandle *f, const uint8_t *text);

/* Wacky.scr [sampl] parser — port of FUN_00409970. Walks the current
 * komnata section between (start, end) and populates the per-asset
 * frame-trigger SFX table from [animacja]…[sampl] pairs. */
void ResetDynamicSfxTable(void);
void ParseSamplTagsForKomnata(const uint8_t *start, const uint8_t *end);
/* Pointer-bearing target descriptor for RenderTextLineToBuffer. Original
 * 32-bit engine used a 5-element uint32_t array (stride, pixels-ptr,
 * font-ptr, x, color). On 64-bit hosts pointer slots must be uintptr_t
 * to avoid truncation — using uint32_t crashed when font/pixel
 * allocations landed above the 4 GB boundary. */
typedef struct TextRenderTarget {
    uint16_t     stride;
    uint16_t     x;
    uint8_t      color_base;
    uint8_t      _pad[3];
    uint8_t     *pixels;
    FontHandle  *font;
} TextRenderTarget;

void        RenderTextLineToBuffer(TextRenderTarget *t, const uint8_t *text);
uint16_t    FindKeyInTaggedTable(const char *table, char tag, int16_t key);

/* game.c */
int  InitializeGameSubsystems(void);
void RunMainGameLoop(void);
int  RunMenuScene(int transition_mode, SceneDef *scene);
void RunGameStageLoop(uint8_t flags);
int  LoadStage(uint16_t stage);
void ProcessGameFrameTick(void);
void DispatchClickEvent(uint16_t obj_id, uint16_t verb_id);
int  HasPendingKey(void);
uint16_t WaitForKey(void);
void PumpWin32Messages(void);            /* alias for PlatformPumpEvents on portable build */

int  WackiMain(int argc, char **argv);
void DrawPlaceholderScreen(const char *wanted_file);   /* stubs.c */

/* heap.c / cygio.c */
void *xmalloc(uint32_t sz);
void *xcalloc(uint32_t sz, int zero);
void  xfree  (void *p);

typedef struct CygFile CygFile;
CygFile *fopen_cyg (const char *name, const char *mode);
void     fclose_cyg(CygFile *);
uint32_t fread_cyg (void *dst, uint32_t sz, uint32_t n, CygFile *);
void     fseek_cyg (CygFile *, int32_t off, int whence);
int32_t  ftell_cyg (CygFile *);

/* ============= Globals (defined in stubs.c / module owners) ============= */
extern HINSTANCE g_hInstance;
extern HWND      g_hWnd;
extern BOOL      g_in_foreground;
extern char      g_cd_path[260];
extern uint8_t   g_palette_rgb[256*3];
extern uint16_t  g_screen_w, g_screen_h;
extern uint8_t  *g_back_shadow;          /* 320×240×8bpp paletted shadow buffer */
extern uint16_t  g_screen_w_dim, g_screen_h_dim;

extern uint32_t  g_script_vars[0x129];
extern uint32_t  g_entity_state[0x11C];
/* g_return_reg removed — see script.c: it's now a macro aliasing var[4]
 * (DAT_00449890 in the original layout). External consumers that need
 * the value should read g_script_vars[4]. */
extern uint16_t  g_active_actor;
extern uint16_t  g_cur_etap;
extern uint16_t  g_cur_komnata;

extern StageDef *g_stage;
extern StageDef *g_stage_table[5];

/* Original PE virtual address of the current stage's DAT_0044A19C value.
 * Set by LoadStage / play_demo_scene. Read by DispatchClickEvent to walk
 * the per-stage verb_table (+4) and object_table (+8) in PE memory via
 * PeLoaderRead. Stage 1 = 0x00428220, stage 2 = 0x004310A0, etc. — these
 * come from PTR_PTR_00442FA8[etap-1]. 0 = no stage (DispatchClick noop). */
extern uint32_t g_stage_va;

extern FontHandle *g_default_font;       /* DAT_0044E598 — "Futura.30" */

/* DAT_0044E5E8 — currently-held inventory item id. 0 = nothing held
 * (treat as 0x26 = "look at" verb). Set when the user clicks an item
 * in the bottom panel; consumed by DispatchClickEvent as `this_id`. */
extern uint16_t g_held_item;

/* WackiRand — 1:1 with FUN_00410F50. Uniform random in [0, bound). */
uint32_t WackiRand(uint16_t bound);
void     WackiRandSeed(uint32_t seed);

/* Go to komnata `id` — script staging entry. T22 phase B: now does a
 * FULL synchronous transition via LoadKomnataScene (walker freeze +
 * LoadKomnata + per-scene asset rebuild). The play_demo_scene main
 * loop does NOT unwind on this — transitions happen in-place.
 * T42 removed the legacy g_pending_komnata / g_komnata_loaded_by_op20
 * staging globals; everything goes through LoadKomnataScene now. */
void ScriptGoToKomnata(uint16_t id);

/* T22 phase B — full scene transition for komnata `id`. Frees the
 * current room's BG/FLD, runs LoadKomnata (which preserves actors,
 * clears non-actor entities, runs new enter_script), then loads the
 * new room's BG/FLD + music into the g_scene_* / g_walk_* globals.
 * Called from ScriptGoToKomnata (op 0x20) and from F9 quickload. */
void LoadKomnataScene(uint16_t id);

/* LoadKomnata — 1:1 port of FUN_00402A50 @ 0x00402A50.
 *
 * Loads the komnata identified by `id` (1-based) from the current
 * stage's komnata table:
 *   - locate entry in komnata array at stage+0
 *   - set g_cur_komnata = id
 *   - palette fade out (deferred)
 *   - clear entity lists (EntityListClearAll + VisibleMasksReset +
 *     ResetFrameSfxState — our mirror of FUN_00405F80 + FUN_00402DB0)
 *   - run enter_script (entry+6)
 *   - palette fade in
 *   - run secondary script (entry+10) if non-NULL
 *
 * Returns the komnata name string (= bg_pic name) for the caller's
 * scene-lookup, or NULL on failure. */
const char *LoadKomnata(uint16_t id);

/* Send actor `idx` (0=Ebek, 1=Fjej) toward (tx, ty) and block until
 * arrival (or safety timeout). Used by op 0x10/0x11/0x12 ANIM_ACTOR.
 * Walks one pixel/tick via FUN_00401150 + FUN_004011D0 stepper. */
void ActorWalkToBlocking(int idx, int16_t tx, int16_t ty);

/* g_game_over_code — DAT_004498B8 in the original.
 *
 * REVERSE-ENGINEERING NOTE (Bug 2 fix, round 32):
 * In WACKI.EXE this is at PE 0x004498B8, which is at byte offset 0x38
 * (= 14 * 4) inside the g_script_vars[0x129] array (DAT_00449880).
 * It IS g_script_vars[14] — the same dword, just accessed as an int.
 *
 * That matters because scripts trigger end-of-stage / death / chapter-select
 * via the main-VM SET_VAR opcode (op 0x0D) with var index 14:
 *   - val=1  → death (Dane_14.dta cutscene)
 *   - val=3  → chapter-select UI (sel_tlo.pic)
 *   - val=4  → stage-end death cutscene then return to menu
 * Confirmed: 9 such writes exist in WACKI.EXE bytecode (4×val=1, 4×val=3,
 * 1×val=4). Pre-fix the port had g_game_over_code as a SEPARATE int —
 * scripts wrote to script_vars[14] which nobody read; the post-loop
 * handler read its own zero. End-of-stage cutscene + chapter-select
 * never fired. After fix: the two are the SAME memory.
 *
 * The macro requires g_script_vars[] to already be declared (above). */
#define g_game_over_code  (*(int *)&g_script_vars[14])
/* g_completed_stages — DAT_004498C4 in original (Bug 5 fix #21).
 * Same alias pattern as g_game_over_code: DAT_004498C4 sits at byte +0x44
 * from g_script_vars base (DAT_00449880), so 0x44/4 = index 17. Bitfield:
 * bit i set = stage (i+1) completed. Original scripts set it via op 0x0A
 * VAR_OR var[17] imm=1<<stage in the stage's final ending bytecode (right
 * before the var[14]=3 chapter-select trigger). Pre-fix the port had
 * g_completed_stages as a SEPARATE uint32_t — script's bit-set landed in
 * script_vars[17] which nobody read; SelTloRefreshButtons read its own
 * zero and lit up all 4 stages as still-to-do (red glow on every map
 * marker).  */
#define g_completed_stages  (*(uint32_t *)&g_script_vars[17])
extern int       g_save_request;

extern uint8_t   g_lmb_clicked;
extern uint8_t   g_rmb_clicked;
extern uint8_t   g_lmb_handled;
extern uint16_t  g_key_state;
/* T53 — F5 / F9 latches. PlatformPumpEvents sets these on F5/F9 key-down;
 * the play_demo_scene main loop consumes and clears them per frame. */
extern uint8_t   g_quicksave_request;
extern uint8_t   g_quickload_request;
/* T56 — F3 stats dump latch. */
extern uint8_t   g_stats_dump_request;
/* T24 — F12 pause/exit confirmation latch (Pytanie.scr equivalent). */
extern uint8_t   g_pause_menu_request;

/* T56 — playthrough stats. Incremented from game.c + stubs.c hot spots.
 * StatsDump() prints a summary to stderr (and HUD overlay if wired). */
typedef struct WackiStats {
    uint32_t boot_tick;        /* tick when game started — for elapsed */
    uint32_t total_clicks;     /* DispatchClickEvent count */
    uint32_t total_dialogs;    /* ScriptCallDialogBegin count */
    uint32_t total_komnata_loads;/* LoadKomnata count */
    uint32_t total_quicksaves; /* F5 */
    uint32_t total_quickloads; /* F9 */
} WackiStats;
extern WackiStats g_stats;
void StatsDump(void);

extern WackiSaveFile g_save;

extern uint32_t  g_tick_counter;
/* g_next_cd_check removed — rule #7. */
extern uint16_t  g_settings_anim_active;     /* T121: was uint8_t — original
                                              * DAT_0044E448 is ushort and
                                              * ScriptCallBgMaskSetup reads
                                              * `(flags & 0xff02) << 1` which
                                              * needs bits 8-15. */
extern uint16_t  g_cursor_speed;            /* DAT_0044A198 — undefined2 in Ghidra */
extern uint16_t  g_perspective_min;
extern uint16_t  g_perspective_step;

extern void *g_dialogues_obj;
extern void *g_scripts_obj;
extern void *g_items_obj;
extern AnimAsset *g_panel_cursor;
extern AnimAsset *g_panel_asset;        /* DAT_00453744 — stage panel.wyc */
extern AnimAsset *g_items_atlas;        /* DAT_0044E6AC — przedm.wyc icons */
extern Entity *g_actor[2];

/* Panel verb selection — FUN_00407260 / DAT_0044346A / DAT_0044E450. */
extern uint16_t g_panel_verb_tab[6];
extern uint16_t g_hover_panel_verb;
extern uint16_t g_hover_scene_verb;       /* DAT_0044988C — T31 v2 cursor */
extern uint8_t  g_panel_cursor_redirect;
extern uint8_t  g_panel_cursor_redirect2;
void PanelHitTest(void);
/* Item-name voice-over (FUN_00402230 / FUN_004021c0 1:1).
 * LoadItemNamesTable reads Item.scr at boot — returns highest index seen.
 * ItemHoverDwellTick runs once per ProcessGameFrameTick after PanelHitTest. */
int  LoadItemNamesTable(void);
void ItemHoverDwellTick(void);

/* T31 v2 — cursor state driver. UpdateCursorState picks the slot
 * (olowek/kaseta/magnes/drzwi) from hover_scene_verb + hover_panel_verb
 * + held_item; PaintCursor blits the chosen sprite frame at the mouse
 * position. 1:1 with FUN_004067C0 split in two. */
void UpdateCursorState(void);
void PaintCursor(void);

/* Per-frame deltas — see stubs.c for unit notes. _ms is real wall-clock
 * milliseconds (held-item ghost interp, speech-balloon dismiss timer);
 * _ticks is 10 ms units (1:1 with DAT_0044E578 — cursor anim, entity VM
 * +0x3C countdown, op 0x14 / op 0x26 / op 0x3D wait loops). */
extern uint32_t g_frame_delta_ms;
extern uint16_t g_frame_delta_ticks;

/* Inventory + panel page rotation — DAT_00443332 (60 slots) +
 * DAT_0044E458 (page index). Dialog choices reuse these slots.
 * 1:1 ports of FUN_00405630/4071F0/004055B0/4055D0/4056D0/405680. */
extern uint16_t  g_panel_page_idx;
extern uint16_t  g_panel_verb_tab_backup[6];
extern uint8_t   g_panel_redraw;
uint16_t *Inventory(void);
void ResetInventory(void);
void PanelPageSwap(void);
int  InventoryPagePrev(void);
int  InventoryPageNext(void);
void InventoryPageCollapse(void);
void InventorySetPageForItem(uint16_t item_verb);
int  InventoryAddItem(uint16_t item_verb);
int  InventoryRemoveItem(uint16_t item_verb);
void InventoryDropItem(uint16_t item_verb);
int  InventoryHasItem(uint16_t item_verb);

/* Positional sound queue (FUN_00410D20/D30/DA0). */
void SoundQueueReset(void);
void SoundQueueEnqueue(int16_t x, int16_t y, uint32_t sound_id, uint16_t volume);
uint32_t SoundQueueMixForListener(int16_t listener_x, int16_t listener_y);

#endif /* WACKI_H */
