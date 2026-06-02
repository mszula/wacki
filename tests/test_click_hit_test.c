/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_click_hit_test.c — scene click-hit dispatch.
 *
 * src/stubs.c ClickHitTest() (1:1 with FUN_00406BB0) is the per-frame
 * cursor → verb_id resolver. It walks the unified click list (kind=1
 * sprite payloads + kind=2 mask payloads, both built by op 0x30 SPAWN /
 * op 0x2E-0x2F REG_VERB_MASK), runs per-pixel hit tests on the owner
 * entity, then picks the WINNER by foot_y depth:
 *
 *   - kind=2 (mask payload) first match → IMMEDIATE return (kind=2
 *     short-circuits the search — used for walk-behind scenery overlays).
 *   - kind=1 (sprite payload) matches → accumulate; deepest foot_y wins
 *     (foot_y at owner+0x12; higher = closer to camera).
 *   - No hits → out_verb = 0x26 (neutral sentinel).
 *
 * Per-pixel test dispatch:
 *   - kind=1: 8bpp atlas frame at owner+0x28, pixel != 0 → hit
 *   - kind=2: mask flag at owner+0x14:
 *       bit 0x10   = always-hit (bbox-only)
 *       bit 0x8000 = HITTABLE — REQUIRED unless 0x10 set
 *       flag^0x8000:
 *         1 = 8bpp pixel test (pixels at owner+0x16)
 *         2 = 1bpp packed test
 *         4 = bbox-only
 *         8 = TERMINATE (no hit)
 *
 * Verb resolution:
 *   - kind=1: click_desc+0x0E points to verb table {count, verbs...},
 *             indexed by owner's current frame (owner+0x30); also cached
 *             back to click_desc+0x12.
 *   - kind=2: verb pre-baked at click_desc+0x12.
 *
 * Reference: src/stubs.c:2008-2155 / FUN_00406BB0.
 */

#include "test.h"
#include "wacki.h"
#include "test_engine_stubs.h"

#include <stdint.h>
#include <string.h>

extern int ClickHitTest(int16_t mouse_x, int16_t mouse_y, uint16_t *out_verb);

extern Entity *g_click_list_head;
extern void    EntityListClearAll(void);
extern void    LinkEntityToList(Entity **head, Entity *e, int position);
extern uint32_t ent_ptr_intern(void *p);

/* Scratch buffers for click descriptors + owner entities. Test reuses
 * the same buffer across tests via reset_clicks(). Sized for up to 4
 * owners + 4 click descriptors. */
static uint8_t  s_owners [4][256];
static uint8_t  s_clicks [4][256];

/* Configure owner entity at +0x0A draw_x/+0x0C draw_y/+0x0E width/
 * +0x10 height/+0x12 foot_y. (Per Entity layout: e[+0x0A]=drawX,
 * +0x0C=drawY, +0x0E=width, +0x10=height, +0x12=foot_y.) */
static Entity *make_owner(int slot, int16_t x, int16_t y,
                          uint16_t w, uint16_t h, int16_t foot_y)
{
    uint8_t *ob = s_owners[slot];
    memset(ob, 0, sizeof s_owners[slot]);
    *(int16_t  *)(ob + 0x0A) = x;
    *(int16_t  *)(ob + 0x0C) = y;
    *(uint16_t *)(ob + 0x0E) = w;
    *(uint16_t *)(ob + 0x10) = h;
    *(int16_t  *)(ob + 0x12) = foot_y;
    return (Entity *)ob;
}

/* Build a kind=2 mask-style click descriptor (bbox-only flag 0x8004
 * means hit-test reduces to bounding-box check — simplest setup). */
static Entity *make_click_kind2(int slot, Entity *owner,
                                uint16_t cached_verb, uint16_t mask_flag)
{
    uint8_t *cb = s_clicks[slot];
    memset(cb, 0, sizeof s_clicks[slot]);
    *(uint16_t *)(cb + 0x08) = 2;                  /* kind=2 */
    *(uint32_t *)(cb + 0x0A) = ent_ptr_intern(owner);
    *(uint32_t *)(cb + 0x0E) = 0;                  /* no verb table → cached */
    *(uint16_t *)(cb + 0x12) = cached_verb;
    *(uint16_t *)((uint8_t *)owner + 0x14) = mask_flag;
    return (Entity *)cb;
}

