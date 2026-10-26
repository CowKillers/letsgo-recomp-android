package com.letsgorecomp;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * One NSP import at a time, on its own thread; the launcher polls the fields.
 * The player picks a folder (SAF) containing prod.keys and the game's .nsp.
 * NativeImporter writes extracted/exefs/ + extracted/romfs(.bin) into the game data folder.
 */
final class LetsGoImportJob {
    private static final String TAG = "LetsGo";
    private static volatile LetsGoImportJob current;

    volatile boolean finished, cancelled;
    volatile long done, total;
    volatile String error;
    volatile String phase = "";
    volatile String currentName = "";
    volatile double seconds;
    volatile boolean keysOk, containerOk;
    volatile String containerName = "";
    final AtomicBoolean cancel = new AtomicBoolean();
    private final long startedAt = System.nanoTime();

    static LetsGoImportJob current() {
        return current;
    }

    void requestCancel() {
        cancel.set(true);
        if (NativeImporter.load()) NativeImporter.cancel();
    }

    private static synchronized LetsGoImportJob begin() {
        if (current != null && !current.finished) return null;
        LetsGoImportJob j = new LetsGoImportJob();
        current = j;
        return j;
    }

    static LetsGoImportJob start(Context context, Uri tree, File dir) {
        return start(context, tree, dir, false);
    }

    /** Folder mode: import the NSP to the loose folder (extracted/romfs + extracted/exefs, no .bin). */
    static LetsGoImportJob start(Context context, Uri tree, File dir, boolean folder) {
        final LetsGoImportJob j = begin();
        if (j == null) return current;
        final Context app = context.getApplicationContext();
        final Uri t = tree;
        final File d = dir;
        final boolean f = folder;
        new Thread(new Runnable() {
            @Override
            public void run() {
                j.work(app, t, d, f);
            }
        }, "letsgo-import").start();
        return j;
    }

    private void finish() {
        seconds = (System.nanoTime() - startedAt) / 1e9;
        finished = true;
    }

    private static final class Doc {
        String name;
        Uri uri;
    }

    static boolean wanted(String name) {
        String low = name == null ? "" : name.toLowerCase(Locale.ROOT);
        return low.endsWith(".nsp") || low.equals("prod.keys") || low.equals("title.keys") || low.equals("keys.txt");
    }

    static boolean isKeys(String low) {
        return low.equals("prod.keys") || low.equals("title.keys") || low.equals("keys.txt");
    }

