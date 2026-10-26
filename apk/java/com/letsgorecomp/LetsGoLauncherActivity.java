package com.letsgorecomp;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.ArrayAdapter;
import android.widget.TextView;

import java.io.File;
import java.io.FileWriter;

// Main launcher: status, Play, and the log viewer/exporter.
// The SDL game runs in LetsGoActivity; if it crashes, this launcher survives
// (separate activity) and can still read the crashed run's logcat.
public class LetsGoLauncherActivity extends Activity {
    private static final int REQ_GAME_DIR = 1001;
    private static final int REQ_IMPORT_DIR = 1002;
    private static final int REQ_IMPORT_DIR_FOLDER = 1003;
    private static final int REQ_IMPORT_DRIVER = 1004;
    private static final String PREFS = "letsgo";
    private static final String KEY_GAME_DIR = "game_dir";
    private HomeScreen home;
    private android.os.Handler importPoller;
    private Runnable importPollTask;

    /** The user-picked game folder, or null if not set. */
    public static String gameDir(android.content.Context ctx) {
        return ctx.getSharedPreferences(PREFS, MODE_PRIVATE)
                  .getString(KEY_GAME_DIR, null);
    }

    /** True only when a COMPLETE import exists: the .import_ok marker (written
     *  by LetsGoImportJob on 100%) or a generated romfs.bin. A bare romfs/
     *  folder is NOT enough — an interrupted import also leaves a folder behind. */
    public static boolean gameDataReady(android.content.Context ctx) {
        File gameLib = new File(ctx.getApplicationInfo().nativeLibraryDir, "liblasgame.so");
        String dir = gameDir(ctx);
        File base = dir != null ? new File(dir) : ctx.getFilesDir();
        File dataBin = new File(base, "extracted/romfs.bin");
        File importOk = new File(base, "extracted/.import_ok");
        File exefsMain = new File(base, "extracted/exefs/main");
        return gameLib.exists() && exefsMain.exists()
                && (dataBin.exists() || importOk.exists());
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        Txt.init(this);

        // Request full storage access (needed for direct fopen() of game files)
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            if (!android.os.Environment.isExternalStorageManager()) {
                try {
                    Intent si = new Intent(
                        android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        android.net.Uri.parse("package:" + getPackageName()));
                    startActivity(si);
                } catch (Exception ignored) {}
            }
        }

        // The home screen (mock-up design/maquette-accueil.html): header, step 1 (your game),
        // step 2 (play). The game settings and the driver panel live below it.
        home = new HomeScreen(this, new HomeScreen.Callbacks() {
            @Override public void onImportNsp() { pickFolder(REQ_IMPORT_DIR); }
            @Override public void onUseExtractedFolder() { pickFolder(REQ_IMPORT_DIR_FOLDER); }
            @Override public void onChangeFolder() { pickFolder(REQ_GAME_DIR); }
            @Override public void onPlay() { play(); }
        });
        View homeView = home.build();

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setBackgroundColor(Color.rgb(20, 22, 28));
        int pad = dp(24);
        root.setPadding(pad, pad, pad, pad);

        root.addView(homeView, lp(-1, -2));

        // The old title/status/play buttons are replaced by the home screen above; what
        // follows are the game settings and the driver panel (they move to their own
        // panels in a later PR).

        Button log = new Button(this);
        log.setText(Txt.t("ver_log", "Ver log"));
        log.setOnClickListener(new android.view.View.OnClickListener() {
            @Override public void onClick(android.view.View v) {
                startActivity(new Intent(LetsGoLauncherActivity.this,
                                         LogActivity.class));
            }
        });
        LinearLayout.LayoutParams llp = lp(-1, -2);
        llp.topMargin = dp(12);
        root.addView(log, llp);

        // ---- GPU driver (Turnip via adrenotools, like the Wonder port) ----
        android.widget.TextView drvTitle = new android.widget.TextView(this);
        drvTitle.setText(Txt.t("gpu", "Driver GPU (Turnip)"));
        drvTitle.setTextSize(16);
        drvTitle.setTextColor(Color.rgb(255, 255, 255));
        drvTitle.setPadding(0, dp(16), 0, dp(4));
        root.addView(drvTitle, lp(-1, -2));

        final android.widget.TextView drvStatus = new android.widget.TextView(this);
        drvStatus.setTextSize(13);
        drvStatus.setTextColor(Color.rgb(180, 190, 200));
        root.addView(drvStatus, lp(-1, -2));

