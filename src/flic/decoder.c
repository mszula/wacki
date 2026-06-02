/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/flic/decoder.c — FLIC (FLI/FLC) frame decoder.
 *
 * FLIC frames are sequenced as a stream of chunks, each prefixed by a
 * size + type. The decoder dispatches on chunk type:
 *
 *   COLOR_256 (4)   — RGB888 palette update, packet-encoded
 *   DELTA_FLC (7)   — line-skip + run-length delta (.flc dialect)
 *   COLOR_64  (11)  — 6-bit VGA palette update, packet-encoded
 *   BLACK     (13)  — clear screen
 *   BRUN      (15)  — full-frame run-length-encoded image
 *   COPY      (16)  — uncompressed full frame
 *   PSTAMP    (18)  — postage-stamp preview (ignored)
 *
 * The output target is the global g_back_shadow (8bpp screen buffer).
 * Palette updates write directly to g_palette_rgb.
 *
 * Used by the AVI playback wrapper in flic.c (PlayFlicAviFile) to
 * decode each video chunk into the back buffer. */

#include "wacki.h"

#include <stdint.h>
#include <string.h>

/* ---- constants ---------------------------------------------------- */

#define FLIC_FRAME_MAGIC            0xF1FA
#define CK_COLOR_64                 11
#define CK_COLOR_256                4
#define CK_DELTA_FLC                7
#define CK_BLACK                    13
#define CK_BRUN                     15
#define CK_COPY                     16
#define CK_PSTAMP                   18

#define FLIC_FRAME_HEADER_BYTES     16    /* +0 size, +4 magic, +6 chunks */
#define FLIC_CHUNK_HEADER_BYTES     6     /* +0 size, +4 type */

#define PALETTE_SIZE                256
#define PALETTE_BYTES_PER_ENTRY     3
#define VGA_6BIT_TO_8BIT_SHIFT      2

/* DELTA_FLC opcode flags in the line-header word. */
#define DELTA_OP_FLAG_MASK          0xC000
#define DELTA_OP_LINE_SKIP          0xC000
#define DELTA_OP_LAST_BYTE          0x8000
#define DELTA_OP_LAST_BYTE_VALUE    0x00FF

/* ---- LE u32 helper ------------------------------------------------ */

static inline uint32_t flic_rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* ---- chunk decoders ---------------------------------------------- */

/* COLOR_256 — RGB888 palette update. Packets advance an index cursor:
 * each packet is `<skip> <count> <r g b>×count`. count==0 means 256. */
static void flic_color_256(const uint8_t *p, uint32_t sz)
{
    (void)sz;
    uint16_t packets = (uint16_t)(p[0] | (p[1] << 8));
    p += 2;
    uint16_t idx = 0;
    while (packets--) {
        idx += p[0];                       /* skip count */
        uint16_t count = p[1];              /* 0 means 256 */
        p += 2;
        uint16_t n = count ? count : PALETTE_SIZE;
        for (uint16_t i = 0; i < n && idx < PALETTE_SIZE; ++i, ++idx) {
            g_palette_rgb[idx * PALETTE_BYTES_PER_ENTRY + 0] = p[0];
            g_palette_rgb[idx * PALETTE_BYTES_PER_ENTRY + 1] = p[1];
            g_palette_rgb[idx * PALETTE_BYTES_PER_ENTRY + 2] = p[2];
            p += 3;
        }
    }
}

/* COLOR_64 — same packet structure as COLOR_256 but RGB values are
 * 6-bit (0..63) legacy VGA. Shift left by 2 to expand to 8-bit. */
static void flic_color_64(const uint8_t *p, uint32_t sz)
{
    (void)sz;
    uint16_t packets = (uint16_t)(p[0] | (p[1] << 8));
    p += 2;
    uint16_t idx = 0;
    while (packets--) {
        idx += p[0];
        uint16_t count = p[1];
        p += 2;
        uint16_t n = count ? count : PALETTE_SIZE;
        for (uint16_t i = 0; i < n && idx < PALETTE_SIZE; ++i, ++idx) {
            g_palette_rgb[idx * PALETTE_BYTES_PER_ENTRY + 0] =
                (uint8_t)(p[0] << VGA_6BIT_TO_8BIT_SHIFT);
            g_palette_rgb[idx * PALETTE_BYTES_PER_ENTRY + 1] =
                (uint8_t)(p[1] << VGA_6BIT_TO_8BIT_SHIFT);
            g_palette_rgb[idx * PALETTE_BYTES_PER_ENTRY + 2] =
                (uint8_t)(p[2] << VGA_6BIT_TO_8BIT_SHIFT);
            p += 3;
        }
    }
}

