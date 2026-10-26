package com.letsgorecomp;

import android.app.Activity;
import android.content.Context;
import android.graphics.Color;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;

// Simple Xbox 360-style touch gamepad overlay. Buttons inject Android KeyEvents
// (KEYCODE_BUTTON_*, KEYCODE_DPAD_*) which SDL3's Android backend translates to
// controller input. The two "analog sticks" are 8-way directional pads emitting
// DPAD events — enough to move and navigate menus for testing.
public class GamepadOverlay extends FrameLayout {
    private final Activity activity;
    private android.widget.TextView fpsView;
    private final android.os.Handler fpsHandler = new android.os.Handler(android.os.Looper.getMainLooper());

    public GamepadOverlay(Activity activity) {
        super(activity);
        this.activity = activity;
        setLayoutParams(new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        build();
        startFpsUpdater();
    }

    private void startFpsUpdater() {
        fpsHandler.post(new Runnable() {
            @Override public void run() {
                if (fpsView != null) {
                    try {
                        double fps = LetsGoActivity.nativeGetFps();
                        double ms = LetsGoActivity.nativeGetFrameMs();
                        fpsView.setText(String.format("%.1f fps | %.0f ms", fps, ms));
                    } catch (Throwable t) { }
                }
                fpsHandler.postDelayed(this, 500);
            }
        });
    }

    private void sendKey(int keyCode, boolean down) {
        // Direct native input: map Android keycode to Switch button bit
        int btn = -1;
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A: btn = 0; break;      // BTN_A
            case KeyEvent.KEYCODE_BUTTON_B: btn = 1; break;      // BTN_B
            case KeyEvent.KEYCODE_BUTTON_X: btn = 2; break;      // BTN_X
            case KeyEvent.KEYCODE_BUTTON_Y: btn = 3; break;      // BTN_Y
            case KeyEvent.KEYCODE_BUTTON_L1: btn = 6; break;     // BTN_L
            case KeyEvent.KEYCODE_BUTTON_R1: btn = 7; break;     // BTN_R
            case KeyEvent.KEYCODE_BUTTON_START: btn = 10; break; // BTN_PLUS
            case KeyEvent.KEYCODE_BUTTON_SELECT: btn = 11; break; // BTN_MINUS
            case KeyEvent.KEYCODE_DPAD_LEFT: btn = 12; break;   // BTN_LEFT
            case KeyEvent.KEYCODE_DPAD_UP: btn = 13; break;     // BTN_UP
            case KeyEvent.KEYCODE_DPAD_RIGHT: btn = 14; break;  // BTN_RIGHT
            case KeyEvent.KEYCODE_DPAD_DOWN: btn = 15; break;   // BTN_DOWN
        }
        if (btn >= 0) {
            try { LetsGoActivity.nativeSetButton(btn, down); } catch (Throwable t) { }
        }
        // Also send KeyEvent for SDL fallback
        final KeyEvent ev = new KeyEvent(down ? KeyEvent.ACTION_DOWN : KeyEvent.ACTION_UP,
                                        keyCode);
        activity.runOnUiThread(new Runnable() {
            @Override public void run() { activity.dispatchKeyEvent(ev); }
        });
    }

