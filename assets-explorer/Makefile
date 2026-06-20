# Makefile — Wacki Assets Explorer (standalone Nuklear + SDL2 tool).
#
# Self-contained subproject: `cd assets-explorer && make` builds
# `dist/wacki-viewer`. It is a THIN layer over the engine — it compiles a
# small SDL-free subset of the engine's own sources straight from ../src
# (the same data core dta-extract uses, plus the asset/graphics/font/flic
# loaders) and links them with this directory's UI code under src/ and the
# vendored single-header libraries under third_party/.
#
# It deliberately does NOT pull in scene/VM/game-loop code; the handful of
# engine symbols those would provide are stubbed in src/stubs.c.
#
# The root engine Makefile keeps a `viewer` target that just delegates here
# (`make -C assets-explorer`), so `make viewer` from the repo root still works.

CC       ?= cc
SDL2_CFG ?= sdl2-config

# Output dir — kept local to the subproject so artefacts never sit beside the
# sources (mirrors the engine's dist/ convention). Gitignored.
DIST := dist

# ---- platform-specific link bits -------------------------------------------
# `OS` is "Windows_NT" under MSYS2/mingw make; uname -s separates Darwin/Linux.
# PLATFORM_LIBS carries everything the link needs beyond SDL2 + our own objects;
# WIN_RES is the compiled Windows icon resource (empty on Linux/macOS).
ifeq ($(OS),Windows_NT)
    EXE := .exe
    # -mwindows: GUI subsystem, so launching the .exe shows NO console window
    #   (same goal as the macOS .app bundle). -Wl,-emainCRTStartup keeps our
    #   plain main() as the entry point under that subsystem (mingw would
    #   otherwise expect WinMain). comdlg32/ole32/shell32/user32 are the Win32
    #   libs tinyfiledialogs calls into for its native Open/Save dialogs.
    PLATFORM_LIBS := -mwindows -Wl,-emainCRTStartup -lcomdlg32 -lole32 -lshell32 -luser32
    # windres compiles tools/wacki.rc (the .ico) into the .exe, so it carries a
    # real icon in Explorer / taskbar / title bar — the Windows counterpart of
    # the macOS .app's AppIcon.icns.
    WINDRES ?= windres
    WIN_RES := $(DIST)/wacki_res.o
else
    EXE :=
    WIN_RES :=
    ifeq ($(shell uname -s),Darwin)
        # Dynamic SDL2 link wants Cocoa; Security mirrors the engine desktop build.
        PLATFORM_LIBS := -framework Cocoa -framework Security
    else
        # Linux/other: -lm in case nuklear/stb reference a libm symbol (harmless
        # if not; on macOS/mingw libm is folded into the base C library).
        PLATFORM_LIBS := -lm
    endif
endif

# SDL2 flags from sdl2-config (empty if it's absent — the link then fails with
# a clear missing-SDL error rather than a confusing one).
#
# Strip -Dmain=SDL_main: mingw's sdl2-config emits it so SDL's WinMain shim can
# rename your main() to SDL_main. We don't want that — the code defines
# SDL_MAIN_HANDLED and keeps a plain main() as the entry point (see PLATFORM_LIBS
# above), so leaving the define in would rename main() out from under the CRT and
# break the link. Harmless no-op on Linux/macOS, where it isn't emitted.
SDL_CFG := $(filter-out -Dmain=SDL_main,$(shell $(SDL2_CFG) --cflags 2>/dev/null))
SDL_LIB := $(shell $(SDL2_CFG) --libs   2>/dev/null)

# ---- sources ---------------------------------------------------------------
# This tool's own UI + asset-preview code.
VIEWER_SRCS = \
	src/main.c src/catalog.c src/hexdump.c \
	src/render.c src/vaudio.c src/vflic.c \
	src/stubs.c

# Engine sources compiled in-place from ../src — the SDL-free data core plus
# the loaders the previews need. NOT copied; kept as the single source of
# truth so the viewer always reflects the real engine behaviour.
ENGINE_SRCS = \
	../src/depack.c ../src/archive.c ../src/platform/sdl/file_host.c \
	../src/heap.c ../src/log.c \
	../src/assets.c ../src/graphics.c ../src/font.c ../src/flic/decoder.c

