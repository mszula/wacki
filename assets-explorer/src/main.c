/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * assets-explorer/src/main.c - Wacki Assets Explorer.
 *
 * A standalone Nuklear + SDL2 tool for browsing the game's .dta archives.
 * Phase 0: mount + list + filter + hex + raw export. Phase 1: faithful preview
 * of graphics - ANIM (animated), PIC (backgrounds), PAL (swatch grid) - drawn
 * through the engine's real loaders/decoders (LoadAssetFromDtaBase,
 * DepackRleFrame) and a chosen palette, then uploaded to an SDL_Texture.
 *
 * It links the engine's data core + asset/graphics/font loaders and a tiny
 * stub TU (stubs.c) for the few scene/VM/platform symbols those reference -
 * NOT the game loop. This TU owns the single Nuklear implementation.
 * SDL_MAIN_HANDLED: we're a console-launched tool, not an app bundle. */

#define SDL_MAIN_HANDLED

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#define NK_SDL_RENDERER_IMPLEMENTATION
#include "nuklear.h"
/* The SDL_Renderer backend includes SDL through this macro. Pin it to <SDL.h> to
 * match our sdl2-config include path (its dir holds SDL.h directly, not
 * SDL2/SDL.h). Without this, newer nuklear releases default to <SDL2/SDL.h> and
 * fail to compile here. */
#define NK_SDL_RENDERER_SDL_H <SDL.h>
#include "nuklear_sdl_renderer.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"             /* PNG export for --dump */

#define MSF_GIF_IMPL
#include "msf_gif.h"                      /* animated GIF export for ANIM */

#include "tinyfiledialogs.h"             /* native Save As dialog */

#include "catalog.h"
#include "hexdump.h"
#include "render.h"
#include "vaudio.h"
#include "vflic.h"
#include "wacki/types.h"                 /* AnimAsset */
#include "wacki/api.h"                   /* LoadAssetFromDtaBase, FreeAsset */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Portable single-directory create. POSIX mkdir takes (path, mode); the Windows
 * (mingw) mkdir in <direct.h> takes (path) only — calling the POSIX form there
 * is a compile error. */
#ifdef _WIN32
#  include <direct.h>
#  define make_dir(p) _mkdir(p)
#else
#  define make_dir(p) mkdir((p), 0755)
#endif

#define DEFAULT_DTA      "data/DANE_02.DTA"
#define EXPORT_DIR       "viewer-export"
#define MAX_ARCHIVES     64
#define MAX_PALETTES     64
#define HEX_MAX_LINES    256
#define PREVIEW_MAX_W    440.0f
#define PREVIEW_MAX_H    330.0f
#define PREVIEW_MAX_ZOOM 8.0f
#define ANIM_FRAME_MS    120
#define WIN_W            1180
#define WIN_H            720
#define LIST_W           240             /* fixed width of the left list panel */

/* ---- state -------------------------------------------------------------- */

static SDL_Renderer *g_ren = NULL;       /* set in main, used by upload_image */
static const struct nk_user_font *g_font_sm = NULL;   /* body text */
static const struct nk_user_font *g_font_lg = NULL;   /* headings */
static struct nk_context         *g_ctx     = NULL;   /* for font reload on DPI change */
static float                      g_cur_scale = 0.0f; /* current render/font scale */

static char     g_archives[MAX_ARCHIVES][512];
static int      g_archive_count = 0;
static int      g_want_open = 0;         /* deferred: open the "locate .DTA" dialog after render */

static char     g_filter[64] = "";
static int      g_filter_focus = 0;      /* filter edit box has keyboard focus */
static int      g_selected   = -1;
static int      g_type_filter = -1;      /* -1 = all, else an AssetType */
static int      g_sort = 0;              /* 0 = type, 1 = name, 2 = offset */
static int      g_order[8192];           /* display order (sorted catalog indices) */
static int      g_order_n = 0;
static int      g_type_counts[AT__COUNT];
static int      g_visible = 0;           /* entries passing the current filter */

static void    *g_buf   = NULL;          /* depacked bytes of g_selected */
static uint32_t g_sz    = 0;
static const char *g_magic = NULL;
static uint32_t g_comp  = 0, g_unp = 0;
static char     g_status[256] = "";

/* preview (Phase 1) */
static AnimAsset   *g_anim       = NULL; /* loaded ANIM, if selection is one */
static int          g_anim_frame = 0;
static int          g_anim_play  = 0;
static uint32_t     g_anim_last  = 0;
static SDL_Texture *g_tex        = NULL; /* current preview texture */
static int          g_tex_w = 0, g_tex_h = 0;
static int          g_has_preview = 0;
static float        g_zoom = 1.0f;       /* preview zoom multiplier on auto-fit */
static int          g_anim_ms = 120;     /* per-frame delay (ms) when playing */
static int          g_hex_popup = 0;     /* hex byte view shown on demand */
static int          g_preview_h = 330;   /* preview box height; scales with window */

/* palettes available in the mounted archive */
static char     g_palettes[MAX_PALETTES][16];
static int      g_pal_count   = 0;
static int      g_pal_current = -1;      /* index into g_palettes, -1 = grayscale */

/* cutscene (AVI) mode - set when the mounted file is a RIFF...AVI, not a DTA */
static VFlic   *g_flic       = NULL;
static int      g_avi_mode   = 0;
static int      g_flic_frame = 0;
static int      g_flic_play  = 0;
static uint32_t g_flic_last  = 0;

static char     g_cur_path[512] = "";    /* mounted file path (DTA or AVI) */

/* ---- helpers ------------------------------------------------------------ */

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int ci_contains(const char *hay, const char *needle)
{
    if (!needle[0]) return 1;
    size_t nl = strlen(needle);
    for (; *hay; ++hay) {
        size_t i = 0;
        for (; i < nl; ++i) {
            char a = hay[i], b = needle[i];
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (b >= 'a' && b <= 'z') b = (char)(b - 32);
            if (!hay[i] || a != b) break;
        }
        if (i == nl) return 1;
    }
    return 0;
}

static int entry_matches(const CatalogEntry *ce)
{
    if (g_type_filter >= 0 && (int)ce->type != g_type_filter) return 0;
    if (!g_filter[0]) return 1;
    return ci_contains(ce->name, g_filter) ||
           ci_contains(catalog_type_label(ce->type), g_filter);
}

static int is_graphic(AssetType t)
{
    return t == AT_ANIM || t == AT_PIC || t == AT_PAL;
}

static int cmp_order(const void *pa, const void *pb)
{
    const CatalogEntry *a = catalog_entry(*(const int *)pa);
    const CatalogEntry *b = catalog_entry(*(const int *)pb);
    if (!a || !b) return 0;
    if (g_sort == 1) return strcasecmp(a->name, b->name);
    if (g_sort == 2) return (a->offset > b->offset) - (a->offset < b->offset);
    if (a->type != b->type) return (int)a->type - (int)b->type;   /* group by type */
    return strcasecmp(a->name, b->name);
}

/* Rebuild the sorted display order + per-type tallies (on mount / sort change). */
static void rebuild_order(void)
{
    int cap = (int)(sizeof g_order / sizeof g_order[0]);
    int n = catalog_count();
    if (n > cap) n = cap;
    g_order_n = n;
    for (int i = 0; i < n; ++i) g_order[i] = i;
    qsort(g_order, (size_t)n, sizeof g_order[0], cmp_order);

    memset(g_type_counts, 0, sizeof g_type_counts);
    int total = catalog_count();
    for (int i = 0; i < total; ++i) {
        const CatalogEntry *ce = catalog_entry(i);
        if (ce) g_type_counts[ce->type]++;
    }
}

/* Clean dark theme - replaces Nuklear's gray default with a modern dev-tool
 * look (soft borders, rounded controls, blue selection accent). */
