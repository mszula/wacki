/* src/util/rng.c — WackiRand: the engine's pseudo-random number generator.
 *
 * The original engine uses a 32-bit state advanced by ROL3 + a fixed
 * additive constant, then masked to 16 bits for the output. Same
 * advance and post-processing here so any per-tick distribution
 * properties scripts depend on (e.g. NPC fidget timing, dialog choice
 * weighting) match the original.
 *
 * NOTE: byte-identical output to the original requires the original's
 * Win32-specific seed (GetLocalTime + GetTimeZoneInformation hash).
 * The portable build uses time instead, so the OUTPUT SEQUENCE
 * differs run-to-run but distribution and per-call advance are the
 * same. Tests that need deterministic output call WackiRandSeed with
 * a fixed seed first.
 */

#include "wacki.h"

#include <stdint.h>
#include <time.h>

/* Magic additive constant from the original engine. ROL3 + this value
 * gives a single-step state advance with reasonable bit mixing. */
#define WACKI_RAND_ADDEND       0x3D8A479Cu
#define WACKI_RAND_ROL_BITS     3

/* Used as the unseeded fallback when WackiRandSeed has never been
 * called (defensive — every call site is supposed to either call
 * Seed or rely on the time bootstrap below). */
#define WACKI_RAND_DEFAULT_SEED 0xDEADBEEFu

static uint32_t s_rand_state  = 0;
static int      s_rand_seeded = 0;

void WackiRandSeed(uint32_t seed)
{
    s_rand_state  = seed ? seed : WACKI_RAND_DEFAULT_SEED;
    s_rand_seeded = 1;
}

uint32_t WackiRand(uint16_t bound)
{
    if (!s_rand_seeded) {
        WackiRandSeed((uint32_t)time(NULL));
    }

    /* ROL3 + magic advance — same as the original 1998 engine. */
    s_rand_state = ((s_rand_state << WACKI_RAND_ROL_BITS) |
                    (s_rand_state >> (32 - WACKI_RAND_ROL_BITS))) +
                   WACKI_RAND_ADDEND;

    uint32_t low = s_rand_state & 0xFFFFu;
    uint32_t b   = (uint32_t)bound;

    if (b <= low) {
        /* Map to [0, bound) via next-power-of-2 mask + subtract.
 * This is the same trick the original uses — cheaper than a
 * modulus and preserves uniformity when bound is a power of 2. */
        uint32_t pow2 = 1;
        if (b > 1) {
            while (pow2 < b) pow2 <<= 1;
        }
        low &= (pow2 - 1);
        if (b <= low) low -= b;
    }
    return low;
}
