/* src/actor/intern.c — pointer-slot intern table.
 *
 * Entity's pointer fields (atlas at +0x28, bytecode at +0x2C, pixels at
 * +0x14, etc.) are 4-byte slot IDs to preserve the original 32-bit
 * binary layout. On a 64-bit host, real pointers are 8 bytes — so each
 * unique pointer is interned into a small table; the returned slot ID
 * fits in 32 bits, and resolve() round-trips back to the real pointer.
 *
 * Why this matters: bug "ptr-truncate" — writing a 64-bit pointer as
 *   *(uint32_t *)(eb + 0x28) = (uint32_t)(uintptr_t)atlas
 * silently truncates the upper 32 bits, then a later deref crashes on
 * a bogus 32-bit address. The slot table is the fix; bypassing it on
 * any pointer-in-u32 field re-introduces the bug.
 *
 * Slot 0 is reserved for NULL so a memset-zeroed Entity reads its
 * pointer fields back as NULL via resolve(0).
 */

#include "wacki.h"

#include <stddef.h>
#include <stdint.h>

/* Capacity tuned for one full scene's working set: 2 actors + ~60 props +
 * dialog atlas + walker scripts + ~10 panels. 1024 has been comfortable
 * since the port started and would need a major scene-system rework to
 * exhaust. NOTE: when full, intern() returns 0 (the NULL slot) — callers
 * that get back 0 after passing a non-NULL pointer should treat it as a
 * failure rather than a "this is NULL" signal. */
#define ENT_PTR_SLOTS 1024

static void *g_ent_ptr_tbl[ENT_PTR_SLOTS];
static int   g_ent_ptr_n = 1;       /* slot 0 reserved for NULL */

uint32_t ent_ptr_intern(void *p)
{
    if (!p) return 0;
    for (int i = 1; i < g_ent_ptr_n; ++i) {
        if (g_ent_ptr_tbl[i] == p) return (uint32_t)i;
    }
    if (g_ent_ptr_n >= ENT_PTR_SLOTS) return 0;
    g_ent_ptr_tbl[g_ent_ptr_n] = p;
    return (uint32_t)g_ent_ptr_n++;
}

void *ent_ptr_resolve(uint32_t slot)
{
    if (slot == 0 || slot >= (uint32_t)g_ent_ptr_n) return NULL;
    return g_ent_ptr_tbl[slot];
}
