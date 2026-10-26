package com.letsgorecomp;

import android.content.Context;
import android.net.Uri;
import android.os.Build;
import android.util.Log;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Custom GPU drivers (Turnip) for the Driver tab. Each driver is a folder under the app-private files dir
 * (never on shared storage: dlopen refuses libraries other apps could tamper with):
 *   <files>/gpu_drivers/<slug>/{meta.json, <libraryName>.so}
 * The launcher writes the choice to config.txt (LARECOMP_GPU_DRIVER=&lt;slug&gt;, LARECOMP_GPU_DRIVER_LIB=&lt;.so&gt;); the
 * runtime loads it through adrenotools in the :game process. No LARECOMP_GPU_DRIVER line = the system driver.
 */
final class GpuDrivers {
    static final String KEY_DRIVER = "LARECOMP_GPU_DRIVER";
    static final String KEY_LIB = "LARECOMP_GPU_DRIVER_LIB";
    static final String BUNDLED_ASSET = "turnip-8g2.zip";

    static final class Driver {
        final String slug;  // folder name under gpu_drivers/
        final String name;
        final String description;
        final String driverVersion;
        final String libraryName;

        Driver(String slug, String name, String description, String driverVersion, String libraryName) {
            this.slug = slug;
            this.name = name;
            this.description = description;
            this.driverVersion = driverVersion;
            this.libraryName = libraryName;
        }
    }

    /** Result of an import: either a driver or a human-readable error. */
    static final class ImportResult {
        final Driver driver;
        final String error;

        ImportResult(Driver driver, String error) {
            this.driver = driver;
            this.error = error;
        }
    }

    private GpuDrivers() {
    }

    static File driversDir(Context c) {
        return new File(c.getFilesDir(), "gpu_drivers");
    }

    /** All installed drivers whose .so is present, in no particular order. */
    static List<Driver> list(Context c) {
        List<Driver> out = new ArrayList<>();
        File[] dirs = driversDir(c).listFiles();
        if (dirs == null) return out;
        for (File d : dirs) {
            if (!d.isDirectory()) continue;
            Driver drv = read(d);
            if (drv != null) out.add(drv);
        }
        return out;
    }

    /** The selected driver, or null for the system driver (also null when the selection points nowhere). */
    static Driver selected(Context c) {
        String slug = c.getSharedPreferences("letsgo", Context.MODE_PRIVATE).getString(KEY_DRIVER, null);
        if (slug == null || slug.isEmpty()) return null;
        Driver d = read(new File(driversDir(c), slug));
        if (d == null) return null;  // stale selection: the folder is gone
        String lib = c.getSharedPreferences("letsgo", Context.MODE_PRIVATE).getString(KEY_LIB, null);
        if (!d.libraryName.equals(lib)) return null;
        return d;
    }

    /** Select a driver, or null for the system driver. Writes to the letsgo prefs. */
    static void select(Context c, Driver d) {
        android.content.SharedPreferences.Editor e =
                c.getSharedPreferences("letsgo", Context.MODE_PRIVATE).edit();
        if (d == null) {
            e.remove(KEY_DRIVER);
            e.remove(KEY_LIB);
        } else {
            e.putString(KEY_DRIVER, d.slug);
            e.putString(KEY_LIB, d.libraryName);
        }
        e.apply();
    }

    static void delete(Context c, Driver d) {
        deleteTree(new File(driversDir(c), d.slug));
        Driver sel = selected(c);
        if (sel != null && sel.slug.equals(d.slug)) select(c, null);  // was in use: back to the system driver
    }

    private static void deleteTree(File f) {
        if (f.isDirectory()) {
            File[] kids = f.listFiles();
            if (kids != null) for (File k : kids) deleteTree(k);
        }
        f.delete();
    }

