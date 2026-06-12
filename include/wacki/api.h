/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * include/wacki/api.h — public function declarations.
 *
 * Pulled out of include/wacki.h. The umbrella header still includes
 * us, so call sites continue to write `#include "wacki.h"`.
 *
 * Groups (roughly one per source module):
 *   Platform, FindDataRoot
 *   Graphics                    (src/graphics.c, src/anim)
 *   Audio + dialog speech       (src/audio.c, src/audio/sfx.c, ...)
 *   Archive                     (src/archive.c, src/depack.c)
 *   Assets                      (src/assets.c)
 *   Script VM                   (src/vm/main.c, src/vm/script_obj.c)
 *   Actor / entity              (src/actor)
 *   Save                        (src/save.c)
 *   Font + text                 (src/font.c)
 *   Game state machine          (src/game.c, src/menu, src/scene)
 *   Heap + cygio                (src/heap.c, src/cygio.c)
 *   Inventory + HUD             (src/hud)
 *   Sound queue                 (src/audio/sound_queue.c)
 *   Pointer-slot intern         (src/actor/intern.c) */

#ifndef WACKI_API_H
#define WACKI_API_H

#include <stdint.h>
#include "wacki/types.h"

/* ---- Platform layer (SDL2 by default) ---------------------------- */

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

int  FindDataRoot(void);

/* wacki.cfg display-preference persistence (src/config.c). ConfigLoad
 * runs before CLI/env parsing so stored prefs are the baseline;
 * ConfigSave is called after the first-run picker + on F11 toggle.
 * g_config_first_run is set by ConfigLoad when no wacki.cfg exists. */
void ConfigLoad(void);
void ConfigSave(void);
extern int g_config_first_run;

/* Deadline-aware frame pacer (src/timer.c). Caps the calling loop at
 * 1000/target_ms FPS but never sleeps PAST the deadline. Replaces the
 * old `SDL_Delay(target_ms)` pattern that added a fixed delay on top
 * of frame work — on slow hardware (Miyoo Mini Plus Cortex-A7) that
 * dropped sustained FPS roughly in half. */
void EnginePaceFrame(uint32_t target_ms);

/* ---- graphics.c -------------------------------------------------- */

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
/* Gradual palette fade of the current frame toward palette entry 0
 * (scene's index-0 colour, normally black). Used before game-over
 * cutscenes. Mutates g_palette_rgb in place; no-op in headless. */
void FadeOutToBlack(void);

/* Scene-BG atlas copy — see graphics.c. Save() copies a kind=2 atlas
 * frame's pixels as the scene's persistent BG; Paint() blits that
 * copy to the backbuffer at the top of each frame (only when a one-
 * shot flag-0x60 BG entity ran for this scene). Free() runs on
 * komnata transition. Used by komnaty whose table .pic is a stub
 * (e.g. magaz3j = 1×1) and the real BG comes from an atlas entity
 * (magaz3c.wyc). */
void SaveSceneBgAtlas(int16_t dx, int16_t dy,
                      uint16_t w, uint16_t h, const uint8_t *src);
void PaintSceneBgAtlasIfAny(void);
void FreeSceneBgAtlas(void);

void InstallPalette(const uint8_t *rgb, uint16_t first);

/* Decompress one RLE-encoded "rich" ANIM frame (asset kind=3) into a
 * flat (w*h)-byte raw pixel buffer. The src buffer is
 * AnimAsset.pixel_ptrs[frame], dst must be at least dst_len bytes. */
void DepackRleFrame(const uint8_t *src, uint8_t *dst, int dst_len);

/* Nearest-neighbor scaled colour-key blit (palette idx 0 =
 * transparent). Used for perspective-scaled actors. */
void BlitSpriteScaledColorKey(int16_t dx, int16_t dy,
                              uint16_t sw, uint16_t sh,
                              uint16_t dw, uint16_t dh,
                              const uint8_t *src);
/* Same as above + horizontal mirror flag for right-facing actors. */
void BlitSpriteScaledColorKeyFlip(int16_t dx, int16_t dy,
                                  uint16_t sw, uint16_t sh,
                                  uint16_t dw, uint16_t dh,
                                  const uint8_t *src, int flip_h);

