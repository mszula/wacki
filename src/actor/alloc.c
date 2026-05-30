/* src/actor/alloc.c — entity lifecycle (allocate, init bitmap, free).
 *
 * Used by the script VM (SPAWN), the walk-behind mask builder, and the
 * speech-balloon path. The allocation strategy is dictated by the
 * `group_flags` argument:
 *
 * bit 0x01 set → primary plane (the sprite's pixel buffer)
 * bit 0x04 set → secondary plane (mask / shadow); doubles the buffer
 *
 * Most call sites pass `0x01` for a vanilla single-buffer entity. The
 * dual-buffer mode (0x05) backs entities that need a separate shadow
 * pass — historically used by the original engine's shadow renderer,
 * which the port doesn't currently exercise.
 */

#include "wacki.h"

#include <stdint.h>
#include <string.h>

extern void *xmalloc(uint32_t sz);
extern void  xfree  (void *p);

#define EGROUP_PRIMARY_PLANE    0x0001
#define EGROUP_SECONDARY_PLANE  0x0004

#define EFLAGS1_ALLOCATED       0x01

static Entity *init_entity_bitmap(Entity *e, uint16_t w, uint16_t h)
{
    if (!e) return NULL;

    uint32_t pixels    = (uint32_t)w * (uint32_t)h;
    uint32_t primary   = (e->group_flags & EGROUP_PRIMARY_PLANE)   ? pixels : 0;
    uint32_t secondary = (e->group_flags & EGROUP_SECONDARY_PLANE) ? pixels : 0;
    uint32_t total     = primary + secondary;

    if (total) {
        if (e->pixels) xfree(e->pixels);
        uint8_t *buf = (uint8_t *)xmalloc(total);
        if (!buf) {
            xfree(e);
            return NULL;
        }
        e->pixels = buf;
    }

    e->flags1 |= EFLAGS1_ALLOCATED;
    e->width   = w;
    e->height  = h;
    return e;
}

Entity *AllocEntity(uint16_t w, uint16_t h, uint16_t kind, uint16_t flags)
{
    Entity *e = (Entity *)xmalloc(sizeof *e);
    if (!e) return NULL;

    memset(e, 0, sizeof *e);
    e->kind        = kind;
    e->group_flags = flags;

    return init_entity_bitmap(e, w, h);
}

void FreeEntity(Entity *e)
{
    if (!e) return;
    if (e->pixels) xfree(e->pixels);
    xfree(e);
}
