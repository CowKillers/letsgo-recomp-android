package com.letsgorecomp;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.InputStreamReader;

// Shows this app's logcat (tag "lasrecomp" + the activity's own logs) and lets
// the user share it. Works after a native crash: logcat is system-wide and the
// crashed run's lines are still in the ring buffer.
public class LogActivity extends Activity {
    private TextView view;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(12, 14, 18));

        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setPadding(dp(12), dp(12), dp(12), dp(12));

        TextView title = new TextView(this);
        title.setText("Log do app");
        title.setTextSize(18);
        title.setTextColor(Color.WHITE);
        LinearLayout.LayoutParams tlp =
            new LinearLayout.LayoutParams(0, -2, 1f);
        bar.addView(title, tlp);

        Button share = new Button(this);
        share.setText("Compartilhar");
        share.setOnClickListener(new android.view.View.OnClickListener() {
            @Override public void onClick(android.view.View v) {
                Intent i = new Intent(Intent.ACTION_SEND);
                i.setType("text/plain");
                i.putExtra(Intent.EXTRA_TEXT, view.getText().toString());
                startActivity(Intent.createChooser(i, "Enviar log"));
            }
        });
        bar.addView(share, new LinearLayout.LayoutParams(-2, -2));

        Button export = new Button(this);
        export.setText("Exportar");
        export.setOnClickListener(new android.view.View.OnClickListener() {
            @Override public void onClick(android.view.View v) {
                try {
                    java.io.File dl = android.os.Environment.getExternalStoragePublicDirectory(
                            android.os.Environment.DIRECTORY_DOWNLOADS);
                    java.io.File out = new java.io.File(dl, "letsgo-log.txt");
                    java.io.FileWriter w = new java.io.FileWriter(out);
                    w.write(view.getText().toString());
                    w.close();
                    Toast.makeText(LogActivity.this,
                        "Log salvo em Downloads/letsgo-log.txt",
                        Toast.LENGTH_LONG).show();
                } catch (Exception e) {
                    Toast.makeText(LogActivity.this,
                        "Falha ao exportar: " + e.getMessage(),
                        Toast.LENGTH_LONG).show();
                }
            }
        });
        bar.addView(export, new LinearLayout.LayoutParams(-2, -2));

        Button refresh = new Button(this);
        refresh.setText("Atualizar");
        refresh.setOnClickListener(new android.view.View.OnClickListener() {
            @Override public void onClick(android.view.View v) { load(); }
        });
        bar.addView(refresh, new LinearLayout.LayoutParams(-2, -2));

        root.addView(bar, new LinearLayout.LayoutParams(-1, -2));

        view = new TextView(this);
        view.setTypeface(Typeface.MONOSPACE);
        view.setTextSize(11);
        view.setTextColor(Color.rgb(200, 210, 220));
        view.setPadding(dp(12), dp(12), dp(12), dp(12));

        ScrollView scroll = new ScrollView(this);
        scroll.addView(view);
        root.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1f));

        setContentView(root);
        load();
    }

    private void load() {
        new Thread(new Runnable() {
            @Override public void run() {
            StringBuilder sb = new StringBuilder();
            try {
                // our native tag + Android runtime crashes for this process
                Process p = Runtime.getRuntime().exec(
                    new String[]{"logcat", "-d", "-v", "threadtime",
                                 "lasrecomp:V", "AndroidRuntime:E", "*:S"});
                BufferedReader r = new BufferedReader(
                    new InputStreamReader(p.getInputStream()));
                String line;
                int n = 0;
                while ((line = r.readLine()) != null && n < 4000) {
                    sb.append(line).append('\n');
                    n++;
                }
                r.close();
            } catch (Exception e) {
                sb.append("Falha ao ler o log: ").append(e.getMessage());
            }
            final String text = sb.length() == 0
                ? "(log vazio — abra o jogo uma vez e volte aqui)"
                : sb.toString();
            final int len = text.length();
            runOnUiThread(new Runnable() {
                @Override public void run() {
                    view.setText(text);
                    Toast.makeText(LogActivity.this,
                        "Log carregado (" + len + " chars)",
                        Toast.LENGTH_SHORT).show();
                }
            });
            } // end Runnable.run
        }).start();
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
