# Unit tests for wacki-src

Custom minimal header-only test framework + 41 suites covering the
SDL-independent subset of the engine. **Production `stubs.c` linked**
(via expanded SDL stub) — Inventory state machine and sound queue
stereo pan math tested end-to-end on real production code. Designed as a **safety net for
refactoring** — every test pins a contract that's load-bearing for the
port, so if a refactor accidentally changes byte layout, opcode
constants, depack output, RNG sequence, VM dispatch, or PE-loader
behavior, the test that owns that contract fails immediately with a
precise diff.

The VM coverage in particular is deep: production `RunScriptInterpreter`
(src/script.c, 1395 LOC) is linked into the test binary via a stub
`SDL.h` (`tests/sdl_stub/`) and ~30 capture-instrumented function stubs
in `tests/test_engine_stubs.c`. Tests build tiny hand-crafted bytecode
programs, run them through the REAL dispatcher, and verify post-state
of `g_script_vars` / call counts on the captured stubs.

## Quick start

```
make test
```

Builds `tests/run-tests` (no SDL — uses `tests/sdl_stub/SDL.h`) and
runs all 41 suites. Exit code 0 if every test passes. Output:

```
[wacki-tests] running 41 suites

[suite 1/41] entity_layout
  PASS  entity_size_at_least_e0
…
[wacki-tests] 353 passed, 0 failed
```

## Coverage map

