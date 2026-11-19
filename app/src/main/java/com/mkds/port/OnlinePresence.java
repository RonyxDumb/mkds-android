package com.mkds.port;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;

/**
 * How many players are online in the room.
 *
 * While online play is set up and the game is in front, the app keeps a place in a room of its own at the relay
 * server, "<room>/online" (next to the game's room, which a phone is only in while its multiplayer menu is open).
 * The server's answer to each hello says how many others are there. With each hello the app also tells the room
 * its player's name (a broadcast: "MKDN", u32 a number of this phone's own, the name in UTF-8), so that everyone
 * sees who is there.
 */
final class OnlinePresence {
    interface Listener {
        /**
         * players: everyone in the room, this phone included; -1 when the server does not answer.
         * names: the others' names, as far as they have told them.
         */
        void onPlayers(int players, String[] names);
    }

    private static final int HELLO_MS = 3000, SILENT_MS = 10000, NAME_MS = 10000;
    private static final int NAME_MAGIC = 0x4e444b4d; // "MKDN"
    static final int NAME_LENGTH = 16;

    private static final class Other {
        String name;
        long heardMs;
    }

    /** A name as it is shown: no control characters, NAME_LENGTH characters at most. */
    static String cleanName(String name) {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < name.length() && b.length() < NAME_LENGTH; i++) {
            char c = name.charAt(i);
            if (c >= ' ' && c != 0x7f && c != ',') b.append(c);
        }
        return b.toString().trim();
    }

    private final String mHost, mRoom, mName;
    private final int mId = new java.util.Random().nextInt();
    private final java.util.HashMap<Integer, Other> mOthers = new java.util.HashMap<>();
    private final int mPort;
    private final Listener mListener;
    private volatile boolean mRunning = true;
    private volatile DatagramSocket mSocket;

    OnlinePresence(String host, int port, String room, String name, Listener listener) {
        mName = cleanName(name);
        mHost = host;
        mPort = port;
        mRoom = room;
        mListener = listener;
        new Thread(this::run, "online-presence").start();
    }

    /** Leaves the room (the thread tells the server and closes the socket, within half a second). */
    void stop() {
        mRunning = false;
    }

    private void run() {
        int room = VoiceCodec.roomHash(mRoom + "/online");
        byte[] hello = new byte[16], buf = new byte[256];
        VoiceCodec.relayHeader(hello, 0, room);
        byte[] nameBytes = mName.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        byte[] name = new byte[16 + 8 + nameBytes.length];
        VoiceCodec.relayHeader(name, 2, room);
        VoiceCodec.put32(name, 16, NAME_MAGIC);
        VoiceCodec.put32(name, 20, mId);
        System.arraycopy(nameBytes, 0, name, 24, nameBytes.length);
        String shownNames = "";
        InetAddress server = null;
        long answered = System.nanoTime() / 1000000;
        int shown = -2;
        while (mRunning) {
            int players = shown;
            try {
                if (server == null) {
                    for (InetAddress a : InetAddress.getAllByName(mHost)) {
                        if (a instanceof Inet4Address) server = a;
                    }
                    if (server == null) throw new java.net.UnknownHostException(mHost);
                    mSocket = new DatagramSocket();
                    mSocket.setSoTimeout(500);
                }
                mSocket.send(new DatagramPacket(hello, hello.length, server, mPort));
                if (nameBytes.length > 0) mSocket.send(new DatagramPacket(name, name.length, server, mPort));
                long until = System.nanoTime() / 1000000 + HELLO_MS;
                while (mRunning && System.nanoTime() / 1000000 < until) { // one hello every HELLO_MS
                    DatagramPacket in = new DatagramPacket(buf, buf.length);
                    try {
                        mSocket.receive(in);
                    } catch (java.net.SocketTimeoutException e) {
                        continue;
                    }
                    int n = in.getLength();
                    if (n < 18 || !in.getAddress().equals(server) || VoiceCodec.get32(buf, 0) != 0x52444b4d
                            || VoiceCodec.get32(buf, 12) != room) continue;
                    if (buf[5] == 0) {
                        players = ((buf[16] & 0xff) | (buf[17] & 0xff) << 8) + 1;
                        answered = System.nanoTime() / 1000000;
                    } else if (buf[5] == 2 && n > 24 && VoiceCodec.get32(buf, 16) == NAME_MAGIC) {
                        int id = VoiceCodec.get32(buf, 20);
                        String other = cleanName(new String(buf, 24, n - 24, java.nio.charset.StandardCharsets.UTF_8));
                        if (id == mId || other.isEmpty()) continue;
                        Other o = mOthers.get(id);
                        if (o == null) {
                            if (mOthers.size() >= 64) continue;
                            mOthers.put(id, o = new Other());
                        }
                        o.name = other;
                        o.heardMs = System.nanoTime() / 1000000;
                    }
                }
            } catch (Exception e) {
                if (!mRunning) break;
                try {
                    Thread.sleep(HELLO_MS);
                } catch (InterruptedException ie) {
                    break;
                }
            }
            if (System.nanoTime() / 1000000 - answered > SILENT_MS) players = -1;
            long now = System.nanoTime() / 1000000;
            java.util.ArrayList<String> names = new java.util.ArrayList<>();
            for (java.util.Iterator<Other> it = mOthers.values().iterator(); it.hasNext(); ) {
                Other o = it.next();
                if (now - o.heardMs > NAME_MS) it.remove();
                else names.add(o.name);
            }
            java.util.Collections.sort(names, String.CASE_INSENSITIVE_ORDER);
            String joined = String.join(",", names);
            if ((players != shown || !joined.equals(shownNames)) && players != -2 && mRunning) {
                shown = players;
                shownNames = joined;
                mListener.onPlayers(players, names.toArray(new String[0]));
            }
        }
        DatagramSocket s = mSocket;
        if (s != null) {
            // Goodbye: a server that knows it (type 6) forgets this place at once instead of after its timeout.
            try {
                byte[] bye = new byte[16];
                VoiceCodec.relayHeader(bye, 6, room);
                s.send(new DatagramPacket(bye, bye.length, server, mPort));
            } catch (Exception e) {
                // it times out at the server
            }
            s.close();
        }
    }
}