/* Build a kind=1 sprite click descriptor with a verb table.
 * verb_tab[0]=count, verb_tab[1..]=verbs per frame. Owner+0x30 holds
 * the current frame index. */
static Entity *make_click_kind1(int slot, Entity *owner,
                                uint16_t *verb_tab, uint16_t frame,
                                AnimAsset *atlas)
{
    uint8_t *cb = s_clicks[slot];
    memset(cb, 0, sizeof s_clicks[slot]);
    *(uint16_t *)(cb + 0x08) = 1;                  /* kind=1 */
    *(uint32_t *)(cb + 0x0A) = ent_ptr_intern(owner);
    *(uint32_t *)(cb + 0x0E) = ent_ptr_intern(verb_tab);

    /* Owner needs: atlas slot @+0x28, current frame @+0x30. */
    uint8_t *ob = (uint8_t *)owner;
    *(uint32_t *)(ob + 0x28) = ent_ptr_intern(atlas);
    *(uint16_t *)(ob + 0x30) = frame;
    return (Entity *)cb;
}

static void reset_clicks(void)
{
    EntityListClearAll();
}

static void install_clicks(Entity **list, int n)
{
    EntityListClearAll();
    for (int i = 0; i < n; ++i) {
        LinkEntityToList(&g_click_list_head, list[i], /*position=*/0);
    }
}

/* ---- empty list returns sentinel ------------------------------------ */

TEST(click_empty_list_returns_sentinel)
{
    reset_clicks();
    uint16_t out = 0xAAAA;
    int hit = ClickHitTest(100, 100, &out);
    ASSERT_EQ(hit, 0);
    ASSERT_EQ(out, 0x26);
}

/* ---- kind=2 mask, bbox-only, mouse inside hits ---------------------- */

TEST(click_kind2_bbox_hit_inside)
{
    reset_clicks();
    Entity *o = make_owner(0, /*x=*/50, /*y=*/50, /*w=*/100, /*h=*/80, /*foot_y=*/130);
    Entity *c = make_click_kind2(0, o, /*verb=*/0x42, /*flag=*/0x8004);
    Entity *list[1] = { c };
    install_clicks(list, 1);

    uint16_t out = 0;
    int hit = ClickHitTest(100, 90, &out);
    ASSERT_EQ(hit, 1);
    ASSERT_EQ(out, 0x42);
}

TEST(click_kind2_bbox_hit_outside_misses)
{
    reset_clicks();
    Entity *o = make_owner(0, 50, 50, 100, 80, 130);
    Entity *c = make_click_kind2(0, o, 0x42, 0x8004);
    Entity *list[1] = { c };
    install_clicks(list, 1);

    uint16_t out = 0xAAAA;
    int hit = ClickHitTest(10, 10, &out);
    ASSERT_EQ(hit, 0);
    ASSERT_EQ(out, 0x26);                          /* sentinel */
}

TEST(click_kind2_bbox_left_edge_inclusive)
{
    /* Production uses `mouse_x < ox` to MISS — so mouse_x == ox is INSIDE. */
    reset_clicks();
    Entity *o = make_owner(0, 50, 50, 100, 80, 130);
    Entity *c = make_click_kind2(0, o, 0x42, 0x8004);
    Entity *list[1] = { c };
    install_clicks(list, 1);

    uint16_t out = 0;
    int hit = ClickHitTest(/*mouse_x=*/50, /*mouse_y=*/60, &out);
    ASSERT_EQ(hit, 1);
    ASSERT_EQ(out, 0x42);
}