    /**
     * Imports a driver .zip (from the file picker): validates meta.json (parseable), the libraryName .so present in
     * the zip, and minApi <= this device's SDK; then extracts just those two files to gpu_drivers/&lt;slug&gt;/.
     */
    static ImportResult importZip(Context c, Uri uri) {
        byte[] metaBytes = null;
        byte[] soBytes = null;
        String soName = null;
        try (InputStream in = c.getContentResolver().openInputStream(uri);
             ZipInputStream z = new ZipInputStream(in)) {
            ZipEntry e;
            while ((e = z.getNextEntry()) != null) {
                if (e.isDirectory()) continue;
                String n = base(e.getName());
                if (n == null) continue;  // nested paths are not a driver layout
                if (n.equals("meta.json") && metaBytes == null) metaBytes = readAll(z);
                // the .so name is only known after meta.json is parsed; keep reading
            }
        } catch (IOException ex) {
            return new ImportResult(null, "Não deu para ler o zip: " + ex.getMessage());
        }
        if (metaBytes == null)
            return new ImportResult(null, "O zip não tem meta.json");
        JSONObject meta;
        try {
            meta = new JSONObject(new String(metaBytes, "UTF-8"));
        } catch (Exception ex) {
            return new ImportResult(null, "meta.json não é um JSON válido");
        }
        String libraryName = meta.optString("libraryName", "");
        if (libraryName.isEmpty() || libraryName.contains("/"))
            return new ImportResult(null, "meta.json não tem libraryName utilizável");
        int minApi = meta.optInt("minApi", 0);
        if (minApi > Build.VERSION.SDK_INT)
            return new ImportResult(null, String.format(Locale.US,
                    "O driver precisa da API %d do Android; este aparelho é API %d",
                    minApi, Build.VERSION.SDK_INT));
        // Second pass: pull the .so (ZipInputStream cannot rewind).
        try (InputStream in = c.getContentResolver().openInputStream(uri);
             ZipInputStream z = new ZipInputStream(in)) {
            ZipEntry e;
            while ((e = z.getNextEntry()) != null) {
                if (e.isDirectory()) continue;
                String n = base(e.getName());
                if (libraryName.equals(n)) {
                    soBytes = readAll(z);
                    soName = n;
                    break;
                }
            }
        } catch (IOException ex) {
            return new ImportResult(null, "Não deu para ler o zip: " + ex.getMessage());
        }
        if (soBytes == null || soBytes.length < 1024)
            return new ImportResult(null, String.format(Locale.US,
                    "O zip não tem %s", libraryName));
        String name = meta.optString("name", libraryName);
        String slug = slug(name);
        File dir = new File(driversDir(c), slug);
        int dup = 1;
        while (dir.isDirectory()) dir = new File(driversDir(c), slug + "-" + (++dup));
        dir.mkdirs();
        try {
            write(new File(dir, "meta.json"), metaBytes);
            write(new File(dir, soName), soBytes);
        } catch (IOException ex) {
            deleteTree(dir);
            return new ImportResult(null, "Não deu para instalar o driver: " + ex.getMessage());
        }
        Driver d = read(dir);
        if (d == null) {
            deleteTree(dir);
            return new ImportResult(null, "O driver não pôde ser lido de volta");
        }
        Log.i("lasrecomp", "gpu driver imported: " + d.slug + " (" + d.libraryName + ")");
        return new ImportResult(d, null);
    }

    /**
     * First run: unpack the Turnip driver bundled in assets/ to gpu_drivers/ and pre-select it when nothing is
     * selected yet. Never replaces or re-selects: an import or a choice the player made wins.
     */
    static void installBundled(Context c) {
        byte[] metaBytes;
        try (InputStream in = c.getAssets().open(BUNDLED_ASSET);
             ZipInputStream z = new ZipInputStream(in)) {
            ZipEntry e;
            metaBytes = null;
            while ((e = z.getNextEntry()) != null) {
                if (!e.isDirectory() && "meta.json".equals(base(e.getName()))) {
                    metaBytes = readAll(z);
                    break;
                }
            }
        } catch (IOException ex) {
            Log.w("lasrecomp", "bundled driver: " + ex);
            return;
        }
        if (metaBytes == null) return;
        String libraryName;
        try {
            JSONObject meta = new JSONObject(new String(metaBytes, "UTF-8"));
            libraryName = meta.optString("libraryName", "");
        } catch (Exception ex) {
            return;
        }
        if (libraryName.isEmpty()) return;
        File dir = new File(driversDir(c), slug(BUNDLED_ASSET.replace(".zip", "")));
        if (!new File(dir, libraryName).isFile()) {
            ImportResult r = importAsset(c, BUNDLED_ASSET, dir);
            if (r.error != null) {
                Log.w("lasrecomp", "bundled driver: " + r.error);
                return;
            }
        }
        if (c.getSharedPreferences("letsgo", Context.MODE_PRIVATE).getString(KEY_DRIVER, null) == null) {
            Driver d = read(dir);
            if (d != null) {
                select(c, d);
                Log.i("lasrecomp", "gpu driver pre-selected: " + d.slug);
            }
        }
    }

