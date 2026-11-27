package com.mkds.port;

import android.app.Activity;
import android.app.Presentation;
import android.content.Context;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.view.Display;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

/**
 * Devices with two displays (issue #22): with Options > Graphics > Second display on, the game's touch screen is
 * shown on the second display, whole and in its proportions, and touched there; the first display shows the top
 * screen alone, as on a DS.
 */
final class DualScreen {
    interface Touch {
        void touch(boolean touching, int x, int y);
    }

    /** The second display, or null when the device has none. */
    static Display secondDisplay(Context c) {
        DisplayManager dm = c.getSystemService(DisplayManager.class);
        if (dm == null) return null;
        Display[] displays = dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        for (Display d : displays) {
            if (d.getDisplayId() != Display.DEFAULT_DISPLAY) return d;
        }
        for (Display d : dm.getDisplays()) {
            if (d.getDisplayId() != Display.DEFAULT_DISPLAY && d.getState() != Display.STATE_OFF) return d;
        }
        return null;
    }

    private final Screen mScreen;

    private DualScreen(Screen screen) {
        mScreen = screen;
    }

    /** Shows the touch screen on the second display; null when there is none or it cannot be used. */
    static DualScreen show(Activity a, Touch touch) {
        Display d = secondDisplay(a);
        if (d == null) return null;
        try {
            Screen s = new Screen(a, d, touch);
            s.show();
            return new DualScreen(s);
        } catch (RuntimeException e) { // the display went away, or the window cannot be added there
            return null;
        }
    }

    void dismiss() {
        Native.setSecondSurface(null);
        try {
            mScreen.dismiss();
        } catch (RuntimeException ignored) {
        }
    }

    private static final class Screen extends Presentation implements SurfaceHolder.Callback {
        private final Touch mTouch;
        private int mWidth, mHeight;

        Screen(Context outer, Display display, Touch touch) {
            super(outer, display);
            mTouch = touch;
        }

        @Override
        protected void onCreate(Bundle savedInstanceState) {
            super.onCreate(savedInstanceState);
            SurfaceView view = new SurfaceView(getContext());
            view.getHolder().addCallback(this);
            view.setOnTouchListener((v, e) -> {
                onTouch(e);
                return true;
            });
            setContentView(view);
            if (getWindow() != null) {
                getWindow().getDecorView().setSystemUiVisibility(android.view.View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | android.view.View.SYSTEM_UI_FLAG_FULLSCREEN
                        | android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
            }
        }

        // The picture is the touch screen fitted in the display, centred (gl/present.c): touches map onto it.
        private void onTouch(MotionEvent e) {
            int action = e.getActionMasked();
            if (mWidth <= 0 || mHeight <= 0) return;
            float pw = mWidth, ph = mWidth * 3f / 4f;
            if (ph > mHeight) {
                ph = mHeight;
                pw = mHeight * 4f / 3f;
            }
            float left = (mWidth - pw) / 2, top = (mHeight - ph) / 2;
            boolean up = action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL;
            int x = Math.max(0, Math.min(255, (int) ((e.getX() - left) * 256 / pw)));
            int y = Math.max(0, Math.min(191, (int) ((e.getY() - top) * 192 / ph)));
            mTouch.touch(!up, x, y);
        }

        @Override
        public void surfaceCreated(SurfaceHolder holder) {}

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            mWidth = width;
            mHeight = height;
            Native.setSecondSurface(holder.getSurface());
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder holder) {
            Native.setSecondSurface(null);
        }
    }
}