TEST(click_kind2_bbox_right_edge_exclusive)
{
    /* `mouse_x >= ox + ow` MISSES. ox=50, ow=100 → 150 is out. */
    reset_clicks();
    Entity *o = make_owner(0, 50, 50, 100, 80, 130);
    Entity *c = make_click_kind2(0, o, 0x42, 0x8004);
    Entity *list[1] = { c };
    install_clicks(list, 1);

    uint16_t out = 0xAAAA;
    int hit = ClickHitTest(150, 60, &out);
    ASSERT_EQ(hit, 0);
    ASSERT_EQ(out, 0x26);
}

/* ---- kind=2 HIDE'd mask does not hit -------------------------------- */

TEST(click_kind2_hidden_mask_does_not_hit)
{
    /* mask_flag=0x0004 → HITTABLE (0x8000) cleared. Production
     * early-continues (the T-RopeExit fix). */
    reset_clicks();
    Entity *o = make_owner(0, 50, 50, 100, 80, 130);
    Entity *c = make_click_kind2(0, o, 0x42, /*flag=*/0x0004);
    Entity *list[1] = { c };
    install_clicks(list, 1);

    uint16_t out = 0xAAAA;
    int hit = ClickHitTest(100, 90, &out);
    ASSERT_EQ(hit, 0);
    ASSERT_EQ(out, 0x26);
}

/* ---- kind=2 always-hit override (bit 0x10) -------------------------- */

TEST(click_kind2_always_hit_overrides_hidden)
{
    /* bit 0x10 set forces hit even without HITTABLE bit. */
    reset_clicks();
    Entity *o = make_owner(0, 50, 50, 100, 80, 130);
    Entity *c = make_click_kind2(0, o, 0x42, /*flag=*/0x0010);
    Entity *list[1] = { c };
    install_clicks(list, 1);

    uint16_t out = 0;
    int hit = ClickHitTest(100, 90, &out);
    ASSERT_EQ(hit, 1);
    ASSERT_EQ(out, 0x42);
}

/* ---- kind=2 TERMINATE flag means no hit ----------------------------- */

TEST(click_kind2_terminate_flag_no_hit)
{
    /* mask_flag = 0x8008 → flag^0x8000 = 8 → switch case 8 → hit=0. */
    reset_clicks();
    Entity *o = make_owner(0, 50, 50, 100, 80, 130);
    Entity *c = make_click_kind2(0, o, 0x42, /*flag=*/0x8008);
    Entity *list[1] = { c };
    install_clicks(list, 1);

    uint16_t out = 0xAAAA;
    int hit = ClickHitTest(100, 90, &out);
    ASSERT_EQ(hit, 0);
    ASSERT_EQ(out, 0x26);
}

/* ---- kind=2 first hit returns IMMEDIATELY --------------------------- */

TEST(click_kind2_first_hit_short_circuits)
{
    /* Two overlapping kind=2 masks. Production returns on first hit.
     * Click list is LIFO (LinkEntityToList prepends position=0), so the
     * LAST installed = head of the list = first walked. Whichever is
     * at index 0 wins regardless of foot_y. */
    reset_clicks();
    Entity *o1 = make_owner(0, 50, 50, 100, 80, /*foot_y=*/100);
    Entity *o2 = make_owner(1, 50, 50, 100, 80, /*foot_y=*/200);
    Entity *c1 = make_click_kind2(0, o1, /*verb=*/0xAA, 0x8004);
    Entity *c2 = make_click_kind2(1, o2, /*verb=*/0xBB, 0x8004);
    Entity *list[2] = { c1, c2 };
    install_clicks(list, 2);
    /* After install: head→c2→c1 (last installed = at front). */

    uint16_t out = 0;
    int hit = ClickHitTest(100, 90, &out);
    ASSERT_EQ(hit, 1);
    ASSERT_EQ(out, 0xBB);                          /* c2 walked first */
}

/* ---- kind=1 sprite, 8bpp pixel test --------------------------------- */

