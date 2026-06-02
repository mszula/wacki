/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* tests/test_main.c — entry point. Runs every suite and prints a summary.
 *
 * Add a new suite:
 *  1. Add a `tests/test_<name>.c` defining `SUITE(<name>) { RUN_TEST(...); }`.
 *  2. Add `extern void run_suite_<name>(int*, int*);` + a call below.
 *  3. Add the file to ENGINE/TEST_SRCS in the Makefile.
 *
 * Exit code: 0 if all passed, 1 if any failed.
 */

#include <stdio.h>
#include <setjmp.h>

#include "test.h"

/* Runner globals — declared `extern` in test.h, defined here once. */
jmp_buf g_test_jmp;
const char *g_test_name = "(none)";
int g_test_failed = 0;
int g_test_passed = 0;

/* ---- suite forward declarations ----------------------------------------- */

extern void run_suite_entity_layout(int *, int *);
extern void run_suite_rng(int *, int *);
extern void run_suite_pkv2(int *, int *);
extern void run_suite_archive(int *, int *);
extern void run_suite_save(int *, int *);
extern void run_suite_walker(int *, int *);
extern void run_suite_graphics(int *, int *);
extern void run_suite_pe_loader(int *, int *);
extern void run_suite_script_vm(int *, int *);
extern void run_suite_save_io(int *, int *);
extern void run_suite_assets(int *, int *);
extern void run_suite_font(int *, int *);
extern void run_suite_binary_data(int *, int *);
extern void run_suite_timer(int *, int *);
extern void run_suite_real_vm(int *, int *);
/* test_vm_dispatch — removed: all 16 cases were capture-counter
 * verifications. With production stubs.c linked, dispatch is exercised
 * via state-machine tests (test_inventory, test_sound_queue, etc.). */
extern void run_suite_inventory(int *, int *);
extern void run_suite_sound_queue(int *, int *);
extern void run_suite_vm_control_flow(int *, int *);
extern void run_suite_vm_script_parser(int *, int *);
extern void run_suite_vm_call_stack(int *, int *);
extern void run_suite_vm_more_ops(int *, int *);
extern void run_suite_heap_cygio(int *, int *);
extern void run_suite_vm_walk_anim(int *, int *);
extern void run_suite_vm_dialog(int *, int *);
extern void run_suite_vm_misc_ops(int *, int *);
extern void run_suite_vm_safety(int *, int *);
extern void run_suite_vm_game_over(int *, int *);
extern void run_suite_archive_extended(int *, int *);
extern void run_suite_pe_loader_malformed(int *, int *);
extern void run_suite_per_entity_vm(int *, int *);
extern void run_suite_assets_rle(int *, int *);
extern void run_suite_font_color(int *, int *);
extern void run_suite_save_slot(int *, int *);
extern void run_suite_vm_with_pe(int *, int *);
extern void run_suite_vm_show_text_bind(int *, int *);
extern void run_suite_graphics_alpha(int *, int *);
extern void run_suite_vm_wait_break(int *, int *);
extern void run_suite_archive_lifecycle(int *, int *);
extern void run_suite_pe_loader_lifecycle(int *, int *);
extern void run_suite_vm_corner_cases(int *, int *);
extern void run_suite_save_io_extended(int *, int *);
extern void run_suite_panel_hit_test(int *, int *);
extern void run_suite_click_hit_test(int *, int *);
extern void run_suite_per_entity_vm_real(int *, int *);
extern void run_suite_click_queue(int *, int *);
extern void run_suite_update_registration(int *, int *);
extern void run_suite_ent_ptr_intern(int *, int *);
extern void run_suite_sampl_parser(int *, int *);

/* ---- main --------------------------------------------------------------- */

struct suite {
    const char *name;
    void (*run)(int *, int *);
};

