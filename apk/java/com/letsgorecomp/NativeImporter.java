package com.letsgorecomp;

/**
 * The on-device NSP importer (importer/, liblgimport.so): the player's own prod.keys and the game's NSP
 * -> extracted/exefs/ + extracted/romfs(.bin) in the game data folder. Streams, checks every block against
 * the NCA's own hashes, resumes where it stopped, never logs key values. The JNI names are fixed by this class.
 */
final class NativeImporter {
    /**
     * Auto-detect: "0" lets the importer use the single game found in the folder
     * (works for Eevee/Pikachu, any region — like the Zelda importer's fallback).
     */
    static final String TITLE_ID = "0";

    interface Callback {
        void progress(String phase, long done, long total, String item);

        void log(String line);
    }

    private static boolean loaded;
    private static String loadError;

    static synchronized boolean load() {
        if (!loaded && loadError == null) {
            try {
                System.loadLibrary("lgimport");
                loaded = true;
            } catch (Throwable t) {
                loadError = String.valueOf(t);
            }
        }
        return loaded;
    }

    static String loadError() {
        return loadError;
    }

    /** 0 on success, else see lastError(); 4 = cancelled. The fds stay open (the caller closes them). */
    static native int run(String outDir, String[] names, int[] fds, String titleId, boolean verify, Callback cb);

    /** Same, but imports to the loose folder (no romfs.bin build; the runtime builds it on first boot). */
    static native int runFolder(String outDir, String[] names, int[] fds, String titleId, boolean verify, Callback cb);

    static native void cancel();

    static native String lastError();

    private NativeImporter() {
    }
}