TEST(click_kind1_opaque_pixel_hits)
{
    /* Build a 4x4 atlas frame with a single opaque pixel at (2, 2). */
    static uint8_t pixels[16] = {
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 1, 0,                                /* opaque @ (2,2) */
        0, 0, 0, 0,
    };
    static uint8_t *pixel_ptrs[1] = { pixels };
    static AnimAsset atlas = {
        .frame_count = 1,
        .pixel_ptrs  = pixel_ptrs,
    };
    static uint16_t verb_tab[2] = { /*count=*/1, /*verb=*/0xC3 };

    reset_clicks();
    Entity *o = make_owner(0, /*x=*/50, /*y=*/50, /*w=*/4, /*h=*/4, /*foot=*/54);
    Entity *c = make_click_kind1(0, o, verb_tab, /*frame=*/0, &atlas);
    Entity *list[1] = { c };
    install_clicks(list, 1);

    /* Mouse at (52, 52) → owner-local (2, 2) → opaque. */
    uint16_t out = 0;
    int hit = ClickHitTest(52, 52, &out);
    ASSERT_EQ(hit, 1);
    ASSERT_EQ(out, 0xC3);
}

TEST(click_kind1_transparent_pixel_misses)
{
    /* Same atlas but mouse at a TRANSPARENT pixel (1, 1) — value 0. */
    static uint8_t pixels[16] = {
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 0,
    };
    static uint8_t *pixel_ptrs[1] = { pixels };
    static AnimAsset atlas = {
        .frame_count = 1,
        .pixel_ptrs  = pixel_ptrs,
    };
    static uint16_t verb_tab[2] = { 1, 0xC3 };

    reset_clicks();
    Entity *o = make_owner(0, 50, 50, 4, 4, 54);
    Entity *c = make_click_kind1(0, o, verb_tab, 0, &atlas);
    Entity *list[1] = { c };
    install_clicks(list, 1);

    uint16_t out = 0xAAAA;
    int hit = ClickHitTest(51, 51, &out);           /* (1,1) → 0 */
    ASSERT_EQ(hit, 0);
    ASSERT_EQ(out, 0x26);
}

/* ---- kind=1 frame indexes verb table -------------------------------- */

TEST(click_kind1_frame_selects_verb)
{
    /* 3-frame verb table; advancing the owner's frame changes the
     * verb that hit-test returns. */
    static uint8_t pixels[1] = { 1 };
    static uint8_t *pixel_ptrs[3] = { pixels, pixels, pixels };
    static AnimAsset atlas = {
        .frame_count = 3,
        .pixel_ptrs  = pixel_ptrs,
    };
    static uint16_t verb_tab[4] = { 3, 0xAA, 0xBB, 0xCC };

    reset_clicks();
    Entity *o = make_owner(0, 100, 100, 1, 1, 100);
    Entity *c = make_click_kind1(0, o, verb_tab, /*frame=*/1, &atlas);
    Entity *list[1] = { c };
    install_clicks(list, 1);

    uint16_t out = 0;
    int hit = ClickHitTest(100, 100, &out);
    ASSERT_EQ(hit, 1);
    ASSERT_EQ(out, 0xBB);                          /* frame 1 → verb[2]=0xBB */
}

TEST(click_kind1_frame_overflow_uses_last_verb)
{
    /* When owner frame >= verb_tab count, production clamps to
     * (count - 1) and uses the LAST verb in the table.
     *
     * Important: atlas frame_count must be LARGER than the owner's
     * frame, otherwise the earlier pixel-test code also clamps
     * cur_frame to 0 (and then the verb-table lookup sees 0, not
     * the real out-of-range frame). Use atlas frame_count = 10
     * and verb_tab count = 3 so they decouple. */
    static uint8_t pixels[1] = { 1 };
    static uint8_t *pixel_ptrs[10] = {
        pixels, pixels, pixels, pixels, pixels,
        pixels, pixels, pixels, pixels, pixels,
    };
    static AnimAsset atlas = {
        .frame_count = 10,                         /* > owner frame (5) */
        .pixel_ptrs  = pixel_ptrs,
    };
    static uint16_t verb_tab[4] = { 3, 0xAA, 0xBB, 0xCC };

    reset_clicks();
    Entity *o = make_owner(0, 100, 100, 1, 1, 100);
    Entity *c = make_click_kind1(0, o, verb_tab, /*frame=*/5, &atlas);
    Entity *list[1] = { c };
    install_clicks(list, 1);

    uint16_t out = 0;
    int hit = ClickHitTest(100, 100, &out);
    ASSERT_EQ(hit, 1);
    ASSERT_EQ(out, 0xCC);                          /* idx clamped to count-1=2 → vt[3] */
}