static const struct suite kSuites[] = {
    { "entity_layout", run_suite_entity_layout },
    { "rng",           run_suite_rng },
    { "pkv2",          run_suite_pkv2 },
    { "archive",       run_suite_archive },
    { "save",          run_suite_save },
    { "save_io",       run_suite_save_io },
    { "assets",        run_suite_assets },
    { "font",          run_suite_font },
    { "walker",        run_suite_walker },
    { "graphics",      run_suite_graphics },
    { "pe_loader",     run_suite_pe_loader },
    { "binary_data",   run_suite_binary_data },
    { "timer",         run_suite_timer },
    { "script_vm",     run_suite_script_vm },
    { "real_vm",          run_suite_real_vm },
    /* vm_dispatch removed — see comment above. */
    { "inventory",        run_suite_inventory },
    { "sound_queue",      run_suite_sound_queue },
    { "vm_control_flow",  run_suite_vm_control_flow },
    { "vm_script_parser", run_suite_vm_script_parser },
    { "vm_call_stack",    run_suite_vm_call_stack },
    { "vm_more_ops",      run_suite_vm_more_ops },
    { "vm_walk_anim",     run_suite_vm_walk_anim },
    { "vm_dialog",        run_suite_vm_dialog },
    { "vm_misc_ops",      run_suite_vm_misc_ops },
    { "vm_safety",        run_suite_vm_safety },
    { "vm_game_over",     run_suite_vm_game_over },
    { "per_entity_vm",    run_suite_per_entity_vm },
    { "archive_extended", run_suite_archive_extended },
    { "pe_loader_malformed", run_suite_pe_loader_malformed },
    { "assets_rle",       run_suite_assets_rle },
    { "font_color",       run_suite_font_color },
    { "save_slot",        run_suite_save_slot },
    { "vm_with_pe",       run_suite_vm_with_pe },
    { "vm_show_text_bind", run_suite_vm_show_text_bind },
    { "graphics_alpha",   run_suite_graphics_alpha },
    { "vm_wait_break",    run_suite_vm_wait_break },
    { "vm_corner_cases",  run_suite_vm_corner_cases },
    { "archive_lifecycle", run_suite_archive_lifecycle },
    { "pe_loader_lifecycle", run_suite_pe_loader_lifecycle },
    { "save_io_extended", run_suite_save_io_extended },
    { "heap_cygio",       run_suite_heap_cygio },
    { "panel_hit_test",   run_suite_panel_hit_test },
    { "click_hit_test",   run_suite_click_hit_test },
    { "per_entity_vm_real", run_suite_per_entity_vm_real },
    { "click_queue",         run_suite_click_queue },
    { "update_registration", run_suite_update_registration },
    { "ent_ptr_intern",      run_suite_ent_ptr_intern },
    { "sampl_parser",        run_suite_sampl_parser },
};

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Disable stdio buffering so output appears in real time even when
     * redirected to a file/pipe. Important for CI debugging if a test
     * hangs — without this, the last few PASS/FAIL lines may sit in the
     * libc buffer until the process exits. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* Tests hard-code POSIX "/tmp/wacki-test-*" paths. On MSYS2/mingw,
     * fopen treats those as "\tmp\..." on the current drive — which doesn't
     * exist on a fresh Windows runner. Ensure it does. Ignore errors:
     * if it already exists mkdir returns -1/EEXIST and that's fine. */
    test_mkdir("/tmp");

    int total_passed = 0;
    int total_failed = 0;
    int n = (int)(sizeof(kSuites) / sizeof(kSuites[0]));

    fprintf(stdout, "[wacki-tests] running %d suites\n", n);

    for (int i = 0; i < n; ++i) {
        fprintf(stdout, "\n[suite %d/%d] %s\n", i + 1, n, kSuites[i].name);
        int sp = 0, sf = 0;
        kSuites[i].run(&sp, &sf);
        total_passed += sp;
        total_failed += sf;
    }

    fprintf(stdout, "\n[wacki-tests] %d passed, %d failed\n",
            total_passed, total_failed);
    return total_failed == 0 ? 0 : 1;
}
