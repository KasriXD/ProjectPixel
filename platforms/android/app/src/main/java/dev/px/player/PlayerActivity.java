package dev.px.player;

import org.libsdl.app.SDLActivity;

import java.util.ArrayList;
import java.util.List;

/**
 * px-player on Android. SDL is linked statically into libmain.so (the
 * player), so that is the only native library to load. The app opens the
 * launcher; the game in assets/game.pxpkg plays from its "On this device"
 * list.
 */
public class PlayerActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    /**
     * Test runs pass a few player options as an extra:
     * {@code adb shell am start -n dev.px.player/.PlayerActivity --es args "--open local:0"}.
     * Other apps can start this activity too, so only options that read or
     * write nothing outside the app are taken.
     */
    @Override
    protected String[] getArguments() {
        String extra = getIntent() != null ? getIntent().getStringExtra("args") : null;
        if (extra == null || extra.trim().isEmpty()) {
            return new String[0];
        }
        String[] words = extra.trim().split("\\s+");
        List<String> args = new ArrayList<>();
        for (int i = 0; i < words.length; ++i) {
            String w = words[i];
            if (w.equals("--verbose") || w.equals("--touch") || w.equals("--launcher")) {
                args.add(w);
            } else if ((w.equals("--open") || w.equals("--frames")) && i + 1 < words.length
                       && words[i + 1].matches("[A-Za-z0-9:_]{1,40}")) {
                args.add(w);
                args.add(words[++i]);
            }
        }
        return args.toArray(new String[0]);
    }
}
