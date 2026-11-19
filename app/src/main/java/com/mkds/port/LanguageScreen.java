package com.mkds.port;

import android.app.Activity;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

/**
 * The screen shown before the game starts: the game's language, in the look of the game's own menus (a light blue
 * checkered background, a yellow title with a dark outline, slanted blue bars, the selected one lit). Chosen by touch
 * or with a gamepad (D-pad or stick to move, A or START to choose); the last choice is selected. When a newer version
 * of the app exists, a bar at the bottom offers it (touch it, or press X).
 */
final class LanguageScreen {
    interface Chosen {
        void onChosen(int index);
    }

    private final Activity mActivity;
    private final String[] mNames;
    private final Chosen mChosen;
    private final Screen mView;
    private int mSelected;
    private boolean mDone;
    private String mUpdateText, mUpdateUrl;

    LanguageScreen(Activity activity, String[] names, int selected, Chosen chosen) {
        mActivity = activity;
        mNames = names;
        mChosen = chosen;
        mSelected = Math.max(0, Math.min(selected, names.length - 1));
        mView = new Screen();
        mView.setFocusable(true);
        mView.setFocusableInTouchMode(true);
        mView.post(mView::requestFocus);
    }

    View view() {
        return mView;
    }

    void showUpdate(String version, String url) {
        if (mDone) return;
        mUpdateText = "New version " + version + " available — tap here or press X to download";
        mUpdateUrl = url;
        mView.invalidate();
    }

    private void choose(int index) {
        if (mDone) return;
        mDone = true;
        mSelected = index;
        mView.invalidate();
        mView.postDelayed(() -> mChosen.onChosen(index), 120); // the lit bar shows for a moment
    }

    private void move(int by) {
        mSelected = (mSelected + by + mNames.length) % mNames.length;
        mView.invalidate();
    }

    /** Keys from the activity (gamepad and keyboard). True when the key was taken. */
    boolean onKey(KeyEvent e) {
        int code = e.getKeyCode();
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
        switch (code) {
            case KeyEvent.KEYCODE_DPAD_UP:
                if (down) move(-1);
                return true;
            case KeyEvent.KEYCODE_DPAD_DOWN:
                if (down) move(1);
                return true;
            case KeyEvent.KEYCODE_BUTTON_A:
            case KeyEvent.KEYCODE_BUTTON_START:
            case KeyEvent.KEYCODE_DPAD_CENTER:
            case KeyEvent.KEYCODE_ENTER:
                if (!down) choose(mSelected);
                return true;
            case KeyEvent.KEYCODE_BUTTON_X:
                if (!down && mUpdateUrl != null) UpdateCheck.open(mActivity, mUpdateUrl);
                return true;
            default:
                return false;
        }
    }