    /** Unpacks an asset zip (meta.json + its libraryName .so) into dir. */
    private static ImportResult importAsset(Context c, String asset, File dir) {
        byte[] metaBytes = null;
        String libraryName = null;
        try (InputStream in = c.getAssets().open(asset);
             ZipInputStream z = new ZipInputStream(in)) {
            ZipEntry e;
            while ((e = z.getNextEntry()) != null) {
                if (!e.isDirectory() && "meta.json".equals(base(e.getName()))) {
                    metaBytes = readAll(z);
                    break;
                }
            }
        } catch (IOException ex) {
            return new ImportResult(null, ex.getMessage());
        }
        if (metaBytes == null) return new ImportResult(null, "no meta.json");
        try {
            libraryName = new JSONObject(new String(metaBytes, "UTF-8")).optString("libraryName", "");
        } catch (Exception ex) {
            return new ImportResult(null, "bad meta.json");
        }
        if (libraryName.isEmpty()) return new ImportResult(null, "no libraryName");
        byte[] soBytes = null;
        try (InputStream in = c.getAssets().open(asset);
             ZipInputStream z = new ZipInputStream(in)) {
            ZipEntry e;
            while ((e = z.getNextEntry()) != null) {
                if (!e.isDirectory() && libraryName.equals(base(e.getName()))) {
                    soBytes = readAll(z);
                    break;
                }
            }
        } catch (IOException ex) {
            return new ImportResult(null, ex.getMessage());
        }
        if (soBytes == null) return new ImportResult(null, "no " + libraryName);
        dir.mkdirs();
        try {
            write(new File(dir, "meta.json"), metaBytes);
            write(new File(dir, libraryName), soBytes);
        } catch (IOException ex) {
            deleteTree(dir);
            return new ImportResult(null, ex.getMessage());
        }
        return new ImportResult(read(dir), null);
    }

    /** Reads an installed driver folder; null when it is not a valid driver. */
    private static Driver read(File dir) {
        File meta = new File(dir, "meta.json");
        if (!meta.isFile()) return null;
        try (InputStream in = new java.io.FileInputStream(meta)) {
            byte[] b = readAll(in);
            JSONObject j = new JSONObject(new String(b, "UTF-8"));
            String libraryName = j.optString("libraryName", "");
            if (libraryName.isEmpty() || !new File(dir, libraryName).isFile()) return null;
            return new Driver(dir.getName(), j.optString("name", dir.getName()),
                    j.optString("description", ""), j.optString("driverVersion", ""), libraryName);
        } catch (Exception e) {
            return null;
        }
    }

    /** Simple file name of a zip entry; null for nested paths (not a driver layout). */
    private static String base(String entry) {
        if (entry == null || entry.isEmpty() || entry.endsWith("/")) return null;
        int s = entry.lastIndexOf('/');
        String n = s < 0 ? entry : entry.substring(s + 1);
        return n.isEmpty() || n.startsWith(".") ? null : n;
    }

    /** Folder-safe slug from a display name. */
    private static String slug(String name) {
        String s = name.toLowerCase(Locale.US).replaceAll("[^a-z0-9]+", "-").replaceAll("^-+|-+$", "");
        return s.isEmpty() ? "driver" : (s.length() > 40 ? s.substring(0, 40) : s);
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        byte[] b = new byte[65536];
        int r;
        while ((r = in.read(b)) > 0) o.write(b, 0, r);
        return o.toByteArray();
    }

    private static void write(File f, byte[] b) throws IOException {
        File tmp = new File(f.getParentFile(), f.getName() + ".part");
        try (OutputStream o = new FileOutputStream(tmp)) {
            o.write(b);
        }
        if (!tmp.renameTo(f)) {
            tmp.delete();
            throw new IOException(f.getName());
        }
    }
}
