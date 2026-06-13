# mk/desktop.mk — the default native build (macOS / Linux / Windows desktop).
# SDL2 from the host (sdl2-config), windowed, mouse + optional gamepad,
# external-media data discovery + native folder picker. Selected when TARGET is
# empty (see the main Makefile's `include mk/$(TGT).mk`).
BIN_NAME := wacki

# STATIC_SDL2=1 → fully self-contained binary: SDL2 + its transitive system
# links baked in, so the user needs no libSDL2 installed (CI release path). The
# default (=0) keeps the dev build dynamic for fast iteration + easier debugging.
ifeq ($(STATIC_SDL2),1)
    SDL_LIB := $(shell $(SDL2_CFG) --static-libs 2>/dev/null)
endif

# Size-optimisation for the static release artefact: -Os trades a few % runtime
# for noticeably smaller code; -ffunction/-fdata-sections + the linker's
# --gc-sections (Linux/mingw) / -dead_strip (macOS) drop unused paths inside
# statically-linked SDL2 (we touch ~half its API); -flto extends that across
# TUs. Off for the default dynamic dev build (faster compile, easier debugging).
ifeq ($(STATIC_SDL2),1)
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
endif

# Windows desktop specifics (build host is MSYS2/mingw, $(OS)=Windows_NT):
#   * a windres-compiled .rc icon resource linked into the .exe so Explorer /
#     Alt-Tab / taskbar show the artwork before the window opens;
#   * -static -static-libgcc for STATIC_SDL2 (drop the mingw / winpthread DLLs);
#   * -mwindows marks the .exe IMAGE_SUBSYSTEM_WINDOWS_GUI so launching it spawns
#     no console box (build WACKI_WINDOWED=0 to keep stderr for dev/CI).
ifeq ($(OS),Windows_NT)
    WACKI_RES := $(DIST)/wacki.res.o
    WINDRES   ?= windres
    ifeq ($(STATIC_SDL2),1)
        LDFLAGS_STATIC := -static -static-libgcc
    endif
    WACKI_WINDOWED ?= 1
    ifeq ($(WACKI_WINDOWED),1)
        LDFLAGS_STATIC += -mwindows
    endif
endif

# macOS: a small Objective-C helper re-titles SDL's default English menu bar
# (App / Window / View) into Polish + adds the "Gra" menu. clang compiles the
# .m as Objective-C inline in the mixed C/.m link; AppKit via -framework Cocoa.
# -framework Security pulls in the SecTranslocate SPI (PlatformMacUntranslocate-
# Path) that undoes Gatekeeper App Translocation so "drop data/ next to
# Wacki.app" works for downloaded bundles. Native-desktop only — a Mac-hosted
# cross build picks a different mk file, so the Cocoa helper never gets dragged
# into an ARM/MIPS link that can't use it.
ifneq ($(OS),Windows_NT)
ifeq ($(shell uname -s 2>/dev/null),Darwin)
    ENGINE_SRCS      += src/platform/macos/macos.m
    MACOS_FRAMEWORKS := -framework Cocoa -framework Security
endif
endif

# Desktop platform HAL: the shared SDL family + the desktop hooks provider
# (display-mode picker / windowed default) + the external-media + folder-picker
# data-root impl.
ENGINE_SRCS += src/platform/sdl/hooks_desktop.c $(SDL_PLATFORM_SRCS) $(SDL_DATAROOT_DESKTOP)
