package com.mkds.port;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

/**
 * Screen layout and touch controls. Landscape: the top screen fills the height on the left, the bottom (touch) screen
 * sits at the top right; the on-screen buttons are drawn over the free areas. Touches on the bottom screen go to the
 * game as stylus input.
 */
final class ControlsView extends View {
    private static final class Button {
        final int bits;
        final String label;
        final RectF rect = new RectF();

        Button(int bits, String label) {
            this.bits = bits;
            this.label = label;
        }
    }

    private final Button[] mButtons = {
        new Button(Native.UP, "▲"), new Button(Native.DOWN, "▼"), new Button(Native.LEFT, "◀"),
        new Button(Native.RIGHT, "▶"), new Button(Native.UP | Native.LEFT, ""), new Button(Native.UP | Native.RIGHT, ""),
        new Button(Native.DOWN | Native.LEFT, ""), new Button(Native.DOWN | Native.RIGHT, ""),
        new Button(Native.A, "A"), new Button(Native.B, "B"), new Button(Native.X, "X"), new Button(Native.Y, "Y"),
        new Button(Native.L, "L"), new Button(Native.R, "R"), new Button(Native.START, "START"),
        new Button(Native.SELECT, "SELECT"),
    };
    private final RectF mTop = new RectF(), mBottom = new RectF();
    private final Paint mFill = new Paint(Paint.ANTI_ALIAS_FLAG), mText = new Paint(Paint.ANTI_ALIAS_FLAG);
    private int mTouchButtons, mPadButtons, mStickButtons;
    private boolean mTouching;
    private int mTouchX, mTouchY;

    ControlsView(Context context) {
        super(context);
        mFill.setColor(0x40ffffff);
        mText.setColor(0xa0ffffff);
        mText.setTextAlign(Paint.Align.CENTER);
    }

    void layoutScreens(int w, int h) {
        float topW = h * 4f / 3f;
        if (topW > w * 0.62f) topW = w * 0.62f;
        float topH = topW * 3f / 4f;
        mTop.set(0, (h - topH) / 2, topW, (h + topH) / 2);
        float bw = w - topW, bh = bw * 3f / 4f;
        if (bh > h * 0.62f) {
            bh = h * 0.62f;
            bw = bh * 4f / 3f;
        }
        mBottom.set(w - bw, 0, w, bh);
        Native.setLayout(Math.round(mTop.left), Math.round(mTop.top), Math.round(mTop.width()), Math.round(mTop.height()),
                Math.round(mBottom.left), Math.round(mBottom.top), Math.round(mBottom.width()),
                Math.round(mBottom.height()));

        float u = h / 9f; // button unit
        mText.setTextSize(u * 0.45f);
        // D-pad, bottom left over the top screen.
        float cx = u * 1.8f, cy = h - u * 1.8f;
        set(0, cx, cy - u, u); // up
        set(1, cx, cy + u, u); // down
        set(2, cx - u, cy, u); // left
        set(3, cx + u, cy, u); // right
        set(4, cx - u, cy - u, u);
        set(5, cx + u, cy - u, u);
        set(6, cx - u, cy + u, u);
        set(7, cx + u, cy + u, u);
        // A B X Y, bottom right under the touch screen.
        float ax = w - u * 1.6f, ay = h - u * 2.0f;
        set(8, ax + u * 0.0f, ay, u * 1.1f);            // A
        set(9, ax - u * 1.1f, ay + u * 1.0f, u * 1.1f); // B
        set(10, ax - u * 1.1f, ay - u * 1.0f, u * 1.1f); // X
        set(11, ax - u * 2.2f, ay, u * 1.1f);            // Y
        // L, R, START, SELECT.
        mButtons[12].rect.set(0, mTop.top, u * 2.2f, mTop.top + u * 1.1f);
        mButtons[13].rect.set(mBottom.left - u * 2.2f, mTop.top, mBottom.left, mTop.top + u * 1.1f);
        mButtons[14].rect.set(mBottom.left, h - u * 0.9f, mBottom.left + u * 2.2f, h);
        mButtons[15].rect.set(mBottom.left, h - u * 1.9f, mBottom.left + u * 2.2f, h - u * 1.0f);
        invalidate();
    }

    private void set(int i, float x, float y, float size) {
        mButtons[i].rect.set(x - size / 2, y - size / 2, x + size / 2, y + size / 2);
    }

    void setPadButtons(int b) {
        mPadButtons = b;
        send();
    }

    void setStickButtons(int b) {
        mStickButtons = b;
        send();
    }

    private void send() {
        Native.setInput(mTouchButtons | mPadButtons | mStickButtons, mTouching, mTouchX, mTouchY);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int buttons = 0;
        boolean touching = false;
        int action = e.getActionMasked();
        for (int i = 0; i < e.getPointerCount(); i++) {
            if ((action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) && i == e.getActionIndex()) {
                continue;
            }
            if (action == MotionEvent.ACTION_CANCEL) break;
            float x = e.getX(i), y = e.getY(i);
            boolean onButton = false;
            for (Button b : mButtons) {
                if (b.rect.contains(x, y)) {
                    buttons |= b.bits;
                    onButton = true;
                }
            }
            if (!onButton && mBottom.contains(x, y)) {
                touching = true;
                mTouchX = Math.min(255, Math.max(0, (int) ((x - mBottom.left) * 256 / mBottom.width())));
                mTouchY = Math.min(191, Math.max(0, (int) ((y - mBottom.top) * 192 / mBottom.height())));
            }
        }
        if (buttons != mTouchButtons) {
            performHapticFeedback(android.view.HapticFeedbackConstants.VIRTUAL_KEY);
        }
        mTouchButtons = buttons;
        mTouching = touching;
        send();
        invalidate();
        return true;
    }

    @Override
    protected void onDraw(Canvas c) {
        int pressed = mTouchButtons | mPadButtons | mStickButtons;
        for (Button b : mButtons) {
            if (b.label.isEmpty()) continue;
            mFill.setColor((pressed & b.bits) == b.bits ? 0x70ffffff : 0x30ffffff);
            c.drawRoundRect(b.rect, b.rect.height() / 4, b.rect.height() / 4, mFill);
            c.drawText(b.label, b.rect.centerX(), b.rect.centerY() - (mText.descent() + mText.ascent()) / 2, mText);
        }
    }
}
