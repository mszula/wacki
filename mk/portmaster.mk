# mk/portmaster.mk — Anbernic & friends via PortMaster. Standard SDL2, ALSA
# audio, real SDL_GameController input. Built via tools/build-portmaster.sh
# (Debian bullseye arm64/armhf Docker images). The arch flags (aarch64 / armhf)
# come from that build environment, so this profile stays arch-agnostic — no
# -mcpu / -mfpu here.
BIN_NAME := wacki

CFLAGS += -DWACKI_HANDHELD -DWACKI_PORTMASTER

# Platform HAL: portmaster/portmaster.c is the hooks provider (fullscreen
# default) on top of the shared SDL family + the handheld data-root impl.
ENGINE_SRCS += src/platform/portmaster/portmaster.c $(SDL_PLATFORM_SRCS) $(SDL_DATAROOT_HANDHELD)