        final Runnable refreshDrivers = new Runnable() {
            @Override public void run() {
                GpuDrivers.Driver sel = GpuDrivers.selected(LetsGoLauncherActivity.this);
                StringBuilder sb = new StringBuilder();
                if (sel != null) {
                    sb.append(Txt.t("in_use_driver", "Em uso: "))
                      .append(sel.name).append(" (").append(sel.driverVersion).append(")");
                } else {
                    sb.append(Txt.t("in_use", "Em uso: driver do sistema"));
                }
                java.util.List<GpuDrivers.Driver> all = GpuDrivers.list(LetsGoLauncherActivity.this);
                if (all.isEmpty()) {
                    sb.append(Txt.t("drivers_none", "Nenhum driver Turnip instalado."));
                } else {
                    sb.append(Txt.t("installed", "Instalados: "));
                    for (int i = 0; i < all.size(); i++) {
                        if (i > 0) sb.append(", ");
                        sb.append(all.get(i).name);
                    }
                }
                drvStatus.setText(sb.toString());
            }
        };
        refreshDrivers.run();

        // Install the bundled Turnip on first run (like Wonder).
        GpuDrivers.installBundled(this);
        refreshDrivers.run();

        Button importDrv = new Button(this);
        importDrv.setText(Txt.t("import_driver", "Importar driver Turnip (.zip)"));
        importDrv.setOnClickListener(new android.view.View.OnClickListener() {
            @Override public void onClick(android.view.View v) {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType("application/zip");
                startActivityForResult(i, REQ_IMPORT_DRIVER);
            }
        });
        LinearLayout.LayoutParams idlp = lp(-1, -2);
        idlp.topMargin = dp(8);
        root.addView(importDrv, idlp);

        Button useSystem = new Button(this);
        useSystem.setText(Txt.t("use_system", "Usar driver do sistema"));
        useSystem.setOnClickListener(new android.view.View.OnClickListener() {
            @Override public void onClick(android.view.View v) {
                GpuDrivers.select(LetsGoLauncherActivity.this, null);
                refreshDrivers.run();
                android.widget.Toast.makeText(LetsGoLauncherActivity.this,
                    Txt.t("driver_sys", "Driver do sistema selecionado."), android.widget.Toast.LENGTH_SHORT).show();
            }
        });
        root.addView(useSystem, lp(-1, -2));

        // Long-press a driver name in the status to select it (simple UI).
        drvStatus.setOnClickListener(new android.view.View.OnClickListener() {
            @Override public void onClick(android.view.View v) {
                java.util.List<GpuDrivers.Driver> all = GpuDrivers.list(LetsGoLauncherActivity.this);
                if (all.isEmpty()) return;
                // Cycle to the next driver (simple single-tap selection).
                GpuDrivers.Driver sel = GpuDrivers.selected(LetsGoLauncherActivity.this);
                int idx = -1;
                for (int i = 0; i < all.size(); i++) {
                    if (sel != null && all.get(i).slug.equals(sel.slug)) { idx = i; break; }
                }
                GpuDrivers.Driver next = all.get((idx + 1) % all.size());
                GpuDrivers.select(LetsGoLauncherActivity.this, next);
                refreshDrivers.run();
                android.widget.Toast.makeText(LetsGoLauncherActivity.this,
                    Txt.t("driver_set", "Driver selecionado: ") + next.name, android.widget.Toast.LENGTH_SHORT).show();
            }
        });

        // ---- Game settings (written to larecomp.ini in the game folder) ----
        TextView cfgTitle = new TextView(this);
        cfgTitle.setText(Txt.t("cfg", "Configurações"));
        cfgTitle.setTextSize(18);
        cfgTitle.setTextColor(Color.WHITE);
        cfgTitle.setPadding(0, dp(20), 0, dp(8));
        root.addView(cfgTitle, lp(-1, -2));

        final CheckBox fps60 = new CheckBox(this);
        fps60.setText(Txt.t("fps60", "60 fps (experimental)"));
        fps60.setTextColor(Color.rgb(200, 210, 220));
        root.addView(fps60, lp(-1, -2));

        final CheckBox smaa = new CheckBox(this);
        smaa.setText(Txt.t("smaa", "Suavizar serrilhado (SMAA)"));
        smaa.setTextColor(Color.rgb(200, 210, 220));
        smaa.setChecked(true);
        root.addView(smaa, lp(-1, -2));

        // Resolution scale selector (emulator-style)
        TextView resLabel = new TextView(this);
        resLabel.setText(Txt.t("res", "Resolução interna:"));
        resLabel.setTextColor(Color.rgb(200, 210, 220));
        root.addView(resLabel, lp(-1, -2));

