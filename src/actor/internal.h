/* src/actor/internal.h — symbols shared between actor sub-modules.
 *
 * The actor subsystem is split across several .c files (intern.c,
 * registration.c, list.c, walker.c, vm.c, render.c). Symbols that
 * cross those boundaries live here; nothing outside src/actor/ should
 * include this — public actor API is in include/wacki.h.
 */

#ifndef WACKI_ACTOR_INTERNAL_H
#define WACKI_ACTOR_INTERNAL_H

#include "wacki.h"

#include <stdint.h>

/* EOFF / EOFF8 and the ENT_OFF_* constants now live in
 * include/entity_offsets.h (auto-included via wacki.h). */

/* ---- registration table -------------------------------------------- *
 *
 * Per-frame entity dispatch table: 500 slots of (Entity*, kind, id).
 * Look-up is LIFO so re-registrations shadow older ones — needed for
 * scene transitions where a new scene's assets register over old ones
 * without an explicit unregister step.
 *
 * The list-management code (list.c) reads this table directly when
 * filtering "what to preserve across scene transitions" — actor click
 * payloads (kind=4, id=1 or 2) are protected through scene boundaries
 * by matching their owner against g_actor[0]/[1]. */
struct UpdateRegEntry {
    Entity   *e;
    uint16_t  kind;
    uint16_t  id;
};

#define ACTOR_UPDATE_TABLE_SIZE 500

extern struct UpdateRegEntry g_update_table[ACTOR_UPDATE_TABLE_SIZE];
extern int                   g_update_table_count;

#endif /* WACKI_ACTOR_INTERNAL_H */
