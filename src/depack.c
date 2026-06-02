/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/depack.c — PKv2 (LZ77 + Shannon-Fano) decompressor.
 *
 * Cygert's custom packer; the string "Depack routines by Henryk Cygert"
 * lives at PE offset 0x446038 in the original WACKI.EXE. Decoder walks
 * the bit stream BACKWARDS from a trailer at the very end of the
 * compressed buffer; output is also produced back-to-front.
 *
 * Buffer layout (all 12-byte header fields little-endian):
 *
 *   +0   u32 magic = 'PKv2' (0x32764B50)
 *   +4   u32 compressed_size (= trailer offset from buf start)
 *   +8   u32 unpacked_size
 *   +12  ... compressed payload (consumed back-to-front)
 *
 * Trailer (relative to buf + compressed_size, i.e. one past last byte):
 *
 *   -32..-29   u32  initial literal-run length (init_lit)
 *   -28        u8   initial bit_buf
 *   -27        u8   initial bit_cnt
 *   -26..-21   6×u8 → 12-nibble table "match-offset bit widths"
 *   -20..-15   6×u8 → 12-nibble table "literal-length bit widths"
 *   -14..-3    12 B "scratch" (copied back to buf[0..11] for in-place)
 *   -2         ignored
 *   -1         u8 mode marker: 0 = raw literal copy, else LZ77
 *
 * Fixed match-length bit widths per group: {0, 0, 0, 3, 5, 16}. */

#include "wacki.h"
#include "wacki/log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- constants ---------------------------------------------------- */

#define TRAILER_OFF_OFF_BITS         0x12   /* eot - 0x12: 6 B → tab_off_bits */
#define TRAILER_OFF_LIT_BITS         0x18   /* eot - 0x18: 6 B → tab_lit_bits */
#define TRAILER_OFF_BIT_CNT          0x19   /* eot - 0x19: initial bs_cnt */
#define TRAILER_OFF_BIT_BUF          0x1A   /* eot - 0x1A: initial bs_buf */
#define TRAILER_OFF_INIT_LIT_LEN     0x1E   /* eot - 0x1E: 4 B init literal run */
#define SCRATCH_TAIL_BYTES           0x0C   /* last 12 B → buf[0..11] for in-place */
#define INIT_LIT_RUN_LEN_BYTES       4
#define MIN_COMP_SIZE                32     /* below this the trailer can't fit */

#define WIDTH_TABLE_ENTRIES          12     /* 4 groups × 3 sub-codes */
#define WIDTH_TABLE_GROUPS           4
#define MATCH_LEN_GROUPS             6      /* k_mlen_bits[] size */
#define UNARY_PREFIX_MAX             5      /* group selector cap */
#define SUB_GROUP_UNARY_MAX          2      /* sub-code selector cap */
#define GROUP_CLAMP_MAX              3      /* g clipped to 0..3 for tab lookups */
#define MATCH_LEN_BASE_FIRST         2      /* base_mlen[0] */
#define MAX_DECODE_ITERS             10000000u

/* Fixed tables — not derived from the trailer. */
static const uint8_t k_mlen_bits[MATCH_LEN_GROUPS] = { 0, 0, 0, 3, 5, 16 };
static const uint8_t k_mask_lut[9] = { 0, 1, 3, 7, 15, 31, 63, 127, 255 };

/* ---- trailer-derived state --------------------------------------- */

/* 12-entry bit-width tables (decoded from the two 6-byte trailer
 * blocks by unpack_widths). */
static uint8_t  tab_off_bits[WIDTH_TABLE_ENTRIES];
static uint8_t  tab_lit_bits[WIDTH_TABLE_ENTRIES];

/* Precomputed running bases used by the per-token decode. */
static uint32_t base_mlen[MATCH_LEN_GROUPS];
static uint32_t base_off [WIDTH_TABLE_ENTRIES];
static uint32_t base_lit [WIDTH_TABLE_ENTRIES];

/* Bit-stream cursor — walks the compressed buffer backwards. */
static const uint8_t *bs_ptr;
static uint32_t       bs_buf;
static uint8_t        bs_cnt;
static const uint8_t *bs_floor;

/* ---- bit-stream helpers ------------------------------------------ */

static inline void bs_refill(void)
{
    if (bs_ptr <= bs_floor) {
        bs_buf = 0;
        bs_cnt = 8;
        return;
    }
    --bs_ptr;
    bs_buf = *bs_ptr;
    bs_cnt = 8;
}

/* Read a unary-coded prefix capped at `max` (= consume up to `max`
 * 1-bits then a terminating 0; returns the run length, or `max` if
 * all `max` 1-bits arrived). */
