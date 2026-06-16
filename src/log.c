/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/log.c — wacki_log + the runtime severity threshold.
 *
 * One line per call; format is `[level/tag] message\n`. Call sites
 * use the LOG_* macros in include/wacki/log.h. */

#include "wacki/log.h"

#include <stdarg.h>
#include <stdio.h>

#ifdef __ANDROID__
#include <android/log.h>   /* stderr is dropped on Android — route to logcat */
#endif

WackiLogLevel g_log_min_level = WL_INFO;

#ifndef __ANDROID__
static const char *const k_level_name[] = {
    "trace",   /* WL_TRACE */
    "debug",   /* WL_DEBUG */
    "info",    /* WL_INFO  */
    "warn",    /* WL_WARN  */
    "error",   /* WL_ERROR */
};
#endif

void wacki_log(WackiLogLevel lvl, const char *tag, const char *fmt, ...)
{
    if (lvl < g_log_min_level) return;
    if (lvl < WL_TRACE || lvl > WL_ERROR) lvl = WL_INFO;

#ifdef __ANDROID__
    /* Android drops native stderr; go through the logcat API instead so the
     * port's logs (and bug reports) are visible via `adb logcat -s wacki`. */
    static const int k_android_prio[] = {
        ANDROID_LOG_VERBOSE, ANDROID_LOG_DEBUG, ANDROID_LOG_INFO,
        ANDROID_LOG_WARN,    ANDROID_LOG_ERROR,
    };
    char    msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    __android_log_print(k_android_prio[lvl], "wacki", "[%s] %s",
                        tag ? tag : "?", msg);
#else
    fprintf(stderr, "[%s/%s] ", k_level_name[lvl], tag ? tag : "?");

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
#endif
}
