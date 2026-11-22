package com.apkfast.sdl2hello;

import org.libsdl.app.SDLActivity;

/// SDL2 está linkada estaticamente em libmain.so.
public class MainActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }
}
