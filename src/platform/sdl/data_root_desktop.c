/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/sdl/data_root_desktop.c — data-root discovery (storage HAL),
 * desktop implementation.
 *
 * The portable part of the search (env var, cwd, binary-adjacent) lives in
 * data_root.c. This file supplies the two SDL-family hooks for the desktop:
 *
 *   plat_data_roots()        — external media: /Volumes (macOS), A:..Z:
 *                              (Windows), /media + /mnt mounts (Linux), plus
 *                              the macOS .app-neighbor folder.
 *   plat_prompt_data_folder()— the native folder picker (osascript /
 *                              PowerShell / zenity / kdialog).
 *
 * The handheld counterpart (SD-card list, no picker) is data_root_handheld.c;
 * PS2 lives in src/platform/ps2/storage_ps2.c (fileXio devices) + system_ps2.c
 * (the USB-mass mount). The `#ifdef`s that remain here are OS variants
 * (macOS / Windows / Linux) intrinsic to one desktop SDL backend. */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/storage.h"

#include <SDL.h>          /* SDL_GetBasePath/SDL_free + the picker complaint box */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__) || defined(__linux__)
#include <dirent.h>
#include <sys/stat.h>
#endif

#define DATA_ROOT_SCAN_PATH_BYTES   1024

/* ---- macOS .app neighbor ----------------------------------------- */

#ifdef __APPLE__
/* Given a path inside a .app bundle (typically the Resources/ dir from
 * SDL_GetBasePath()), walk parents until a component ending in ".app" is
 * found, and return the directory that .app sits in. Caller frees the
 * malloc()'d string. Returns NULL when the input isn't inside a bundle. */
static char *macos_dirname_of_app(const char *base)
{
    if (!base || !*base) return NULL;
    char *cur = strdup(base);
    if (!cur) return NULL;
    for (;;) {
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

/* /Volumes/{*}/ and /Volumes/{*}/data/. macOS mounts every external disk
 * (USB, CD, DVD, network share) under /Volumes — same place Finder shows
 * them. The probe tries each volume and its data/ subfolder. */
static int scan_macos_volumes(int (*probe)(const char *root))
{
    DIR *d = opendir("/Volumes");
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char buf[DATA_ROOT_SCAN_PATH_BYTES];
        snprintf(buf, sizeof buf, "/Volumes/%s", de->d_name);
        if (probe(buf)) {
            LOG_INFO("data-root", "matched on /Volumes/%s", de->d_name);
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}
#endif /* __APPLE__ */

#ifdef _WIN32
#include <windows.h>
/* Iterate A:..Z: looking for the probe file at root or in data/.
 * GetLogicalDrives returns a bitmask of drives that currently exist
 * (mounted volumes + CD drives even with no media), so the probe is
 * O(present-drives), not O(26). */
static int scan_windows_drives(int (*probe)(const char *root))
{
    DWORD mask = GetLogicalDrives();
    for (int letter = 0; letter < 26; ++letter) {
        if (!(mask & (1u << letter))) continue;
        char root[8];
        snprintf(root, sizeof root, "%c:/", 'A' + letter);
        if (probe(root)) {
            LOG_INFO("data-root", "matched on drive %c:", 'A' + letter);
            return 1;
        }
    }
    return 0;
}
#endif /* _WIN32 */

#ifdef __linux__
/* Walk the standard Linux mount-point parents looking for any subdirectory
 * that contains the probe file. */
static int scan_linux_mounts(int (*probe)(const char *root))
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
            char child[DATA_ROOT_SCAN_PATH_BYTES];
            snprintf(child, sizeof child, "%s/%s", parents[i], de->d_name);
            if (probe(child)) {
                LOG_INFO("data-root", "matched on %s", child);
                closedir(d);
                return 1;
            }
        }
        closedir(d);
    }
    return 0;
}
#endif /* __linux__ */

int plat_data_roots(int (*probe)(const char *root))
{
#ifdef __APPLE__
    /* The folder where Wacki.app sits — lets the user drop Dane_*.dta next
     * to the bundle without opening Show Package Contents. Tried before the
     * external-media scan. */
    char *base = SDL_GetBasePath();
    if (base) {
        char *neighbor = macos_dirname_of_app(base);
        SDL_free(base);
        if (neighbor) {
            int hit = probe(neighbor);
            free(neighbor);
            if (hit) return 1;
        }
    }
    return scan_macos_volumes(probe);
#elif defined(_WIN32)
    return scan_windows_drives(probe);
#elif defined(__linux__)
    return scan_linux_mounts(probe);
#else
    (void)probe;
    return 0;
#endif
}

/* ================================================================= *
 *  Native folder picker
 * ================================================================= */

/* Run the supplied shell command and read one line of stdout. Returns 1 on a
 * non-empty line trimmed into `out`, 0 otherwise. */
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

int plat_prompt_data_folder(int (*probe)(const char *root))
{
    /* No GUI in headless mode (CI smoke, --headless dev runs): a folder
     * picker on a runner with no display either errors out instantly or
     * hangs until the harness timeout. */
    if (g_headless) return 0;

    char picked[1024] = {0};

#if defined(__APPLE__)
    const char *cmd =
        "osascript -e 'tell application \"Finder\" to activate' "
        "-e 'POSIX path of (choose folder with prompt "
        "\"Wybierz folder z plikami Dane_*.dta\")' 2>/dev/null";
    if (!run_picker_command(cmd, picked, sizeof picked)) return 0;

#elif defined(_WIN32)
    /* PowerShell FolderBrowserDialog — ships with every Windows. STA
     * threading required for the Forms dialog. */
    const char *cmd =
        "powershell -NoProfile -STA -Command "
        "\"Add-Type -AssemblyName System.Windows.Forms; "
        "$f=New-Object System.Windows.Forms.FolderBrowserDialog; "
        "$f.Description='Wybierz folder z plikami Dane_*.dta'; "
        "if ($f.ShowDialog() -eq 'OK') { $f.SelectedPath }\" 2>nul";
    if (!run_picker_command(cmd, picked, sizeof picked)) return 0;

#elif defined(__linux__)
    /* Try zenity first (GTK), then kdialog (KDE), then give up. */
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

    if (probe(picked)) return 1;

    /* User picked a folder that doesn't contain the probe — complain and
     * bail. They can re-launch and pick again. */
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
