package com.letsgorecomp;

import android.content.Context;
import android.graphics.Color;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

/**
 * The home screen of the launcher, built in code (no layout XML, no new dependency).
 *
 * It follows the mock-up design/maquette-accueil.html: a header, step 1 (your game) and
 * step 2 (play). Step 1 shows the real state of the installation — first run, importing,
 * error, ready — and only ONE amber action button is shown at a time (mock-up P6): the
 * import button while the game is missing, the play button once it is there.
 *
 * The launcher owns the actions (it is the one that knows the folder picker and the
 * import job); this class only draws and reports.
 */
final class HomeScreen {
    // The amber of the single "next action" button (mock-up P6).
    static final int AMBER = 0xFFF0A030;
    static final int AMBER_TEXT = 0xFF1A1206;
    static final int INK = 0xFFE8ECF2;
    static final int MUTED = 0xFF9AA6B4;
    static final int CARD = 0xFF1B1F27;
    static final int CARD_LINE = 0xFF2C3340;

    /** What the home screen can ask the launcher to do. */
    interface Callbacks {
        void onImportNsp();
        void onUseExtractedFolder();
        void onChangeFolder();
        void onPlay();
    }

    private final Context ctx;
    private final Callbacks cb;

    private TextView hint1, errorText, doneText, playNote, progressPct, progressFile;
    private TextView errorTitle;
    private LinearLayout errorBox, importingBox, doneBox, actions1, actions2;
    private ProgressBar progressBar;
    private Button importBtn, folderBtn, retryBtn, playBtn, changeBtn;

    HomeScreen(Context ctx, Callbacks cb) {
        this.ctx = ctx;
        this.cb = cb;
    }

    /** The whole screen (a scroll view): header, step 1, step 2. */
    View build() {
        LinearLayout root = new LinearLayout(ctx);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setBackgroundColor(Color.rgb(14, 16, 20));
        int pad = dp(20);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(ctx);
        title.setText(Txt.t("title", "Let's Go Recomp"));
        title.setTextSize(26);
        title.setTextColor(Color.WHITE);
        title.setGravity(Gravity.CENTER);
        root.addView(title, lp(-1, -2));

        TextView intro = new TextView(ctx);
        intro.setText(Txt.t("intro", "Port não oficial de Pokémon: Let's Go, Évoli. O jogo não vem incluído: importe a sua própria cópia e depois jogue."));
        intro.setTextSize(14);
        intro.setTextColor(MUTED);
        intro.setGravity(Gravity.CENTER);
        intro.setPadding(0, dp(6), 0, dp(18));
        root.addView(intro, lp(-1, -2));

        root.addView(buildStep1(), lp(-1, -2, 0, 0, 0, 8));
        root.addView(buildStep2(), lp(-1, -2));

        android.widget.ScrollView scroll = new android.widget.ScrollView(ctx);
        scroll.setFillViewport(true);
        scroll.addView(root);
        return scroll;
    }

    // ------------------------------------------------------------------ step 1