    private static void list(ContentResolver cr, Uri tree, String docId, int depth, List<Doc> out) {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        String[] cols = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE};
        try (Cursor c = cr.query(children, cols, null, null, null)) {
            if (c == null) return;
            while (c.moveToNext()) {
                String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    if (depth < 3) list(cr, tree, id, depth + 1, out);
                } else if (wanted(name)) {
                    Doc d = new Doc();
                    d.name = name;
                    d.uri = DocumentsContract.buildDocumentUriUsingTree(tree, id);
                    out.add(d);
                }
            }
        }
    }

    private void work(Context context, Uri tree, File dir, boolean folder) {
        ContentResolver cr = context.getContentResolver();
        List<ParcelFileDescriptor> open = new ArrayList<>();
        try {
            phase = "scan";
            if (!NativeImporter.load()) throw new IOException("liblgimport: " + NativeImporter.loadError());
            if (!dir.isDirectory() && !dir.mkdirs())
                throw new IOException("Não foi possível criar a pasta " + dir);
            List<Doc> docs = new ArrayList<>();
            list(cr, tree, DocumentsContract.getTreeDocumentId(tree), 0, docs);
            boolean keys = false;
            int containers = 0;
            for (Doc d : docs) {
                String low = d.name.toLowerCase(Locale.ROOT);
                keys |= isKeys(low);
                if (low.endsWith(".nsp")) {
                    containers++;
                    if (containerName.isEmpty()) containerName = d.name;
                }
            }
            if (!keys)
                throw new IOException("prod.keys não foi encontrado na pasta escolhida (nem nas subpastas).");
            keysOk = true;
            if (containers == 0)
                throw new IOException("Nenhum arquivo .nsp na pasta escolhida (nem nas subpastas).");
            String[] names = new String[docs.size()];
            int[] fds = new int[docs.size()];
            for (int i = 0; i < docs.size(); i++) {
                ParcelFileDescriptor pfd = cr.openFileDescriptor(docs.get(i).uri, "r");
                if (pfd == null) throw new IOException("Não foi possível abrir " + docs.get(i).name);
                open.add(pfd);
                names[i] = docs.get(i).name;
                fds[i] = pfd.getFd();
            }
            NativeImporter.Callback importCb = new NativeImporter.Callback() {
                @Override
                public void progress(String ph, long d, long t, String item) {
                    if (!"scan".equals(ph)) containerOk = true;
                    phase = ph;
                    done = d;
                    total = t;
                    currentName = item;
                    if (cancel.get()) NativeImporter.cancel();
                }

                @Override
                public void log(String line) {
                    Log.i(TAG, "import: " + line);
                }
            };
            int rc = folder
                    ? NativeImporter.runFolder(dir.getAbsolutePath(), names, fds,
                            NativeImporter.TITLE_ID, false, importCb)
                    : NativeImporter.run(dir.getAbsolutePath(), names, fds,
                            NativeImporter.TITLE_ID, false, importCb);
            if (rc == 4) {
                cancelled = true;
                throw new IOException("Importação cancelada. O que já foi gravado fica; a próxima continua dali.");
            }
            if (rc != 0) throw new IOException(NativeImporter.lastError());
            // Post-import verification (like Zelda's validated flow): the runtime
            // FATALs if extracted/exefs/main is missing or not a valid NSO.
            // Check it here so a broken import never marks itself complete.
            java.io.File exefsMain = new java.io.File(dir, "extracted/exefs/main");
            if (!exefsMain.isFile()) {
                throw new IOException("Importação terminou mas extracted/exefs/main não foi criado. " +
                        "O NSP pode estar incompleto ou as chaves incorretas.");
            }
            try {
                java.io.RandomAccessFile raf = new java.io.RandomAccessFile(exefsMain, "r");
                byte[] magic = new byte[4];
                raf.readFully(magic);
                raf.close();
                if (magic[0] != 'N' || magic[1] != 'S' || magic[2] != 'O' || magic[3] != '0') {
                    throw new IOException("extracted/exefs/main extraído não é um NSO válido (arquivo corrompido). " +
                            "Tente importar novamente ou verifique o NSP.");
                }
            } catch (java.io.IOException e) {
                throw new IOException("Não foi possível verificar extracted/exefs/main: " + e.getMessage());
            }
            // Import succeeded: remember this folder as the game dir so the game
            // always reads from where the import wrote (they can never mismatch).
            context.getSharedPreferences("letsgo", android.content.Context.MODE_PRIVATE)
                .edit().putString("game_dir", dir.getAbsolutePath()).apply();
            // Mark the import as 100% complete: a bare romfs/ folder is not
            // proof of completion (an interrupted import also leaves one behind).
            // The launcher only offers Jogar when this marker (or romfs.bin) exists.
            try {
                new java.io.File(dir, "extracted/.import_ok").createNewFile();
            } catch (java.io.IOException ignored) {}
            phase = "done";
        } catch (IOException e) {
            error = e.getMessage();
        } catch (SecurityException e) {
            error = "Sem permissão para ler a pasta: " + e.getMessage();
        } catch (RuntimeException e) {
            error = "Erro inesperado: " + e;
        } finally {
            for (ParcelFileDescriptor p : open) {
                try {
                    p.close();
                } catch (IOException ignored) {
                }
            }
            finish();
        }
    }
}
