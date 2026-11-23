package com.mkds.port;

import android.view.Surface;

/** The native game (libmkds.so, built from port/). */
final class Native {
    static {
        System.loadLibrary("mkds");
    }

    // DS buttons, as in the KEYINPUT register (1 = pressed).
    static final int A = 0x001, B = 0x002, SELECT = 0x004, START = 0x008, RIGHT = 0x010, LEFT = 0x020, UP = 0x040,
            DOWN = 0x080, R = 0x100, L = 0x200, X = 0x400, Y = 0x800;

    /** Installs the crash handler (crash.txt) and the log (log.txt) in dir. */
    static native void init(String dir);

    /** Reserves the DS memory map, opens the ROM and starts the game thread. Returns an error message or null. */
    static native String start(String romPath, String savePath);

    static native void setSurface(Surface surface);

    /** Screen rectangles in surface pixels: top screen, bottom screen (x, y, width, height each). */
    static native void setLayout(int tx, int ty, int tw, int th, int bx, int by, int bw, int bh);

    static native void setInput(int buttons, boolean touching, int touchX, int touchY);

    static native void setPaused(boolean paused);

    /** The options screen: the largest 3D scale (0 = as the display needs) and frame interpolation above 60 Hz. */
    static native void setGraphics(int maxScale, boolean interpolate);

    /** One screen: where the touch screen's part of the picture is (x, y, w, h in surface pixels; w 0: none). */
    static native void touchRect(float[] out);

    /** Widescreen: the aspect ratio of the race view (the whole display). */
    static native void setWideAspect(float aspect);

    /** The console language (before start): 1 English, 2 French, 3 German, 4 Italian, 5 Spanish. */
    static native void setLanguage(int language);

    /** The player's name in multiplayer and races, 10 characters at most; empty for the save file's nickname. */
    static native void setPlayerName(String name);

    /** The game's volume, 0 to 100 percent (the voice chat keeps its own). */
    static native void setGameVolume(int percent);

    /** The player drives a race: after the countdown, before the goal, not in a demo (auto-accelerate). */
    static native boolean isDriving();

    /** Whether the game is showing a race (the layout depends on it). */
    static native boolean isRace();

    // Meta Quest build only (port/platform/android/xr.c).
    /** Starts showing the game in the headset (an OpenXR session for the activity). */
    static native void xrStart(android.app.Activity activity);

    static native void xrStop();

    /** 0 not running, 1 starting, 2 showing the game, -1 failed (xrError says why). */
    static native int xrState();

    static native String xrError();

    /** How often the microphone button of the controller has been pressed. */
    static native int xrMicToggles();

    /** How often the player has asked for the settings (a trigger pulled on the status line). */
    static native int xrSettingsRequests();

    /** The status line shown next to the game: STATUS_WIDTH x STATUS_HEIGHT ARGB pixels. */
    static native void xrSetStatus(int[] pixels);

    static final int XR_STATUS_WIDTH = 880, XR_STATUS_HEIGHT = 120;

    private Native() {}
}
