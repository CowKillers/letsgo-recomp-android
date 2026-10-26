package com.letsgorecomp;

import android.os.Bundle;
import org.libsdl.app.SDLActivity;

// Entry point of the Android port: SDLActivity with our native libraries, plus an
// early native setup (before SDL starts its thread in super.onCreate) telling the
// runtime where its files live (LARECOMP_DATA_ROOT) and where the prebuilt game
// library is (LARECOMP_GAME_LIB). See runtime/platform/android.c.
public class LetsGoActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL3",
            "lasrecomp",
        };
    }

    private void applyFullscreen() {
        // Simplified: rely on theme (@style/Fullscreen) + SDL_SetWindowFullscreen.
        // SDLActivity handles COMMAND_CHANGE_WINDOW_STYLE via WindowInsetsController.
        // Manual calls here were conflicting with SDL's handler.
        android.util.Log.i("lasrecomp", "[fs] applyFullscreen: delegating to SDL/theme");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Force landscape: locked (not sensor) to avoid SDL resize confusion
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        // Resolution scale (user-selectable, emulator-style): set before native loads
        try {
            String scale = getSharedPreferences("letsgo", MODE_PRIVATE).getString("resolution_scale", "0.6667");
            android.system.Os.setenv("LARECOMP_SCALE", scale, true);
            android.util.Log.i("lasrecomp", "[res] scale: " + scale);
        } catch (Exception e) {
            android.util.Log.w("lasrecomp", "[res] scale setup: " + e);
        }
        // DIAGNÓSTICO Teste 1: V-flip para texto (v0062) - DESLIGADO por padrão após regressão
        // Para ativar manualmente: adb shell setprop debug.larecomp.text_vflip 1 (não implementado - usar build específica)
        // Mantido desligado para não quebrar sampler2DArray/sampler2DShadow
        try {
            // NÃO setar por padrão - regressão confirmada em v0062
            // android.system.Os.setenv("LARECOMP_TEXT_VFLIP", "1", true);
            android.util.Log.i("lasrecomp", "[text] V-flip DISABLED (regression fix)");
        } catch (Exception e) {
            android.util.Log.w("lasrecomp", "[text] vflip setup: " + e);
        }
        // v0069: dump de GLSL ativado por padrão para diagnóstico do shader de texto
        try {
            android.system.Os.setenv("LARECOMP_DUMP_FONT_GLSL", "1", true);
            android.util.Log.i("lasrecomp", "[shader] GLSL dump ENABLED (v0069 diagnostic)");
        } catch (Exception e) {
            android.util.Log.w("lasrecomp", "[shader] glsl dump setup: " + e);
        }
        // v0071 ETAPA 3: modo diagnóstico magenta para shaders de texto
        // 1 = magenta opaco sem discard | 2 = magenta com discard | 0 = desligado
        try {
            android.system.Os.setenv("LARECOMP_TEXT_DIAG", "1", true);
            android.util.Log.i("lasrecomp", "[shader] TEXT_DIAG mode 1 ENABLED (v0071: magenta, no discard)");
        } catch (Exception e) {
            android.util.Log.w("lasrecomp", "[shader] text diag setup: " + e);
        }
        // v0090 VS_RANGE_DIAG: override CONDICIONAL do gl_Position do diálogo
        // (só força o quad centrado se o gl_Position calculado estiver fora
        // do clip range). Quads magenta centrados = VS emite fora do range
        // (diverge da simulação); nada = VS emite dentro do range e o
        // problema é downstream do VS. Gated, inerte sem a env.
        try {
            android.system.Os.setenv("LARECOMP_VS_RANGE_DIAG", "1", true);
            android.util.Log.i("lasrecomp", "[shader] VS_RANGE_DIAG ENABLED (v0090: conditional quad)");
        } catch (Exception e) {
            android.util.Log.w("lasrecomp", "[shader] vs range diag setup: " + e);
        }
        // v0088: VS_DIAG DESATIVADO — o quad forçado da v0087 mascararia o fix
        // real (A0_FIX em exec.c). O código VS_DIAG permanece no runtime, gated
        // pela env, mas sem ela setada fica inerte. TEXT_DIAG continua ativo:
        // se o A0_FIX funcionar, o texto aparece como glifos magenta reais nas
        // posições do diálogo (não o blob centralizado da v0087).
        // Custom GPU driver (Turnip via adrenotools): set the env before the native
        // runtime loads, so load_gipa() in device.c picks it up. Null = system driver.
        try {
            GpuDrivers.Driver drv = GpuDrivers.selected(this);
            if (drv != null) {
                java.io.File so = new java.io.File(
                        new java.io.File(GpuDrivers.driversDir(this), drv.slug), drv.libraryName);
                if (so.isFile()) {
                    android.system.Os.setenv("LARECOMP_GPU_DRIVER", so.getAbsolutePath(), true);
                    android.system.Os.setenv("LARECOMP_NATIVE_LIB_DIR",
                            getApplicationInfo().nativeLibraryDir, true);
                    android.util.Log.i("lasrecomp", "[gpu] custom driver: " + so.getAbsolutePath());
                } else {
                    android.util.Log.w("lasrecomp", "[gpu] selected driver .so missing: " + so.getAbsolutePath());
                }
            } else {
                android.util.Log.i("lasrecomp", "[gpu] system driver (no custom driver selected)");
            }
        } catch (Exception e) {
            android.util.Log.w("lasrecomp", "[gpu] driver setup: " + e);
        }
        System.loadLibrary("lasrecomp");
        // The user-picked game folder (via the launcher), or the app's files
        // dir as a fallback. The native runtime reads LARECOMP_DATA_ROOT.
        String gameDir = LetsGoLauncherActivity.gameDir(this);
        if (gameDir == null) gameDir = getFilesDir().getAbsolutePath();
        // Copy bundled DejaVuSans.ttf to the game folder for the system font.
        // Diagnosed copy: logs success/failure, validates size, cleans up partial files.
        try {
            java.io.File fontDir = new java.io.File(gameDir, "fonts");
            fontDir.mkdirs();
            java.io.File fontFile = new java.io.File(fontDir, "DejaVuSans.ttf");
            if (fontFile.exists() && fontFile.length() > 100000) {
                android.util.Log.i("lasrecomp", "[font] bundled DejaVuSans.ttf already present (" + fontFile.length() + " bytes)");
            } else {
                // Remove partial/corrupt file before retrying.
                if (fontFile.exists()) fontFile.delete();
                java.io.InputStream in = null;
                java.io.OutputStream out = null;
                try {
                    in = getAssets().open("fonts/DejaVuSans.ttf");
                    out = new java.io.FileOutputStream(fontFile);
                    byte[] buf = new byte[8192];
                    int n;
                    long total = 0;
                    while ((n = in.read(buf)) > 0) { out.write(buf, 0, n); total += n; }
                    out.flush();
                    android.util.Log.i("lasrecomp", "[font] copied bundled DejaVuSans.ttf (" + total + " bytes) to " + fontFile.getAbsolutePath());
                    if (total < 100000) {
                        android.util.Log.w("lasrecomp", "[font] WARNING: copied font is suspiciously small (" + total + " bytes), deleting");
                        fontFile.delete();
                    }
                } finally {
                    if (in != null) try { in.close(); } catch (Exception ignored) {}
                    if (out != null) try { out.close(); } catch (Exception ignored) {}
                }
            }
        } catch (Exception e) {
            android.util.Log.e("lasrecomp", "[font] FAILED to copy bundled font: " + e.getMessage());
        }
        // Copy bundled pre-compiled SPIR-V shaders to the shader cache dir
        // (<gameDir>/cache/spirv/), so first runs don't stutter compiling.
        // Only copies files that are not already cached (runtime may have newer ones).
        try {
            String[] spirvAssets = getAssets().list("spirv");
            if (spirvAssets != null && spirvAssets.length > 0) {
                java.io.File cacheDir = new java.io.File(gameDir, "cache/spirv");
                cacheDir.mkdirs();
                int copied = 0, skipped = 0;
                for (String name : spirvAssets) {
                    java.io.File dst = new java.io.File(cacheDir, name);
                    if (dst.exists() && dst.length() > 0) { skipped++; continue; }
                    java.io.InputStream in = null;
                    java.io.OutputStream out = null;
                    try {
                        in = getAssets().open("spirv/" + name);
                        out = new java.io.FileOutputStream(dst);
                        byte[] buf = new byte[8192];
                        int n;
                        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                        out.flush();
                        copied++;
                    } finally {
                        if (in != null) try { in.close(); } catch (Exception ignored) {}
                        if (out != null) try { out.close(); } catch (Exception ignored) {}
                    }
                }
                android.util.Log.i("lasrecomp", "[shader] bundled SPIR-V: " + copied + " copied, " + skipped + " already cached (" + spirvAssets.length + " in APK)");
            } else {
                android.util.Log.i("lasrecomp", "[shader] no bundled SPIR-V in APK");
            }
        } catch (Exception e) {
            android.util.Log.e("lasrecomp", "[shader] FAILED to copy bundled SPIR-V: " + e.getMessage());
        }
        nativeSetup(gameDir, getApplicationInfo().nativeLibraryDir);
        super.onCreate(savedInstanceState);
        // Apply fullscreen AFTER super.onCreate (SDLActivity resets UI visibility)
        applyFullscreen();
        // Touch gamepad overlay (Xbox 360 layout) - toggleable in launcher settings
        boolean touchEnabled = getSharedPreferences("letsgo", MODE_PRIVATE).getBoolean("touch", true);
        if (touchEnabled) {
            addContentView(new GamepadOverlay(this),
                new android.view.ViewGroup.LayoutParams(
                    android.view.ViewGroup.LayoutParams.MATCH_PARENT,
                    android.view.ViewGroup.LayoutParams.MATCH_PARENT));
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Fullscreen handled by theme + SDL_SetWindowFullscreen (like Wonder).
        // No manual intervention to avoid conflicts with SDLActivity.
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        // Fullscreen handled by theme + SDL. No manual re-apply.
    }

    // Force landscape: SDL's native code calls setOrientationBis() which overrides
    // our setRequestedOrientation(). By overriding it here, we force landscape always.
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    public static native void nativeSetup(String filesDir, String libDir);
    public static native void nativeSetButton(int button, boolean pressed);
    public static native void nativeSetStick(int stick, int x, int y);
    public static native double nativeGetFps();
    public static native double nativeGetFrameMs();
}
