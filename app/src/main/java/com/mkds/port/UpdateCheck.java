package com.mkds.port;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;

import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * Looks for a newer version of the app among the GitHub releases of the project, once per launch, in the background.
 * The answer goes to whoever asked for it with whenAvailable (the language screen shows a button); without a network
 * nothing happens.
 */
final class UpdateCheck {
    private static final String LATEST = "https://api.github.com/repos/RonyxDumb/mkds-android/releases/latest";

    interface Listener {
        void onUpdate(String version, String url);
    }

    private static boolean sStarted, sDone;
    private static String sVersion, sUrl; // a newer release, or null
    private static Listener sListener;
    private static final Handler sMain = new Handler(Looper.getMainLooper());

    static void start(Activity activity) {
        if (sStarted) return;
        sStarted = true;
        final String current = currentVersion(activity);
        Thread t = new Thread(() -> {
            String tag = null, url = null;
            try {
                HttpURLConnection c = (HttpURLConnection) new URL(LATEST).openConnection();
                c.setConnectTimeout(5000);
                c.setReadTimeout(5000);
                c.setRequestProperty("Accept", "application/vnd.github+json");
                c.setRequestProperty("User-Agent", "mkds-android");
                if (c.getResponseCode() == 200) {
                    String json;
                    try (InputStream in = c.getInputStream()) {
                        json = new String(readAll(in), "UTF-8");
                    }
                    tag = jsonString(json, "tag_name");
                    url = jsonString(json, "html_url");
                }
                c.disconnect();
            } catch (Exception e) {
                // no network: no answer
            }
            final String found = tag != null && newer(tag, current) ? tag : null, page = url;
            sMain.post(() -> {
                sDone = true;
                sVersion = found;
                sUrl = page;
                if (found != null && sListener != null) sListener.onUpdate(sVersion, sUrl);
            });
        }, "update-check");
        t.setDaemon(true);
        t.start();
    }

    /** Calls listener (on the main thread) when a newer version is known, now or once the check is done. */
    static void whenAvailable(Activity activity, Listener listener) {
        sListener = listener;
        if (sDone && sVersion != null) listener.onUpdate(sVersion, sUrl);
    }

    static void open(Activity activity, String url) {
        try {
            activity.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (Exception e) {
            // no browser
        }
    }

    private static String currentVersion(Activity activity) {
        try {
            return activity.getPackageManager().getPackageInfo(activity.getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "0";
        }
    }

    // "v0.24" against "0.23": numbers compared part by part.
    static boolean newer(String tag, String current) {
        int[] a = parts(tag), b = parts(current);
        for (int i = 0; i < Math.max(a.length, b.length); i++) {
            int x = i < a.length ? a[i] : 0, y = i < b.length ? b[i] : 0;
            if (x != y) return x > y;
        }
        return false;
    }

    private static int[] parts(String v) {
        String[] p = v.replaceAll("[^0-9.]", "").split("\\.");
        int[] out = new int[p.length];
        for (int i = 0; i < p.length; i++) {
            try {
                out[i] = Integer.parseInt(p[i]);
            } catch (NumberFormatException e) {
                out[i] = 0;
            }
        }
        return out;
    }

    private static byte[] readAll(InputStream in) throws java.io.IOException {
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        return out.toByteArray();
    }

    // The first string value of key in a JSON text (enough for the fields used here).
    private static String jsonString(String json, String key) {
        int k = json.indexOf("\"" + key + "\"");
        if (k < 0) return null;
        int colon = json.indexOf(':', k);
        int q1 = json.indexOf('"', colon + 1);
        int q2 = json.indexOf('"', q1 + 1);
        if (colon < 0 || q1 < 0 || q2 < 0) return null;
        return json.substring(q1 + 1, q2);
    }
}
