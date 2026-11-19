package com.mkds.port;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.AudioTrack;
import android.media.MediaRecorder;
import android.media.audiofx.AcousticEchoCanceler;
import android.media.audiofx.NoiseSuppressor;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.Iterator;

/**
 * Voice chat between the players of an online room.
 *
 * Runs beside the game, in the app: the microphone's speech goes to the relay server (tools/relay_server.py) as a
 * broadcast to a room of its own, "<room>/voice", next to the game's room, and the server hands it to the others
 * there. Only speech is sent (silence is not), 20 ms a packet (VoiceCodec). What arrives is kept a few packets deep
 * per speaker, to even out the network, and mixed into one output.
 */
final class VoiceChat {
    private static final int RELAY_HEADER = 16, RELAY_HELLO = 0, RELAY_BROADCAST = 2;
    private static final int HELLO_MS = 2000;
    private static final int START_DEPTH = 3, MAX_DEPTH = 12; // packets kept per speaker: before playing; at most
    private static final int SPEAKER_IDLE_MS = 10000;
    // Speech detection: louder than this (RMS of 16-bit samples) opens the microphone, which then stays open a moment.
    private static final double SPEECH_RMS = 350;
    private static final int HANG_FRAMES = 20;

    private static final class Speaker {
        final ArrayDeque<short[]> frames = new ArrayDeque<>();
        boolean waiting = true; // filling up before it plays
        int sequence = -1;
        long heardMs;
    }

    private final String mHost, mRoom;
    private final int mPort;
    private final boolean mCanRecord;
    private final int mId = new java.util.Random().nextInt() | 1;
    private final HashMap<Integer, Speaker> mSpeakers = new HashMap<>();
    private volatile boolean mRunning = true, mMicOn = true;
    private volatile DatagramSocket mSocket;
    private InetAddress mServer;
    private int mRoomHash;

    /** Starts at once (on threads of its own). canRecord: the app may use the microphone; else it only listens. */
    VoiceChat(String host, int port, String room, boolean canRecord) {
        mHost = host;
        mPort = port;
        mRoom = room;
        mCanRecord = canRecord;
        new Thread(this::run, "voice").start();
    }

    void setMicOn(boolean on) {
        mMicOn = on;
    }

    void stop() {
        mRunning = false;
        DatagramSocket s = mSocket;
        if (s != null) s.close();
    }

    private void run() {
        try {
            for (InetAddress a : InetAddress.getAllByName(mHost)) {
                if (a instanceof Inet4Address) mServer = a;
            }
            if (mServer == null) return;
            mRoomHash = VoiceCodec.roomHash(mRoom + "/voice");
            mSocket = new DatagramSocket();
            mSocket.setSoTimeout(500);
        } catch (Exception e) {
            return;
        }
        if (!mRunning) {
            mSocket.close();
            return;
        }
        Thread send = new Thread(this::sendLoop, "voice-send"), play = new Thread(this::playLoop, "voice-play");
        send.start();
        play.start();
        receiveLoop();
    }

    // ------------------------------------------------------------------------------------------ sending
    private void send(byte[] data, int length) {
        try {
            mSocket.send(new DatagramPacket(data, length, mServer, mPort));
        } catch (Exception e) {
            // the network is away for a moment: speech is not resent
        }
    }

