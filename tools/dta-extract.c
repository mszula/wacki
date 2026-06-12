/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * dta-extract.c — standalone extractor for Cygert's .dta archives.
 *
 * Build:  i686-w64-mingw32-gcc -O2 -o dta-extract dta-extract.c
 *         ../src/depack.c ../src/platform/sdl/file_host.c ../src/heap.c
 *
 * Usage:  dta-extract <Dane_XX.dta> [out_dir]
 *
 * Dumps every entry, decompressed, with the original (uppercase) 12-char
 * name. The file's leading magic ("ANIM" / "MASK" / "FILD") is kept intact
 * — the consumer can re-parse it with assets.c::LoadAssetFromDtaBase.
 */
#include "../include/wacki.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

extern int OpenDtaArchiveFile(const char *);
extern int LoadFileFromDta(const char *, void **, uint32_t *);

extern void *xmalloc(uint32_t); extern void xfree(void *);

/* re-declared here because we want raw access for printing */
extern struct { char name[12]; uint32_t off; } *s_dir;
extern int32_t s_dir_count;

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <dta> [outdir]\n", argv[0]); return 1; }
    const char *out = argc > 2 ? argv[2] : ".";

    if (!OpenDtaArchiveFile(argv[1])) {
        fprintf(stderr, "cannot open %s\n", argv[1]); return 1;
    }
    printf("opened %s, %d entries\n", argv[1], s_dir_count);
    for (int i = 0; i < s_dir_count; ++i) {
        char nm[16]; memcpy(nm, s_dir[i].name, 12); nm[12] = 0;
        for (int j = 0; nm[j]; ++j) nm[j] = (char)tolower((unsigned char)nm[j]);

        void *buf = NULL; uint32_t sz = 0;
        if (!LoadFileFromDta(nm, &buf, &sz)) {
            fprintf(stderr, "  skip %s (decompress failed)\n", nm); continue;
        }
        char path[512];
        snprintf(path, sizeof path, "%s/%s", out, nm);
        FILE *fp = fopen(path, "wb");
        if (fp) { fwrite(buf, 1, sz, fp); fclose(fp); }
        printf("  %s  (%u bytes)\n", nm, sz);
        xfree(buf);
    }
    return 0;
}
