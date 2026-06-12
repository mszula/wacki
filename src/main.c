/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/main.c — portable entry point.
 *
 * Responsibilities:
 *
 *   - main() → WackiMain(): parse argv + env, install SIGINT handler,
 *     find the data directory, open SDL, run the cutscene-test mode
 *     OR drop into RunMainGameLoop.
 *
 *   - StatsDump(): F3 stats log line (callers: F3 key handler).
 *
 *   - Module-owned globals: input latches (g_lmb_clicked / g_rmb_
 *     clicked / g_key_state / g_*_request), playthrough stats
 *     (g_stats), display knobs (g_headless / g_no_pacing /
 *     g_scale_factor / g_scale_mode), mouse coords (s_mouse_x /
 *     s_mouse_y), data root (g_data_root).
 *
 *   - Win32-equivalent shims: PumpEvents / HasPendingKey /
 *     WaitForKey — the engine's call sites still use the original
 *     Win32-style names, so this file provides portable aliases. */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/system.h"

#include <SDL.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bring-up tracing for the PS2 port: ps2_mark() leaves a breadcrumb in a
 * global array read over PINE (ps2sdk's stderr doesn't reach PCSX2's
 * console). No-op on every other target. Defined in src/platform_ps2.c. */
#ifdef WACKI_PS2
extern void ps2_mark(unsigned int);
#define WK_PS2_MARK(x) ps2_mark((unsigned int)(x))
#else
#define WK_PS2_MARK(x) ((void)0)
#endif

/* ---- constants ---------------------------------------------------- */

/* FindDataRoot return value when the data root was found. The
 * original Win32 enum was {0 = no drive, 1 = non-Wacki CD, 2 = Wacki
 * CD found}; the port collapses to {0 = not found, 2 = found} and
 * keeps the 2 sentinel so any caller comparing against the legacy
 * value still works. */
#define DATA_ROOT_FOUND 2

/* CLI clamps. */
#define DEV_START_STAGE_CLAMP_MIN 1
#define DEV_START_STAGE_CLAMP_MAX 5
#define SCALE_FACTOR_MAX 8

/* WaitForKey poll interval — wakes 100×/s to re-check the key latch +
 * the platform-quit flag. Fast enough that ESC feels responsive,
 * slow enough not to spin the CPU. */
#define KEY_WAIT_POLL_MS 10

/* g_key_state stores the latched SDL key sym in the low byte; the
 * top byte is reserved for modifier flags (currently unused). */
#define KEY_STATE_KEYCODE_MASK 0xFF
#define KEY_STATE_MOD_MASK 0xFF00

/* Mirrors VK_ESCAPE in platform_sdl.c — we return this when the
 * platform requests quit so the caller can treat shutdown as a
 * synthetic ESC press. */
#define VK_ESCAPE_KEYCODE 0x1B

/* Tick is the multimedia timer at ~1 kHz on the original; SDL_GetTicks
 * matches that cadence so 1000 ticks ≈ 1 wall-clock second. */
#define TICKS_PER_SECOND 1000

/* ---- module-owned globals ---------------------------------------- */

char g_data_root[260] = "";
int16_t s_mouse_x = 0;
int16_t s_mouse_y = 0;

uint8_t g_lmb_clicked = 0;
uint8_t g_rmb_clicked = 0;
uint16_t g_key_state = 0;

uint8_t g_quicksave_request = 0;  /* T53 — F5 latch */
uint8_t g_quickload_request = 0;  /* T53 — F9 latch */
uint8_t g_stats_dump_request = 0; /* T56 — F3 latch */
uint8_t g_pause_menu_request = 0; /* T24 — F12 latch */

WackiStats g_stats = {0}; /* T56 — playthrough stats */

/* T45 — headless mode forces SDL dummy video + audio drivers. Set via
 * --headless or WACKI_HEADLESS=1. PlatformPresent is a no-op while
 * set; PumpEvents still runs so SDL_Delay sleeps and the event queue
 * stays alive. */
int g_headless = 0;

/* T29 — skip per-frame SDL_Delay in cutscene playback. Used by
 * --test-cutscenes so a 16-file sweep doesn't take 5+ minutes of
 * real time. NOT recommended for interactive playback. */
int g_no_pacing = 0;

/* T54 — display scaling. The framebuffer is always 640×480 8-bpp; the
 * SDL window can be enlarged Nx via SDL_RenderSetLogicalSize with the
 * filter selected by --scaler (nearest / linear / best). */
int g_scale_factor = 0;
const char *g_scale_mode = "nearest";