| Suite              | Cases | What it locks                                                          | Production code                |
|--------------------|------:|------------------------------------------------------------------------|--------------------------------|
| `entity_layout`    | 3 + 27 SA | Byte offsets of `Entity`, `WackiSlot`, `WackiSaveFile`, magic constants (compile-time `_Static_assert`) | `include/wacki.h`              |
| `rng`              | 8 | `WackiRand` determinism + golden-vector first 6 outputs for seed=1     | `src/stubs.c` WackiRand        |
| `pkv2`             | 7 | PKv2 depack header parse + literal mode + sanity bails                 | `src/depack.c`                 |
| `archive`          | 5 | DTA mount + name lookup (case-fold) + miss/bad-magic handling          | `src/archive.c`                |
| `save`             | 9 | `.sav` wire format constants + struct sizes + field offsets            | `include/wacki.h` save consts  |
| `save_io`          | 6 | Wacki.sav file I/O — defaults, roundtrip, atomic write, corrupt magic  | `src/save.c`                   |
| `assets`           | 5 | ANIM/MASK/FILD header parse + bbox + flag_22/kind logic                | `src/assets.c`                 |
| `font`             | 8 | `FindKeyInTaggedTable` (tagged-table walker) + `ParseFutFontFile` 1bpp | `src/font.c`                   |
| `walker`           | 12 | Fixed-point line stepper (16.16 accumulator + no-overshoot contract)   | `src/actor.c` ~648-711 (characterization) |
| `graphics`         | 19 | `DepackRleFrame` (RLE decode + marker_A==marker_B collision) + `InstallPalette` (full/partial/out-of-range/first=255 boundary) + `SetAlphaTint` + **BlitSpriteToBackbuffer** (color-key mode 0, opaque mode 1, right-edge clip, negative dx left-clip, negative dy top-clip, NULL src safety) + **PaintImageToBackbuffer** | `src/graphics.c`               |
| `pe_loader`        | 7 | Minimal PE32 build + section map + VA range check                      | `src/pe_loader.c`              |
| `binary_data`      | 4 | `xlat_binary_ptr` / `xlat_asset_name` — NULL guard + PE integration    | `src/binary_data.c`            |
| `timer`            | 3 | Multimedia timer stub semantics (rc=1/0, no callback fired)            | `src/timer.c`                  |
| `script_vm`        | 13 | Opcode constants + bytecode operand layout + mock interpreter exec    | `docs/script-vm.md` (characterization) |
| `real_vm`          | 17 | **PRODUCTION** `RunScriptInterpreter`: VAR_SET/ADD/SUB/OR/ANDNOT, this/that remap on a0=0x27/0x28, GET_CUR_ROOM, GET_ACTOR_ID, IS_ITEM signed underflow, GET_HELD_ITEM, InventoryHasItem, ABORT vs EOF vs END_FORCE semantics | `src/script.c` |
| `vm_dispatch`      | 16 | **PRODUCTION** opcode→ScriptCall* dispatch with capture stubs: SOUND_PLAY (i32_at8 quirk), SOUND_STOP, HIDE_ENT, SHOW_ENT, DESTROY_ENT 0x31/0x32, GO_EXIT (0x26 sentinel), PAL_LOAD with/without fade, PAL_FADE_STEP, SAVE_TICK, GET_TICK_DELTA, BG_MASK_SETUP, LOAD_ASSET (inline string path) | `src/script.c` |
| `vm_control_flow`  | 15 | **PRODUCTION** IF/ELSE/GOTO/LABEL/LOOP including round-31 regression: `skip_to_endif` and `find_label` step over op 0x55 END_FORCE (only 0x56 terminates scan). Nested IF skip depth balancing. Signed `IF_GT` (op 0x02). Six IF variants taken/skipped. | `src/script.c` |
| `vm_script_parser` | 10 | `LoadScriptFile` (filesystem + DTA fallback), `FindScriptByStageAndRoom`, `ScriptObjFindSection` with/without altparam terminator, last-section extends to EOF, accessors with NULL self | `src/script.c` |
| `vm_call_stack`    | 6 | CALL_SUB (0x25) + TAILCALL (0x24) via minimal PE fixture w `/tmp` — callee runs, control returns post-call, propagate this/that, sequential calls, unresolved VA falls through, stack overflow recovery contract | `src/script.c` + `src/pe_loader.c` |
| `vm_more_ops`      | 22 | Pozostałe opcodes: SET_CURSOR_SPEED (0x40), SET_VERB / g_held_item (0x2B) z 4 variantami (item range / 0x26 sentinel / above range / this-swap), FLAG_CLEAR/SET (0x43/0x44 dead-state), IS_SOUND_PLAYING (0x2A → WackiRand bridge), PRELOAD_LIST_A/B (0x2E/0x2F), SPAWN_ENTITY (0x30), NUDGE_ENT (0x47) z entity fixture, GET_ENT_X/Y (0x4B/0x4C) — atlas+flag obie ścieżki, SET_ENT_POS (0x28), TIMER_SET (0x0B), WAIT_FRAME (0x13), DEBUG_LOG (0x57) | `src/script.c` |
| `vm_walk_anim`     | 11 | ANIM_ACTOR1/2 (0x10/0x11), ANIM_BOTH (0x12) → ActorWalkBothBlocking z mode capture, WALK_MODE1/2 (0x35/0x37), WALKTO_MODE1/2 (0x38/0x3A z target=reg_id quirk), ATTACH_PROP_TMP/KEEP (0x3B/0x3C), RESET_ACTORS (0x33) — czyści 10 walker fields na g_actor[0/1] | `src/script.c` |
| `vm_dialog`        | 10 | SHOW_TEXT (0x09 z g_speech_balloon NULL exit), QUEUE_DIALOG (0x19), OPEN_MENU (0x1A → add+setpage+swap), CLOSE_MENU (0x1B → remove+collapse+swap), PAGE_NEXT/PREV (0x1C/0x1D) z conditional swap, DROP (0x1F), BEGIN/END_DIALOG (0x52/0x53) | `src/script.c` |
| `vm_misc_ops`      | 14 | SET_ENTITY_SCRIPT (0x0E) + SET_ENTITY_ANIM (0x0F) — entity state reset blocks, WAIT_MS (0x14) — 4 process_frame calls dla a0=10, WAIT_ENT_IDLE (0x15), WAIT_ANIM_FRAME (0x26 + 0x3D), SET_OBJ_PROP (0x27) unresolved VA safe, SUBANIM_HIDE/SET (0x50/0x51), SHOW_PICTURE (0x54) NULL name path | `src/script.c` |
| `vm_safety`        | 9 | **PRODUCTION safety net**: op > 0x57 → END, len > 32 → END, boundary at op exactly 0x57 (valid), boundary at len 32 (valid), var index masking 0x1FF, this/that remap z dużymi this_id, **bogus op inside CALL_SUB callee pops stack** (regression test) | `src/script.c` lines 330-345 |
| `vm_game_over`     | 9 | **CRITICAL Bug 2 fix**: VAR_SET var[14] (=1/3/4 death/chapter/stage-end) musi być widoczne via `g_game_over_code` macro (alias do `*(int *)&g_script_vars[14]`), VAR_ADD/VAR_OR na var[14] propagują, neighbour spill nie zachodzi (vars[13]/[15] safe), g_return_reg alias do low word var[4], op 0x29 GET_CUR_ROOM zapisuje tylko low 16 bits | `include/wacki.h:570` + `src/script.c` |
| `per_entity_vm`    | 9 | **Characterization** FUN_004012E0 (actor.c, niełinkowane): opcode constants (END=0x21, LABEL=0x0A), stride contract `dlt * 2` bytes (vs main VM `len * 4`), END terminator, dlt=0 defensive break, scan_for_label by exact id + wildcard 0xFFFF, scan stops at END | `src/actor.c` 279-360 |
| `archive_extended` | 4 | Multi-entry DTA (5 files, all findable), repeated lookups (no state leak), mixed-case query matches uppercase entry, zero-entry terminator NIE matchuje empty query | `src/archive.c` |
| `pe_loader_malformed` | 9 | File < 0x200 rejected, e_lfanew past EOF rejected, missing PE signature rejected, optional header < 0x60 rejected, too many sections rejected, virtual extent > 0x10000000 rejected, virtual extent < 0x1000 rejected, **section rptr past EOF SKIPS but load succeeds** (calloc-zero default), NULL path rejected | `src/pe_loader.c` |
| `assets_rle`       | 3 | **ANIM kind=3 integration**: flag_22 != 0 → kind=3, pixel_ptrs[N] punkt na RLE block (3-byte header + stream), DepackRleFrame integration decoduje frame poprawnie, flag_22 raw bits preserved (alpha-plane marker bit 0) | `src/assets.c` + `src/graphics.c` |
| `font_color`       | 4 | **Futura.30 color font** (sub-magic 0x3EA + raw[0x70] bit 0x40): plane_count from raw[0x90], plane[i] offsets at +0x9A + i*4 (BE32), zero-offset plane skipped (NULL), sub-magic 0x3EA accepted, gating purely na flag byte | `src/font.c` |
| `save_slot`        | 11 | LoadSaveSlot: script_vars / entity_state / scene_snapshot field-copy z slot do live globals, g_cur_etap + g_cur_komnata set, out-of-range rejected, empty slot (stage_indicator=0) rejected, QuickSaveToSlot refuses gdy nie-in-game, QuickLoadFromSlot empty slot rejected | `src/save.c` |
| `vm_with_pe`       | 7 | **VM ops z PE fixture**: op 0x0E SET_ENTITY_SCRIPT z resolvable VA → bytecode bound do entity[+0x2C] przez ent_ptr_intern; op 0x28 SET_ENT_POS **foot-anchor compensation** (atlas + flag bit 1 → drawn_x = raw + atlas.off_drawX[fid]), no-atlas path, flag-not-set path, frame >= frame_count safety; op 0x27 SET_OBJ_PROP — tagged-table write (idx != 0), idx == 0 first-record skip | `src/script.c` + `src/pe_loader.c` |
| `vm_show_text_bind` | 4 | **Op 0x09 speech-bind path**: QUEUE_DIALOG (0x19) push, SHOW_TEXT (0x09) match → FindEntityByVerbId, xlat_binary_ptr bytecode → reset state + ent_ptr_intern bind, walker state cleared, g_speech_unbind_speaker/data stash gated by `if (sp)` (NOT `if (new_bc)`), no-queue / no-entity / unresolved-ptr-but-sp-found permutations | `src/script.c` case 0x09 |
| `graphics_alpha`   | 9 | **BlitAlphaScaled** modes 0/1/2: mode 0 (nearest) 1:1 passthrough, 2× upscale/downscale x i y axes, mode 1 (1D box + RGB12 quant) z grayscale palette roundtrips close-to-source, mode 2 (2D box) smoke, safety bails (dst > 0x400, NULL src, zero dims) | `src/graphics.c` |
| `vm_wait_break`    | 9 | **Wait-loop break conditions** (op 0x14/0x15/0x26/0x3D): PlatformShouldQuit settable stub → wait loops exit after 1 iter; g_game_over_code (= var[14]) → same; WAIT_MS continues normalnie bez break; **safety cap 2000** w op 0x26/0x3D verified (program nie hanguje gdy żadne break nie firuje) | `src/script.c` lines 551, 575, 596, 620 |
| `vm_corner_cases`  | 8 | VAR_SET imm=0 (explicit zero), VAR_OR imm=0 (no-op), **IF_AND imm=0 always-taken** ((v & 0) == 0), LOOP limit=1 (exactly 1 iter), **chained TAILCALL** (A → B → END_FORCE via 2 PE blocks), GOTO to LABEL at offset 0, 25-VAR_SET long program smoke, ELSE-after-taken-IF skips entire ELSE body to ENDIF | `src/script.c` |
| `archive_lifecycle` | 4 | OpenDtaArchiveFile dwa razy z różnymi archives → second replaces first (lookup miss/hit flip), same path twice idempotent, **case-fold basename retry** (lowercase path → fopen fail → uppercase retry succeeds), no-uppercase fallback path | `src/archive.c` |
| `pe_loader_lifecycle` | 6 | Free clears Loaded/Read/ContainsVA state, multiple Free safe (no double-free), **second Init replaces first** (VA range flips), Init without Free works (visible state replaced), **failed Init keeps previous mapping** (conservative), 3-cycle Free/Init alternation | `src/pe_loader.c` |
| `save_io_extended` | 6 | All 10 slots writable independently + readable, **all WackiSettings fields** persist (video_mode/sound/music/voice/subtitles/dialogues/pad), settings+slots roundtrip together, repeated Writes preserve last-written, entity_state + scene_snapshot per-slot, file size remains WACKI_SAVE_SIZE across writes | `src/save.c` |
| `heap_cygio`       | 8 | xmalloc/xfree/xcalloc (zero-init flag), fopen_cyg roundtrip read/write/seek SEEK_SET/SEEK_END/tell, fclose_cyg NULL safety | `src/heap.c` + `src/platform/sdl/file_host.c` |

