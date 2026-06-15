# android/app/jni/CMakeLists.txt — native build for the Android port.
#
# This is the Android equivalent of the desktop/handheld mk/<target>.mk files:
# it pins the platform source set (the engine core + the shared SDL HAL + the
# Android-specific HAL) and the compile flags. The NDK toolchain is supplied by
# the Gradle Android plugin via externalNativeBuild; this file only describes
# WHAT to build, never the cross toolchain.
#
# The C source list below mirrors the Makefile's ENGINE_SRCS + SDL_PLATFORM_SRCS
# (with the Android data-root + hooks replacing the desktop/handheld pair). Keep
# the two in sync when adding engine .c files — the project already duplicates
# this list for the unit-test build (TEST_ENGINE_SRCS), so this is consistent
# with how the codebase tracks its source set.
#
# Output: libmain.so (+ libSDL2.so from the SDL submodule). SDLActivity loads
# both; SDL renames our main() to SDL_main (via SDL_main.h) and calls it.

cmake_minimum_required(VERSION 3.22)
project(wacki C)

# Repo root is three levels up: android/app/jni -> <repo>.
get_filename_component(WACKI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

# ---- SDL2 (built from the submodule at jni/SDL) --------------------------- *
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/SDL/CMakeLists.txt")
    message(FATAL_ERROR
        "SDL source not found at ${CMAKE_CURRENT_SOURCE_DIR}/SDL.\n"
        "Fetch it once:\n"
        "    git -C \"${WACKI_ROOT}\" submodule update --init --recursive\n"
        "or clone SDL2 (release-2.x) into that directory. See android/README.md.")
endif()
add_subdirectory(SDL)

# ---- embedded WACKI.EXE PE blob ------------------------------------------ *
# WACKI.EXE's .rdata + .data are linked into the binary (the PE loader resolves
# original VAs against them at runtime). The generator is a HOST tool — it runs
# on the build machine, not the NDK target — so we build + run it with the host
# compiler at configure time. Skipped when the generated source already exists
# (e.g. a prior desktop `make`); delete src/embedded_wacki_pe.c to regenerate.
set(WACKI_EXE "${WACKI_ROOT}/data/WACKI.EXE" CACHE FILEPATH "Path to original WACKI.EXE")
set(EMBED_PE_SRC "${WACKI_ROOT}/src/embedded_wacki_pe.c")
if(NOT EXISTS "${EMBED_PE_SRC}")
    find_program(HOST_CC NAMES cc gcc clang)
    if(NOT HOST_CC)
        message(FATAL_ERROR
            "No host C compiler found to generate ${EMBED_PE_SRC}.\n"
            "Run `make` once in ${WACKI_ROOT} on the desktop to produce it.")
    endif()
    if(NOT EXISTS "${WACKI_EXE}")
        message(FATAL_ERROR
            "WACKI.EXE not found at ${WACKI_EXE}.\n"
            "Copy it from the original disc into <repo>/data/ "
            "(or pass -DWACKI_EXE=/path/to/WACKI.EXE).")
    endif()
    message(STATUS "wacki: generating ${EMBED_PE_SRC} from ${WACKI_EXE} (host tool)")
    execute_process(
        COMMAND ${HOST_CC} -O2 -I${WACKI_ROOT}/include
                -o ${CMAKE_BINARY_DIR}/embed-pe-data
                ${WACKI_ROOT}/tools/embed-pe-data.c
        RESULT_VARIABLE _embed_build_rc)
    if(_embed_build_rc)
        message(FATAL_ERROR "wacki: building embed-pe-data failed (rc=${_embed_build_rc})")
    endif()
    execute_process(
        COMMAND ${CMAKE_BINARY_DIR}/embed-pe-data ${WACKI_EXE} ${EMBED_PE_SRC}
        RESULT_VARIABLE _embed_run_rc)
    if(_embed_run_rc)
        message(FATAL_ERROR "wacki: embed-pe-data failed (rc=${_embed_run_rc})")
    endif()
endif()

# ---- engine + platform sources (mirror of Makefile ENGINE_SRCS) ---------- *
set(WACKI_SRCS
    ${EMBED_PE_SRC}
    ${WACKI_ROOT}/src/embedded_icon.c
    ${WACKI_ROOT}/src/main.c
    ${WACKI_ROOT}/src/data_root.c
    ${WACKI_ROOT}/src/config.c
    ${WACKI_ROOT}/src/game.c
    ${WACKI_ROOT}/src/graphics.c
    ${WACKI_ROOT}/src/audio.c
    ${WACKI_ROOT}/src/archive.c
    ${WACKI_ROOT}/src/depack.c
    ${WACKI_ROOT}/src/assets.c
    ${WACKI_ROOT}/src/vm/main.c
    ${WACKI_ROOT}/src/actor/intern.c
    ${WACKI_ROOT}/src/actor/registration.c
    ${WACKI_ROOT}/src/actor/list.c
    ${WACKI_ROOT}/src/actor/vm.c
    ${WACKI_ROOT}/src/actor/render.c
    ${WACKI_ROOT}/src/actor/alloc.c
    ${WACKI_ROOT}/src/actor/walker.c
    ${WACKI_ROOT}/src/save.c
    ${WACKI_ROOT}/src/font.c
    ${WACKI_ROOT}/src/flic.c
    ${WACKI_ROOT}/src/flic/decoder.c
    ${WACKI_ROOT}/src/heap.c
    ${WACKI_ROOT}/src/timer.c
    ${WACKI_ROOT}/src/stubs.c
    ${WACKI_ROOT}/src/binary_data.c
    ${WACKI_ROOT}/src/pe_loader.c
    ${WACKI_ROOT}/src/log.c
    ${WACKI_ROOT}/src/platform/sdl/platform_sdl.c
    ${WACKI_ROOT}/src/vm/script_obj.c
    ${WACKI_ROOT}/src/vm/parser.c
    ${WACKI_ROOT}/src/util/rng.c
    ${WACKI_ROOT}/src/hud/panel.c
    ${WACKI_ROOT}/src/hud/inventory.c
    ${WACKI_ROOT}/src/hud/items.c
    ${WACKI_ROOT}/src/scene/click_queue.c
    ${WACKI_ROOT}/src/scene/hit_test.c
    ${WACKI_ROOT}/src/scene/mask_list.c
    ${WACKI_ROOT}/src/scene/navigation.c
    ${WACKI_ROOT}/src/scene/actor_walk.c
    ${WACKI_ROOT}/src/scene/stage.c
    ${WACKI_ROOT}/src/scene/bg_mask.c
    ${WACKI_ROOT}/src/scene/spawn.c
    ${WACKI_ROOT}/src/scene/komnata.c
    ${WACKI_ROOT}/src/scene/preload.c
    ${WACKI_ROOT}/src/scene/hud_paint.c
    ${WACKI_ROOT}/src/scene/frame_tick.c
    ${WACKI_ROOT}/src/scene/dispatch.c
    ${WACKI_ROOT}/src/hud/cursor.c
    ${WACKI_ROOT}/src/scene/walkability.c
    ${WACKI_ROOT}/src/scene/scene_input.c
    ${WACKI_ROOT}/src/scene/play_loop.c
    ${WACKI_ROOT}/src/scene/komnata_scene.c
    ${WACKI_ROOT}/src/audio/sound_queue.c
    ${WACKI_ROOT}/src/audio/cutscene.c
    ${WACKI_ROOT}/src/audio/sfx.c
    ${WACKI_ROOT}/src/audio/music_stream.c
    ${WACKI_ROOT}/src/script_bridge/palette.c
    ${WACKI_ROOT}/src/script_bridge/entity.c
    ${WACKI_ROOT}/src/text/balloon.c
    ${WACKI_ROOT}/src/text/dialog.c
    ${WACKI_ROOT}/src/anim/resolver.c
    ${WACKI_ROOT}/src/util/screenshot.c
    ${WACKI_ROOT}/src/anim/paint_primitives.c
    ${WACKI_ROOT}/src/anim/alpha_blit.c
    ${WACKI_ROOT}/src/menu/chapter_select.c
    ${WACKI_ROOT}/src/menu/slot_picker.c
    ${WACKI_ROOT}/src/menu/options.c
    ${WACKI_ROOT}/src/menu/menu_loop.c
    ${WACKI_ROOT}/src/menu/main_menu.c
    ${WACKI_ROOT}/src/menu/port_attribution.c
    # ---- shared SDL HAL (Makefile SDL_PLATFORM_SRCS) ----
    ${WACKI_ROOT}/src/platform/sdl/save_host.c
    ${WACKI_ROOT}/src/platform/sdl/file_host.c
    ${WACKI_ROOT}/src/platform/sdl/audio_sdl.c
    ${WACKI_ROOT}/src/platform/sdl/flic_host.c
    ${WACKI_ROOT}/src/platform/sdl/video_sdl.c
    ${WACKI_ROOT}/src/platform/sdl/system_sdl.c
    ${WACKI_ROOT}/src/platform/sdl/gamepad_sdl.c
    # ---- Android platform HAL (replaces data_root_* + hooks_* ) ----
    ${WACKI_ROOT}/src/platform/android/hooks_android.c
    ${WACKI_ROOT}/src/platform/android/data_root_android.c
)

# SDLActivity loads "SDL2" then "main" → this must be libmain.so.
add_library(main SHARED ${WACKI_SRCS})

target_include_directories(main PRIVATE ${WACKI_ROOT}/include)

# Version string baked into the startup banner (git describe, or "android").
execute_process(
    COMMAND git -C ${WACKI_ROOT} describe --tags --always --dirty
    OUTPUT_VARIABLE WACKI_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _git_rc)
if(_git_rc OR NOT WACKI_VERSION)
    set(WACKI_VERSION "android")
endif()
target_compile_definitions(main PRIVATE WACKI_VERSION="${WACKI_VERSION}")

# -fno-strict-aliasing is REQUIRED (the Entity struct is type-punned through
# multiple aliases — see the Makefile comment). gnu99 for typeof + stmt-exprs.
target_compile_options(main PRIVATE
    -O2 -Wall -Wextra
    -Wno-unused-parameter -Wno-pointer-sign -Wno-language-extension-token
    -fno-strict-aliasing
    -std=gnu99)

# SDL2 (shared) + SDL2main (the Android SDL_main bootstrap glue).
target_link_libraries(main SDL2 SDL2main)
