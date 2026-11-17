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
    private static final int PICK_ROM = 1;
    private static boolean sStarted;

    private ControlsView mControls;
    private int mPadButtons;

    private File romFile() {
        return new File(getFilesDir(), "rom.nds");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getAttributes().layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        Native.init(getFilesDir().getAbsolutePath());
        File crash = new File(getFilesDir(), "crash.txt");
        if (crash.isFile()) {
            showCrashReport(crash);
            return;
        }
        if (isValidRom(romFile())) {
            startGame();
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
        startGame();
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

    private void startGame() {
        if (!sStarted) {
            Native.setLanguage(gameLanguage());
            String err = Native.start(romFile().getAbsolutePath(), new File(getFilesDir(), "mkds.sav").getAbsolutePath());
            if (err != null) {
                new AlertDialog.Builder(this).setTitle("Mario Kart DS").setMessage(err)
                        .setPositiveButton("Quit", (d, w) -> finish()).setCancelable(false).show();
                return;
            }
            sStarted = true;
        }
        FrameLayout root = new FrameLayout(this);
        SurfaceView surface = new SurfaceView(this);
        surface.getHolder().addCallback(this);
        mControls = new ControlsView(this);
        root.addView(surface);
        root.addView(mControls);
        setContentView(root);
        hideSystemBars();
        preferHighRefreshRate();
        if (android.os.Build.VERSION.SDK_INT >= 33) {
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT, this::onGameBack);
        }
    }

    // Back does not leave the game: in a race it pauses (START), elsewhere it does nothing.
    private void onGameBack() {
        if (mControls != null && Native.isRace()) mControls.pulse(Native.START);
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
        for (android.view.Display.Mode m : display.getSupportedModes()) {
            if (m.getPhysicalWidth() == current.getPhysicalWidth() && m.getPhysicalHeight() == current.getPhysicalHeight()
                    && m.getRefreshRate() > best.getRefreshRate()) {
                best = m;
            }
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

    private void hideSystemBars() {
        WindowInsetsController c = getWindow().getInsetsController();
        if (c != null) {
            c.hide(WindowInsets.Type.systemBars());
            c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (sStarted) Native.setPaused(false);
    }

    @Override
    protected void onPause() {
        super.onPause();
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
    private static int buttonFor(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A: return Native.A;
            case KeyEvent.KEYCODE_BUTTON_B: return Native.B;
            case KeyEvent.KEYCODE_BUTTON_X: return Native.X;
            case KeyEvent.KEYCODE_BUTTON_Y: return Native.Y;
            case KeyEvent.KEYCODE_BUTTON_L1: return Native.L;
            case KeyEvent.KEYCODE_BUTTON_R1: return Native.R;
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
        int button = buttonFor(event.getKeyCode());
        if (button == 0 || mControls == null) return super.dispatchKeyEvent(event);
        if (event.getAction() == KeyEvent.ACTION_DOWN) mPadButtons |= button;
        else if (event.getAction() == KeyEvent.ACTION_UP) mPadButtons &= ~button;
        mControls.setPadButtons(mPadButtons);
        return true;
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (mControls != null && (event.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                && event.getAction() == MotionEvent.ACTION_MOVE) {
            float x = event.getAxisValue(MotionEvent.AXIS_X) + event.getAxisValue(MotionEvent.AXIS_HAT_X);
            float y = event.getAxisValue(MotionEvent.AXIS_Y) + event.getAxisValue(MotionEvent.AXIS_HAT_Y);
            int stick = 0;
            if (x < -0.4f) stick |= Native.LEFT;
            if (x > 0.4f) stick |= Native.RIGHT;
            if (y < -0.4f) stick |= Native.UP;
            if (y > 0.4f) stick |= Native.DOWN;
            if (event.getAxisValue(MotionEvent.AXIS_LTRIGGER) > 0.5f) stick |= Native.L;
            if (event.getAxisValue(MotionEvent.AXIS_RTRIGGER) > 0.5f) stick |= Native.R;
            mControls.setStickButtons(stick);
            return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }
}