/* T7: Alpha-plane scaled blit.
 * Used for entities with flag 0x100 + 0x400 (alpha + perspective
 * scaled).
 *   Mode 0 = nearest-neighbour with x-step LUT (equivalent to
 *            BlitSpriteScaledColorKey but uses precomputed step
 *            table).
 *   Mode 1 = 1D horizontal box filter with RGB12 quantization.
 *   Mode 2 = 2D box filter with RGB12 quantization (full alpha).
 *
 * Caller responsibilities:
 *   - Call RebuildAlphaQuantLuts when palette changes (Install
 *     Palette does this automatically).
 *   - SetAlphaTint(0x808080) for identity; <0x80 darkens, >0x80
 *     brightens the corresponding BGR channel.
 *   - src/dst are 8bpp paletted buffers, palette idx 0 = transparent
 *     (skipped during box-filter accumulation). */
void RebuildAlphaQuantLuts(void);
void SetAlphaTint(uint32_t bgr);
void BlitAlphaScaled(uint16_t src_w, uint16_t src_h, const uint8_t *src,
                     uint16_t dst_w, uint16_t dst_h, uint8_t *dst,
                     uint16_t mode);
/* Convenience wrapper: BlitAlphaScaled + colour-key shadow blit,
 * similar API to BlitSpriteScaledColorKey but with alpha-plane mode
 * selection. */
void BlitAlphaScaledToBackbuffer(int16_t dx, int16_t dy,
                                 uint16_t sw, uint16_t sh,
                                 uint16_t dw, uint16_t dh,
                                 const uint8_t *src, uint16_t mode);

/* ---- audio.c + audio subsystem ----------------------------------- */

int  InitializeDirectSound(void);
void PlaySceneCutsceneAvi(const char *avi_name);

/* Menu / background WAV music. The original engine opened a CD-DA
 * track via MCI; we accept a Dane_XX.dta filename that is a plain
 * RIFF/WAVE file on disk and stream it looped on a dedicated SDL
 * audio device. PlayMenuMusic("Dane_01.dta") => menu music. */
void PlayMenuMusic(const char *dta_name, int loop);
void StopMenuMusic(void);
/* Per-frame tick — no-op since the T6 mixer (callback handles loop
 * natively). Kept for API compatibility. */
void TickMenuMusic(void);

void AudioSetMusicEnabled(int on);
void AudioSetSfxEnabled  (int on);
void AudioSetVoiceEnabled(int on);
void AudioSetSoundEnabled(int on);

/* T6 audio mixer — single SDL device + callback mixing 8 channels:
 *   ch 0    = music (looped)
 *   ch 1    = dialog speech (one-shot)
 *   ch 2..7 = SFX pool (one-shot, age-based stealing)
 * All streams converted to 22050 Hz S16 stereo at load time. */
void     PlaySfx(const char *wav_name);
void     TickSfx(void);
uint32_t PlayDialogLine(const char *wav_name);  /* returns byte length */
void     StopDialogLine(void);
int      IsDialogLinePlaying(void);

/* Release the mixer's hold on the audio output (frees the device + channels)
 * so a single-slot backend can hand the hardware to an AVI playback; the
 * mixer re-opens lazily on the next play. Called by the AVI audio device. */
void     mixer_release(void);

/* Wacky.scr [sampl] parser. Walks the current komnata section
 * between (start, end) and populates the per-asset frame-trigger SFX
 * table from [animacja]…[sampl] pairs. */
void ResetDynamicSfxTable(void);
void ParseSamplTagsForKomnata(const uint8_t *start, const uint8_t *end);

/* Positional alpha-tint source queue (VM op 0x41/0x42 — dynamic
 * lighting on alpha-plane sprites, NOT sound; see audio/sound_queue.c). */
void     SoundQueueReset(void);
void     SoundQueueEnqueue(int16_t x, int16_t y, uint32_t rgb,
                           uint16_t radius);
/* Blend all sources at (lx, ly) → packed 0xBBGGRR tint (empty → 0x808080). */
uint32_t AlphaTintForListener(int16_t lx, int16_t ly);

/* Grafika menu "video_mode" toggle — gates the alpha-plane tint/lighting
 * effect. Non-zero = effect on. */
int      GraphicsAlphaFxEnabled(void);

/* ---- archive.c / depack.c ---------------------------------------- */