    private LinearLayout buildStep1() {
        LinearLayout s1 = new LinearLayout(ctx);
        s1.setOrientation(LinearLayout.VERTICAL);
        s1.setBackground(card());
        s1.setPadding(dp(16), dp(14), dp(16), dp(14));

        s1.addView(stepHeader("1", Txt.t("step1", "Seu jogo")), lp(-1, -2));

        hint1 = new TextView(ctx);
        hint1.setText(Txt.t("step1_hint", "Escolha a pasta que contém o seu arquivo .nsp e o prod.keys. A importação leva alguns minutos e só é feita uma vez."));
        hint1.setTextSize(13);
        hint1.setTextColor(MUTED);
        hint1.setPadding(0, dp(8), 0, 0);
        s1.addView(hint1, lp(-1, -2));

        // error state
        errorBox = new LinearLayout(ctx);
        errorBox.setOrientation(LinearLayout.VERTICAL);
        errorTitle = new TextView(ctx);
        errorTitle.setText(Txt.t("error_title", "A importação falhou."));
        errorTitle.setTextSize(13);
        errorTitle.setTextColor(0xFFE06A5A);
        errorBox.addView(errorTitle, lp(-1, -2));
        errorText = new TextView(ctx);
        errorText.setTextSize(13);
        errorText.setTextColor(0xFFE06A5A);
        errorBox.addView(errorText, lp(-1, -2));
        s1.addView(errorBox, lp(-1, -2));

        // importing state
        importingBox = new LinearLayout(ctx);
        importingBox.setOrientation(LinearLayout.VERTICAL);
        LinearLayout progRow = new LinearLayout(ctx);
        progRow.setOrientation(LinearLayout.HORIZONTAL);
        TextView importingLabel = new TextView(ctx);
        importingLabel.setText(Txt.t("importing", "Importando"));
        importingLabel.setTextSize(14);
        importingLabel.setTextColor(INK);
        progRow.addView(importingLabel, lp(-1, -2));
        progressPct = new TextView(ctx);
        progressPct.setTextSize(14);
        progressPct.setTextColor(AMBER);
        progRow.addView(progressPct, lp(-2, -2));
        importingBox.addView(progRow, lp(-1, -2));
        progressBar = new ProgressBar(ctx, null, android.R.attr.progressBarStyleHorizontal);
        progressBar.setProgressTintList(android.content.res.ColorStateList.valueOf(AMBER));
        LinearLayout.LayoutParams pblp = lp(-1, dp(6));
        pblp.topMargin = dp(6);
        importingBox.addView(progressBar, pblp);
        progressFile = new TextView(ctx);
        progressFile.setTextSize(12);
        progressFile.setTextColor(MUTED);
        progressFile.setPadding(0, dp(6), 0, 0);
        importingBox.addView(progressFile, lp(-1, -2));
        TextView keepOpen = new TextView(ctx);
        keepOpen.setText(Txt.t("keep_open", "Mantenha o app aberto até 100%. Se cancelar, a próxima importação continua de onde parou."));
        keepOpen.setTextSize(12);
        keepOpen.setTextColor(MUTED);
        keepOpen.setPadding(0, dp(6), 0, 0);
        importingBox.addView(keepOpen, lp(-1, -2));
        s1.addView(importingBox, lp(-1, -2));

        // ready state
        doneBox = new LinearLayout(ctx);
        doneBox.setOrientation(LinearLayout.HORIZONTAL);
        doneBox.setGravity(Gravity.CENTER_VERTICAL);
        doneText = new TextView(ctx);
        doneText.setTextSize(13);
        doneText.setTextColor(0xFF7FD18C);
        doneBox.addView(doneText, lp(-1, -2));
        s1.addView(doneBox, lp(-1, -2));

        // actions
        actions1 = new LinearLayout(ctx);
        actions1.setOrientation(LinearLayout.VERTICAL);
        importBtn = button(Txt.t("import_btn", "Importar meu jogo (NSP)"), true,
                new android.view.View.OnClickListener() {
                    @Override public void onClick(android.view.View v) { cb.onImportNsp(); }
                });
        actions1.addView(importBtn, lp(-1, -2));
        folderBtn = button(Txt.t("alt_folder", "Já tenho uma pasta extraída"), false,
                new android.view.View.OnClickListener() {
                    @Override public void onClick(android.view.View v) { cb.onUseExtractedFolder(); }
                });
        LinearLayout.LayoutParams fblp = lp(-1, -2);
        fblp.topMargin = dp(6);
        actions1.addView(folderBtn, fblp);
        retryBtn = button(Txt.t("retry", "Escolher outra pasta"), true,
                new android.view.View.OnClickListener() {
                    @Override public void onClick(android.view.View v) { cb.onImportNsp(); }
                });
        actions1.addView(retryBtn, lp(-1, -2));
        s1.addView(actions1, lp(-1, -2));

        return s1;
    }

    // ------------------------------------------------------------------ step 2

    private LinearLayout buildStep2() {
        LinearLayout s2 = new LinearLayout(ctx);
        s2.setOrientation(LinearLayout.VERTICAL);
        s2.setBackground(card());
        s2.setPadding(dp(16), dp(14), dp(16), dp(14));

        s2.addView(stepHeader("2", Txt.t("step2", "Jogar")), lp(-1, -2));

        actions2 = new LinearLayout(ctx);
        actions2.setOrientation(LinearLayout.VERTICAL);
        playBtn = button(Txt.t("play", "Jogar"), true,
                new android.view.View.OnClickListener() {
                    @Override public void onClick(android.view.View v) { cb.onPlay(); }
                });
        actions2.addView(playBtn, lp(-1, -2));
        changeBtn = button(Txt.t("change", "Trocar"), false,
                new android.view.View.OnClickListener() {
                    @Override public void onClick(android.view.View v) { cb.onChangeFolder(); }
                });
        LinearLayout.LayoutParams chlp = lp(-1, -2);
        chlp.topMargin = dp(6);
        actions2.addView(changeBtn, chlp);
        s2.addView(actions2, lp(-1, -2));

        playNote = new TextView(ctx);
        playNote.setTextSize(13);
        playNote.setTextColor(MUTED);
        playNote.setPadding(0, dp(8), 0, 0);
        s2.addView(playNote, lp(-1, -2));

        return s2;
    }

