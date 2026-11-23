package com.mkds.port;

import android.content.Context;
import android.net.Uri;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Mods: ROM hacks of Mario Kart DS (Europe), added as an xdelta patch or as a patched ROM.
 *
 * The game's code here is the port's own (the decompilation compiled natively), not the ROM's, so a mod's files are
 * what it brings: each file of the original ROM that the mod changes (graphics, models, courses, music, texts) is
 * taken from the mod, the rest from the original ROM. Files a mod adds and code it changes are not used: new courses,
 * cups or menus that need the mod's own code do not appear. When the mod changes the game's code, its sound archive
 * is left out too (its sounds go with its code; the game's sound IDs would not match).
 *
 * The game runs from files/mod.nds, made from the original ROM and the active mod when it is chosen; each mod keeps
 * its own save file.
 */
final class Mods {
    static final String PREF = "mod"; // the active mod's name, "" for none

    interface Progress {
        void at(String what, int percent);
    }

    private Mods() {}

    static File dir(Context c) {
        File d = new File(c.getFilesDir(), "mods");
        d.mkdirs();
        return d;
    }

    static File rom(Context c, String name) {
        return new File(dir(c), name + ".nds");
    }

    static File merged(Context c) {
        return new File(c.getFilesDir(), "mod.nds");
    }

    static String active(Context c) {
        String name = OptionsScreen.prefs(c).getString(PREF, "");
        return rom(c, name).isFile() ? name : "";
    }

    /** The installed mods, by name. */
    static List<String> list(Context c) {
        List<String> names = new ArrayList<>();
        File[] files = dir(c).listFiles();
        if (files == null) return names;
        for (File f : files) {
            if (f.getName().endsWith(".nds")) names.add(f.getName().substring(0, f.getName().length() - 4));
        }
        java.util.Collections.sort(names, String.CASE_INSENSITIVE_ORDER);
        return names;
    }

    static void remove(Context c, String name) {
        rom(c, name).delete();
        if (name.equals(OptionsScreen.prefs(c).getString(PREF, ""))) setActive(c, "");
    }

    static void setActive(Context c, String name) {
        OptionsScreen.prefs(c).edit().putString(PREF, name).apply();
        merged(c).delete(); // made again at the next start
    }

    /** A mod's name from its file name: letters, digits, spaces and - _ . only. */
    static String nameFor(String fileName) {
        String n = fileName.replaceAll("\\.(xdelta|vcdiff|delta|nds|zip)$", "");
        n = n.replaceAll("[^A-Za-z0-9 ._-]", "_").trim();
        return n.isEmpty() ? "Mod" : n;
    }

    /**
     * Adds a mod from an xdelta patch (applied to the original ROM) or a patched ROM. Runs for a while: call it from
     * a background thread. Returns a message for the user.
     */
    static String add(Context c, Uri uri, String name, File baseRom, Progress progress) throws IOException {
        File tmp = new File(dir(c), name + ".tmp");
        File dst = rom(c, name);
        byte[] head = new byte[4];
        try (InputStream in = c.getContentResolver().openInputStream(uri)) {
            if (in == null || in.read(head) != 4) throw new IOException("cannot read the file");
        }
        long size = -1;
        try (android.content.res.AssetFileDescriptor fd = c.getContentResolver().openAssetFileDescriptor(uri, "r")) {
            if (fd != null) size = fd.getLength();
        } catch (Exception ignored) {
        }
        try {
            if ((head[0] & 0xff) == 0xd6 && (head[1] & 0xff) == 0xc3 && (head[2] & 0xff) == 0xc4) {
                try (InputStream in = c.getContentResolver().openInputStream(uri);
                     RandomAccessFile src = new RandomAccessFile(baseRom, "r");
                     RandomAccessFile out = new RandomAccessFile(tmp, "rw")) {
                    final long total = size;
                    Vcdiff.apply(src, in, size, out, (done, all) -> {
                        if (progress != null && total > 0) progress.at("Applying the patch", (int) (done * 100 / total));
                    });
                }
            } else {
                try (InputStream in = c.getContentResolver().openInputStream(uri);
                     OutputStream out = new FileOutputStream(tmp)) {
                    byte[] buf = new byte[1 << 16];
                    int n;
                    long done = 0;
                    while ((n = in.read(buf)) > 0) {
                        out.write(buf, 0, n);
                        done += n;
                        if (progress != null && size > 0) progress.at("Copying", (int) (done * 100 / size));
                    }
                }
            }
            String code = gameCode(tmp);
            if (!"AMCP".equals(code)) {
                throw new IOException(code == null ? "the result is not a DS ROM"
                        : "this mod is for another version of the game (" + code + "), not the European one (AMCP)");
            }
            if (dst.exists()) dst.delete();
            if (!tmp.renameTo(dst)) throw new IOException("cannot store the mod");
        } finally {
            tmp.delete();
        }
        return describe(c, name, baseRom);
    }

    /** What the port takes from a mod, for the user. */
    static String describe(Context c, String name, File baseRom) {
        try (RandomAccessFile base = new RandomAccessFile(baseRom, "r");
             RandomAccessFile mod = new RandomAccessFile(rom(c, name), "r")) {
            Fs b = new Fs(base), m = new Fs(mod);
            int changed = 0, added = 0;
            for (Map.Entry<String, Integer> e : m.files.entrySet()) {
                Integer id = b.files.get(e.getKey());
                if (id == null) added++;
                else if (!sameFile(base, b, id, mod, m, e.getValue())) changed++;
            }
            String text = changed + " files of the game changed";
            if (added > 0) text += ", " + added + " new files (not used)";
            if (codeChanged(base, mod)) {
                text += ". This mod changes the game's code: only its graphics, models, courses that take the place of "
                        + "the original ones and texts are used; its new courses, cups and menus and its sounds are "
                        + "not.";
            }
            return text;
        } catch (IOException e) {
            return "";
        }
    }

    /**
     * The ROM the game runs from: the original, or files/mod.nds made from it and the active mod (made now when it
     * is missing). Runs for a few seconds the first time: call it from a background thread.
     */
    static File romForGame(Context c, File baseRom) {
        String name = active(c);
        if (name.isEmpty()) return baseRom;
        File out = merged(c);
        File mod = rom(c, name);
        if (out.isFile() && out.lastModified() >= mod.lastModified() && out.lastModified() >= baseRom.lastModified()) {
            return out;
        }
        try {
            merge(baseRom, mod, out);
            return out;
        } catch (IOException e) {
            out.delete();
            return baseRom;
        }
    }

    /** The save file of the active mod (each mod has its own), or null for the game's own. */
    static String saveSuffix(Context c) {
        String name = active(c);
        return name.isEmpty() ? null : name;
    }

    // ------------------------------------------------------------------------------------------ merging
    // The original ROM with, for each of its files that the mod has at the same path, the mod's content (appended,
    // its FAT entry pointing there). The original's file table and IDs stay: the game's code finds its files.
    static void merge(File baseRom, File modRom, File out) throws IOException {
        File tmp = new File(out.getPath() + ".tmp");
        copy(baseRom, tmp);
        try (RandomAccessFile base = new RandomAccessFile(baseRom, "r");
             RandomAccessFile mod = new RandomAccessFile(modRom, "r");
             RandomAccessFile dst = new RandomAccessFile(tmp, "rw")) {
            Fs b = new Fs(base), m = new Fs(mod);
            boolean code = codeChanged(base, mod);
            long end = dst.length();
            for (Map.Entry<String, Integer> e : b.files.entrySet()) {
                Integer mid = m.files.get(e.getKey());
                if (mid == null) continue;
                if (code && e.getKey().endsWith(".sdat")) continue; // its sounds go with its code
                if (sameFile(base, b, e.getValue(), mod, m, mid)) continue;
                long[] r = m.range(mod, mid);
                byte[] data = new byte[(int) (r[1] - r[0])];
                mod.seek(r[0]);
                mod.readFully(data);
                end = (end + 0x1ff) & ~0x1ffL;
                dst.seek(end);
                dst.write(data);
                dst.seek(b.fat + e.getValue() * 8L);
                dst.write(le32(end));
                dst.write(le32(end + data.length));
                end += data.length;
            }
        }
        if (out.exists()) out.delete();
        if (!tmp.renameTo(out)) throw new IOException("cannot store the ROM");
    }

    private static boolean sameFile(RandomAccessFile a, Fs fa, int ida, RandomAccessFile b, Fs fb, int idb)
            throws IOException {
        long[] ra = fa.range(a, ida), rb = fb.range(b, idb);
        long len = ra[1] - ra[0];
        if (len != rb[1] - rb[0]) return false;
        byte[] x = new byte[1 << 16], y = new byte[1 << 16];
        for (long done = 0; done < len; done += x.length) {
            int n = (int) Math.min(x.length, len - done);
            a.seek(ra[0] + done);
            a.readFully(x, 0, n);
            b.seek(rb[0] + done);
            b.readFully(y, 0, n);
            for (int i = 0; i < n; i++) {
                if (x[i] != y[i]) return false;
            }
        }
        return true;
    }

    // Whether the mod's ARM9 program (with its overlays' table) differs from the original's.
    private static boolean codeChanged(RandomAccessFile base, RandomAccessFile mod) throws IOException {
        return u32(base, 0x2c) != u32(mod, 0x2c) || u32(base, 0x54) != u32(mod, 0x54)
                || !sameRange(base, u32(base, 0x20), mod, u32(mod, 0x20), u32(base, 0x2c));
    }

    private static boolean sameRange(RandomAccessFile a, long pa, RandomAccessFile b, long pb, long len)
            throws IOException {
        byte[] x = new byte[1 << 16], y = new byte[1 << 16];
        for (long done = 0; done < len; done += x.length) {
            int n = (int) Math.min(x.length, len - done);
            a.seek(pa + done);
            a.readFully(x, 0, n);
            b.seek(pb + done);
            b.readFully(y, 0, n);
            if (!java.util.Arrays.equals(x, y)) return false;
        }
        return true;
    }

    /** The file system of a ROM: path -> file ID, and where its FAT is. */
    private static final class Fs {
        final Map<String, Integer> files = new HashMap<>();
        final long fat, fnt;

        Fs(RandomAccessFile r) throws IOException {
            fnt = u32(r, 0x40);
            fat = u32(r, 0x48);
            walk(r, 0xf000, "");
        }

        long[] range(RandomAccessFile r, int id) throws IOException {
            return new long[] {u32(r, fat + id * 8L), u32(r, fat + id * 8L + 4)};
        }

        private void walk(RandomAccessFile r, int dir, String prefix) throws IOException {
            if (prefix.length() > 512) return;
            long entry = fnt + (dir & 0xfff) * 8L;
            long p = fnt + u32(r, entry);
            int id = u16(r, entry + 4);
            while (true) {
                r.seek(p);
                int len = r.readUnsignedByte();
                p++;
                if (len == 0) break;
                byte[] name = new byte[len & 0x7f];
                r.readFully(name);
                p += name.length;
                String n = new String(name, StandardCharsets.ISO_8859_1);
                if ((len & 0x80) != 0) {
                    int sub = u16(r, p);
                    p += 2;
                    walk(r, sub, prefix + n + "/");
                } else {
                    files.put(prefix + n, id++);
                }
            }
        }
    }

    private static String gameCode(File f) {
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
            if (r.length() < 0x200) return null;
            byte[] code = new byte[4];
            r.seek(0x0c);
            r.readFully(code);
            return new String(code, StandardCharsets.US_ASCII);
        } catch (IOException e) {
            return null;
        }
    }

    private static long u32(RandomAccessFile r, long at) throws IOException {
        r.seek(at);
        int v = r.readInt();
        return Integer.toUnsignedLong(Integer.reverseBytes(v));
    }

    private static int u16(RandomAccessFile r, long at) throws IOException {
        r.seek(at);
        return Short.toUnsignedInt(Short.reverseBytes(r.readShort()));
    }

    private static byte[] le32(long v) {
        return new byte[] {(byte) v, (byte) (v >> 8), (byte) (v >> 16), (byte) (v >> 24)};
    }

    private static void copy(File from, File to) throws IOException {
        try (java.io.FileInputStream in = new java.io.FileInputStream(from);
             java.io.FileOutputStream out = new java.io.FileOutputStream(to)) {
            in.getChannel().transferTo(0, in.getChannel().size(), out.getChannel());
        }
    }
}