/* Fullscreen flag — set by --fullscreen / -f CLI or WACKI_FULLSCREEN
 * env. PlatformInit consumes it to switch SDL_WINDOW_FULLSCREEN_DESKTOP
 * on; F11 also toggles at runtime. No-op on WACKI_HANDHELD (Miyoo is
 * always full-screen since there's no windowing system). */
int g_fullscreen = 0;

/* ---- stats dump (F3) -------------------------------------------- */

void StatsDump(void)
{
    extern uint32_t g_tick_counter;
    uint32_t elapsed = g_tick_counter - g_stats.boot_tick;
    uint32_t secs = elapsed / TICKS_PER_SECOND;
    LOG_INFO("stats", "elapsed=%02u:%02u clicks=%u dialogs=%u komnata-loads=%u "
                      "quicksave=%u quickload=%u",
             secs / 60, secs % 60, g_stats.total_clicks, g_stats.total_dialogs, g_stats.total_komnata_loads, g_stats.total_quicksaves, g_stats.total_quickloads);
}

/* ---- data-root discovery ---------------------------------------- *
 *
 * Implementation lives in src/data_root.c. The original FindDataRoot
 * scanned A:..Z: for a drive whose volume label was WACKI_1; the
 * portable variant searches env → cwd → ./data → adjacent to the
 * binary → external media → handheld card paths → native folder
 * picker as a GUI fallback. See data_root.c for the full ladder. */

/* ---- SIGINT handler --------------------------------------------- */

/* T133 — Ctrl-C → graceful shutdown. Pushes the same SDL_QUIT event
 * SDL would push on a window close, so the main loop unwinds normally
 * (flushes save, releases SDL, etc.). Without this, Ctrl-C in
 * headless CI runs terminated abruptly mid-frame and left Wacki.sav.
 * tmp dangling. */
static void sigint_handler(int sig)
{
    (void)sig;
    SDL_Event ev = {0};
    ev.type = SDL_QUIT;
    SDL_PushEvent(&ev);
}

/* ---- CLI parsing ------------------------------------------------ */

typedef struct CliArgs
{
    int start_stage;      /* 1..5 = dev jump, 0 = normal flow */
    const char *play_avi; /* single-AVI test mode (--play-avi) */
    int test_cutscenes;   /* batch cutscene sweep (--test-cutscenes) */
} CliArgs;

static void parse_cli_args(int argc, char **argv, CliArgs *out)
{
    *out = (CliArgs){0};

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--headless") == 0)
        {
            g_headless = 1;
        }
        else if (strcmp(argv[i], "--start-stage") == 0 && i + 1 < argc)
        {
            int n = atoi(argv[++i]);
            if (n < DEV_START_STAGE_CLAMP_MIN ||
                n > DEV_START_STAGE_CLAMP_MAX)
                n = 0;
            out->start_stage = n;
        }
        /* T54 — HiDPI scaling. --scale N enlarges window NxN, --scaler
         * nearest|linear|best controls upscale filtering. */
        else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc)
        {
            g_scale_factor = atoi(argv[++i]);
            if (g_scale_factor < 1)
                g_scale_factor = 1;
            if (g_scale_factor > SCALE_FACTOR_MAX)
            {
                g_scale_factor = SCALE_FACTOR_MAX;
            }
        }
        else if (strcmp(argv[i], "--scaler") == 0 && i + 1 < argc)
        {
            g_scale_mode = argv[++i];
        }
        /* Fullscreen toggle — SDL borderless desktop fullscreen so the
         * window covers the active display without changing the desktop
         * resolution. F11 also toggles in-game. */
        else if (strcmp(argv[i], "--fullscreen") == 0 ||
                 strcmp(argv[i], "-f") == 0)
        {
            g_fullscreen = 1;
        }
        /* T29 / T30 — single-AVI test mode + batch cutscene sweep. */
        else if (strcmp(argv[i], "--play-avi") == 0 && i + 1 < argc)
        {
            out->play_avi = argv[++i];
        }
        else if (strcmp(argv[i], "--test-cutscenes") == 0)
        {
            out->test_cutscenes = 1;
            g_no_pacing = 1;
        }
        /* T29 — force-fast decode for cutscenes (no SDL_Delay). */
        else if (strcmp(argv[i], "--no-pacing") == 0)
        {
            g_no_pacing = 1;
        }
        /* Log verbosity. `-v` / `--verbose` enables LOG_TRACE +
         * LOG_DEBUG (requires -DWACKI_VERBOSE at build time too);
         * `-q` / `--quiet` drops to WARN+. Default is INFO. */
        else if (strcmp(argv[i], "-v") == 0 ||
                 strcmp(argv[i], "--verbose") == 0)
        {
            g_log_min_level = WL_TRACE;
        }
        else if (strcmp(argv[i], "-q") == 0 ||
                 strcmp(argv[i], "--quiet") == 0)
        {
            g_log_min_level = WL_WARN;
        }
    }
}

