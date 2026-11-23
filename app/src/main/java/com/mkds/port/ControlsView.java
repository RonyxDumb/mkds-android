package com.mkds.port;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.SparseArray;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

/**
 * Screen layout and touch controls for playing on a phone in landscape.
 *
 * The top screen fills the display in widescreen (the game draws the wider view), the touch screen sits as an inset
 * in the top right corner; the swap button exchanges them (the touch screen large and centred, for menus that are
 * easier to tap).
 * Controls:
 *  - left half: a floating stick that appears where the thumb lands and stays with that finger (the D-pad);
 *  - right side: a big A with R right above it (one thumb between them holds both: accelerate and drift), B, L (item),
 *    X and Y; a finger can slide from button to button;
 *  - top: SELECT, START, the screen swap and, outside races, NET (the network settings: local or online play);
 *    with voice chat on, MIC (the microphone on or off).
 * Touches on the touch screen go to the game as stylus input.
 */
final class ControlsView extends View {
    private static final class Button {
        final int bits;
        String label;
        float x, y, r;

        Button(int bits, String label) {
            this.bits = bits;
            this.label = label;
        }

        boolean hit(float px, float py, float scale) {
            float dx = px - x, dy = py - y, rr = r * scale;
            return dx * dx + dy * dy <= rr * rr;
        }
    }

    private static final int SWAP = 1 << 30; // not a DS button
    private static final int NET = 1 << 29;  // not a DS button: opens the network settings
    private static final int MIC = 1 << 28;  // not a DS button: the voice chat's microphone
    private static final int SCREENS = 1 << 27; // not a DS button: one or two screens
    private static final int OPT = 1 << 26; // not a DS button: the options screen
    private static final int OWN = SWAP | NET | MIC | SCREENS | OPT;

    private final Button mA = new Button(Native.A, "A"), mB = new Button(Native.B, "B"), mX = new Button(Native.X, "X"),
            mY = new Button(Native.Y, "Y"), mL = new Button(Native.L, "L"), mR = new Button(Native.R, "R"),
            mStart = new Button(Native.START, "START"), mSelect = new Button(Native.SELECT, "SELECT"),
            mSwap = new Button(SWAP, "⇄"), mNet = new Button(NET, "NET"),
            mMic = new Button(MIC, "MIC"), mScreens = new Button(SCREENS, "2"), mOpt = new Button(OPT, "⚙");
    private final Button[] mButtons = {mA, mB, mX, mY, mL, mR, mStart, mSelect, mSwap, mNet, mMic, mScreens, mOpt};
    private Runnable mOnNetwork, mOnMic, mOnOptions;
    // From the options screen (OptionsScreen): see applyOptions.
    private boolean mWide = true, mTouchAlways, mAutoAccel, mBigDrift, mHaptics = true;
    private float mSizeScale = 1f, mOpacity = 1f;
    private long mAReleaseUntil; // auto-accelerate: A let go briefly when the A button is touched (a fresh press)
    private boolean mOnline, mVoice, mMicOn;
    private int mPlayers; // online in the room, this phone included; 0 = not known yet, -1 = the server is silent
    private String mPlayerNames = ""; // the others' names

    // What each finger does, by pointer id.
    private static final int ROLE_BUTTONS = 0, ROLE_STICK = 1, ROLE_STYLUS = 2;

    private static final class Finger {
        int role;
        float x0, y0, x, y;
    }

    private final SparseArray<Finger> mFingers = new SparseArray<>();
    private final RectF mTopRect = new RectF(), mBottomRect = new RectF();
    private RectF mTouchRect = mBottomRect;
    private boolean mSwapped;
    // One screen: only the screen that matters is shown, the other one is not drawn at all: the top screen in
    // races, the touch screen elsewhere (the menus' choices are there). The swap button shows the other one instead.
    private boolean mSingle;
    private boolean mShownRace; // what the layout was made for
    private final Runnable mWatchRace = new Runnable() {
        @Override
        public void run() {
            if (mSingle && Native.isRace() != mShownRace) {
                mSwapped = false; // each change of scene starts with the screen that matters
                applyScreens();
                invalidate();
            }
            if (mAutoAccel) send(); // A follows races starting and ending
            postDelayed(this, 200);
        }
    };
    private int mWidth, mHeight;
    private float mUnit;
    private final Paint mFill = new Paint(Paint.ANTI_ALIAS_FLAG), mText = new Paint(Paint.ANTI_ALIAS_FLAG),
            mRing = new Paint(Paint.ANTI_ALIAS_FLAG);
    private int mTouchButtons, mPadButtons, mStickButtons;
    private boolean mTouching;
    private int mTouchX, mTouchY;

