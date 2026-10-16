// Handing control to the runtime.

#pragma once

#include "ultramodern/renderer_context.hpp"

struct SDL_Window;

namespace oot {
    namespace boot {

        // Builds the runtime Configuration and calls recomp::start. Does not return until the game
        // exits: the runtime owns the thread from here.
        void run(SDL_Window* window,
                 ultramodern::renderer::WindowHandle window_handle,
                 int argc,
                 char** argv);

        // The window's client size in pixels as the window system last reported it. Safe from
        // any thread: the values are kept by the thread that pumps the window's messages.
        void window_size(int& width, int& height);

    } // namespace boot
} // namespace oot
