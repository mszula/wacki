# Makefile — builds the reconstructed Wacki engine (SDL2 portable) + tools.

CC       ?= cc
SDL2_CFG ?= sdl2-config
# Default build: release-ish with all warnings. -Wpedantic enabled —
# the two GNU extensions we use intentionally (typeof, statement-exprs)
# are suppressed via -Wno-language-extension-token so the build stays
# clean. Any NEW warning surfaces.
CFLAGS   ?= -O2 -Wall -Wextra -Wpedantic \
            -Wno-unused-parameter -Wno-pointer-sign \
            -Wno-language-extension-token \
            -fno-strict-aliasing \
            -std=gnu99 -I include
# NOTE: -fno-strict-aliasing is REQUIRED. The Entity struct is accessed
# through multiple type-punned aliases — script writes `*(int32_t *)(eb +
# 0x48)` and same memory is read as `*(int16_t *)(eb + 0x4A)` (upper 16
# bits of the int32). Under strict-aliasing the compiler may reorder
# these → walker step loop misses the target-reached comparison and
# overshoots by 1px per tick → cascading "actor walks past target"
# bugs (Fjej weź-kwiatka overshoot, Ebek climb glitches).
SDL_CFG  := $(shell $(SDL2_CFG) --cflags 2>/dev/null)
SDL_LIB  := $(shell $(SDL2_CFG) --libs   2>/dev/null)

# T43 — debug build with AddressSanitizer + UBSan. Use `make debug` to
# rebuild with sanitizers + frame pointer + no opt for actionable
# backtraces. Crashes/leaks abort with a full stack.
DEBUG_CFLAGS = -O0 -g -fno-omit-frame-pointer \
               -fsanitize=address -fsanitize=undefined \
               -fno-strict-aliasing \
               -Wall -Wextra -Wno-unused-parameter -Wno-pointer-sign \
               -Wno-language-extension-token \
               -std=gnu99 -I include
DEBUG_LDFLAGS = -fsanitize=address -fsanitize=undefined

# ---- modules ----------------------------------------------------------------
ENGINE_SRCS = \
	src/main.c     src/game.c    src/graphics.c  src/audio.c     \
	src/archive.c  src/depack.c  src/assets.c    src/script.c    \
	src/actor.c    src/actor/intern.c                            \
	src/save.c    src/font.c      src/flic.c                     \
	src/heap.c     src/cygio.c   src/timer.c     src/stubs.c     \
	src/binary_data.c src/pe_loader.c                            \
	src/platform_sdl.c

TOOL_SRCS_EXTRACT = tools/dta-extract.c src/depack.c src/archive.c \
                    src/cygio.c src/heap.c
TOOL_SRCS_PKV2    = tools/pkv2-depack.c src/depack.c

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
	tests/test_ent_ptr_intern.c                                \
	tests/test_engine_stubs.c

TEST_ENGINE_SRCS = \
	src/depack.c    src/archive.c  src/graphics.c \
	src/pe_loader.c src/heap.c     src/cygio.c    \
	src/assets.c    src/font.c     src/save.c     \
	src/binary_data.c src/timer.c  src/script.c   \
	src/stubs.c     src/actor.c    src/actor/intern.c

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
.PHONY: all engine tools clean run debug test
all: engine tools

engine: wacki
wacki: $(ENGINE_SRCS)
	$(CC) $(CFLAGS) $(SDL_CFG) -o $@ $^ $(SDL_LIB)

# Debug build with sanitizers — separate binary so the release build
# stays untouched. Run via ./wacki-debug --headless for CI fuzz runs.
debug: wacki-debug
wacki-debug: $(ENGINE_SRCS)
	$(CC) $(DEBUG_CFLAGS) $(SDL_CFG) -o $@ $^ $(SDL_LIB) $(DEBUG_LDFLAGS)

tools: dta-extract pkv2-depack

dta-extract: $(TOOL_SRCS_EXTRACT)
	$(CC) $(CFLAGS) -o $@ $^

pkv2-depack: $(TOOL_SRCS_PKV2)
	$(CC) $(CFLAGS) -o $@ $^

run: wacki
	./wacki

# Build + run unit tests. Exit non-zero if any test fails.
test: tests/run-tests
	./tests/run-tests

tests/run-tests: $(TEST_SRCS) $(TEST_ENGINE_SRCS)
	$(CC) $(TEST_CFLAGS) -o $@ $^

clean:
	rm -f wacki wacki-debug dta-extract pkv2-depack tests/run-tests \
	      *.o src/*.o tools/*.o tests/*.o
