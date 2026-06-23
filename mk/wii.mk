# mk/wii.mk — Nintendo Wii homebrew via devkitPPC + libogc + SDL2 portlib.
# Built via tools/build-wii.sh (devkitpro/devkitwii Docker image, matching
# the miyoo/ps2/switch wrapper-script pattern), which produces a .dol
# launchable from the Homebrew Channel.
#
# WACKI_HANDHELD: fullscreen, no display-mode picker, no GUI folder-picker.
# WACKI_WII:     device-specific bits live in src/platform/wii/.
# __wii__:       defined automatically by devkitPPC (same class as __SWITCH__
#                for devkitA64, __ANDROID__ for Android NDK) — used in shared
#                files (system_sdl.c) for the cwd-pin block.
#
# Input model: Wiimote IR is automatically mapped to SDL_MOUSEMOTION +
# SDL_MOUSEBUTTONDOWN (left=A, right=B) by SDL2-wii — the engine's existing
# mouse path in platform_sdl.c requires zero modification. The remaining
# Wiimote buttons (+/-/1/2) arrive as SDL_JOYBUTTONDOWN and are handled by
# src/platform/wii/gamepad_wii.c (implements the same platform_pad_*
# interface as gamepad_sdl.c, but via SDL_Joystick instead of
# SDL_GameController).
#
# Copyright note — WACKI.EXE: same situation as TARGET=switch. EMBEDDED_PE_SRC
# is pre-set to an empty stub; the player's WACKI.EXE is loaded from the SD
# card at runtime via PeLoaderInit() in src/platform/wii/wii.c.

DEVKITPRO ?= /opt/devkitpro
DEVKITPPC ?= $(DEVKITPRO)/devkitPPC

CC       := $(DEVKITPPC)/bin/powerpc-eabi-gcc
BIN_NAME := wacki

CFLAGS += -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float \
          -DWACKI_HANDHELD -DWACKI_WII \
          -I$(DEVKITPRO)/libogc/include \
          -I$(DEVKITPRO)/portlibs/wii/include \
          -I src/platform/sdl

# -Os + section GC keeps the .dol lean; no -flto (conservative, same
# reasoning as miyoo.mk and switch.mk).
CFLAGS_SIZE  := -Os -ffunction-sections -fdata-sections
LDFLAGS_SIZE := -Wl,--gc-sections

# SDL2 portlib for Wii via pkg-config (same pattern as switch.mk).
WII_PKGCONF     := $(DEVKITPRO)/portlibs/wii/bin/powerpc-eabi-pkg-config
WII_PKGCONF_PATH := PKG_CONFIG_PATH=$(DEVKITPRO)/portlibs/wii/lib/pkgconfig
SDL_CFG := $(shell $(WII_PKGCONF_PATH) $(WII_PKGCONF) --cflags sdl2 2>/dev/null)
SDL_LIB := $(shell $(WII_PKGCONF_PATH) $(WII_PKGCONF) --libs sdl2 SDL2_mixer 2>/dev/null) \
           -lfat -logc -lm

LDFLAGS_STATIC := -L$(DEVKITPRO)/libogc/lib/wii \
                   -L$(DEVKITPRO)/portlibs/wii/lib \
                   -L$(DEVKITPRO)/portlibs/ppc/lib \
                   -mrvl -mcpu=750 -meabi -mhard-float

# Same empty-stub pattern as TARGET=switch.
EMBEDDED_PE_SRC := src/platform/wii/embedded_wacki_pe_stub.c

# Platform sources: reuse the SDL family for file I/O, audio, FLIC, video,
# and system init. Use our own gamepad_wii.c (SDL_Joystick) instead of
# gamepad_sdl.c (SDL_GameController) — SDL2-wii exposes the Wiimote as a
# joystick, not a GameController, so the standard SDL_GameController path
# finds nothing. data_root_wii.c provides Homebrew Channel SD paths.
# storage_wii.c adds fatSync() after every save (libfat equivalent of
# Switch's fsdevCommitDevice).
ENGINE_SRCS += src/platform/wii/wii.c \
               src/platform/wii/storage_wii.c \
               src/platform/wii/data_root_wii.c \
               src/platform/wii/gamepad_wii.c \
               src/platform/sdl/file_host.c \
               src/platform/sdl/audio_sdl.c \
               src/platform/sdl/flic_host.c \
               src/platform/sdl/video_sdl.c \
               src/platform/sdl/system_sdl.c

# On Wii we want the linker output to be an .elf (then converted to .dol).
# Override EXE so the top Makefile's engine rule produces dist/wacki.elf.
EXE      := .elf

# ---- .dol packaging --------------------------------------------------------
DIST_DOL := $(DIST)/$(BIN_NAME).dol
ELF2DOL  := $(DEVKITPPC)/bin/elf2dol

all: $(DIST_DOL)

$(DIST_DOL): $(DIST)/$(BIN_NAME)$(EXE)
	$(ELF2DOL) $< $@
