package com.mkds.port;

import android.content.Context;
import android.content.Intent;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.drawable.Icon;

import java.io.File;
import java.io.RandomAccessFile;

/**
 * The game's own icon (the 32x32 picture the DS shows in its menu, from the banner of the user's ROM) as a home screen
 * shortcut. The app cannot ship it (no game content in the APK), so it is read from the ROM on the phone.
 */
final class GameIcon {
    private GameIcon() {}

    /** The banner icon, 32x32 ARGB, or null. */
    static Bitmap read(File rom) {
        try (RandomAccessFile r = new RandomAccessFile(rom, "r")) {
            r.seek(0x68);
            long banner = Integer.toUnsignedLong(Integer.reverseBytes(r.readInt()));
            if (banner == 0 || banner + 0x240 > r.length()) return null;
            byte[] pixels = new byte[0x200], pal = new byte[0x20];
            r.seek(banner + 0x20);
            r.readFully(pixels);
            r.readFully(pal);
            int[] colors = new int[16];
            for (int i = 1; i < 16; i++) { // colour 0 is transparent
                int c = (pal[i * 2] & 0xff) | (pal[i * 2 + 1] & 0xff) << 8;
                int red = (c & 31) * 255 / 31, green = (c >> 5 & 31) * 255 / 31, blue = (c >> 10 & 31) * 255 / 31;
                colors[i] = 0xff000000 | red << 16 | green << 8 | blue;
            }
            int[] argb = new int[32 * 32];
            for (int tile = 0; tile < 16; tile++) { // 4x4 tiles of 8x8, 4 bits per pixel
                for (int i = 0; i < 64; i++) {
                    int b = pixels[tile * 32 + i / 2] & 0xff, index = (i & 1) != 0 ? b >> 4 : b & 15;
                    int x = (tile % 4) * 8 + i % 8, y = (tile / 4) * 8 + i / 8;
                    argb[y * 32 + x] = colors[index];
                }
            }
            return Bitmap.createBitmap(argb, 32, 32, Bitmap.Config.ARGB_8888);
        } catch (Exception e) {
            return null;
        }
    }

    /** Asks the launcher to pin a shortcut with the game's icon. False when the launcher cannot or the ROM has none. */
    static boolean pinShortcut(Context context, File rom) {
        Bitmap icon = read(rom);
        ShortcutManager sm = context.getSystemService(ShortcutManager.class);
        if (icon == null || sm == null || !sm.isRequestPinShortcutSupported()) return false;
        // Adaptive bitmap: 108 units with the picture in the middle 72 (the launcher's mask may cut the rest), on the
        // icon's own background colour (its top left pixel, white when transparent). Pixels stay sharp.
        int size = 432, inner = 288;
        Bitmap out = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(out);
        int corner = icon.getPixel(0, 0);
        c.drawColor((corner >>> 24) == 0 ? 0xffffffff : corner);
        Paint p = new Paint();
        p.setFilterBitmap(false);
        int o = (size - inner) / 2;
        c.drawBitmap(icon, null, new Rect(o, o, o + inner, o + inner), p);
        Intent launch = new Intent(Intent.ACTION_MAIN).setClass(context, MainActivity.class)
                .addCategory(Intent.CATEGORY_LAUNCHER);
        ShortcutInfo info = new ShortcutInfo.Builder(context, "game-icon")
                .setShortLabel("Mario Kart DS")
                .setIcon(Icon.createWithAdaptiveBitmap(out))
                .setIntent(launch)
                .build();
        return sm.requestPinShortcut(info, null);
    }
}
