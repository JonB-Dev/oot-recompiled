#include "main/caption.h"

#include "ui_tokens.h"

#include <SDL.h>
#include <SDL_syswm.h>

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <iterator>
#include <mutex>

namespace oot::caption {

    namespace {

        HWND g_hwnd = nullptr;
        WNDPROC g_sdl_proc = nullptr;
        Actions g_actions;
        std::mutex g_mutex;
        Rect g_rects[3];
        std::atomic<Button> g_hovered{ Button::None };
        std::atomic<Button> g_pressed{ Button::None };
        bool g_tracking = false;

        int hit_code(Button button) {
            switch (button) {
                case Button::Minimize: return HTMINBUTTON;
                case Button::Maximize: return HTMAXBUTTON;
                case Button::Close:    return HTCLOSE;
                default:               return HTNOWHERE;
            }
        }

        Button from_hit(WPARAM hit) {
            switch (hit) {
                case HTMINBUTTON: return Button::Minimize;
                case HTMAXBUTTON: return Button::Maximize;
                case HTCLOSE:     return Button::Close;
                default:          return Button::None;
            }
        }

        bool is_resize(LRESULT hit) {
            return hit >= HTLEFT && hit <= HTBOTTOMRIGHT;
        }

        // Framed is windowed: the renderer's full screen takes the frame styles away (WS_POPUP).
        bool framed(HWND hwnd) {
            return (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME) != 0;
        }

        void run(Button button) {
            void (*action)() = button == Button::Minimize ? g_actions.minimize
                             : button == Button::Maximize ? g_actions.maximize
                             : button == Button::Close    ? g_actions.close
                                                          : nullptr;
            if (action != nullptr) {
                action();
            }
        }

        LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
            switch (message) {
                case WM_NCCALCSIZE:
                    // No frame of the system's: the client is the whole window. A maximized window
                    // with a sizing frame is that frame's width larger than the screen on every side,
                    // so its client is brought back inside.
                    if (wparam == TRUE && framed(hwnd)) {
                        if (IsZoomed(hwnd)) {
                            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
                            const UINT dpi = GetDpiForWindow(hwnd);
                            const int pad = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                            const int x = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + pad;
                            const int y = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + pad;
                            params->rgrc[0].left += x;
                            params->rgrc[0].right -= x;
                            params->rgrc[0].top += y;
                            params->rgrc[0].bottom -= y;
                        }
                        return 0;
                    }
                    break;

                case WM_NCHITTEST: {
                    // SDL's own answer (its hit test: drag and resize) first; a resizing edge keeps
                    // precedence at the window's rim, as on Windows' own buttons.
                    const LRESULT sdl = CallWindowProcW(g_sdl_proc, hwnd, message, wparam, lparam);
                    if (is_resize(sdl) && !IsZoomed(hwnd)) {
                        return sdl;
                    }
                    POINT point{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
                    ScreenToClient(hwnd, &point);
                    Rect rects[3];
                    {
                        std::lock_guard<std::mutex> lock(g_mutex);
                        std::copy(std::begin(g_rects), std::end(g_rects), std::begin(rects));
                    }
                    const Button button = button_at(point.x, point.y, rects);
                    return button != Button::None ? hit_code(button) : sdl;
                }

                case WM_NCMOUSEMOVE: {
                    const Button button = from_hit(wparam);
                    g_hovered.store(button);
                    if (button != Button::None && !g_tracking) {
                        TRACKMOUSEEVENT track{ sizeof(TRACKMOUSEEVENT), TME_LEAVE | TME_NONCLIENT, hwnd, 0 };
                        g_tracking = TrackMouseEvent(&track) != FALSE;
                    }
                    break;
                }

                case WM_NCMOUSELEAVE:
                    g_tracking = false;
                    g_hovered.store(Button::None);
                    g_pressed.store(Button::None);
                    break;

                case WM_MOUSEMOVE:
                    g_hovered.store(Button::None);
                    break;

                // Windows' own handling of a press on a caption button draws the old style button
                // and runs its own loop, so the press, the double click and the release on one of
                // ours stop here; a release runs the button only over the button it was pressed on.
                case WM_NCLBUTTONDOWN:
                case WM_NCLBUTTONDBLCLK:
                    if (const Button button = from_hit(wparam); button != Button::None) {
                        g_pressed.store(button);
                        return 0;
                    }
                    break;

                case WM_NCLBUTTONUP:
                    if (const Button button = from_hit(wparam); button != Button::None) {
                        const bool same = g_pressed.exchange(Button::None) == button;
                        if (same) {
                            run(button);
                        }
                        return 0;
                    }
                    g_pressed.store(Button::None);
                    break;

                case WM_NCRBUTTONDOWN:
                case WM_NCRBUTTONUP:
                    if (from_hit(wparam) != Button::None) {
                        return 0;
                    }
                    break;

                default:
                    break;
            }
            return CallWindowProcW(g_sdl_proc, hwnd, message, wparam, lparam);
        }

