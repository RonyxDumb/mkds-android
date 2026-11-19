package com.mkds.port;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Meta Quest: the panel the app opens with, which also opens over the game from VR (QuestActivity). The ROM, the
 * network settings (online play, voice chat, name, volume: the same as on a phone, with the headset's keyboard),
 * the button that starts the game in VR, and, when VR did not start or the app crashed, the report of it.
 */
public class QuestLauncherActivity extends Activity {
    private static final int PICK_ROM = 1;

    private TextView mStatus;
    private final OnlineUi mOnline = new OnlineUi(this, new OnlineUi.Host() {
        @Override
        public void onOnline(boolean online) {
            refresh();
        }

        @Override
        public void onPlayers(int players, String names) {
            refresh();
        }

        @Override
        public void onVoice(boolean running, boolean micOn) {}

        @Override
        public void onSettingsClosed() {
            refresh();
        }
    });

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        float dp = getResources().getDisplayMetrics().density;
        int pad = (int) (24 * dp);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(pad, pad, pad, pad);
        box.setGravity(Gravity.CENTER_HORIZONTAL);

        TextView title = new TextView(this);
        title.setText("Mario Kart DS VR");
        title.setTextSize(28);
        box.addView(title);

        mStatus = new TextView(this);
        mStatus.setPadding(0, pad / 2, 0, pad / 2);
        box.addView(mStatus);

        Button play = new Button(this);
        play.setText(QuestActivity.sAlive ? "Back to the game" : "Play in VR");
        play.setOnClickListener(v -> play());
        box.addView(play);

        Button network = new Button(this);
        network.setText("Network, voice chat, name, volume");
        network.setOnClickListener(v -> mOnline.showSettings());
        box.addView(network);

        Button rom = new Button(this);
        rom.setText("Choose the ROM file");
        rom.setOnClickListener(v -> pickRom());
        box.addView(rom);

        TextView help = new TextView(this);
        help.setPadding(0, pad / 2, 0, 0);
        help.setText("Controls: left stick steers. A accelerate, B brake, right trigger or grip drift (R), left trigger "
                + "or grip item (L), X and Y as on the DS. Menu button START, right stick click SELECT. Point a "
                + "controller at the touch screen and pull its trigger to touch it. Left stick click turns the "
                + "microphone on and off. To open these settings, point a controller at the line under the "
                + "touch screen and pull its trigger.");
        box.addView(help);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(box);
        setContentView(scroll);
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
        mOnline.update(!QuestActivity.sAlive); // the game's activity runs the count of players and the voice chat
    }

    @Override
    protected void onPause() {
        super.onPause();
        mOnline.update(false);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        QuestActivity.sSettingsClosed = true;
    }

    private void refresh() {
        if (mStatus == null) return;
        StringBuilder text = new StringBuilder();
        // What went wrong the last time VR was started.
        String error = getSharedPreferences("quest", MODE_PRIVATE).getString("vr_error", "");
        if (!error.isEmpty()) text.append("VR could not start: ").append(error).append("\n\n");
        String report = report();
        if (!report.isEmpty()) text.append(report).append("\n");
        text.append(QuestGame.hasRom(this) ? "ROM: ready." : "ROM: choose your Mario Kart DS (Europe, AMCP) file.");
        text.append("\n");
        if (mOnline.isOnline()) {
            String players = mOnline.playersOnline();
            text.append("Online play: on. ").append(players.isEmpty() ? "" : players);
        } else {
            text.append("Online play: off (local Wi-Fi).");
        }
        mStatus.setText(text);
    }

    // The crash report of the last run and how far VR got, short enough for a screenshot. Empty when all went well.
    private String report() {
        StringBuilder out = new StringBuilder();
        boolean crashed = false;
        try {
            File crash = new File(getFilesDir(), "crash.txt");
            if (crash.isFile()) {
                crashed = true;
                out.append("REPORT (please send a screenshot)\nThe app crashed:\n");
                String[] lines = new String(java.nio.file.Files.readAllBytes(crash.toPath()), "UTF-8").split("\n");
                for (int i = 0; i < lines.length && i < 22; i++) {
                    if (lines[i].startsWith("stack:") || lines[i].startsWith("x")) continue;
                    out.append(lines[i].trim()).append("\n");
                }
            }
            boolean pending = getSharedPreferences("quest", MODE_PRIVATE).getBoolean("vr_pending", false);
            File log = new File(getFilesDir(), "log.txt");
            if ((crashed || pending) && log.isFile()) {
                if (!crashed) out.append("REPORT (please send a screenshot)\nVR closed while starting.\n");
                out.append("Log:\n");
                String[] lines = new String(java.nio.file.Files.readAllBytes(log.toPath()), "UTF-8").split("\n");
                for (int i = Math.max(0, lines.length - 14); i < lines.length; i++) out.append(lines[i]).append("\n");
            }
        } catch (Exception e) {
            out.append(e).append("\n");
        }
        return out.toString();
    }

    private void play() {
        if (!QuestGame.hasRom(this)) {
            pickRom();
            return;
        }
        if (!QuestActivity.sAlive) new File(getFilesDir(), "crash.txt").delete();
        String err = QuestGame.start(this, mOnline);
        if (err != null) {
            new AlertDialog.Builder(this).setTitle("Mario Kart DS").setMessage(err).setPositiveButton("OK", null).show();
            return;
        }
        if (!QuestActivity.sAlive) {
            // From the panel to the immersive activity. "vr_pending" stays set until VR shows the game: if the app
            // is back here with it set, VR closed on the way and the panel shows the log.
            getSharedPreferences("quest", MODE_PRIVATE).edit().remove("vr_error").putBoolean("vr_pending", true)
                    .commit();
            startActivity(new Intent(this, QuestActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            finishAndRemoveTask();
            return;
        }
        finish(); // the game is behind this panel
    }

    private void pickRom() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, PICK_ROM);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_ROM || resultCode != RESULT_OK || data == null || data.getData() == null) return;
        if (QuestGame.started()) {
            new AlertDialog.Builder(this).setMessage("The game is running: quit the app and open it again to change "
                    + "the ROM.").setPositiveButton("OK", null).show();
            return;
        }
        File dst = QuestGame.romFile(this);
        try {
            copy(data.getData(), dst);
        } catch (Exception e) {
            dst.delete();
            new AlertDialog.Builder(this).setMessage("Cannot read the file: " + e.getMessage())
                    .setPositiveButton("OK", null).show();
            return;
        }
        if (!QuestGame.hasRom(this)) {
            dst.delete();
            new AlertDialog.Builder(this).setMessage("This is not Mario Kart DS (Europe, game code AMCP).")
                    .setPositiveButton("OK", null).show();
        }
        refresh();
    }

    private void copy(Uri uri, File dst) throws Exception {
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(dst)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
    }
}
