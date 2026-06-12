# Makefile — builds the reconstructed Wacki engine (SDL2 portable) + tools.

CC       ?= cc
SDL2_CFG ?= sdl2-config

# ---- cross-compile knobs ------------------------------------------------- *
#
# TARGET=miyoo cross-compiles for Miyoo Mini Plus and pin-compatible
# handhelds (Cortex-A7 armv7l, hardfloat, NEON). Used together with a
# CROSS_COMPILE prefix:
#
#   make TARGET=miyoo CROSS_COMPILE=arm-linux-gnueabihf-
#
# In practice you run it through tools/build-miyoo.sh which sets up the
# union-miyoomini-toolchain Docker image with the right sdl2-config.
TARGET         ?=
CROSS_COMPILE  ?=
# HOSTCC is the compiler for build-time tools (embed-pe-data) which
# MUST run on the build machine, not the target. When cross-compiling
# we still need a host gcc/cc to produce a runnable code generator;
# default to whatever cc resolved to before CROSS_COMPILE overrode it.
HOSTCC         ?= cc
ifneq ($(CROSS_COMPILE),)
    CC := $(CROSS_COMPILE)gcc
endif

# Default build: release-ish with all warnings. -Wpedantic enabled —
# the two GNU extensions we use intentionally (typeof, statement-exprs)
# are suppressed via -Wno-language-extension-token so the build stays
# clean. Any NEW warning surfaces.
CFLAGS   ?= -O2 -Wall -Wextra -Wpedantic \
            -Wno-unused-parameter -Wno-pointer-sign \
            -Wno-language-extension-token \
            -fno-strict-aliasing \
            -std=gnu99 -I include

# Version string baked into the binary's startup banner. Defaults to
# git describe so dev builds carry "<last-tag>-<n>-g<sha>[-dirty]";
# users running a release can read it back from the wacki.log + paste
# it into bug reports. Falls back to "unknown" if we're outside a git
# checkout (release tarball extracted somewhere weird, etc.).
WACKI_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)
CFLAGS        += -DWACKI_VERSION='"$(WACKI_VERSION)"'

# Handheld build profiles. WACKI_HANDHELD is the generic "this is a
# handheld" switch (fullscreen, no display-mode picker, gamepad → cursor
# input, SD-card data paths). WACKI_MIYOO layers the Miyoo Mini Plus
# device specifics on top (the mmiyoo SDL2 fork's MI_AO volume restore +
# its keysym button mapping in platform_miyoo.c).
#
#   TARGET=miyoo       Cortex-A7 + NEON + hardfloat, mmiyoo SDL2.
#                      Don't enable LTO — the Miyoo toolchain's ld has
#                      mis-emitted thumb thunks under -flto; -Os + section
#                      GC still gets ~90% of the size win.
#   TARGET=portmaster  Anbernic & friends (PortMaster). Standard SDL2,
#                      stereo ALSA audio, real SDL_GameController input.
#                      Arch flags (aarch64 / armhf) come from the build
#                      environment (tools/build-portmaster.sh), so this
#                      profile stays architecture-agnostic.
ifeq ($(TARGET),miyoo)
    CFLAGS  += -mcpu=cortex-a7 -mfpu=neon -mfloat-abi=hard \
               -DWACKI_HANDHELD -DWACKI_MIYOO
    BIN_NAME := wacki-miyoo
else ifeq ($(TARGET),portmaster)
    CFLAGS  += -DWACKI_HANDHELD -DWACKI_PORTMASTER
    BIN_NAME := wacki
else ifeq ($(TARGET),ps2)
    # PlayStation 2 via the ps2dev toolchain (mips64r5900el-ps2-elf-gcc,
    # run inside the ps2dev Docker image). WACKI_HANDHELD: fullscreen, no
    # display-mode picker, no GUI folder-picker. WACKI_PS2: device
    # specifics. The SDL2-PS2 include/lib flags (ports + gsKit + ee lib
    # paths and the SDL2 dependency chain) are supplied by
    # tools/build-ps2.sh as SDL_CFG / SDL_LIB overrides.
    CFLAGS  += -DWACKI_HANDHELD -DWACKI_PS2
    BIN_NAME := wacki-ps2.elf