        // The window's own edge in the bar's hairline color (Windows 11), as Developer Studio sets
        // its accent color: with the system's accent on window borders, Windows drew that instead.
        void color_edge(HWND hwnd) {
            const uint32_t rgba = oot::ui::tokens::color_line;
            const COLORREF color = RGB((rgba >> 24) & 0xFF, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF);
            DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &color, sizeof(color));
        }

        bool covers_display(HWND hwnd) {
            RECT window{};
            GetWindowRect(hwnd, &window);
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
                return false;
            }
            const RECT& screen = monitor.rcMonitor;
            return !IsZoomed(hwnd) && window.left <= screen.left && window.top <= screen.top &&
                   window.right >= screen.right && window.bottom >= screen.bottom;
        }

        HWND handle_of(SDL_Window* window) {
            SDL_SysWMinfo info{};
            SDL_VERSION(&info.version);
            return (window != nullptr && SDL_GetWindowWMInfo(window, &info)) ? info.info.win.window : nullptr;
        }

    } // namespace

    Button button_at(int x, int y, const Rect (&rects)[3]) {
        static constexpr Button ORDER[3] = { Button::Minimize, Button::Maximize, Button::Close };
        for (int i = 0; i < 3; ++i) {
            const Rect& r = rects[i];
            if (r.w > 0 && r.h > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
                return ORDER[i];
            }
        }
        return Button::None;
    }

    void attach(SDL_Window* window, const Actions& actions) {
        HWND hwnd = handle_of(window);
        if (hwnd == nullptr || g_hwnd != nullptr) {
            return;
        }
        g_hwnd = hwnd;
        g_actions = actions;
        g_sdl_proc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(window_proc)));
        color_edge(hwnd);
        restyle();
    }

    void restyle() {
        if (g_hwnd == nullptr || covers_display(g_hwnd)) {
            return;
        }
        const LONG_PTR style = GetWindowLongPtrW(g_hwnd, GWL_STYLE);
        const LONG_PTR want = (style | WS_OVERLAPPEDWINDOW) & ~WS_POPUP;
        if (want != style) {
            SetWindowLongPtrW(g_hwnd, GWL_STYLE, want);
            SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                         SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
    }

    void set_buttons(const Rect& minimize, const Rect& maximize, const Rect& close) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_rects[0] = minimize;
        g_rects[1] = maximize;
        g_rects[2] = close;
    }

    Button hovered() {
        return g_hovered.load();
    }

    Button pressed() {
        return g_pressed.load();
    }

    bool maximized() {
        return g_hwnd != nullptr && IsZoomed(g_hwnd);
    }

    void set_topmost(bool on) {
        if (g_hwnd == nullptr) {
            return;
        }
        SetWindowPos(g_hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    int selftest() {
        auto fail = [](const char* what) {
            std::fprintf(stderr, "CAPTION_SELFTEST_FAILED: %s\n", what);
            return 1;
        };

        // The lookup: each rect is its button, an empty rect is none, and outside is none.
        {
            const Rect rects[3] = { { 100, 0, 46, 30 }, { 146, 0, 46, 30 }, { 192, 0, 46, 30 } };
            if (button_at(100, 0, rects) != Button::Minimize) return fail("the minimize rect's corner is not minimize");
            if (button_at(170, 29, rects) != Button::Maximize) return fail("inside the maximize rect is not maximize");
            if (button_at(237, 15, rects) != Button::Close) return fail("inside the close rect is not close");
            if (button_at(238, 15, rects) != Button::None) return fail("past the close rect is a button");
            if (button_at(150, 30, rects) != Button::None) return fail("below the bar is a button");
            const Rect hidden[3] = {};
            if (button_at(0, 0, hidden) != Button::None) return fail("an empty rect is a button");
        }

        // The window: hidden, never shown, so nothing comes to the front.
        static int minimized = 0;
        static int maximized_count = 0;
        static int closed = 0;
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            return fail("SDL's video would not start");
        }
        SDL_Window* window = SDL_CreateWindow("caption selftest", 100, 100, 800, 600,
                                              SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS);
        if (window == nullptr) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return fail("no window");
        }
        attach(window, Actions{ [] { ++minimized; }, [] { ++maximized_count; }, [] { ++closed; } });
        int result = 0;
        const auto check = [&](bool ok, const char* what) {
            if (result == 0 && !ok) {
                result = fail(what);
            }
        };

        const LONG_PTR style = GetWindowLongPtrW(g_hwnd, GWL_STYLE);
        check((style & WS_THICKFRAME) != 0, "no sizing frame, so no edge snapping");
        check((style & WS_MAXIMIZEBOX) != 0, "no maximize box, so no snap layouts or Win and Up");
        check((style & WS_SYSMENU) != 0, "no system menu");
        check((style & WS_POPUP) == 0, "still a popup");

        set_buttons({ 662, 0, 46, 30 }, { 708, 0, 46, 30 }, { 754, 0, 46, 30 });
        const auto hit = [&](int x, int y) {
            POINT point{ x, y };
            ClientToScreen(g_hwnd, &point);
            return SendMessageW(g_hwnd, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
        };
        check(hit(730, 15) == HTMAXBUTTON, "the maximize button is not HTMAXBUTTON to Windows");
        check(hit(680, 15) == HTMINBUTTON, "the minimize button is not HTMINBUTTON to Windows");
        check(hit(790, 20) == HTCLOSE, "the close button is not HTCLOSE to Windows");
        check(hit(400, 300) == HTCLIENT, "the middle of the window is not the client");

        RECT window_rect{};
        GetWindowRect(g_hwnd, &window_rect);
        NCCALCSIZE_PARAMS params{};
        params.rgrc[0] = window_rect;
        check(SendMessageW(g_hwnd, WM_NCCALCSIZE, TRUE, reinterpret_cast<LPARAM>(&params)) == 0, "the frame size was not answered");
        check(EqualRect(&params.rgrc[0], &window_rect) != FALSE, "a frame of the system's took part of the window");

        SendMessageW(g_hwnd, WM_NCMOUSEMOVE, HTMAXBUTTON, 0);
        check(hovered() == Button::Maximize, "the pointer on maximize is not reported");
        SendMessageW(g_hwnd, WM_NCLBUTTONDOWN, HTMINBUTTON, 0);
        check(pressed() == Button::Minimize, "the press on minimize is not reported");
        SendMessageW(g_hwnd, WM_NCLBUTTONUP, HTMINBUTTON, 0);
        check(minimized == 1, "a press and release on minimize did not minimize");
        SendMessageW(g_hwnd, WM_NCLBUTTONDOWN, HTMINBUTTON, 0);
        SendMessageW(g_hwnd, WM_NCLBUTTONUP, HTCLOSE, 0);
        check(minimized == 1 && closed == 0, "a release on another button ran a button");
        SendMessageW(g_hwnd, WM_NCLBUTTONDOWN, HTMAXBUTTON, 0);
        SendMessageW(g_hwnd, WM_NCLBUTTONUP, HTMAXBUTTON, 0);
        SendMessageW(g_hwnd, WM_NCLBUTTONDOWN, HTCLOSE, 0);
        SendMessageW(g_hwnd, WM_NCLBUTTONUP, HTCLOSE, 0);
        check(maximized_count == 1 && closed == 1, "maximize or close did not run");
        SendMessageW(g_hwnd, WM_NCMOUSELEAVE, 0, 0);
        check(hovered() == Button::None, "the pointer leaving left a button lit");

        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_sdl_proc));
        g_hwnd = nullptr;
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return result;
    }

} // namespace oot::caption