/* Apply env-var fallbacks for the flags the CLI didn't touch. Lets CI
 * runners that can't easily change argv use WACKI_HEADLESS / WACKI_
 * START_STAGE / etc. instead. */
static void apply_env_overrides(CliArgs *args)
{
    const char *env;

    env = getenv("WACKI_HEADLESS");
    if (env && *env && *env != '0')
        g_headless = 1;

    if (args->start_stage == 0)
    {
        env = getenv("WACKI_START_STAGE");
        if (env && *env)
        {
            int n = atoi(env);
            if (n >= DEV_START_STAGE_CLAMP_MIN &&
                n <= DEV_START_STAGE_CLAMP_MAX)
                args->start_stage = n;
        }
    }

    if (g_scale_factor == 0)
    {
        env = getenv("WACKI_SCALE");
        if (env && *env)
            g_scale_factor = atoi(env);
    }

    env = getenv("WACKI_SCALER");
    if (env && *env)
        g_scale_mode = env;

    if (!g_fullscreen)
    {
        env = getenv("WACKI_FULLSCREEN");
        if (env && *env && *env != '0')
            g_fullscreen = 1;
    }
}

/* Apply the parsed args' early-side effects: dev-stage jump, headless
 * SDL drivers. Runs after env merge but before SDL init so the env-
 * forced drivers stick. */
static void apply_early_cli_effects(const CliArgs *args)
{
    if (args->start_stage)
    {
        extern int g_dev_start_stage;
        g_dev_start_stage = args->start_stage;
        LOG_INFO("wacki", "dev mode: jump to stage %d (skip menu+intro)", args->start_stage);
    }
    if (g_headless)
    {
        /* Force SDL's null video / audio backends so SDL_Init doesn't
         * try to connect to Cocoa / X11 / Wayland. Caller can still
         * override by setting SDL_VIDEODRIVER before launch.
         *
         * SDL_setenv is portable across every platform SDL supports
         * (Win32, POSIX, …) and handles the "set if not already set"
         * branch via SDL_getenv. Plain setenv() isn't in MSVCRT, so
         * mingw rejects it. */
        if (!SDL_getenv("SDL_VIDEODRIVER"))
            SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
        if (!SDL_getenv("SDL_AUDIODRIVER"))
            SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
        LOG_INFO("wacki", "headless mode");
    }
}

/* ---- cutscene test modes --------------------------------------- */

/* Every cutscene file that ships in DANE_*.DTA. Order matches stage
 * order so a batch sweep walks the engine's natural playback sequence. */
static const char *const k_known_cutscenes[] = {
    "DANE_10.DTA",
    "DANE_11.DTA",
    "DANE_12.DTA",
    "DANE_13.DTA",
    "DANE_14.DTA",
    "DANE_21.DTA",
    "DANE_22.DTA",
    "DANE_30.DTA",
    "DANE_31.DTA",
    "DANE_32.DTA",
    "DANE_40.DTA",
    "DANE_41.DTA",
    "DANE_42.DTA",
    "DANE_50.DTA",
    "DANE_51.DTA",
    "DANE_52.DTA",
};
#define KNOWN_CUTSCENE_COUNT \
    (sizeof k_known_cutscenes / sizeof k_known_cutscenes[0])

static int run_cutscene_test_mode(const CliArgs *args)
{
    if (args->play_avi)
    {
        LOG_TRACE("cutscene-test", "play '%s'", args->play_avi);
        PlaySceneCutsceneAvi(args->play_avi);
        LOG_TRACE("cutscene-test", "done");
        return 1;
    }
    if (args->test_cutscenes)
    {
        LOG_TRACE("cutscene-test", "batch %u files",
                  (unsigned)KNOWN_CUTSCENE_COUNT);
        for (size_t i = 0; i < KNOWN_CUTSCENE_COUNT; ++i)
        {
            LOG_TRACE("cutscene-test", "[%u/%u] %s",
                      (unsigned)(i + 1), (unsigned)KNOWN_CUTSCENE_COUNT,
                      k_known_cutscenes[i]);
            PlaySceneCutsceneAvi(k_known_cutscenes[i]);
        }
        LOG_TRACE("cutscene-test", "done");
        return 1;
    }
    return 0;
}

/* ---- WackiMain + main ------------------------------------------- */