else
    BIN_NAME := wacki
endif
# NOTE: -fno-strict-aliasing is REQUIRED. The Entity struct is accessed
# through multiple type-punned aliases — script writes `*(int32_t *)(eb +
# 0x48)` and same memory is read as `*(int16_t *)(eb + 0x4A)` (upper 16
# bits of the int32). Under strict-aliasing the compiler may reorder
# these → walker step loop misses the target-reached comparison and
# overshoots by 1px per tick → cascading "actor walks past target"
# bugs (Fjej weź-kwiatka overshoot, Ebek climb glitches).
SDL_CFG  := $(shell $(SDL2_CFG) --cflags 2>/dev/null)

# STATIC_SDL2=1 selects a fully self-contained engine binary: SDL2 +
# its transitive system links are baked into the binary so the user
# doesn't need libSDL2 installed. CI release builds set this; the
# default (=0) keeps the developer build dynamic for fast iteration
# + easier debugging.
#
# `SDL_LIB_DYN`  always dynamic — used by the sanitized debug build
#                (ASan + static SDL2 don't mix).
# `SDL_LIB`      tracks STATIC_SDL2 — used by the release engine.
STATIC_SDL2 ?= 0
SDL_LIB_DYN := $(shell $(SDL2_CFG) --libs 2>/dev/null)
ifeq ($(STATIC_SDL2),1)
    SDL_LIB := $(shell $(SDL2_CFG) --static-libs 2>/dev/null)
else
    SDL_LIB := $(SDL_LIB_DYN)
endif

# All built binaries land in $(DIST). The directory is gitignored —
# build artefacts never sit at the repo root or alongside the source
# files they're built from.
DIST     := dist
$(DIST):
	@mkdir -p $@

# Windows (MSYS2/mingw) binaries need a `.exe` suffix. The `OS`
# env var is set to "Windows_NT" by both mingw-make and GNU make
# under MSYS2; all POSIX hosts leave it empty.
ifeq ($(OS),Windows_NT)
    EXE := .exe
else
    EXE :=
endif

# Windows resource: assets/icons/wacki.rc references wacki.ico — the
# multi-size (16/32/48/64/128/256) Win32 icon. windres compiles the
# .rc into a COFF object that gets linked directly into the .exe so
# Explorer + Alt-Tab + taskbar show the artwork even before the
# binary starts (SDL_SetWindowIcon takes over once the window opens).
# POSIX hosts skip this — there's no equivalent embed-file-icon-in-
# ELF pattern.
ifeq ($(OS),Windows_NT)
    WACKI_RES := $(DIST)/wacki.res.o
    WINDRES   ?= windres
endif

# Extra link flags for fully-static Windows: drop the mingw gcc /
# winpthread DLLs that linker pulls in by default. POSIX builds
# don't need this — the static SDL2 already lists every system
# library through `sdl2-config --static-libs`.
ifeq ($(STATIC_SDL2),1)
ifeq ($(OS),Windows_NT)
    LDFLAGS_STATIC := -static -static-libgcc
else
    LDFLAGS_STATIC :=
endif
else
    LDFLAGS_STATIC :=
endif

# Windows subsystem: -mwindows marks the .exe as IMAGE_SUBSYSTEM_
# WINDOWS_GUI so Explorer launches it WITHOUT spawning a console
# window — players double-click wacki.exe and only see the game.
# Without this, the default mingw build emits a CUI (console)
# subsystem binary and Windows pops up a cmd.exe-style black box
# every launch.
#
# Entry point doesn't need a rewrite: SDL2 ships libSDL2main.a with
# a WinMain that calls our int main() — sdl2-config --libs adds it
# to the link line.
#
# We intentionally lose stderr/stdout output as a side-effect (no
# attached console). For dev / CI runs that need the log, build
# WACKI_WINDOWED=0 to skip -mwindows and keep the console.
ifeq ($(OS),Windows_NT)
WACKI_WINDOWED ?= 1
ifeq ($(WACKI_WINDOWED),1)
    LDFLAGS_STATIC += -mwindows
