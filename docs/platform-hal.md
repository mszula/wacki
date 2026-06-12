# Platform abstraction layer (HAL) — separating platform-specific code

## Problem

The PS2 port surfaced that platform-specific code is **interleaved** with
the engine core via `#ifdef`, not separated. There's a partial HAL already
— the `Platform*` functions in `include/wacki/api.h`, implemented in
`platform_sdl.c` + per-target `platform_{ps2,miyoo,portmaster}.c` — but a lot
still leaks into shared files. Survey of `#ifdef <platform>` density:

| Shared file | `#ifdef`s | Subsystem that varies |
|---|---|---|
| `src/data_root.c` | 23 | where the game data lives + how to discover it |
| `src/platform_sdl.c` | 13 | SDL base + delegations to PS2/handheld/macOS |
| `src/flic.c` | 9 | cutscene file reader (PS2 async vs stdio) |
| `src/main.c` | 7 | init order / CLI / lifecycle |
| `src/audio.c` | 6 | output device (audsrv vs SDL) |
| `src/save.c` | 4 | save storage (libmc vs file) |

Macros in play: `WACKI_PS2` (36), `WACKI_HANDHELD` (16), `__APPLE__` (9),
`_WIN32` (8), `WACKI_MIYOO` (4), `__linux__` (4), `WACKI_PORTMASTER` (2),
`WACKI_VITA` (1). Every new platform adds a branch to each hotspot.

## Principles

1. **Core code calls interfaces, never `#ifdef <platform>`.** Engine files
   (`game.c`, `vm/`, `scene/`, the `audio.c` mixer, `flic.c`, `save.c`,
   `data_root.c`) must be platform-agnostic.
2. **HAL is per-subsystem, not monolithic.** Platforms mix backends — PS2 =
   SDL input + custom video (gsKit) + audio (audsrv) + I/O (fileXio). So the
   abstraction is split along the axes that actually vary, and each platform
   picks an implementation per axis.
3. **Compile-time selection, not runtime.** Targets are cross-compiled
   (separate builds), so the linker picks the right `.c` files — no vtable /
   function-pointer dispatch overhead. Each interface is plain function
   declarations; exactly one platform provides the definitions.
4. **Adding a platform = a new directory + a Makefile entry.** Zero edits to
   core code, no new `#ifdef` in shared files.

## Target structure

```
include/wacki/platform/
    storage.h   data-root discovery + file open/read/seek/close + save R/W
    audio.h     open device + pull-callback hookup + lock/unlock + is_open
    video.h     init + present(shadow, palette) + mode
    input.h     poll -> cursor delta + buttons + typed chars
    system.h    init/shutdown + message box + folder-pick + thread/time
src/platform/
    sdl/        video_sdl.c audio_sdl.c input_sdl.c storage_stdio.c
                save_host.c system_sdl.c          (desktop + handheld base)
    ps2/        video_ps2.c audio_ps2.c storage_ps2.c save_ps2.c system_ps2.c
                (reuses sdl/ input)
    miyoo/      system_miyoo.c (MI_AO volume), input_miyoo.c
    portmaster/ input_portmaster.c
    win32/ macos/   folder-picker, atomic rename, message box
```

The Makefile composes a `PLATFORM_SRCS` list per `TARGET`; the core
`ENGINE_SRCS` never names a platform.

## The interfaces (what varies)

- **storage.h** — `plat_data_roots()` (candidate data dirs, per platform),
  the `CygFile` open/read/seek/close shim (already in `cygio.c`), and
  `plat_save_read/write`. Subsumes `data_root.c`'s path lists + `cygio.c` +
  `save.c`'s storage + the PS2 USB mount.
- **audio.h** — `plat_audio_open(spec, pull_cb)`, `_close`, `_lock/_unlock`,
  `_is_open`. The `audio.c` mixer becomes a pure channel mixer that supplies
  the pull callback; the platform drives it (SDL callback or the audsrv
  thread). Kills the `s_mix_dev == 0` / `mixer_is_open()` special-casing.
- **video.h** — `plat_video_init/present/mode` (mostly the existing
  `PlatformPresent`; PS2 present is gsKit).
- **input.h** — `plat_input_poll(...)` (the existing `platform_pad_*`).
- **system.h** — `plat_init/shutdown`, `plat_message_box`, `plat_pick_folder`
  (desktop), thread/time helpers.

## Migration plan (incremental; build stays green each step)

Ordered by worst spaghetti / cleanest win:

1. **Storage** — biggest win, ~27 `#ifdef`s removed.
   - 1a. **Save** (`save.c` -> `plat_save_read/write`; impls in
     `platform/sdl/save_host.c` + the PS2 libmc impl). *(first PoC)*
   - 1b. **Data-root** (`data_root.c` -> `plat_data_roots()`; PS2 host/cdfs/
     mass + USB mount in `storage_ps2.c`; SDCARD/roms in the SDL impl).
   - 1c. **File I/O** (`cygio.c` already abstracts open/read; formalize under
     `storage.h`).
2. **Audio** — `audio.c` mixer goes platform-agnostic; device moves to
   `audio_{sdl,ps2}.c`.
3. **FLIC** — `flic.c`'s `FlicFp` uses `storage.h`; PS2 async reader is a
   `storage_ps2.c` detail.
4. **Video + Input** — split `platform_sdl.c` into `sdl/{video,audio,input,
   system}`; remove its internal `#ifdef WACKI_PS2`.
