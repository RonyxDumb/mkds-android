package com.mkds.port;

import android.app.Activity;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * The app's options, over the game (which pauses meanwhile), laid out for a wide screen: three columns side by side
 * (graphics, controls, game). Each option is a row of choices; touch one, or move with the D-pad / stick and press A.
 * B, back or the close button leave. Everything is kept in the "mkds" preferences and read through the getters below.
 */
final class OptionsScreen {
    interface Host {
        void onOptionsChanged();
        void onOptionsClosed();
        void exportSave();
        void importSave();
        void restartGame();
        void pinGameIcon();
    }

    static final String PREFS = "mkds";

    // ---------------------------------------------------------------------------------------- stored options
    static SharedPreferences prefs(android.content.Context c) {
        return c.getSharedPreferences(PREFS, android.content.Context.MODE_PRIVATE);
    }

    /** Frames drawn at the display's full refresh rate (interpolated) or at the game's 60. */
    static boolean highFrameRate(android.content.Context c) { return prefs(c).getBoolean("high_fps", true); }

    /** The largest 3D scale: 0 = as the display needs, 1 = DS resolution, 2..4. */
    static int maxScale(android.content.Context c) { return prefs(c).getInt("max_scale", 0); }

    static boolean widescreen(android.content.Context c) { return prefs(c).getBoolean("widescreen", true); }

    /** The on-screen controls stay with a gamepad connected. */
    static boolean touchAlways(android.content.Context c) { return prefs(c).getBoolean("touch_always", false); }

    /** In races, A is held unless B is: the right thumb is free for R (drift) and L. */
    static boolean autoAccelerate(android.content.Context c) { return prefs(c).getBoolean("auto_accel", false); }

    /** Drift button: R separate from A, larger, placed for the thumb (true), or R right above A (false). */
    static boolean bigDrift(android.content.Context c) { return prefs(c).getBoolean("big_drift", false); }

    static int buttonSizePercent(android.content.Context c) { return prefs(c).getInt("button_size", 100); }

    static int buttonOpacityPercent(android.content.Context c) { return prefs(c).getInt("button_opacity", 100); }

    static boolean haptics(android.content.Context c) { return prefs(c).getBoolean("haptics", true); }

    static boolean unlockAll(android.content.Context c) { return prefs(c).getBoolean("unlock_all", false); }

    // ---------------------------------------------------------------------------------------- the screen
    private static final int BLUE = 0xff1f5fd0, BLUE_DARK = 0xff10307a, LIGHT = 0xffd8ecff, YELLOW = 0xffffe040;

    private final Activity mActivity;
    private final Host mHost;
    private final FrameLayout mRoot;
    private final float mDp;
    private View mFirst;

