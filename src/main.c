/*
 * main.c — portable entry point.
 *
 * On Windows the original binary entered via 'entry' @ 0x004161C0 and then
 * WinMain @ 0x004043B0. In this portable build we use a normal int main
 * that forwards to WackiMain; SDL2 supplies SDL_main.
 *
 * Original logic preserved:
 * • CheckCdRomDrive loop (volume label WACKI_1)
 * • CheckDirectSoundVersion (always succeeds in the SDL build)
 * • PlatformInit -> InitializeGameSubsystems -> RunMainGameLoop
 */
#include "wacki.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/* ---- shared globals owned by main.c ------------------------------------ */
HINSTANCE g_hInstance = NULL;
HWND      g_hWnd      = NULL;
BOOL      g_in_foreground = 1;
char      g_cd_path[260]  = "";
uint8_t   g_lmb_clicked   = 0;
uint8_t   g_rmb_clicked   = 0;
uint16_t  g_key_state     = 0;
uint8_t   g_quicksave_request = 0;            /* T53 — F5 latch */
uint8_t   g_quickload_request = 0;            /* T53 — F9 latch */
uint8_t   g_stats_dump_request = 0;           /* T56 — F3 latch */
uint8_t   g_pause_menu_request = 0;           /* T24 — F12 latch */
WackiStats g_stats              = {0};         /* T56 — playthrough stats */

void StatsDump(void)
{
    extern uint32_t g_tick_counter;
    uint32_t elapsed = g_tick_counter - g_stats.boot_tick;
    /* Tick is multimedia timer at ~1kHz on the original; SDL timer at
 * same cadence in port. Display as mm:ss for human readability. */
    uint32_t secs = elapsed / 1000;
    fprintf(stderr,
        "[stats] elapsed=%02u:%02u clicks=%u dialogs=%u komnata-loads=%u "
        "quicksave=%u quickload=%u\n",
        secs / 60, secs % 60,
        g_stats.total_clicks, g_stats.total_dialogs,
        g_stats.total_komnata_loads,
        g_stats.total_quicksaves, g_stats.total_quickloads);
}
int16_t   s_mouse_x       = 0;
int16_t   s_mouse_y       = 0;

/* T45 — headless mode. Skips SDL_CreateWindow / Renderer / Texture and
 * forces SDL_VIDEODRIVER=dummy. Used by CI smoke tests. Set via
 * --headless arg, or WACKI_HEADLESS=1 env. PlatformPresent is a no-op
 * when this is set; PumpEvents still runs (so SDL_Delay sleeps and
 * event queue stays alive). */
int g_headless = 0;

/* T29 — skip frame pacing in cutscene playback (no SDL_Delay between
 * frames). Used by --test-cutscenes batch mode so a 16-file coverage
 * sweep doesn't take 5+ minutes of real time. The decoder + audio
 * queue still operate normally; we just don't sleep to maintain the
 * native frame interval. NOT recommended for interactive playback —
 * audio would race ahead of video and the cutscene would visually
 * blink past in milliseconds. */
int g_no_pacing = 0;

/* T54 — display scaling. The game is natively 640×480 8bpp paletted.
 * On modern displays (HiDPI / 4K) the SDL window can be enlarged via
 * SDL_RenderSetLogicalSize, keeping the framebuffer 640×480 but
 * scaling on present.
 *
 * Set via --scale N / --scaler MODE args or WACKI_SCALE/WACKI_SCALER env.
 * Used by PlatformInit to size the window + set RENDER_SCALE_QUALITY hint. */
int        g_scale_factor = 0;
const char *g_scale_mode  = "nearest";

/* ------------------------------------------------------------------------- *
 * CheckCdRomDrive — portable variant.
 *
 * The original scanned A:..Z: for a drive whose label == "WACKI_1". On
 * macOS we look for a /Volumes/WACKI_1 mount or for a WACKI_PATH env var
 * that points at a directory containing Dane_02.dta.
 * ------------------------------------------------------------------------- */
