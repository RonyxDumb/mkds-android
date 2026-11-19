package com.mkds.port;

/**
 * Voice chat packets: 20 ms of speech (16 kHz mono) as IMA ADPCM, 4 bits a sample.
 *
 * A packet stands alone (it carries the decoder's state), so a lost one costs 20 ms and nothing after it.
 * Layout, little endian: "MKDV", u32 speaker, u16 sequence, s16 predictor, u8 step index, u8 0, 160 bytes of samples.
 */
final class VoiceCodec {
    static final int RATE = 16000, FRAME = 320, HEADER = 14, PACKET = HEADER + FRAME / 2;
    static final int MAGIC = 0x56444b4d; // "MKDV"

    private static final int[] INDEX = {-1, -1, -1, -1, 2, 4, 6, 8};
    private static final int[] STEP = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50,
            55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
            494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749,
            3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
            15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

    // The encoder's state runs on from packet to packet.
    private int mPredictor, mIndex;

    /** Encodes FRAME samples into out (PACKET bytes). */
    void encode(short[] pcm, int speaker, int sequence, byte[] out) {
        put32(out, 0, MAGIC);
        put32(out, 4, speaker);
        out[8] = (byte) sequence;
        out[9] = (byte) (sequence >> 8);
        out[10] = (byte) mPredictor;
        out[11] = (byte) (mPredictor >> 8);
        out[12] = (byte) mIndex;
        out[13] = 0;
        int predictor = mPredictor, index = mIndex;
        for (int i = 0; i < FRAME; i++) {
            int step = STEP[index], diff = pcm[i] - predictor, code = 0;
            if (diff < 0) {
                code = 8;
                diff = -diff;
            }
            int delta = step >> 3;
            if (diff >= step) {
                code |= 4;
                diff -= step;
                delta += step;
            }
            if (diff >= step >> 1) {
                code |= 2;
                diff -= step >> 1;
                delta += step >> 1;
            }
            if (diff >= step >> 2) {
                code |= 1;
                delta += step >> 2;
            }
            predictor = clamp16((code & 8) != 0 ? predictor - delta : predictor + delta);
            index = Math.max(0, Math.min(88, index + INDEX[code & 7]));
            if ((i & 1) == 0) out[HEADER + i / 2] = (byte) code;
            else out[HEADER + i / 2] |= (byte) (code << 4);
        }
        mPredictor = predictor;
        mIndex = index;
    }

    /** Whether data (length bytes from offset) is a voice packet. */
    static boolean valid(byte[] data, int offset, int length) {
        return length == PACKET && get32(data, offset) == MAGIC && (data[offset + 12] & 0xff) <= 88;
    }

    static int speaker(byte[] data, int offset) {
        return get32(data, offset + 4);
    }

    static int sequence(byte[] data, int offset) {
        return (data[offset + 8] & 0xff) | (data[offset + 9] & 0xff) << 8;
    }

    /** Decodes a valid packet into FRAME samples. */
    static void decode(byte[] data, int offset, short[] pcm) {
        int predictor = (short) ((data[offset + 10] & 0xff) | data[offset + 11] << 8), index = data[offset + 12] & 0xff;
        for (int i = 0; i < FRAME; i++) {
            int b = data[offset + HEADER + i / 2], code = ((i & 1) == 0 ? b : b >> 4) & 15, step = STEP[index];
            int delta = step >> 3;
            if ((code & 4) != 0) delta += step;
            if ((code & 2) != 0) delta += step >> 1;
            if ((code & 1) != 0) delta += step >> 2;
            predictor = clamp16((code & 8) != 0 ? predictor - delta : predictor + delta);
            index = Math.max(0, Math.min(88, index + INDEX[code & 7]));
            pcm[i] = (short) predictor;
        }
    }

    /** FNV-1a, as the game's room hash (online_room_hash). */
    static int roomHash(String room) {
        int hash = 0x811c9dc5;
        for (byte b : room.getBytes(java.nio.charset.StandardCharsets.UTF_8)) hash = (hash ^ (b & 0xff)) * 16777619;
        return hash;
    }

    /** The relay server's header (port/platform/hostside/online.h) for a hello (0) or a broadcast (2) to a room. */
    static void relayHeader(byte[] out, int type, int room) {
        put32(out, 0, 0x52444b4d);
        out[4] = 1;
        out[5] = (byte) type;
        out[6] = out[7] = 0;
        put32(out, 8, 0);
        put32(out, 12, room);
    }

    static void put32(byte[] b, int at, int v) {
        b[at] = (byte) v;
        b[at + 1] = (byte) (v >> 8);
        b[at + 2] = (byte) (v >> 16);
        b[at + 3] = (byte) (v >> 24);
    }

    static int get32(byte[] b, int at) {
        return (b[at] & 0xff) | (b[at + 1] & 0xff) << 8 | (b[at + 2] & 0xff) << 16 | b[at + 3] << 24;
    }

    private static int clamp16(int v) {
        return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
}
