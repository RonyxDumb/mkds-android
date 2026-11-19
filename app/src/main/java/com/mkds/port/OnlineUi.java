package com.mkds.port;

import android.app.Activity;
import android.app.AlertDialog;

import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStream;

/**
 * Online play and what comes with it, for the activity that shows the game (the phone's, the headset's): the
 * network settings, the count and names of the players in the room, the voice chat, the game's volume and the
 * player's name.
 */
final class OnlineUi {
    /** What the activity shows of it. */
    interface Host {
        void onOnline(boolean online);

        /** players: in the room, this one included; 0 not known yet, -1 the server is silent. names: the others'. */
        void onPlayers(int players, String names);

        void onVoice(boolean running, boolean micOn);

        void onSettingsClosed();
    }

    private final Activity mActivity;
    private final Host mHost;

    OnlineUi(Activity activity, Host host) {
        mActivity = activity;
        mHost = host;
    }

    boolean isOnline() {
        return onlineFile().length() > 0;
    }

    /** A line for the players in the room ("Online in your room: ..."), or "". */
    String playersOnline() {
        return mPlayersOnline;
    }

    boolean voiceRunning() {
        return mVoice != null;
    }

    boolean micOn() {
        return mMicOn;
    }

    void toggleMic() {
        mMicOn = !mMicOn;
        if (mVoice != null) mVoice.setMicOn(mMicOn);
        mHost.onVoice(mVoice != null, mMicOn);
    }

    /** The saved game volume and player name, to the game. */
    void applyVolume() {
        Native.setGameVolume(mActivity.getSharedPreferences("network", Activity.MODE_PRIVATE).getInt("volume", 100));
    }

    void applyName() {
        Native.setPlayerName(mActivity.getSharedPreferences("network", Activity.MODE_PRIVATE).getString("name", ""));
    }

    // The game's wireless multiplayer (Multi-Card Play) over the internet: the phones of a room meet at a relay
    // server (tools/relay_server.py) instead of on the Wi-Fi network. The settings are kept in the preferences; the
    // game reads them from a file next to the save, "host port room" on one line, each time its wireless starts
    // (no file: local play).
    private static final int RELAY_PORT = 23536;
    // The relay server the settings start with (the player can enter another one).
    private static final String DEFAULT_SERVER = "138.68.53.90";

    File onlineFile() {
        return new File(mActivity.getFilesDir(), "mkds.sav.online");
    }

    // "host" or "host:port" -> {host, port}; null when it is neither.
    private static String[] parseServer(String text) {
        String host = text.trim();
        int port = RELAY_PORT;
        int colon = host.lastIndexOf(':');
        if (colon >= 0) {
            try {
                port = Integer.parseInt(host.substring(colon + 1).trim());
            } catch (NumberFormatException e) {
                return null;
            }
            host = host.substring(0, colon).trim();
        }
        if (host.isEmpty() || host.length() > 190 || port < 1 || port > 65535 || !host.matches("[A-Za-z0-9._-]+")) {
            return null;
        }
        return new String[] {host, Integer.toString(port)};
    }

    // The room code as the game and the server see it: lower case, no spaces; "public" when empty.
    private static String roomCode(String text) {
        String room = text.trim().toLowerCase(java.util.Locale.ROOT).replaceAll("\\s+", "_");
        if (room.length() > 20) room = room.substring(0, 20);
        return room.isEmpty() ? "public" : room;
    }

    private void writeOnline(String[] server, String room) {
        File f = onlineFile();
        if (server == null) {
            f.delete();
        } else {
            try (OutputStream out = new FileOutputStream(f)) {
                out.write((server[0] + " " + server[1] + " " + room + "\n").getBytes("UTF-8"));
            } catch (Exception e) {
                f.delete();
            }
        }
        mHost.onOnline(f.length() > 0);
    }

    // Voice chat (VoiceChat): runs while the game is in front, online play is set up and the box is ticked.
    static final int ASK_MICROPHONE = 2;
    private VoiceChat mVoice;
    private String mPlayersOnline = ""; // for the network settings
    private boolean mMicOn = true, mActive;

