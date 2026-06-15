/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * SetupActivity — first-run asset import (the launcher activity).
 *
 * The game needs the original DANE_*.DTA archives (~440 MB) which we can't and
 * won't redistribute. On first launch this activity asks the user to point at
 * the folder where they've copied those files from their own disc, via the
 * Storage Access Framework (ACTION_OPEN_DOCUMENT_TREE — no storage permission
 * required), then copies the .DTA files into the app's private external files
 * dir. That directory is a real, fopen()-able filesystem path, so the C engine
 * reads it through the unmodified stdio file HAL (see data_root_android.c).
 *
 * Once the data is present (this run or a previous one) it hands straight off
 * to WackiActivity and finishes.
 */
package pl.wacki;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.Button;
import android.widget.ProgressBar;
import android.widget.TextView;

import androidx.documentfile.provider.DocumentFile;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Executors;

public class SetupActivity extends Activity {

    /** The probe archive every original Wacki disc ships — mirrors the C
     *  FindDataRoot probe (DATA_PROBE_FILENAME). Its presence means "ready". */
    private static final String PROBE_NAME = "dane_02.dta";

    private static final int REQ_PICK_FOLDER = 1;
    private static final int COPY_BUF = 4 * 1024 * 1024;   // 4 MiB

    private TextView   status;
    private ProgressBar progress;
    private Button     pickButton;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_setup);

        status     = findViewById(R.id.status);
        progress   = findViewById(R.id.progress);
        pickButton = findViewById(R.id.pick_button);
        pickButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { pickFolder(); }
        });

        // Already imported on a previous run? Go straight to the game.
        if (dataReady()) {
            launchGame();
        }
    }

    /** The app's data directory: <getExternalFilesDir>/data, or the internal
     *  fallback if external storage is unavailable. Mirrors the C probe order
     *  (external first, then internal). */
    private File dataDir() {
        File base = getExternalFilesDir(null);
        if (base == null) base = getFilesDir();
        return new File(base, "data");
    }

    private boolean dataReady() {
        File probe = new File(dataDir(), PROBE_NAME);
        if (probe.exists()) return true;
        // Case-insensitive: a disc copy may carry DANE_02.DTA uppercase.
        File dir = dataDir();
        File[] kids = dir.listFiles();
        if (kids != null) {
            for (File f : kids) {
                if (f.getName().equalsIgnoreCase(PROBE_NAME)) return true;
            }
        }
        return false;
    }

    private void pickFolder() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivityForResult(i, REQ_PICK_FOLDER);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req != REQ_PICK_FOLDER || res != RESULT_OK || data == null) return;
        Uri tree = data.getData();
        if (tree == null) return;
        try {
            getContentResolver().takePersistableUriPermission(
                tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) { /* copy works without persisting */ }
        importFrom(tree);
    }

    private void importFrom(final Uri treeUri) {
        pickButton.setEnabled(false);
        progress.setVisibility(View.VISIBLE);
        progress.setIndeterminate(true);
        setStatus(getString(R.string.setup_scanning));

        final Handler ui = new Handler(Looper.getMainLooper());
        Executors.newSingleThreadExecutor().execute(new Runnable() {
            @Override public void run() {
                String error = null;
                try {
                    copyDtaFiles(treeUri, ui);
                } catch (Exception e) {
                    error = e.getMessage();
                }
                final String err = error;
                ui.post(new Runnable() {
                    @Override public void run() { onImportDone(err); }
                });
            }
        });
    }

    /** Copy every *.DTA file in the picked folder into dataDir(). */
    private void copyDtaFiles(Uri treeUri, final Handler ui) throws Exception {
        DocumentFile tree = DocumentFile.fromTreeUri(this, treeUri);
        if (tree == null || !tree.isDirectory()) {
            throw new Exception(getString(R.string.setup_err_folder));
        }

        List<DocumentFile> dta = new ArrayList<>();
        for (DocumentFile f : tree.listFiles()) {
            String name = f.getName();
            if (f.isFile() && name != null && name.toLowerCase().endsWith(".dta")) {
                dta.add(f);
            }
        }
        if (dta.isEmpty()) {
            throw new Exception(getString(R.string.setup_err_no_dta));
        }

        File dir = dataDir();
        if (!dir.exists() && !dir.mkdirs()) {
            throw new Exception("mkdir " + dir + " failed");
        }

        final int total = dta.size();
        byte[] buf = new byte[COPY_BUF];
        for (int idx = 0; idx < total; idx++) {
            DocumentFile src = dta.get(idx);
            final String name = src.getName();
            final int n = idx + 1;
            ui.post(new Runnable() {
                @Override public void run() {
                    progress.setIndeterminate(false);
                    progress.setMax(total);
                    progress.setProgress(n - 1);
                    setStatus(getString(R.string.setup_copying, n, total, name));
                }
            });

            File out = new File(dir, name);
            File tmp = new File(dir, name + ".part");
            try (InputStream in = getContentResolver().openInputStream(src.getUri());
                 OutputStream os = new FileOutputStream(tmp)) {
                if (in == null) throw new Exception("open " + name + " failed");
                int r;
                while ((r = in.read(buf)) > 0) os.write(buf, 0, r);
            }
            // Atomic-ish: only the fully-written file gets the real name, so an
            // interrupted import never leaves a half-copied archive behind.
            if (out.exists() && !out.delete()) { /* overwrite best-effort */ }
            if (!tmp.renameTo(out)) {
                tmp.delete();
                throw new Exception("rename " + name + " failed");
            }
        }
    }

    private void onImportDone(String error) {
        progress.setVisibility(View.GONE);
        pickButton.setEnabled(true);
        if (error == null && dataReady()) {
            launchGame();
        } else {
            setStatus(error != null
                ? getString(R.string.setup_err_prefix, error)
                : getString(R.string.setup_err_no_dta));
        }
    }

    private void setStatus(String s) { status.setText(s); }

    private void launchGame() {
        startActivity(new Intent(this, WackiActivity.class));
        finish();
    }
}