    ControlsView(Context context) {
        super(context);
        readOptions();
        post(mWatchRace);
        mText.setColor(0xc0ffffff);
        mText.setTextAlign(Paint.Align.CENTER);
        mText.setFakeBoldText(true);
        mRing.setStyle(Paint.Style.STROKE);
        mRing.setColor(0x60ffffff);
    }

    private void readOptions() {
        Context c = getContext();
        mSingle = OptionsScreen.prefs(c).getBoolean("single_screen", false);
        mScreens.label = mSingle ? "1" : "2";
        mWide = OptionsScreen.widescreen(c);
        mTouchAlways = OptionsScreen.touchAlways(c);
        mAutoAccel = OptionsScreen.autoAccelerate(c);
        mBigDrift = OptionsScreen.bigDrift(c);
        mHaptics = OptionsScreen.haptics(c);
        mSizeScale = OptionsScreen.buttonSizePercent(c) / 100f;
        mOpacity = OptionsScreen.buttonOpacityPercent(c) / 100f;
    }

    /** The options screen changed something: read the options again and lay out anew. */
    void applyOptions() {
        readOptions();
        mSwapped = false;
        if (mWidth > 0) layoutScreens(mWidth, mHeight);
        send();
    }

    /** What the options button does. */
    void setOnOptions(Runnable r) {
        mOnOptions = r;
    }

    void layoutScreens(int w, int h) {
        mWidth = w;
        mHeight = h;
        float u = mUnit = Math.min(h / 8f, w / 15f);
        Native.setWideAspect(mWide ? (float) w / h : 4f / 3f);
        applyScreens();

        float b = u * mSizeScale; // the game's buttons, at the chosen size
        if (mBigDrift) {
            // A in the corner, a large R to its left (with auto-accelerate the thumb rests there); B above A, L above R.
            place(mA, w - b * 1.45f, h - b * 1.45f, b * 0.9f);
            place(mR, w - b * 3.75f, h - b * 1.55f, b * 1.15f);
            place(mB, w - b * 1.45f, h - b * 3.35f, b * 0.75f);
            place(mL, w - b * 3.75f, h - b * 3.75f, b * 0.75f);
            place(mX, w - b * 2.6f, h - b * 4.9f, b * 0.5f);
            place(mY, w - b * 5.75f, h - b * 2.8f, b * 0.5f);
        } else {
            // Right hand: A big, R above it, close enough for one thumb to cover both; B to the left of A; L and X/Y
            // above B.
            place(mA, w - b * 1.45f, h - b * 1.55f, b * 0.95f);
            place(mR, w - b * 1.45f, h - b * 3.5f, b * 0.9f);
            place(mB, w - b * 3.45f, h - b * 1.1f, b * 0.75f);
            place(mL, w - b * 3.45f, h - b * 2.9f, b * 0.7f);
            place(mX, w - b * 3.1f, h - b * 4.55f, b * 0.5f);
            place(mY, w - b * 4.75f, h - b * 2.0f, b * 0.5f);
        }
        // Top: SELECT, START, swap.
        place(mSelect, w / 2f - u * 1.3f, u * 0.4f, u * 0.38f);
        place(mStart, w / 2f + u * 1.3f, u * 0.4f, u * 0.38f);
        place(mSwap, u * 0.6f, u * 0.6f, u * 0.45f);
        place(mNet, u * 1.75f, u * 0.6f, u * 0.45f);
        place(mMic, u * 0.6f, u * 1.75f, u * 0.45f);
        place(mScreens, u * 0.6f, u * 2.9f, u * 0.45f);
        place(mOpt, w / 2f + u * 2.6f, u * 0.4f, u * 0.38f);
        mText.setTextSize(u * 0.42f);
        mRing.setStrokeWidth(u * 0.06f);
        invalidate();
    }