    // Starts or stops what runs beside the game while online play is set up: the count of players and the voice chat.
    void update(boolean active) {
        mActive = active;
        if (mVoice != null) {
            mVoice.stop();
            mVoice = null;
        }
        String[] config = null; // host, port, room
        if (active) {
            try {
                String line = new String(java.nio.file.Files.readAllBytes(onlineFile().toPath()), "UTF-8").trim();
                if (!line.isEmpty()) config = line.split(" ");
            } catch (Exception e) {
                // not online
            }
        }
        updatePresence(active, config != null && config.length == 3 ? config : null);
        if (config != null && config.length == 3
                && mActivity.getSharedPreferences("network", Activity.MODE_PRIVATE).getBoolean("voice", false)) {
            boolean canRecord = mActivity.checkSelfPermission(android.Manifest.permission.RECORD_AUDIO)
                    == android.content.pm.PackageManager.PERMISSION_GRANTED;
            mVoice = new VoiceChat(config[0], Integer.parseInt(config[1]), config[2], canRecord);
            mVoice.setMicOn(mMicOn);
        }
        mHost.onVoice(mVoice != null, mMicOn);
    }

    // The count of players: one place in the room for the whole app, whichever activity is in front, and kept
    // through short breaks (from the panel to VR, the settings, a permission question). Each new place counts as
    // one more player at the server until the old one has been silent for a while.
    private static final long PRESENCE_LINGER_MS = 8000;
    private static final android.os.Handler sHandler = new android.os.Handler(android.os.Looper.getMainLooper());
    private static OnlinePresence sPresence;
    private static String sPresenceKey = "";
    private static OnlineUi sOwner; // shows the count
    private static int sPlayers;
    private static String[] sNames = new String[0];
    private static final Runnable sStopPresence = OnlineUi::stopPresence;

    private static void stopPresence() {
        if (sPresence != null) sPresence.stop();
        sPresence = null;
        sPresenceKey = "";
        sPlayers = 0;
        sNames = new String[0];
    }

    private void updatePresence(boolean active, String[] config) {
        if (!active) {
            // Not in front any more: the place is given up only if no activity takes over soon. (An activity
            // that was not showing the count has nothing to give up.)
            if (sOwner == this) {
                sOwner = null;
                sHandler.removeCallbacks(sStopPresence);
                sHandler.postDelayed(sStopPresence, PRESENCE_LINGER_MS);
            }
            return;
        }
        sHandler.removeCallbacks(sStopPresence);
        sOwner = this;
        String name = mActivity.getSharedPreferences("network", Activity.MODE_PRIVATE).getString("name", "");
        String key = config == null ? "" : config[0] + " " + config[1] + " " + config[2] + " " + name;
        if (!key.equals(sPresenceKey)) {
            stopPresence();
            sPresenceKey = key;
            if (config != null) {
                final OnlinePresence[] self = new OnlinePresence[1];
                sPresence = self[0] = new OnlinePresence(config[0], Integer.parseInt(config[1]), config[2], name,
                        (players, names) -> sHandler.post(() -> {
                            if (sPresence != self[0]) return;
                            sPlayers = players;
                            sNames = names;
                            if (sOwner != null) sOwner.showPlayers();
                        }));
            }
        }
        showPlayers();
    }

    private void showPlayers() {
        int players = sPlayers;
        String others = String.join(", ", sNames);
        mPlayersOnline = players == 0 ? "" : players < 0 ? "The server does not answer."
                : players == 1 ? "Online in your room: only you."
                : "Online in your room: " + players + " players"
                + (others.isEmpty() ? "." : " (you, " + others
                + (sNames.length < players - 1 ? " and others without a name" : "") + ").");
        mHost.onPlayers(players, others);
    }

