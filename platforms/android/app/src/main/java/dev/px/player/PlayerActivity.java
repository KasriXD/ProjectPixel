package dev.px.player;

import org.libsdl.app.SDLActivity;

/**
 * px-player on Android. SDL is linked statically into libmain.so (the
 * player), so that is the only native library to load. The game comes from
 * assets/game.pxpkg.
 */
public class PlayerActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }
}