int  OpenDtaArchiveFile(const char *path);
int  LoadFileFromDta(const char *name, void **out_buf, uint32_t *out_size);
int  DtaPreload(const char *name);   /* load + pin in the RAM cache (any size) */
void DtaClearPreloads(void);         /* free pinned entries, keep the LRU */
void DepackPkv2Buffer(void *src, void *dst, void (*progress)(int));

/* ---- pe_loader.c ------------------------------------------------- *
 *
 * Resolves original WACKI.EXE addresses (verb tables, scripts,
 * asset filename strings, …) into host pointers. The engine binary
 * embeds the original .rdata + .data sections as a const slice
 * table at build time (see include/wacki/embedded_exe.h) — no
 * runtime init needed.
 *
 * PeLoaderInit / PeLoaderInitFromMemory parse a PE blob at runtime
 * and override the embedded table; used by the test suite (which
 * constructs synthetic PE blobs) and by tools that want to inspect
 * a different image. PeLoaderFree drops the override and falls back
 * to the embedded path. */
int          PeLoaderInit(const char *exe_path);
int          PeLoaderInitFromMemory(const uint8_t *file, size_t fsz,
                                    const char *label);
void         PeLoaderFree(void);
const void  *PeLoaderRead(uint32_t va);
int          PeLoaderLoaded(void);
int          PeLoaderContainsVA(uint32_t va);

/* Translate an original-VA pointer (asset filename / bytecode /
 * data) into a usable host pointer. */
const void  *xlat_binary_ptr(uint32_t addr);
const char  *xlat_asset_name(uint32_t addr);

/* ---- assets.c ---------------------------------------------------- */

AnimAsset *LoadAssetFromDtaBase(const char *name);
void       FreeAsset(AnimAsset *a);

/* ---- script VM --------------------------------------------------- */

int  RunScriptInterpreter(uint16_t this_id, uint16_t that_id,
                          uint8_t *bytecode);
int  LoadScriptFile(void *script_obj, const char *name);
int  FindScriptByStageAndRoom(void *script_obj, const char *etap,
                              const char *komnata);
int  ScriptObjFindSection(void *script_obj, const char *tag,
                          const char *param, const char *altparam);
const uint8_t *ScriptObjGetSectionStart(void *self);
const uint8_t *ScriptObjGetSectionEnd  (void *self);

/* ---- actor / entity ---------------------------------------------- */

Entity *AllocEntity(uint16_t w, uint16_t h, uint16_t kind, uint16_t flags);
void    RegisterEntityForUpdate(Entity *e, uint16_t kind, uint16_t id);
void    UpdateActorMovement(int16_t target_x, int16_t target_y);
void    FreeEntity(Entity *e);

/* Pointer-slot intern table (used by every Entity pointer-field
 * setter to keep the original 32-bit slot layout on 64-bit hosts). */
uint32_t ent_ptr_intern(void *p);
void    *ent_ptr_resolve(uint32_t slot);
void     ent_ptr_reset(void);   /* drop all slots; slot 0 stays NULL */

/* Entity list management. Two parallel lists — render (drawables)
 * and click (hotspots). `LinkEntityToList` takes the address of a
 * head global (g_render_list_head or g_click_list_head) and routes
 * accordingly. Iterators use a flag (0 = render, 1 = click). */
void   LinkEntityToList(Entity **head, Entity *e, int position);
void   UnlinkEntity(Entity *e);
void   EntityListClearAll(void);
Entity *EntityListFirst(int click_list);
Entity *EntityListAt(int click_list, int idx);
int    EntityListCount(int click_list);

/* Walker step (one tick) — used by op 0x10/0x11/0x12 ANIM_ACTOR.
 * Walks one pixel/tick via the actor walker stepper. */
void ActorWalkToBlocking(int idx, int16_t tx, int16_t ty);

/* ---- save.c ------------------------------------------------------ */

void LoadSaveStateOrInitialize(void);
int  LoadSaveSlot(uint16_t slot);
void WriteSaveFile(void);
/* T53 — F5/F9 helpers. Slot 0 reserved for the quicksave cycle. */
int  QuickSaveToSlot(uint16_t slot);
int  QuickLoadFromSlot(uint16_t slot);
/* T34 — boot-time hook: push Wacki.sav settings (music/sound/voice/
 * video) into the in-memory opt flags + audio mixer state. Called
 * once from RunMainGameLoop after LoadSaveStateOrInitialize. */
void ApplySavedSettings(void);

/* ---- font.c ------------------------------------------------------ */

