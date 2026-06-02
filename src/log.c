/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 */

/* src/log.c — wacki_log + the runtime severity threshold.
 *
 * One line per call; format is `[level/tag] message\n`. Call sites
 * use the LOG_* macros in include/wacki/log.h. */

#include "wacki/log.h"

#include <stdarg.h>
#include <stdio.h>

WackiLogLevel g_log_min_level = WL_INFO;

static const char *const k_level_name[] = {
    "trace",   /* WL_TRACE */
    "debug",   /* WL_DEBUG */
    "info",    /* WL_INFO  */
    "warn",    /* WL_WARN  */
    "error",   /* WL_ERROR */
};

void wacki_log(WackiLogLevel lvl, const char *tag, const char *fmt, ...)
{
    if (lvl < g_log_min_level) return;
    if (lvl < WL_TRACE || lvl > WL_ERROR) lvl = WL_INFO;

    fprintf(stderr, "[%s/%s] ", k_level_name[lvl], tag ? tag : "?");

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
}