### What's NOT covered

These were intentionally skipped because they need either SDL state
beyond `SDL_Delay` (which we stub) or a deeply-stateful subsystem the
tests can't isolate without rebuilding the whole engine:

- **Audio mixer** (`src/audio.c`) — pulls SDL_AudioDevice + thread
  callbacks. Covered by smoke runner + manual playthrough.
- **Renderer blit paths** (`BlitSpriteScaledColorKey`, etc.) — write
  into `g_back_shadow` which is sized via `PlatformInit`. Covered
  end-to-end by interactive runs + `wac*.bmp` reference dumps.
- **Game loop** (`src/game.c`) — pulls stubs.c + actor.c + audio.c.
- **FLIC/AVI decoder** (`src/flic.c`) — pulls platform layer.
- **Walker tick / EntityListClearAll** in `actor.c` — pulls graphics +
  stubs. Walker MATH is covered by `walker` suite (characterization).
- **Save slot restore** (`LoadSaveSlot`, `QuickSaveToSlot`) — calls
  `LoadStage` (game.c, pulls SDL). The `save_io` suite covers the
  file-I/O half; slot restore needs a per-test fixture of g_script_vars
  / g_entity_state and a working LoadStage.
- **RenderTextLineToBuffer / MeasureTextLine** in `font.c` — write into
  a target buffer using palette state; deferred until refactor extracts
  the layout pass.
