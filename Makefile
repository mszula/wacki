# Makefile — builds the reconstructed Wacki engine + tools.
#
# Per-target config is chopped out into mk/<target>.mk — one file per platform,
# mirroring src/platform/<family>/. This top file holds only what's COMMON
# (toolchain, base flags, the platform-agnostic source lists, the generic
# recipes); it then `include`s exactly one mk/<target>.mk based on $(TARGET),
# which sets that platform's CFLAGS / BIN_NAME / size opts / PLATFORM_SRCS.
#
# Adding a platform = a new src/platform/<plat>/ dir + a new mk/<plat>.mk; no
# edits to this file. Build a non-default target with `make TARGET=<plat>`
# (in practice via the tools/build-<plat>.sh Docker wrappers).

CC       ?= cc
SDL2_CFG ?= sdl2-config

# ---- cross-compile toolchain --------------------------------------------- *
# A CROSS_COMPILE prefix retargets the engine compiler (e.g. arm-linux-
# gnueabihf- for Miyoo). HOSTCC stays native — it builds the embed-pe-data code
# generator, which MUST run on the build machine, not the target.
TARGET         ?=
CROSS_COMPILE  ?=
HOSTCC         ?= cc
ifneq ($(CROSS_COMPILE),)
    CC := $(CROSS_COMPILE)gcc
endif

# Base CFLAGS: release-ish with all warnings. -Wpedantic stays on — the two GNU
# extensions we use intentionally (typeof, statement-exprs) are suppressed via
# -Wno-language-extension-token so any NEW warning still surfaces. mk/<target>.mk
# appends its arch + -D switches on top.
#
# -fno-strict-aliasing is REQUIRED, not cosmetic: the Entity struct is accessed
# through multiple type-punned aliases — script writes `*(int32_t *)(eb + 0x48)`
# and the same memory is read as `*(int16_t *)(eb + 0x4A)` (upper 16 bits).
# Under strict-aliasing the compiler may reorder these → the walker step loop
# misses the target-reached comparison and overshoots 1px/tick → cascading
# "actor walks past target" bugs (Fjej weź-kwiatka, Ebek climb glitches).
CFLAGS   ?= -O2 -Wall -Wextra -Wpedantic \
            -Wno-unused-parameter -Wno-pointer-sign \
            -Wno-language-extension-token \
            -fno-strict-aliasing \
            -std=gnu99 -I include

# Version string baked into the startup banner. Defaults to git describe so dev
# builds carry "<last-tag>-<n>-g<sha>[-dirty]" (users paste it into bug reports);
# falls back to "unknown" outside a git checkout (extracted release tarball).
WACKI_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)
CFLAGS        += -DWACKI_VERSION='"$(WACKI_VERSION)"'

# All built binaries land in $(DIST) — gitignored, so artefacts never sit at the
# repo root or next to the sources they're built from.
DIST     := dist
$(DIST):
	@mkdir -p $@

# Windows (MSYS2/mingw) binaries need a `.exe` suffix. `OS` is "Windows_NT"
# under both mingw-make and MSYS2 GNU make; POSIX hosts leave it empty.
ifeq ($(OS),Windows_NT)
    EXE := .exe
else
    EXE :=
endif

# SDL2 link flags. SDL_CFG (--cflags) + SDL_LIB_DYN (--libs) are the dynamic
# defaults shared by every SDL-family target; SDL_LIB tracks them by default and
# is overridden by mk/desktop.mk for STATIC_SDL2 release builds. TARGET=ps2
# brings its own SDL2-PS2 and overrides SDL_CFG/SDL_LIB from tools/build-ps2.sh,
# so these shell-outs (empty when sdl2-config is absent) don't matter there.
STATIC_SDL2 ?= 0
SDL_CFG     := $(shell $(SDL2_CFG) --cflags 2>/dev/null)
SDL_LIB_DYN := $(shell $(SDL2_CFG) --libs 2>/dev/null)
SDL_LIB     := $(SDL_LIB_DYN)

# Knobs mk/<target>.mk may set; default them empty here so a target that doesn't
# care leaves them blank rather than referencing an undefined var.
CFLAGS_SIZE      :=
LDFLAGS_SIZE     :=
LDFLAGS_STATIC   :=
MACOS_FRAMEWORKS :=
WACKI_RES        :=

# ---- platform-agnostic engine sources --------------------------------------
# The core engine — everything that is NOT platform-specific. mk/<target>.mk
# appends its PLATFORM_SRCS (HAL impls + hooks provider) to this list.
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
	src/platform/sdl/platform_sdl.c src/vm/script_obj.c src/vm/parser.c \
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

# Shared platform building blocks referenced by mk/{desktop,miyoo,portmaster}.mk
# (see docs/platform-hal.md). SDL_PLATFORM_SRCS = the SDL2/stdio HAL impls
# shared by desktop + the handhelds: storage (save + file I/O), audio, video,
# system, and the SDL_GameController glue. data-root discovery differs
# desktop-vs-handheld (external-media scanners + folder picker vs a fixed
# SD-card list), so each target links its own data_root_* rather than one
# #ifdef'd file. PS2 brings its own equivalents and pulls none of these.
SDL_PLATFORM_SRCS = src/platform/sdl/save_host.c \
                    src/platform/sdl/file_host.c src/platform/sdl/audio_sdl.c \
                    src/platform/sdl/flic_host.c src/platform/sdl/video_sdl.c \
                    src/platform/sdl/system_sdl.c src/platform/sdl/gamepad_sdl.c