static inline int bs_unary(int max)
{
    int n = 0;
    while (n < max) {
        if (bs_cnt == 0) bs_refill();
        uint8_t b = bs_buf & 1;
        bs_buf >>= 1;
        --bs_cnt;
        if (!b) break;
        ++n;
    }
    return n;
}

/* Read `n` bits (n ≤ 32) into a u32. */
static inline uint32_t bs_bits(uint8_t n)
{
    if (n == 0) return 0;
    if (n > 32) return 0;
    uint32_t val   = 0;
    uint8_t  shift = 0;
    while (n) {
        if (bs_cnt == 0) bs_refill();
        uint8_t  take  = (n < bs_cnt) ? n : bs_cnt;
        uint32_t chunk = (uint32_t)(k_mask_lut[take] & bs_buf);
        val |= chunk << shift;
        bs_buf >>= take;
        bs_cnt  -= take;
        shift   += take;
        n       -= take;
    }
    return val;
}

/* ---- trailer-table decoders -------------------------------------- */

/* Decode a 6-byte trailer block into a 12-entry bit-width table.
 *
 * Per byte: hi nibble (signed) + lo nibble (unsigned). The first
 * iteration latches the carry to hi[0]; every entry is `nibble + carry`. */
static void unpack_widths(uint8_t out[WIDTH_TABLE_ENTRIES], const uint8_t *six)
{
    int8_t carry = (int8_t)six[0] >> 4;
    out[0] = (uint8_t)carry;
    out[1] = (uint8_t)((int8_t)(six[0] & 0x0F) + carry);
    for (int i = 1; i < 6; ++i) {
        int8_t hi = (int8_t)six[i] >> 4;
        int8_t lo = (int8_t)(six[i] & 0x0F);
        out[2 * i + 0] = (uint8_t)(hi + carry);
        out[2 * i + 1] = (uint8_t)(lo + carry);
    }
}

/* Precompute base_mlen / base_off / base_lit from the decoded width
 * tables. Called once per decode after unpack_widths fills both
 * tab_*_bits arrays. */
static void precompute_bases(void)
{
    base_mlen[0] = MATCH_LEN_BASE_FIRST;
    for (int i = 1; i < MATCH_LEN_GROUPS; ++i) {
        uint8_t bw = k_mlen_bits[i];
        base_mlen[i] = base_mlen[i - 1] + (bw == 0 ? 1u : (1u << bw));
    }

    /* base_off — 4 groups of 3 sub-codes. Each group starts fresh
     * (no carry from the previous group). */
    for (int g = 0; g < WIDTH_TABLE_GROUPS; ++g) {
        int i = g * 3;
        base_off[i + 0] = 1u << tab_off_bits[i + 0];
        base_off[i + 1] = base_off[i + 0] + (1u << tab_off_bits[i + 1]);
        base_off[i + 2] = base_off[i + 1] + (1u << tab_off_bits[i + 2]);
    }

    /* base_lit — same shape but starts with (1<<bits) - 1 instead of
     * (1<<bits) for the group head. */
    for (int g = 0; g < WIDTH_TABLE_GROUPS; ++g) {
        int i = g * 3;
        base_lit[i + 0] = (1u << tab_lit_bits[i + 0]) - 1u;
        base_lit[i + 1] = base_lit[i + 0] + (1u << tab_lit_bits[i + 1]);
        base_lit[i + 2] = base_lit[i + 1] + (1u << tab_lit_bits[i + 2]);
    }
}

/* ---- main decode loop -------------------------------------------- */