- **Speech balloon, dialog stack push/pop** in stubs.c — `g_speech_balloon`
  is stubbed as NULL; SHOW_TEXT and BEGIN_DIALOG / END_DIALOG paths
  dispatch through capture stubs but the stub-side state machine isn't
  exercised (would require porting half of `stubs.c`).
- **Per-entity VM** (`FUN_004012E0` in actor.c) — actor.c is too
  coupled to graphics+stubs to link. Could be split out in a future
  refactor; deferred.

For end-to-end coverage of the above, use:

```
make tools && ./tools/dta-validate.sh      # PKv2 byte-perfect (1782 files)
make debug && ./wacki-debug --headless     # ASAN/UBSan smoke
./tools/smoke-runner.sh                    # headless smoke run
```

## Framework reference

`tests/test.h` (~150 LOC, zero deps):

```c
#include "test.h"

TEST(name_of_test) {
    ASSERT_EQ(2 + 2, 4);
    ASSERT_TRUE(predicate);
    ASSERT_FALSE(predicate);
    ASSERT_STREQ(actual_str, expected_str);
    ASSERT_MEMEQ(actual_buf, expected_buf, size);
    ASSERT_NULL(ptr);
    ASSERT_NOT_NULL(ptr);
    ASSERT_NE(a, b);
}

SUITE(name_of_suite) {
    RUN_TEST(name_of_test);
    RUN_TEST(another_test);
}
```