static void apply_theme(struct nk_context *ctx)
{
    struct nk_color t[NK_COLOR_COUNT];
    t[NK_COLOR_TEXT]                    = nk_rgb(214, 216, 222);
    t[NK_COLOR_WINDOW]                  = nk_rgb(26, 27, 32);
    t[NK_COLOR_HEADER]                  = nk_rgb(32, 33, 39);
    t[NK_COLOR_BORDER]                  = nk_rgb(48, 50, 58);
    t[NK_COLOR_BUTTON]                  = nk_rgb(42, 44, 52);
    t[NK_COLOR_BUTTON_HOVER]            = nk_rgb(54, 57, 67);
    t[NK_COLOR_BUTTON_ACTIVE]           = nk_rgb(66, 70, 84);
    t[NK_COLOR_TOGGLE]                  = nk_rgb(42, 44, 52);
    t[NK_COLOR_TOGGLE_HOVER]            = nk_rgb(54, 57, 67);
    t[NK_COLOR_TOGGLE_CURSOR]           = nk_rgb(80, 150, 220);
    t[NK_COLOR_SELECT]                  = nk_rgb(30, 31, 37);
    t[NK_COLOR_SELECT_ACTIVE]           = nk_rgb(48, 84, 132);
    t[NK_COLOR_SLIDER]                  = nk_rgb(30, 31, 37);
    t[NK_COLOR_SLIDER_CURSOR]           = nk_rgb(80, 150, 220);
    t[NK_COLOR_SLIDER_CURSOR_HOVER]     = nk_rgb(96, 166, 236);
    t[NK_COLOR_SLIDER_CURSOR_ACTIVE]    = nk_rgb(110, 180, 250);
    t[NK_COLOR_PROPERTY]                = nk_rgb(38, 40, 48);
    t[NK_COLOR_EDIT]                    = nk_rgb(20, 21, 26);
    t[NK_COLOR_EDIT_CURSOR]             = nk_rgb(214, 216, 222);
    t[NK_COLOR_COMBO]                   = nk_rgb(38, 40, 48);
    t[NK_COLOR_CHART]                   = nk_rgb(30, 31, 37);
    t[NK_COLOR_CHART_COLOR]             = nk_rgb(80, 150, 220);
    t[NK_COLOR_CHART_COLOR_HIGHLIGHT]   = nk_rgb(236, 110, 110);
    t[NK_COLOR_SCROLLBAR]               = nk_rgb(24, 25, 30);
    t[NK_COLOR_SCROLLBAR_CURSOR]        = nk_rgb(56, 59, 70);
    t[NK_COLOR_SCROLLBAR_CURSOR_HOVER]  = nk_rgb(70, 74, 88);
    t[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = nk_rgb(84, 90, 106);
    t[NK_COLOR_TAB_HEADER]              = nk_rgb(42, 44, 52);
    t[NK_COLOR_KNOB]                    = nk_rgb(42, 44, 52);
    t[NK_COLOR_KNOB_CURSOR]             = nk_rgb(80, 150, 220);
    t[NK_COLOR_KNOB_CURSOR_HOVER]       = nk_rgb(96, 166, 236);
    t[NK_COLOR_KNOB_CURSOR_ACTIVE]      = nk_rgb(110, 180, 250);
    nk_style_from_table(ctx, t);

    ctx->style.window.rounding       = 0;
    ctx->style.window.border         = 1.0f;
    ctx->style.window.padding        = nk_vec2(10, 8);
    ctx->style.window.group_padding  = nk_vec2(8, 8);
    ctx->style.window.spacing        = nk_vec2(7, 6);
    ctx->style.window.group_border   = 1.0f;
    ctx->style.window.background       = nk_rgb(26, 27, 32);
    ctx->style.window.fixed_background = nk_style_item_color(nk_rgb(26, 27, 32));
    ctx->style.button.rounding       = 5;
    ctx->style.button.padding        = nk_vec2(8, 4);
    ctx->style.button.border         = 0;
    ctx->style.edit.rounding         = 5;
    ctx->style.combo.rounding        = 5;
    ctx->style.property.rounding     = 5;
    ctx->style.selectable.rounding   = 5;
    ctx->style.selectable.padding    = nk_vec2(7, 4);
}

static int  gif_export_anim(AnimAsset *a, const char *path, int centisecs);
static void png_name(const char *name, char *out, size_t cap);
static int  write_png_img(const char *path, ViewImage *img);

/* ---- preview ------------------------------------------------------------ */

static void destroy_tex(void)
{
    if (g_tex) { SDL_DestroyTexture(g_tex); g_tex = NULL; }
    g_tex_w = g_tex_h = 0;
    g_has_preview = 0;
}

static void upload_image(const ViewImage *img)
{
    destroy_tex();
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32,
                              SDL_TEXTUREACCESS_STATIC, img->w, img->h);
    if (!g_tex) return;
    SDL_SetTextureBlendMode(g_tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(g_tex, SDL_ScaleModeNearest);   /* crisp pixels */
    SDL_UpdateTexture(g_tex, NULL, img->rgba, img->w * 4);
    g_tex_w = img->w;
    g_tex_h = img->h;
    g_has_preview = 1;
}

/* Re-render the current selection's preview into g_tex (palette/frame change
 * or new selection). No-op for non-graphic types. */
static void rebuild_preview(void)
{
    destroy_tex();
    const CatalogEntry *ce = (g_selected >= 0) ? catalog_entry(g_selected) : NULL;
    if (!ce) return;

    ViewImage img = {0};
    int ok = 0;
    if (ce->type == AT_ANIM && g_anim) {
        if (g_anim->frame_count > 0)
            g_anim_frame %= g_anim->frame_count;
        ok = view_render_anim_frame(g_anim, g_anim_frame, &img);
    } else if ((ce->type == AT_MASK || ce->type == AT_FILD) && g_anim) {
        if (g_anim->frame_count > 0)
            g_anim_frame %= g_anim->frame_count;
        ok = view_render_mask(g_anim, g_anim_frame, &img);
    } else if (ce->type == AT_PIC) {
        ok = view_render_pic(g_buf, g_sz, &img);
    } else if (ce->type == AT_PAL) {
        ok = view_render_pal(g_buf, g_sz, 20, &img);
    } else if (ce->type == AT_WAV) {
        ok = vaudio_waveform(g_buf, g_sz, 420, 130, &img);
    }
    if (ok) { upload_image(&img); view_image_free(&img); }
}

/* Render the current cutscene frame into g_tex. */
static void flic_rebuild(void)
{
    destroy_tex();
    if (!g_flic) return;
    ViewImage img = {0};
    if (vflic_render_frame(g_flic, g_flic_frame, &img)) {
        upload_image(&img);
        view_image_free(&img);
    }
}

/* ---- palette ------------------------------------------------------------ */

static void scan_palettes(void)
{
    g_pal_count = 0;
    int n = catalog_count();
    for (int i = 0; i < n && g_pal_count < MAX_PALETTES; ++i) {
        const CatalogEntry *ce = catalog_entry(i);
        if (ce->type == AT_PAL)
            snprintf(g_palettes[g_pal_count++], 16, "%s", ce->name);
    }
}

/* Install palette `which` (index into g_palettes, or -1 for grayscale) and
 * re-render. */
static void apply_palette(int which)
{
    if (which < 0 || which >= g_pal_count) {
        viewer_set_palette(NULL);
        g_pal_current = -1;
    } else {
        int n = catalog_count();
        for (int i = 0; i < n; ++i) {
            const CatalogEntry *ce = catalog_entry(i);
            if (ce->type != AT_PAL || strcmp(ce->name, g_palettes[which]) != 0)
                continue;
            void *pb = NULL; uint32_t ps = 0;
            if (catalog_load(i, &pb, &ps)) {
                if (ps >= 768) viewer_set_palette((const uint8_t *)pb);
                catalog_free(pb);
            }
            break;
        }
        g_pal_current = which;
    }
    rebuild_preview();
}

/* ---- selection ---------------------------------------------------------- */

static void drop_loaded(void)
{
    if (g_buf) catalog_free(g_buf);
    g_buf = NULL; g_sz = 0; g_magic = NULL; g_comp = g_unp = 0;
    if (g_anim) { FreeAsset(g_anim); g_anim = NULL; }
    g_anim_frame = 0; g_anim_play = 0;
    vaudio_stop();
    destroy_tex();
}

static void select_entry(int idx)
{
    drop_loaded();
    g_selected = idx;
    if (idx < 0) return;

    const CatalogEntry *ce = catalog_entry(idx);
    catalog_entry_sizes(idx, &g_comp, &g_unp);
    if (catalog_load(idx, &g_buf, &g_sz)) {
        g_magic = catalog_magic_label(g_buf, g_sz);
    } else {
        snprintf(g_status, sizeof g_status, "load failed: %s", ce ? ce->name : "?");
        return;
    }
    /* ANIM/MASK/FILD need the engine's structured loader for frame/pixel ptrs. */
    if (ce->type == AT_ANIM || ce->type == AT_MASK || ce->type == AT_FILD)
        g_anim = LoadAssetFromDtaBase(ce->name);
    if (ce->type == AT_ANIM && g_anim && g_anim->frame_count > 1)
        g_anim_play = 1;                 /* auto-play animations on select */
    if (ce->type == AT_WAV)
        vaudio_play(g_buf, g_sz);        /* auto-play sounds on select */

    rebuild_preview();
}

static int mount_archive(const char *path)   /* returns 1 on success, 0 on failure */
{
    /* Tear down any previous mount (DTA or AVI). */
    if (g_flic) { vflic_close(g_flic); g_flic = NULL; }
    g_avi_mode = 0;
    drop_loaded();
    g_selected = -1;
    snprintf(g_cur_path, sizeof g_cur_path, "%s", path);

    /* Cutscene? Open as AVI and switch to the player instead of a DTA mount. */
    if (vflic_is_avi(path)) {
        g_flic = vflic_open(path);
        if (g_flic) {
            g_avi_mode = 1;
            g_flic_frame = 0; g_flic_play = 0;
            flic_rebuild();
            snprintf(g_status, sizeof g_status, "cutscene %s - %d frames %dx%d",
                     basename_of(path), vflic_frame_count(g_flic),
                     vflic_width(g_flic), vflic_height(g_flic));
            return 1;
        }
        snprintf(g_status, sizeof g_status, "AVI failed to open: %s", path);
        return 0;
    }

    if (!catalog_open(path)) {
        snprintf(g_status, sizeof g_status, "failed to mount %s", path);
        return 0;
    }
    select_entry(-1);
    g_selected = -1;
    rebuild_order();

    /* Default palette: prefer PALETA.PAL, else the first .pal, else grayscale. */
    scan_palettes();
    int def = -1;
    for (int i = 0; i < g_pal_count; ++i)
        if (!strcasecmp(g_palettes[i], "PALETA.PAL")) { def = i; break; }
    if (def < 0 && g_pal_count > 0) def = 0;
    apply_palette(def);

    snprintf(g_status, sizeof g_status, "mounted %s - %d entries, %d palettes",
             basename_of(path), catalog_count(), g_pal_count);
    return 1;
}

static void scan_archives(const char *dta_path)
{
    g_archive_count = 0;
    char dir[512];
    const char *slash = strrchr(dta_path, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - dta_path);
        if (dlen >= sizeof dir) dlen = sizeof dir - 1;
        memcpy(dir, dta_path, dlen);
        dir[dlen] = 0;
    } else { dir[0] = '.'; dir[1] = 0; }

    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && g_archive_count < MAX_ARCHIVES) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcasecmp(dot, ".dta") != 0) continue;
        snprintf(g_archives[g_archive_count++], sizeof g_archives[0],
                 "%s/%s", dir, e->d_name);
    }
    closedir(d);
}

/* Native Save As dialog; returns the chosen path or NULL if cancelled. */
static const char *ask_save_path(const char *default_name, const char *pattern,
                                 const char *desc)
{
    const char *filters[1] = { pattern };
    return tinyfd_saveFileDialog("Save as", default_name, 1, filters, desc);
}

/* Native "open a .DTA" dialog; returns the chosen path or NULL if cancelled.
 * The default path MUST be empty (not DEFAULT_DTA): tinyfd feeds the directory
 * part to AppleScript as `default location`, and a relative/nonexistent dir like
 * "data" can't be coerced to an alias -> osascript error -1700 and no dialog. An
 * empty default makes tinyfd omit the clause, so the picker just opens. */
static const char *ask_open_dta(void)
{
    const char *filters[1] = { "*.dta" };
    return tinyfd_openFileDialog("Locate Wacki game data - pick a .DTA file",
                                 "", 1, filters, "Wacki .DTA archive", 0);
}

static void export_selected(void)
{
    const CatalogEntry *ce = catalog_entry(g_selected);
    if (!ce || !g_buf) return;
    const char *path = ask_save_path(ce->name, "*.*", "file");
    if (!path) return;
    if (hexdump_export_raw(path, g_buf, g_sz))
        snprintf(g_status, sizeof g_status, "saved %s (%u B)", basename_of(path), g_sz);
    else
        snprintf(g_status, sizeof g_status, "save failed");
}

/* ---- UI ----------------------------------------------------------------- */

/* ---- vector icons (Nuklear's default font has none) --------------------- */

typedef enum {
    IC_PLAY, IC_STOP, IC_PREV, IC_NEXT,
    IC_DOWNLOAD, IC_IMAGE, IC_GRID, IC_FILM, IC_FILE,
    IC_SEARCH, IC_BOX, IC_PALETTE
} IconType;