static int try_open_path(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fclose(fp);
    return 1;
}
static int directory_has_archive(const char *root, const char *needle)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "%s/%s", root, needle);
    if (try_open_path(buf)) return 1;
    /* macOS may upper- or lower-case archive names — try both */
    char upper[64]; size_t i;
    for (i = 0; needle[i] && i < sizeof upper - 1; ++i) {
        char c = needle[i];
        upper[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    upper[i] = 0;
    snprintf(buf, sizeof buf, "%s/%s", root, upper);
    return try_open_path(buf);
}

static int try_root(const char *root)
{
    if (!root || !*root) return 0;
    if (!directory_has_archive(root, "Dane_02.dta")) return 0;
    snprintf(g_cd_path, sizeof g_cd_path, "%s", root);
    return 1;
}

int CheckCdRomDrive(void)
{
    /* 1. explicit override */
    if (try_root(getenv("WACKI_PATH"))) return 2;
    /* 2. ./data */
    if (try_root("./data")) return 2;
    if (try_root("data"))   return 2;
    /* 3. <binary_dir>/data + 4. <binary_dir> */
    char *base = SDL_GetBasePath();
    if (base) {
        size_t blen = strlen(base);
        while (blen > 1 && base[blen-1] == '/') base[--blen] = 0;
        char buf[1024];
        snprintf(buf, sizeof buf, "%s/data", base);
        if (try_root(buf))  { SDL_free(base); return 2; }
        if (try_root(base)) { SDL_free(base); return 2; }
        SDL_free(base);
    }
    /* 5. current dir */
    if (try_root(".")) return 2;
    return 0;
}

/* ------------------------------------------------------------------------- *
 * CheckDirectSoundVersion — always OK on portable build.
 * ------------------------------------------------------------------------- */
int CheckDirectSoundVersion(void) { return 1; }

/* T133 — SIGINT handler. Sets the same quit flag SDL_QUIT would set so
 * the main loop unwinds cleanly (flushes save, releases SDL, etc.).
 * Without this, Ctrl-C in headless CI runs terminates abruptly mid-frame,
 * leaving Wacki.sav.tmp dangling (post T131 atomic write). */
extern int  PlatformShouldQuit(void);
extern void PlatformShutdown(void);
static void sigint_handler(int sig)
{
    (void)sig;
    /* SDL_QUIT push is async-signal-safe enough on Unix; the main loop
 * polls it via PlatformShouldQuit and unwinds normally. */
    SDL_Event ev = {0};
    ev.type = SDL_QUIT;
    SDL_PushEvent(&ev);
}

/* ------------------------------------------------------------------------- *
 * WackiMain — the real entry point.
 *
 * Mirrors WinMain @ 0x004043B0 but uses the SDL platform abstraction.
 * ------------------------------------------------------------------------- */
int WackiMain(int argc, char **argv)
{
    /* T133 — SIGINT (Ctrl-C) → graceful shutdown. Installed first so it
 * covers init phase too. */
    signal(SIGINT, sigint_handler);


    /* T45 — parse --headless before any SDL init. Env var WACKI_HEADLESS=1
 * works equivalently for CI runners that can't easily change argv. */
    uint32_t seed_override = 0;
    int      seed_set      = 0;
    const char *play_avi   = NULL;     /* T29/T30 — single-AVI test mode */
    int test_cutscenes     = 0;        /* T29 — batch test all cutscenes */
    int start_stage        = 0;        /* DEV — --start-stage N: skip menu+intro,
 * mark stages 1..(N-1) as completed, jump
 * straight into stage N gameplay. Useful
 * during testing to avoid replaying earlier
 * stages on every iteration. */
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--headless") == 0) g_headless = 1;
        else if (strcmp(argv[i], "--start-stage") == 0 && i + 1 < argc) {
            start_stage = atoi(argv[++i]);
            if (start_stage < 1 || start_stage > 5) start_stage = 0;
        }
        /* T44 — deterministic smoke harness: --seed N sets WackiRand
 * seed before any rand call. Combined with --headless gives a
 * reproducible smoke run for CI assertions. */
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed_override = (uint32_t)strtoul(argv[++i], NULL, 0);
            seed_set      = 1;
        }
        /* T54 — HiDPI scaling. --scale N enlarges window NxN, keeps
 * framebuffer at native 640×480 via SDL_RenderSetLogicalSize.
 * --scaler nearest|linear|best controls upscale filtering. */
        else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            g_scale_factor = atoi(argv[++i]);
            if (g_scale_factor < 1) g_scale_factor = 1;
            if (g_scale_factor > 8) g_scale_factor = 8;     /* sanity */
        }
        else if (strcmp(argv[i], "--scaler") == 0 && i + 1 < argc) {
            g_scale_mode = argv[++i];
        }
        /* T29/T30 — play a single cutscene then exit. Useful for QA
 * runs without needing to drive the game to the death/intro
 * trigger. Usage: ./wacki --play-avi DANE_14.DTA */
        else if (strcmp(argv[i], "--play-avi") == 0 && i + 1 < argc) {
            play_avi = argv[++i];
        }
        /* T29 — iterate over all known cutscene files (intros, alts,
 * death), play each briefly, report success/failure. Designed
 * for headless CI to catch decoder regressions. */
        else if (strcmp(argv[i], "--test-cutscenes") == 0) {
            test_cutscenes = 1;
            g_no_pacing    = 1;     /* batch mode: don't sleep per frame */
        }
        /* T29 — force-fast decode for cutscenes (no SDL_Delay between
 * frames). Useful for single-AVI verification too. */
        else if (strcmp(argv[i], "--no-pacing") == 0) {
            g_no_pacing = 1;
        }
    }
    {
        const char *env = getenv("WACKI_HEADLESS");
        if (env && *env && *env != '0') g_headless = 1;
    }
    if (start_stage == 0) {
        const char *env = getenv("WACKI_START_STAGE");
        if (env && *env) {
            int n = atoi(env);
            if (n >= 1 && n <= 5) start_stage = n;
        }
    }
    if (start_stage) {
        extern int g_dev_start_stage;
        g_dev_start_stage = start_stage;
        fprintf(stderr, "[wacki] dev mode: jump to stage %d (skip menu+intro)\n",
                start_stage);
    }
    if (!seed_set) {
        const char *env = getenv("WACKI_SEED");
        if (env && *env) { seed_override = (uint32_t)strtoul(env, NULL, 0); seed_set = 1; }
    }
    if (g_scale_factor == 0) {
        const char *env = getenv("WACKI_SCALE");
        if (env && *env) g_scale_factor = atoi(env);
    }
    {
        const char *env = getenv("WACKI_SCALER");
        if (env && *env) g_scale_mode = env;
    }
    if (g_headless) {
        /* Force SDL's null video backend so SDL_Init doesn't try to
 * connect to Cocoa/X11/Wayland. Caller can still override by
 * setting SDL_VIDEODRIVER before launch (rare). */
        setenv("SDL_VIDEODRIVER", "dummy", 0);
        setenv("SDL_AUDIODRIVER", "dummy", 0);
        fprintf(stderr, "[wacki] headless mode\n");
    }
    if (seed_set) {
        WackiRandSeed(seed_override);
        fprintf(stderr, "[wacki] WackiRand seed = 0x%08x\n", seed_override);
    }

    if (CheckCdRomDrive() != 2) {
        fprintf(stderr,
            "Nie znalaz\xC5\x82""em plik\xC3\xB3w Dane_*.dta.\n"
            "U\xC5\xBC""yj jednego z:\n"
            "  • umie\xC5\x9B\xC4\x87 .dta w katalogu  ./data/\n"
            "  • umie\xC5\x9B\xC4\x87 .dta obok binarki ./wacki\n"
            "  • ustaw WACKI_PATH=/sciezka/do/danych\n");
        return 1;
    }
    fprintf(stderr, "[wacki] build " __DATE__ " " __TIME__ "\n");
    fprintf(stderr, "[wacki] data source: %s\n", g_cd_path);

    /* Optional: load WACKI.EXE as a passive PE image so xlat_binary_ptr
 * can resolve original .data/.rdata/.text addresses (verb tables,
 * scripts, etc.) that haven't been manually transcribed into the
 * port's binary_data.c. Missing or unreadable → port falls back to
 * the manually-embedded blobs only (current behaviour). */
    extern int PeLoaderInit(const char *exe_path);
    {
        char p[512];
        snprintf(p, sizeof p, "%s/WACKI.EXE", g_cd_path);
        if (!PeLoaderInit(p)) {
            snprintf(p, sizeof p, "%s/wacki.exe", g_cd_path);
            PeLoaderInit(p);
        }
    }

    if (!PlatformInit(WACKI_SCREEN_W, WACKI_SCREEN_H, "Wacki")) return 1;

    if (!InitializeGameSubsystems()) {
        PlatformShowMessageBox("Wacki",
            "B\xC5\x82\xC4\x85""d podczas uruchomienia programu");
        PlatformShutdown();
        return 1;
    }

    /* T29/T30 — cutscene test modes. Run AFTER InitializeGameSubsystems
 * (which mounts archives + builds stage table) so PlaySceneCutsceneAvi
 * can find the .dta files via the standard try_play_at chain, and
 * BEFORE RunMainGameLoop so we don't load menu state we'll throw
 * away. Both modes exit after their run instead of dropping into
 * the menu. */
    if (play_avi) {
        fprintf(stderr, "[cutscene-test] play '%s'\n", play_avi);
        PlaySceneCutsceneAvi(play_avi);
        fprintf(stderr, "[cutscene-test] done\n");
        PlatformShutdown();
        return 0;
    }
    if (test_cutscenes) {
        /* Walks every known cutscene file. Each call blocks until the
 * AVI finishes (or fails to open); we log start/end so a CI
 * runner can grep for failures. */
        static const char *cutscenes[] = {
            "DANE_10.DTA",          /* engine intro */
            "DANE_11.DTA",          /* alt-stage 5 (credits roll?) */
            "DANE_12.DTA",          /* mid-game */
            "DANE_13.DTA",          /* ? */
            "DANE_14.DTA",          /* death */
            "DANE_21.DTA",          /* stage 1 alt */
            "DANE_22.DTA",          /* stage 1 alt3 */
            "DANE_30.DTA",          /* stage 2 intro */
            "DANE_31.DTA",          /* stage 2 alt */
            "DANE_32.DTA",          /* stage 2 alt3 */
            "DANE_40.DTA",          /* stage 3 intro */
            "DANE_41.DTA",          /* stage 3 alt */
            "DANE_42.DTA",          /* stage 3 alt3 */
            "DANE_50.DTA",          /* stage 4 intro */
            "DANE_51.DTA",          /* stage 4 alt */
            "DANE_52.DTA",          /* stage 4 alt3 */
        };
        fprintf(stderr, "[cutscene-test] batch %zu files\n",
                sizeof cutscenes / sizeof cutscenes[0]);
        for (size_t i = 0; i < sizeof cutscenes / sizeof cutscenes[0]; ++i) {
            fprintf(stderr, "[cutscene-test] [%zu/%zu] %s\n",
                    i + 1, sizeof cutscenes / sizeof cutscenes[0], cutscenes[i]);
            PlaySceneCutsceneAvi(cutscenes[i]);
        }
        fprintf(stderr, "[cutscene-test] done\n");
        PlatformShutdown();
        return 0;
    }

    RunMainGameLoop();

    PlatformShutdown();
    return 0;
}

/* SDL_main forwards here */
int main(int argc, char **argv)
{
    return WackiMain(argc, argv);
}

/* ---- portable replacements for the original Win32 helpers -------------- */

void PumpWin32Messages(void)
{
    PlatformPumpEvents();
}

int HasPendingKey(void)
{
    return (g_key_state & 0xFF) != 0;
}

uint16_t WaitForKey(void)
{
    while ((g_key_state & 0xFF) == 0) {
        PlatformPumpEvents();
        if (PlatformShouldQuit()) return 0x1B; /* ESC */
        SDL_Delay(10);
    }
    uint16_t k = (uint16_t)(g_key_state & 0xFF);
    g_key_state &= 0xFF00;
    return k;
}
