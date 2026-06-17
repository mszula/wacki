/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * include/wacki/platform/storage.h — platform storage HAL.
 *
 * Where bytes live and how they're read/written is platform-specific; the
 * engine core calls these and never #ifdefs on a platform. Each platform
 * provides exactly one implementation (linked by the build per TARGET — see
 * docs/platform-hal.md). This header grows as the storage subsystems migrate
 * behind it; for now it covers the save image.
 *
 * Implementations:
 *   desktop / handheld  src/platform/sdl/save_host.c  (a file + atomic rename)
 *   PS2                 src/platform/ps2/storage_ps2.c            (the memory card, libmc)
 */
#ifndef WACKI_PLATFORM_STORAGE_H
#define WACKI_PLATFORM_STORAGE_H

#include <stddef.h>   /* size_t — plat_resolve_path_ci out-buffer length */
#include <stdint.h>   /* fread_cyg/ftell_cyg widths — keeps this header self-contained */

/* ---- save image -------------------------------------------------- *
 *
 * The whole save (`g_save`) is read once at boot and written on demand. */

/* Read up to `size` bytes of the save image into `buf`. Returns the number
 * of bytes read — 0 if no save exists yet or the read failed. */
int plat_save_read(void *buf, int size);

/* Persist `size` bytes from `buf` as the save image, as atomically as the
 * platform allows (the engine relies on never observing a half-written
 * save). Returns 1 on success, 0 on failure. */
int plat_save_write(const void *buf, int size);

/* ---- data-root discovery ----------------------------------------- *
 *
 * The engine's portable search (env var, cwd, binary-adjacent) lives in
 * data_root.c; everything platform-specific about *where else* the game
 * data can live is delegated here. Both entry points are driven by a
 * core-supplied `probe(root)` callback that tests a candidate directory
 * for the data archive and, on a hit, commits it as the data root —
 * returning non-zero. The platform stops at the first non-zero probe. */

/* Probe the platform's built-in candidate data roots in priority order:
 * external media (desktop), the SD-card layout (handheld), or the PS2
 * storage devices plus a lazy USB-mass mount. Returns the probe's non-zero
 * value on the first match, or 0 if none matched. */
int plat_data_roots(int (*probe)(const char *root));

/* Last-resort interactive fallback: ask the user to point at the data
 * folder via the OS's native directory picker (desktop only). The picked
 * path is handed to `probe`; returns its non-zero value on a valid pick, or
 * 0 when no picker is available (handheld / PS2), the user cancelled, or the
 * run is headless. */
int plat_prompt_data_folder(int (*probe)(const char *root));

/* ---- file I/O ---------------------------------------------------- *
 *
 * The Cygert "Base_IO_CPP" file-class shim — every DTA archive + asset read
 * routes through here, which is exactly why swapping the backend per platform
 * needs no other engine changes. CygFile is opaque; the concrete struct is
 * defined by the per-platform implementation:
 *
 *   desktop / handheld  src/platform/sdl/file_host.c  (newlib stdio)
 *   PS2                 src/platform/ps2/storage_ps2.c            (fileXio — newlib fopen
 *                                                     reaches no device) */
typedef struct CygFile CygFile;

CygFile *fopen_cyg (const char *name, const char *mode);
void     fclose_cyg(CygFile *);
uint32_t fread_cyg (void *dst, uint32_t sz, uint32_t n, CygFile *);
void     fseek_cyg (CygFile *, int32_t off, int whence);
int32_t  ftell_cyg (CygFile *);

/* ---- FLIC / AVI cutscene streaming reader ------------------------ *
 *
 * A read-ahead-optimized reader for the (large) cutscene files: the decoder
 * issues one read per AVI sub-chunk and must never block on disc latency.
 * Only one cutscene plays at a time, so this is a single *global* reader (no
 * handle). Implementations:
 *
 *   desktop / handheld  src/platform/sdl/flic_host.c  (a setvbuf'd stdio FILE
 *                                                      — 1 MiB read-ahead)
 *   PS2                 src/platform/ps2/storage_ps2.c            (a background thread
 *                                                      filling a ring; a flood
 *                                                      of tiny fileXio RPCs
 *                                                      would starve audsrv) */
int      plat_flic_open(const char *path);   /* 1 = opened, 0 = failed */
uint32_t plat_flic_read(void *dst, uint32_t n);
void     plat_flic_seek(int32_t off, int whence);
int32_t  plat_flic_tell(void);
void     plat_flic_close(void);

/* ---- case-insensitive path resolution ---------------------------- *
 *
 * On case-sensitive filesystems (Linux) the original CD's lower-cased data
 * files (dane_02.dta) don't match the engine's mixed-case requests
 * (Dane_02.dta), so the literal open misses and the game can't find its
 * data. Given a path whose literal open just FAILED, scan its directory for
 * a basename equal ignoring ASCII case; on a match write the real on-disk
 * path into out[outcap] and return 1, else return 0. Call only on the open-
 * miss path — the fast path (exact case, or a case-insensitive FS like
 * macOS/Windows) never reaches here. Desktop/handheld only (POSIX dirent);
 * the PS2 ISO backend is upper-case-only and needs no resolution. */
int plat_resolve_path_ci(const char *in, char *out, size_t outcap);

#endif /* WACKI_PLATFORM_STORAGE_H */
