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

    /** Reserves the DS memory map, opens the ROM and starts the game thread. Returns an error message or null. */
    static native String start(String romPath, String savePath);

    static native void setSurface(Surface surface);

    /** Screen rectangles in surface pixels: top screen, bottom screen (x, y, width, height each). */
    static native void setLayout(int tx, int ty, int tw, int th, int bx, int by, int bw, int bh);

    static native void setInput(int buttons, boolean touching, int touchX, int touchY);

    static native void setPaused(boolean paused);

    private Native() {}
}