/* ---- kind=1 foot_y depth sort: deepest wins ------------------------- */

TEST(click_kind1_deeper_foot_y_wins)
{
    /* Two overlapping kind=1 sprites — the one with HIGHER foot_y
     * (closer to camera) wins regardless of list order.
     *
     * Production: `if (foot_y > hit_foot_y) { hit_verb = …; }`
     * After walking the list, hit_verb reflects the maximum foot_y. */
    static uint8_t pixels[1] = { 1 };
    static uint8_t *pixel_ptrs[1] = { pixels };
    static AnimAsset atlas = { .frame_count = 1, .pixel_ptrs = pixel_ptrs };
    static uint16_t verb_far [2] = { 1, 0x10 };    /* low foot_y */
    static uint16_t verb_near[2] = { 1, 0x20 };    /* high foot_y */

    reset_clicks();
    /* Owner A foot_y=100, Owner B foot_y=200 — both overlap (50,50,1,1). */
    Entity *oA = make_owner(0, 50, 50, 1, 1, /*foot_y=*/100);
    Entity *oB = make_owner(1, 50, 50, 1, 1, /*foot_y=*/200);
    Entity *cA = make_click_kind1(0, oA, verb_far,  0, &atlas);
    Entity *cB = make_click_kind1(1, oB, verb_near, 0, &atlas);
    Entity *list[2] = { cA, cB };                  /* head = cB then cA */
    install_clicks(list, 2);

    uint16_t out = 0;
    int hit = ClickHitTest(50, 50, &out);
    ASSERT_EQ(hit, 1);
    ASSERT_EQ(out, 0x20);                          /* near (foot_y=200) wins */
}

TEST(click_kind1_list_order_does_not_change_winner)
{
    /* Same setup but reversed install order — still deeper foot_y wins.
     * This guarantees foot_y sort works regardless of which sprite
     * was registered first. */
    static uint8_t pixels[1] = { 1 };
    static uint8_t *pixel_ptrs[1] = { pixels };
    static AnimAsset atlas = { .frame_count = 1, .pixel_ptrs = pixel_ptrs };
    static uint16_t verb_far [2] = { 1, 0x10 };
    static uint16_t verb_near[2] = { 1, 0x20 };

    reset_clicks();
    Entity *oA = make_owner(0, 50, 50, 1, 1, 100);
    Entity *oB = make_owner(1, 50, 50, 1, 1, 200);
    Entity *cA = make_click_kind1(0, oA, verb_far,  0, &atlas);
    Entity *cB = make_click_kind1(1, oB, verb_near, 0, &atlas);
    Entity *list[2] = { cB, cA };                  /* swap install order */
    install_clicks(list, 2);

    uint16_t out = 0;
    int hit = ClickHitTest(50, 50, &out);
    ASSERT_EQ(hit, 1);
    ASSERT_EQ(out, 0x20);
}

/* ---- kind=2 short-circuit prevents kind=1 deeper from winning ------- */

TEST(click_kind2_short_circuits_even_with_deeper_kind1)
{
    /* When kind=2 hits first (it's the LIFO head), production returns
     * immediately — kind=1 with a deeper foot_y never gets compared. */
    static uint8_t pixels[1] = { 1 };
    static uint8_t *pixel_ptrs[1] = { pixels };
    static AnimAsset atlas = { .frame_count = 1, .pixel_ptrs = pixel_ptrs };
    static uint16_t verb_kind1[2] = { 1, 0xDD };

    reset_clicks();
    Entity *o1 = make_owner(0, 50, 50, 100, 80, /*foot_y=*/200);     /* DEEPER */
    Entity *o2 = make_owner(1, 50, 50, 100, 80, /*foot_y=*/100);
    Entity *c1 = make_click_kind1(0, o1, verb_kind1, 0, &atlas);
    Entity *c2 = make_click_kind2(1, o2, /*verb=*/0xEE, 0x8004);
    /* Install kind=1 first, then kind=2 → kind=2 ends up at head. */
    Entity *list[2] = { c1, c2 };
    install_clicks(list, 2);

    uint16_t out = 0;
    int hit = ClickHitTest(100, 90, &out);
    ASSERT_EQ(hit, 1);
    ASSERT_EQ(out, 0xEE);                          /* kind=2 wins */
}

