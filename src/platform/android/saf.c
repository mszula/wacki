/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/android/saf.c — Android Storage Access Framework read-in-place
 * bridge (see include/wacki/platform/android_saf.h).
 *
 * The data archives live behind a content:// document tree the user picked, not
 * a real path, so they can't be fopen()'d. This calls back into Java —
 * WackiActivity.nativeOpenDataFd(name) → ContentResolver.openFileDescriptor →
 * ParcelFileDescriptor.detachFd — to get a raw fd, then fdopen()'s it. Local
 * SAF files are seekable, so the resulting FILE * supports the engine's
 * fseek/ftell archive access unchanged.
 *
 * JNI note: app classes can't be reached with FindClass from the SDL thread
 * (wrong class loader), so we go through the activity object SDL hands us and
 * resolve the method on its concrete class via GetObjectClass — which needs no
 * class loader. */

#include "wacki/platform/android_saf.h"
#include "wacki/log.h"

#include <SDL.h>      /* SDL_AndroidGetJNIEnv / SDL_AndroidGetActivity */
#include <jni.h>
#include <stdio.h>
#include <unistd.h>   /* close */

static int s_saf_active = 0;

int  android_saf_active(void)       { return s_saf_active; }
void android_saf_set_active(int on) { s_saf_active = on ? 1 : 0; }

/* Call one of WackiActivity's helper methods on the SDL-provided activity.
 * `sig`/`is_int` pick between int nativeOpenDataFd(String) and
 * boolean nativeHasData(). Returns the int result (fd, or 0/1 for the bool),
 * or -1 on any JNI failure. */
static int call_activity(const char *method, const char *sig,
                         const char *str_arg, int is_int)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = (jobject)SDL_AndroidGetActivity();
    if (!env || !act) {
        if (act) (*env)->DeleteLocalRef(env, act);
        return -1;
    }

    int result = -1;
    jclass    cls = (*env)->GetObjectClass(env, act);
    jmethodID mid = (*env)->GetMethodID(env, cls, method, sig);
    if (mid) {
        if (is_int) {
            jstring js = str_arg ? (*env)->NewStringUTF(env, str_arg) : NULL;
            result = (int)(*env)->CallIntMethod(env, act, mid, js);
            if (js) (*env)->DeleteLocalRef(env, js);
        } else {
            result = (*env)->CallBooleanMethod(env, act, mid) ? 1 : 0;
        }
    }
    if ((*env)->ExceptionCheck(env)) {   /* don't leave a pending exception */
        (*env)->ExceptionClear(env);
        result = -1;
    }
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
    return result;
}

int android_saf_has_data(void)
{
    return call_activity("nativeHasData", "()Z", NULL, 0) == 1;
}

FILE *android_saf_fopen(const char *path)
{
    if (!path) return NULL;

    /* The engine passes "<root>/NAME"; the Java side keys on basename, but send
     * the basename anyway so the lookup is unambiguous. */
    const char *base = path;
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;

    int fd = call_activity("nativeOpenDataFd", "(Ljava/lang/String;)I", base, 1);
    if (fd < 0) return NULL;

    FILE *fp = fdopen(fd, "rb");
    if (!fp) {
        close(fd);
        LOG_INFO("saf", "fdopen failed for %s (fd=%d)", base, fd);
        return NULL;
    }
    return fp;
}