FontHandle *ParseFutFontFile(const uint8_t *raw);
int         MeasureTextLine(FontHandle *f, const uint8_t *text);
void        RenderTextLineToBuffer(TextRenderTarget *t, const uint8_t *text);
uint16_t    FindKeyInTaggedTable(const char *table, char tag, int16_t key);

/* ---- game.c + scene / menu / komnata ----------------------------- */

int  InitializeGameSubsystems(void);
void RunMainGameLoop(void);
int  RunMenuScene(int transition_mode, SceneDef *scene);
void RunGameStageLoop(uint8_t flags);
int  LoadStage(uint16_t stage);
void ProcessGameFrameTick(void);
void DispatchClickEvent(uint16_t obj_id, uint16_t verb_id);
int  HasPendingKey(void);
uint16_t WaitForKey(void);
void PumpEvents(void);   /* alias for PlatformPumpEvents */

/* Go to komnata `id`. Runs a FULL synchronous transition via
 * LoadKomnataScene (walker freeze + LoadKomnata + per-scene asset
 * rebuild). The play_demo_scene main loop does NOT unwind — the
 * transition happens in-place. */
void ScriptGoToKomnata(uint16_t id);

/* T22 phase B — full scene transition for komnata `id`. Frees the
 * current room's BG/FLD, runs LoadKomnata (which preserves actors,
 * clears non-actor entities, runs new enter_script), then loads the
 * new room's BG/FLD + music into the g_scene_* / g_walk_* globals.
 * Called from ScriptGoToKomnata (op 0x20) and from F9 quickload. */
void LoadKomnataScene(uint16_t id);

/* Load the komnata identified by `id` (1-based) from the current
 * stage's komnata table:
 *   - locate entry in komnata array at stage+0
 *   - set g_cur_komnata = id
 *   - palette fade out (deferred)
 *   - clear entity lists (EntityListClearAll + VisibleMasksReset +
 *     ResetFrameSfxState)
 *   - run enter_script (entry+6)
 *   - palette fade in
 *   - run secondary script (entry+10) if non-NULL
 *
 * Returns the komnata name string (= bg_pic name) for the caller's
 * scene-lookup, or NULL on failure. */
const char *LoadKomnata(uint16_t id);

int  WackiMain(int argc, char **argv);
void DrawPlaceholderScreen(const char *wanted_file);   /* stubs.c */

/* ---- HUD: panel, cursor, inventory, item-name -------------------- */

void PanelHitTest(void);

/* T31 v2 — cursor state driver. UpdateCursorState picks the slot
 * (olowek/kaseta/magnes/drzwi) from hover_scene_verb +
 * hover_panel_verb + held_item; PaintCursor blits the chosen sprite
 * frame at the mouse position. */
void UpdateCursorState(void);
void PaintCursor(void);

/* Item-name voice-over. LoadItemNamesTable reads Item.scr at boot —
 * returns highest index seen. ItemHoverDwellTick runs once per
 * ProcessGameFrameTick after PanelHitTest. */
int  LoadItemNamesTable(void);
void ItemHoverDwellTick(void);

/* Inventory + panel page rotation — 60 backing slots paged into the
 * 6 panel buttons. Dialog choices reuse these slots. */
uint16_t *Inventory(void);
void      ResetInventory(void);
void      PanelPageSwap(void);
int       InventoryPagePrev(void);
int       InventoryPageNext(void);
void      InventoryPageCollapse(void);
void      InventorySetPageForItem(uint16_t item_verb);
int       InventoryAddItem(uint16_t item_verb);
int       InventoryRemoveItem(uint16_t item_verb);
void      InventoryDropItem(uint16_t item_verb);
int       InventoryHasItem(uint16_t item_verb);

/* ---- stats ------------------------------------------------------- */

void StatsDump(void);

/* ---- random ------------------------------------------------------ */

/* WackiRand — uniform random in [0, bound). */
uint32_t WackiRand(uint16_t bound);
void     WackiRandSeed(uint32_t seed);

/* ---- heap.c ------------------------------------------------------ */

void *xmalloc(uint32_t sz);
void *xcalloc(uint32_t sz, int zero);
void  xfree  (void *p);

/* The fopen_cyg/... file-I/O shim is declared in wacki/platform/storage.h
 * (the storage HAL), pulled in by the umbrella header. */

#endif /* WACKI_API_H */
