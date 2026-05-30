/* src/scene/actor_walk.c — blocking actor walk dispatch.
 *
 * Script opcodes 0x10 (walk Ebek), 0x11 (walk Fjej), and 0x12 (walk
 * both) drive the actor to a target position synchronously — the
 * script blocks the VM tick on the walker until the actor reaches
 * the target or the walk is interrupted by a click.
 *
 * The work itself is implemented via BindActorWalker (actor/walker.c)
 * which sets up Bresenham state, plants the walk anim, and binds the
 * per-entity VM. This module just provides the blocking wait loop
 * that pumps frames until the walker drains.
 */

#include "wacki.h"
#include <SDL.h>

#include <stdint.h>
#include <stdio.h>

extern int  BindActorWalker(int actor_idx, int target_x, int target_y);
extern void ProcessGameFrameTick(void);

void ActorWalkToBlocking(int idx, int16_t tx, int16_t ty)
{
    extern Entity *g_actor[2];
    if (idx < 0 || idx > 1 || !g_actor[idx]) return;
    Entity *e = g_actor[idx];
    uint8_t *eb = (uint8_t *)e;

    /* Already at target — original short-circuits via the outer if. */
    if ((int)*(int16_t *)(eb + 0x22) == (int)tx &&
        (int)*(int16_t *)(eb + 0x24) == (int)ty) {
        return;
    }

    /* Bind walker bytecode + plant path (BindActorWalker does the full
 * 1:1 with UpdateActorMovement walker-bind tail). */
    if (!BindActorWalker(idx, (int)tx, (int)ty)) {
        return;
    }

    /* Pump frames until walker drains OR user clicks (interrupt).
 * case 0x10/0x11 wait loop:
 * do { PGFT; if (walker drained) goto LAB; } while (g_lmb_clicked == 0);
 * g_lmb_handled = 1;
 * LAB: g_lmb_clicked = 0; var[4] = g_lmb_handled; g_lmb_handled = 0;
 *
 * So loop exits early on user click (g_lmb_clicked nonzero AFTER
 * a PGFT iteration). var[4] = 1 if interrupted by click, 0 if
 * walker drained. */
    int interrupted = 0;
    int safety = 1024;
    while (safety-- > 0) {
        uint32_t wdx = *(uint32_t *)(eb + 0x4C);
        uint32_t wdy = *(uint32_t *)(eb + 0x50);
        if (wdx == 0 && wdy == 0) break;                       /* walker drained */
        ProcessGameFrameTick();
        if (PlatformShouldQuit() || g_game_over_code) break;
        if (g_lmb_clicked) { interrupted = 1; break; }         /* 1:1 early exit */
        SDL_Delay(33);  /* T-anim-speed: match main loop pacing */
    }
    /* : consume the click, set var[4] from interrupt flag,
 * clear g_lmb_handled (= DAT_0044e5a4). The verb-script's nested IF
 * (var[4]==1) reads var[4] to decide whether to abort (e.g. verb-7
 * @ 0x00427B90 offset 40 → END_FORCE if interrupted; else fall
 * through to op 0x20 GO_EXIT). g_lmb_handled clear is important
 * because some downstream code paths read it as a click-pending
 * latch — leaving it set across script boundary can confuse
 * subsequent click dispatch. */
    g_lmb_clicked = 0;
    g_lmb_handled = 0;
    g_script_vars[4] = (uint32_t)interrupted;
}

/* ActorWalkBothBlocking — 1:1 port of original case 0x12 walk-both
 * dispatch (Ghidra @ RunScriptInterpreter case 0x12, line ~582):
 *
 * setup actor 0 walker → target = (tx, ty)
 * wait 0x32 ticks (≈ 50ms via DAT_0044E578 decrement loop)
 * setup actor 1 walker → target = (tx, ty)
 * if (mode == 0) wait for BOTH to arrive
 * else wait for actor 0 only
 *
 * The 50-tick stagger is the visible "Wacki + Fjej walk together" effect
 * (Fjej starts a few frames after Wacki). With the port's
 * play_demo_scene walker NOT integrated into ProcessGameFrameTick yet,
 * we approximate by interleaving the two ActorWalkToBlocking-style
 * step loops in a single function. */
void ActorWalkBothBlocking(int16_t tx, int16_t ty, int mode)
{
    extern Entity *g_actor[2];
    extern uint32_t g_frame_delta_ms;
    if (!g_actor[0] && !g_actor[1]) return;

    /* Bind walker bytecode for both actors via the standard path (same
 * BindActorWalker call the click handler uses). Plants the path
 * immediately so per-entity VM op 0x15/0x16 just steps each frame.
 *
 * The original does setup actor 0 → wait 0x32 ms → setup actor 1
 * (= visible "Wacki + Fjej walk together" stagger). We bind actor 0
 * up front + bind actor 1 only after the stagger drains. */
    int bound[2] = { 0, 0 };
    if (g_actor[0]) bound[0] = BindActorWalker(0, (int)tx, (int)ty);

    /* case 0x12 wait loop: exit EARLY on
 * g_lmb_clicked (interrupt) or walker drain. var[4]=1 if click,
 * 0 if drained. Loop must NOT clear g_lmb_clicked mid-loop
 * (original keeps it set until LAB epilogue consumes it). */
    int interrupted = 0;

    /* Phase 1: stagger — actor 0 walks for 0x32=50 TICKS (= ~500 ms) while
 * actor 1 idle. The original op 0x12 head-start loop
 * subtracts DAT_0044E578 (ticks) from a constant 0x32. Reading it as
 * 50 ms (g_frame_delta_ms) drained the stagger in ~3 frames at 30 fps
 * — actor 1 started moving almost immediately, killing the "walk
 * together" visual cue. */
    extern uint16_t g_frame_delta_ticks;
    int stagger_left = 0x32;
    while (stagger_left > 0) {
        ProcessGameFrameTick();
        if (PlatformShouldQuit() || g_game_over_code) goto done;
        if (g_lmb_clicked) { interrupted = 1; goto done; }
        int dt = g_frame_delta_ticks ? (int)g_frame_delta_ticks : 3;
        stagger_left -= dt;
        SDL_Delay(33);  /* T-anim-speed: match main loop pacing */
    }

    if (g_actor[1]) bound[1] = BindActorWalker(1, (int)tx, (int)ty);

    /* Phase 2: wait until walker(s) drain. mode==0 → both must arrive;
 * mode==1 → only actor 0. */
    int safety = 2048;
    while (safety-- > 0) {
        int still = 0;
        for (int i = 0; i < 2; ++i) {
            if (!bound[i] || !g_actor[i]) continue;
            if (mode != 0 && i != 0) continue;
            uint8_t *eb = (uint8_t *)g_actor[i];
            uint32_t wdx = *(uint32_t *)(eb + 0x4C);
            uint32_t wdy = *(uint32_t *)(eb + 0x50);
            if (wdx != 0 || wdy != 0) { still = 1; break; }
        }
        if (!still) break;                                /* walker drained */
        ProcessGameFrameTick();
        if (PlatformShouldQuit() || g_game_over_code) break;
        if (g_lmb_clicked) { interrupted = 1; break; }    /* 1:1 early exit */
        SDL_Delay(33);  /* T-anim-speed: match main loop pacing */
    }
done:
    /* epilogue: consume click + write var[4] + clear
 * g_lmb_handled. See ActorWalkToBlocking's matching epilogue. */
    g_lmb_clicked = 0;
    g_lmb_handled = 0;
    g_script_vars[4] = (uint32_t)interrupted;
}