# Third-party libs that ship a .c (tinyfiledialogs); nuklear / stb / msf_gif are
# header-only and pulled in via the -I flags below. All are fetched on demand by
# `make deps` into the gitignored third_party/ — not committed (see fetch-deps.sh).
THIRDPARTY_SRCS = third_party/tinyfd/tinyfiledialogs.c

SRCS = $(VIEWER_SRCS) $(ENGINE_SRCS) $(THIRDPARTY_SRCS)

# Built single-shot with relaxed flags: Nuklear is a 30k-line public-domain
# header that doesn't survive the engine's -Wextra -Wpedantic, so we drop to
# -Wall + a few targeted suppressions. -fno-strict-aliasing matches the engine
# (its Entity/walker code is type-punned; the loaders we link rely on it).
CFLAGS = -O2 -std=gnu99 \
         -I ../include \
         -I third_party/nuklear -I third_party/stb \
         -I third_party/msf_gif -I third_party/tinyfd \
         -Wall -Wno-unused-function -Wno-sign-compare \
         -Wno-unused-but-set-variable -fno-strict-aliasing

# ---- targets ---------------------------------------------------------------
.PHONY: all clean run deps app

all: $(DIST)/wacki-viewer$(EXE)

$(DIST):
	@mkdir -p $@

# Fetch + checksum-verify the vendored single-header libs into third_party/
# (gitignored, so never committed). Idempotent and fully offline once the files
# are present with the right sha256; see tools/fetch-deps.sh for the pins.
deps:
	@sh tools/fetch-deps.sh

# `deps` is an ORDER-ONLY prerequisite (after the |): it guarantees third_party/
# is populated before we compile, but its phony-ness never forces a relink. The
# fetched files (tinyfiledialogs.c + the headers) are deliberately NOT normal
# prerequisites — listing a not-yet-fetched file there would make `make` fail
# with "no rule to make target" before deps ever runs.
$(DIST)/wacki-viewer$(EXE): $(VIEWER_SRCS) $(ENGINE_SRCS) $(WIN_RES) | $(DIST) deps
	$(CC) $(CFLAGS) $(SDL_CFG) -o $@ $(SRCS) $(WIN_RES) $(SDL_LIB) $(PLATFORM_LIBS)

# Windows icon resource — only built when WIN_RES is non-empty (i.e. on Windows).
# -I tools lets the .rc find wacki.ico; -O coff emits a linkable object.
$(DIST)/wacki_res.o: tools/wacki.rc tools/wacki.ico | $(DIST)
	$(WINDRES) -O coff -I tools tools/wacki.rc -o $@

# Convenience: build + launch against the symlinked ../data assets.
run: all
	./$(DIST)/wacki-viewer$(EXE)

# Package the binary as a macOS .app bundle so it launches from Finder / Dock /
# Spotlight as a normal GUI app — no Terminal/console window (the default when
# you run a bare Unix executable). Output in dist/ (gitignored). macOS only; the
# Dock icon is built (via sips/iconutil) from icon-source.png, or the committed
# tools/app-icon.png fallback — the bundle still builds without either (generic
# icon). Launched this way the app's cwd is /, so it can't see ./data — that's
# exactly when the "locate a .DTA" dialog helps.
APP_NAME := Wacki Assets Explorer
app: all
	@rm -rf "$(DIST)/$(APP_NAME).app"
	@mkdir -p "$(DIST)/$(APP_NAME).app/Contents/MacOS" \
	          "$(DIST)/$(APP_NAME).app/Contents/Resources"
	@cp "$(DIST)/wacki-viewer$(EXE)" "$(DIST)/$(APP_NAME).app/Contents/MacOS/wacki-viewer"
	@cp tools/Info.plist "$(DIST)/$(APP_NAME).app/Contents/Info.plist"
	@sh tools/make-icns.sh "$(DIST)/$(APP_NAME).app/Contents/Resources/AppIcon.icns"
	@echo "built '$(DIST)/$(APP_NAME).app' — double-click it (no console)"

clean:
	rm -rf $(DIST)
