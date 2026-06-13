# mk/miyoo.mk — Miyoo Mini Plus + pin-compatible SigmaStar SSD20x handhelds
# (Anbernic RG35XX, Powkiddy RGB30, …). Cortex-A7 + NEON + hardfloat, the
# mmiyoo SDL2 fork. Built via tools/build-miyoo.sh (union-miyoomini-toolchain
# Docker image; its sdl2-config is in PATH). Don't enable LTO — that toolchain's
# ld has mis-emitted thumb thunks under -flto; -Os + section GC gets ~90% of
# the size win anyway.
BIN_NAME := wacki-miyoo

CFLAGS += -mcpu=cortex-a7 -mfpu=neon -mfloat-abi=hard \
          -DWACKI_HANDHELD -DWACKI_MIYOO

# Size matters (128 MB RAM, ~16 MB free for the app — every kB counts even
# with dynamic SDL2).
CFLAGS_SIZE  := -Os -ffunction-sections -fdata-sections
# -ldl: miyoo/miyoo.c dlsym's MI_AO_SetVolume to bind the MStar audio API for
# OnionOS volume restore (alsa-style tinymix controls don't exist on MMP).
LDFLAGS_SIZE := -Wl,--gc-sections -ldl

# Platform HAL: miyoo/miyoo.c is the hooks provider (MI_AO volume + keysym
# buttons) on top of the shared SDL family + the handheld data-root impl.
ENGINE_SRCS += src/platform/miyoo/miyoo.c $(SDL_PLATFORM_SRCS) $(SDL_DATAROOT_HANDHELD)