static void draw_icon(struct nk_command_buffer *c, struct nk_rect r, IconType ic,
                      struct nk_color col)
{
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f;
    float s = NK_MIN(r.w, r.h) * 0.5f;
    switch (ic) {
    case IC_PLAY:
        nk_fill_triangle(c, cx - s*0.45f, cy - s*0.7f, cx - s*0.45f, cy + s*0.7f,
                         cx + s*0.65f, cy, col);
        break;
    case IC_STOP:
        nk_fill_rect(c, nk_rect(cx - s*0.55f, cy - s*0.55f, s*1.1f, s*1.1f), 1, col);
        break;
    case IC_PREV:
        nk_fill_triangle(c, cx + s*0.5f, cy - s*0.7f, cx + s*0.5f, cy + s*0.7f,
                         cx - s*0.5f, cy, col);
        nk_fill_rect(c, nk_rect(cx - s*0.7f, cy - s*0.7f, s*0.22f, s*1.4f), 0, col);
        break;
    case IC_NEXT:
        nk_fill_triangle(c, cx - s*0.5f, cy - s*0.7f, cx - s*0.5f, cy + s*0.7f,
                         cx + s*0.5f, cy, col);
        nk_fill_rect(c, nk_rect(cx + s*0.48f, cy - s*0.7f, s*0.22f, s*1.4f), 0, col);
        break;
    case IC_DOWNLOAD:
        nk_stroke_line(c, cx, cy - s*0.8f, cx, cy + s*0.3f, 1.6f, col);
        nk_fill_triangle(c, cx - s*0.45f, cy - s*0.05f, cx + s*0.45f, cy - s*0.05f,
                         cx, cy + s*0.5f, col);
        nk_stroke_line(c, cx - s*0.7f, cy + s*0.8f, cx + s*0.7f, cy + s*0.8f, 1.6f, col);
        break;
    case IC_IMAGE:
        nk_stroke_rect(c, nk_rect(cx - s*0.8f, cy - s*0.7f, s*1.6f, s*1.4f), 1, 1.4f, col);
        nk_fill_circle(c, nk_rect(cx - s*0.4f, cy - s*0.45f, s*0.4f, s*0.4f), col);
        nk_fill_triangle(c, cx - s*0.7f, cy + s*0.6f, cx, cy - s*0.1f, cx + s*0.7f, cy + s*0.6f, col);
        break;
    case IC_GRID:
        for (int gy = 0; gy < 2; ++gy) for (int gx = 0; gx < 2; ++gx)
            nk_fill_rect(c, nk_rect(cx - s*0.7f + gx*s*0.8f, cy - s*0.7f + gy*s*0.8f,
                                    s*0.55f, s*0.55f), 1, col);
        break;
    case IC_FILM:
        nk_stroke_rect(c, nk_rect(cx - s*0.8f, cy - s*0.6f, s*1.6f, s*1.2f), 1, 1.4f, col);
        for (int i = 0; i < 3; ++i)
            nk_stroke_line(c, cx - s*0.4f + i*s*0.4f, cy - s*0.6f,
                           cx - s*0.4f + i*s*0.4f, cy + s*0.6f, 1.0f, col);
        break;
    case IC_FILE:
        nk_stroke_rect(c, nk_rect(cx - s*0.6f, cy - s*0.8f, s*1.2f, s*1.6f), 1, 1.4f, col);
        nk_stroke_line(c, cx - s*0.3f, cy - s*0.3f, cx + s*0.3f, cy - s*0.3f, 1.0f, col);
        nk_stroke_line(c, cx - s*0.3f, cy + s*0.1f, cx + s*0.3f, cy + s*0.1f, 1.0f, col);
        break;
    case IC_SEARCH:
        nk_stroke_circle(c, nk_rect(cx - s*0.7f, cy - s*0.7f, s*1.1f, s*1.1f), 1.6f, col);
        nk_stroke_line(c, cx + s*0.25f, cy + s*0.25f, cx + s*0.75f, cy + s*0.75f, 1.8f, col);
        break;
    case IC_PALETTE:
        nk_stroke_circle(c, nk_rect(cx - s*0.8f, cy - s*0.8f, s*1.6f, s*1.6f), 1.4f, col);
        nk_fill_circle(c, nk_rect(cx - s*0.5f, cy - s*0.5f, s*0.3f, s*0.3f), col);
        nk_fill_circle(c, nk_rect(cx + s*0.1f, cy - s*0.55f, s*0.3f, s*0.3f), col);
        break;
    case IC_BOX:
    default:
        nk_stroke_rect(c, nk_rect(cx - s*0.8f, cy - s*0.7f, s*1.6f, s*1.4f), 1, 1.4f, col);
        nk_stroke_line(c, cx - s*0.8f, cy - s*0.2f, cx + s*0.8f, cy - s*0.2f, 1.0f, col);
        break;
    }
}

/* Button with a leading vector icon + optional label; returns 1 on click. */
static int icon_button(struct nk_context *ctx, IconType ic, const char *label)
{
    struct nk_rect b;
    if (nk_widget(&b, ctx) == NK_WIDGET_INVALID) return 0;
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    int hov = nk_input_is_mouse_hovering_rect(&ctx->input, b);
    int clk = nk_input_is_mouse_click_in_rect(&ctx->input, NK_BUTTON_LEFT, b);
    nk_fill_rect(canvas, b, 5.0f, hov ? nk_rgb(54, 57, 67) : nk_rgb(42, 44, 52));
    struct nk_color fg = nk_rgb(206, 208, 216);
    int has_label = label && label[0];
    if (has_label) {
        const struct nk_user_font *f = ctx->style.font;
        float tw = f->width(f->userdata, f->height, label, (int)strlen(label));
        float total = 16.0f + 6.0f + tw;
        float ix = b.x + (b.w - total) / 2.0f;
        draw_icon(canvas, nk_rect(ix, b.y + (b.h - 14) / 2.0f, 14, 14), ic, fg);
        nk_draw_text(canvas, nk_rect(ix + 22, b.y + (b.h - f->height) / 2.0f, tw + 4, f->height),
                     label, (int)strlen(label), f, nk_rgb(42, 44, 52), nk_rgb(218, 220, 226));
    } else {
        draw_icon(canvas, nk_rect(b.x + (b.w - 14) / 2.0f, b.y + (b.h - 14) / 2.0f, 14, 14), ic, fg);
    }
    return clk;
}

static void draw_topbar(struct nk_context *ctx)
{
    static const char *SORT_LABEL[3] = { "Sort: type", "Sort: name", "Sort: offset" };
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    const struct nk_user_font *f = ctx->style.font;
    nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 6);

    /* Title with app icon. */
    nk_layout_row_push(ctx, 0.19f);
    {
        struct nk_rect tb;
        if (nk_widget(&tb, ctx) != NK_WIDGET_INVALID) {
            draw_icon(canvas, nk_rect(tb.x + 2, tb.y + (tb.h - 16) / 2.0f, 16, 16),
                      IC_BOX, nk_rgb(110, 160, 235));
            nk_draw_text(canvas, nk_rect(tb.x + 24, tb.y + (tb.h - f->height) / 2.0f, tb.w - 26, f->height),
                         "Wacki Explorer", 14, f, nk_rgb(26, 27, 32), nk_rgb(224, 226, 232));
        }
    }

    /* Archive picker — its first item locates a .DTA anywhere on disk; below it,
     * the other archives found next to the current one. */
    nk_layout_row_push(ctx, 0.15f);
    const char *arch_label = (catalog_count() || g_avi_mode)
                             ? basename_of(g_cur_path) : "Open .DTA...";
    if (nk_combo_begin_label(ctx, arch_label, nk_vec2(300, 320))) {
        nk_layout_row_dynamic(ctx, 22, 1);
        if (nk_combo_item_label(ctx, "Open .DTA...", NK_TEXT_LEFT)) g_want_open = 1;
        for (int i = 0; i < g_archive_count; ++i)
            if (nk_combo_item_label(ctx, basename_of(g_archives[i]), NK_TEXT_LEFT))
                mount_archive(g_archives[i]);
        nk_combo_end(ctx);
    }

    /* Type filter with per-type counts. */
    nk_layout_row_push(ctx, 0.16f);
    char tf[32];
    snprintf(tf, sizeof tf, "Type: %s",
             g_type_filter < 0 ? "all" : catalog_type_label((AssetType)g_type_filter));
    if (nk_combo_begin_label(ctx, tf, nk_vec2(190, 340))) {
        nk_layout_row_dynamic(ctx, 22, 1);
        if (nk_combo_item_label(ctx, "all", NK_TEXT_LEFT)) g_type_filter = -1;
        for (int t = 0; t < AT__COUNT; ++t) {
            if (g_type_counts[t] == 0) continue;
            char it[40];
            snprintf(it, sizeof it, "%s  (%d)", catalog_type_label((AssetType)t), g_type_counts[t]);
            if (nk_combo_item_label(ctx, it, NK_TEXT_LEFT)) g_type_filter = t;
        }
        nk_combo_end(ctx);
    }

    /* Sort order. */
    nk_layout_row_push(ctx, 0.13f);
    if (nk_combo_begin_label(ctx, SORT_LABEL[g_sort], nk_vec2(150, 120))) {
        nk_layout_row_dynamic(ctx, 22, 1);
        for (int s = 0; s < 3; ++s)
            if (nk_combo_item_label(ctx, SORT_LABEL[s], NK_TEXT_LEFT) && s != g_sort) {
                g_sort = s;
                rebuild_order();
            }
        nk_combo_end(ctx);
    }

    /* Filter field with search-icon placeholder when empty. */
    nk_layout_row_push(ctx, 0.24f);
    struct nk_rect er = nk_widget_bounds(ctx);
    nk_flags ef = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, g_filter,
                                                 sizeof g_filter, nk_filter_default);
    g_filter_focus = (ef & NK_EDIT_ACTIVE) != 0;
    if (!g_filter[0]) {
        draw_icon(canvas, nk_rect(er.x + 6, er.y + (er.h - 13) / 2.0f, 13, 13),
                  IC_SEARCH, nk_rgb(118, 120, 128));
        nk_draw_text(canvas, nk_rect(er.x + 24, er.y + (er.h - f->height) / 2.0f, er.w - 28, f->height),
                     "search name or type...", (int)sizeof("search name or type...") - 1, f,
                     nk_rgb(20, 21, 26), nk_rgb(120, 122, 130));
    }

    /* Count: visible / total when filtering, else total. */
    nk_layout_row_push(ctx, 0.13f);
    if (g_filter[0] || g_type_filter >= 0)
        nk_labelf(ctx, NK_TEXT_RIGHT, "%d / %d", g_visible, catalog_count());
    else
        nk_labelf(ctx, NK_TEXT_RIGHT, "%d", catalog_count());
    nk_layout_row_end(ctx);
}

/* Per-type badge colours (fill, text) - light pills on the dark list,
 * mirroring the design mockup. */
