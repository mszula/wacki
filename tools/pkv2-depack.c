/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/*
 * pkv2-depack.c — standalone "PKv2" depacker (Henryk Cygert format).
 *
 * Build:  cc -O2 -o pkv2-depack pkv2-depack.c ../src/depack.c
 * Usage:  pkv2-depack input.bin output.bin
 */
#include "../include/wacki.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void DepackPkv2Buffer(void *src, void *dst, void (*progress)(int));

static void on_progress(int pct) { fprintf(stderr, "\r%3d%%", pct); }

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s in out\n", argv[0]); return 1; }
    FILE *fi = fopen(argv[1], "rb"); if (!fi) return 1;
    fseek(fi, 0, SEEK_END); long sz = ftell(fi); fseek(fi, 0, SEEK_SET);
    uint8_t *src = malloc(sz);  fread(src, 1, sz, fi); fclose(fi);

    Pkv2Header *h = (Pkv2Header *)src;
    if (h->magic != PKV2_MAGIC) { fprintf(stderr, "not PKv2\n"); return 1; }

    uint8_t *dst = malloc(h->unpacked_size);
    memset(dst, 0x00, h->unpacked_size);          /* fresh zeros so depack output is clear */
    DepackPkv2Buffer(src, dst, on_progress);
    fprintf(stderr, "\n");

    FILE *fo = fopen(argv[2], "wb");
    fwrite(dst, 1, h->unpacked_size, fo);
    fclose(fo);
    return 0;
}
