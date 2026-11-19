package com.mkds.port;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * The screen shown before the game starts: the game's language, chosen by touch or with a gamepad (D-pad or stick to
 * move, A or START to choose), the last choice selected. A notice appears when a newer version of the app exists.
 */
final class LanguageScreen {
    interface Chosen {
        void onChosen(int index);
    }

    private final Activity mActivity;
    private final LinearLayout mRoot;
    private final Button[] mButtons;
    private final Chosen mChosen;
    private Button mUpdate;
    private boolean mDone;

    LanguageScreen(Activity activity, String[] names, int selected, Chosen chosen) {
        mActivity = activity;
        mChosen = chosen;
        mRoot = new LinearLayout(activity);
        mRoot.setOrientation(LinearLayout.VERTICAL);
        mRoot.setGravity(Gravity.CENTER);
        mRoot.setBackgroundColor(0xff10141c);
        int pad = dp(16);
        mRoot.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(activity);
        title.setText("Mario Kart DS");
        title.setTextColor(Color.WHITE);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 26);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setGravity(Gravity.CENTER);
        mRoot.addView(title);

        TextView subtitle = new TextView(activity);
        subtitle.setText("Language · Langue · Sprache · Lingua · Idioma");
        subtitle.setTextColor(0xffb8c0d0);
        subtitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        subtitle.setGravity(Gravity.CENTER);
        subtitle.setPadding(0, dp(4), 0, dp(14));
        mRoot.addView(subtitle);

        LinearLayout list = new LinearLayout(activity);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setGravity(Gravity.CENTER_HORIZONTAL);
        mButtons = new Button[names.length];
        for (int i = 0; i < names.length; i++) {
            Button b = button(names[i]);
            final int index = i;
            b.setOnClickListener(v -> choose(index));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(260), dp(48));
            lp.topMargin = dp(6);
            list.addView(b, lp);
            mButtons[i] = b;
        }
        ScrollView scroll = new ScrollView(activity);
        scroll.addView(list);
        mRoot.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        Button first = mButtons[Math.max(0, Math.min(selected, mButtons.length - 1))];
        first.post(first::requestFocus);
    }

    View view() {
        return mRoot;
    }

    private int dp(float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, mActivity.getResources().getDisplayMetrics());
    }

    private Button button(String text) {
        Button b = new Button(mActivity);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        b.setFocusable(true);
        b.setFocusableInTouchMode(true);
        StateListDrawable bg = new StateListDrawable();
        bg.addState(new int[] {android.R.attr.state_pressed}, rounded(0xff3a6ee8, 0xffffffff));
        bg.addState(new int[] {android.R.attr.state_focused}, rounded(0xff2f5bc4, 0xffffffff));
        bg.addState(new int[] {}, rounded(0xff222a38, 0xff3a4558));
        b.setBackground(bg);
        return b;
    }

    private GradientDrawable rounded(int fill, int stroke) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        d.setCornerRadius(dp(10));
        d.setStroke(dp(2), stroke);
        return d;
    }

    private void choose(int index) {
        if (mDone) return;
        mDone = true;
        mChosen.onChosen(index);
    }

    /** A newer version exists: a button that opens its page (reachable with the gamepad too). */
    void showUpdate(String version, String url) {
        if (mDone || mUpdate != null) return;
        mUpdate = button("Update available: " + version + " — download");
        mUpdate.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        mUpdate.setOnClickListener(v -> UpdateCheck.open(mActivity, url));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(44));
        lp.topMargin = dp(16);
        mUpdate.setPadding(dp(16), 0, dp(16), 0);
        mRoot.addView(mUpdate, lp);
    }

    /**
     * Gamepad keys: A and START choose what has the focus (Android moves the focus with the D-pad and the stick by
     * itself). True when the key was taken.
     */
    boolean onKey(KeyEvent e) {
        int code = e.getKeyCode();
        boolean confirm = code == KeyEvent.KEYCODE_BUTTON_A || code == KeyEvent.KEYCODE_BUTTON_START
                || code == KeyEvent.KEYCODE_BUTTON_X;
        if (!confirm) return false;
        if (e.getAction() == KeyEvent.ACTION_UP) {
            View focus = mActivity.getCurrentFocus();
            if (focus != null) focus.performClick();
            else if (mButtons.length > 0) mButtons[0].performClick();
        }
        return true;
    }
}
