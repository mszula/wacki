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
- [ ] Step 2 — audio device.
- [ ] Step 3 — FLIC reader.
- [ ] Step 4 — video + input split.
- [ ] Step 5 — lifecycle/CLI.
