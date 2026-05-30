/*
 * depack.c — Cygert's "PKv2" LZ77 + Shannon-Fano decoder.
 *
 * Byte-faithful port of + in the original
 * WACKI.EXE (string at 0x446038: "Depack routines by Henryk Cygert").
 *
 * The algorithm walks the bit stream BACKWARDS from a trailer that lives
 * at the very end of the buffer. The output is also produced back-to-front.
 *
 * Buffer layout:
 * +0 uint32 magic = 0x32764B50 "PKv2"
 * +4 uint32 comp (total compressed size = trailer offset)
 * +8 uint32 unp (unpacked size)
 * +12 ... compressed payload (consumed back-to-front)
 * end-32..-29 uint32 init_literal_run_length
 * end-28 uint8 initial bit_buf
 * end-27 uint8 initial bit_cnt
 * end-26..-21 6 bytes -> 12-nibble table "match-offset bit-widths"
 * end-20..-15 6 bytes -> 12-nibble table "literal-length bit-widths"
 * end-14..-3 12 bytes "scratch" (copied back to src+0..src+11)
 * end-2 ignored
 * end-1 uint8 marker: 0 = raw literal mode, else LZ77
 *
 * (All offsets above are relative to buf+comp = past-the-end.
 * "end" below means src+comp.)
 *
 * Fixed match-length bit widths: {0, 0, 0, 3, 5, 16}
 * Fixed bit-mask LUT: {0, 1, 3, 7, 15, 31, 63, 127, 255}
 */
#include "wacki.h"
#include <string.h>
#include <stdio.h>

static const uint8_t k_mlen_bits[6] = { 0, 0, 0, 3, 5, 16 };
static const uint8_t k_mask_lut[9]  = { 0, 1, 3, 7, 15, 31, 63, 127, 255 };

/* trailer-derived bit-width tables (12 entries each) */
static uint8_t  tab_off_bits[12];   /* DAT_00479630 — match-offset bit widths */
static uint8_t  tab_lit_bits[12];   /* DAT_00479640 — literal-length bit widths */

/* precomputed bases */
static uint32_t base_mlen[6];       /* DAT_00479610 */
static uint32_t base_off [12];      /* DAT_00479658, organised as 4 groups of 3 */
static uint32_t base_lit [12];      /* DAT_00479688, 4 groups of 3 */

/* bit-stream cursor (walks the buffer backward) */
static const uint8_t *bs_ptr;       /* DAT_0047964C */
static uint32_t       bs_buf;       /* DAT_004796B8 */
static uint8_t        bs_cnt;       /* DAT_00479650 */
static const uint8_t *bs_floor;     /* lower bound for sanity */

/* ---------------------------------------------------------------- helpers */
static int      bs_underflowed;
static uint32_t bs_underflow_iter;
static inline void bs_refill(void)
{
    if (bs_ptr <= bs_floor) {
        bs_underflowed = 1;
        bs_buf = 0;
        bs_cnt = 8;
        return;
    }
    --bs_ptr;
    bs_buf = *bs_ptr;
    bs_cnt = 8;
}

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

static inline uint32_t bs_bits(uint8_t n)
{
    if (n == 0) return 0;
    if (n > 32) return 0;                   /* sanity */
    uint32_t val = 0;
    uint8_t shift = 0;
    while (n) {
        if (bs_cnt == 0) bs_refill();
        uint8_t take = (n < bs_cnt) ? n : bs_cnt;
        uint32_t chunk = (uint32_t)(k_mask_lut[take] & bs_buf);
        val |= chunk << shift;
        bs_buf >>= take;
        bs_cnt -= take;
        shift += take;
        n -= take;
    }
    return val;
}

/* Build 12-entry bit-width table from 6 trailer bytes.
 *
 * Per inner table-loops:
 * For each byte we extract a signed hi nibble (cVar3 = (int8_t)b >> 4)
 * and an unsigned lo nibble (b & 0x0F).
 * On the FIRST iteration only, the carry latches to hi[0]; subsequent
 * iterations use that same carry. Result:
 * out[0] = hi[0]
 * out[1] = lo[0] + hi[0]
 * out[2i] = hi[i] + hi[0] (i >= 1)
 * out[2i+1]= lo[i] + hi[0] (i >= 1)
 */