    private Button padButton(final int keyCode, String label, int sizeDp) {
        Button b = new Button(getContext());
        b.setText(label);
        b.setTextSize(14);
        b.setAlpha(0.55f);
        int px = dp(sizeDp);
        b.setLayoutParams(new LinearLayout.LayoutParams(px, px));
        b.setOnTouchListener(new OnTouchListener() {
            @Override public boolean onTouch(View v, MotionEvent e) {
                int a = e.getActionMasked();
                if (a == MotionEvent.ACTION_DOWN) { sendKey(keyCode, true); return true; }
                if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_CANCEL) {
                    sendKey(keyCode, false); return true;
                }
                return false;
            }
        });
        return b;
    }

    // Analog stick: touch position sets X/Y axis via JNI (-32768..32767).
    // stickId: 0=left, 1=right.
    private View analogStick(final String label, final int stickId) {
        View stick = new View(getContext());
        stick.setBackgroundColor(Color.argb(60, 120, 120, 120));
        int px = dp(110);
        stick.setLayoutParams(new FrameLayout.LayoutParams(px, px));
        stick.setOnTouchListener(new OnTouchListener() {
            @Override public boolean onTouch(View v, MotionEvent e) {
                int a = e.getActionMasked();
                float cx = v.getWidth() / 2f, cy = v.getHeight() / 2f;
                float r = Math.min(v.getWidth(), v.getHeight()) / 2f;
                int x = 0, y = 0;
                if (a != MotionEvent.ACTION_UP && a != MotionEvent.ACTION_CANCEL) {
                    float dx = e.getX() - cx, dy = e.getY() - cy;
                    float dist = (float) Math.hypot(dx, dy);
                    if (dist >= r * 0.25f) {
                        float nx = dx / r, ny = dy / r;
                        float len = (float) Math.hypot(nx, ny);
                        if (len > 1f) { nx /= len; ny /= len; }
                        x = (int) (nx * 32767);
                        y = (int) (-ny * 32767);  // up = positive Y
                    }
                }
                try { LetsGoActivity.nativeSetStick(stickId, x, y); } catch (Throwable t) { }
                return true;
            }
        });
        return stick;
    }

    private void build() {
        Context ctx = getContext();

        // ---- D-pad (bottom-left) ----
        FrameLayout dpad = new FrameLayout(ctx);
        int d = dp(150);
        FrameLayout.LayoutParams dlp = new FrameLayout.LayoutParams(d, d);
        dlp.gravity = Gravity.BOTTOM | Gravity.LEFT;
        dlp.leftMargin = dp(16); dlp.bottomMargin = dp(16);
        dpad.setLayoutParams(dlp);
        int b = dp(48);
        addDpadBtn(dpad, KeyEvent.KEYCODE_DPAD_UP, "▲", b, d, b, 0);
        addDpadBtn(dpad, KeyEvent.KEYCODE_DPAD_DOWN, "▼", b, d, b, 2 * b);
        addDpadBtn(dpad, KeyEvent.KEYCODE_DPAD_LEFT, "◀", b, d, 0, b);
        addDpadBtn(dpad, KeyEvent.KEYCODE_DPAD_RIGHT, "▶", b, d, 2 * b, b);
        addView(dpad);

        // ---- ABXY (bottom-right) ----
        FrameLayout abxy = new FrameLayout(ctx);
        FrameLayout.LayoutParams alp = new FrameLayout.LayoutParams(d, d);
        alp.gravity = Gravity.BOTTOM | Gravity.RIGHT;
        alp.rightMargin = dp(16); alp.bottomMargin = dp(16);
        abxy.setLayoutParams(alp);
        addDpadBtn(abxy, KeyEvent.KEYCODE_BUTTON_Y, "Y", b, d, b, 0);
        addDpadBtn(abxy, KeyEvent.KEYCODE_BUTTON_A, "A", b, d, b, 2 * b);
        addDpadBtn(abxy, KeyEvent.KEYCODE_BUTTON_X, "X", b, d, 0, b);
        addDpadBtn(abxy, KeyEvent.KEYCODE_BUTTON_B, "B", b, d, 2 * b, b);
        addView(abxy);

        // ---- Left stick (above d-pad) ----
        View ls = analogStick("L", 0);
        FrameLayout.LayoutParams llp = new FrameLayout.LayoutParams(dp(110), dp(110));
        llp.gravity = Gravity.BOTTOM | Gravity.LEFT;
        llp.leftMargin = dp(24); llp.bottomMargin = dp(190);
        ls.setLayoutParams(llp);
        addView(ls);

        // ---- Right stick (above ABXY) ----
        View rs = analogStick("R", 1);
        FrameLayout.LayoutParams rlp = new FrameLayout.LayoutParams(dp(110), dp(110));
        rlp.gravity = Gravity.BOTTOM | Gravity.RIGHT;
        rlp.rightMargin = dp(24); rlp.bottomMargin = dp(190);
        rs.setLayoutParams(rlp);
        addView(rs);

        // ---- Top row: LB / RB / Start / Select ----
        LinearLayout top = new LinearLayout(ctx);
        top.setOrientation(LinearLayout.HORIZONTAL);
        FrameLayout.LayoutParams tlp = new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        tlp.gravity = Gravity.TOP | Gravity.CENTER_HORIZONTAL;
        tlp.topMargin = dp(12);
        top.setLayoutParams(tlp);
        top.addView(padButton(KeyEvent.KEYCODE_BUTTON_L1, "LB", 56));
        top.addView(padButton(KeyEvent.KEYCODE_BUTTON_R1, "RB", 56));
        top.addView(padButton(KeyEvent.KEYCODE_BUTTON_SELECT, "SEL", 56));
        top.addView(padButton(KeyEvent.KEYCODE_BUTTON_START, "STA", 56));
        addView(top);

        // ---- LOG button: dump logcat and share ----
        Button logBtn = new Button(ctx);
        logBtn.setText("LOG");
        logBtn.setTextSize(10);
        logBtn.setAlpha(0.7f);
        FrameLayout.LayoutParams logLp = new FrameLayout.LayoutParams(dp(64), dp(40));
        logLp.gravity = Gravity.TOP | Gravity.RIGHT;
        logLp.topMargin = dp(8); logLp.rightMargin = dp(8);
        logBtn.setLayoutParams(logLp);
        logBtn.setOnClickListener(new OnClickListener() {
            @Override public void onClick(View v) {
                try {
                    Process p = Runtime.getRuntime().exec(new String[]{"logcat", "-d", "-v", "threadtime"});
                    java.io.InputStream in = p.getInputStream();
                    java.io.File out = new java.io.File(
                        android.os.Environment.getExternalStoragePublicDirectory(
                            android.os.Environment.DIRECTORY_DOWNLOADS),
                        "letsgo-log.txt");
                    java.io.FileOutputStream fos = new java.io.FileOutputStream(out);
                    byte[] buf = new byte[8192];
                    int n;
                    while ((n = in.read(buf)) > 0) fos.write(buf, 0, n);
                    fos.close(); in.close();
                    android.content.Intent share = new android.content.Intent(
                        android.content.Intent.ACTION_SEND);
                    share.setType("text/plain");
                    share.putExtra(android.content.Intent.EXTRA_STREAM,
                        android.net.Uri.fromFile(out));
                    activity.startActivity(android.content.Intent.createChooser(share, "Compartilhar log"));
                } catch (Exception e) { }
            }
        });
        addView(logBtn);

        // ---- CAP button: trigger font capture (MISSÃO 2 v0067) ----
        // Creates $LARECOMP_DATA_ROOT/capture_font to trigger on-demand capture
        Button capBtn = new Button(ctx);
        capBtn.setText("CAP");
        capBtn.setTextSize(10);
        capBtn.setAlpha(0.7f);
        FrameLayout.LayoutParams capLp = new FrameLayout.LayoutParams(dp(64), dp(40));
        capLp.gravity = Gravity.TOP | Gravity.RIGHT;
        capLp.topMargin = dp(8); capLp.rightMargin = dp(80);
        capBtn.setLayoutParams(capLp);
        capBtn.setOnClickListener(new OnClickListener() {
            @Override public void onClick(View v) {
                try {
                    // Get game dir (same as LARECOMP_DATA_ROOT)
                    String gameDir = LetsGoLauncherActivity.gameDir(activity);
                    if (gameDir == null) gameDir = activity.getFilesDir().getAbsolutePath();
                    java.io.File trig = new java.io.File(gameDir, "capture_font");
                    boolean created = trig.createNewFile();
                    // DEFEITO A v0070: log CAP_REQUESTED
                    android.util.Log.i("lasrecomp", "[font-capture] CAP_REQUESTED: trigger file " + trig.getAbsolutePath() + (created ? " created" : " already exists"));
                    android.widget.Toast.makeText(activity, "Captura ativada!", android.widget.Toast.LENGTH_SHORT).show();
                } catch (Exception e) {
                    android.util.Log.e("lasrecomp", "[font-capture] CAP_REQUESTED FAILED: " + e.getMessage());
                    android.widget.Toast.makeText(activity, "Erro: " + e.getMessage(), android.widget.Toast.LENGTH_SHORT).show();
                }
            }
        });
        addView(capBtn);

        // ---- FPS counter (top-left) - added LAST so it's on top of everything ----
        fpsView = new android.widget.TextView(ctx);
        fpsView.setText("FPS --");
        fpsView.setTextColor(Color.GREEN);
        fpsView.setTextSize(18);
        fpsView.setBackgroundColor(Color.BLACK);
        fpsView.setPadding(dp(12), dp(8), dp(12), dp(8));
        FrameLayout.LayoutParams flp = new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        flp.gravity = Gravity.TOP | Gravity.LEFT;
        flp.leftMargin = dp(8); flp.topMargin = dp(8);
        fpsView.setLayoutParams(flp);
        addView(fpsView);
    }

    private void addDpadBtn(FrameLayout parent, int keyCode, String label,
                            int btnPx, int parentPx, int left, int top) {
        Button btn = padButton(keyCode, label, 48);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(btnPx, btnPx);
        lp.leftMargin = left; lp.topMargin = top;
        btn.setLayoutParams(lp);
        parent.addView(btn);
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