static const struct { struct nk_color bg, fg; } TYPE_BADGE[AT__COUNT] = {
    [AT_ANIM]    = {{206, 201, 246, 255}, {38, 33, 92, 255}},   /* purple */
    [AT_MASK]    = {{211, 209, 199, 255}, {44, 44, 42, 255}},   /* gray */
    [AT_FILD]    = {{211, 209, 199, 255}, {44, 44, 42, 255}},   /* gray */
    [AT_PIC]     = {{181, 212, 244, 255}, {4, 44, 83, 255}},    /* blue */
    [AT_PAL]     = {{250, 199, 117, 255}, {65, 36, 2, 255}},    /* amber */
    [AT_WAV]     = {{159, 225, 203, 255}, {4, 52, 44, 255}},    /* teal */
    [AT_FLIC]    = {{245, 196, 179, 255}, {74, 27, 12, 255}},   /* coral */
    [AT_SCRIPT]  = {{211, 209, 199, 255}, {44, 44, 42, 255}},   /* gray */
    [AT_UNKNOWN] = {{190, 190, 190, 255}, {44, 44, 42, 255}},   /* gray */
};

static void draw_badge(struct nk_context *ctx, struct nk_command_buffer *canvas, AssetType t)
{
    struct nk_rect b;
    if (nk_widget(&b, ctx) == NK_WIDGET_INVALID) return;
    const char *tag = catalog_type_label(t);
    struct nk_color bg = TYPE_BADGE[t].bg, fg = TYPE_BADGE[t].fg;
    struct nk_rect pill = nk_rect(b.x, b.y + 1.0f, b.w, b.h - 2.0f);
    nk_fill_rect(canvas, pill, 3.0f, bg);
    const struct nk_user_font *font = ctx->style.font;
    float th = font->height;
    struct nk_rect tr = nk_rect(pill.x + 6.0f, pill.y + (pill.h - th) / 2.0f,
                                pill.w - 8.0f, th);
    nk_draw_text(canvas, tr, tag, (int)strlen(tag), font, bg, fg);
}

/* Paint a light fill across the whole group region — Nuklear's group background
 * stays dark regardless of the style table, so we cover it ourselves. */
static void fill_panel_bg(struct nk_context *ctx)
{
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    struct nk_rect r = nk_window_get_content_region(ctx);
    nk_fill_rect(canvas, nk_rect(r.x - 10, r.y - 10, r.w + 20, r.h + 20), 0,
                 nk_rgb(26, 27, 32));
}

static void draw_list(struct nk_context *ctx)
{
    if (!nk_group_begin(ctx, "list", NK_WINDOW_BORDER)) return;
    fill_panel_bg(ctx);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    const struct nk_user_font *font = ctx->style.font;
    float fh = font->height;
    g_visible = 0;
    for (int k = 0; k < g_order_n; ++k) {
        int i = g_order[k];
        const CatalogEntry *ce = catalog_entry(i);
        if (!ce || !entry_matches(ce)) continue;
        g_visible++;

        /* Self-drawn row: full control over colours (nk_selectable ignores the
         * style table cleanly, and this lets us pack badge + name in one row). */
        nk_layout_row_dynamic(ctx, 23, 1);
        struct nk_rect row;
        if (nk_widget(&row, ctx) == NK_WIDGET_INVALID) continue;

        if (nk_input_is_mouse_click_in_rect(&ctx->input, NK_BUTTON_LEFT, row)
            && i != g_selected)
            select_entry(i);
        int hov = nk_input_is_mouse_hovering_rect(&ctx->input, row);
        struct nk_color rowbg = (i == g_selected) ? nk_rgb(48, 84, 132)
                              : hov ? nk_rgb(40, 42, 50) : nk_rgb(30, 31, 37);
        nk_fill_rect(canvas, row, 4.0f, rowbg);

        struct nk_rect pill = nk_rect(row.x + 5, row.y + 3, 46, row.h - 6);
        struct nk_color bbg = TYPE_BADGE[ce->type].bg, bfg = TYPE_BADGE[ce->type].fg;
        nk_fill_rect(canvas, pill, 3.0f, bbg);
        const char *tag = catalog_type_label(ce->type);
        nk_draw_text(canvas, nk_rect(pill.x + 5, pill.y + (pill.h - fh) / 2, pill.w - 7, fh),
                     tag, (int)strlen(tag), font, bbg, bfg);

        struct nk_color nfg = (i == g_selected) ? nk_rgb(236, 240, 250) : nk_rgb(205, 207, 214);
        nk_draw_text(canvas, nk_rect(row.x + 58, row.y + (row.h - fh) / 2, row.w - 62, fh),
                     ce->name, (int)strlen(ce->name), font, rowbg, nfg);
    }
    nk_group_end(ctx);
}

static void draw_palette_combo(struct nk_context *ctx)
{
    const char *cur = (g_pal_current >= 0) ? g_palettes[g_pal_current]
                                           : "(grayscale)";
    nk_layout_row_begin(ctx, NK_DYNAMIC, 24, 2);
    nk_layout_row_push(ctx, 0.18f);
    nk_label(ctx, "palette:", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 0.82f);
    if (nk_combo_begin_label(ctx, cur, nk_vec2(260, 320))) {
        nk_layout_row_dynamic(ctx, 22, 1);
        if (nk_combo_item_label(ctx, "(grayscale)", NK_TEXT_LEFT))
            apply_palette(-1);
        for (int i = 0; i < g_pal_count; ++i)
            if (nk_combo_item_label(ctx, g_palettes[i], NK_TEXT_LEFT))
                apply_palette(i);
        nk_combo_end(ctx);
    }
    nk_layout_row_end(ctx);
}

/* Transparency checkerboard behind the preview (so alpha reads clearly). */
static void draw_checker(struct nk_command_buffer *canvas, struct nk_rect b)
{
    /* Adaptive cell size keeps the cell COUNT bounded regardless of box size.
     * Fixed 10px cells on a fullscreen-sized box produced tens of thousands of
     * rects and overflowed the draw buffer (crash). */
    float cell = b.w / 64.0f;            /* small cells, but bounded count */
    float cyc  = b.h / 48.0f;
    if (cyc > cell) cell = cyc;
    if (cell < 11.0f) cell = 11.0f;
    struct nk_color a = nk_rgb(60, 62, 72), c = nk_rgb(43, 45, 53);
    nk_fill_rect(canvas, b, 0, c);       /* solid base so no sub-pixel gaps show */
    int nx = (int)(b.w / cell) + 1;
    int ny = (int)(b.h / cell) + 1;
    for (int y = 0; y < ny; ++y) {
        for (int x = 0; x < nx; ++x) {
            if (((x + y) & 1) == 0) continue;     /* only the lighter squares */
            float cx = b.x + (float)x * cell, cy = b.y + (float)y * cell;
            float cw = NK_MIN(cell + 1.0f, b.x + b.w - cx);   /* +1 overlap, no seams */
            float ch = NK_MIN(cell + 1.0f, b.y + b.h - cy);
            if (cw <= 0.0f || ch <= 0.0f) continue;
            nk_fill_rect(canvas, nk_rect(cx, cy, cw, ch), 0, a);
        }
    }
}

/* A small stat card: muted label on top, value below, rounded surface. */
static void metric_card(struct nk_context *ctx, struct nk_command_buffer *canvas,
                        const char *label, const char *value)
{
    struct nk_rect b;
    if (nk_widget(&b, ctx) == NK_WIDGET_INVALID) return;
    struct nk_color bg = nk_rgb(34, 35, 42);
    nk_fill_rect(canvas, b, 6.0f, bg);
    const struct nk_user_font *f = ctx->style.font;
    nk_draw_text(canvas, nk_rect(b.x + 9, b.y + 7, b.w - 14, f->height),
                 label, (int)strlen(label), f, bg, nk_rgb(128, 130, 140));
    nk_draw_text(canvas, nk_rect(b.x + 9, b.y + b.h - f->height - 8, b.w - 14, f->height),
                 value, (int)strlen(value), f, bg, nk_rgb(224, 226, 232));
}

static void draw_preview_image(struct nk_context *ctx)
{
    /* Fit-to-box with nearest scaling, capped so tiny sprites still read big
     * and huge backgrounds shrink to fit; g_zoom multiplies on top. */
    /* Full-width checkerboard box; the image is centred and scaled to fit.
     * Height scales with the window so it fills the space nicely. */
    nk_layout_row_dynamic(ctx, (float)g_preview_h, 1);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    struct nk_rect box;
    if (nk_widget(&box, ctx) == NK_WIDGET_INVALID) return;
    draw_checker(canvas, box);

    float sc = NK_MIN(box.w / (float)g_tex_w, box.h / (float)g_tex_h);
    if (sc > PREVIEW_MAX_ZOOM) sc = PREVIEW_MAX_ZOOM;
    sc *= g_zoom;
    if (sc < 0.02f) sc = 0.02f;
    float dw = (float)g_tex_w * sc, dh = (float)g_tex_h * sc;
    struct nk_rect ir = nk_rect(box.x + (box.w - dw) / 2.0f, box.y + (box.h - dh) / 2.0f, dw, dh);
    struct nk_image img = nk_image_ptr(g_tex);
    nk_draw_image(canvas, ir, &img, nk_rgb(255, 255, 255));
}

static void draw_anim_controls(struct nk_context *ctx)
{
    int count = g_anim ? g_anim->frame_count : 0;
    nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 4);
    nk_layout_row_push(ctx, 0.16f);
    if (icon_button(ctx, g_anim_play ? IC_STOP : IC_PLAY, NULL))
        g_anim_play = !g_anim_play;
    nk_layout_row_push(ctx, 0.12f);
    if (icon_button(ctx, IC_PREV, NULL)) {
        if (count > 0) { g_anim_frame = (g_anim_frame + count - 1) % count; rebuild_preview(); }
    }
    nk_layout_row_push(ctx, 0.12f);
    if (icon_button(ctx, IC_NEXT, NULL)) {
        if (count > 0) { g_anim_frame = (g_anim_frame + 1) % count; rebuild_preview(); }
    }
    nk_layout_row_push(ctx, 0.60f);
    int f = g_anim_frame;
    if (count > 1 && nk_slider_int(ctx, 0, &f, count - 1, 1) && f != g_anim_frame) {
        g_anim_frame = f;
        rebuild_preview();
    }
    nk_layout_row_end(ctx);
    nk_layout_row_dynamic(ctx, 16, 1);
    nk_labelf(ctx, NK_TEXT_LEFT, "frame %d / %d", g_anim_frame + 1, count);

    int fps = 1000 / (g_anim_ms > 0 ? g_anim_ms : 120);
    nk_layout_row_begin(ctx, NK_DYNAMIC, 18, 2);
    nk_layout_row_push(ctx, 0.28f);
    nk_labelf(ctx, NK_TEXT_LEFT, "speed: %d fps", fps);
    nk_layout_row_push(ctx, 0.72f);
    if (nk_slider_int(ctx, 2, &fps, 30, 1))
        g_anim_ms = 1000 / (fps > 0 ? fps : 1);
    nk_layout_row_end(ctx);
}

