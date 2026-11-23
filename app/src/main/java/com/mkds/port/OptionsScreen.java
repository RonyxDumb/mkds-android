package com.mkds.port;

import android.animation.ValueAnimator;
import android.app.Activity;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.content.res.ColorStateList;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.animation.DecelerateInterpolator;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.List;

/**
 * The app's options, over the paused game, laid out for a wide screen: a sidebar of sections on the left (Graphics,
 * Controls, Game, Mods), the chosen section's settings on the right, in rounded groups. The look is the game's (deep
 * blue with a checkered band, red and yellow accents, bold italic titles) drawn with care: an 8 dp grid, one corner
 * radius per size, segmented controls with a sliding thumb, hairline separators, and touch feedback on every row.
 * Touch, or use a gamepad: up/down move, left/right change a choice, A chooses, L/R change the section, B closes.
 * Everything is kept in the "mkds" preferences and read through the getters below.
 */
final class OptionsScreen {
    interface Host {
        void onOptionsChanged();
        void onOptionsClosed();
        void exportSave();
        void importSave();
        void restartGame();
        void addMod();
        void onModChosen();
        void editLayout();
        void shareLog();
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

    /** The on-screen controls stay with a gamepad connected. */
    static boolean touchAlways(android.content.Context c) { return prefs(c).getBoolean("touch_always", false); }

    /** In races, A is held unless B is: the right thumb is free for R (drift) and L. */
    static boolean autoAccelerate(android.content.Context c) { return prefs(c).getBoolean("auto_accel", false); }

    /** Drift button: R separate from A, larger, placed for the thumb (true), or R right above A (false). */
    static boolean bigDrift(android.content.Context c) { return prefs(c).getBoolean("big_drift", false); }

    /** The stick: a fixed base (true) or one that appears where the thumb lands (false). */
    static boolean fixedStick(android.content.Context c) { return prefs(c).getBoolean("stick_fixed", true); }

    static int buttonSizePercent(android.content.Context c) { return prefs(c).getInt("button_size", 100); }

    static int buttonOpacityPercent(android.content.Context c) { return prefs(c).getInt("button_opacity", 100); }

    static boolean haptics(android.content.Context c) { return prefs(c).getBoolean("haptics", true); }

    static boolean unlockAll(android.content.Context c) { return prefs(c).getBoolean("unlock_all", false); }

    // ---------------------------------------------------------------------------------------- look
    private static final int NAVY = 0xff0a1a44, BLUE = 0xff1747b8, BLUE_LIGHT = 0xff2f6fe8;
    private static final int RED = 0xffe60012, YELLOW = 0xffffd21f;
    private static final int CARD = 0xf7ffffff, INK = 0xff11151c, INK_2 = 0xff6b7280, HAIRLINE = 0x1f0a1a44;
    private static final int TRACK = 0xffe9edf5;
    private static final String[] SECTIONS = {"Graphics", "Controls", "Game", "Mods"};
    private static final String[] SECTION_ICONS = {"◐", "◎", "★", "✚"};

    private final Activity mActivity;
    private final Host mHost;
    private final FrameLayout mRoot;
    private final float mDp;
    private final LinearLayout mSidebar, mContent;
    private final ScrollView mScroll;
    private final TextView[] mTabs = new TextView[SECTIONS.length];
    private int mSection;

    OptionsScreen(Activity activity, Host host) {
        mActivity = activity;
        mHost = host;
        mDp = activity.getResources().getDisplayMetrics().density;
        mSection = prefs(activity).getInt("options_section", 0);
        if (mSection < 0 || mSection >= SECTIONS.length) mSection = 0;

        mRoot = new FrameLayout(activity);
        // Opaque: the game does not show through (a translucent view over the game's surface is drawn unevenly).
        mRoot.setBackgroundColor(NAVY);
        mRoot.setClickable(true); // nothing reaches the game under it
        mRoot.setTranslationZ(dp(64)); // over the on-screen controls
        View backdrop = new View(activity);
        backdrop.setBackground(new Backdrop());
        mRoot.addView(backdrop, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));

        LinearLayout row = new LinearLayout(activity);
        row.setOrientation(LinearLayout.HORIZONTAL);
        mRoot.addView(row, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));