    void showSettings() {
        final android.content.SharedPreferences prefs = mActivity.getSharedPreferences("network", Activity.MODE_PRIVATE);
        float dp = mActivity.getResources().getDisplayMetrics().density;
        android.widget.LinearLayout box = new android.widget.LinearLayout(mActivity);
        box.setOrientation(android.widget.LinearLayout.VERTICAL);
        int pad = (int) (20 * dp);
        box.setPadding(pad, pad / 2, pad, 0);

        final android.widget.CheckBox online = new android.widget.CheckBox(mActivity);
        online.setText("Play online (Multi-Card Play over the internet)");
        online.setChecked(onlineFile().length() > 0);
        final android.widget.EditText server = new android.widget.EditText(mActivity);
        server.setHint("Relay server (host or host:port)");
        server.setSingleLine(true);
        server.setInputType(android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_VARIATION_URI);
        server.setText(prefs.getString("server", DEFAULT_SERVER));
        final android.widget.EditText room = new android.widget.EditText(mActivity);
        room.setHint("Room code (the same for every player)");
        room.setSingleLine(true);
        room.setInputType(android.text.InputType.TYPE_CLASS_TEXT
                | android.text.InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD);
        room.setText(prefs.getString("room", ""));
        final android.widget.EditText name = new android.widget.EditText(mActivity);
        name.setHint("Your name (online list; first 10 letters in races)");
        name.setSingleLine(true);
        name.setFilters(new android.text.InputFilter[] {
                new android.text.InputFilter.LengthFilter(OnlinePresence.NAME_LENGTH)});
        name.setText(prefs.getString("name", ""));
        final android.widget.TextView status = new android.widget.TextView(mActivity);
        status.setText((mPlayersOnline.isEmpty() ? "" : mPlayersOnline + "\n\n") + "Off: players are found on the local Wi-Fi network. On: players who enter the same server "
                + "and room code find each other in Multiplayer → Multi-Card Play. Changes apply the next time "
                + "you enter the multiplayer menu.");
        status.setPadding(0, pad / 2, 0, 0);
        final android.widget.CheckBox voice = new android.widget.CheckBox(mActivity);
        voice.setText("Voice chat with the room (MIC turns the microphone on and off; headphones avoid echo)");
        voice.setChecked(prefs.getBoolean("voice", false));
        box.addView(online);
        box.addView(server);
        box.addView(room);
        box.addView(name);
        box.addView(voice);
        // The game's volume: lower, to hear the others. Heard at once; kept with Save.
        final int savedVolume = prefs.getInt("volume", 100);
        final android.widget.TextView volumeLabel = new android.widget.TextView(mActivity);
        volumeLabel.setText("Game volume: " + savedVolume + "%");
        volumeLabel.setPadding(0, pad / 2, 0, 0);
        final android.widget.SeekBar volume = new android.widget.SeekBar(mActivity);
        volume.setMax(100);
        volume.setProgress(savedVolume);
        volume.setOnSeekBarChangeListener(new android.widget.SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(android.widget.SeekBar bar, int progress, boolean fromUser) {
                volumeLabel.setText("Game volume: " + progress + "%");
                Native.setGameVolume(progress);
            }

            @Override
            public void onStartTrackingTouch(android.widget.SeekBar bar) {}

            @Override
            public void onStopTrackingTouch(android.widget.SeekBar bar) {}
        });
        box.addView(volumeLabel);
        box.addView(volume);
        box.addView(status);
        android.widget.ScrollView scroll = new android.widget.ScrollView(mActivity);
        scroll.addView(box);