        final android.widget.Spinner resSpinner = new android.widget.Spinner(this);
        final String[] resNames = {Txt.t("res_05", "0.5x (480p) - Performance"), Txt.t("res_075", "0.75x (540p) - Equilibrado"), Txt.t("res_1x", "1x (720p) - Padrão"), Txt.t("res_2x", "2x (1080p) - Qualidade")};
        final String[] resValues = {"0.4444", "0.5", "0.6667", "1.0"};
        android.widget.ArrayAdapter<String> resAdapter = new android.widget.ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item, resNames);
        resAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        resSpinner.setAdapter(resAdapter);
        // Load saved preference (default 1x = 720p)
        String savedRes = getSharedPreferences("letsgo", MODE_PRIVATE).getString("resolution_scale", "0.6667");
        for (int i = 0; i < resValues.length; i++) {
            if (resValues[i].equals(savedRes)) {
                resSpinner.setSelection(i);
                break;
            }
        }
        resSpinner.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> parent, android.view.View view, int position, long id) {
                getSharedPreferences("letsgo", MODE_PRIVATE).edit()
                        .putString("resolution_scale", resValues[position]).apply();
            }
            @Override public void onNothingSelected(android.widget.AdapterView<?> parent) {}
        });
        root.addView(resSpinner, lp(-1, -2));

        final CheckBox wild = new CheckBox(this);
        wild.setText(Txt.t("wild", "Batalhas selvagens (experimental)"));
        wild.setTextColor(Color.rgb(200, 210, 220));
        root.addView(wild, lp(-1, -2));

        final CheckBox touch = new CheckBox(this);
        touch.setText(Txt.t("touch", "Controles touch na tela"));
        touch.setTextColor(Color.rgb(200, 210, 220));
        touch.setChecked(getSharedPreferences("letsgo", MODE_PRIVATE).getBoolean("touch", true));
        root.addView(touch, lp(-1, -2));
        touch.setOnCheckedChangeListener(new android.widget.CompoundButton.OnCheckedChangeListener() {
            @Override public void onCheckedChanged(android.widget.CompoundButton b, boolean c) {
                getSharedPreferences("letsgo", MODE_PRIVATE).edit().putBoolean("touch", c).apply();
            }
        });

        // load saved settings
        String[] ini = readIni();
        if (ini != null) {
            fps60.setChecked("60".equals(ini[0]));
            smaa.setChecked("1".equals(ini[1]));
            wild.setChecked("1".equals(ini[2]));
        }

        android.view.View.OnClickListener saveIni = new android.view.View.OnClickListener() {
            @Override public void onClick(android.view.View v) {
                writeIni(fps60.isChecked() ? "60" : "30",
                         smaa.isChecked() ? "1" : "0",
                         wild.isChecked() ? "1" : "0");
            }
        };
        fps60.setOnClickListener(saveIni);
        smaa.setOnClickListener(saveIni);
        wild.setOnClickListener(saveIni);

        TextView hint = new TextView(this);
        hint.setText(Txt.t("hint_log", "Se o jogo fechar ao abrir, use \"Ver log\" e me envie o conteúdo."));
        hint.setTextSize(13);
        hint.setTextColor(Color.rgb(140, 150, 160));
        hint.setGravity(Gravity.CENTER);
        hint.setPadding(0, dp(24), 0, 0);
        root.addView(hint, lp(-1, -2));

        android.widget.ScrollView scroll = new android.widget.ScrollView(this);
        scroll.setFillViewport(true);
        scroll.addView(root);
        setContentView(scroll);
        // The home screen starts in its default state; refreshState() decides what to show.
        refreshState();
    }

    /** Launch the game — never into a native crash (without complete game data the runtime FATALs). */
    private void play() {
        LetsGoImportJob ij = LetsGoImportJob.current();
        if (ij != null && !ij.finished) {
            android.widget.Toast.makeText(this,
                Txt.t("wait_import", "Aguarde a importação terminar antes de jogar."),
                android.widget.Toast.LENGTH_LONG).show();
            return;
        }
        if (!gameDataReady(this)) {
            android.widget.Toast.makeText(this,
                "O jogo ainda não foi importado por completo.",
                android.widget.Toast.LENGTH_LONG).show();
            return;
        }
        startActivity(new Intent(this, LetsGoActivity.class));
    }

    /** Open the folder picker (SAF). */
    private void pickFolder(int req) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        startActivityForResult(i, req);
    }

    /** Show the state of the installation on the home screen. */
    private void refreshState() {
        if (home == null) return;
        LetsGoImportJob job = LetsGoImportJob.current();
        boolean importing = job != null && !job.finished;
        String error = (!importing && job != null) ? job.error : null;
        home.showState(importing, error, gameDataReady(this));
        if (importing) {
            int pct = job.total > 0 ? (int) (100.0 * job.done / job.total) : 0;
            home.showProgress(pct, job.phase, job.currentName);
        }
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req == REQ_GAME_DIR && res == RESULT_OK && data != null
                && data.getData() != null) {
            android.net.Uri uri = data.getData();
            // keep access across restarts
            try {
                getContentResolver().takePersistableUriPermission(uri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (Exception ignored) {}
            String path = resolvePath(uri);
            if (path != null) {
                getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putString(KEY_GAME_DIR, path).apply();
            }
            refreshState();
        } else if ((req == REQ_IMPORT_DIR || req == REQ_IMPORT_DIR_FOLDER) && res == RESULT_OK
                && data != null && data.getData() != null) {
            android.widget.Toast.makeText(this, Txt.t("folder_ok", "Pasta escolhida, iniciando..."),
                    android.widget.Toast.LENGTH_SHORT).show();
            startImport(data.getData(), req == REQ_IMPORT_DIR_FOLDER);
        } else if (req == REQ_IMPORT_DRIVER && res == RESULT_OK
                && data != null && data.getData() != null) {
            GpuDrivers.ImportResult r = GpuDrivers.importZip(this, data.getData());
            if (r.error != null) {
                android.widget.Toast.makeText(this, "Erro: " + r.error,
                        android.widget.Toast.LENGTH_LONG).show();
            } else {
                GpuDrivers.select(this, r.driver);
                android.widget.Toast.makeText(this, Txt.t("driver_new", "Driver instalado: ") + r.driver.name,
                        android.widget.Toast.LENGTH_SHORT).show();
            }
            // Refresh the driver status (recreate is simplest).
            recreate();
        }
    }

    /** Kick off the import job (folder = true: loose folder import, no romfs.bin). */
    private void startImport(android.net.Uri uri, boolean folder) {
        try {
            getContentResolver().takePersistableUriPermission(uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        } catch (Exception ignored) {}
        // Import into the game dir (or the app's private files if none picked yet).
        String dir = gameDir(this);
        File outDir = dir != null ? new File(dir) : getFilesDir();
        LetsGoImportJob job = LetsGoImportJob.start(this, uri, outDir, folder);
        if (job != null) startImportPolling();
        refreshState();
    }

    /** Poll the import job and show the progress on the home screen. */
    private void startImportPolling() {
        if (importPoller == null) importPoller = new android.os.Handler();
        if (importPollTask != null) importPoller.removeCallbacks(importPollTask);
        importPollTask = new Runnable() {
            @Override public void run() {
                LetsGoImportJob job = LetsGoImportJob.current();
                if (job == null || job.finished) {
                    refreshState();
                    return;
                }
                int pct = job.total > 0 ? (int) (100.0 * job.done / job.total) : 0;
                home.showProgress(pct, job.phase, job.currentName);
                importPoller.postDelayed(this, 500);
            }
        };
        importPoller.post(importPollTask);
    }

    /** Resolve a SAF tree URI to a filesystem path (primary storage only). */
    private String resolvePath(android.net.Uri uri) {
        try {
            String docId = android.provider.DocumentsContract.getTreeDocumentId(uri);
            if (docId != null && docId.startsWith("primary:")) {
                return "/storage/emulated/0/" + docId.substring("primary:".length());
            }
        } catch (Exception ignored) {}
        return null;
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private LinearLayout.LayoutParams lp(int w, int h) {
        return new LinearLayout.LayoutParams(w, h);
    }

    /** Read fps/smaa/wild from larecomp.ini in the game folder, or null. */
    private String[] readIni() {
        try {
            String dir = gameDir(this);
            File ini = new File(dir != null ? dir : getFilesDir().getAbsolutePath(),
                                "larecomp.ini");
            if (!ini.exists()) return null;
            String fps = "30", smaa = "1", wild = "0";
            java.util.Scanner sc = new java.util.Scanner(ini);
            while (sc.hasNextLine()) {
                String l = sc.nextLine().trim();
                if (l.startsWith("fps = ")) fps = l.substring(6).trim();
                else if (l.startsWith("smaa = ")) smaa = l.substring(7).trim();
                else if (l.startsWith("wild_battle = ")) wild = l.substring(14).trim();
            }
            sc.close();
            return new String[]{fps, smaa, wild};
        } catch (Exception e) { return null; }
    }

    /** Write the game settings to larecomp.ini in the game folder. */
    private void writeIni(String fps, String smaa, String wild) {
        try {
            String dir = gameDir(this);
            File ini = new File(dir != null ? dir : getFilesDir().getAbsolutePath(),
                                "larecomp.ini");
            FileWriter w = new FileWriter(ini);
            w.write("# Let's Go Recomp settings (Android launcher)\n");
            w.write("fps = " + fps + "\n");
            w.write("smaa = " + smaa + "\n");
            w.write("wild_battle = " + wild + "\n");
            w.write("fullscreen = 0\n");
            w.close();
        } catch (Exception ignored) {}
    }
}
