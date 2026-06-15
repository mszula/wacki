/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * WackiData — Storage Access Framework data access (read-in-place, no copy).
 *
 * The user picks the folder holding their original DANE_*.DTA via SAF
 * (SetupActivity); we persist that content:// tree grant and, from then on,
 * the native engine reads the archives straight from there. The native side
 * calls WackiActivity.nativeOpenDataFd(name) → openFd() here, which maps a
 * basename to the tree's document and hands back a file descriptor
 * (ContentResolver.openFileDescriptor → ParcelFileDescriptor.detachFd). The C
 * bridge fdopen()'s it; local SAF files are seekable, so the engine's archive
 * fseek/ftell work unchanged. No 440 MB copy into app storage.
 *
 * All entry points lazy-load the persisted tree + a case-insensitive
 * name→Uri index, so they're safe to call from the SDL thread before any
 * explicit init.
 */
package pl.mszula.wacki;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import androidx.documentfile.provider.DocumentFile;

import java.util.HashMap;
import java.util.Map;

public final class WackiData {

    private static final String PREFS = "wacki_prefs";
    private static final String KEY_TREE = "data_tree_uri";
    /** Probe archive — mirrors the C FindDataRoot probe (DATA_PROBE_FILENAME),
     *  lowercased for the case-insensitive index. */
    private static final String PROBE = "dane_02.dta";

    private static Uri treeUri;
    private static Map<String, Uri> index;   // lowercased basename -> document Uri

    private WackiData() {}

    /** Persist the picked tree and rebuild the index. Called by SetupActivity
     *  after the SAF folder pick. */
    public static synchronized void setTree(Context ctx, Uri uri) {
        ctx.getApplicationContext()
           .getSharedPreferences(PREFS, Context.MODE_PRIVATE)
           .edit().putString(KEY_TREE, uri.toString()).apply();
        treeUri = uri;
        index = null;
        buildIndex(ctx);
    }

    /** True when a tree is configured and actually contains the data. */
    public static synchronized boolean isConfigured(Context ctx) {
        ensureLoaded(ctx);
        return index != null && index.containsKey(PROBE);
    }

    /** Open a read-only fd for the data file named by `name` (basename match,
     *  case-insensitive). Returns a raw fd owned by the caller (the native
     *  side fdopen()'s + fclose()'s it), or -1 if absent / the grant is gone.
     *  Invoked from native via WackiActivity.nativeOpenDataFd. */
    public static synchronized int openFd(Context ctx, String name) {
        ensureLoaded(ctx);
        if (index == null || name == null) return -1;

        int slash = Math.max(name.lastIndexOf('/'), name.lastIndexOf('\\'));
        String base = (slash >= 0 ? name.substring(slash + 1) : name).toLowerCase();

        Uri u = index.get(base);
        if (u == null) return -1;
        try {
            ParcelFileDescriptor pfd =
                ctx.getContentResolver().openFileDescriptor(u, "r");
            return pfd != null ? pfd.detachFd() : -1;
        } catch (Exception e) {
            return -1;
        }
    }

    // ---- internals ----

    private static void ensureLoaded(Context ctx) {
        if (treeUri == null) {
            String s = ctx.getApplicationContext()
                          .getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                          .getString(KEY_TREE, null);
            if (s != null) treeUri = Uri.parse(s);
        }
        if (treeUri != null && index == null) buildIndex(ctx);
    }

    private static void buildIndex(Context ctx) {
        index = new HashMap<>();
        try {
            DocumentFile dir =
                DocumentFile.fromTreeUri(ctx.getApplicationContext(), treeUri);
            if (dir != null && dir.isDirectory()) {
                for (DocumentFile f : dir.listFiles()) {
                    String n = f.getName();
                    if (f.isFile() && n != null) {
                        index.put(n.toLowerCase(), f.getUri());
                    }
                }
            }
        } catch (Exception e) {
            // Grant revoked / folder moved → empty index → isConfigured()==false,
            // so the launcher re-prompts for the folder.
        }
    }
}