/* Re-queue the cutscene's audio from the offset matching the current frame,
 * so seeking moves the sound too. No-op when not playing. */
static void flic_audio_seek(void)
{
    if (!g_flic_play) return;
    uint32_t len  = vflic_audio_len(g_flic);
    int      hz   = vflic_audio_hz(g_flic);
    int      ch   = vflic_audio_ch(g_flic);
    int      bits = vflic_audio_bits(g_flic);
    double   fps  = vflic_fps(g_flic);
    if (!len || hz <= 0 || ch <= 0 || bits <= 0 || fps <= 0.0) return;
    uint32_t bpf  = (uint32_t)ch * (uint32_t)(bits / 8);     /* bytes per frame */
    uint32_t off  = (uint32_t)((double)g_flic_frame / fps * hz) * bpf;
    if (off >= len) off = 0;
    vaudio_play_pcm(vflic_audio_data(g_flic) + off, len - off, hz, ch, bits);
}

static void draw_cutscene(struct nk_context *ctx)
{
    if (!nk_group_begin(ctx, "cutscene", NK_WINDOW_BORDER)) return;
    int count = vflic_frame_count(g_flic);

    nk_layout_row_dynamic(ctx, 18, 1);
    nk_labelf(ctx, NK_TEXT_LEFT, "%s - %d frames, %dx%d @ %.0f fps",
              basename_of(g_cur_path), count,
              vflic_width(g_flic), vflic_height(g_flic), vflic_fps(g_flic));

    if (g_has_preview && g_tex) draw_preview_image(ctx);

    nk_layout_row_begin(ctx, NK_DYNAMIC, 28, 4);
    nk_layout_row_push(ctx, 0.18f);
    if (icon_button(ctx, g_flic_play ? IC_STOP : IC_PLAY, NULL)) {
        g_flic_play = !g_flic_play;
        if (g_flic_play) {
            g_flic_frame = 0;
            flic_rebuild();
            if (vflic_audio_len(g_flic) > 0)
                vaudio_play_pcm(vflic_audio_data(g_flic), vflic_audio_len(g_flic),
                                vflic_audio_hz(g_flic), vflic_audio_ch(g_flic),
                                vflic_audio_bits(g_flic));
        } else {
            vaudio_stop();
        }
    }
    nk_layout_row_push(ctx, 0.12f);
    if (icon_button(ctx, IC_PREV, NULL)) { if (count > 0) { g_flic_frame = (g_flic_frame + count - 1) % count; flic_rebuild(); flic_audio_seek(); } }
    nk_layout_row_push(ctx, 0.12f);
    if (icon_button(ctx, IC_NEXT, NULL)) { if (count > 0) { g_flic_frame = (g_flic_frame + 1) % count; flic_rebuild(); flic_audio_seek(); } }
    nk_layout_row_push(ctx, 0.58f);
    int f = g_flic_frame;
    if (count > 1 && nk_slider_int(ctx, 0, &f, count - 1, 1) && f != g_flic_frame) {
        g_flic_frame = f; flic_rebuild(); flic_audio_seek();
    }
    nk_layout_row_end(ctx);

    nk_layout_row_dynamic(ctx, 16, 1);
    nk_labelf(ctx, NK_TEXT_LEFT, "frame %d / %d", g_flic_frame + 1, count);
    nk_group_end(ctx);
}

/* .scr is plain text: [etap]/[komnata]/[rozmowa]/[sampl]... sections with tab
 * indentation. Show it as text, section tags tinted, instead of hex. */
static void draw_script(struct nk_context *ctx)
{
    const char *p   = (const char *)g_buf;
    const char *end = p + g_sz;
    int line_cap = 4000;                       /* guard pathological files */
    nk_layout_row_dynamic(ctx, 15, 1);
    char line[256];
    while (p < end && line_cap-- > 0) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        size_t len = (size_t)(le - p), o = 0;
        for (size_t i = 0; i < len && o + 2 < sizeof line; ++i) {
            char c = p[i];
            if (c == '\t')      { line[o++] = ' '; line[o++] = ' '; }
            else if (c != '\r') { line[o++] = c; }
        }
        line[o] = 0;
        const char *t = line;
        while (*t == ' ') ++t;
        struct nk_color col = (*t == '[') ? nk_rgb(120, 210, 140)
                                          : nk_rgb(205, 205, 210);
        nk_label_colored(ctx, line, NK_TEXT_LEFT, col);
        p = nl ? nl + 1 : end;
    }
}

/* Export the currently-previewed frame/image as PNG. */
static void export_current_png(const CatalogEntry *ce)
{
    ViewImage img = {0};
    int ok = 0;
    if (ce->type == AT_ANIM && g_anim)
        ok = view_render_anim_frame(g_anim, g_anim_frame, &img);
    else if ((ce->type == AT_MASK || ce->type == AT_FILD) && g_anim)
        ok = view_render_mask(g_anim, g_anim_frame, &img);
    else if (ce->type == AT_PIC) ok = view_render_pic(g_buf, g_sz, &img);
    else if (ce->type == AT_PAL) ok = view_render_pal(g_buf, g_sz, 22, &img);
    else if (ce->type == AT_WAV) ok = vaudio_waveform(g_buf, g_sz, 600, 160, &img);
    if (!ok) { snprintf(g_status, sizeof g_status, "PNG: not rendered"); return; }
    char nm[64];
    png_name(ce->name, nm, sizeof nm);
    const char *path = ask_save_path(nm, "*.png", "PNG image");
    if (!path) { view_image_free(&img); return; }
    snprintf(g_status, sizeof g_status,
             write_png_img(path, &img) ? "PNG: %s" : "PNG error: %s", basename_of(path));
}

/* Suffix the basename before its extension, e.g. "EBEK.WYC" + "_sheet.png". */
static void suffixed_name(const char *name, const char *suffix, char *out, size_t cap)
{
    snprintf(out, cap, "%s", name);
    char *dot = strrchr(out, '.');
    size_t off = dot ? (size_t)(dot - out) : strlen(out);
    snprintf(out + off, cap - off, "%s", suffix);
}

static void export_sheet(const CatalogEntry *ce)
{
    if (!g_anim) return;
    ViewImage img = {0};
    if (!view_render_anim_sheet(g_anim, &img)) {
        snprintf(g_status, sizeof g_status, "sprite-sheet: not rendered");
        return;
    }
    char nm[64];
    suffixed_name(ce->name, "_sheet.png", nm, sizeof nm);
    const char *path = ask_save_path(nm, "*.png", "PNG image");
    if (!path) { view_image_free(&img); return; }
    snprintf(g_status, sizeof g_status,
             write_png_img(path, &img) ? "sheet: %s" : "sheet error: %s", basename_of(path));
}

static void export_gif_current(const CatalogEntry *ce)
{
    if (!g_anim) return;
    char nm[64];
    suffixed_name(ce->name, ".gif", nm, sizeof nm);
    const char *path = ask_save_path(nm, "*.gif", "GIF");
    if (!path) return;
    snprintf(g_status, sizeof g_status,
             gif_export_anim(g_anim, path, 10) ? "GIF: %s" : "GIF error: %s", basename_of(path));
}

/* A little vertical breathing room between sections. */
static void section_gap(struct nk_context *ctx)
{
    nk_layout_row_dynamic(ctx, 6, 1);
    nk_label(ctx, "", NK_TEXT_LEFT);
}

/* Muted section label. */
static void section_header(struct nk_context *ctx, const char *txt)
{
    nk_layout_row_dynamic(ctx, 16, 1);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    struct nk_rect b;
    if (nk_widget(&b, ctx) == NK_WIDGET_INVALID) return;
    const struct nk_user_font *f = ctx->style.font;
    nk_draw_text(canvas, nk_rect(b.x + 2, b.y + 1, b.w - 4, f->height),
                 txt, (int)strlen(txt), f, nk_rgb(26, 27, 32), nk_rgb(124, 128, 138));
}

/* ---- palette test window ------------------------------------------------ */

static int          g_paltest = 0;
static SDL_Texture *g_pt_tex[MAX_PALETTES];
static int          g_pt_n = 0;
static int          g_pt_apply = -1;     /* palette index chosen via "Use" */

static SDL_Texture *make_preview_tex(ViewImage *img)
{
    SDL_Texture *t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STATIC, img->w, img->h);
    if (t) {
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest);
        SDL_UpdateTexture(t, NULL, img->rgba, img->w * 4);
    }
    return t;
}

/* Render the current asset (ANIM frame / PIC) with whatever palette is active. */
static int render_current_asset(ViewImage *out)
{
    const CatalogEntry *ce = catalog_entry(g_selected);
    if (!ce) return 0;
    if (ce->type == AT_ANIM && g_anim) return view_render_anim_frame(g_anim, g_anim_frame, out);
    if (ce->type == AT_PIC)            return view_render_pic(g_buf, g_sz, out);
    return 0;
}

static void load_palette_into_viewer(int which)
{
    int n = catalog_count();
    for (int i = 0; i < n; ++i) {
        const CatalogEntry *ce = catalog_entry(i);
        if (ce->type != AT_PAL || strcmp(ce->name, g_palettes[which]) != 0) continue;
        void *pb = NULL; uint32_t ps = 0;
        if (catalog_load(i, &pb, &ps)) {
            if (ps >= 768) viewer_set_palette((const uint8_t *)pb);
            catalog_free(pb);
        }
        return;
    }
}

static void close_paltest(void)
{
    for (int i = 0; i < g_pt_n; ++i)
        if (g_pt_tex[i]) { SDL_DestroyTexture(g_pt_tex[i]); g_pt_tex[i] = NULL; }
    g_pt_n = 0;
    g_paltest = 0;
}

/* Render the current sprite once per palette into its own texture. */
static void open_paltest(void)
{
    close_paltest();
    int cap = g_pal_count < MAX_PALETTES ? g_pal_count : MAX_PALETTES;
    for (int p = 0; p < cap; ++p) {
        load_palette_into_viewer(p);
        ViewImage img = {0};
        g_pt_tex[p] = render_current_asset(&img) ? make_preview_tex(&img) : NULL;
        view_image_free(&img);
    }
    g_pt_n = cap;
    if (g_pal_current >= 0) load_palette_into_viewer(g_pal_current);   /* restore */
    else                    viewer_set_palette(NULL);
    g_paltest = 1;
}

