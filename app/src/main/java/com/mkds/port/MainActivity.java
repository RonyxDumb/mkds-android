package com.mkds.port;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;

/** Asks for the ROM once, then runs the game full screen. */
public class MainActivity extends Activity implements SurfaceHolder.Callback {
    private static final int PICK_ROM = 1, EXPORT_SAVE = 2, IMPORT_SAVE = 3, ADD_MOD = 4;
    private static boolean sStarted;

    private ControlsView mControls;
    private int mPadButtons;

    private File romFile() {
        return new File(getFilesDir(), "rom.nds");
    }

    // The console's identity (its wireless address, "<save>.mac", and the Nintendo WFC user ID in its firmware,
    // "<save>.fw") belongs to this device. Android copies an app's files to a new phone (backup, device transfer):
    // both phones were then the same console, with the same friend code. On another device (or the first time this
    // check runs) the identity is made anew; the saves stay.
    private void bindIdentityToDevice() {
        String id = android.provider.Settings.Secure.getString(getContentResolver(),
                android.provider.Settings.Secure.ANDROID_ID);
        if (id == null) id = "";
        File marker = new File(getFilesDir(), "device-id");
        String stored = null;
        try {
            stored = new String(java.nio.file.Files.readAllBytes(marker.toPath()), "UTF-8").trim();
        } catch (Exception ignored) {
        }
        if (id.equals(stored)) return;
        File[] files = getFilesDir().listFiles();
        if (files != null) {
            for (File f : files) {
                String n = f.getName();
                if (n.endsWith(".sav.mac") || n.endsWith(".sav.fw")) f.delete();
            }
        }
        try {
            java.nio.file.Files.write(marker.toPath(), id.getBytes("UTF-8"));
        } catch (Exception ignored) {
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (android.os.Build.VERSION.SDK_INT >= 28) { // Android 9: displays with a notch
            getWindow().getAttributes().layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        bindIdentityToDevice();
        Native.init(getFilesDir().getAbsolutePath());
        File crash = new File(getFilesDir(), "crash.txt");
        if (crash.isFile()) {
            showCrashReport(crash);
            return;
        }
        UpdateCheck.start(this);
        if (isValidRom(romFile())) {
            chooseLanguageThenStart();
        } else {
            new AlertDialog.Builder(this)
                    .setTitle("Mario Kart DS")
                    .setMessage("Select your Mario Kart DS ROM (Europe, AMCP). It is copied into the app's private "
                            + "storage once.")
                    .setPositiveButton("Choose file", (d, w) -> pickRom())
                    .setCancelable(false)
                    .show();
        }
    }

    /** The game could not start: the reason, the device, the crash report (if any) and the end of the log, which
     * can be copied for a bug report (log.txt is in the app's private storage, out of the player's reach). */
    private void showStartError(String err) {
        StringBuilder text = new StringBuilder(err).append("\n\n").append("Mario Kart DS ")
                .append(BuildConfig.VERSION_NAME).append(", Android ").append(android.os.Build.VERSION.RELEASE)
                .append(", ").append(android.os.Build.MANUFACTURER).append(' ').append(android.os.Build.MODEL)
                .append(", ").append(android.text.TextUtils.join("/", android.os.Build.SUPPORTED_ABIS)).append('\n');
        for (String name : new String[] {"crash.txt", "log.txt"}) {
            File f = new File(getFilesDir(), name);
            if (!f.isFile()) continue;
            try {
                byte[] all = java.nio.file.Files.readAllBytes(f.toPath());
                int from = Math.max(0, all.length - 6000);
                text.append('\n').append(name).append(":\n").append(new String(all, from, all.length - from, "UTF-8"));
            } catch (Exception ignored) {
            }
        }
        new File(getFilesDir(), "crash.txt").delete(); // shown here, not again at the next start
        final String report = text.toString();
        new AlertDialog.Builder(this).setTitle("The game could not start").setMessage(report)
                .setPositiveButton("Copy report", (d, w) -> {
                    android.content.ClipboardManager cm =
                            (android.content.ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
                    cm.setPrimaryClip(android.content.ClipData.newPlainText("mkds start error", report));
                    toast("Copied: paste it in a bug report");
                    finish();
                })
                .setNegativeButton("Quit", (d, w) -> finish()).setCancelable(false).show();
    }

    /** The report of the last crash, with the log before it: shown once, with a button to copy it. */
    private void showCrashReport(File crash) {
        StringBuilder text = new StringBuilder();
        try {
            text.append(new String(java.nio.file.Files.readAllBytes(crash.toPath()), "UTF-8"));
            File log = new File(getFilesDir(), "log.txt");
            if (log.isFile()) {
                text.append("\nlog:\n").append(new String(java.nio.file.Files.readAllBytes(log.toPath()), "UTF-8"));
            }
        } catch (Exception e) {
            text.append(e);
        }
        crash.delete();
        final String report = text.toString();
        new AlertDialog.Builder(this)
                .setTitle("The game crashed last time")
                .setMessage(report)
                .setPositiveButton("Copy report", (d, w) -> {
                    android.content.ClipboardManager cm =
                            (android.content.ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
                    cm.setPrimaryClip(android.content.ClipData.newPlainText("mkds crash", report));
                    recreate();
                })
                .setNegativeButton("Continue", (d, w) -> recreate())
                .setCancelable(false)
                .show();
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
        if (requestCode == ADD_MOD) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) finishAddMod(data.getData());
            return;
        }
        if (requestCode == EXPORT_SAVE || requestCode == IMPORT_SAVE) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) {
                if (requestCode == EXPORT_SAVE) finishExport(data.getData());
                else finishImport(data.getData());
            }
            return;
        }
        if (requestCode != PICK_ROM) return;
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            finish();
            return;
        }
        try {
            copy(data.getData(), romFile());
        } catch (Exception e) {
            showError("Cannot read the file: " + e.getMessage());
            return;
        }
        if (!isValidRom(romFile())) {
            romFile().delete();
            showError("This is not Mario Kart DS (Europe, game code AMCP).");
            return;
        }
        chooseLanguageThenStart();
    }

    private void copy(Uri uri, File dst) throws Exception {
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(dst)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
    }

    private static boolean isValidRom(File f) {
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

    private void showError(String message) {
        new AlertDialog.Builder(this)
                .setTitle("Mario Kart DS")
                .setMessage(message)
                .setPositiveButton("Choose file", (d, w) -> pickRom())
                .setNegativeButton("Quit", (d, w) -> finish())
                .setCancelable(false)
                .show();
    }

    // ------------------------------------------------------------------------------------------ start
    // The game's languages (Europe): the game is started in the one chosen at each launch.
    private static final String[] LANGUAGE_NAMES = {"English", "Français", "Deutsch", "Italiano", "Español"};
    private static final int[] LANGUAGE_IDS = {1, 2, 3, 4, 5};
    private int mLanguage = 1;
    private LanguageScreen mLanguageScreen;

    private void chooseLanguageThenStart() {
        if (sStarted) {
            startGame();
            return;
        }
        android.content.SharedPreferences prefs = getSharedPreferences("mkds", MODE_PRIVATE);
        int last = prefs.getInt("language", gameLanguage());
        mLanguageScreen = new LanguageScreen(this, LANGUAGE_NAMES, indexOfLanguage(last), index -> {
            mLanguage = LANGUAGE_IDS[index];
            prefs.edit().putInt("language", mLanguage).apply();
            mLanguageScreen = null;
            startGame();
        });
        setContentView(mLanguageScreen.view());
        hideSystemBars();
        UpdateCheck.whenAvailable(this, mLanguageScreen::showUpdate);
    }

    private static int indexOfLanguage(int id) {
        for (int i = 0; i < LANGUAGE_IDS.length; i++) {
            if (LANGUAGE_IDS[i] == id) return i;
        }
        return 0;
    }

    // The save file: the game's own, or the active mod's (each mod has its own).
    private File saveFile() {
        String mod = Mods.saveSuffix(this);
        return new File(getFilesDir(), mod == null ? "mkds.sav" : "mkds-" + mod + ".sav");
    }

    private File mGameRom;

    private void startGame() {
        if (!sStarted && mGameRom == null) {
            // With a mod: its ROM is made first (a few seconds, the first time).
            if (Mods.active(this).isEmpty()) {
                mGameRom = romFile();
            } else {
                android.app.ProgressDialog wait = android.app.ProgressDialog.show(this, "Mod",
                        "Preparing " + Mods.active(this) + "…", true, false);
                new Thread(() -> {
                    File rom = Mods.romForGame(this, romFile());
                    runOnUiThread(() -> {
                        wait.dismiss();
                        mGameRom = rom;
                        startGame();
                    });
                }).start();
                return;
            }
        }
        if (!sStarted) {
            // An imported save file replaces the current one before the game reads it.
            File imported = new File(getFilesDir(), "mkds.sav.import");
            if (imported.isFile()) {
                File old = new File(getFilesDir(), "mkds.sav.before-import");
                old.delete();
                saveFile().renameTo(old);
                if (!imported.renameTo(saveFile())) old.renameTo(saveFile());
            }
            // The language, and the options the game reads at its start (port/platform/os.c).
            // 0x200: one screen (what the game shows on two screens is made for one, gl/present.c).
            // Dual-screen devices: the two screens as on a DS (no 0x200), the touch screen on the second display.
            sDual = OptionsScreen.secondDisplay(this) && DualScreen.secondDisplay(this) != null;
            Native.setLanguage(mLanguage | (sDual ? 0 : 0x200) | (OptionsScreen.unlockAll(this) ? 0x100 : 0));
            applyGraphics();
            mOnline.applyVolume();
            String err = Native.start(mGameRom.getAbsolutePath(), saveFile().getAbsolutePath());
            if (err != null) {
                showStartError(err);
                return;
            }
            sStarted = true;
            mOnline.applyName();
        }
        FrameLayout root = mRoot = new FrameLayout(this);
        SurfaceView surface = new SurfaceView(this);
        surface.getHolder().addCallback(this);
        mControls = new ControlsView(this);
        mControls.setDual(sDual);
        mControls.setOnNetwork(mOnline::showSettings);
        mControls.setOnline(mOnline.isOnline());
        mControls.setOnMic(mOnline::toggleMic);
        mControls.setOnOptions(this::showOptions);
        updateVoice();
        root.addView(surface);
        root.addView(mControls);
        mGamepad = false;
        updateGamepads(false);
        setContentView(root);
        hideSystemBars();
        preferHighRefreshRate();
        if (mResumed) showDualScreen(true);
        if (android.os.Build.VERSION.SDK_INT >= 33) {
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT, this::onGameBack);
        }
    }

    // Back does not leave the game: in a race it pauses (START), elsewhere it does nothing. It closes the options.
    private void onGameBack() {
        if (mControls != null && mControls.isEditing()) {
            mControls.cancelEditing();
            return;
        }
        if (mOptions != null) {
            mOptions.close();
            return;
        }
        if (mControls != null && Native.isRace()) mControls.pulse(Native.START);
    }

    // ------------------------------------------------------------------------------------------ options
    private FrameLayout mRoot;
    private OptionsScreen mOptions;

    private void showOptions() {
        if (mOptions != null || mRoot == null) return;
        if (sStarted) Native.setPaused(true); // also writes the save file
        mOptions = new OptionsScreen(this, new OptionsScreen.Host() {
            @Override
            public void onOptionsChanged() {
                if (mControls != null) mControls.applyOptions();
                applyGraphics();
                preferHighRefreshRate();
            }

            @Override
            public void onOptionsClosed() {
                mOptions = null;
                if (mControls != null) mControls.setVisibility(View.VISIBLE);
                if (sStarted && mResumed) Native.setPaused(false);
                hideSystemBars();
            }

            @Override
            public void exportSave() {
                Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("application/octet-stream");
                intent.putExtra(Intent.EXTRA_TITLE, "mkds.sav");
                startActivityForResult(intent, EXPORT_SAVE);
            }

            @Override
            public void importSave() {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("*/*");
                startActivityForResult(intent, IMPORT_SAVE);
            }

            @Override
            public void restartGame() {
                MainActivity.this.restartGame();
            }

            @Override
            public void addMod() {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("*/*");
                startActivityForResult(intent, ADD_MOD);
            }

            @Override
            public void shareLog() {
                // The end of log.txt (the game's messages: Nintendo WFC's are "wfc:"), shared as text.
                try {
                    byte[] all = java.nio.file.Files.readAllBytes(new File(getFilesDir(), "log.txt").toPath());
                    int from = Math.max(0, all.length - 200000);
                    String text = "Mario Kart DS " + BuildConfig.VERSION_NAME + " (Android "
                            + android.os.Build.VERSION.RELEASE + ", " + android.os.Build.MODEL + ")\n"
                            + new String(all, from, all.length - from, "UTF-8");
                    Intent send = new Intent(Intent.ACTION_SEND).setType("text/plain")
                            .putExtra(Intent.EXTRA_SUBJECT, "Mario Kart DS log").putExtra(Intent.EXTRA_TEXT, text);
                    startActivity(Intent.createChooser(send, "Share the log"));
                } catch (Exception e) {
                    toast("No log yet");
                }
            }

            @Override
            public void remapGamepad() {
                GamepadMapping.showEditor(MainActivity.this);
            }

            @Override
            public void editLayout() {
                // The editor is drawn by the controls over the paused game; the options come back after it.
                if (mOptions != null) mOptions.close();
                if (mControls == null) return;
                if (sStarted) Native.setPaused(true);
                mControls.setVisibility(View.VISIBLE);
                mControls.startEditing(() -> {
                    if (sStarted && mResumed) Native.setPaused(false);
                    showOptions();
                });
            }

            @Override
            public void onModChosen() {
                new AlertDialog.Builder(MainActivity.this)
                        .setTitle(Mods.active(MainActivity.this).isEmpty() ? "Original game" : Mods.active(MainActivity.this))
                        .setMessage("The game restarts to load it.")
                        .setPositiveButton("Restart now", (d, w) -> MainActivity.this.restartGame())
                        .setNegativeButton("Later", null)
                        .show();
            }
        });
        if (mControls != null) mControls.setVisibility(View.INVISIBLE);
        mRoot.addView(mOptions.view(), new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));
        mOptions.view().bringToFront();
    }

    private void applyGraphics() {
        Native.setGraphics(OptionsScreen.maxScale(this), OptionsScreen.highFrameRate(this));
        Native.setHud(OptionsScreen.hudSize(this), OptionsScreen.hudCorner(this), OptionsScreen.menuScreens(this));
    }

    // The save file as the game last wrote it (the game is paused while the options are open, which writes it).
    private void finishExport(Uri uri) {
        try (InputStream in = new java.io.FileInputStream(saveFile());
             OutputStream out = getContentResolver().openOutputStream(uri, "wt")) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            toast("Save file exported");
        } catch (Exception e) {
            toast("Export failed: " + e.getMessage());
        }
    }

    // A save file from elsewhere (an export, or a DS / emulator save of the European game: 256 KiB flash). It takes
    // the place of the current one at the next start (the old one is kept as mkds.sav.before-import).
    private void finishImport(Uri uri) {
        File dst = new File(getFilesDir(), "mkds.sav.import");
        try {
            copy(uri, dst);
        } catch (Exception e) {
            dst.delete();
            toast("Import failed: " + e.getMessage());
            return;
        }
        long size = dst.length();
        if (size < 0x2000 || size > 0x80000) {
            dst.delete();
            toast("This is not a Mario Kart DS save file (" + size + " bytes)");
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle("Import save file")
                .setMessage("The imported save replaces your current progress. The game restarts to load it.")
                .setPositiveButton("Restart now", (d, w) -> restartGame())
                .setNegativeButton("Cancel", (d, w) -> dst.delete())
                .setCancelable(false)
                .show();
    }

    // A mod chosen in the file picker: an xdelta patch or a patched ROM, added in the background.
    private void finishAddMod(Uri uri) {
        String file = uri.getLastPathSegment() == null ? "Mod" : uri.getLastPathSegment();
        try (android.database.Cursor c = getContentResolver().query(uri,
                new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && c.getString(0) != null) file = c.getString(0);
        } catch (Exception ignored) {
        }
        final String name = Mods.nameFor(file.substring(file.lastIndexOf('/') + 1));
        android.app.ProgressDialog wait = new android.app.ProgressDialog(this);
        wait.setTitle("Adding " + name);
        wait.setMessage("Reading…");
        wait.setProgressStyle(android.app.ProgressDialog.STYLE_HORIZONTAL);
        wait.setMax(100);
        wait.setCancelable(false);
        wait.show();
        new Thread(() -> {
            String message;
            boolean ok;
            try {
                message = Mods.add(this, uri, name, romFile(),
                        (what, percent) -> runOnUiThread(() -> {
                            wait.setMessage(what + "…");
                            wait.setProgress(percent);
                        }));
                ok = true;
            } catch (Exception e) {
                message = e.getMessage() == null ? e.toString() : e.getMessage();
                ok = false;
            }
            final String text = message;
            final boolean added = ok;
            runOnUiThread(() -> {
                wait.dismiss();
                if (mOptions != null) mOptions.refreshMods();
                AlertDialog.Builder b = new AlertDialog.Builder(this)
                        .setTitle(added ? name + " added" : "The mod could not be added")
                        .setMessage(text);
                if (added) {
                    b.setPositiveButton("Use it now", (d, w) -> {
                        Mods.setActive(this, name);
                        restartGame();
                    }).setNegativeButton("Later", (d, w) -> {
                        if (mOptions != null) mOptions.refreshMods();
                    });
                } else {
                    b.setPositiveButton("OK", null);
                }
                b.show();
            });
        }).start();
    }

    private void toast(String text) {
        android.widget.Toast.makeText(this, text, android.widget.Toast.LENGTH_LONG).show();
    }

    // Starts the app again in a new process: the game reads its options and the save file at its start.
    private void restartGame() {
        if (sStarted) Native.setPaused(true); // writes the save file first
        Intent intent = Intent.makeRestartActivityTask(getComponentName());
        startActivity(intent);
        Runtime.getRuntime().exit(0);
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        if (sStarted) onGameBack();
        else super.onBackPressed();
    }

    // The display's fastest mode at the current resolution: the game is drawn with interpolated frames above 60 Hz.
    private float mRefreshRate = 60f;

    private void preferHighRefreshRate() {
        android.view.Display display = getWindowManager().getDefaultDisplay();
        android.view.Display.Mode current = display.getMode(), best = current;
        boolean high = OptionsScreen.highFrameRate(this);
        for (android.view.Display.Mode m : display.getSupportedModes()) {
            if (m.getPhysicalWidth() != current.getPhysicalWidth() || m.getPhysicalHeight() != current.getPhysicalHeight()) {
                continue;
            }
            // The fastest mode; at 60 fps the one nearest 60 Hz.
            boolean better = high ? m.getRefreshRate() > best.getRefreshRate()
                                  : Math.abs(m.getRefreshRate() - 60f) < Math.abs(best.getRefreshRate() - 60f);
            if (better) best = m;
        }
        mRefreshRate = best.getRefreshRate();
        android.view.WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.preferredDisplayModeId = best.getModeId();
        getWindow().setAttributes(lp);
    }

    // The phone's language among the game's (English otherwise).
    private static int gameLanguage() {
        switch (java.util.Locale.getDefault().getLanguage()) {
            case "fr": return 2;
            case "de": return 3;
            case "it": return 4;
            case "es": return 5;
            default: return 1;
        }
    }

    // Full screen. Android 11 and later: the insets controller; before (Android 8 to 10, API 26-29): the system UI
    // flags (the controller does not exist there: calling it ended the app at its start).
    @SuppressWarnings("deprecation")
    private void hideSystemBars() {
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        }
    }

    // ------------------------------------------------------------------------------------------ online play
    // Online play, the count of players, the voice chat and the player's name: OnlineUi.
    private static final int ASK_MICROPHONE = OnlineUi.ASK_MICROPHONE;
    private boolean mResumed;
    private final OnlineUi mOnline = new OnlineUi(this, new OnlineUi.Host() {
        @Override
        public void onOnline(boolean online) {
            if (mControls != null) mControls.setOnline(online);
        }

        @Override
        public void onPlayers(int players, String names) {
            if (mControls != null) mControls.setPlayers(players, names);
        }

        @Override
        public void onVoice(boolean running, boolean micOn) {
            if (mControls != null) mControls.setVoice(running, micOn);
        }

        @Override
        public void onSettingsClosed() {
            hideSystemBars();
        }
    });

    private void updateVoice() {
        mOnline.update(sStarted && mResumed);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        if (requestCode == ASK_MICROPHONE) updateVoice(); // without the microphone: listening only
    }

    // Local wireless play: the game finds other phones by broadcast on the Wi-Fi network, and its races share every
    // frame's input, so the Wi-Fi must let broadcasts in and not doze between packets.
    private android.net.wifi.WifiManager.MulticastLock mMulticastLock;
    private android.net.wifi.WifiManager.WifiLock mWifiLock;

    private void holdWifi(boolean on) {
        try {
            android.net.wifi.WifiManager wifi =
                    (android.net.wifi.WifiManager) getApplicationContext().getSystemService(WIFI_SERVICE);
            if (wifi == null) return;
            if (mMulticastLock == null) {
                mMulticastLock = wifi.createMulticastLock("mkds-local");
                mMulticastLock.setReferenceCounted(false);
                int mode = android.os.Build.VERSION.SDK_INT >= 29 ? android.net.wifi.WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                                                                   : android.net.wifi.WifiManager.WIFI_MODE_FULL_HIGH_PERF;
                mWifiLock = wifi.createWifiLock(mode, "mkds-local");
                mWifiLock.setReferenceCounted(false);
            }
            if (on) {
                mMulticastLock.acquire();
                mWifiLock.acquire();
            } else {
                mMulticastLock.release();
                mWifiLock.release();
            }
        } catch (RuntimeException e) {
            // no Wi-Fi: local play just finds nobody
        }
    }

    // Dual-screen devices: the touch screen's window on the second display, while the game is in front.
    private static boolean sDual;
    private DualScreen mDualScreen;

    private void showDualScreen(boolean show) {
        if (show && sDual && sStarted && mControls != null && mDualScreen == null) {
            ControlsView controls = mControls;
            mDualScreen = DualScreen.show(this, controls::setExternalTouch);
        } else if (!show && mDualScreen != null) {
            mDualScreen.dismiss();
            mDualScreen = null;
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        mResumed = true;
        showDualScreen(true);
        updateVoice();
        holdWifi(true);
        if (sStarted && mOptions == null) Native.setPaused(false);
    }

    @Override
    protected void onPause() {
        super.onPause();
        mResumed = false;
        showDualScreen(false);
        updateVoice();
        holdWifi(false);
        if (sStarted) Native.setPaused(true);
    }

    // ------------------------------------------------------------------------------------------ surface
    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            holder.getSurface().setFrameRate(mRefreshRate, android.view.Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        mControls.layoutScreens(width, height);
        Native.setSurface(holder.getSurface());
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        Native.setSurface(null);
    }

    // ------------------------------------------------------------------------------------------ gamepads
    // A gamepad (USB or Bluetooth) hides the on-screen controls; when the last one goes away during a race the game
    // pauses and the on-screen controls come back.
    private final android.hardware.input.InputManager.InputDeviceListener mDeviceListener =
            new android.hardware.input.InputManager.InputDeviceListener() {
                @Override
                public void onInputDeviceAdded(int deviceId) { updateGamepads(false); }

                @Override
                public void onInputDeviceRemoved(int deviceId) { updateGamepads(true); }

                @Override
                public void onInputDeviceChanged(int deviceId) { updateGamepads(false); }
            };
    private boolean mGamepad;

    // A real gamepad: some phones list built-in devices (sensors, fingerprint readers, a "virtual joystick") with
    // gamepad or joystick sources, so the device must also be external (Android 10+) and have gamepad buttons.
    private static boolean isGamepad(InputDevice d) {
        if (d == null || d.isVirtual()) return false;
        int s = d.getSources();
        boolean source = (s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
        if (!source) return false;
        if (android.os.Build.VERSION.SDK_INT >= 29 && !d.isExternal()) return false;
        boolean[] keys = d.hasKeys(KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B);
        return keys[0] || keys[1];
    }

    /** The player used a gamepad: its controls take over (the on-screen ones hide) until the screen is touched. */
    private void gamepadUsed() {
        if (mControls != null && !mControls.isGamepadMode()) mControls.setGamepadMode(true);
    }

    private void updateGamepads(boolean removed) {
        boolean any = false;
        for (int id : InputDevice.getDeviceIds()) {
            if (isGamepad(InputDevice.getDevice(id))) any = true;
        }
        if (any == mGamepad) return;
        mGamepad = any;
        if (mControls == null) return;
        if (!any) {
            mPadButtons = 0;
            mControls.setPadButtons(0);
            mControls.setStickButtons(0);
            if (removed && Native.isRace()) mControls.pulse(Native.START); // pause
        }
        mControls.setGamepadMode(any);
    }

    @Override
    protected void onStart() {
        super.onStart();
        android.hardware.input.InputManager im = getSystemService(android.hardware.input.InputManager.class);
        im.registerInputDeviceListener(mDeviceListener, null);
        updateGamepads(false);
    }

    @Override
    protected void onStop() {
        super.onStop();
        getSystemService(android.hardware.input.InputManager.class).unregisterInputDeviceListener(mDeviceListener);
    }

    private int buttonFor(int keyCode) {
        int mapped = GamepadMapping.buttonFor(this, keyCode);
        if (mapped >= 0) return mapped;
        return defaultButtonFor(keyCode);
    }

    static int defaultButtonFor(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A: return Native.A;
            case KeyEvent.KEYCODE_BUTTON_B: return Native.B;
            case KeyEvent.KEYCODE_BUTTON_X: return Native.X;
            case KeyEvent.KEYCODE_BUTTON_Y: return Native.Y;
            case KeyEvent.KEYCODE_BUTTON_L1:
            case KeyEvent.KEYCODE_BUTTON_L2: return Native.L;
            case KeyEvent.KEYCODE_BUTTON_R1:
            case KeyEvent.KEYCODE_BUTTON_R2: return Native.R;
            case KeyEvent.KEYCODE_BUTTON_START: return Native.START;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return Native.SELECT;
            case KeyEvent.KEYCODE_DPAD_UP: return Native.UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return Native.DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT: return Native.LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return Native.RIGHT;
            default: return 0;
        }
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (mLanguageScreen != null && mLanguageScreen.onKey(event)) return true;
        if (mOptions != null) return mOptions.onKey(event) || super.dispatchKeyEvent(event);
        if (mControls != null && mControls.isEditing()) {
            if (event.getKeyCode() == KeyEvent.KEYCODE_BUTTON_B && event.getAction() == KeyEvent.ACTION_DOWN) {
                mControls.cancelEditing();
            }
            return true;
        }
        if (event.getKeyCode() == KeyEvent.KEYCODE_BUTTON_MODE && event.getAction() == KeyEvent.ACTION_DOWN) {
            showOptions(); // the gamepad's home / guide button
            return true;
        }
        int button = buttonFor(event.getKeyCode());
        if (button == 0 || mControls == null) return super.dispatchKeyEvent(event);
        if (event.isFromSource(InputDevice.SOURCE_GAMEPAD) || event.isFromSource(InputDevice.SOURCE_JOYSTICK)) {
            gamepadUsed();
        }
        if (event.getAction() == KeyEvent.ACTION_DOWN) mPadButtons |= button;
        else if (event.getAction() == KeyEvent.ACTION_UP) mPadButtons &= ~button;
        mControls.setPadButtons(mPadButtons);
        return true;
    }

    // The options screen with a gamepad whose D-pad or stick are axes (most of them): moves as D-pad keys, once per
    // push (issue #12).
    private int mMenuAxes;

    private boolean optionsAxes(MotionEvent event) {
        float x = event.getAxisValue(MotionEvent.AXIS_X) + event.getAxisValue(MotionEvent.AXIS_HAT_X);
        float y = event.getAxisValue(MotionEvent.AXIS_Y) + event.getAxisValue(MotionEvent.AXIS_HAT_Y);
        int dirs = 0;
        if (x < -0.5f) dirs |= Native.LEFT;
        if (x > 0.5f) dirs |= Native.RIGHT;
        if (y < -0.5f) dirs |= Native.UP;
        if (y > 0.5f) dirs |= Native.DOWN;
        int pressed = dirs & ~mMenuAxes;
        mMenuAxes = dirs;
        if ((pressed & Native.UP) != 0) mOptions.navigate(KeyEvent.KEYCODE_DPAD_UP);
        if ((pressed & Native.DOWN) != 0) mOptions.navigate(KeyEvent.KEYCODE_DPAD_DOWN);
        if ((pressed & Native.LEFT) != 0) mOptions.navigate(KeyEvent.KEYCODE_DPAD_LEFT);
        if ((pressed & Native.RIGHT) != 0) mOptions.navigate(KeyEvent.KEYCODE_DPAD_RIGHT);
        return true;
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (mOptions != null && (event.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                && event.getAction() == MotionEvent.ACTION_MOVE) {
            return optionsAxes(event);
        }
        if (mControls != null && (event.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                && event.getAction() == MotionEvent.ACTION_MOVE) {
            float x = event.getAxisValue(MotionEvent.AXIS_X) + event.getAxisValue(MotionEvent.AXIS_HAT_X);
            float y = event.getAxisValue(MotionEvent.AXIS_Y) + event.getAxisValue(MotionEvent.AXIS_HAT_Y);
            int stick = 0;
            if (x < -0.4f) stick |= Native.LEFT;
            if (x > 0.4f) stick |= Native.RIGHT;
            if (y < -0.4f) stick |= Native.UP;
            if (y > 0.4f) stick |= Native.DOWN;
            // Triggers: some gamepads report them as brake and gas.
            if (Math.max(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE)) > 0.5f) {
                stick |= Native.L;
            }
            if (Math.max(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS)) > 0.5f) {
                stick |= Native.R;
            }
            if (stick != 0) gamepadUsed();
            mControls.setStickButtons(stick);
            return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }
}
