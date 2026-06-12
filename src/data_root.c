/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/data_root.c — FindDataRoot: locate the directory holding
 * Dane_*.dta. Replaces a previous shorter version that lived in
 * main.c; grew large enough (per-OS scanners + GUI fallback) to
 * warrant its own module.
 *
 * Search order:
 *
 *   1. WACKI_PATH env var
 *   2. ./
 *   3. ./data
 *
 *   4. Adjacent to the binary (SDL_GetBasePath() and that/data):
 *        Linux/Windows: <bindir> and <bindir>/data
 *        macOS .app:    Wacki.app/Contents/Resources and .../data
 *        macOS bare:    same as Linux/Windows
 *
 *   5. macOS .app neighbor — the folder Wacki.app itself sits in,
 *      and that folder/data. Common UX: drag Wacki.app to Desktop,
 *      drop Dane_*.dta next to it.
 *
 *   6. External media / CD drives:
 *        macOS:   /Volumes/{*}/ and /Volumes/{*}/data/
 *        Windows: A:..Z: and X:/data
 *        Linux:   /media/$USER/{*}/, /mnt/{*}/, /run/media/$USER/{*}/
 *
 *   7. Handheld (WACKI_HANDHELD):
 *        /mnt/SDCARD/, /mnt/SDCARD/data/,
 *        /mnt/SDCARD/wacki/, /mnt/SDCARD/wacki/data/
 *
 *   8. GUI fallback — native folder picker (osascript on macOS,
 *      PowerShell on Windows, zenity/kdialog on Linux). User selects
 *      a folder; we re-probe it for Dane_02.dta. Skipped on handheld
 *      (no keyboard).
 *
 * First hit wins. Probe file is hard-coded to Dane_02.dta — the only
 * archive every original Wacki CD shipped that the engine references
 * at startup. Case-insensitive: lowercase tried first, then the same
 * basename uppercased (for CDs written with DOS-conventional
 * DANE_02.DTA).
 *
 * On success, g_data_root is committed to the absolute or relative
 * root that matched. */

#include "wacki.h"
#include "wacki/log.h"

#include <SDL.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__) || defined(__linux__) || defined(WACKI_HANDHELD)
#include <dirent.h>
#include <sys/stat.h>
#endif

/* DATA_ROOT_FOUND must match the constant in src/main.c. */
#define DATA_ROOT_FOUND                     2

#define ARCHIVE_PROBE_PATH_BYTES            1024
#define UPPERCASE_NAME_BYTES                64
#define DATA_PROBE_FILENAME                 "Dane_02.dta"

extern char g_data_root[260];

/* ---- probe helpers ----------------------------------------------- */

static int try_open_path(const char *path)
{
#ifdef WACKI_PS2
    /* PS2 reads through fileXio, not newlib fopen (which reaches no
     * device) — so the probe must use the same path so it and the real
     * archive reads (cygio.c) agree. */
    extern void ps2_normalize_path(const char *, char *, size_t);
    extern int  fileXioOpen(const char *, int);
    extern int  fileXioClose(int);
    char fixed[ARCHIVE_PROBE_PATH_BYTES];
    ps2_normalize_path(path, fixed, sizeof fixed);
    int fd = fileXioOpen(fixed, 0x0001 /* O_RDONLY */);
    if (fd < 0) return 0;
    fileXioClose(fd);
    return 1;
#else
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fclose(fp);
    return 1;
#endif
}