SDL_DATAROOT_DESKTOP  = src/platform/sdl/data_root_desktop.c
SDL_DATAROOT_HANDHELD = src/platform/sdl/data_root_handheld.c

# Select the active platform: include exactly one mk/<target>.mk. TARGET empty
# → desktop. An unknown TARGET fails fast here with a clear message rather than
# a confusing "missing symbol" link error later.
TGT := $(or $(strip $(TARGET)),desktop)
ifeq ($(wildcard mk/$(TGT).mk),)
$(error Unknown TARGET '$(TARGET)' — no mk/$(TGT).mk (valid: desktop, miyoo, portmaster, ps2))
endif
include mk/$(TGT).mk

# ---- debug build (sanitizers) ----------------------------------------------
# `make debug` rebuilds desktop with ASan + UBSan + frame pointers + no opt for
# actionable backtraces; -DWACKI_VERBOSE turns on LOG_TRACE/LOG_DEBUG (compiled
# out of release). Separate binary; always dynamic SDL2 (ASan + static SDL2
# don't mix).
DEBUG_CFLAGS = -O0 -g -fno-omit-frame-pointer \
               -fsanitize=address -fsanitize=undefined \
               -fno-strict-aliasing \
               -Wall -Wextra -Wno-unused-parameter -Wno-pointer-sign \
               -Wno-language-extension-token \
               -DWACKI_VERBOSE \
               -std=gnu99 -I include
DEBUG_LDFLAGS = -fsanitize=address -fsanitize=undefined

# ---- embedded WACKI.EXE data sections --------------------------------------
# tools/embed-pe-data reads WACKI.EXE and emits a generated C source with the
# .rdata + .data raw bytes as a const slice table + blob. The PE-loader resolves
# original VAs against it at runtime — no PE parsing, no file I/O after build.
# Build-time dep: data/WACKI.EXE must exist; the generated file is gitignored.
EMBEDDED_PE_SRC = src/embedded_wacki_pe.c
EMBEDDED_PE_BIN = data/WACKI.EXE
EMBED_PE_TOOL   = $(DIST)/embed-pe-data$(EXE)

# Built with HOSTCC: this tool runs at build time on the build machine; cross-
# compiling it would mean qemu-ing it every time the embedded PE source needs
# regenerating.
$(EMBED_PE_TOOL): tools/embed-pe-data.c | $(DIST)
	$(HOSTCC) -O2 -Wall -I include -o $@ $<

$(EMBEDDED_PE_SRC): $(EMBEDDED_PE_BIN) $(EMBED_PE_TOOL)
	$(EMBED_PE_TOOL) $(EMBEDDED_PE_BIN) $(EMBEDDED_PE_SRC)

# Window icon: embed the 64×64 BMP from assets/icons/ as a C byte array so
# SDL_SetWindowIcon can hand it to the window system (Dock / taskbar / titlebar).
# Generated with `xxd -i` + a hand-written SPDX header; checked into git so
# contributors without xxd can still build. Regenerate via this target after
# changing the BMP.
EMBEDDED_ICON_SRC = src/embedded_icon.c
EMBEDDED_ICON_BIN = assets/icons/wacki-window.bmp

$(EMBEDDED_ICON_SRC): $(EMBEDDED_ICON_BIN)
	@which xxd >/dev/null || { echo "xxd missing — install vim-common"; exit 1; }
	@(printf '/* SPDX-License-Identifier: GPL-3.0-or-later\n * Copyright (C) 2026 Mateusz Szu\xc5\x82\x61\n *\n * src/embedded_icon.c — GENERATED from %s.\n * Loaded as an SDL_Surface via SDL_LoadBMP_RW and handed to\n * SDL_SetWindowIcon in PlatformInit so the engine'\''s window /\n * taskbar / dock entry carry the game artwork.\n */\n\n#include <stddef.h>\n\n' "$(EMBEDDED_ICON_BIN)"; xxd -i -n wacki_icon_bmp $(EMBEDDED_ICON_BIN)) > $@

# ---- tools + tests (host-side, desktop config) -----------------------------
TOOL_SRCS_EXTRACT = tools/dta-extract.c src/depack.c src/archive.c \
                    src/platform/sdl/file_host.c src/heap.c src/log.c
TOOL_SRCS_PKV2    = tools/pkv2-depack.c src/depack.c src/log.c

# Unit tests link only the SDL-free subset of the engine + small mocks for
# symbols normally defined in stubs.c (which #includes SDL). Coverage map + how
# to add a suite: tests/README.md.
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

# Tests reuse the engine's warnings but use a stub SDL.h (tests/sdl_stub) instead
# of the system SDL2 headers; -I tests/sdl_stub MUST come first so the stub wins
# script.c's `#include <SDL.h>`. gnu11 makes _Static_assert first-class.
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

# Cross-target convenience wrappers — build through the per-platform Docker
# image so callers don't need a host-installed cross toolchain. Each script
# invokes `make TARGET=<plat> ...` (with the right sdl2-config / SDL overrides)
# inside its container.
miyoo:
	./tools/build-miyoo.sh

ps2:
	./tools/build-ps2.sh

# Bootable PS2 ISO (SYSTEM.CNF + ELF + game data) so PCSX2 runs it via "Boot
# ISO" with no HostFS config. Builds the ELF first if needed.
ps2-iso:
	./tools/build-ps2-iso.sh

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

# `clean` blows away the whole $(DIST) tree (every built artefact lives there),
# the generated embed source, and stray .o files from ad-hoc compiles.
clean:
	rm -rf $(DIST)
	rm -f $(EMBEDDED_PE_SRC)
	rm -f *.o src/*.o tools/*.o tests/*.o