5. **Lifecycle/CLI** — `main.c` `#ifdef`s -> `plat_system_init()` hooks.

## Adding a new platform

Create `src/platform/<plat>/` implementing the five interface headers (reuse
`sdl/` files where the platform has SDL2), add a `PLATFORM_SRCS` branch in
the Makefile. The core is untouched.

## Status

- [x] Plan written.
- [x] Step 1a — save storage behind `storage.h` (PoC; pattern + build
      composition established).
- [x] Step 1b — data-root. `data_root.c` is now platform-`#ifdef`-free
      (23 → 0): the portable search stays in core; external media / SD-card /
      PS2 devices + the native folder picker moved behind
      `plat_data_roots()` / `plat_prompt_data_folder()`
      (`platform/sdl/data_root_host.c` + the PS2 impl in `platform_ps2.c`).
      The file-existence probe routes through the existing `cygio` shim, so
      it needed no new interface.
- [x] Step 1c — file I/O. `cygio.c` deleted; its two backends split into
      `platform/sdl/file_host.c` (newlib stdio) and the fileXio backend in
      `platform_ps2.c` — no `#ifdef WACKI_PS2` left in a shared file. The
      `CygFile` shim (`fopen_cyg/...`) is now declared in `storage.h` (pulled
      by the umbrella), so `types.h`/`api.h` no longer carry it. **Storage
      subsystem done** — the three worst hotspots (`data_root.c` 23,
      `platform_sdl.c` partial, `save.c` 4) are off the core.
- [x] Step 2 — audio device. `audio.c` is now a pure channel mixer feeding a
      single pull callback (`mixer_pull`); the device lives behind
      `audio.h` — `plat_audio_open/close/is_open/lock/unlock`
      (`platform/sdl/audio_sdl.c` = SDL_OpenAudioDevice; `platform_ps2.c` =
      audsrv feeder thread). Killed the `s_mix_dev==0` / `mixer_is_open()` /
      `MIX_DEV_LOCK` per-platform special-casing. NOTE: the WAV *file read*
      (`load_wav_via_cyg`) still `#ifdef`s on PS2 — a storage concern, folded
      in a later pass.
- [x] Step 3 — FLIC reader. flic.c's `FlicFp` now drives `plat_flic_*`
      (wacki/platform/storage.h) — a single global read-ahead reader:
      `platform/sdl/flic_host.c` (setvbuf'd stdio) and the PS2 async ring-fill
      thread (renamed from `platform_ps2_aread_*`) in `platform_ps2.c`. The
      reader `#ifdef WACKI_PS2` is gone from flic.c. NOTE: flic.c's *AVI audio
      device* (the cutscene push path: audio_ensure / SDL_QueueAudio vs audsrv
      + the pacing loop) still `#ifdef`s on PS2 — a separate audio-device
      concern, deferred (the PS2 pacing is timing-sensitive — the "looping
      sample" fix — so it wants its own careful pass).
- [x] Step 4 — video split. platform_sdl.c's three `#ifdef WACKI_PS2` blocks
      (SDL_Init flags, gsKit video init, gsKit present) are gone — routed
      through a video HAL (wacki/platform/video.h): `plat_video_sdl_init_flags`
      / `plat_video_init` / `plat_video_present` / `plat_video_shutdown` /
      `plat_video_toggle_fullscreen` / `plat_video_message_box`. The SDL window
      + renderer + streaming-texture present moved to
      `platform/sdl/video_sdl.c`; PS2 wraps its gsKit + audsrv in
      `platform_ps2.c`. platform_sdl.c stays the shared input/event pump (PS2
      reuses it — the one remaining `WACKI_PS2` mention is just the
      `WACKI_HAS_SDL_GAMEPAD` feature gate, since the DualShock 2 *is* an SDL
      gamepad). Input wasn't physically split out — it has no `WACKI_PS2`
      `#ifdef` to remove (only SDL-family `WACKI_MIYOO`/`WACKI_HANDHELD`/
      `__APPLE__` variants, which are fine inside the SDL platform file).
- [x] Step 5 — lifecycle/CLI. main.c's edge-of-process `#ifdef`s (PS2 IOP
      bring-up, Win32 stderr→logfile, macOS app-support cwd, the PS2 exit
      park) are routed through a system HAL (wacki/platform/system.h):
      `plat_system_early_init` / `plat_system_exit`
      (`platform/sdl/system_sdl.c` + `platform_ps2.c`). The only conditionals
      left in main.c are the `WK_PS2_MARK` bring-up-trace macro (a no-op
      elsewhere — kept deliberately for the PS2 PINE debug rig) and the
      `WACKI_VERSION` build-string fallback; no platform lifecycle `#ifdef`
      blocks remain.

## Outcome

The five planned steps are done — the core engine (`save.c`, `data_root.c`,
`audio.c` mixer, `flic.c` reader, `platform_sdl.c`, `main.c`) calls
per-subsystem HAL interfaces and no longer `#ifdef`s on a platform. Adding a
platform is now a new `src/platform/<plat>/` dir implementing
`storage.h` / `audio.h` / `video.h` / `system.h` (reusing `sdl/` where the
platform has SDL2) plus one `PLATFORM_SRCS` branch in the Makefile.

Deferred follow-ups (tracked, not blocking): the FLIC **AVI-audio** device
(flic.c's cutscene push path + PS2 pacing loop) and the **WAV file-read**
(`audio.c::load_wav_via_cyg`) still `#ifdef` on PS2 — both are narrow,
timing/format-sensitive, and want their own careful passes.
