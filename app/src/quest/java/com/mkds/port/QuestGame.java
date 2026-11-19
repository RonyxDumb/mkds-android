package com.mkds.port;

import android.content.Context;

import java.io.File;
import java.io.RandomAccessFile;

/** The game process, started once for the app (by the panel or by the VR activity, whichever comes first). */
final class QuestGame {
    private static boolean sStarted;

    static File romFile(Context c) {
        return new File(c.getFilesDir(), "rom.nds");
    }

    static boolean hasRom(Context c) {
        File f = romFile(c);
        if (!f.isFile() || f.length() < 0x200) return false;
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
            byte[] code = new byte[4];
            r.seek(0x0c);
            r.readFully(code);
            return new String(code, "US-ASCII").equals("AMCP");
        } catch (Exception e) {
            return false;
        }
    }

    private static boolean sInit;

    /** The log and the crash handler, once: a new log.txt. */
    static void init(Context c) {
        if (sInit) return;
        sInit = true;
        Native.init(c.getFilesDir().getAbsolutePath());
    }

    static boolean started() {
        return sStarted;
    }

    // The headset's language among the game's (English otherwise).
    private static int gameLanguage() {
        switch (java.util.Locale.getDefault().getLanguage()) {
            case "fr": return 2;
            case "de": return 3;
            case "it": return 4;
            case "es": return 5;
            default: return 1;
        }
    }

    /** Starts the game if it is not running. Returns an error message, or null. */
    static String start(Context c, OnlineUi online) {
        if (sStarted) return null;
        if (!hasRom(c)) return "No ROM has been chosen yet.";
        if (!sInit) init(c);
        Native.setLanguage(gameLanguage());
        online.applyVolume();
        String err = Native.start(romFile(c).getAbsolutePath(), new File(c.getFilesDir(), "mkds.sav").getAbsolutePath());
        if (err != null) return err;
        sStarted = true;
        online.applyName();
        return null;
    }

    private QuestGame() {}
}