static void unpack_widths(uint8_t out[12], const uint8_t *six)
{
    int8_t carry = (int8_t)six[0] >> 4;          /* hi[0] (signed) */
    out[0] = (uint8_t)carry;
    out[1] = (uint8_t)((int8_t)(six[0] & 0x0F) + carry);
    for (int i = 1; i < 6; ++i) {
        int8_t hi = (int8_t)six[i] >> 4;
        int8_t lo = (int8_t)(six[i] & 0x0F);
        out[2*i + 0] = (uint8_t)(hi + carry);
        out[2*i + 1] = (uint8_t)(lo + carry);
    }
}

/* — precompute the three "base" tables. */
static void precompute_bases(void)
{
    /* base_mlen[0] = 2; base_mlen[i+1] = base_mlen[i] + (bits[i+1] ? 1<<bits[i+1] : 1) */
    base_mlen[0] = 2;
    for (int i = 1; i < 6; ++i) {
        uint8_t bw = k_mlen_bits[i];
        base_mlen[i] = base_mlen[i - 1] + (bw == 0 ? 1u : (1u << bw));
    }

    /* base_off: 4 groups of 3. In each group, starts fresh (no -1):
 * base[3g+0] = (1 << tab_off_bits[3g+0])
 * base[3g+1] = base[3g+0] + (1 << tab_off_bits[3g+1])
 * base[3g+2] = base[3g+1] + (1 << tab_off_bits[3g+2])
 */
    for (int g = 0; g < 4; ++g) {
        int i = g * 3;
        base_off[i + 0] = 1u << tab_off_bits[i + 0];
        base_off[i + 1] = base_off[i + 0] + (1u << tab_off_bits[i + 1]);
        base_off[i + 2] = base_off[i + 1] + (1u << tab_off_bits[i + 2]);
    }

    /* base_lit: 4 groups of 3, starting with (1<<bits)-1 instead of (1<<bits) */
    for (int g = 0; g < 4; ++g) {
        int i = g * 3;
        base_lit[i + 0] = (1u << tab_lit_bits[i + 0]) - 1u;
        base_lit[i + 1] = base_lit[i + 0] + (1u << tab_lit_bits[i + 1]);
        base_lit[i + 2] = base_lit[i + 1] + (1u << tab_lit_bits[i + 2]);
    }
}