    private void applyScreens() {
        int w = mWidth, h = mHeight;
        if (w == 0) return;
        RectF top = mTopRect, bottom = mBottomRect;
        if (mSingle) {
            mShownRace = Native.isRace();
            boolean showTop = mShownRace != mSwapped;
            if (showTop) {
                if (mWide) top.set(0, 0, w, h); // widescreen
                else top.set((w - h * 4f / 3f) / 2f, 0, (w + h * 4f / 3f) / 2f, h);
                bottom.set(0, 0, 0, 0);
            } else {
                bottom.set((w - h * 4f / 3f) / 2f, 0, (w + h * 4f / 3f) / 2f, h);
                top.set(0, 0, 0, 0);
            }
            mTouchRect = bottom;
            Native.setLayout(Math.round(top.left), Math.round(top.top), Math.round(top.width()), Math.round(top.height()),
                    Math.round(bottom.left), Math.round(bottom.top), Math.round(bottom.width()),
                    Math.round(bottom.height()));
            return;
        }
        // The main screen fills the display (the top one is widescreen; the touch screen keeps 4:3, centred); the
        // other one is an inset in the top right corner, as wide as the side margin of a 4:3 picture or more.
        float ih = Math.max(h * 0.32f, (w - h * 4f / 3f) / 2f * 3f / 4f), iw = ih * 4f / 3f;
        RectF main = mSwapped ? bottom : top, inset = mSwapped ? top : bottom;
        if (mSwapped) main.set((w - h * 4f / 3f) / 2f, 0, (w + h * 4f / 3f) / 2f, h);
        else if (mWide) main.set(0, 0, w, h);
        else main.set((w - h * 4f / 3f) / 2f, 0, (w + h * 4f / 3f) / 2f, h);
        inset.set(w - iw, 0, w, ih);
        mTouchRect = bottom;
        Native.setLayout(Math.round(top.left), Math.round(top.top), Math.round(top.width()), Math.round(top.height()),
                Math.round(bottom.left), Math.round(bottom.top), Math.round(bottom.width()), Math.round(bottom.height()));
    }

    private static void place(Button b, float x, float y, float r) {
        b.x = x;
        b.y = y;
        b.r = r;
    }

    private int mPulseButtons;
    private boolean mGamepadMode; // a gamepad is connected: the on-screen buttons and stick are hidden

    /** What the NET button does. */
    void setOnNetwork(Runnable r) {
        mOnNetwork = r;
    }

    /** Online play is set up: the NET button shows it. */
    void setOnline(boolean online) {
        mOnline = online;
        invalidate();
    }

    /** How many players are online in the room (OnlinePresence): shown next to NET. */
    void setPlayers(int players, String names) {
        mPlayers = players;
        mPlayerNames = names;
        invalidate();
    }

    /** What the MIC button does. */
    void setOnMic(Runnable r) {
        mOnMic = r;
    }

    /** Voice chat is running (the MIC button is there), with the microphone on or off. */
    void setVoice(boolean voice, boolean micOn) {
        mVoice = voice;
        mMicOn = micOn;
        invalidate();
    }

    // The buttons of the game are hidden with a gamepad (not the screen buttons); NET is hidden during races, MIC
    // without voice chat.
    private boolean hidden(Button b, boolean race) {
        if (b.bits == MIC) return !mVoice;
        return b.bits == NET ? race : mGamepadMode && !mTouchAlways && (b.bits & (SWAP | SCREENS | OPT)) == 0;
    }

    boolean isGamepadMode() {
        return mGamepadMode;
    }

    void setGamepadMode(boolean on) {
        mGamepadMode = on;
        mFingers.clear();
        mTouchButtons = 0;
        send();
        invalidate();
    }

