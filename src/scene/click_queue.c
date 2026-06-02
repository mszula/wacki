/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/scene/click_queue.c — deferred click-event queue.
 *
 * Per-entity VM opcode 0x22 enqueues (obj, verb) pairs here instead of
 * dispatching them synchronously. The queue is drained at the tail of
 * ProcessGameFrameTick, after every per-entity tick has completed.
 *
 * Why deferred: scripts may add or remove entities in response to a
 * click event, and the per-entity VM iterates the entity list. A
 * synchronous dispatch would mutate that list mid-iteration and
 * corrupt the walker.
 *
 * Queue size is capped at 10; further enqueues during a single frame
 * are silently dropped. Ten is well above any real frame's load —
 * scripts emit clicks one-at-a-time and the queue is fully drained
 * each frame.
 */

#include "wacki.h"

#include <stdint.h>

extern void DispatchClickEvent(uint16_t obj_id, uint16_t verb_id);

#define CLICK_QUEUE_MAX 10

typedef struct ClickQueueEntry {
    uint16_t obj;
    uint16_t verb;
} ClickQueueEntry;

static ClickQueueEntry s_click_queue[CLICK_QUEUE_MAX];
static int             s_click_queue_count = 0;

void EnqueueClickEvent(uint16_t obj, uint16_t verb)
{
    if (s_click_queue_count >= CLICK_QUEUE_MAX) return;
    s_click_queue[s_click_queue_count].obj  = obj;
    s_click_queue[s_click_queue_count].verb = verb;
    ++s_click_queue_count;
}

void FlushQueuedClicks(void)
{
    int n = s_click_queue_count;
    s_click_queue_count = 0;
    for (int i = 0; i < n; ++i) {
        DispatchClickEvent(s_click_queue[i].obj, s_click_queue[i].verb);
    }
}
