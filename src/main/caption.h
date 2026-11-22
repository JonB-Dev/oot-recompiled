// THE WINDOW'S CAPTION BUTTONS, AS WINDOWS SEES THEM (the user, 2026-11-20: "replace the custom
// minimize, maximize, and close window buttons in both in-game and launcher with modfified, branded
// windows defaults ... but also allow proper win snapping", "similar to exactly how website lab or
// developer studio does").
//
// The program draws its own bar and its own three buttons, as Electron's title bar overlay does for
// those apps. What kept them from acting like Windows' own was what Windows could not see: SDL gives
// a borderless window WS_POPUP and WS_MINIMIZEBOX only, so there was no edge snapping, no Win and
// arrow keys and no snap layouts, and SDL's hit test can say "drag" or "resize" but never "this is
// the maximize button".
//
// This takes the window's messages ahead of SDL. The window gets the standard frame styles with no
// frame drawn (WM_NCCALCSIZE; a maximized window is kept inside the screen), and the pointer over a
// drawn button is that button to Windows (HTMINBUTTON, HTMAXBUTTON, HTCLOSE), so hovering maximize
// shows Windows 11's snap layouts. A press and release on a button runs its action; the interface
// draws the hover and press Windows reports. Full screen is left as the renderer makes it.

#pragma once

struct SDL_Window;

namespace oot::caption {

    enum class Button { None, Minimize, Maximize, Close };

    struct Rect {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
    };

    // What a released button does. The program passes its window requests (launch.h); the self
    // check passes counters.
    struct Actions {
        void (*minimize)() = nullptr;
        void (*maximize)() = nullptr;
        void (*close)() = nullptr;
    };

    // Takes the window's messages and gives it the frame styles. Once, after the window exists.
    void attach(SDL_Window* window, const Actions& actions);

    // The frame styles again, after SDL sets its own (SDL_SetWindowBordered). Left alone while the
    // window covers its display, which is the renderer's full screen.
    void restyle();

    // Where the interface drew the three buttons this frame, in window pixels: minimize, maximize,
    // close. An empty rect is a button not on screen.
    void set_buttons(const Rect& minimize, const Rect& maximize, const Rect& close);

    // The button under the pointer and the one held down, for the interface to draw.
    Button hovered();
    Button pressed();

    // Whether Windows holds the window maximized, for the maximize button's glyph.
    bool maximized();

    // Always on top (the Always on top row): the window in Windows' topmost band or out of it,
    // never activated to get there. The thread that made the window.
    void set_topmost(bool on);

    // The button at a point of the window, given the three rects in that order.
    Button button_at(int x, int y, const Rect (&rects)[3]);

    // --caption-selftest: the button lookup, then a hidden window, never shown, taken through the
    // styles, the hit test, the frame size and a press on each button. 0 on success; the first
    // failure is printed otherwise.
    int selftest();

} // namespace oot::caption