    private void sendLoop() {
        android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO);
        AudioRecord record = null;
        AcousticEchoCanceler echo = null;
        NoiseSuppressor noise = null;
        if (mCanRecord) {
            try {
                int min = AudioRecord.getMinBufferSize(VoiceCodec.RATE, AudioFormat.CHANNEL_IN_MONO,
                        AudioFormat.ENCODING_PCM_16BIT);
                // The voice source: the phone takes its own loudspeaker's sound out of it where it can.
                record = new AudioRecord(MediaRecorder.AudioSource.VOICE_COMMUNICATION, VoiceCodec.RATE,
                        AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT,
                        Math.max(min, VoiceCodec.FRAME * 2 * 4));
                if (record.getState() != AudioRecord.STATE_INITIALIZED) {
                    record.release();
                    record = null;
                } else {
                    if (AcousticEchoCanceler.isAvailable()) {
                        echo = AcousticEchoCanceler.create(record.getAudioSessionId());
                        if (echo != null) echo.setEnabled(true);
                    }
                    if (NoiseSuppressor.isAvailable()) {
                        noise = NoiseSuppressor.create(record.getAudioSessionId());
                        if (noise != null) noise.setEnabled(true);
                    }
                    record.startRecording();
                }
            } catch (RuntimeException e) {
                record = null; // no microphone: listening only
            }
        }
        VoiceCodec codec = new VoiceCodec();
        short[] pcm = new short[VoiceCodec.FRAME];
        byte[] packet = new byte[RELAY_HEADER + VoiceCodec.PACKET], voice = new byte[VoiceCodec.PACKET];
        byte[] hello = new byte[RELAY_HEADER];
        VoiceCodec.relayHeader(hello, RELAY_HELLO, mRoomHash);
        VoiceCodec.relayHeader(packet, RELAY_BROADCAST, mRoomHash);
        long helloMs = 0;
        int sequence = 0, hang = 0;
        while (mRunning) {
            long now = android.os.SystemClock.elapsedRealtime();
            if (now - helloMs >= HELLO_MS) { // keeps this phone in the voice room
                helloMs = now;
                send(hello, hello.length);
            }
            if (record == null) {
                android.os.SystemClock.sleep(100);
                continue;
            }
            int n = 0;
            while (n < pcm.length && mRunning) {
                int r = record.read(pcm, n, pcm.length - n);
                if (r <= 0) break;
                n += r;
            }
            if (n < pcm.length) {
                android.os.SystemClock.sleep(20);
                continue;
            }
            if (!mMicOn) {
                hang = 0;
                continue;
            }
            double sum = 0;
            for (short s : pcm) sum += s * s;
            if (Math.sqrt(sum / pcm.length) >= SPEECH_RMS) hang = HANG_FRAMES;
            else if (hang > 0) hang--;
            if (hang == 0) continue;
            codec.encode(pcm, mId, sequence++ & 0xffff, voice);
            System.arraycopy(voice, 0, packet, RELAY_HEADER, voice.length);
            send(packet, packet.length);
        }
        if (record != null) {
            try {
                record.stop();
            } catch (RuntimeException e) {
                // already stopped
            }
            if (echo != null) echo.release();
            if (noise != null) noise.release();
            record.release();
        }
    }

    // ------------------------------------------------------------------------------------------ receiving
    private void receiveLoop() {
        byte[] buf = new byte[1500];
        DatagramPacket in = new DatagramPacket(buf, buf.length);
        while (mRunning) {
            try {
                in.setLength(buf.length);
                mSocket.receive(in);
            } catch (java.net.SocketTimeoutException e) {
                continue;
            } catch (Exception e) {
                if (!mRunning) break;
                android.os.SystemClock.sleep(200);
                continue;
            }
            int n = in.getLength();
            if (!in.getAddress().equals(mServer) || in.getPort() != mPort || n < RELAY_HEADER) continue;
            if (VoiceCodec.get32(buf, 0) != 0x52444b4d || buf[5] != RELAY_BROADCAST
                    || VoiceCodec.get32(buf, 12) != mRoomHash) continue;
            if (!VoiceCodec.valid(buf, RELAY_HEADER, n - RELAY_HEADER)) continue;
            int id = VoiceCodec.speaker(buf, RELAY_HEADER), sequence = VoiceCodec.sequence(buf, RELAY_HEADER);
            if (id == mId) continue;
            short[] pcm = new short[VoiceCodec.FRAME];
            VoiceCodec.decode(buf, RELAY_HEADER, pcm);
            long now = android.os.SystemClock.elapsedRealtime();
            synchronized (mSpeakers) {
                Speaker s = mSpeakers.get(id);
                if (s == null) {
                    if (mSpeakers.size() >= 16) continue;
                    mSpeakers.put(id, s = new Speaker());
                }
                // A packet that comes after a later one is dropped (a new start after a pause is not: far away).
                int ahead = (short) (sequence - s.sequence);
                if (s.sequence >= 0 && ahead <= 0 && ahead > -50) continue;
                s.sequence = sequence;
                s.heardMs = now;
                s.frames.add(pcm);
                if (s.frames.size() > MAX_DEPTH) { // the network delivered a burst: catch up
                    while (s.frames.size() > START_DEPTH) s.frames.poll();
                }
            }
        }
        mSocket.close();
    }

    // ------------------------------------------------------------------------------------------ playing
    // Mixes the next 20 ms of every speaker into out; false when nobody has anything.
    private boolean mix(int[] sum, short[] out) {
        boolean any = false;
        java.util.Arrays.fill(sum, 0);
        long now = android.os.SystemClock.elapsedRealtime();
        synchronized (mSpeakers) {
            for (Iterator<Speaker> it = mSpeakers.values().iterator(); it.hasNext(); ) {
                Speaker s = it.next();
                if (now - s.heardMs > SPEAKER_IDLE_MS) {
                    it.remove();
                    continue;
                }
                // Stopped speaking for a while: what is left plays out, and the next speech fills up again.
                if (s.waiting && (s.frames.size() >= START_DEPTH || (!s.frames.isEmpty() && now - s.heardMs > 100))) {
                    s.waiting = false;
                }
                if (s.waiting) continue;
                short[] frame = s.frames.poll();
                if (frame == null) {
                    s.waiting = true;
                    continue;
                }
                for (int i = 0; i < sum.length; i++) sum[i] += frame[i];
                any = true;
            }
        }
        if (any) {
            for (int i = 0; i < out.length; i++) {
                int v = sum[i];
                out[i] = (short) (v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
        }
        return any;
    }

    private void playLoop() {
        android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO);
        AudioTrack track;
        try {
            int min = AudioTrack.getMinBufferSize(VoiceCodec.RATE, AudioFormat.CHANNEL_OUT_MONO,
                    AudioFormat.ENCODING_PCM_16BIT);
            // Played like the game's sound (the loudspeaker or the headphones), not as a phone call (the earpiece).
            track = new AudioTrack.Builder()
                    .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME)
                            .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build())
                    .setAudioFormat(new AudioFormat.Builder().setSampleRate(VoiceCodec.RATE)
                            .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                    .setBufferSizeInBytes(Math.max(min, VoiceCodec.FRAME * 2 * 4))
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .build();
            track.play();
        } catch (RuntimeException e) {
            return;
        }
        int[] sum = new int[VoiceCodec.FRAME];
        short[] out = new short[VoiceCodec.FRAME];
        while (mRunning) {
            if (mix(sum, out)) {
                track.write(out, 0, out.length); // blocks: the output sets the pace
            } else {
                android.os.SystemClock.sleep(10);
            }
        }
        try {
            track.stop();
        } catch (RuntimeException e) {
            // already stopped
        }
        track.release();
    }
}