static void draw_paltest(struct nk_context *ctx)
{
    if (!g_paltest) return;
    const CatalogEntry *ce = catalog_entry(g_selected);
    if (nk_begin(ctx, "Palette test", nk_rect(120, 70, 840, 580),
            NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE | NK_WINDOW_MOVABLE |
            NK_WINDOW_SCALABLE | NK_WINDOW_BORDER)) {
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_labelf(ctx, NK_TEXT_LEFT, "%s in %d palettes - click 'Use' to apply:",
                  ce ? ce->name : "", g_pt_n);
        const int cols = 4;
        for (int i = 0; i < g_pt_n; i += cols) {
            nk_layout_row_dynamic(ctx, 156, cols);
            for (int c = 0; c < cols && i + c < g_pt_n; ++c) {
                int pi = i + c;
                char gid[24];
                snprintf(gid, sizeof gid, "pt%d", pi);
                if (nk_group_begin_titled(ctx, gid, "", NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
                    nk_layout_row_dynamic(ctx, 98, 1);
                    if (g_pt_tex[pi]) nk_image(ctx, nk_image_ptr(g_pt_tex[pi]));
                    else              nk_label(ctx, "(none)", NK_TEXT_CENTERED);
                    nk_layout_row_dynamic(ctx, 14, 1);
                    nk_label(ctx, g_palettes[pi], NK_TEXT_CENTERED);
                    nk_layout_row_dynamic(ctx, 24, 1);
                    if (nk_button_label(ctx, "Use")) g_pt_apply = pi;
                    nk_group_end(ctx);
                }
            }
        }
    } else {
        close_paltest();
    }
    nk_end(ctx);
}

static void draw_detail(struct nk_context *ctx)
{
    if (!nk_group_begin(ctx, "detail", NK_WINDOW_BORDER)) return;
    fill_panel_bg(ctx);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);

    const CatalogEntry *ce = (g_selected >= 0) ? catalog_entry(g_selected) : NULL;
    if (!ce) {
        if (catalog_count() == 0) {
            /* Nothing mounted — invite the user to locate the game data. */
            nk_layout_row_dynamic(ctx, 30, 1);
            nk_label(ctx, "No game data loaded.", NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_label(ctx, "Locate a Wacki .DTA file (e.g. the game's data/ folder).",
                     NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 8, 1);
            nk_label(ctx, "", NK_TEXT_LEFT);                  /* spacer */
            nk_layout_row_static(ctx, 30, 180, 1);
            if (icon_button(ctx, IC_FILE, "Open .DTA...")) g_want_open = 1;
        } else {
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_label(ctx, "Select an entry from the list on the left.", NK_TEXT_LEFT);
        }
        nk_group_end(ctx);
        return;
    }

    /* Fit the preview box to the panel so it never needs a scrollbar — subtract
     * the (roughly fixed) height of everything else for this type; min 160. */
    {
        int rest = 450;                                            /* ANIM: most chrome */
        if      (ce->type == AT_PIC || ce->type == AT_PAL)   rest = 250;
        else if (ce->type == AT_WAV)                         rest = 300;
        else if (ce->type == AT_MASK || ce->type == AT_FILD) rest = 420;
        g_preview_h = (int)nk_window_get_content_region(ctx).h - rest;
        if (g_preview_h < 160) g_preview_h = 160;
    }

    /* Header: big name + type badge. */
    nk_layout_row_begin(ctx, NK_DYNAMIC, 30, 2);
    nk_layout_row_push(ctx, 0.76f);
    if (g_font_lg) nk_style_set_font(ctx, g_font_lg);
    nk_label(ctx, ce->name, NK_TEXT_LEFT);
    if (g_font_sm) nk_style_set_font(ctx, g_font_sm);
    nk_layout_row_push(ctx, 0.24f);
    draw_badge(ctx, canvas, ce->type);
    nk_layout_row_end(ctx);

    /* Graphic / mask preview. */
    int is_mask = (ce->type == AT_MASK || ce->type == AT_FILD);
    if (is_graphic(ce->type) || is_mask) {
        section_gap(ctx);
        if (ce->type == AT_ANIM || ce->type == AT_PIC) {
            draw_palette_combo(ctx);
            nk_layout_row_dynamic(ctx, 24, 1);
            if (icon_button(ctx, IC_PALETTE, "Palette test (all)")) open_paltest();
        }
        if (g_has_preview && g_tex) {
            draw_preview_image(ctx);
            if (ce->type == AT_ANIM || is_mask) draw_anim_controls(ctx);
            if (is_mask) {
                nk_layout_row_dynamic(ctx, 14, 1);
                nk_label(ctx, ce->type == AT_FILD
                         ? "green = walkable (walkability .fld)"
                         : "green = clickable region (mask)", NK_TEXT_LEFT);
            }
        } else {
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "(failed to render)", NK_TEXT_LEFT);
        }
    }

    /* Audio (WAV): waveform preview + icon transport, like the animation one. */
    if (ce->type == AT_WAV) {
        section_gap(ctx);
        if (g_has_preview && g_tex) draw_preview_image(ctx);
        nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 2);
        nk_layout_row_push(ctx, 0.16f);
        if (icon_button(ctx, vaudio_is_playing() ? IC_STOP : IC_PLAY, NULL)) {
            if (vaudio_is_playing()) vaudio_stop();
            else                     vaudio_play(g_buf, g_sz);
        }
        nk_layout_row_push(ctx, 0.84f);
        int hz = 0, ch = 0, bits = 0; double sec = 0;
        if (vaudio_info(g_buf, g_sz, &hz, &ch, &bits, &sec))
            nk_labelf(ctx, NK_TEXT_LEFT, "%d Hz   %d-bit   %s   %.2f s",
                      hz, bits, ch == 1 ? "mono" : "stereo", sec);
        else
            nk_label(ctx, "", NK_TEXT_LEFT);
        nk_layout_row_end(ctx);
    }

    /* Actions — export buttons depend on type. */
    section_gap(ctx);
    section_header(ctx, "Actions");
    {
        const char *lbl[5]; int aid[5]; IconType ico[5]; int nb = 0;
        enum { A_PNG, A_SHEET, A_GIF, A_RAW };
        if (is_graphic(ce->type) || is_mask) {   /* PNG only for graphics, not WAV */
            lbl[nb] = "PNG"; ico[nb] = IC_IMAGE; aid[nb++] = A_PNG;
        }
        if (ce->type == AT_ANIM) { lbl[nb] = "Sprite-sheet"; ico[nb] = IC_GRID; aid[nb++] = A_SHEET; }
        if (ce->type == AT_ANIM) { lbl[nb] = "GIF"; ico[nb] = IC_FILM; aid[nb++] = A_GIF; }
        lbl[nb] = (ce->type == AT_WAV) ? "WAV" : "Raw"; ico[nb] = IC_FILE; aid[nb++] = A_RAW;
        nk_layout_row_dynamic(ctx, 32, nb);
        for (int bi = 0; bi < nb; ++bi)
            if (icon_button(ctx, ico[bi], lbl[bi])) {
                if      (aid[bi] == A_PNG)   export_current_png(ce);
                else if (aid[bi] == A_SHEET) export_sheet(ce);
                else if (aid[bi] == A_GIF)   export_gif_current(ce);
                else                         export_selected();
            }
    }
    nk_layout_row_dynamic(ctx, 26, 2);
    if (icon_button(ctx, IC_FILE, "Show bytes (hex)")) g_hex_popup = 1;
    nk_label(ctx, g_status, NK_TEXT_LEFT);

    /* Technical details — moved low so the top stays clean. */
    section_gap(ctx);
    section_header(ctx, "Details");
    {
        double ratio = g_comp ? (100.0 * (double)g_unp / (double)g_comp) : 0.0;
        int have_anim = (ce->type == AT_ANIM || ce->type == AT_MASK || ce->type == AT_FILD) && g_anim;
        char m_size[32], m_a[32], m_b[32];
        snprintf(m_size, sizeof m_size, "%u B", g_unp);
        if (have_anim) {
            snprintf(m_a, sizeof m_a, "%d", g_anim->frame_count);
            snprintf(m_b, sizeof m_b, "%dx%d", g_anim->max_w, g_anim->max_h);
        } else {
            snprintf(m_a, sizeof m_a, "%.0f%%", ratio);
            snprintf(m_b, sizeof m_b, "0x%08X", ce->offset);
        }
        nk_layout_row_dynamic(ctx, 46, 4);
        metric_card(ctx, canvas, "magic", g_magic ? g_magic : catalog_type_label(ce->type));
        metric_card(ctx, canvas, "size", m_size);
        metric_card(ctx, canvas, have_anim ? "frames" : "compression", m_a);
        metric_card(ctx, canvas, have_anim ? "dimensions" : "offset", m_b);
    }

    /* .scr stays inline (it IS the content); raw bytes go to the Hex popup. */
    if (ce->type == AT_SCRIPT) {
        section_gap(ctx);
        section_header(ctx, "Script");
        draw_script(ctx);
    }

    /* On-demand hex byte view. */
    if (g_hex_popup) {
        if (nk_popup_begin(ctx, NK_POPUP_STATIC, "Bytes (hex)",
                NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE | NK_WINDOW_MOVABLE | NK_WINDOW_BORDER,
                nk_rect(30, 50, 560, 430))) {
            int total = hexdump_line_count(g_sz);
            int cap = total < 4096 ? total : 4096;
            nk_layout_row_dynamic(ctx, 15, 1);
            for (int l = 0; l < cap; ++l) {
                char line[128];
                hexdump_format_line((const uint8_t *)g_buf, g_sz, l, line, sizeof line);
                nk_label(ctx, line, NK_TEXT_LEFT);
            }
            if (total > cap)
                nk_labelf(ctx, NK_TEXT_LEFT, "... %d of %u B (full: Raw)",
                          cap * HEXDUMP_BYTES_PER_LINE, g_sz);
            nk_popup_end(ctx);
        } else {
            g_hex_popup = 0;
        }
    }

    nk_group_end(ctx);
}

/* ---- CLI dump mode (headless render -> BMP) ------------------------------ */

static void dump_load_palette(void)
{
    int n = catalog_count(), pick = -1;
    for (int i = 0; i < n; ++i) {
        const CatalogEntry *ce = catalog_entry(i);
        if (ce->type != AT_PAL) continue;
        if (pick < 0) pick = i;                          /* fallback: first .pal */
        if (!strcasecmp(ce->name, "PALETA.PAL")) { pick = i; break; }
    }
    if (pick < 0) { viewer_set_palette(NULL); return; }
    void *pb = NULL; uint32_t ps = 0;
    if (catalog_load(pick, &pb, &ps)) {
        if (ps >= 768) viewer_set_palette((const uint8_t *)pb);
        catalog_free(pb);
    }
}

static int dump_find(const char *name)
{
    int n = catalog_count();
    for (int i = 0; i < n; ++i)
        if (!strcasecmp(catalog_entry(i)->name, name)) return i;
    return -1;
}

