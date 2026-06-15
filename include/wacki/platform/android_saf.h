/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * include/wacki/platform/android_saf.h — Android Storage Access Framework
 * read-in-place bridge.
 *
 * Only meaningful on Android. Lets the engine read the user's data archives
 * straight from the folder they picked (a content:// document tree) instead of
 * copying them: a basename is mapped to a file descriptor via JNI
 * (ContentResolver.openFileDescriptor → ParcelFileDescriptor.detachFd) and
 * fdopen()'d into a normal stdio stream — so the rest of the engine's stdio
 * file + FLIC HAL works unchanged on a seekable local-file fd.
 *
 * Implementation: src/platform/android/saf.c. The shared SDL file/FLIC HAL
 * (file_host.c / flic_host.c) call these under #ifdef __ANDROID__; the data
 * root resolver (data_root_android.c) turns the mode on once it commits the
 * SAF tree as the data root. */
#ifndef WACKI_PLATFORM_ANDROID_SAF_H
#define WACKI_PLATFORM_ANDROID_SAF_H

#include <stdio.h>

/* Non-zero once the data root has resolved to the SAF tree, i.e. reads should
 * go through the content:// bridge rather than a real path. */
int  android_saf_active(void);
void android_saf_set_active(int on);

/* Whether a SAF tree is configured AND holds the data (probe archive present).
 * Queried through JNI (WackiActivity.nativeHasData). 0 when no folder was
 * picked, the picked folder lacks the archives, or its grant was lost. */
int  android_saf_has_data(void);

/* Open the data file named by `path`'s basename through SAF; returns a
 * read-only stdio stream (fdopen of the detached SAF fd) or NULL if the file
 * isn't in the tree / the grant is gone. The basename match is
 * case-insensitive (discs ship DANE_NN.DTA upper-case). */
FILE *android_saf_fopen(const char *path);

#endif /* WACKI_PLATFORM_ANDROID_SAF_H */