endif
endif

# Size-optimisation knobs for the release artefact. -Os trades a few
# % runtime perf for noticeably smaller code (fine for a 1997 point-
# and-click). -ffunction-sections / -fdata-sections + the linker's
# --gc-sections (Linux/mingw) or -dead_strip (macOS) drop unused
# code paths inside statically-linked SDL2 — a big win since we
# touch ~half of SDL2's API. -flto lets the linker see across TUs
# for further dead-code elimination.
#
# Always on for STATIC_SDL2=1 (release path); never on for the
# default dev build (faster compile, easier to debug).
# Size opts apply to STATIC_SDL2=1 (release artefact) AND TARGET=miyoo
# (always, since 128 MB RAM + ~16 MB free for app on Miyoo Mini Plus
# means every kB counts even with dynamic SDL2). Miyoo also skips
# -flto — the union toolchain's ld + linker plugins have been known
# to mis-emit thumb thunks under -flto.
ifeq ($(TARGET),miyoo)
    CFLAGS_SIZE  := -Os -ffunction-sections -fdata-sections
    # -ldl: platform_sdl.c uses dlsym(RTLD_DEFAULT, "MI_AO_SetVolume") to
    # bind directly to the MStar audio API for OnionOS volume restore
    # (alsa-style tinymix controls don't exist on MMP — kernel exposes
    # MI_AO instead, which the mmiyoo SDL2 backend has already loaded).
    LDFLAGS_SIZE := -Wl,--gc-sections -ldl
else ifeq ($(TARGET),ps2)
    # Keep the base -O2 — the 294 MHz Emotion Engine writes every blit
    # pixel, so favour speed over size (assets ship on USB/DVD, not a
    # tight cart). The ps2dev toolchain specs handle crt0/linkfile, so no
    # extra LD glue is needed.
    CFLAGS_SIZE  :=
    LDFLAGS_SIZE :=
else ifeq ($(STATIC_SDL2),1)
    CFLAGS_SIZE := -Os -ffunction-sections -fdata-sections -flto
    UNAME_S := $(shell uname -s 2>/dev/null)
    ifeq ($(OS),Windows_NT)
        LDFLAGS_SIZE := -flto -Wl,--gc-sections
    else ifeq ($(UNAME_S),Linux)
        LDFLAGS_SIZE := -flto -Wl,--gc-sections
    else ifeq ($(UNAME_S),Darwin)
        LDFLAGS_SIZE := -flto -Wl,-dead_strip
    else
        LDFLAGS_SIZE := -flto
    endif
else
    CFLAGS_SIZE  :=
    LDFLAGS_SIZE :=
endif

# T43 — debug build with AddressSanitizer + UBSan. Use `make debug` to
# rebuild with sanitizers + frame pointer + no opt for actionable
# backtraces. Crashes/leaks abort with a full stack.
# -DWACKI_VERBOSE enables LOG_TRACE / LOG_DEBUG (compiled out in
# release for zero overhead).
DEBUG_CFLAGS = -O0 -g -fno-omit-frame-pointer \
               -fsanitize=address -fsanitize=undefined \
               -fno-strict-aliasing \
               -Wall -Wextra -Wno-unused-parameter -Wno-pointer-sign \
               -Wno-language-extension-token \
               -DWACKI_VERBOSE \
               -std=gnu99 -I include
DEBUG_LDFLAGS = -fsanitize=address -fsanitize=undefined