/* ---- cached verb mirror on kind=1 hit ------------------------------- */

TEST(click_kind1_caches_verb_at_descriptor_plus_12)
{
    /* After a kind=1 hit, production writes the resolved verb back to
     * click_desc+0x12 so FindEntityByVerbId can read it without rerunning
     * the lookup. */
    static uint8_t pixels[1] = { 1 };
    static uint8_t *pixel_ptrs[1] = { pixels };
    static AnimAsset atlas = { .frame_count = 1, .pixel_ptrs = pixel_ptrs };
    static uint16_t verb_tab[2] = { 1, 0x77 };

    reset_clicks();
    Entity *o = make_owner(0, 50, 50, 1, 1, 50);
    Entity *c = make_click_kind1(0, o, verb_tab, 0, &atlas);
    *(uint16_t *)((uint8_t *)c + 0x12) = 0x00;     /* pre-clear cache */
    Entity *list[1] = { c };
    install_clicks(list, 1);

    uint16_t out = 0;
    (void)ClickHitTest(50, 50, &out);
    ASSERT_EQ(out, 0x77);
    /* Now cache slot at +0x12 must reflect 0x77. */
    ASSERT_EQ(*(uint16_t *)((uint8_t *)c + 0x12), 0x77);
}

/* ---- mouse outside bbox does not consult pixel test ----------------- */

TEST(click_kind1_outside_bbox_skips_pixel_test)
{
    /* Owner at (50,50,1,1) — mouse at (200,200) → bbox miss. Pixel ptr
     * is NULL so a stray pixel lookup would crash; this test just
     * verifies the bbox guard fires first. */
    static AnimAsset atlas = { .frame_count = 1, .pixel_ptrs = NULL };
    static uint16_t verb_tab[2] = { 1, 0x99 };

    reset_clicks();
    Entity *o = make_owner(0, 50, 50, 1, 1, 50);
    Entity *c = make_click_kind1(0, o, verb_tab, 0, &atlas);
    Entity *list[1] = { c };
    install_clicks(list, 1);

    uint16_t out = 0xAAAA;
    int hit = ClickHitTest(200, 200, &out);        /* must not crash */
    ASSERT_EQ(hit, 0);
    ASSERT_EQ(out, 0x26);
}

SUITE(click_hit_test)
{
    RUN_TEST(click_empty_list_returns_sentinel);
    RUN_TEST(click_kind2_bbox_hit_inside);
    RUN_TEST(click_kind2_bbox_hit_outside_misses);
    RUN_TEST(click_kind2_bbox_left_edge_inclusive);
    RUN_TEST(click_kind2_bbox_right_edge_exclusive);
    RUN_TEST(click_kind2_hidden_mask_does_not_hit);
    RUN_TEST(click_kind2_always_hit_overrides_hidden);
    RUN_TEST(click_kind2_terminate_flag_no_hit);
    RUN_TEST(click_kind2_first_hit_short_circuits);
    RUN_TEST(click_kind1_opaque_pixel_hits);
    RUN_TEST(click_kind1_transparent_pixel_misses);
    RUN_TEST(click_kind1_frame_selects_verb);
    RUN_TEST(click_kind1_frame_overflow_uses_last_verb);
    RUN_TEST(click_kind1_deeper_foot_y_wins);
    RUN_TEST(click_kind1_list_order_does_not_change_winner);
    RUN_TEST(click_kind2_short_circuits_even_with_deeper_kind1);
    RUN_TEST(click_kind1_caches_verb_at_descriptor_plus_12);
    RUN_TEST(click_kind1_outside_bbox_skips_pixel_test);
}