Assertion failures call `longjmp` back to the runner. A single failing
assertion aborts ONLY that test; the rest of the suite (and all
remaining suites) still execute. Failure messages include file, line,
test name, expression, and actual vs expected values.

## Adding a new suite

1. Write `tests/test_<name>.c` with one or more `TEST(...)` blocks and
   a `SUITE(<name>) { RUN_TEST(...); }` at the bottom.
2. In `tests/test_main.c`:
   - Add `extern void run_suite_<name>(int *, int *);`
   - Add an entry to `kSuites[]`.
3. In `Makefile`:
   - Add `tests/test_<name>.c` to `TEST_SRCS`.
   - If the suite needs a new engine TU, add it to `TEST_ENGINE_SRCS`
     (must NOT include SDL.h).
4. Run `make test`. New PASS lines appear.

## Why custom framework

C unit-test ecosystems are pretty inconsistent:

- **Unity** (ThrowTheSwitch) — solid, but vendor-ed ~2 KLOC and
  conventions that don't match this codebase's minimal-stdlib aesthetic.
- **cmocka** — system dependency (libcmocka); install-skew across hosts.
- **Criterion** — heavier still, requires its own runner / setup.
- **CMake + GoogleTest** — wrong stack (this is a plain Makefile +
  gnu99 codebase, no C++).

`tests/test.h` is 150 lines, zero deps, survives `-Wpedantic` cleanly
under gnu11, and one whole-program TU per test file = predictable build.
For a codebase whose engine has zero external deps besides SDL2, the
tests should too.

## How tests handle engine globals

The test binary links 12 engine TUs:
```
depack.c  archive.c  graphics.c  pe_loader.c  heap.c
platform/sdl/file_host.c  (the stdio fopen_cyg shim)
assets.c  font.c     save.c      binary_data.c  timer.c
script.c  ← linked via tests/sdl_stub/SDL.h (declares only SDL_Delay)
```

Symbols the tests need that normally live in `stubs.c` / `game.c` /
`actor.c` (which pull in deeply coupled state) are mirrored in
`tests/test_engine_stubs.c` — most as no-op stubs, the ScriptCall*
family as **capture stubs** with counters + last-arg storage exposed
via `tests/test_engine_stubs.h`:

- `WackiRand` / `WackiRandSeed` — copy-pasted from `stubs.c`. If the
  production version changes, update both. The golden-vector test in
  `test_rng.c` catches divergence on the next CI run.
- `PlatformPresent` — no-op (graphics.c's `FlushFrameToPrimary` is
  never exercised by tests but the linker needs the symbol).
- `g_script_vars` / `g_entity_state` / `g_scene_snapshot` /
  `g_active_actor` / `g_cur_etap` / `g_cur_komnata` / `g_stats` —
  empty definitions so save.c links. The dormant slot-restore paths
  reference these.
- `LoadStage` — returns 1 (success). Same reason.
- `g_scripts_obj` = NULL — assets.c guards `if (g_scripts_obj)` before
  calling FindAnimationScript, so as long as it's NULL the stub never
  fires. (`FindAnimationScript` is also stubbed for the linker.)
- `g_default_world_state` — all-zero 0x2664-byte array (the world-state
  template normally embedded in binary_data.c).
- ~25 `ScriptCall*` / `FindEntityByVerbId` / `Inventory*` / etc. stubs
  with capture infrastructure. See `tests/test_engine_stubs.h` for the
  `g_stub.<counter>` and `g_stub_last.<field>` API tests use.

### Capture-stub usage pattern (VM dispatch tests)

```c
#include "test_engine_stubs.h"

TEST(my_op_dispatches_to_sound_play) {
    reset_vm();                  /* zeroes g_script_vars + vm_stubs_reset() */
    uint16_t prog[16] = { 0 };
    /* ... emit bytecode ending in op 0x55 ... */
    RunScriptInterpreter(0, 0, (uint8_t *)prog);

    ASSERT_EQ(g_stub.sound_play, 1);
    ASSERT_EQ(g_stub_last.sound_play_id, expected_id);
}
```

Long-term cleanup: extract WackiRand to `src/rng.c` so engine + tests
share one TU. Tracked in `REFACTOR.md` (proposal, not yet shipped).

## CI integration

Not yet wired. Suggested when someone adds GitHub Actions:

```yaml
- run: make tools && ./tools/dta-validate.sh
- run: make test
- run: make debug && ./wacki-debug --headless
```

`make test` exits non-zero on any failure, so it's drop-in.