# ---- embedded WACKI.EXE data sections --------------------------------------
# tools/embed-pe-data reads WACKI.EXE and emits a generated C source
# with the .rdata + .data raw bytes as a const slice table + blob.
# The engine PE-loader resolves original VAs against this table at
# runtime — no PE parsing, no file I/O after build. Other sections
# (.text x86 code, .idata, .rsrc) are skipped (we never reference
# them). Build-time dep: data/WACKI.EXE must exist; generated file is
# gitignored.
EMBEDDED_PE_SRC = src/embedded_wacki_pe.c
EMBEDDED_PE_BIN = data/WACKI.EXE
EMBED_PE_TOOL   = $(DIST)/embed-pe-data$(EXE)

# Built with HOSTCC because this tool runs at build time on the build
# machine; cross-compiling it for the target would mean trying to
# qemu it (or worse, run an ARM binary natively on x86_64) every
# time the embedded PE source needs regenerating.
$(EMBED_PE_TOOL): tools/embed-pe-data.c | $(DIST)
	$(HOSTCC) -O2 -Wall -I include -o $@ $<

$(EMBEDDED_PE_SRC): $(EMBEDDED_PE_BIN) $(EMBED_PE_TOOL)
	$(EMBED_PE_TOOL) $(EMBEDDED_PE_BIN) $(EMBEDDED_PE_SRC)

# Window icon: embed the 64×64 BMP from assets/icons/wacki-window.bmp
# as a C byte array so SDL_SetWindowIcon can hand it to whichever
# window system the user runs (macOS Dock / Linux taskbar / Windows
# titlebar). Generator is plain `xxd -i` plus a hand-written SPDX
# header; checked into git so contributors without xxd installed can
# still build. Regenerate by running this target after changing the
# BMP source. */
EMBEDDED_ICON_SRC = src/embedded_icon.c
EMBEDDED_ICON_BIN = assets/icons/wacki-window.bmp