    /** Presses buttons for a moment (the system back gesture pauses a race with START). */
    void pulse(int buttons) {
        mPulseButtons |= buttons;
        send();
        postDelayed(() -> {
            mPulseButtons &= ~buttons;
            send();
        }, 120);
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
        int buttons = mTouchButtons | mPadButtons | mStickButtons | mPulseButtons;
        // Auto-accelerate: A held in races, except while braking (B) and for a moment when A itself is touched, so
        // that the touch is a fresh press (the pause menu and the end of a race take A).
        if (mAutoAccel && Native.isRace() && (buttons & Native.B) == 0) {
            if (android.os.SystemClock.uptimeMillis() >= mAReleaseUntil) buttons |= Native.A;
            else buttons &= ~Native.A;
        }
        Native.setInput(buttons, mTouching, mTouchX, mTouchY);
    }

    // Buttons under a point: slightly larger than drawn, so that a thumb between A and R holds both.
    private int buttonsAt(float x, float y) {
        int bits = 0;
        boolean race = Native.isRace();
        for (Button b : mButtons) {
            if (hidden(b, race)) continue;
            if (b.hit(x, y, 1.25f)) bits |= b.bits;
        }
        return bits;
    }

    private int stickDirection(Finger f) {
        float dx = f.x - f.x0, dy = f.y - f.y0, dead = mUnit * 0.3f;
        int bits = 0;
        // Wide left/right sectors (steering), narrower up/down.
        if (Math.abs(dx) > dead && Math.abs(dx) > Math.abs(dy) * 0.45f) bits |= dx < 0 ? Native.LEFT : Native.RIGHT;
        if (Math.abs(dy) > dead && Math.abs(dy) > Math.abs(dx) * 0.45f) bits |= dy < 0 ? Native.UP : Native.DOWN;
        return bits;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        int index = e.getActionIndex();
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            float x = e.getX(index), y = e.getY(index);
            Finger f = new Finger();
            f.x0 = f.x = x;
            f.y0 = f.y = y;
            int bits = buttonsAt(x, y);
            if (bits != 0) {
                f.role = ROLE_BUTTONS;
                if ((bits & SCREENS) != 0) {
                    mSingle = !mSingle;
                    mSwapped = false;
                    mScreens.label = mSingle ? "1" : "2";
                    getContext().getSharedPreferences("mkds", Context.MODE_PRIVATE).edit()
                            .putBoolean("single_screen", mSingle).apply();
                    applyScreens();
                } else if ((bits & SWAP) != 0) {
                    mSwapped = !mSwapped;
                    applyScreens();
                } else if ((bits & NET) != 0 && mOnNetwork != null) {
                    mOnNetwork.run();
                } else if ((bits & MIC) != 0 && mOnMic != null) {
                    mOnMic.run();
                } else if ((bits & OPT) != 0 && mOnOptions != null) {
                    post(mOnOptions);
                }
            } else if (mTouchRect.width() > 0 && mTouchRect.contains(x, y)) {
                f.role = ROLE_STYLUS;
            } else if (mGamepadMode) {
                // The screen touched outside the touch screen with the gamepad's controls on: the on-screen controls
                // come back (also when a built-in device was taken for a gamepad).
                mGamepadMode = false;
                f.role = x < mWidth * 0.5f ? ROLE_STICK : ROLE_BUTTONS;
            } else if (x < mWidth * 0.5f) {
                f.role = ROLE_STICK;
            } else {
                f.role = ROLE_BUTTONS;
            }
            mFingers.put(e.getPointerId(index), f);
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            mFingers.remove(e.getPointerId(index));
        } else if (action == MotionEvent.ACTION_CANCEL) {
            mFingers.clear();
        }
        for (int i = 0; i < e.getPointerCount(); i++) {
            Finger f = mFingers.get(e.getPointerId(i));
            if (f == null) continue;
            f.x = e.getX(i);
            f.y = e.getY(i);
            if (f.role == ROLE_STICK) {
                // The stick follows a thumb that moves far: its centre stays within reach.
                float dx = f.x - f.x0, dy = f.y - f.y0, max = mUnit * 1.2f, d = (float) Math.hypot(dx, dy);
                if (d > max) {
                    f.x0 = f.x - dx * max / d;
                    f.y0 = f.y - dy * max / d;
                }
            }
        }

