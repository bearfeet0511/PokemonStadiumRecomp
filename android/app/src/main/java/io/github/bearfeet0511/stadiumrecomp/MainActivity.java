package io.github.bearfeet0511.stadiumrecomp;

import org.libsdl.app.SDLActivity;

/**
 * Hosts the recompiled game. SDLActivity loads these libraries, starts its own
 * thread, and calls SDL_main inside the last one.
 */
public class MainActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "PokemonStadiumRecomp" };
    }
}