static int dump_render(int idx, ViewImage *img)
{
    const CatalogEntry *ce = catalog_entry(idx);
    int ok = 0;
    if (ce->type == AT_ANIM || ce->type == AT_MASK || ce->type == AT_FILD) {
        AnimAsset *a = LoadAssetFromDtaBase(ce->name);
        if (a) {
            ok = (ce->type == AT_ANIM) ? view_render_anim_frame(a, 0, img)
                                       : view_render_mask(a, 0, img);
            FreeAsset(a);
        }
    } else if (ce->type == AT_PIC || ce->type == AT_PAL) {
        void *b = NULL; uint32_t s = 0;
        if (catalog_load(idx, &b, &s)) {
            ok = (ce->type == AT_PIC) ? view_render_pic(b, s, img)
                                      : view_render_pal(b, s, 22, img);
            catalog_free(b);
        }
    } else if (ce->type == AT_WAV) {
        void *b = NULL; uint32_t s = 0;
        if (catalog_load(idx, &b, &s)) { ok = vaudio_waveform(b, s, 600, 160, img); catalog_free(b); }
    }
    return ok;
}

static int run_dump_mode(const char *dta, const char *name, const char *out)
{
    if (!out) { fprintf(stderr, "--dump requires <NAME> <out.png>\n"); return 2; }
    if (!catalog_open(dta)) { fprintf(stderr, "mount failed: %s\n", dta); return 1; }
    dump_load_palette();

    int idx = dump_find(name);
    if (idx < 0) { fprintf(stderr, "entry not found: %s\n", name); return 1; }

    ViewImage img = {0};
    if (!dump_render(idx, &img)) {
        fprintf(stderr, "render failed (non-renderable type?): %s\n", name);
        return 1;
    }
    int rc = stbi_write_png(out, img.w, img.h, 4, img.rgba, img.w * 4) ? 0 : -1;
    printf("%s: %dx%d -> %s (%s)\n", name, img.w, img.h, out, rc == 0 ? "ok" : "FAIL");
    view_image_free(&img);
    catalog_close();
    return rc == 0 ? 0 : 1;
}

/* ---- GIF export (ANIM -> animated GIF) ----------------------------------- */

static int gif_export_anim(AnimAsset *a, const char *path, int centisecs)
{
    if (!a || a->frame_count == 0) return 0;
    int w = a->max_w, h = a->max_h;
    if (w <= 0 || h <= 0) return 0;

    MsfGifState gs = {0};
    if (!msf_gif_begin(&gs, w, h)) return 0;

    uint8_t *frame = (uint8_t *)malloc((size_t)w * h * 4);
    if (!frame) { msf_gif_end(&gs); return 0; }

    for (int i = 0; i < a->frame_count; ++i) {
        memset(frame, 0, (size_t)w * h * 4);            /* transparent canvas */
        ViewImage img = {0};
        if (view_render_anim_frame(a, i, &img)) {
            int cw = img.w < w ? img.w : w;
            for (int y = 0; y < img.h && y < h; ++y)
                memcpy(frame + (size_t)y * w * 4,
                       img.rgba + (size_t)y * img.w * 4, (size_t)cw * 4);
            view_image_free(&img);
        }
        msf_gif_frame(&gs, frame, centisecs, 16, w * 4);
    }
    free(frame);

    MsfGifResult res = msf_gif_end(&gs);
    int ok = 0;
    if (res.data) {
        FILE *fp = fopen(path, "wb");
        if (fp) { ok = (fwrite(res.data, 1, res.dataSize, fp) == res.dataSize); fclose(fp); }
    }
    msf_gif_free(res);
    return ok;
}

static int run_gif_mode(const char *dta, const char *name, const char *out)
{
    if (!out) { fprintf(stderr, "--gif requires <NAME> <out.gif>\n"); return 2; }
    if (!catalog_open(dta)) { fprintf(stderr, "mount failed: %s\n", dta); return 1; }
    dump_load_palette();
    int idx = dump_find(name);
    if (idx < 0 || catalog_entry(idx)->type != AT_ANIM) {
        fprintf(stderr, "not ANIM or not found: %s\n", name);
        catalog_close();
        return 1;
    }
    AnimAsset *a = LoadAssetFromDtaBase(catalog_entry(idx)->name);
    int ok = a && gif_export_anim(a, out, 10);
    if (a) {
        printf("%s: %d frames -> %s (%s)\n", name, a->frame_count, out, ok ? "ok" : "FAIL");
        FreeAsset(a);
    }
    catalog_close();
    return ok ? 0 : 1;
}

/* ---- CLI batch export (whole archive -> PNG/WAV/raw) --------------------- */

/* "EBEK.WYC" -> "EBEK.png" in out[]. */
static void png_name(const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s", name);
    char *dot = strrchr(out, '.');
    size_t off = dot ? (size_t)(dot - out) : strlen(out);
    snprintf(out + off, cap - off, ".png");
}

static int write_png_img(const char *path, ViewImage *img)
{
    int rc = stbi_write_png(path, img->w, img->h, 4, img->rgba, img->w * 4);
    view_image_free(img);
    return rc ? 1 : 0;
}

static int run_dumpall_mode(const char *dta, const char *dir)
{
    if (!catalog_open(dta)) { fprintf(stderr, "mount failed: %s\n", dta); return 1; }
    dump_load_palette();
    make_dir(dir);

    int  n = catalog_count(), png = 0, wav = 0, raw = 0, fail = 0;
    char path[700], nm[64];

    for (int i = 0; i < n; ++i) {
        const CatalogEntry *ce = catalog_entry(i);
        ViewImage img = {0};
        int done = 0;

        if (ce->type == AT_ANIM) {
            AnimAsset *a = LoadAssetFromDtaBase(ce->name);
            if (a) {
                if (view_render_anim_sheet(a, &img)) {
                    png_name(ce->name, nm, sizeof nm);
                    snprintf(path, sizeof path, "%s/%s", dir, nm);
                    if (write_png_img(path, &img)) { ++png; done = 1; }
                }
                FreeAsset(a);
            }
        } else if (ce->type == AT_MASK || ce->type == AT_FILD) {
            AnimAsset *a = LoadAssetFromDtaBase(ce->name);
            if (a) {
                if (view_render_mask(a, 0, &img)) {
                    png_name(ce->name, nm, sizeof nm);
                    snprintf(path, sizeof path, "%s/%s", dir, nm);
                    if (write_png_img(path, &img)) { ++png; done = 1; }
                }
                FreeAsset(a);
            }
        } else if (ce->type == AT_PIC || ce->type == AT_PAL) {
            void *b = NULL; uint32_t s = 0;
            if (catalog_load(i, &b, &s)) {
                int ok = (ce->type == AT_PIC) ? view_render_pic(b, s, &img)
                                              : view_render_pal(b, s, 22, &img);
                catalog_free(b);
                if (ok) {
                    png_name(ce->name, nm, sizeof nm);
                    snprintf(path, sizeof path, "%s/%s", dir, nm);
                    if (write_png_img(path, &img)) { ++png; done = 1; }
                }
            }
        }

        if (!done) {                         /* WAV + everything else -> raw bytes */
            void *b = NULL; uint32_t s = 0;
            if (catalog_load(i, &b, &s)) {
                snprintf(path, sizeof path, "%s/%s", dir, ce->name);
                if (hexdump_export_raw(path, b, s)) { (ce->type == AT_WAV) ? ++wav : ++raw; }
                else ++fail;
                catalog_free(b);
            } else ++fail;
        }

        if ((i % 200) == 199) printf("  ... %d/%d\n", i + 1, n);
    }
    printf("\n%s -> %s/\n  %d PNG, %d WAV, %d raw, %d fail (of %d entries)\n",
           dta, dir, png, wav, raw, fail, n);
    catalog_close();
    return 0;
}

/* ---- CLI list mode ------------------------------------------------------ */

static int run_list_mode(const char *dta)
{
    if (!catalog_open(dta)) { fprintf(stderr, "mount failed: %s\n", dta); return 1; }
    int counts[AT__COUNT] = {0};
    int n = catalog_count();
    for (int i = 0; i < n; ++i) {
        const CatalogEntry *ce = catalog_entry(i);
        counts[ce->type]++;
        if (i < 30)
            printf("  %-4s  0x%08X  %s\n",
                   catalog_type_label(ce->type), ce->offset, ce->name);
    }
    if (n > 30) printf("  ... (%d more)\n", n - 30);
    printf("\n%s - %d entries\n", dta, n);
    for (int t = 0; t < AT__COUNT; ++t)
        if (counts[t]) printf("  %-5s %d\n", catalog_type_label((AssetType)t), counts[t]);
    catalog_close();
    return 0;
}

/* (Re)bake the fonts at the given DPI scale. Called at startup and whenever the
 * window moves to a monitor with a different pixel density. */
static void reload_fonts(float scale)
{
    if (scale < 0.1f) scale = 1.0f;
    struct nk_font_atlas *atlas;
    nk_sdl_font_stash_begin(&atlas);
    struct nk_font *fsm = nk_font_atlas_add_default(atlas, 14.0f * scale, NULL);
    struct nk_font *flg = nk_font_atlas_add_default(atlas, 19.0f * scale, NULL);
    nk_sdl_font_stash_end();
    fsm->handle.height /= scale;
    flg->handle.height /= scale;
    g_font_sm = &fsm->handle;
    g_font_lg = &flg->handle;
    if (g_ctx) nk_style_set_font(g_ctx, &fsm->handle);
    g_cur_scale = scale;
}

/* ---- keyboard navigation ------------------------------------------------ */

static void navigate_entry(int dir)
{
    if (g_avi_mode || g_order_n == 0) return;
    /* Walk the DISPLAY order (sorted + filtered), not the raw catalog index,
     * so up/down match what's on the list. */
    int pos = -1;
    for (int k = 0; k < g_order_n; ++k)
        if (g_order[k] == g_selected) { pos = k; break; }
    for (int step = 0; step < g_order_n; ++step) {
        pos += dir;
        if (pos < 0)            pos = g_order_n - 1;
        if (pos >= g_order_n)   pos = 0;
        const CatalogEntry *ce = catalog_entry(g_order[pos]);
        if (ce && entry_matches(ce)) { select_entry(g_order[pos]); return; }
    }
}

static void navigate_frame(int dir)
{
    if (g_avi_mode) {
        int fc = vflic_frame_count(g_flic);
        if (fc > 0) { g_flic_frame = (g_flic_frame + dir + fc) % fc; flic_rebuild(); }
    } else if (g_anim && g_anim->frame_count > 0) {
        int fc = g_anim->frame_count;
        g_anim_frame = (g_anim_frame + dir + fc) % fc;
        rebuild_preview();
    }
}