    OptionsScreen(Activity activity, Host host) {
        mActivity = activity;
        mHost = host;
        mDp = activity.getResources().getDisplayMetrics().density;
        mRoot = new FrameLayout(activity);
        mRoot.setBackgroundColor(0xe0081840);
        mRoot.setClickable(true); // nothing reaches the game under it

        LinearLayout page = new LinearLayout(activity);
        page.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(14);
        page.setPadding(pad, dp(8), pad, dp(8));

        // Title band.
        LinearLayout band = new LinearLayout(activity);
        band.setOrientation(LinearLayout.HORIZONTAL);
        band.setGravity(Gravity.CENTER_VERTICAL);
        GradientDrawable bandBg = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                new int[] {BLUE_DARK, BLUE, BLUE_DARK});
        bandBg.setCornerRadius(dp(10));
        band.setBackground(bandBg);
        band.setPadding(dp(16), dp(4), dp(8), dp(4));
        TextView title = new TextView(activity);
        title.setText("OPTIONS");
        title.setTextColor(YELLOW);
        title.setTextSize(22);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setShadowLayer(dp(2), dp(1), dp(1), Color.BLACK);
        band.addView(title, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        View close = chip("✕  Close", true);
        close.setOnClickListener(v -> close());
        band.addView(close);
        page.addView(band, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        // Three columns across the wide screen.
        LinearLayout columns = new LinearLayout(activity);
        columns.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams colsLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1);
        colsLp.topMargin = dp(8);
        page.addView(columns, colsLp);

        LinearLayout graphics = column(columns, "Graphics");
        choice(graphics, "Frame rate", new String[] {"60 fps", "Display max"}, highFrameRate(activity) ? 1 : 0,
                i -> put("high_fps", i == 1));
        int[] scales = {0, 1, 2, 3, 4};
        choice(graphics, "3D resolution", new String[] {"Auto", "1×", "2×", "3×", "4×"},
                indexOf(scales, maxScale(activity)), i -> putInt("max_scale", scales[i]));
        choice(graphics, "Widescreen", new String[] {"Off (4:3)", "On"}, widescreen(activity) ? 1 : 0,
                i -> put("widescreen", i == 1));
        choice(graphics, "Screens", new String[] {"Two", "One"},
                prefs(activity).getBoolean("single_screen", false) ? 1 : 0, i -> put("single_screen", i == 1));
        note(graphics, "Display max draws extra frames between the game's own (90, 120, 144 Hz). Lower 3D "
                + "resolution and 60 fps save battery on slower phones.");

        LinearLayout controls = column(columns, "Controls");
        choice(controls, "Touch controls", new String[] {"Hide with gamepad", "Always"},
                touchAlways(activity) ? 1 : 0, i -> put("touch_always", i == 1));
        choice(controls, "Auto-accelerate", new String[] {"Off", "On"}, autoAccelerate(activity) ? 1 : 0,
                i -> put("auto_accel", i == 1));
        choice(controls, "Drift button (R)", new String[] {"Above A", "Large, apart"}, bigDrift(activity) ? 1 : 0,
                i -> put("big_drift", i == 1));
        int[] sizes = {80, 100, 120, 140};
        choice(controls, "Button size", new String[] {"80%", "100%", "120%", "140%"},
                indexOf(sizes, buttonSizePercent(activity)), i -> putInt("button_size", sizes[i]));
        int[] opacities = {40, 70, 100};
        choice(controls, "Button opacity", new String[] {"Low", "Medium", "High"},
                indexOf(opacities, buttonOpacityPercent(activity)), i -> putInt("button_opacity", opacities[i]));
        choice(controls, "Vibration", new String[] {"Off", "On"}, haptics(activity) ? 1 : 0,
                i -> put("haptics", i == 1));
        note(controls, "Auto-accelerate holds A in races (B still brakes): drift with R and steer with the "
                + "stick, using one thumb for each.");

        LinearLayout game = column(columns, "Game");
        choice(game, "Unlock everything", new String[] {"Off", "On"}, unlockAll(activity) ? 1 : 0, i -> {
            put("unlock_all", i == 1);
            if (i == 1) {
                new android.app.AlertDialog.Builder(mActivity)
                        .setTitle("Unlock everything")
                        .setMessage("Every cup, class (Mirror included), character and kart: each Grand Prix gets a "
                                + "gold trophy with three stars in your save file. This cannot be undone (export your "
                                + "save first to keep a copy). The game restarts to apply it.")
                        .setPositiveButton("Restart now", (d, w) -> mHost.restartGame())
                        .setNegativeButton("Later", null)
                        .show();
            }
        });
        action(game, "Export save file…", mHost::exportSave);
        action(game, "Import save file…", mHost::importSave);
        action(game, "Home screen icon from the game", mHost::pinGameIcon);
        note(game, "Your progress is saved in the app every few seconds and when you leave it. Keep a copy with "
                + "Export before uninstalling: uninstalling deletes it.");

        mRoot.addView(page, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        mRoot.post(() -> {
            if (mFirst != null) mFirst.requestFocus();
        });
    }

    View view() {
        return mRoot;
    }

    void close() {
        if (mRoot.getParent() instanceof ViewGroup) ((ViewGroup) mRoot.getParent()).removeView(mRoot);
        mHost.onOptionsClosed();
    }

    /** Keys while the screen is open: D-pad moves the focus (Android's), A chooses, B / START close. */
    boolean onKey(KeyEvent e) {
        int code = e.getKeyCode();
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
        switch (code) {
            case KeyEvent.KEYCODE_BUTTON_A:
                if (down) {
                    View f = mRoot.findFocus();
                    if (f != null) f.performClick();
                }
                return true;
            case KeyEvent.KEYCODE_BUTTON_B:
            case KeyEvent.KEYCODE_BUTTON_START:
            case KeyEvent.KEYCODE_BACK:
                if (down) close();
                return true;
            default:
                return false; // D-pad: focus navigation
        }
    }

    // ---------------------------------------------------------------------------------------- building
    private interface Pick {
        void picked(int index);
    }

    private void put(String key, boolean value) {
        prefs(mActivity).edit().putBoolean(key, value).apply();
        mHost.onOptionsChanged();
    }

    private void putInt(String key, int value) {
        prefs(mActivity).edit().putInt(key, value).apply();
        mHost.onOptionsChanged();
    }

    private static int indexOf(int[] values, int v) {
        for (int i = 0; i < values.length; i++) {
            if (values[i] == v) return i;
        }
        return 0;
    }

    private int dp(float v) {
        return Math.round(v * mDp);
    }

    private LinearLayout column(LinearLayout columns, String heading) {
        ScrollView scroll = new ScrollView(mActivity);
        scroll.setFillViewport(true);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0x40000000);
        bg.setCornerRadius(dp(10));
        scroll.setBackground(bg);
        LinearLayout col = new LinearLayout(mActivity);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(10), dp(6), dp(10), dp(10));
        TextView h = new TextView(mActivity);
        h.setText(heading);
        h.setTextColor(YELLOW);
        h.setTextSize(17);
        h.setTypeface(Typeface.DEFAULT_BOLD);
        col.addView(h);
        scroll.addView(col);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1);
        if (columns.getChildCount() > 0) lp.leftMargin = dp(8);
        columns.addView(scroll, lp);
        return col;
    }

    private TextView label(LinearLayout col, String text) {
        TextView t = new TextView(mActivity);
        t.setText(text);
        t.setTextColor(LIGHT);
        t.setTextSize(13);
        t.setPadding(0, dp(8), 0, dp(3));
        col.addView(t);
        return t;
    }

    private void note(LinearLayout col, String text) {
        TextView t = label(col, text);
        t.setTextSize(11);
        t.setTextColor(0xa0d8ecff);
        t.setPadding(0, dp(10), 0, 0);
    }

    private TextView chip(String text, boolean selected) {
        TextView t = new TextView(mActivity);
        t.setText(text);
        t.setTextSize(13);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setGravity(Gravity.CENTER);
        t.setPadding(dp(10), dp(6), dp(10), dp(6));
        t.setFocusable(true);
        t.setClickable(true);
        styleChip(t, selected);
        return t;
    }

    private void styleChip(TextView t, boolean selected) {
        GradientDrawable on = new GradientDrawable(), off = new GradientDrawable(), focus = new GradientDrawable();
        on.setColor(BLUE);
        on.setStroke(dp(2), YELLOW);
        off.setColor(0x60103070);
        off.setStroke(dp(1), 0x80d8ecff);
        focus.setColor(selected ? BLUE : 0x90205090);
        focus.setStroke(dp(3), Color.WHITE);
        for (GradientDrawable d : new GradientDrawable[] {on, off, focus}) d.setCornerRadius(dp(16));
        StateListDrawable s = new StateListDrawable();
        s.addState(new int[] {android.R.attr.state_focused}, focus);
        s.addState(new int[] {android.R.attr.state_pressed}, focus);
        s.addState(new int[] {}, selected ? on : off);
        t.setBackground(s);
        t.setTextColor(selected ? YELLOW : LIGHT);
    }

    private void choice(LinearLayout col, String name, String[] options, int selected, Pick pick) {
        label(col, name);
        android.widget.HorizontalScrollView scroll = new android.widget.HorizontalScrollView(mActivity);
        scroll.setHorizontalScrollBarEnabled(false);
        LinearLayout row = new LinearLayout(mActivity);
        row.setOrientation(LinearLayout.HORIZONTAL);
        TextView[] chips = new TextView[options.length];
        for (int i = 0; i < options.length; i++) {
            final int index = i;
            chips[i] = chip(options[i], i == selected);
            chips[i].setOnClickListener(v -> {
                for (int k = 0; k < chips.length; k++) styleChip(chips[k], k == index);
                pick.picked(index);
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.rightMargin = dp(6);
            row.addView(chips[i], lp);
            if (mFirst == null) mFirst = chips[i];
        }
        scroll.addView(row);
        col.addView(scroll);
    }

    private void action(LinearLayout col, String name, Runnable run) {
        TextView t = chip(name, false);
        t.setOnClickListener(v -> run.run());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(10);
        col.addView(t, lp);
    }
}