        // Sidebar: the title, the sections, and Close at the bottom.
        mSidebar = new LinearLayout(activity);
        mSidebar.setOrientation(LinearLayout.VERTICAL);
        mSidebar.setPadding(dp(20), dp(16), dp(8), dp(12));
        TextView title = new TextView(activity);
        title.setText("Options");
        title.setTextColor(Color.WHITE);
        title.setTextSize(24);
        title.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD_ITALIC));
        title.setShadowLayer(dp(3), 0, dp(2), 0x80000000);
        mSidebar.addView(title);
        View stripe = new View(activity);
        GradientDrawable stripeBg = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                new int[] {RED, YELLOW});
        stripeBg.setCornerRadius(dp(2));
        stripe.setBackground(stripeBg);
        LinearLayout.LayoutParams stripeLp = new LinearLayout.LayoutParams(dp(48), dp(4));
        stripeLp.topMargin = dp(4);
        stripeLp.bottomMargin = dp(12);
        mSidebar.addView(stripe, stripeLp);
        for (int i = 0; i < SECTIONS.length; i++) {
            final int index = i;
            TextView tab = new TextView(activity);
            tab.setText(SECTION_ICONS[i] + "   " + SECTIONS[i]);
            tab.setTextSize(16);
            tab.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
            tab.setGravity(Gravity.CENTER_VERTICAL);
            tab.setPadding(dp(16), 0, dp(16), 0);
            tab.setFocusable(true);
            tab.setClickable(true);
            tab.setOnClickListener(v -> showSection(index));
            mTabs[i] = tab;
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(42));
            lp.bottomMargin = dp(4);
            mSidebar.addView(tab, lp);
        }
        View spring = new View(activity);
        mSidebar.addView(spring, new LinearLayout.LayoutParams(1, 0, 1));
        TextView close = pill("Close", false);
        close.setOnClickListener(v -> close());
        LinearLayout.LayoutParams closeLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(40));
        closeLp.topMargin = dp(8);
        mSidebar.addView(close, closeLp);
        // On short displays the sidebar scrolls rather than squeezing its rows.
        ScrollView sideScroll = new ScrollView(activity);
        sideScroll.setFillViewport(true);
        sideScroll.setVerticalScrollBarEnabled(false);
        sideScroll.addView(mSidebar, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        row.addView(sideScroll, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 0.28f));

        // Content.
        mScroll = new ScrollView(activity);
        mScroll.setVerticalScrollBarEnabled(false);
        mScroll.setClipToPadding(false);
        mScroll.setPadding(dp(12), dp(16), dp(24), dp(16));
        mContent = new LinearLayout(activity);
        mContent.setOrientation(LinearLayout.VERTICAL);
        mScroll.addView(mContent);
        row.addView(mScroll, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 0.72f));

        showSection(mSection);
    }

    View view() {
        return mRoot;
    }

    void close() {
        if (mRoot.getParent() instanceof ViewGroup) ((ViewGroup) mRoot.getParent()).removeView(mRoot);
        mHost.onOptionsClosed();
    }

    /** The mods changed (one added): the Mods section is shown again. */
    void refreshMods() {
        if (mSection == 3) showSection(3);
    }

    /** Keys while the screen is open: D-pad moves the focus, A chooses, L/R change the section, B / START close. */
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
            case KeyEvent.KEYCODE_BUTTON_L1:
            case KeyEvent.KEYCODE_BUTTON_R1:
                if (down) showSection((mSection + (code == KeyEvent.KEYCODE_BUTTON_R1 ? 1 : SECTIONS.length - 1))
                        % SECTIONS.length);
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

    // ---------------------------------------------------------------------------------------- sections
    private void showSection(int index) {
        mSection = index;
        prefs(mActivity).edit().putInt("options_section", index).apply();
        for (int i = 0; i < mTabs.length; i++) styleTab(mTabs[i], i == index);
        mContent.removeAllViews();
        switch (index) {
            case 0: buildGraphics(); break;
            case 1: buildControls(); break;
            case 2: buildGame(); break;
            default: buildMods(); break;
        }
        mScroll.scrollTo(0, 0);
        // The new section slides in a little (position only: no transparency over the game).
        mContent.setTranslationY(dp(10));
        mContent.animate().translationY(0).setDuration(180).setInterpolator(new DecelerateInterpolator()).start();
        mTabs[index].post(() -> mTabs[index].requestFocus());
    }

    private void buildGraphics() {
        LinearLayout g = group("Display");
        segmented(g, "Frame rate", new String[] {"60 fps", "Display max"}, highFrameRate(mActivity) ? 1 : 0,
                i -> put("high_fps", i == 1));
        int[] scales = {0, 1, 2, 3, 4};
        segmented(g, "3D resolution", new String[] {"Auto", "1×", "2×", "3×", "4×"},
                indexOf(scales, maxScale(mActivity)), i -> putInt("max_scale", scales[i]));
        footnote("Display max draws extra frames between the game's own on 90, 120 and 144 Hz displays. 60 fps and a "
                + "lower 3D resolution save battery.");
    }

    private void buildControls() {
        LinearLayout g = group("Touch");
        toggle(g, "Always show touch controls", "Also with a gamepad connected", touchAlways(mActivity),
                v -> put("touch_always", v));
        toggle(g, "Auto-accelerate", "A is held in races; B still brakes", autoAccelerate(mActivity),
                v -> put("auto_accel", v));
        segmented(g, "Stick", new String[] {"Fixed", "Floating"}, fixedStick(mActivity) ? 0 : 1,
                i -> put("stick_fixed", i == 0));
        segmented(g, "Drift button (R)", new String[] {"Above A", "Large, apart"}, bigDrift(mActivity) ? 1 : 0,
                i -> put("big_drift", i == 1));
        LinearLayout look = group("Buttons");
        action(look, "Edit layout", "Move and resize the on-screen buttons", mHost::editLayout);
        int[] sizes = {80, 100, 120, 140};
        segmented(look, "Size", new String[] {"S", "M", "L", "XL"}, indexOf(sizes, buttonSizePercent(mActivity)),
                i -> putInt("button_size", sizes[i]));
        int[] opacities = {40, 70, 100};
        segmented(look, "Opacity", new String[] {"Low", "Medium", "High"},
                indexOf(opacities, buttonOpacityPercent(mActivity)), i -> putInt("button_opacity", opacities[i]));
        toggle(look, "Vibration", null, haptics(mActivity), v -> put("haptics", v));
        footnote("With auto-accelerate, one thumb steers with the stick and the other drifts with R.");
    }

    private void buildGame() {
        LinearLayout g = group("Progress");
        toggle(g, "Unlock everything", "Gold trophies with three stars in every cup", unlockAll(mActivity), v -> {
            put("unlock_all", v);
            if (v) {
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
        LinearLayout s = group("Save file");
        action(s, "Export save file", "Keep a copy anywhere", mHost::exportSave);
        action(s, "Import save file", "From an export, a DS or an emulator", mHost::importSave);
        LinearLayout help = group("Help");
        action(help, "Share the log", "The game's messages, for reporting a problem", mHost::shareLog);
        footnote("Progress is saved every few seconds and when you leave the app. Uninstalling deletes it: export a "
                + "copy first.");
    }

    private void buildMods() {
        String active = Mods.active(mActivity);
        List<String> mods = Mods.list(mActivity);
        LinearLayout g = group("Active mod");
        radio(g, "None", "The original game", active.isEmpty(), () -> chooseMod(""));
        for (String name : mods) {
            radio(g, name, null, name.equals(active), () -> chooseMod(name), () -> confirmRemove(name));
        }
        LinearLayout add = group(null);
        action(add, "Add a mod…", "An .xdelta patch for Mario Kart DS (Europe), or a patched ROM", mHost::addMod);
        footnote("A mod's graphics, models, courses that take the place of the original ones, music and texts are "
                + "used. Courses, cups and menus a mod adds with its own code (CTGP-style) are not: the game's code "
                + "here is the port's. Each mod has its own save file. Touch and hold a mod to remove it.");
    }

    private void chooseMod(String name) {
        if (name.equals(Mods.active(mActivity))) return;
        Mods.setActive(mActivity, name);
        showSection(3);
        mHost.onModChosen();
    }

    private void confirmRemove(String name) {
        new android.app.AlertDialog.Builder(mActivity)
                .setTitle("Remove " + name + "?")
                .setMessage("Its save file is kept.")
                .setPositiveButton("Remove", (d, w) -> {
                    boolean wasActive = name.equals(Mods.active(mActivity));
                    Mods.remove(mActivity, name);
                    showSection(3);
                    if (wasActive) mHost.onModChosen();
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    // ---------------------------------------------------------------------------------------- building blocks
    private interface Pick {
        void picked(int index);
    }

    private interface Flip {
        void flipped(boolean on);
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

    private void styleTab(TextView tab, boolean selected) {
        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(dp(12));
        if (selected) {
            bg.setColors(new int[] {RED, 0xffff3b30});
            bg.setOrientation(GradientDrawable.Orientation.LEFT_RIGHT);
        } else {
            bg.setColor(0x00000000);
        }
        tab.setBackground(new RippleDrawable(ColorStateList.valueOf(0x33ffffff), bg, maskOf(dp(12))));
        tab.setTextColor(selected ? Color.WHITE : 0xccffffff);
        tab.setElevation(selected ? dp(4) : 0);
    }

    private TextView pill(String text, boolean strong) {
        TextView t = new TextView(mActivity);
        t.setText(text);
        t.setTextSize(15);
        t.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        t.setGravity(Gravity.CENTER);
        t.setTextColor(strong ? NAVY : Color.WHITE);
        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(dp(22));
        bg.setColor(strong ? YELLOW : 0x26ffffff);
        t.setBackground(new RippleDrawable(ColorStateList.valueOf(0x40ffffff), bg, maskOf(dp(22))));
        t.setFocusable(true);
        t.setClickable(true);
        return t;
    }

    private static Drawable maskOf(int radius) {
        GradientDrawable m = new GradientDrawable();
        m.setCornerRadius(radius);
        m.setColor(Color.WHITE);
        return m;
    }

    // A titled group: a white card with rounded corners holding rows separated by hairlines.
    private LinearLayout group(String heading) {
        if (heading != null) {
            TextView h = new TextView(mActivity);
            h.setText(heading.toUpperCase(java.util.Locale.ROOT));
            h.setTextSize(12);
            h.setLetterSpacing(0.08f);
            h.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
            h.setTextColor(0xb3ffffff);
            h.setPadding(dp(16), mContent.getChildCount() == 0 ? 0 : dp(20), 0, dp(8));
            mContent.addView(h);
        } else {
            View gap = new View(mActivity);
            mContent.addView(gap, new LinearLayout.LayoutParams(1, dp(16)));
        }
        LinearLayout card = new LinearLayout(mActivity);
        card.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(dp(16));
        bg.setColor(CARD);
        card.setBackground(bg);
        card.setElevation(dp(2));
        card.setClipToOutline(true);
        mContent.addView(card, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        return card;
    }

    private void footnote(String text) {
        TextView t = new TextView(mActivity);
        t.setText(text);
        t.setTextSize(12);
        t.setTextColor(0x99ffffff);
        t.setLineSpacing(dp(2), 1f);
        t.setPadding(dp(16), dp(8), dp(16), 0);
        mContent.addView(t);
    }

    private LinearLayout rowIn(LinearLayout card) {
        if (card.getChildCount() > 0) {
            View line = new View(mActivity);
            line.setBackgroundColor(HAIRLINE);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 1);
            lp.leftMargin = dp(16);
            card.addView(line, lp);
        }
        LinearLayout row = new LinearLayout(mActivity);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setMinimumHeight(dp(56));
        row.setPadding(dp(16), dp(8), dp(12), dp(8));
        card.addView(row, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        return row;
    }

    private LinearLayout labels(LinearLayout row, String title, String subtitle) {
        LinearLayout box = new LinearLayout(mActivity);
        box.setOrientation(LinearLayout.VERTICAL);
        TextView t = new TextView(mActivity);
        t.setText(title);
        t.setTextSize(16);
        t.setTextColor(INK);
        t.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        t.setSingleLine(true);
        t.setEllipsize(TextUtils.TruncateAt.END);
        box.addView(t);
        if (subtitle != null) {
            TextView s = new TextView(mActivity);
            s.setText(subtitle);
            s.setTextSize(13);
            s.setTextColor(INK_2);
            box.addView(s);
        }
        row.addView(box, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        return box;
    }

    private void segmented(LinearLayout card, String title, String[] options, int selected, Pick pick) {
        LinearLayout row = rowIn(card);
        labels(row, title, null);
        Segmented seg = new Segmented(options, selected, pick);
        row.addView(seg, new LinearLayout.LayoutParams(dp(Math.max(160, options.length * 64)), dp(36)));
    }

    private void toggle(LinearLayout card, String title, String subtitle, boolean on, Flip flip) {
        LinearLayout row = rowIn(card);
        labels(row, title, subtitle);
        Switch sw = new Switch(on);
        row.addView(sw, new LinearLayout.LayoutParams(dp(52), dp(32)));
        rowFeedback(row, () -> {
            sw.set(!sw.on);
            flip.flipped(sw.on);
        });
    }

    private void action(LinearLayout card, String title, String subtitle, Runnable run) {
        LinearLayout row = rowIn(card);
        labels(row, title, subtitle);
        TextView chevron = new TextView(mActivity);
        chevron.setText("›");
        chevron.setTextSize(24);
        chevron.setTextColor(0xffc0c6d0);
        row.addView(chevron);
        rowFeedback(row, run);
    }

    private void radio(LinearLayout card, String title, String subtitle, boolean checked, Runnable run) {
        radio(card, title, subtitle, checked, run, null);
    }

    private void radio(LinearLayout card, String title, String subtitle, boolean checked, Runnable run,
                       Runnable longPress) {
        LinearLayout row = rowIn(card);
        labels(row, title, subtitle);
        TextView mark = new TextView(mActivity);
        mark.setText(checked ? "✓" : "");
        mark.setTextSize(20);
        mark.setTypeface(Typeface.DEFAULT_BOLD);
        mark.setTextColor(RED);
        row.addView(mark);
        rowFeedback(row, run);
        if (longPress != null) {
            row.setOnLongClickListener(v -> {
                longPress.run();
                return true;
            });
        }
    }

    private void rowFeedback(View row, Runnable run) {
        Drawable ripple = new RippleDrawable(ColorStateList.valueOf(0x220a1a44), null, maskOf(0));
        row.setBackground(ripple);
        row.setFocusable(true);
        row.setClickable(true);
        row.setOnClickListener(v -> run.run());
        row.setOnFocusChangeListener((v, has) -> {
            if (has) v.setBackgroundColor(0x1a1747b8);
            else v.setBackground(ripple);
        });
    }

    // ---------------------------------------------------------------------------------------- controls
    /** iOS-style segmented control: a light track, a white thumb with a soft shadow that slides to the choice. */
    private final class Segmented extends View {
        private final String[] mOptions;
        private final Pick mPick;
        private int mSelected;
        private float mThumb; // animated position, in segments
        private final Paint mTrack = new Paint(Paint.ANTI_ALIAS_FLAG), mThumbPaint = new Paint(Paint.ANTI_ALIAS_FLAG),
                mText = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF mRect = new RectF();

        Segmented(String[] options, int selected, Pick pick) {
            super(mActivity);
            mOptions = options;
            mPick = pick;
            mSelected = Math.max(0, Math.min(selected, options.length - 1));
            mThumb = mSelected;
            mTrack.setColor(TRACK);
            mThumbPaint.setColor(Color.WHITE);
            mThumbPaint.setShadowLayer(dp(3), 0, dp(1), 0x33000000);
            setLayerType(LAYER_TYPE_SOFTWARE, null); // the shadow
            mText.setTextAlign(Paint.Align.CENTER);
            mText.setTextSize(dp(13));
            setFocusable(true);
            setClickable(true);
            setOnClickListener(v -> choose((mSelected + 1) % mOptions.length)); // A with a gamepad: next
            setOnKeyListener((v, code, e) -> {
                if (e.getAction() != KeyEvent.ACTION_DOWN) return false;
                if (code == KeyEvent.KEYCODE_DPAD_LEFT && mSelected > 0) {
                    choose(mSelected - 1);
                    return true;
                }
                if (code == KeyEvent.KEYCODE_DPAD_RIGHT && mSelected < mOptions.length - 1) {
                    choose(mSelected + 1);
                    return true;
                }
                return false;
            });
        }

        private void choose(int index) {
            if (index == mSelected) return;
            ValueAnimator a = ValueAnimator.ofFloat(mThumb, index);
            a.setDuration(200);
            a.setInterpolator(new DecelerateInterpolator(1.6f));
            a.addUpdateListener(an -> {
                mThumb = (float) an.getAnimatedValue();
                invalidate();
            });
            a.start();
            mSelected = index;
            mPick.picked(index);
            performHapticFeedback(android.view.HapticFeedbackConstants.CLOCK_TICK);
        }

        @Override
        public boolean onTouchEvent(MotionEvent e) {
            if (e.getAction() == MotionEvent.ACTION_UP) {
                int index = (int) (e.getX() / getWidth() * mOptions.length);
                choose(Math.max(0, Math.min(index, mOptions.length - 1)));
            }
            return true;
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight(), seg = w / mOptions.length, r = h / 2;
            mRect.set(0, 0, w, h);
            c.drawRoundRect(mRect, r, r, mTrack);
            float inset = dp(2.5f);
            mRect.set(mThumb * seg + inset, inset, (mThumb + 1) * seg - inset, h - inset);
            c.drawRoundRect(mRect, r - inset, r - inset, mThumbPaint);
            if (isFocused()) {
                Paint ring = new Paint(Paint.ANTI_ALIAS_FLAG);
                ring.setStyle(Paint.Style.STROKE);
                ring.setStrokeWidth(dp(2));
                ring.setColor(BLUE_LIGHT);
                mRect.set(1, 1, w - 1, h - 1);
                c.drawRoundRect(mRect, r, r, ring);
            }
            for (int i = 0; i < mOptions.length; i++) {
                boolean on = i == mSelected;
                mText.setColor(on ? INK : INK_2);
                mText.setTypeface(Typeface.create(Typeface.DEFAULT, on ? Typeface.BOLD : Typeface.NORMAL));
                float y = h / 2 - (mText.descent() + mText.ascent()) / 2;
                c.drawText(mOptions[i], (i + 0.5f) * seg, y, mText);
            }
        }
    }

    /** A switch: a pill track (grey off, the game's red on) with a white knob that slides. */
    private final class Switch extends View {
        boolean on;
        private float mPos;
        private final Paint mPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF mRect = new RectF();

        Switch(boolean on) {
            super(mActivity);
            this.on = on;
            mPos = on ? 1 : 0;
            setLayerType(LAYER_TYPE_SOFTWARE, null);
        }

        void set(boolean value) {
            on = value;
            ValueAnimator a = ValueAnimator.ofFloat(mPos, value ? 1 : 0);
            a.setDuration(180);
            a.setInterpolator(new DecelerateInterpolator());
            a.addUpdateListener(an -> {
                mPos = (float) an.getAnimatedValue();
                invalidate();
            });
            a.start();
            performHapticFeedback(android.view.HapticFeedbackConstants.CLOCK_TICK);
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight(), r = h / 2;
            int off = 0xffdde2ea;
            int red = (int) (Color.red(off) + (Color.red(RED) - Color.red(off)) * mPos);
            int green = (int) (Color.green(off) + (Color.green(RED) - Color.green(off)) * mPos);
            int blue = (int) (Color.blue(off) + (Color.blue(RED) - Color.blue(off)) * mPos);
            mPaint.setShadowLayer(0, 0, 0, 0);
            mPaint.setColor(Color.rgb(red, green, blue));
            mRect.set(0, 0, w, h);
            c.drawRoundRect(mRect, r, r, mPaint);
            float knob = r - dp(2.5f), cx = r + (w - 2 * r) * mPos;
            mPaint.setColor(Color.WHITE);
            mPaint.setShadowLayer(dp(2), 0, dp(1), 0x40000000);
            c.drawCircle(cx, r, knob, mPaint);
        }
    }

    /** The backdrop: the game's deep blue, a diagonal checkered band and a soft vignette. */
    private final class Backdrop extends Drawable {
        private final Paint mPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

        @Override
        public void draw(Canvas c) {
            float w = getBounds().width(), h = getBounds().height();
            mPaint.setShader(new LinearGradient(0, 0, w, h, new int[] {0xff0a1a44, 0xff123a9a, 0xff0a1a44}, null,
                    Shader.TileMode.CLAMP));
            c.drawRect(0, 0, w, h, mPaint);
            mPaint.setShader(null);
            // Checkered band across the lower right, faint.
            float s = dp(14);
            c.save();
            c.rotate(-18, w * 0.75f, h * 0.85f);
            mPaint.setColor(0x10ffffff);
            for (int row = 0; row < 3; row++) {
                for (float x = -w; x < w * 2; x += 2 * s) {
                    float ox = row % 2 == 0 ? 0 : s;
                    c.drawRect(x + ox, h * 0.85f + row * s, x + ox + s, h * 0.85f + (row + 1) * s, mPaint);
                }
            }
            c.restore();
            // A thin red and yellow stripe along the top.
            mPaint.setShader(new LinearGradient(0, 0, w, 0, new int[] {RED, YELLOW, RED}, null,
                    Shader.TileMode.CLAMP));
            c.drawRect(0, 0, w, dp(3), mPaint);
            mPaint.setShader(null);
        }

        @Override
        public void setAlpha(int alpha) {}

        @Override
        public void setColorFilter(android.graphics.ColorFilter cf) {}

        @Override
        public int getOpacity() {
            return android.graphics.PixelFormat.TRANSLUCENT;
        }
    }
}