static void toggle_play(void)
{
    if (g_avi_mode) g_flic_play = !g_flic_play;
    else if (g_anim && g_anim->frame_count > 1) g_anim_play = !g_anim_play;
}

/* Bottom status bar: context + keyboard shortcuts. */
static void draw_statusbar(struct nk_context *ctx)
{
    nk_layout_row_dynamic(ctx, 20, 1);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    struct nk_rect b;
    if (nk_widget(&b, ctx) == NK_WIDGET_INVALID) return;
    nk_fill_rect(canvas, b, 0, nk_rgb(20, 21, 26));
    const struct nk_user_font *f = ctx->style.font;
    char info[280];
    if (g_avi_mode)
        snprintf(info, sizeof info,
                 "%s   |   cutscene   |   arrows = frames    space = play/stop",
                 basename_of(g_cur_path));
    else
        snprintf(info, sizeof info,
                 "%s   |   %d entries, %d palettes   |   arrows = navigate   space = play   "
                 "+/- = zoom   Esc = clear filter",
                 basename_of(g_cur_path), catalog_count(), g_pal_count);
    nk_draw_text(canvas, nk_rect(b.x + 8, b.y + (b.h - f->height) / 2.0f, b.w - 12, f->height),
                 info, (int)strlen(info), f, nk_rgb(20, 21, 26), nk_rgb(150, 152, 160));
}

int main(int argc, char **argv)
{
    const char *dta = DEFAULT_DTA;
    int list_mode = 0;
    const char *dump_name = NULL, *dump_out = NULL, *dumpall_dir = NULL;
    const char *gif_name = NULL, *gif_out = NULL, *shot_path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--list")) {
            list_mode = 1;
        } else if (!strcmp(argv[i], "--screenshot")) {
            if (i + 1 < argc) { shot_path = argv[i + 1]; i += 1; }
        } else if (!strcmp(argv[i], "--dump-all")) {
            if (i + 1 < argc) { dumpall_dir = argv[i + 1]; i += 1; }
        } else if (!strcmp(argv[i], "--gif")) {
            if (i + 2 < argc) { gif_name = argv[i + 1]; gif_out = argv[i + 2]; i += 2; }
        } else if (!strcmp(argv[i], "--dump")) {
            if (i + 2 < argc) { dump_name = argv[i + 1]; dump_out = argv[i + 2]; i += 2; }
        } else if (argv[i][0] != '-') {
            dta = argv[i];
        }
    }
    if (dumpall_dir) return run_dumpall_mode(dta, dumpall_dir);
    if (gif_name)    return run_gif_mode(dta, gif_name, gif_out);
    if (dump_name)   return run_dump_mode(dta, dump_name, dump_out);
    if (list_mode)   return run_list_mode(dta);

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    Uint32 wflags = shot_path
        ? (SDL_WINDOW_HIDDEN | SDL_WINDOW_ALLOW_HIGHDPI)
        : (SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Window *win = SDL_CreateWindow(
        "Wacki Assets Explorer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H, wflags);
    if (!win) { fprintf(stderr, "CreateWindow: %s\n", SDL_GetError()); return 1; }

    g_ren = SDL_CreateRenderer(win, -1,
                               SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) { fprintf(stderr, "CreateRenderer: %s\n", SDL_GetError()); return 1; }

    struct nk_context *ctx = nk_sdl_init(win, g_ren);
    g_ctx = ctx;
    apply_theme(ctx);
    {
        int rw, rh, ww, wh;
        SDL_GetRendererOutputSize(g_ren, &rw, &rh);
        SDL_GetWindowSize(win, &ww, &wh);
        float sy = wh ? (float)rh / (float)wh : 1.0f;
        SDL_RenderSetScale(g_ren, ww ? (float)rw / (float)ww : 1.0f, sy);
        reload_fonts(sy);
    }

    vaudio_init();

    scan_archives(dta);
    /* If the assets aren't where we expected (e.g. the binary was launched
     * outside the game's data/ dir), ask the user to locate a .DTA instead of
     * starting blank. Skipped under --screenshot so headless runs never block. */
    if (!mount_archive(dta) && !shot_path) {
        for (;;) {
            const char *picked = ask_open_dta();
            if (!picked) break;                 /* cancelled: start empty, Open... stays available */
            scan_archives(picked);
            if (mount_archive(picked)) break;   /* mounted ok */
        }                                       /* else: unreadable file, ask again */
    }

    if (shot_path) {                 /* preselect a sprite so the shot has a preview */
        int n = catalog_count();
        for (int i = 0; i < n && g_selected < 0; ++i)
            if (!strcasecmp(catalog_entry(i)->name, "DZIEW.WYC")) select_entry(i);
        for (int i = 0; i < n && g_selected < 0; ++i)
            if (catalog_entry(i)->type == AT_ANIM) select_entry(i);
    }

    int running = 1;
    int shot_frame = 0;
    while (running) {
        SDL_Event evt;
        int key_entry = 0, key_frame = 0, key_play = 0, key_zoom = 0;
        nk_input_begin(ctx);
        while (SDL_PollEvent(&evt)) {
            if (evt.type == SDL_QUIT) running = 0;
            if (evt.type == SDL_KEYDOWN) {
                switch (evt.key.keysym.sym) {
                    case SDLK_DOWN:    key_entry =  1; break;
                    case SDLK_UP:      key_entry = -1; break;
                    case SDLK_RIGHT:   key_frame =  1; break;
                    case SDLK_LEFT:    key_frame = -1; break;
                    case SDLK_SPACE:   key_play  =  1; break;
                    case SDLK_EQUALS:
                    case SDLK_PLUS:    key_zoom  =  1; break;
                    case SDLK_MINUS:   key_zoom  = -1; break;
                    case SDLK_0:       g_zoom = 1.0f;  break;
                    case SDLK_ESCAPE:  g_filter[0] = 0; break;
                    default: break;
                }
            }
            /* Ctrl + wheel = zoom preview (plain wheel still scrolls panels). */
            if (evt.type == SDL_MOUSEWHEEL && (SDL_GetModState() & KMOD_CTRL)) {
                g_zoom *= (evt.wheel.y > 0) ? 1.15f : 0.87f;
                if (g_zoom < 0.05f) g_zoom = 0.05f;
                if (g_zoom > 16.0f) g_zoom = 16.0f;
                continue;
            }
            nk_sdl_handle_event(&evt);
        }
        nk_sdl_handle_grab();
        nk_input_end(ctx);

        /* Keyboard nav - skip only when the filter box has focus, so typing
         * still works but arrows/space navigate everywhere else. */
        if (!g_filter_focus) {
            if (key_entry) navigate_entry(key_entry);
            if (key_frame) navigate_frame(key_frame);
            if (key_play)  toggle_play();
            if (key_zoom) {
                g_zoom *= (key_zoom > 0) ? 1.25f : 0.8f;
                if (g_zoom < 0.05f) g_zoom = 0.05f;
                if (g_zoom > 16.0f) g_zoom = 16.0f;
            }
        }

        /* Animation auto-advance. */
        if (g_anim_play && g_anim && g_anim->frame_count > 1) {
            uint32_t now = SDL_GetTicks();
            if (now - g_anim_last > (uint32_t)g_anim_ms) {
                g_anim_frame = (g_anim_frame + 1) % g_anim->frame_count;
                g_anim_last = now;
                rebuild_preview();
            }
        }

        /* Cutscene auto-advance at the file's fps. */
        if (g_avi_mode && g_flic_play) {
            int    fc  = vflic_frame_count(g_flic);
            double fps = vflic_fps(g_flic);
            if (fps < 1.0) fps = 10.0;
            uint32_t ms  = (uint32_t)(1000.0 / fps);
            uint32_t now = SDL_GetTicks();
            if (fc > 1 && now - g_flic_last >= ms) {
                g_flic_frame = (g_flic_frame + 1) % fc;
                g_flic_last = now;
                flic_rebuild();
            }
        }

        int ww, wh;
        SDL_GetWindowSize(win, &ww, &wh);
        {
            int rw, rh;
            SDL_GetRendererOutputSize(g_ren, &rw, &rh);
            float sy = wh ? (float)rh / (float)wh : 1.0f;
            SDL_RenderSetScale(g_ren, ww ? (float)rw / (float)ww : 1.0f, sy);
            float d = sy - g_cur_scale; if (d < 0) d = -d;
            if (sy > 0.1f && d > 0.01f) reload_fonts(sy);   /* monitor DPI changed */
        }
        g_preview_h = wh - 300;          /* fallback (cutscene); draw_detail refines it */
        if (g_preview_h < 160) g_preview_h = 160;
        if (nk_begin(ctx, "root", nk_rect(0, 0, (float)ww, (float)wh),
                     NK_WINDOW_NO_SCROLLBAR)) {
            draw_topbar(ctx);
            float body_h = (float)wh - 44.0f - 26.0f;
            if (body_h < 80.0f) body_h = 80.0f;
            if (g_avi_mode) {
                nk_layout_row_dynamic(ctx, body_h, 1);
                draw_cutscene(ctx);
            } else {
                nk_layout_row_template_begin(ctx, body_h);
                nk_layout_row_template_push_static(ctx, (float)LIST_W);
                nk_layout_row_template_push_dynamic(ctx);
                nk_layout_row_template_end(ctx);
                draw_list(ctx);
                draw_detail(ctx);
            }
            draw_statusbar(ctx);
        }
        nk_end(ctx);

        draw_paltest(ctx);
        if (g_pt_apply >= 0) {
            apply_palette(g_pt_apply);
            close_paltest();
            g_pt_apply = -1;
        }
        /* Deferred (tinyfd blocks, so never call it mid-render): locate a .DTA. */
        if (g_want_open) {
            g_want_open = 0;
            const char *picked = ask_open_dta();
            if (picked) { scan_archives(picked); mount_archive(picked); }
        }

        SDL_SetRenderDrawColor(g_ren, 26, 27, 32, 255);
        SDL_RenderClear(g_ren);
        nk_sdl_render(NK_ANTI_ALIASING_ON);

        if (shot_path && ++shot_frame >= 3) {
            int rw = 0, rh = 0;
            SDL_GetRendererOutputSize(g_ren, &rw, &rh);
            uint8_t *px = (uint8_t *)malloc((size_t)rw * rh * 4);
            if (px && SDL_RenderReadPixels(g_ren, NULL, SDL_PIXELFORMAT_RGBA32,
                                           px, rw * 4) == 0) {
                stbi_write_png(shot_path, rw, rh, 4, px, rw * 4);
                printf("screenshot -> %s (%dx%d)\n", shot_path, rw, rh);
            }
            free(px);
            running = 0;
        }
        SDL_RenderPresent(g_ren);
    }

    if (g_flic) vflic_close(g_flic);
    drop_loaded();
    catalog_close();
    vaudio_shutdown();
    nk_sdl_shutdown();
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