$(EMBEDDED_ICON_SRC): $(EMBEDDED_ICON_BIN)
	@which xxd >/dev/null || { echo "xxd missing — install vim-common"; exit 1; }
	@(printf '/* SPDX-License-Identifier: GPL-3.0-or-later\n * Copyright (C) 2026 Mateusz Szu\xc5\x82\x61\n *\n * src/embedded_icon.c — GENERATED from %s.\n * Loaded as an SDL_Surface via SDL_LoadBMP_RW and handed to\n * SDL_SetWindowIcon in PlatformInit so the engine'\''s window /\n * taskbar / dock entry carry the game artwork.\n */\n\n#include <stddef.h>\n\n' "$(EMBEDDED_ICON_BIN)"; xxd -i -n wacki_icon_bmp $(EMBEDDED_ICON_BIN)) > $@

# ---- modules ----------------------------------------------------------------
ENGINE_SRCS = \
	$(EMBEDDED_PE_SRC) \
	$(EMBEDDED_ICON_SRC) \
	src/main.c     src/data_root.c src/config.c \
	src/game.c    src/graphics.c  src/audio.c     \
	src/archive.c  src/depack.c  src/assets.c    src/vm/main.c   \
	src/actor/intern.c    src/actor/registration.c \
	src/actor/list.c src/actor/vm.c \
	src/actor/render.c src/actor/alloc.c \
	src/actor/walker.c                            \
	src/save.c    src/font.c      src/flic.c   src/flic/decoder.c \
	src/heap.c     src/timer.c     src/stubs.c                    \
	src/binary_data.c src/pe_loader.c src/log.c                  \
	src/platform_sdl.c src/vm/script_obj.c src/vm/parser.c       \
	src/util/rng.c                                               \
	src/hud/panel.c src/hud/inventory.c src/hud/items.c          \
	src/scene/click_queue.c src/scene/hit_test.c src/scene/mask_list.c \
	src/scene/navigation.c src/scene/actor_walk.c src/scene/stage.c \
	src/scene/bg_mask.c src/scene/spawn.c src/scene/komnata.c     \
	src/scene/preload.c src/scene/hud_paint.c src/scene/frame_tick.c \
	src/scene/dispatch.c src/hud/cursor.c                            \
	src/scene/walkability.c src/scene/scene_input.c                  \
	src/scene/play_loop.c   src/scene/komnata_scene.c                \
	src/audio/sound_queue.c   src/audio/cutscene.c                \
	src/audio/sfx.c           src/audio/music_stream.c           \
	src/script_bridge/palette.c src/script_bridge/entity.c        \
	src/text/balloon.c src/text/dialog.c                          \
	src/anim/resolver.c src/util/screenshot.c                     \
	src/anim/paint_primitives.c src/anim/alpha_blit.c             \
	src/menu/chapter_select.c src/menu/slot_picker.c                 \
	src/menu/options.c        src/menu/menu_loop.c                   \
	src/menu/main_menu.c      src/menu/port_attribution.c

# Platform-specific glue is appended only for the matching TARGET.
# src/platform_miyoo.c carries the OnionOS/MStar bits (MI_AO volume
# restore) and pulls in libdl — kept out of desktop builds because
# desktop linkers wouldn't find dlsym + libmi_ao.so doesn't exist.
# src/platform_portmaster.c carries the Anbernic SDL_GameController →
# cursor/click input glue.
# Platform HAL implementations, composed per TARGET (see docs/platform-hal.md).
# The SDL family (desktop + the handhelds) shares the stdio/SDL storage impls
# — save (file + atomic rename), data-root discovery (external media / SD card
# + the native folder picker) and the file-I/O shim (newlib stdio). PS2
# provides those same interfaces from platform_ps2.c (libmc save + fileXio
# devices + fileXio file I/O), so it omits SDL_STORAGE_SRCS.
SDL_STORAGE_SRCS = src/platform/sdl/save_host.c src/platform/sdl/data_root_host.c \
                   src/platform/sdl/file_host.c
ifeq ($(TARGET),miyoo)
    ENGINE_SRCS += src/platform_miyoo.c $(SDL_STORAGE_SRCS)
else ifeq ($(TARGET),portmaster)
    ENGINE_SRCS += src/platform_portmaster.c $(SDL_STORAGE_SRCS)
else ifeq ($(TARGET),ps2)
    # Shared SDL_GameController glue: the DualShock 2 reaches the cursor
    # through the same pad path as PortMaster/Vita. platform_ps2.c adds the
    # PS2 device glue + the bring-up trace breadcrumbs read over PINE, and
    # the storage HAL impls (libmc save + fileXio data-root discovery).
    ENGINE_SRCS += src/platform_portmaster.c src/platform_ps2.c
else
    ENGINE_SRCS += $(SDL_STORAGE_SRCS)
endif

# macOS desktop gets a small Objective-C helper that re-titles SDL's
# default English menu bar (App / Window / View) into Polish. clang
# compiles the .m as Objective-C from its extension even inside the
# single mixed C/.m link command; AppKit is pulled in via -framework
# Cocoa. -framework Security pulls in the SecTranslocate SPI used to undo
# Gatekeeper App Translocation (see PlatformMacUntranslocatePath), so "drop
# data/ next to Wacki.app" works for downloaded bundles. Desktop-only: gated
# on an EMPTY TARGET so a cross-build hosted on a Mac (the Miyoo/PortMaster
# toolchains, or a native ps2/vita build) doesn't drag the Cocoa helper +
# frameworks into an ARM/MIPS link that can't use them.
ifeq ($(TARGET),)
ifneq ($(OS),Windows_NT)
ifeq ($(shell uname -s 2>/dev/null),Darwin)
    ENGINE_SRCS      += src/platform_macos.m
    MACOS_FRAMEWORKS := -framework Cocoa -framework Security
endif
endif
endif

TOOL_SRCS_EXTRACT = tools/dta-extract.c src/depack.c src/archive.c \
                    src/platform/sdl/file_host.c src/heap.c src/log.c
TOOL_SRCS_PKV2    = tools/pkv2-depack.c src/depack.c src/log.c

# ---- tests (no SDL) -----------------------------------------------------
# Unit tests link only the SDL-free subset of the engine + small mocks
# for symbols normally defined in stubs.c (which #includes SDL).
# Coverage map + how to add a suite: see tests/README.md.
TEST_SRCS = \
	tests/test_main.c              tests/test_entity_layout.c \
	tests/test_rng.c               tests/test_pkv2.c          \
	tests/test_archive.c           tests/test_save.c          \
	tests/test_save_io.c           tests/test_assets.c        \
	tests/test_font.c              tests/test_walker.c        \
	tests/test_graphics.c          tests/test_pe_loader.c     \
	tests/test_binary_data.c       tests/test_timer.c         \
	tests/test_script_vm.c         tests/test_real_vm.c       \
	tests/test_vm_control_flow.c                              \
	tests/test_vm_script_parser.c  tests/test_vm_call_stack.c \
	tests/test_vm_more_ops.c       tests/test_heap_cygio.c    \
	tests/test_vm_walk_anim.c      tests/test_vm_dialog.c     \
	tests/test_vm_misc_ops.c       tests/test_vm_safety.c     \
	tests/test_vm_game_over.c      tests/test_per_entity_vm.c \
	tests/test_archive_extended.c  tests/test_pe_loader_malformed.c \
	tests/test_assets_rle.c        tests/test_font_color.c    \
	tests/test_save_slot.c         tests/test_vm_with_pe.c    \
	tests/test_vm_show_text_bind.c tests/test_graphics_alpha.c \
	tests/test_vm_wait_break.c     tests/test_archive_lifecycle.c \
	tests/test_pe_loader_lifecycle.c tests/test_vm_corner_cases.c \
	tests/test_save_io_extended.c                              \
	tests/test_inventory.c         tests/test_sound_queue.c   \
	tests/test_panel_hit_test.c    tests/test_click_hit_test.c \
	tests/test_per_entity_vm_real.c                            \
	tests/test_click_queue.c       tests/test_update_registration.c \
	tests/test_ent_ptr_intern.c    tests/test_sampl_parser.c   \
	tests/test_komnata_load.c                                  \
	tests/test_engine_stubs.c

TEST_ENGINE_SRCS = \
	tests/embedded_wacki_pe_stub.c                 \
	src/depack.c    src/archive.c  src/graphics.c \
	src/pe_loader.c src/heap.c     src/platform/sdl/file_host.c \
	src/assets.c    src/font.c     src/save.c     \
	src/platform/sdl/save_host.c                   \
	src/binary_data.c src/timer.c  src/vm/main.c  src/log.c \
	src/vm/script_obj.c src/vm/parser.c          \
	src/util/rng.c                                \
	src/hud/panel.c src/hud/inventory.c src/hud/items.c \
	src/scene/click_queue.c src/scene/hit_test.c src/scene/mask_list.c \
	src/scene/navigation.c src/scene/actor_walk.c src/scene/stage.c \
	src/scene/bg_mask.c src/scene/spawn.c src/scene/komnata.c     \
	src/audio/sound_queue.c   src/audio/sfx.c                    \
	src/script_bridge/palette.c src/script_bridge/entity.c        \
	src/text/balloon.c src/text/dialog.c                          \
	src/anim/resolver.c                                           \
	src/stubs.c     src/actor/intern.c    src/actor/registration.c \
	src/actor/list.c src/actor/vm.c \
	src/actor/render.c src/actor/alloc.c \
	src/actor/walker.c src/anim/alpha_blit.c

# Tests reuse the engine's CFLAGS but use a stub SDL.h (tests/sdl_stub)
# instead of the system SDL2 headers. -I tests/sdl_stub MUST come first
# so the stub is picked up by script.c's `#include <SDL.h>`. Bump to
# gnu11 so `_Static_assert` is a first-class citizen (no
# -Wc11-extensions noise).
TEST_CFLAGS = -O2 -Wall -Wextra -Wpedantic \
              -Wno-unused-parameter -Wno-pointer-sign \
              -Wno-language-extension-token \
              -fno-strict-aliasing \
              -std=gnu11 -I tests/sdl_stub -I include -I tests

# ---- targets ----------------------------------------------------------------
.PHONY: all engine tools clean run debug test miyoo ps2 ps2-iso
all: engine tools

engine: $(DIST)/$(BIN_NAME)$(EXE)
$(DIST)/$(BIN_NAME)$(EXE): $(ENGINE_SRCS) $(WACKI_RES) | $(DIST)
	$(CC) $(CFLAGS) $(CFLAGS_SIZE) $(SDL_CFG) -o $@ $(ENGINE_SRCS) $(WACKI_RES) $(SDL_LIB) $(LDFLAGS_STATIC) $(LDFLAGS_SIZE) $(MACOS_FRAMEWORKS)

ifeq ($(OS),Windows_NT)
$(WACKI_RES): assets/icons/wacki.rc assets/icons/wacki.ico | $(DIST)
	$(WINDRES) --include-dir=assets/icons -i $< -o $@
endif

# Convenience target: build through the union-miyoomini-toolchain
# Docker image so callers don't need a host-installed ARM cross compiler.
# Inside the container sdl2-config is in PATH and points at the cross-
# built static SDL2 the toolchain ships with.
miyoo:
	./tools/build-miyoo.sh

# Build the PS2 ELF through the ps2dev Docker image (mips64r5900el-ps2-
# elf-gcc + SDL2-PS2). Produces dist/wacki-ps2.elf. Experiment: drop it
# on a USB stick and launch via uLaunchELF, or run it in PCSX2.
ps2:
	./tools/build-ps2.sh

# Build a bootable PS2 ISO (SYSTEM.CNF + ELF + game data) so it runs in
# PCSX2 via "Boot ISO" with no HostFS config. Builds the ELF first if
# needed. Produces dist/wacki-ps2.iso.
ps2-iso:
	./tools/build-ps2-iso.sh

# Debug build with sanitizers — separate binary so the release build
# stays untouched. Run via $(DIST)/wacki-debug --headless for CI fuzz
# runs. Sanitizers + static linking don't mix, so the debug build
# always uses the dynamic SDL2.
debug: $(DIST)/wacki-debug$(EXE)
$(DIST)/wacki-debug$(EXE): $(ENGINE_SRCS) | $(DIST)
	$(CC) $(DEBUG_CFLAGS) $(SDL_CFG) -o $@ $(ENGINE_SRCS) $(SDL_LIB_DYN) $(DEBUG_LDFLAGS) $(MACOS_FRAMEWORKS)

tools: $(DIST)/dta-extract$(EXE) $(DIST)/pkv2-depack$(EXE)

$(DIST)/dta-extract$(EXE): $(TOOL_SRCS_EXTRACT) | $(DIST)
	$(CC) $(CFLAGS) -o $@ $(TOOL_SRCS_EXTRACT)

$(DIST)/pkv2-depack$(EXE): $(TOOL_SRCS_PKV2) | $(DIST)
	$(CC) $(CFLAGS) -o $@ $(TOOL_SRCS_PKV2)

run: $(DIST)/$(BIN_NAME)$(EXE)
	$(DIST)/$(BIN_NAME)$(EXE)

# Build + run unit tests. Exit non-zero if any test fails.
test: $(DIST)/run-tests$(EXE)
	$(DIST)/run-tests$(EXE)

$(DIST)/run-tests$(EXE): $(TEST_SRCS) $(TEST_ENGINE_SRCS) | $(DIST)
	$(CC) $(TEST_CFLAGS) -o $@ $(TEST_SRCS) $(TEST_ENGINE_SRCS)

# `clean` blows away the whole $(DIST) tree (every built artefact
# lives there now). Also removes the generated embed source +
# stray .o files left behind by ad-hoc compiles.
clean:
	rm -rf $(DIST)
	rm -f $(EMBEDDED_PE_SRC)
	rm -f *.o src/*.o tools/*.o tests/*.o