    private final class Screen extends View {
        private final Paint mPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint mStroke = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path mPath = new Path();
        private final RectF[] mBars;
        private final RectF mUpdateBar = new RectF();
        private final Typeface mTitleFace = Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD_ITALIC);
        private final Typeface mBarFace = Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD);

        Screen() {
            super(mActivity);
            mBars = new RectF[mNames.length];
            for (int i = 0; i < mBars.length; i++) mBars[i] = new RectF();
            mStroke.setStyle(Paint.Style.STROKE);
            mStroke.setStrokeJoin(Paint.Join.ROUND);
        }

        @Override
        protected void onDraw(Canvas c) {
            int w = getWidth(), h = getHeight();
            float u = Math.min(w / 16f, h / 9f);

            // Background: light blue fading down, checker bands at the top and bottom, a faint one in between.
            mPaint.setShader(new LinearGradient(0, 0, 0, h, 0xffeaf6ff, 0xff6fb4ef, Shader.TileMode.CLAMP));
            c.drawRect(0, 0, w, h, mPaint);
            mPaint.setShader(null);
            float sq = u * 0.55f;
            for (int row = 0; row * sq < h; row++) {
                float y = row * sq;
                float fade = Math.max(0f, 1f - Math.min(y, h - y - sq) / (h * 0.28f));
                int alpha = (int) (0x14 + 0x40 * fade);
                for (int col = (row & 1); col * sq < w; col += 2) {
                    mPaint.setColor((alpha << 24) | 0x3a5fd0);
                    c.drawRect(col * sq, y, col * sq + sq, y + sq, mPaint);
                }
            }

            // Title band and title: yellow italic with a dark outline.
            mPaint.setColor(0xcc1b3fa8);
            c.drawRect(0, u * 0.45f, w, u * 1.85f, mPaint);
            mPaint.setColor(0xff8fd6ff);
            c.drawRect(0, u * 1.85f, w, u * 1.95f, mPaint);
            drawOutlined(c, "LANGUAGE", u * 0.7f, u * 1.55f, u * 1.05f, mTitleFace, 0xffffe03a, 0xff10205a, false);

            // Bars.
            int n = mNames.length;
            float barH = Math.min(u * 1.05f, (h - u * 3.4f) / (n * 1.18f)), gap = barH * 0.18f;
            float barW = Math.min(w * 0.55f, u * 9f);
            float x0 = (w - barW) / 2f, y0 = u * 2.55f;
            for (int i = 0; i < n; i++) {
                float y = y0 + i * (barH + gap);
                mBars[i].set(x0, y, x0 + barW, y + barH);
                drawBar(c, mBars[i], mNames[i], i == mSelected, barH);
            }

            // Update notice.
            if (mUpdateText != null) {
                float bh = u * 0.75f;
                mUpdateBar.set(u * 0.4f, h - bh - u * 0.3f, w - u * 0.4f, h - u * 0.3f);
                mPaint.setColor(0xe0ffe03a);
                c.drawRoundRect(mUpdateBar, bh / 2, bh / 2, mPaint);
                mStroke.setColor(0xff10205a);
                mStroke.setStrokeWidth(u * 0.06f);
                c.drawRoundRect(mUpdateBar, bh / 2, bh / 2, mStroke);
                mPaint.setColor(0xff10205a);
                mPaint.setTypeface(mBarFace);
                mPaint.setTextSize(bh * 0.42f);
                mPaint.setTextAlign(Paint.Align.CENTER);
                c.drawText(mUpdateText, mUpdateBar.centerX(),
                        mUpdateBar.centerY() - (mPaint.descent() + mPaint.ascent()) / 2, mPaint);
                mPaint.setTextAlign(Paint.Align.LEFT);
            }
        }

        // A slanted bar: dark blue with a light edge; the selected one light blue with yellow text.
        private void drawBar(Canvas c, RectF r, String text, boolean on, float barH) {
            float slant = barH * 0.35f;
            mPath.reset();
            mPath.moveTo(r.left + slant, r.top);
            mPath.lineTo(r.right, r.top);
            mPath.lineTo(r.right - slant, r.bottom);
            mPath.lineTo(r.left, r.bottom);
            mPath.close();
            int top = on ? 0xffd8f6ff : 0xff3a63e0, bottom = on ? 0xff5cc8f5 : 0xff1430a8;
            mPaint.setShader(new LinearGradient(0, r.top, 0, r.bottom, top, bottom, Shader.TileMode.CLAMP));
            c.drawPath(mPath, mPaint);
            mPaint.setShader(null);
            // The darker tab on the left, as on the game's menu buttons.
            mPath.reset();
            mPath.moveTo(r.left + slant, r.top);
            mPath.lineTo(r.left + slant + barH * 0.55f, r.top);
            mPath.lineTo(r.left + barH * 0.55f, r.bottom);
            mPath.lineTo(r.left, r.bottom);
            mPath.close();
            mPaint.setColor(on ? 0xff2fa3e6 : 0xff0c2280);
            c.drawPath(mPath, mPaint);
            // Edge.
            mPath.reset();
            mPath.moveTo(r.left + slant, r.top);
            mPath.lineTo(r.right, r.top);
            mPath.lineTo(r.right - slant, r.bottom);
            mPath.lineTo(r.left, r.bottom);
            mPath.close();
            mStroke.setStrokeWidth(barH * (on ? 0.09f : 0.06f));
            mStroke.setColor(on ? 0xffffffff : 0xff9fdcff);
            c.drawPath(mPath, mStroke);
            drawOutlined(c, text, r.left + barH * 1.0f, r.centerY() + barH * 0.17f, barH * 0.48f, mBarFace,
                    on ? 0xffffe03a : 0xffffffff, on ? 0xff10205a : 0xff0a1650, false);
        }

        private void drawOutlined(Canvas c, String text, float x, float y, float size, Typeface face, int fill,
                                  int outline, boolean center) {
            mPaint.setTypeface(face);
            mPaint.setTextSize(size);
            mPaint.setTextAlign(center ? Paint.Align.CENTER : Paint.Align.LEFT);
            mStroke.setTypeface(face);
            mStroke.setTextSize(size);
            mStroke.setTextAlign(center ? Paint.Align.CENTER : Paint.Align.LEFT);
            mStroke.setStrokeWidth(size * 0.16f);
            mStroke.setColor(outline);
            c.drawText(text, x, y, mStroke);
            mPaint.setColor(fill);
            c.drawText(text, x, y, mPaint);
            mPaint.setTextAlign(Paint.Align.LEFT);
        }

        @Override
        public boolean onTouchEvent(MotionEvent e) {
            float x = e.getX(), y = e.getY();
            if (e.getActionMasked() == MotionEvent.ACTION_DOWN || e.getActionMasked() == MotionEvent.ACTION_MOVE) {
                for (int i = 0; i < mBars.length; i++) {
                    if (mBars[i].contains(x, y) && mSelected != i) {
                        mSelected = i;
                        invalidate();
                    }
                }
            } else if (e.getActionMasked() == MotionEvent.ACTION_UP) {
                if (mUpdateText != null && mUpdateBar.contains(x, y)) {
                    UpdateCheck.open(mActivity, mUpdateUrl);
                    return true;
                }
                for (int i = 0; i < mBars.length; i++) {
                    if (mBars[i].contains(x, y)) choose(i);
                }
            }
            return true;
        }
    }
}
