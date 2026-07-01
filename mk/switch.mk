# mk/switch.mk — Nintendo Switch homebrew (devkitA64 + libnx + SDL2 portlib).

DEVKITPRO ?= /opt/devkitpro
DEVKITA64 ?= $(DEVKITPRO)/devkitA64

CC       := $(DEVKITA64)/bin/aarch64-none-elf-gcc
BIN_NAME := wacki

CFLAGS += -D__SWITCH__ -DWACKI_HANDHELD -DWACKI_SWITCH \
          -march=armv8-a -mtune=cortex-a57 -mtp=soft \
          -ftls-model=local-exec \
          -I$(DEVKITPRO)/libnx/include \
          -I$(DEVKITPRO)/portlibs/switch/include \
          -I src/platform/sdl

CFLAGS_SIZE  := -Os -ffunction-sections -fdata-sections
LDFLAGS_SIZE := -Wl,--gc-sections

SWITCH_PKGCONF      := $(DEVKITPRO)/portlibs/switch/bin/aarch64-none-elf-pkg-config
SWITCH_PKGCONF_PATH := PKG_CONFIG_PATH=$(DEVKITPRO)/portlibs/switch/lib/pkgconfig
SDL_CFG := $(shell $(SWITCH_PKGCONF_PATH) $(SWITCH_PKGCONF) --cflags sdl2 2>/dev/null)
SDL_LIB := $(shell $(SWITCH_PKGCONF_PATH) $(SWITCH_PKGCONF) --libs sdl2 SDL2_mixer 2>/dev/null) \
           -lEGL -lglapi -ldrm_nouveau -lnx -lm

LDFLAGS_STATIC := -L$(DEVKITPRO)/libnx/lib \
                   -L$(DEVKITPRO)/portlibs/switch/lib \
                   -specs=$(DEVKITPRO)/libnx/switch.specs

# Jesli data/WACKI.EXE istnieje (CI z sekretem / lokalne budowanie),
# uzywamy standardowego embed-pe-data. W przeciwnym razie - pusty stub.
ifeq ($(wildcard data/WACKI.EXE),)
    EMBEDDED_PE_SRC := src/platform/switch/embedded_wacki_pe_stub.c
endif

# Switch uzywa wlasnego gamepad_switch.c zamiast gamepad_sdl.c -
# remapping A/B pozostaje wylacznie w plikach Switch-specyficznych.
ENGINE_SRCS += src/platform/switch/switch.c \
               src/platform/switch/storage_switch.c \
               src/platform/switch/data_root_switch.c \
               src/platform/switch/gamepad_switch.c \
               src/platform/sdl/file_host.c \
               src/platform/sdl/audio_sdl.c \
               src/platform/sdl/flic_host.c \
               src/platform/sdl/video_sdl.c \
               src/platform/sdl/system_sdl.c

# ---- .nro packaging ------------------------------------------------------
SWITCH_ICON := assets/icons/wacki-switch.jpg
SWITCH_NACP := $(DIST)/wacki.nacp
SWITCH_NRO  := $(DIST)/wacki.nro
NACPTOOL    := $(DEVKITPRO)/tools/bin/nacptool
ELF2NRO     := $(DEVKITPRO)/tools/bin/elf2nro

all: $(SWITCH_NRO)

$(SWITCH_NACP): | $(DIST)
	$(NACPTOOL) --create "Wacki: Kosmiczna rozgrywka" "mszula" "$(WACKI_VERSION)" $(SWITCH_NACP)

$(SWITCH_NRO): $(DIST)/$(BIN_NAME)$(EXE) $(SWITCH_NACP)
	$(ELF2NRO) $(DIST)/$(BIN_NAME)$(EXE) $(SWITCH_NRO) \
	    --icon=$(SWITCH_ICON) --nacp=$(SWITCH_NACP)