    // ------------------------------------------------------------------ state

    /**
     * Show the state of the installation.
     *
     * @param importing an import is running
     * @param error    the last import failed (null when there was none)
     * @param ready    a complete game is present
     */
    void showState(boolean importing, String error, boolean ready) {
        boolean failed = !importing && error != null;
        hint1.setVisibility(importing || ready ? View.GONE : View.VISIBLE);
        errorBox.setVisibility(failed ? View.VISIBLE : View.GONE);
        importingBox.setVisibility(importing ? View.VISIBLE : View.GONE);
        doneBox.setVisibility(ready ? View.VISIBLE : View.GONE);
        actions1.setVisibility(importing ? View.GONE : View.VISIBLE);
        actions2.setVisibility(ready ? View.VISIBLE : View.GONE);

        // Only one amber button at a time (mock-up P6).
        importBtn.setVisibility(!ready && !failed ? View.VISIBLE : View.GONE);
        folderBtn.setVisibility(!ready && !failed ? View.VISIBLE : View.GONE);
        retryBtn.setVisibility(failed ? View.VISIBLE : View.GONE);

        if (failed) errorText.setText(error);
        if (ready) {
            doneText.setText(Txt.t("done", "Jogo importado.") + "  "
                    + Txt.t("done_path", "Armazenamento interno do app"));
            playNote.setText(Txt.t("ready_hint", "Se o jogo fechar ao abrir, use \"Ver log\" em Avançado e me envie o conteúdo."));
        } else {
            playNote.setText(Txt.t("locked", "Disponível quando a etapa 1 terminar."));
        }
    }

    /** The progress of a running import (percent, bar, current file). */
    void showProgress(int percent, String phase, String file) {
        progressPct.setText(percent + " %");
        progressBar.setProgress(percent);
        progressFile.setText("[" + phase + "] " + file);
    }

    // ------------------------------------------------------------------ helpers

    private LinearLayout stepHeader(String number, String title) {
        LinearLayout head = new LinearLayout(ctx);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);

        TextView badge = new TextView(ctx);
        badge.setText(number);
        badge.setTextSize(14);
        badge.setTextColor(AMBER_TEXT);
        badge.setGravity(Gravity.CENTER);
        badge.setBackground(round(AMBER, 999));
        head.addView(badge, lp(dp(26), dp(26)));

        TextView label = new TextView(ctx);
        label.setText(title);
        label.setTextSize(18);
        label.setTextColor(INK);
        label.setPadding(dp(10), 0, 0, 0);
        head.addView(label, lp(-1, -2));
        return head;
    }

    private Button button(String text, boolean primary, android.view.View.OnClickListener l) {
        Button btn = new Button(ctx);
        btn.setText(text);
        btn.setTextSize(15);
        btn.setAllCaps(false);
        btn.setGravity(Gravity.CENTER);
        if (primary) {
            btn.setTextColor(AMBER_TEXT);
            btn.setBackground(round(AMBER, 12));
        } else {
            btn.setTextColor(MUTED);
            btn.setBackground(round(0x00000000, 12));
        }
        btn.setOnClickListener(l);
        return btn;
    }

    private android.graphics.drawable.Drawable card() {
        android.graphics.drawable.GradientDrawable d = new android.graphics.drawable.GradientDrawable();
        d.setColor(CARD);
        d.setCornerRadius(dp(14));
        d.setStroke(dp(1), CARD_LINE);
        return d;
    }

    private android.graphics.drawable.Drawable round(int color, int radiusDp) {
        android.graphics.drawable.GradientDrawable d = new android.graphics.drawable.GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(radiusDp));
        return d;
    }

    private int dp(int v) {
        return Math.round(v * ctx.getResources().getDisplayMetrics().density);
    }

    private LinearLayout.LayoutParams lp(int w, int h) {
        return new LinearLayout.LayoutParams(w, h);
    }

    private LinearLayout.LayoutParams lp(int w, int h, int l, int t, int r, int b) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(w, h);
        p.setMargins(l, t, r, b);
        return p;
    }
}