int WackiMain(int argc, char **argv)
{
    /* Earliest process setup, before config/data: PS2 IOP fileio bring-up
     * (+ its first trace breadcrumb), Win32 stderr→logfile, macOS app-support
     * cwd. The system HAL picks the right one per platform (a no-op on bare
     * Linux / handheld). */
    plat_system_early_init();

    /* SIGINT first so it covers init failures too. */
    signal(SIGINT, sigint_handler);

    /* Load persisted display prefs FIRST so they form the baseline;
     * the CLI parser + env overrides below then layer on top (an
     * explicit --scale / --fullscreen always wins over wacki.cfg).
     * Also sets g_config_first_run when no wacki.cfg exists, which
     * PlatformInit reads to decide whether to show the one-time
     * display-mode picker. */
    ConfigLoad();

    CliArgs args;
    parse_cli_args(argc, argv, &args);
    apply_env_overrides(&args);
    apply_early_cli_effects(&args);

    WK_PS2_MARK(0x0002u);                 /* reached data-root search */
    int wk_dr = FindDataRoot();
    WK_PS2_MARK(0x3000u | (unsigned)wk_dr);   /* dr: 2=found, 0=not found */
    if (wk_dr != DATA_ROOT_FOUND)
    {
        /* All search ladders + the GUI folder picker exhausted. Tell
         * the user what we tried and bail. SDL_ShowSimpleMessageBox
         * pops a native dialog on every host (NSAlert on macOS,
         * MessageBox() on Windows, GTK/Xlib message on Linux) so a
         * Finder/Explorer double-click without data files still
         * leaves the user with a clear "what now" message instead of
         * a silent exit. */
        const char *msg =
            "Nie znalazłem plików Dane_*.dta. Szukałem w:\n\n"
            "  • katalogu uruchomienia + ./data/\n"
            "  • obok binarki (lub w pakiecie .app)\n"
            "  • na napędach CD / podłączonych dyskach\n"
            "  • zmiennej WACKI_PATH\n\n"
            "Skopiuj pliki z oryginalnej płyty do dowolnego z tych "
            "miejsc i uruchom grę ponownie.";
        LOG_INFO("log", "%s", msg);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                                 "Wacki — brak danych", msg, NULL);
        return 1;
    }
#ifndef WACKI_VERSION
#define WACKI_VERSION "unknown"
#endif
    LOG_INFO("wacki", "Wacki port %s (build " __DATE__ " " __TIME__ ")",
             WACKI_VERSION);
    LOG_INFO("wacki", "data source: %s", g_data_root);

    /* WACKI.EXE's .rdata + .data sections are linked into the binary
     * (see include/wacki/embedded_exe.h); PeLoaderRead resolves
     * against them with no runtime init. */

    int wk_pi = PlatformInit(WACKI_SCREEN_W, WACKI_SCREEN_H, WACKI_WINDOW_TITLE);
    WK_PS2_MARK(0x4000u | (unsigned)(wk_pi & 1));   /* PlatformInit ok? */
    if (!wk_pi)
        return 1;

    int wk_ig = InitializeGameSubsystems();
    WK_PS2_MARK(0x5000u | (unsigned)(wk_ig & 1));   /* subsystems ok? */
    if (!wk_ig)
    {
        PlatformShowMessageBox("Wacki",
                               "B\xC5\x82\xC4\x85"
                               "d podczas uruchomienia programu");
        PlatformShutdown();
        return 1;
    }

    /* Cutscene-only test modes exit after their run instead of dropping
     * into the main menu. Both modes run AFTER InitializeGameSubsystems
     * so PlaySceneCutsceneAvi has the archive mounted. */
    if (run_cutscene_test_mode(&args))
    {
        PlatformShutdown();
        return 0;
    }

    WK_PS2_MARK(0x6000u);                 /* entering the main game loop */
    RunMainGameLoop();
    PlatformShutdown();
    return 0;
}

int main(int argc, char **argv)
{
    int rc = WackiMain(argc, argv);
    /* Final teardown: a no-op on most platforms (we return rc); on PS2 this
     * stamps the exit code and parks the EE forever so PINE can read the
     * trace (a returned main() reboots PCSX2 to the BIOS browser). */
    plat_system_exit(rc);
    return rc;
}

/* ---- portable replacements for the original Win32 helpers ------- */

void PumpEvents(void)
{
    PlatformPumpEvents();
}

int HasPendingKey(void)
{
    return (g_key_state & KEY_STATE_KEYCODE_MASK) != 0;
}

uint16_t WaitForKey(void)
{
    while ((g_key_state & KEY_STATE_KEYCODE_MASK) == 0)
    {
        PlatformPumpEvents();
        if (PlatformShouldQuit())
            return VK_ESCAPE_KEYCODE;
        SDL_Delay(KEY_WAIT_POLL_MS);
    }
    uint16_t k = (uint16_t)(g_key_state & KEY_STATE_KEYCODE_MASK);
    g_key_state &= KEY_STATE_MOD_MASK;
    return k;
}
