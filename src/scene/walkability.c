/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/scene/walkability.c — per-pixel walkability lookup.
 *
 * is_walkable_at consults the current scene's FLD bitmap to determine
 * whether a given screen-space pixel sits on a walkable surface. Used
 * by the walker bind path's straight-line clip and by the waypoint
 * pathfinder.
 *
 * Returns 1 inside the walkable area, 0 outside (including off-screen).
 * If no FLD has been loaded for the current scene (g_walk_fld_pixels
 * is NULL), the function falls back to a generous bounding box so
 * cutscenes and special rooms without an FLD still let walkers move.
 */

#include "wacki.h"

#include <stdint.h>

extern const uint8_t *g_walk_fld_pixels;
extern int            g_walk_fld_w;
extern int            g_walk_fld_h;
extern int            g_walk_fld_stride;
extern int            g_walk_fld_ox;
extern int            g_walk_fld_oy;
extern int            g_walk_x0, g_walk_x1, g_walk_y0, g_walk_y1;

/* T2 phase B: walkability test using globals (set by play_demo_scene at
 * scene load). 1bpp packed .fld mask if loaded, else fallback bbox.
 * Replaces the IS_WALKABLE macro that was a play_demo_scene-local. */
int is_walkable_at(int sx, int sy)
{
    if (g_walk_fld_pixels) {
        if (sx < g_walk_fld_ox || sx >= g_walk_fld_ox + g_walk_fld_w) return 0;
        if (sy < g_walk_fld_oy || sy >= g_walk_fld_oy + g_walk_fld_h) return 0;
        size_t byte_idx = (size_t)(sy - g_walk_fld_oy) * g_walk_fld_stride
                        + (sx - g_walk_fld_ox) / 8;
        uint8_t bit = (uint8_t)(0x80u >> ((sx - g_walk_fld_ox) & 7));
        return (g_walk_fld_pixels[byte_idx] & bit) != 0;
    }
    return (sx >= g_walk_x0 && sx < g_walk_x1 &&
            sy >= g_walk_y0 && sy < g_walk_y1);
}