/* BRUN — full-frame run-length-encoded image. One unused packet-count
 * byte per scanline, then packets. Each packet:
 *   signed byte n: n > 0 → n repetitions of next byte (yes inverted vs DELTA)
 *                  n < 0 → |n| literal bytes follow */
static void flic_brun(const uint8_t *p, uint32_t sz, int w, int h)
{
    (void)sz;
    for (int y = 0; y < h; ++y) {
        uint8_t *dst = g_back_shadow + (size_t)y * w;
        ++p;                                /* packet count — unused */
        int x = 0;
        while (x < w) {
            int8_t n = (int8_t)*p++;
            if (n >= 0) {
                uint8_t v = *p++;
                for (int i = 0; i < n; ++i) {
                    if (x < w) dst[x++] = v;
                }
            } else {
                int cnt = -n;
                for (int i = 0; i < cnt; ++i) {
                    if (x < w) dst[x++] = *p++;
                }
            }
        }
    }
}

/* DELTA_FLC — line-skip + run-length delta. Each line opcode is a u16
 * word: top two bits select among (skip lines down, last byte in line,
 * packet count). Packets work in word pairs (2 bytes per "pixel").
 *
 * Negative n in a packet = |n| pairs of (v0, v1) repeated; non-negative
 * n = n literal word pairs follow. */
static void flic_delta_flc(const uint8_t *p, uint32_t sz, int w)
{
    (void)sz;
    uint16_t lines = (uint16_t)(p[0] | (p[1] << 8));
    p += 2;
    int y = 0;
    while (lines > 0) {
        uint16_t opcode = (uint16_t)(p[0] | (p[1] << 8));
        p += 2;
        if ((opcode & DELTA_OP_FLAG_MASK) == DELTA_OP_LINE_SKIP) {
            y += -(int16_t)opcode;
            continue;
        }
        if ((opcode & DELTA_OP_FLAG_MASK) == DELTA_OP_LAST_BYTE) {
            uint8_t *dst = g_back_shadow + (size_t)y * w + (w - 1);
            *dst = (uint8_t)(opcode & DELTA_OP_LAST_BYTE_VALUE);
            ++y; --lines;
            continue;
        }
        uint16_t packets = opcode;
        uint8_t *dst = g_back_shadow + (size_t)y * w;
        int x = 0;
        for (uint16_t pk = 0; pk < packets; ++pk) {
            x += *p++;                      /* skip */
            int8_t n = (int8_t)*p++;
            if (n >= 0) {
                /* n word pairs literal */
                for (int i = 0; i < n; ++i) {
                    if (x < w) dst[x++] = *p;
                    if (x < w) dst[x++] = *(p + 1);
                    p += 2;
                }
            } else {
                int cnt = -n;
                uint8_t v0 = *p++;
                uint8_t v1 = *p++;
                for (int i = 0; i < cnt; ++i) {
                    if (x < w) dst[x++] = v0;
                    if (x < w) dst[x++] = v1;
                }
            }
        }
        ++y; --lines;
    }
}

/* ---- public entry point ------------------------------------------ */

/* Decode one FLIC frame into g_back_shadow + g_palette_rgb. The frame
 * data is a FLIC_FRAME_MAGIC-tagged record containing N chunks; each
 * chunk dispatches on its type byte. */
void flic_decode_frame(const uint8_t *fdata, uint32_t fsize, int w, int h)
{
    if (fsize < FLIC_FRAME_HEADER_BYTES) return;
    uint16_t magic  = (uint16_t)(fdata[4] | (fdata[5] << 8));
    uint16_t chunks = (uint16_t)(fdata[6] | (fdata[7] << 8));
    if (magic != FLIC_FRAME_MAGIC) return;

    const uint8_t *p   = fdata + FLIC_FRAME_HEADER_BYTES;
    const uint8_t *end = fdata + fsize;
    for (uint16_t i = 0;
         i < chunks && p + FLIC_CHUNK_HEADER_BYTES <= end;
         ++i)
    {
        uint32_t sz   = flic_rd_u32(p);
        uint16_t type = (uint16_t)(p[4] | (p[5] << 8));
        const uint8_t *body = p + FLIC_CHUNK_HEADER_BYTES;
        uint32_t       bsz  = sz - FLIC_CHUNK_HEADER_BYTES;
        switch (type) {
        case CK_COLOR_64:  flic_color_64 (body, bsz);    break;
        case CK_COLOR_256: flic_color_256(body, bsz);    break;
        case CK_BRUN:      flic_brun(body, bsz, w, h);   break;
        case CK_DELTA_FLC: flic_delta_flc(body, bsz, w); break;
        case CK_BLACK:     memset(g_back_shadow, 0, (size_t)w * h); break;
        case CK_COPY:      memcpy(g_back_shadow, body, (size_t)w * h); break;
        case CK_PSTAMP:    break;
        default:           break;
        }
        p += sz;
    }
}