void DepackPkv2Buffer(void *src_, void *dst_, void (*progress)(int))
{
    uint8_t    *src = (uint8_t *)src_;
    uint8_t    *dst = (uint8_t *)dst_;
    Pkv2Header *h   = (Pkv2Header *)src;

    if (h->magic != PKV2_MAGIC) {
        LOG_TRACE("depack", "bad PKv2 magic 0x%08X", h->magic);
        return;
    }
    uint32_t comp = h->compressed_size;
    uint32_t unp  = h->unpacked_size;
    if (comp < MIN_COMP_SIZE || unp == 0) {
        LOG_TRACE("depack", "sanity: comp=%u unp=%u — bail", comp, unp);
        return;
    }

    uint8_t *eot = src + comp - 1;   /* last byte of compressed payload */

    /* Mode marker — 0 = no compression, body is raw copy. */
    if (*eot == 0) {
        memcpy(dst, src + sizeof *h, unp);
        if (progress) progress(100);
        return;
    }

    /* In-place callers: the encoder stashed 12 B of "scratch" at the
     * trailer tail; copy it back to the buffer head where the decoder
     * is about to overwrite the final iterations' bytes. For out-of-
     * place callers (src != dst) this is a no-op since dst starts
     * clean. */
    if (src == dst) memcpy(src, eot - SCRATCH_TAIL_BYTES, SCRATCH_TAIL_BYTES);

    unpack_widths(tab_off_bits, eot - TRAILER_OFF_OFF_BITS);
    unpack_widths(tab_lit_bits, eot - TRAILER_OFF_LIT_BITS);
    precompute_bases();

    /* Initial bit-stream state — the bytes immediately above the two
     * width tables. */
    bs_cnt = *(eot - TRAILER_OFF_BIT_CNT);
    bs_buf = *(eot - TRAILER_OFF_BIT_BUF);
    bs_ptr =  eot - TRAILER_OFF_INIT_LIT_LEN;

    uint32_t init_lit;
    memcpy(&init_lit, bs_ptr, INIT_LIT_RUN_LEN_BYTES);   /* avoid misaligned load */
    if (init_lit > unp) {
        LOG_TRACE("depack", "init_lit=%u > unp=%u — bail", init_lit, unp);
        return;
    }

    /* The initial literal run sits just before the init_lit DWORD —
     * memmove it into the tail of the output buffer, then walk the
     * bit stream from the byte right before it. */
    bs_ptr  -= init_lit;
    bs_floor = src;

    uint8_t *out = dst + (unp - init_lit);
    memmove(out, bs_ptr, init_lit);

    int      last_pct = -1;
    uint32_t iters    = 0;
    while (out > dst) {
        /* 1. Group selector — unary prefix capped at UNARY_PREFIX_MAX. */
        int g = bs_unary(UNARY_PREFIX_MAX);

        /* 2. Match length: groups 0..2 use the fixed base; groups 3+
         * read k_mlen_bits[g] extra bits + the previous base + 1. */
        uint32_t mlen;
        uint8_t  bw = k_mlen_bits[g];
        if (bw == 0) {
            mlen = base_mlen[g];
        } else {
            uint32_t x = bs_bits(bw);
            mlen = base_mlen[g - 1] + 1 + x;
        }

        /* 3. Clamp group selector for the 12-entry sub-tables. */
        int gc = g > GROUP_CLAMP_MAX ? GROUP_CLAMP_MAX : g;

        /* 4. Match offset: unary sub-code → bit width → bits + group base. */
        int      sg   = bs_unary(SUB_GROUP_UNARY_MAX);
        int      io   = gc * 3 + sg;
        uint32_t moff = bs_bits(tab_off_bits[io]) + 1;
        if (sg != 0) moff += base_off[io - 1];

        /* 5. Literal length: same shape as match offset. */
        int      sg2  = bs_unary(SUB_GROUP_UNARY_MAX);
        int      il   = gc * 3 + sg2;
        uint32_t llen = bs_bits(tab_lit_bits[il]);
        if (sg2 != 0) llen += base_lit[il - 1] + 1;

        /* 6. Back-reference copy. Clamps below are dead in practice
         * (verified across all 1782 DANE_02.DTA entries) — if either
         * fires it's data corruption, so bail. */
        if (mlen > (uint32_t)(out - dst)) {
            LOG_TRACE("depack", "iter %u: mlen=%u overshoots remaining %u — bail", iters, mlen, (uint32_t)(out - dst));
            return;
        }
        if (mlen == 0) break;
        uint8_t *back = out + moff;
        for (uint32_t i = 0; i < mlen; ++i) {
            *--out = *--back;
        }

        /* 7. Literal copy from the bit-stream pointer. */
        if (llen > (uint32_t)(out - dst)) {
            LOG_TRACE("depack", "iter %u: llen=%u overshoots remaining %u — bail", iters, llen, (uint32_t)(out - dst));
            return;
        }
        out    -= llen;
        bs_ptr -= llen;
        if (bs_ptr < bs_floor) {
            LOG_TRACE("depack", "iter %u: bs_ptr underflow — bail", iters);
            return;
        }
        /* In-place src/dst: bs_ptr and out can overlap near the end —
         * use memmove for defined behaviour. */
        memmove(out, bs_ptr, llen);

        if (progress) {
            uint32_t done = unp - (uint32_t)(out - dst);
            int      pct  = (int)((done * 100u) / unp);
            if (pct != last_pct) {
                progress(pct);
                last_pct = pct;
            }
        }

        if (++iters > MAX_DECODE_ITERS) {
            LOG_TRACE("depack", "runaway after %u iters — bail", iters);
            return;
        }
    }
    if (progress) progress(100);
}