        int buttons = 0;
        boolean touching = false;
        for (int i = 0; i < mFingers.size(); i++) {
            Finger f = mFingers.valueAt(i);
            switch (f.role) {
            case ROLE_STICK:
                buttons |= stickDirection(f);
                break;
            case ROLE_STYLUS:
                touching = true;
                mTouchX = clamp((int) ((f.x - mTouchRect.left) * 256 / mTouchRect.width()), 255);
                mTouchY = clamp((int) ((f.y - mTouchRect.top) * 192 / mTouchRect.height()), 191);
                break;
            default:
                buttons |= buttonsAt(f.x, f.y) & ~OWN;
                break;
            }
        }
        if (mHaptics && (buttons & ~mTouchButtons) != 0) performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
        if (mAutoAccel && (buttons & ~mTouchButtons & Native.A) != 0) {
            mAReleaseUntil = android.os.SystemClock.uptimeMillis() + 60;
            postDelayed(this::send, 70);
        }
        mTouchButtons = buttons;
        mTouching = touching;
        send();
        invalidate();
        return true;
    }

    private static int clamp(int v, int max) {
        return Math.max(0, Math.min(max, v));
    }

    @Override
    protected void onDraw(Canvas c) {
        int pressed = mTouchButtons | mPadButtons | mStickButtons;
        boolean race = Native.isRace();
        for (Button b : mButtons) {
            if (hidden(b, race)) continue;
            boolean on = (b.bits & OWN) == 0 && (pressed & b.bits) == b.bits;
            mFill.setColor(on ? 0x80ffffff : b.bits == NET && mOnline ? 0x7030a060
                    : b.bits == MIC ? (mMicOn ? 0x7030a060 : 0x70a03030) : 0x38000000);
            float alpha = (b.bits & OWN) == 0 ? mOpacity : 1f;
            mFill.setAlpha(Math.round(mFill.getAlpha() * alpha));
            mRing.setAlpha(Math.round(0x60 * alpha));
            mText.setAlpha(Math.round(0xc0 * alpha));
            c.drawCircle(b.x, b.y, b.r, mFill);
            c.drawCircle(b.x, b.y, b.r, mRing);
            float size = mText.getTextSize();
            if (b.label.length() > 2) mText.setTextSize(b.r * 0.55f);
            c.drawText(b.label, b.x, b.y - (mText.descent() + mText.ascent()) / 2, mText);
            mText.setTextSize(size);
        }
        mRing.setAlpha(0x60);
        mText.setAlpha(0xc0);
        if (mOnline && !race && mPlayers != 0) {
            // Next to NET: who is there.
            float size = mText.getTextSize();
            mText.setTextSize(mUnit * 0.32f);
            mText.setTextAlign(Paint.Align.LEFT);
            String text = mPlayers < 0 ? "no server" : mPlayers == 1 ? "1 online (you)" : mPlayers + " online";
            if (mPlayers > 1 && !mPlayerNames.isEmpty()) {
                text += ": " + (mPlayerNames.length() > 36 ? mPlayerNames.substring(0, 35) + "…" : mPlayerNames);
            }
            c.drawText(text, mNet.x + mNet.r * 1.3f, mNet.y - (mText.descent() + mText.ascent()) / 2, mText);
            mText.setTextAlign(Paint.Align.CENTER);
            mText.setTextSize(size);
        }
        // The stick under the thumb.
        for (int i = 0; i < mFingers.size(); i++) {
            Finger f = mFingers.valueAt(i);
            if (f.role != ROLE_STICK) continue;
            mFill.setColor(0x30000000);
            c.drawCircle(f.x0, f.y0, mUnit * 1.2f, mFill);
            c.drawCircle(f.x0, f.y0, mUnit * 1.2f, mRing);
            mFill.setColor(0x70ffffff);
            c.drawCircle(f.x, f.y, mUnit * 0.5f, mFill);
        }
        if ((!mGamepadMode || mTouchAlways) && (mFingers.size() == 0 || !hasStick())) {
            // Where the stick goes.
            mRing.setAlpha(0x30);
            c.drawCircle(mUnit * 2.2f, mHeight - mUnit * 2.2f, mUnit * 1.2f, mRing);
            mRing.setAlpha(0x60);
        }
    }

    private boolean hasStick() {
        for (int i = 0; i < mFingers.size(); i++) {
            if (mFingers.valueAt(i).role == ROLE_STICK) return true;
        }
        return false;
    }
}
