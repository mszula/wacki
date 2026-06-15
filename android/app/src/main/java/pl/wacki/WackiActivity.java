/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * WackiActivity — the game activity. A thin SDLActivity subclass: SDL's Java
 * glue (org.libsdl.app, compiled from the SDL submodule) does all the heavy
 * lifting — it loads the native libraries, creates the GLES surface, pumps
 * input/lifecycle events into SDL, and calls SDL_main (our main.c, renamed by
 * SDL_main.h) on a dedicated thread.
 *
 * The data archives are already in place by the time this activity starts:
 * SetupActivity imports them before launching us.
 */
package pl.wacki;

import org.libsdl.app.SDLActivity;

public class WackiActivity extends SDLActivity {

    /** Native libraries to load, in order: libSDL2.so then our libmain.so. */
    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL2",
            "main",
        };
    }
}
