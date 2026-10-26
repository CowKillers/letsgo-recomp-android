package com.letsgorecomp;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.ArrayAdapter;
import android.widget.TextView;

import java.io.File;

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
    private TextView status;
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

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setBackgroundColor(Color.rgb(20, 22, 28));
        int pad = dp(24);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("Let's Go Recomp");
        title.setTextSize(28);
        title.setTextColor(Color.WHITE);
        title.setGravity(Gravity.CENTER);
        root.addView(title, lp(-1, -2));

        status = new TextView(this);
        status.setTextSize(15);
        status.setTextColor(Color.rgb(180, 190, 200));
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, dp(12), 0, dp(24));
        root.addView(status, lp(-1, -2));
        refreshStatus();

        Button play = new Button(this);
        play.setText("Jogar");
        play.setTextSize(18);
        play.setOnClickListener(new android.view.View.OnClickListener() {
            @Override public void onClick(android.view.View v) {
                // Never boot from a partial import: the game FATALs on missing exefs/main.
                LetsGoImportJob ij = LetsGoImportJob.current();
                if (ij != null && !ij.finished) {
                    android.widget.Toast.makeText(LetsGoLauncherActivity.this,
                        "Aguarde a importação terminar antes de jogar.",
                        android.widget.Toast.LENGTH_LONG).show();
                    return;
                }
                // Never launch into a native crash: without complete game data
                // the runtime FATALs (see main.c boot()). Tell the user instead.
                if (!gameDataReady(LetsGoLauncherActivity.this)) {
                    android.widget.Toast.makeText(LetsGoLauncherActivity.this,
                        "O jogo ainda não foi importado por completo: toque em \"Importar jogo do NSP\" e aguarde chegar a 100%.",
                        android.widget.Toast.LENGTH_LONG).show();
                    return;
                }
                startActivity(new Intent(LetsGoLauncherActivity.this,
                                         LetsGoActivity.class));
            }
        });
        root.addView(play, lp(-1, -2));

        Button pick = new Button(this);
        pick.setText("Selecionar pasta do jogo");
        pick.setOnClickListener(new android.view.View.OnClickListener() {
            @Override public void onClick(android.view.View v) {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                startActivityForResult(i, REQ_GAME_DIR);
            }
        });
        LinearLayout.LayoutParams plp = lp(-1, -2);
        plp.topMargin = dp(12);
        root.addView(pick, plp);

        Button importBtn = new Button(this);
        importBtn.setText("Importar jogo do NSP");
        importBtn.setOnClickListener(new android.view.View.OnClickListener() {
            @Override public void onClick(android.view.View v) {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                startActivityForResult(i, REQ_IMPORT_DIR);
            }
        });
        LinearLayout.LayoutParams ilp = lp(-1, -2);
        ilp.topMargin = dp(12);
        root.addView(importBtn, ilp);

        Button importFolderBtn = new Button(this);
        importFolderBtn.setText("Importar NSP para pasta (sem .bin)");
        importFolderBtn.setOnClickListener(new android.view.View.OnClickListener() {
            @Override public void onClick(android.view.View v) {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                startActivityForResult(i, REQ_IMPORT_DIR_FOLDER);
            }
        });
        LinearLayout.LayoutParams iflp = lp(-1, -2);
        iflp.topMargin = dp(8);
        root.addView(importFolderBtn, iflp);

        Button log = new Button(this);
        log.setText("Ver log");
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
        drvTitle.setText("Driver GPU (Turnip)");
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
                    sb.append("Em uso: ").append(sel.name).append(" (").append(sel.driverVersion).append(")\n");
                } else {
                    sb.append("Em uso: driver do sistema\n");
                }
                java.util.List<GpuDrivers.Driver> all = GpuDrivers.list(LetsGoLauncherActivity.this);
                if (all.isEmpty()) {
                    sb.append("Nenhum driver Turnip instalado.");
                } else {
                    sb.append("Instalados: ");
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
        importDrv.setText("Importar driver Turnip (.zip)");
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
        useSystem.setText("Usar driver do sistema");
        useSystem.setOnClickListener(new android.view.View.OnClickListener() {
            @Override public void onClick(android.view.View v) {
                GpuDrivers.select(LetsGoLauncherActivity.this, null);
                refreshDrivers.run();
                android.widget.Toast.makeText(LetsGoLauncherActivity.this,
                    "Driver do sistema selecionado.", android.widget.Toast.LENGTH_SHORT).show();
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
                    "Driver selecionado: " + next.name, android.widget.Toast.LENGTH_SHORT).show();
            }
        });

        // ---- Game settings (written to larecomp.ini in the game folder) ----
        TextView cfgTitle = new TextView(this);
        cfgTitle.setText("Configurações");
        cfgTitle.setTextSize(18);
        cfgTitle.setTextColor(Color.WHITE);
        cfgTitle.setPadding(0, dp(20), 0, dp(8));
        root.addView(cfgTitle, lp(-1, -2));

        final CheckBox fps60 = new CheckBox(this);
        fps60.setText("60 fps (experimental)");
        fps60.setTextColor(Color.rgb(200, 210, 220));
        root.addView(fps60, lp(-1, -2));

        final CheckBox smaa = new CheckBox(this);
        smaa.setText("Suavizar serrilhado (SMAA)");
        smaa.setTextColor(Color.rgb(200, 210, 220));
        smaa.setChecked(true);
        root.addView(smaa, lp(-1, -2));

        // Resolution scale selector (emulator-style)
        TextView resLabel = new TextView(this);
        resLabel.setText("Resolução interna:");
        resLabel.setTextColor(Color.rgb(200, 210, 220));
        root.addView(resLabel, lp(-1, -2));

        final android.widget.Spinner resSpinner = new android.widget.Spinner(this);
        final String[] resNames = {"0.5x (480p) - Performance", "0.75x (540p) - Equilibrado", "1x (720p) - Padrão", "2x (1080p) - Qualidade"};
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

        // Game language: the game's RomFS has every language's texts (bin/message/<language>/: English, French,
        // Spanish...), and a dump from any region is the same game. The runtime tells the game larecomp.ini's
        // game_lang (runtime/mods.c game_language); empty = English, which is also what a fan translation that
        // replaces the English/ folder (PT-BR) needs. An installed mod that carries its own language still wins.
        TextView langLabel = new TextView(this);
        langLabel.setText("Idioma do jogo:");
        langLabel.setTextColor(Color.rgb(200, 210, 220));
        root.addView(langLabel, lp(-1, -2));

        final android.widget.Spinner langSpinner = new android.widget.Spinner(this);
        final String[] langNames = {"Padrão (inglês / tradução PT-BR)", "English", "Français", "Español", "Deutsch",
                                    "Italiano", "日本語", "한국어", "简体中文", "繁體中文"};
        final String[] langValues = {"", "en-US", "fr", "es", "de", "it", "ja", "ko", "zh-CN", "zh-TW"};
        android.widget.ArrayAdapter<String> langAdapter = new android.widget.ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item, langNames);
        langAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        langSpinner.setAdapter(langAdapter);
        String savedLang = readIniValue("game_lang");
        for (int i = 0; i < langValues.length; i++) {
            if (langValues[i].equals(savedLang == null ? "" : savedLang)) {
                langSpinner.setSelection(i);
                break;
            }
        }
        langSpinner.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> parent, android.view.View view, int position, long id) {
                String cur = readIniValue("game_lang");
                if (langValues[position].equals(cur == null ? "" : cur)) return;  // (the initial selection)
                java.util.LinkedHashMap<String, String> kv = new java.util.LinkedHashMap<>();
                kv.put("game_lang", langValues[position]);
                updateIni(kv);
            }
            @Override public void onNothingSelected(android.widget.AdapterView<?> parent) {}
        });
        root.addView(langSpinner, lp(-1, -2));

        final CheckBox wild = new CheckBox(this);
        wild.setText("Batalhas selvagens (experimental)");
        wild.setTextColor(Color.rgb(200, 210, 220));
        root.addView(wild, lp(-1, -2));

        final CheckBox touch = new CheckBox(this);
        touch.setText("Controles touch na tela");
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
        hint.setText("Se o jogo fechar ao abrir, use \"Ver log\" e me envie o conteúdo.");
        hint.setTextSize(13);
        hint.setTextColor(Color.rgb(140, 150, 160));
        hint.setGravity(Gravity.CENTER);
        hint.setPadding(0, dp(24), 0, 0);
        root.addView(hint, lp(-1, -2));

        android.widget.ScrollView scroll = new android.widget.ScrollView(this);
        scroll.setFillViewport(true);
        scroll.addView(root);
        setContentView(scroll);
    }

    private void refreshStatus() {
        // If an import is running, the poller owns the status line.
        LetsGoImportJob job = LetsGoImportJob.current();
        if (job != null && !job.finished) { startImportPolling(); return; }
        File gameLib = new File(getApplicationInfo().nativeLibraryDir, "liblasgame.so");
        String dir = gameDir(this);
        File base = dir != null ? new File(dir) : getFilesDir();
        File dataRoot = new File(base, "extracted/romfs");
        File dataRootAlt = new File(base, "romfs");
        boolean romfsOk = dataRoot.exists() || dataRootAlt.exists();
        if (gameLib.exists() && romfsOk) {
            status.setText("Jogo pronto para iniciar.\nPasta: " + (dir != null ? dir : "(interna)"));
        } else if (dir != null) {
            status.setText("Pasta selecionada, mas romfs não encontrado nela.\n" + dir);
        } else {
            status.setText("Selecione a pasta do jogo (com extracted/romfs dentro).");
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
            refreshStatus();
        } else if ((req == REQ_IMPORT_DIR || req == REQ_IMPORT_DIR_FOLDER) && res == RESULT_OK
                && data != null && data.getData() != null) {
            android.widget.Toast.makeText(this, "Pasta escolhida, iniciando...",
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
                android.widget.Toast.makeText(this, "Driver instalado: " + r.driver.name,
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
        status.setText("Iniciando importação para:\n" + outDir.getAbsolutePath());
        LetsGoImportJob job = LetsGoImportJob.start(this, uri, outDir, folder);
        if (job != null) startImportPolling();
        else status.setText("Já há uma importação em andamento.");
    }

    /** Poll the import job and show progress in the status line. */
    private void startImportPolling() {
        if (importPoller == null) importPoller = new android.os.Handler();
        if (importPollTask != null) importPoller.removeCallbacks(importPollTask);
        importPollTask = new Runnable() {
            @Override public void run() {
                LetsGoImportJob job = LetsGoImportJob.current();
                if (job == null || job.finished) {
                    if (job != null) {
                        if (job.error != null) status.setText("Erro na importação:\n" + job.error);
                        else if (job.cancelled) status.setText("Importação cancelada.");
                        else status.setText("Importação concluída em " +
                                String.format("%.0f", job.seconds) + "s.\n" + job.containerName);
                    }
                    refreshStatus();
                    return;
                }
                String pct = job.total > 0
                        ? String.format(" %.0f%%", 100.0 * job.done / job.total)
                        : "";
                String txt = "Importando [" + job.phase + "]" + pct + "\n" + job.currentName;
                if (!job.keysOk) txt = "Procurando prod.keys e NSP...";
                status.setText(txt);
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

    private File iniFile() {
        String dir = gameDir(this);
        return new File(dir != null ? dir : getFilesDir().getAbsolutePath(), "larecomp.ini");
    }

    /** One key's value in larecomp.ini ("key = value", the runtime's format), or null if absent. */
    private String readIniValue(String key) {
        try {
            File ini = iniFile();
            if (!ini.exists()) return null;
            String val = null;
            java.util.Scanner sc = new java.util.Scanner(ini, "UTF-8");
            while (sc.hasNextLine()) {
                String l = sc.nextLine().trim();
                int eq = l.indexOf('=');
                if (eq > 0 && l.substring(0, eq).trim().equals(key)) val = l.substring(eq + 1).trim();
            }
            sc.close();
            return val;
        } catch (Exception e) { return null; }
    }

    /** Read fps/smaa/wild from larecomp.ini in the game folder, or null. */
    private String[] readIni() {
        if (!iniFile().exists()) return null;
        String fps = readIniValue("fps"), smaa = readIniValue("smaa"), wild = readIniValue("wild_battle");
        return new String[]{fps != null ? fps : "30", smaa != null ? smaa : "1", wild != null ? wild : "0"};
    }

    /** Write the launcher's game settings to larecomp.ini in the game folder. */
    private void writeIni(String fps, String smaa, String wild) {
        java.util.LinkedHashMap<String, String> kv = new java.util.LinkedHashMap<>();
        kv.put("fps", fps);
        kv.put("smaa", smaa);
        kv.put("wild_battle", wild);
        kv.put("fullscreen", "0");
        updateIni(kv);
    }

    /** Set some keys of larecomp.ini, keeping every other line (game_lang, mod, render_height, controller
     *  bindings... the runtime writes them too): the file used to be rewritten with only the launcher's keys. */
    private void updateIni(java.util.Map<String, String> kv) {
        try {
            File ini = iniFile();
            java.util.List<String> lines = new java.util.ArrayList<>();
            if (ini.exists()) {
                java.util.Scanner sc = new java.util.Scanner(ini, "UTF-8");
                while (sc.hasNextLine()) lines.add(sc.nextLine());
                sc.close();
            } else {
                lines.add("# Let's Go Recomp settings (Android launcher)");
            }
            java.io.Writer w = new java.io.OutputStreamWriter(new java.io.FileOutputStream(ini), "UTF-8");
            for (String l : mergeIni(lines, kv)) w.write(l + "\n");
            w.close();
        } catch (Exception ignored) {}
    }

    /** lines with kv's keys set ("key = value", in place; the missing ones appended), every other line unchanged. */
    static java.util.List<String> mergeIni(java.util.List<String> lines, java.util.Map<String, String> kv) {
        java.util.List<String> out = new java.util.ArrayList<>();
        java.util.Set<String> done = new java.util.HashSet<>();
        for (String l : lines) {
            int eq = l.indexOf('=');
            String k = eq > 0 ? l.substring(0, eq).trim() : null;
            if (k != null && kv.containsKey(k)) {
                if (done.add(k)) out.add(k + " = " + kv.get(k));  // (a duplicated key: kept once)
            } else {
                out.add(l);
            }
        }
        for (java.util.Map.Entry<String, String> e : kv.entrySet())
            if (!done.contains(e.getKey())) out.add(e.getKey() + " = " + e.getValue());
        return out;
    }
}