static int directory_has_archive(const char *root, const char *needle)
{
    char buf[ARCHIVE_PROBE_PATH_BYTES];

    snprintf(buf, sizeof buf, "%s/%s", root, needle);
    if (try_open_path(buf)) return 1;

    char   upper[UPPERCASE_NAME_BYTES];
    size_t i;
    for (i = 0; needle[i] && i < sizeof upper - 1; ++i) {
        char c = needle[i];
        upper[i] = (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
    }
    upper[i] = 0;
    snprintf(buf, sizeof buf, "%s/%s", root, upper);
    return try_open_path(buf);
}

/* Accept `root` as the data directory if it contains the probe file.
 * Commits g_data_root and returns 1; otherwise leaves g_data_root
 * untouched and returns 0. */
static int try_root(const char *root)
{
    if (!root || !*root) return 0;
    if (!directory_has_archive(root, DATA_PROBE_FILENAME)) return 0;
    snprintf(g_data_root, sizeof g_data_root, "%s", root);
    return 1;
}

/* Try `<root>` and then `<root>/data`. Common pattern that lets a
 * caller probe both "bare files" and the "files in data/ subfolder"
 * conventions in one call. */
static int try_root_and_data(const char *root)
{
    if (!root || !*root) return 0;
    if (try_root(root)) return 1;
    char buf[ARCHIVE_PROBE_PATH_BYTES];
    snprintf(buf, sizeof buf, "%s/data", root);
    return try_root(buf);
}

/* ---- macOS .app neighbor ----------------------------------------- */

#ifdef __APPLE__
/* Given a path inside a .app bundle (typically the Resources/ dir
 * from SDL_GetBasePath()), walk parents until we hit a component
 * ending in ".app". Return the directory that .app sits in. Caller
 * frees the returned malloc()'d string. Returns NULL when the input
 * isn't inside a bundle — caller should skip the neighbor probe. */
static char *macos_dirname_of_app(const char *base)
{
    if (!base || !*base) return NULL;
    char *cur = strdup(base);
    if (!cur) return NULL;
    for (;;) {
        /* Strip trailing slash + last component. */
        size_t n = strlen(cur);
        while (n > 1 && cur[n - 1] == '/') cur[--n] = 0;
        char *slash = strrchr(cur, '/');
        if (!slash || slash == cur) { free(cur); return NULL; }
        size_t last_len = strlen(slash + 1);
        if (last_len >= 4 &&
            strcmp(slash + 1 + last_len - 4, ".app") == 0)
        {
            *slash = 0;   /* cut off ".../Wacki.app" → ".../" */
            return cur;
        }
        *slash = 0;       /* climb one level up */
    }
}
#endif

/* ---- per-OS external-media scanners ------------------------------ */

#ifdef __APPLE__
/* /Volumes/{*}/ and /Volumes/{*}/data/. macOS mounts every external
 * disk (USB, CD, DVD, network share) under /Volumes — same place
 * Finder shows them. */
static int scan_macos_volumes(void)
{
    DIR *d = opendir("/Volumes");
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char buf[ARCHIVE_PROBE_PATH_BYTES];
        snprintf(buf, sizeof buf, "/Volumes/%s", de->d_name);
        if (try_root_and_data(buf)) {
            LOG_INFO("data-root", "matched on /Volumes/%s", de->d_name);
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}
#endif

#ifdef _WIN32
#include <windows.h>
/* Iterate A:..Z: looking for Dane_02.dta at root or in data/.
 * GetLogicalDrives returns a bitmask of drives that currently exist
 * (mounted volumes + CD drives even with no media), so the probe is
 * O(present-drives), not O(26). */
static int scan_windows_drives(void)
{
    DWORD mask = GetLogicalDrives();
    for (int letter = 0; letter < 26; ++letter) {
        if (!(mask & (1u << letter))) continue;
        char root[8];
        snprintf(root, sizeof root, "%c:/", 'A' + letter);
        /* Skip drives without a probeable archive quickly; try_root_and
         * _data will already short-circuit on a failed fopen. */
        if (try_root_and_data(root)) {
            LOG_INFO("data-root", "matched on drive %c:", 'A' + letter);
            return 1;
        }
    }
    return 0;
}
#endif

#if defined(__linux__) && !defined(WACKI_HANDHELD)
/* Walk the standard Linux mount-point parents looking for any
 * subdirectory that contains the probe file. */
static int scan_linux_mounts(void)
{
    const char *user = getenv("USER");
    const char *parents[6];
    int n = 0;
    parents[n++] = "/media";
    parents[n++] = "/mnt";
    char buf1[256], buf2[256];
    if (user && *user) {
        snprintf(buf1, sizeof buf1, "/media/%s", user);
        parents[n++] = buf1;
        snprintf(buf2, sizeof buf2, "/run/media/%s", user);
        parents[n++] = buf2;
    }
    for (int i = 0; i < n; ++i) {
        DIR *d = opendir(parents[i]);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            char child[ARCHIVE_PROBE_PATH_BYTES];
            snprintf(child, sizeof child, "%s/%s", parents[i], de->d_name);
            if (try_root_and_data(child)) {
                LOG_INFO("data-root", "matched on %s", child);
                closedir(d);
                return 1;
            }
        }
        closedir(d);
    }
    return 0;
}
#endif

#ifdef WACKI_HANDHELD
/* Canonical handheld data locations. The normal case is already covered
 * by the cwd + SDL_GetBasePath probes above (OnionOS' launch_standalone
 * and PortMaster both cd into the game dir before exec), so this is a
 * belt-and-suspenders fallback for the Miyoo SD root and the common
 * PortMaster ports folders (/roms/ports, /roms2/ports) used across
 * Anbernic firmwares (muOS, ROCKNIX, ArkOS, Knulli, Batocera...). */
static int scan_handheld_card(void)
{
    const char *paths[] = {
#ifdef WACKI_PS2
        /* PS2 — opened via fileXio (see try_open_path / cygio.c), both
         * confirmed on PCSX2:
         *   host: — PCSX2's HostFS, rooted at the booted ELF's folder
         *           (dev: a bare ELF next to ./data).
         *   cdfs: — the ISO9660 disc; DATA/ holds the archives. cdfs is
         *           upper-case and forward-slash (the normalizer upper-
         *           cases the path; no ';1' suffix). */
        "host:data",
        "host:",
        "cdfs:/DATA",
        "cdfs:",
#endif
        /* Miyoo / OnionOS */
        "/mnt/SDCARD",
        "/mnt/SDCARD/data",
        "/mnt/SDCARD/wacki",
        "/mnt/SDCARD/wacki/data",
        "/mnt/SDCARD/Roms/PORTS/Games/Wacki",
        "/mnt/SDCARD/Roms/PORTS/Games/Wacki/data",
        /* PortMaster (Anbernic & friends) */
        "/roms/ports/Wacki",
        "/roms/ports/Wacki/data",
        "/roms2/ports/Wacki",
        "/roms2/ports/Wacki/data",
        NULL
    };
    for (int i = 0; paths[i]; ++i) {
        if (try_root(paths[i])) {
            LOG_INFO("data-root", "matched on %s", paths[i]);
            return 1;
        }
    }

#ifdef WACKI_PS2
    /* Real-hardware last resort: host: (PCSX2 HostFS) and cdfs: (a disc)
     * don't exist when booted from a USB stick via uLaunchELF — the data
     * sits on the same stick at mass:/wacki/data/. Bring up the USB FAT
     * stack (lazily, so PCSX2/disc boots skip it) and probe mass:. */
    extern int platform_ps2_mount_usb(void);
    if (platform_ps2_mount_usb()) {
        const char *usb[] = {
            "mass:/wacki/data", "mass:/wacki", "mass:/DATA", "mass:", NULL
        };
        for (int i = 0; usb[i]; ++i) {
            if (try_root(usb[i])) {
                LOG_INFO("data-root", "matched on %s (USB)", usb[i]);
                return 1;
            }
        }
    }
#endif
    return 0;
}
#endif

/* ---- GUI fallback — native folder picker ------------------------- */

#ifndef WACKI_HANDHELD
/* Run the supplied shell command and read one line of stdout.
 * Returns 1 on a non-empty line trimmed into `out`, 0 otherwise. */
static int run_picker_command(const char *cmd, char *out, size_t out_sz)
{
    if (!cmd || !out || out_sz < 2) return 0;
    FILE *fp =
#ifdef _WIN32
        _popen(cmd, "r");
#else
        popen(cmd, "r");
#endif
    if (!fp) return 0;
    int ok = 0;
    if (fgets(out, (int)out_sz, fp)) {
        /* Strip trailing newline + slash. */
        size_t n = strlen(out);
        while (n > 0 &&
               (out[n - 1] == '\n' || out[n - 1] == '\r' ||
                out[n - 1] == '/'  || out[n - 1] == '\\'))
        {
            out[--n] = 0;
        }
        ok = (n > 0);
    }
#ifdef _WIN32
    _pclose(fp);
#else
    pclose(fp);
#endif
    return ok;
}

/* Try the OS's native folder-picker dialog. Returns 1 on a valid
 * pick that contains Dane_02.dta; 0 otherwise (no picker available,
 * user cancelled, or selected a folder without the archive). */
static int ask_user_for_data_location(void)
{
    /* No GUI in headless mode (CI smoke, --headless dev runs): a
     * folder picker on a runner with no display either errors out
     * instantly or hangs until the harness timeout. Skip straight
     * to the no-data exit. */
    extern int g_headless;
    if (g_headless) return 0;

    char picked[1024] = {0};

#if defined(__APPLE__)
    const char *cmd =
        "osascript -e 'tell application \"Finder\" to activate' "
        "-e 'POSIX path of (choose folder with prompt "
        "\"Wybierz folder z plikami Dane_*.dta\")' 2>/dev/null";
    if (!run_picker_command(cmd, picked, sizeof picked)) return 0;

#elif defined(_WIN32)
    /* PowerShell FolderBrowserDialog — ships with every Windows.
     * STA threading required for the Forms dialog. */
    const char *cmd =
        "powershell -NoProfile -STA -Command "
        "\"Add-Type -AssemblyName System.Windows.Forms; "
        "$f=New-Object System.Windows.Forms.FolderBrowserDialog; "
        "$f.Description='Wybierz folder z plikami Dane_*.dta'; "
        "if ($f.ShowDialog() -eq 'OK') { $f.SelectedPath }\" 2>nul";
    if (!run_picker_command(cmd, picked, sizeof picked)) return 0;

#elif defined(__linux__)
    /* Try zenity first (GTK), then kdialog (KDE), then exit. */
    if (!run_picker_command(
            "zenity --file-selection --directory "
            "--title='Wybierz folder z plikami Dane_*.dta' 2>/dev/null",
            picked, sizeof picked))
    {
        if (!run_picker_command(
                "kdialog --getexistingdirectory ~ "
                "--title 'Wybierz folder z plikami Dane_*.dta' 2>/dev/null",
                picked, sizeof picked))
        {
            return 0;
        }
    }
#else
    return 0;
#endif

    if (!picked[0]) return 0;
    LOG_INFO("data-root", "user picked '%s'", picked);

    if (try_root_and_data(picked)) return 1;

    /* User picked a folder that doesn't contain the probe — show a
     * complaint dialog and bail. They can re-launch and pick again. */
    char msg[1200];
    snprintf(msg, sizeof msg,
             "Wybrany folder nie zawiera Dane_*.dta:\n\n%s\n\n"
             "Wskaż katalog z oryginalnej płyty (lub jego "
             "podkatalog data/) i uruchom grę ponownie.",
             picked);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING,
                             "Wacki — brak danych", msg, NULL);
    return 0;
}
#endif /* !WACKI_HANDHELD */