/* -------------------------------------------------------------------- main */
void DepackPkv2Buffer(void *src_, void *dst_, void (*progress)(int))
{
    uint8_t *src = (uint8_t *)src_;
    uint8_t *dst = (uint8_t *)dst_;
    Pkv2Header *h = (Pkv2Header *)src;

    if (h->magic != PKV2_MAGIC) {
        fprintf(stderr, "[depack] bad PKv2 magic 0x%08X\n", h->magic);
        return;
    }
    uint32_t comp = h->compressed_size;
    uint32_t unp  = h->unpacked_size;
    if (comp < 32 || unp == 0) {
        fprintf(stderr, "[depack] sanity: comp=%u unp=%u — bail\n", comp, unp);
        return;
    }
    uint8_t *eot = src + comp - 1;       /* last payload byte */

    /* literal mode */
    if (*eot == 0) {
        memcpy(dst, src + 12, unp);
        if (progress) progress(100);
        return;
    }

    /* Reproduce the original side-effect: copy the last 12 bytes of the
 * compressed payload over the first 12 bytes of the buffer. For
 * in-place callers (where src == dst) this seeds the first 12 dst
 * bytes — the LZ77 decoder is expected to overwrite them with the
 * final iterations' literals/back-refs. */
    if (src == dst) memcpy(src, eot - 0x0C, 0x0C);

    /* Read the two 6-byte → 12-entry bit-width tables.
 * eot - 0x12 == &byte[comp-19] — six bytes for tab_off_bits
 * eot - 0x18 == &byte[comp-25] — six bytes for tab_lit_bits */
    unpack_widths(tab_off_bits, eot - 0x12);
    unpack_widths(tab_lit_bits, eot - 0x18);
    precompute_bases();


    /* Initial bit-stream state — read from the bytes right BEFORE the two
 * width tables (i.e. immediately above the literal/dictionary stream).
 * bs_cnt at offset (comp - 26) = eot - 0x19
 * bs_buf at offset (comp - 27) = eot - 0x1A
 * bs_ptr at offset (comp - 31) = eot - 0x1E
 */
    bs_cnt = *(eot - 0x19);
    bs_buf = *(eot - 0x1A);
    bs_ptr = eot - 0x1E;
    uint32_t init_lit;
    memcpy(&init_lit, bs_ptr, 4);                  /* T43b: avoid misaligned load */
    if (init_lit > unp) {
        fprintf(stderr, "[depack] init_lit=%u > unp=%u — bail\n", init_lit, unp);
        return;
    }

    /* The initial literal run sits just before the init_lit DWORD.
 * memcpy(dst + unp - init_lit, bs_ptr - init_lit, init_lit)
 * After the memcpy, the bit stream walks bytes <= (bs_ptr - init_lit - 1). */
    bs_ptr -= init_lit;
    /* In-place callers (src == dst) pre-load bytes 0..11 of the buffer with
 * the scratch (= the asset header, conveniently stashed at the end of
 * the compressed payload). The encoder is free to encode bit-stream
 * data right up to src+0, so we let bs_ptr walk down to the very start. */
    bs_floor = src;

    uint8_t *out = dst + (unp - init_lit);
    memmove(out, bs_ptr, init_lit);     /* may overlap with bs_ptr in-place */

    int      last_pct = -1;
    uint32_t iters    = 0;
    bs_underflowed = 0;
    bs_underflow_iter = 0;
    while (out > dst) {
        /* 1. Unary prefix (≤5) → group g */
        int g = bs_unary(5);

        /* 2. match_length */
        uint32_t mlen;
        uint8_t bw = k_mlen_bits[g];
        if (bw == 0) {
            mlen = base_mlen[g];                /* fixed: 2, 3, or 4 */
        } else {
            uint32_t x = bs_bits(bw);
            /* base shifted by -1 → use base_mlen[g-1] */
            mlen = base_mlen[g - 1] + 1 + x;
        }

        /* 3. clip g for the next two tables (12 entries, indexed by g*3+sub) */
        int gc = g > 3 ? 3 : g;

        /* 4. unary → sub_g, bits → match_offset */
        int sg  = bs_unary(2);
        int io  = gc * 3 + sg;
        uint32_t moff = bs_bits(tab_off_bits[io]) + 1;
        if (sg != 0) moff += base_off[io - 1];

        /* 5. unary → sub_g2, bits → literal_length */
        int sg2 = bs_unary(2);
        int il  = gc * 3 + sg2;
        uint32_t llen = bs_bits(tab_lit_bits[il]);
        if (sg2 != 0) llen += base_lit[il - 1] + 1;

        /* 6. back-ref copy. T127 audit (2026-05-27): clamp on overshoot
 * verified DEAD across all 1782 DANE_02.DTA entries — never
 * triggers in practice. Earlier port had a "1-byte short last
 * iteration" tolerance that turned out to be a WIP-era artifact
 * (likely fixed by subsequent bs_ptr/header parsing tweaks).
 * Now bail with log if it ever happens — safer than silent
 * truncation. */
        if (mlen > (uint32_t)(out - dst)) {
            fprintf(stderr, "[depack] iter %u: mlen=%u overshoots remaining %u — bail\n",
                    iters, mlen, (uint32_t)(out - dst));
            return;
        }
        if (mlen == 0) break;
        uint8_t *back = out + moff;
        for (uint32_t i = 0; i < mlen; ++i)
            *--out = *--back;

        /* 7. literal copy. T127 audit: same as mlen — DEAD clamp. */
        if (llen > (uint32_t)(out - dst)) {
            fprintf(stderr, "[depack] iter %u: llen=%u overshoots remaining %u — bail\n",
                    iters, llen, (uint32_t)(out - dst));
            return;
        }
        out     -= llen;
        bs_ptr  -= llen;
        if (bs_ptr < bs_floor) {
            fprintf(stderr, "[depack] iter %u: bs_ptr underflow — bail\n", iters);
            /* keep whatever we produced so the caller can inspect it */
            return;
        }
        /* In-place: src buffer == dst buffer. Near the end of decoding, the
 * source and destination ranges can overlap (bs_ptr just ahead of
 * out). Use memmove for defined behavior. */
        memmove(out, bs_ptr, llen);

        if (progress) {
            uint32_t done = unp - (uint32_t)(out - dst);
            int pct = (int)((done * 100u) / unp);
            if (pct != last_pct) { progress(pct); last_pct = pct; }
        }
        if (++iters > 10000000u) {
            fprintf(stderr, "[depack] runaway after %u iters — bail\n", iters);
            /* keep whatever we produced so the caller can inspect it */
            return;
        }
    }
    if (progress) progress(100);
}
