package com.mkds.port;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.view.KeyEvent;

/**
 * The gamepad's buttons chosen by the player (Options > Controls > Map gamepad buttons, issue #14): each DS button
 * can take a key of the gamepad, kept in the preferences as "pad_<button>" = key code. A key chosen for a DS button
 * does that button only; the other keys keep their usual meaning (MainActivity.defaultButtonFor).
 */
final class GamepadMapping {
    private static final String[] NAMES = {"A", "B", "X", "Y", "L", "R", "START", "SELECT", "UP", "DOWN", "LEFT",
            "RIGHT"};
    private static final int[] BITS = {Native.A, Native.B, Native.X, Native.Y, Native.L, Native.R, Native.START,
            Native.SELECT, Native.UP, Native.DOWN, Native.LEFT, Native.RIGHT};

    private static int[] sKeys; // per DS button, its chosen key (0: the usual one)

    private static int[] keys(android.content.Context c) {
        if (sKeys == null) {
            SharedPreferences p = OptionsScreen.prefs(c);
            int[] k = new int[NAMES.length];
            for (int i = 0; i < NAMES.length; i++) k[i] = p.getInt("pad_" + NAMES[i], 0);
            sKeys = k;
        }
        return sKeys;
    }

    /** The DS buttons a key was chosen for, or -1 when the key has no chosen meaning. */
    static int buttonFor(android.content.Context c, int keyCode) {
        int[] k = keys(c);
        int bits = 0;
        boolean found = false;
        for (int i = 0; i < k.length; i++) {
            if (k[i] == keyCode) {
                bits |= BITS[i];
                found = true;
            }
        }
        return found ? bits : -1;
    }

    private static String keyName(int keyCode) {
        String n = KeyEvent.keyCodeToString(keyCode);
        return n.startsWith("KEYCODE_") ? n.substring(8).replace("BUTTON_", "").replace('_', ' ') : n;
    }

    /** The list of DS buttons with their keys: touching one asks for the gamepad key. */
    static void showEditor(Activity a) {
        int[] k = keys(a);
        String[] items = new String[NAMES.length];
        for (int i = 0; i < NAMES.length; i++) {
            items[i] = NAMES[i] + "  —  " + (k[i] != 0 ? keyName(k[i]) : "usual");
        }
        new AlertDialog.Builder(a)
                .setTitle("Gamepad buttons")
                .setItems(items, (d, which) -> askKey(a, which))
                .setNeutralButton("Reset", (d, w) -> {
                    SharedPreferences.Editor e = OptionsScreen.prefs(a).edit();
                    for (String n : NAMES) e.remove("pad_" + n);
                    e.apply();
                    sKeys = null;
                })
                .setPositiveButton("Done", null)
                .show();
    }

    private static void askKey(Activity a, int index) {
        AlertDialog dialog = new AlertDialog.Builder(a)
                .setTitle("DS button " + NAMES[index])
                .setMessage("Press the gamepad button for " + NAMES[index] + ".")
                .setNegativeButton("Cancel", (d, w) -> showEditor(a))
                .create();
        final int[] downKey = {0}; // the key pressed in this dialog (not the end of the press that opened it)
        dialog.setOnKeyListener((d, keyCode, e) -> {
            if (keyCode == KeyEvent.KEYCODE_BACK) return false;
            if (e.getAction() == KeyEvent.ACTION_DOWN) {
                if (e.getRepeatCount() == 0) downKey[0] = keyCode;
                return true;
            }
            if (e.getAction() != KeyEvent.ACTION_UP || downKey[0] != keyCode) return true;
            int[] k = keys(a);
            SharedPreferences.Editor ed = OptionsScreen.prefs(a).edit();
            for (int i = 0; i < k.length; i++) {
                if (i != index && k[i] == keyCode) { // a key does one DS button
                    k[i] = 0;
                    ed.remove("pad_" + NAMES[i]);
                }
            }
            k[index] = keyCode;
            ed.putInt("pad_" + NAMES[index], keyCode).apply();
            d.dismiss();
            showEditor(a);
            return true;
        });
        dialog.show();
    }

    private GamepadMapping() {}
}
