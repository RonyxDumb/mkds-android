package com.mkds.port;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.WindowManager;

/**
 * Meta Quest: the game in VR; the app opens with it. The picture and the controllers are native
 * (port/platform/android/xr.c); this activity starts them, runs what goes with online play (count of players,
 * voice chat) and draws the status line shown under the touch screen. The settings are a panel
 * (QuestLauncherActivity) that opens over the game: when the player pulls a trigger on the status line, and by
 * itself as long as no ROM has been chosen.
 */
public class QuestActivity extends Activity {
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mResumed, mShown;
    private int mMicToggles, mSettingsRequests;
    static boolean sAlive; // for the panel: the game is shown behind it
    static volatile boolean sSettingsClosed; // by the panel: the settings may have changed
    private int mPlayers;
    private String mNames = "";
    private boolean mVoice, mMicOn = true;

    private final OnlineUi mOnline = new OnlineUi(this, new OnlineUi.Host() {
        @Override
        public void onOnline(boolean online) {
            drawStatus();
        }

        @Override
        public void onPlayers(int players, String names) {
            mPlayers = players;
            mNames = names;
            drawStatus();
        }

        @Override
        public void onVoice(boolean running, boolean micOn) {
            mVoice = running;
            mMicOn = micOn;
            drawStatus();
        }

        @Override
        public void onSettingsClosed() {}
    });

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        sAlive = true;
        getSharedPreferences("quest", MODE_PRIVATE).edit().remove("vr_error").apply();
        QuestGame.init(this); // the log, and the crash report if this goes wrong
        startGame();
        mMicToggles = Native.xrMicToggles();
        mSettingsRequests = Native.xrSettingsRequests();
        drawStatus();
        Native.xrStart(this);
        mHandler.postDelayed(mPoll, 100);
        if (!QuestGame.started()) mHandler.postDelayed(this::openSettings, 1500); // the ROM is chosen there
    }

    // Starts the game once there is a ROM (chosen in the panel).
    private void startGame() {
        if (QuestGame.started() || !QuestGame.hasRom(this)) return;
        String err = QuestGame.start(this, mOnline);
        if (err != null) getSharedPreferences("quest", MODE_PRIVATE).edit().putString("vr_error", err).commit();
    }

    // The settings panel, over the game.
    private void openSettings() {
        if (isFinishing() || isDestroyed()) return;
        startActivity(new android.content.Intent(this, QuestLauncherActivity.class));
    }

    // VR cannot be shown: the panel says what went wrong (nothing can be shown from here).
    private void fail(String error) {
        getSharedPreferences("quest", MODE_PRIVATE).edit().putString("vr_error", error).commit();
        sAlive = false;
        startActivity(new android.content.Intent(this, QuestLauncherActivity.class)
                .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK));
        finish();
    }

    // The controller's microphone button, and the end of the session.
    private final Runnable mPoll = new Runnable() {
        @Override
        public void run() {
            if (isFinishing() || isDestroyed()) return;
            int toggles = Native.xrMicToggles();
            if (toggles != mMicToggles) {
                if (((toggles - mMicToggles) & 1) != 0) mOnline.toggleMic();
                mMicToggles = toggles;
            }
            if (sSettingsClosed) { // whether or not the headset paused this activity for the panel
                sSettingsClosed = false;
                startGame();
                mOnline.update(mResumed);
                drawStatus();
            }
            int requests = Native.xrSettingsRequests();
            if (requests != mSettingsRequests) {
                mSettingsRequests = requests;
                openSettings();
            }
            int state = Native.xrState();
            if (state == 2) {
                if (!mShown) getSharedPreferences("quest", MODE_PRIVATE).edit().remove("vr_pending").apply();
                mShown = true;
            } else if (state == -1) {
                fail(Native.xrError());
                return;
            } else if (state == 0 && mShown) {
                finishAndRemoveTask(); // the headset ended the session
                return;
            }
            mHandler.postDelayed(this, 100);
        }
    };

    @Override
    protected void onResume() {
        super.onResume();
        mResumed = true;
        startGame();
        Native.setPaused(false);
        mOnline.update(true);
        drawStatus();
    }

    @Override
    protected void onPause() {
        super.onPause();
        mResumed = false;
        mOnline.update(false);
        Native.setPaused(true);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        mHandler.removeCallbacks(mPoll);
        sAlive = false;
        Native.xrStop();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        if (requestCode == OnlineUi.ASK_MICROPHONE) mOnline.update(mResumed);
    }

    // The line under the touch screen: who is online, and the microphone.
    private void drawStatus() {
        int w = Native.XR_STATUS_WIDTH, h = Native.XR_STATUS_HEIGHT;
        Bitmap bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(bitmap);
        c.drawColor(0xff101418);
        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setColor(0xffe8e8e8);
        text.setTextSize(34);
        String first, second = "";
        if (!QuestGame.started()) {
            first = "Choose your ROM in the settings";
        } else if (!mOnline.isOnline()) {
            first = "Local Wi-Fi play";
        } else if (mPlayers < 0) {
            first = "Online: the server does not answer";
        } else if (mPlayers == 0) {
            first = "Online";
        } else if (mPlayers == 1) {
            first = "Online: only you";
        } else {
            first = mPlayers + " online" + (mNames.isEmpty() ? "" : ": " + mNames);
        }
        if (mVoice) second = mMicOn ? "Microphone ON (left stick click: off)" : "Microphone OFF (left stick click: on)";
        else second = "Point here and pull the trigger for the settings";
        if (first.length() > 34) first = first.substring(0, 33) + "…";
        c.drawText(first, 20, 46, text);
        text.setColor(mVoice && mMicOn ? 0xff60d080 : 0xffd08060);
        text.setTextSize(28);
        c.drawText(second, 20, 96, text);
        // The button for the settings panel (a trigger pulled anywhere on this line opens it).
        Paint box = new Paint(Paint.ANTI_ALIAS_FLAG);
        box.setColor(0xff2c5a8c);
        c.drawRoundRect(w - 250, 14, w - 14, 64, 12, 12, box);
        text.setColor(0xffffffff);
        text.setTextSize(30);
        text.setTextAlign(Paint.Align.CENTER);
        c.drawText("SETTINGS", w - 132, 50, text);
        int[] pixels = new int[w * h];
        bitmap.getPixels(pixels, 0, w, 0, 0, w, h);
        bitmap.recycle();
        Native.xrSetStatus(pixels);
    }
}