/* ---- public entry ------------------------------------------------ */

int FindDataRoot(void)
{
    /* 1. Explicit env override. */
    if (try_root(getenv("WACKI_PATH"))) return DATA_ROOT_FOUND;

    /* 2 + 3. cwd + ./data. */
    if (try_root("."))      return DATA_ROOT_FOUND;
    if (try_root("./data")) return DATA_ROOT_FOUND;
    if (try_root("data"))   return DATA_ROOT_FOUND;

    /* 4. Adjacent to the binary. SDL_GetBasePath gives the directory
     * containing the executable (or, in an .app bundle, the
     * Resources/ folder). */
    char *base = SDL_GetBasePath();
    if (base) {
        size_t blen = strlen(base);
        while (blen > 1 &&
               (base[blen - 1] == '/' || base[blen - 1] == '\\'))
        {
            base[--blen] = 0;
        }
        if (try_root_and_data(base)) {
            SDL_free(base);
            return DATA_ROOT_FOUND;
        }

#ifdef __APPLE__
        /* 5. macOS .app neighbor — the folder where Wacki.app sits.
         * Lets the user just drop Dane_*.dta into the same folder
         * as the bundle without opening Show Package Contents.
         *
         * A quarantined bundle (downloaded / AirDropped) launched from
         * Finder runs translocated: SDL_GetBasePath points at a random
         * read-only AppTranslocation mount whose neighbor holds no user
         * files. Resolve the real on-disk bundle path first so the probe
         * looks where the user actually dropped data/. No-op (returns
         * NULL, original base kept) when not translocated. */
        extern char *PlatformMacUntranslocatePath(const char *);
        char *real_base = PlatformMacUntranslocatePath(base);
        char *neighbor = macos_dirname_of_app(real_base ? real_base : base);
        if (neighbor) {
            if (try_root_and_data(neighbor)) {
                free(neighbor);
                free(real_base);
                SDL_free(base);
                return DATA_ROOT_FOUND;
            }
            free(neighbor);
        }
        free(real_base);
#endif
        SDL_free(base);
    }

    /* 6 + 7. External media / handheld card. */
#ifdef WACKI_HANDHELD
    if (scan_handheld_card())  return DATA_ROOT_FOUND;
#elif defined(__APPLE__)
    if (scan_macos_volumes())  return DATA_ROOT_FOUND;
#elif defined(_WIN32)
    if (scan_windows_drives()) return DATA_ROOT_FOUND;
#elif defined(__linux__)
    if (scan_linux_mounts())   return DATA_ROOT_FOUND;
#endif

    /* 8. GUI fallback. Skipped on handheld — no keyboard, no native
     * folder picker, and OnionOS' Ports launcher would have refused
     * to start the game without GameDataFile already present. */
#ifndef WACKI_HANDHELD
    if (ask_user_for_data_location()) return DATA_ROOT_FOUND;
#endif

    return 0;
}
