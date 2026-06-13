# mk/ps2.mk — PlayStation 2 via the ps2dev toolchain (mips64r5900el-ps2-elf-gcc,
# run inside the ps2dev Docker image). Built via tools/build-ps2.sh, which also
# supplies the SDL2-PS2 include/lib flags (ports + gsKit + ee lib paths and the
# SDL2 dependency chain) as SDL_CFG / SDL_LIB make-var overrides — so this file
# deliberately does NOT run sdl2-config.
#
# WACKI_HANDHELD: fullscreen, no display-mode picker, no GUI folder-picker.
# WACKI_PS2:      device specifics. Keep the base -O2 (no CFLAGS_SIZE) — the
# 294 MHz Emotion Engine writes every blit pixel, so favour speed over size;
# assets ship on USB/DVD, not a tight cart. The ps2dev specs handle crt0 /
# linkfile, so no extra LDFLAGS glue is needed.
BIN_NAME := wacki-ps2.elf

CFLAGS += -DWACKI_HANDHELD -DWACKI_PS2

# PS2 backend, split per HAL subsystem (src/platform/ps2/); system_ps2.c is also
# the target's hooks provider (analog/USB-mouse pad extras). gamepad_sdl.c is the
# shared SDL_GameController glue for the DualShock — the only sdl/ file PS2 links
# (it brings its own storage / audio / video via libmc / audsrv / gsKit).
ENGINE_SRCS += src/platform/sdl/gamepad_sdl.c \
               src/platform/ps2/system_ps2.c src/platform/ps2/storage_ps2.c \
               src/platform/ps2/audio_ps2.c  src/platform/ps2/video_ps2.c
