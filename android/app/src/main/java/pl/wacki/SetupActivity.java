/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * SetupActivity — first-run data-folder setup (the launcher activity).
 *
 * The game needs the original DANE_*.DTA archives (~440 MB) which we can't and
 * won't redistribute. On first launch this asks the user to point at the folder
 * holding those files via the Storage Access Framework (ACTION_OPEN_DOCUMENT_
 * TREE — no storage permission needed), then PERSISTS that grant. The engine
 * reads the archives straight from there through a content:// fd (read in
 * place, no copy — see WackiData / src/platform/android/saf.c).
 *
 * Once a usable folder is configured (this run or a previous one) it hands off
 * to WackiActivity and finishes. Files dropped directly into the app's external
 * files dir (adb push / file manager) are also honoured as a fallback.
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

import java.io.File;
import java.util.concurrent.Executors;

public class SetupActivity extends Activity {

    private static final int REQ_PICK_FOLDER = 1;
    /** Probe archive (matches the C FindDataRoot probe), for the app-dir fallback. */
    private static final String PROBE_NAME = "dane_02.dta";

    private TextView    status;
    private ProgressBar progress;
    private Button      pickButton;

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

        if (dataReady()) launchGame();
    }

    /** Data usable already? SAF folder configured, or archives sitting in the
     *  app's own external files dir (adb push / file manager fallback). */
    private boolean dataReady() {
        if (WackiData.isConfigured(this)) return true;
        File base = getExternalFilesDir(null);
        if (base != null) {
            File dir = new File(base, "data");
            if (new File(dir, PROBE_NAME).exists()) return true;
            File[] kids = dir.listFiles();
            if (kids != null) {
                for (File f : kids) {
                    if (f.getName().equalsIgnoreCase(PROBE_NAME)) return true;
                }
            }
        }
        return false;
    }

    private void pickFolder() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                 | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(i, REQ_PICK_FOLDER);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req != REQ_PICK_FOLDER || res != RESULT_OK || data == null) return;
        final Uri tree = data.getData();
        if (tree == null) return;

        // Persist the grant so the engine can reopen the folder on later launches.
        try {
            getContentResolver().takePersistableUriPermission(
                tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) { /* still usable this session */ }

        configure(tree);
    }

    /** Index the picked folder (SAF listFiles can be slow → off the UI thread)
     *  and, if it holds the archives, start the game. */
    private void configure(final Uri tree) {
        pickButton.setEnabled(false);
        progress.setVisibility(View.VISIBLE);
        progress.setIndeterminate(true);
        setStatus(getString(R.string.setup_scanning));

        final Handler ui = new Handler(Looper.getMainLooper());
        Executors.newSingleThreadExecutor().execute(new Runnable() {
            @Override public void run() {
                WackiData.setTree(SetupActivity.this, tree);
                final boolean ok = WackiData.isConfigured(SetupActivity.this);
                ui.post(new Runnable() {
                    @Override public void run() {
                        progress.setVisibility(View.GONE);
                        pickButton.setEnabled(true);
                        if (ok) launchGame();
                        else    setStatus(getString(R.string.setup_err_no_dta));
                    }
                });
            }
        });
    }

    private void setStatus(String s) { status.setText(s); }

    private void launchGame() {
        startActivity(new Intent(this, WackiActivity.class));
        finish();
    }
}
