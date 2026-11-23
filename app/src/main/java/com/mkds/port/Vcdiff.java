package com.mkds.port;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;

/**
 * Applies a VCDIFF patch (RFC 3284), as made by xdelta3: the default code table, xdelta3's secondary compression of
 * the sections with LZMA (an .xz stream after the section's decompressed size) or none. ROM hacks are distributed as
 * such patches (.xdelta) against the original ROM.
 */
final class Vcdiff {
    interface Progress {
        void at(long done, long total);
    }

    private Vcdiff() {}

    /** Writes source patched with patch to out. Throws IOException with a message for the user. */
    static void apply(RandomAccessFile source, InputStream patchStream, long patchSize, RandomAccessFile out,
                      Progress progress) throws IOException {
        DataInputStream in = new DataInputStream(new java.io.BufferedInputStream(patchStream, 1 << 16));
        int[] magic = {in.readUnsignedByte(), in.readUnsignedByte(), in.readUnsignedByte(), in.readUnsignedByte()};
        if (magic[0] != 0xd6 || magic[1] != 0xc3 || magic[2] != 0xc4) throw new IOException("not an xdelta patch");
        int hdr = in.readUnsignedByte();
        int secondary = 0;
        if ((hdr & 1) != 0) secondary = in.readUnsignedByte();
        if ((hdr & 2) != 0) throw new IOException("patches with their own code table are not supported");
        if ((hdr & 4) != 0) skip(in, varint(in));
        out.setLength(0);
        long read = 0;
        byte[] t = new byte[0];
        int[] near = new int[4];
        int[] same = new int[3 * 256];
        while (true) {
            int wi;
            try {
                wi = in.readUnsignedByte();
            } catch (EOFException e) {
                break;
            }
            long ssz = 0, spos = 0;
            if ((wi & 3) != 0) {
                ssz = varint(in);
                spos = varint(in);
            }
            varint(in); // delta encoding length
            int tlen = (int) varint(in);
            int di = in.readUnsignedByte();
            int dl = (int) varint(in), il = (int) varint(in), al = (int) varint(in);
            if ((wi & 4) != 0) skip(in, 4); // Adler-32 of the window
            byte[] data = section(in, dl, (di & 1) != 0, secondary);
            byte[] inst = section(in, il, (di & 2) != 0, secondary);
            byte[] addr = section(in, al, (di & 4) != 0, secondary);
            read += dl + il + al;
            // The source segment: from the source file, or from what has been written (VCD_TARGET).
            byte[] seg = new byte[(int) ssz];
            if (ssz > 0) {
                RandomAccessFile from = (wi & 1) != 0 ? source : out;
                long keep = out.getFilePointer();
                from.seek(spos);
                from.readFully(seg);
                out.seek(keep);
            }
            if (t.length < tlen) t = new byte[tlen];
            int tp = 0, dp = 0;
            int[] ip = {0}, ap = {0};
            int ni = 0;
            java.util.Arrays.fill(near, 0);
            java.util.Arrays.fill(same, 0);
            while (ip[0] < inst.length) {
                int code = inst[ip[0]++] & 0xff;
                for (int half = 0; half < 2; half++) {
                    int typ = TYPE[half][code], size = SIZE[half][code], mode = MODE[half][code];
                    if (typ == 0) continue;
                    if (size == 0) size = (int) varint(inst, ip);
                    if (typ == 1) { // ADD
                        System.arraycopy(data, dp, t, tp, size);
                        dp += size;
                        tp += size;
                    } else if (typ == 3) { // RUN
                        java.util.Arrays.fill(t, tp, tp + size, data[dp++]);
                        tp += size;
                    } else { // COPY
                        int here = (int) ssz + tp, a;
                        if (mode == 0) a = (int) varint(addr, ap);
                        else if (mode == 1) a = here - (int) varint(addr, ap);
                        else if (mode < 6) a = near[mode - 2] + (int) varint(addr, ap);
                        else a = same[(mode - 6) * 256 + (addr[ap[0]++] & 0xff)];
                        near[ni] = a;
                        ni = (ni + 1) & 3;
                        same[a % 768] = a;
                        for (int k = 0; k < size; k++) {
                            int q = a + k;
                            t[tp++] = q < ssz ? seg[q] : t[q - (int) ssz];
                        }
                    }
                }
            }
            if (tp != tlen) throw new IOException("the patch is damaged");
            out.write(t, 0, tlen);
            if (progress != null) progress.at(read, patchSize);
        }
    }

    private static byte[] section(DataInputStream in, int len, boolean compressed, int secondary) throws IOException {
        byte[] raw = new byte[len];
        in.readFully(raw);
        if (!compressed) return raw;
        if (secondary != 2) throw new IOException("this patch's compression is not supported (use LZMA or none)");
        int[] p = {0};
        int size = (int) varint(raw, p);
        byte[] outb = new byte[size];
        try (InputStream xz = new org.tukaani.xz.XZInputStream(new ByteArrayInputStream(raw, p[0], len - p[0]))) {
            int done = 0;
            while (done < size) {
                int n = xz.read(outb, done, size - done);
                if (n < 0) throw new IOException("the patch is damaged");
                done += n;
            }
        }
        return outb;
    }

    private static long varint(DataInputStream in) throws IOException {
        long v = 0;
        while (true) {
            int c = in.readUnsignedByte();
            v = (v << 7) | (c & 0x7f);
            if ((c & 0x80) == 0) return v;
        }
    }

    private static long varint(byte[] b, int[] p) {
        long v = 0;
        while (true) {
            int c = b[p[0]++] & 0xff;
            v = (v << 7) | (c & 0x7f);
            if ((c & 0x80) == 0) return v;
        }
    }

    private static void skip(DataInputStream in, long n) throws IOException {
        while (n > 0) {
            long k = in.skip(n);
            if (k <= 0) {
                in.readByte();
                k = 1;
            }
            n -= k;
        }
    }

    // The default code table: for each code, two instructions (type 0 none, 1 ADD, 2 COPY, 3 RUN), size, mode.
    private static final int[][] TYPE = new int[2][256], SIZE = new int[2][256], MODE = new int[2][256];

    static {
        int i = 0;
        set(i++, 3, 0, 0, 0, 0, 0);
        for (int sz = 0; sz < 18; sz++) set(i++, 1, sz, 0, 0, 0, 0);
        for (int m = 0; m < 9; m++) {
            set(i++, 2, 0, m, 0, 0, 0);
            for (int sz = 4; sz <= 18; sz++) set(i++, 2, sz, m, 0, 0, 0);
        }
        for (int m = 0; m < 6; m++) {
            for (int asz = 1; asz <= 4; asz++) {
                for (int csz = 4; csz <= 6; csz++) set(i++, 1, asz, 0, 2, csz, m);
            }
        }
        for (int m = 6; m < 9; m++) {
            for (int asz = 1; asz <= 4; asz++) set(i++, 1, asz, 0, 2, 4, m);
        }
        for (int m = 0; m < 9; m++) set(i++, 2, 4, m, 1, 1, 0);
    }

    private static void set(int code, int t1, int s1, int m1, int t2, int s2, int m2) {
        TYPE[0][code] = t1;
        SIZE[0][code] = s1;
        MODE[0][code] = m1;
        TYPE[1][code] = t2;
        SIZE[1][code] = s2;
        MODE[1][code] = m2;
    }
}