        final AlertDialog dialog = new AlertDialog.Builder(mActivity)
                .setTitle("Network")
                .setView(scroll)
                .setPositiveButton("Save", null)
                .setNeutralButton("Test", null)
                .setNegativeButton("Cancel", null)
                .create();
        dialog.setOnDismissListener(d -> {
            // Closed without Save: the volume goes back to what was saved.
            Native.setGameVolume(prefs.getInt("volume", 100));
            mHost.onSettingsClosed();
        });
        dialog.show();
        // Buttons that do not close the dialog on their own.
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            String[] parsed = parseServer(server.getText().toString());
            if (online.isChecked() && parsed == null) {
                status.setText("Enter the relay server's host name or address, for example relay.example.org or "
                        + "203.0.113.5:23536.");
                return;
            }
            prefs.edit().putString("server", server.getText().toString().trim())
                    .putString("room", room.getText().toString().trim())
                    .putString("name", OnlinePresence.cleanName(name.getText().toString()))
                    .putBoolean("voice", voice.isChecked()).putInt("volume", volume.getProgress()).commit();
            writeOnline(online.isChecked() ? parsed : null, roomCode(room.getText().toString()));
            Native.setPlayerName(prefs.getString("name", ""));
            dialog.dismiss();
            if (online.isChecked() && voice.isChecked()
                    && mActivity.checkSelfPermission(android.Manifest.permission.RECORD_AUDIO)
                    != android.content.pm.PackageManager.PERMISSION_GRANTED) {
                mActivity.requestPermissions(new String[] {android.Manifest.permission.RECORD_AUDIO}, ASK_MICROPHONE);
            }
            update(mActive);
        });
        dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v -> {
            final String[] parsed = parseServer(server.getText().toString());
            if (parsed == null) {
                status.setText("Enter the relay server's host name or address first.");
                return;
            }
            final String code = roomCode(room.getText().toString());
            status.setText("Asking " + parsed[0] + "…");
            new Thread(() -> {
                final String result = testRelay(parsed[0], Integer.parseInt(parsed[1]), code);
                mActivity.runOnUiThread(() -> {
                    if (dialog.isShowing()) status.setText(result);
                });
            }).start();
        });
    }

    // Asks the relay server who is in the room (port/platform/hostside/online.h) and reports its answer.
    private static String testRelay(String host, int port, String room) {
        int hash = 0x811c9dc5; // FNV-1a, as online_room_hash
        try {
            for (byte b : room.getBytes("UTF-8")) hash = (hash ^ (b & 0xff)) * 16777619;
        } catch (java.io.UnsupportedEncodingException e) {
            return e.toString();
        }
        java.nio.ByteBuffer hello = java.nio.ByteBuffer.allocate(16).order(java.nio.ByteOrder.LITTLE_ENDIAN);
        hello.putInt(0x52444b4d).put((byte) 1).put((byte) 5).putShort((short) 0).putInt(0).putInt(hash); // a query
        try (java.net.DatagramSocket socket = new java.net.DatagramSocket()) {
            java.net.InetAddress address = null;
            for (java.net.InetAddress a : java.net.InetAddress.getAllByName(host)) {
                if (a instanceof java.net.Inet4Address) address = a;
            }
            if (address == null) return host + " has no IPv4 address.";
            socket.setSoTimeout(700);
            byte[] in = new byte[64];
            for (int i = 0; i < 4; i++) {
                long sent = System.nanoTime();
                socket.send(new java.net.DatagramPacket(hello.array(), 16, address, port));
                java.net.DatagramPacket answer = new java.net.DatagramPacket(in, in.length);
                try {
                    socket.receive(answer);
                } catch (java.net.SocketTimeoutException e) {
                    continue;
                }
                java.nio.ByteBuffer b = java.nio.ByteBuffer.wrap(in, 0, answer.getLength())
                        .order(java.nio.ByteOrder.LITTLE_ENDIAN);
                if (answer.getLength() < 18 || b.getInt(0) != 0x52444b4d || b.get(5) != 0) continue;
                long ms = (System.nanoTime() - sent) / 1000000;
                int others = b.getShort(16) & 0xffff;
                return "The server answers in " + ms + " ms. " + (others == 0 ? "Nobody is"
                        : others == 1 ? "1 player is" : others + " players are")
                        + " in room \"" + room + "\" right now."
                        + (ms > 40 ? " Races run in step with every player: with this delay expect slow motion "
                        + "unless the phones reach each other directly." : "");
            }
            return "No answer from " + host + " on UDP port " + port + ".";
        } catch (java.net.UnknownHostException e) {
            return "Cannot find " + host + ".";
        } catch (Exception e) {
            return "Cannot reach the server: " + e.getMessage();
        }
    }
}
