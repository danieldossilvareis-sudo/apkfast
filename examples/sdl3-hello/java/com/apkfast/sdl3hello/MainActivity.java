package com.apkfast.sdl3hello;

import org.libsdl.app.SDLActivity;

/// A SDL3 está linkada estaticamente dentro de libmain.so,
/// então não há libSDL3.so separada para carregar.
public class MainActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }
}
