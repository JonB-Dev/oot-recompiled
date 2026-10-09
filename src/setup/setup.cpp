// The setup file: the ONE file a person downloads.
//
// It carries the whole program inside itself as a zip resource (the executable, SDL2.dll,
// dxcompiler.dll, dxil.dll, the Visual C++ runtime files and assets\), shows a window in the program's own look that says where
// the files will go and lets the person change it, choose a Start menu entry and a desktop shortcut,
// watches the files land, and says what happened and where. Running it again repairs or updates an
// existing install, so it is also how an update lands.
//
// WHY THIS IS A SEPARATE EXECUTABLE, because the obvious design is for the game to unpack its own
// files and it cannot work. Windows resolves an executable's import table BEFORE main() runs, so a
// game binary with no SDL2.dll beside it fails at load time and never reaches the code that would
// have written one. Delay loading the imports gets past that and then dies anyway, because a
// static initializer reaches SDL before the first line of main: the process ends with
// ERROR_MOD_NOT_FOUND (0xC06D007E) having printed nothing at all, which is a genuinely horrible
// thing to debug. This stub imports nothing but the operating system, so none of that can happen.
//
// THE WINDOW IS DRAWN BY HAND, in the tokens of assets/ui/tokens.rcss (the "Instrument Case"
// decision: the walnut case lit from the top left, brass for the one thing to do, hardware doing
// the dividing), because this program has no RmlUi and must not: the interface library lives in
// the payload it is about to unpack. The colors and the metrics below are that file's, copied
// once and named the same, so a change there is a change here. It used to install in silence and
// start the game, and "it doesn't even feel like it installed" (the user, 2026-09-24).
//
// It requires no administrator rights and writes nothing outside the folder the person chose,
// which defaults to their own application data.

#include "main/places.h"
#include "main/install.h"

#include "build_info.h"

#include <miniz.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <windowsx.h>
#include <shobjidl.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

    // Matched by src/setup/setup.rc.in.
    constexpr int PAYLOAD_RESOURCE = 100;
    constexpr int ICON_RESOURCE = 1;

    constexpr const wchar_t* GAME_EXE = L"OoTRecompiled.exe";
    constexpr const wchar_t* WINDOW_CLASS = L"OoTRecompiledSetup";

    // The tokens (assets/ui/tokens.rcss), as COLORREF.
    constexpr COLORREF CASE_TOP = RGB(58, 44, 32);
    constexpr COLORREF CASE_BOTTOM = RGB(36, 26, 19);
    constexpr COLORREF TRACK = RGB(0x1c, 0x15, 0x0f);
    constexpr COLORREF TEXT = RGB(0xf2, 0xe7, 0xd5);
    constexpr COLORREF TEXT_DIM = RGB(0xb3, 0x9a, 0x78);
    constexpr COLORREF ON_ACCENT = RGB(0x24, 0x1a, 0x13);
    constexpr COLORREF ACCENT = RGB(0xcf, 0xa3, 0x3c);
    constexpr COLORREF BRASS_LIGHT = RGB(0xe8, 0xcd, 0x7e);
    constexpr COLORREF LINE = RGB(0x5d, 0x47, 0x2f);
    constexpr COLORREF LINE_BRIGHT = RGB(0x8a, 0x6a, 0x36);
    constexpr COLORREF ROW_ON = RGB(70, 54, 34);
    constexpr COLORREF OK_COLOR = RGB(0x9f, 0xb4, 0x6a);
    constexpr COLORREF BAD = RGB(0xcd, 0x7a, 0x5a);

    // The metrics, in device independent pixels (dp), scaled by the window's DPI when drawn.
    constexpr int WINDOW_W = 560;
    constexpr int WINDOW_H = 400;
    constexpr int PAD = 17;
    constexpr int PAD_ROW = 8;
    constexpr int ACT_HEIGHT = 40;
    constexpr int RADIUS = 4;
    constexpr int RULE = 1;
    constexpr int FIELD_HEIGHT = 32;
    constexpr int CHECK = 14;
    constexpr int SIZE_TITLE = 21;
    constexpr int SIZE_ROW = 15;
    constexpr int SIZE_NOTE = 13;
    constexpr int SIZE_CAPTION = 12;

    // The window's states.
    enum class Stage { Choose, Installing, Done, Failed };

    // The hit regions, laid out in dp and scaled at draw and at click.
    enum class Hit { None, Browse, StartMenu, Desktop, Primary, Secondary };

    // Everything the window shows, owned by the interface thread; the worker only posts messages.
    struct State {
        Stage stage = Stage::Choose;
        std::wstring destination;
        oot::install::Options options{ true, true };
        std::wstring report;          // the finish line, or the failure
        std::wstring detail;          // the second line of either
        int done = 0;
        int total = 1;
        Hit hover = Hit::None;
        HWND field = nullptr;         // the destination, an EDIT
        HFONT title_font = nullptr;
        HFONT body_font = nullptr;
        HFONT note_font = nullptr;
        HFONT caption_font = nullptr;
        HFONT field_font = nullptr;
        HBRUSH track_brush = nullptr;
        UINT dpi = 96;
        std::filesystem::path installed_exe;
    };

    State g_state;

    constexpr UINT WM_APP_PROGRESS = WM_APP + 1;   // wParam done, lParam total
    constexpr UINT WM_APP_FINISHED = WM_APP + 2;   // wParam 1 ok, 0 failed; the strings are in the state

    // Everything this reports goes to a file beside the user's other files as well as the window,
    // because a failed install with no explanation is the worst outcome here.
    std::ofstream g_log;

    void say(const std::string& line) {
        if (g_log) {
            g_log << line << "\n";
            g_log.flush();
        }
    }

    // The one native dialog left: the report that the WINDOW itself could not be made, which is
    // the one failure this program has no window to put in.
    void report_failure(const std::wstring& text) {
        MessageBoxW(nullptr, text.c_str(), L"OoT Recompiled could not be installed",
                    MB_OK | MB_ICONERROR);
    }

    int dp(int value) {
        return MulDiv(value, static_cast<int>(g_state.dpi), 96);
    }

    std::wstring widen(const std::string& text) {
        if (text.empty()) {
            return {};
        }
        const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring result(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), length);
        return result;
    }

    // ---- the install itself, on the worker thread -------------------------------------------

    struct Progress {
        HWND window;
        std::atomic<bool>* canceled;
    };

    bool unpack(const std::filesystem::path& into, const Progress& progress, std::string& failure) {
        // RT_RCDATA is MAKEINTRESOURCE(10), the ANSI form unless UNICODE is defined, and this
        // project spells the W functions out. So the type is named by its number.
        const HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(PAYLOAD_RESOURCE),
                                          MAKEINTRESOURCEW(10));
        if (found == nullptr) {
            failure = "this setup file carries no payload";
            return false;
        }
        const HGLOBAL loaded = LoadResource(nullptr, found);
        const void* bytes = loaded != nullptr ? LockResource(loaded) : nullptr;
        const DWORD size = SizeofResource(nullptr, found);
        if (bytes == nullptr || size <= 64) {
            failure = "the payload is present but empty";
            return false;
        }

        mz_zip_archive zip{};
        if (!mz_zip_reader_init_mem(&zip, bytes, size, 0)) {
            failure = "the payload is not readable as an archive";
            return false;
        }

        bool ok = true;
        const mz_uint count = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < count && ok; ++i) {
            if (progress.canceled->load()) {
                failure = "canceled";
                ok = false;
                break;
            }
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
                failure = "an entry of the payload could not be read";
                ok = false;
                break;
            }
            PostMessageW(progress.window, WM_APP_PROGRESS, static_cast<WPARAM>(i + 1), static_cast<LPARAM>(count));
            if (mz_zip_reader_is_file_a_directory(&zip, i)) {
                continue;
            }

            // Our own archive, but still read as hostile input: an entry naming `..` or an
            // absolute path would write outside the folder we chose, and that is the one bug in
            // an unpacker that actually matters.
            const std::filesystem::path entry(stat.m_filename);
            if (entry.is_absolute() || entry.has_root_name()) {
                failure = "refused an absolute path in the payload";
                ok = false;
                break;
            }
            bool escapes = false;
            for (const std::filesystem::path& part : entry) {
                if (part == "..") {
                    escapes = true;
                }
            }
            if (escapes) {
                failure = "refused a path that climbs out of the folder";
                ok = false;
                break;
            }

            size_t length = 0;
            void* data = mz_zip_reader_extract_to_heap(&zip, i, &length, 0);
            if (data == nullptr) {
                failure = "could not decompress " + entry.string();
                ok = false;
                break;
            }

            // Written through std::filesystem so the path stays wide. miniz's own extract to file
            // takes a narrow path and would mangle a profile directory whose name is not ASCII,
            // which is a real machine rather than a hypothetical one.
            const std::filesystem::path target = into / entry;
            std::error_code ec;
            std::filesystem::create_directories(target.parent_path(), ec);
            {
                std::ofstream out(target, std::ios::binary | std::ios::trunc);
                if (out) {
                    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(length));
                }
                if (!out) {
                    failure = "could not write " + target.string();
                    ok = false;
                }
            }
            mz_free(data);
        }

        mz_zip_reader_end(&zip);
        return ok;
    }

    // The whole install, from the folder and the choices to the registered program. Runs on the
    // worker; the outcome goes to the window as a message and to the log as lines.
    void install_worker(HWND window, std::wstring destination, oot::install::Options options,
                        std::atomic<bool>* canceled) {
        std::wstring report;
        std::wstring detail;
        bool ok = false;

        do {
            if (!oot::places::adopt_install_root(std::filesystem::path(destination))) {
                report = L"That folder could not be made.";
                detail = L"Choose a folder on a drive you can write to.";
                break;
            }

            g_log.open(oot::places::data() / "install.log", std::ios::binary | std::ios::trunc);
            say(std::string("OoT Recompiled setup, version ") + oot::build_info::version);
            say("installing into " + oot::places::program().string());

            // The game may be running from a previous install, in which case its own file cannot
            // be replaced. Say so plainly rather than writing half an install.
            const std::filesystem::path exe = oot::places::program() / GAME_EXE;
            std::error_code ec;
            if (std::filesystem::exists(exe, ec)) {
                std::ofstream probe(exe, std::ios::binary | std::ios::app);
                if (!probe) {
                    say("the installed copy is in use");
                    report = L"OoT Recompiled is already running from that folder.";
                    detail = L"Close it and press Install again.";
                    break;
                }
            }

            std::string failure;
            if (!unpack(oot::places::program(), Progress{ window, canceled }, failure)) {
                say(failure);
                if (failure == "canceled") {
                    report = L"Canceled before it finished.";
                    detail = L"Nothing was registered. The files written so far are in the folder above and can be deleted.";
                } else {
                    report = L"The files could not be written.";
                    detail = widen(failure) + L". There is more detail in install.log, in the folder above.";
                }
                break;
            }
            say("payload written");

            // The marker goes down only once the files behind it are there, so a half written
            // install never convinces the game that it is installed.
            if (!oot::places::mark_installed(oot::build_info::version)) {
                say("the marker could not be written");
                report = L"The files were written but the install could not be recorded.";
                detail = L"The folder above may be read only.";
                break;
            }
            say("marked installed");

            oot::install::set_options(options);
            oot::install::register_for(exe);
            say(std::string("registered: start menu ") + (options.start_menu ? "yes" : "no") +
                ", desktop " + (options.desktop ? "yes" : "no"));

            g_state.installed_exe = exe;
            report = L"Installed.";
            std::wstring made;
            if (options.start_menu && options.desktop) {
                made = L"A Start menu entry and a desktop shortcut were made, and it is listed in your installed apps.";
            } else if (options.start_menu) {
                made = L"A Start menu entry was made, and it is listed in your installed apps.";
            } else if (options.desktop) {
                made = L"A desktop shortcut was made, and it is listed in your installed apps.";
            } else {
                made = L"It is listed in your installed apps; no shortcut was made, as you chose.";
            }
            detail = L"Version " + widen(oot::build_info::version) + L" is in the folder above. " + made;
            ok = true;
        } while (false);

        g_state.report = report;
        g_state.detail = detail;
        PostMessageW(window, WM_APP_FINISHED, ok ? 1 : 0, 0);
    }

    std::thread g_worker;
    std::atomic<bool> g_canceled{ false };

    bool start_game(const std::filesystem::path& exe, const std::filesystem::path& working) {
        std::wstring command = L"\"" + exe.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const BOOL started = CreateProcessW(exe.wstring().c_str(), command.data(), nullptr, nullptr,
                                            FALSE, 0, nullptr, working.wstring().c_str(),
                                            &startup, &process);
        if (!started) {
            return false;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }

    // ---- the layout, in dp ---------------------------------------------------------------------

    struct Layout {
        RECT head;        // the lid: mark and title
        RECT where_label; // "Install to"
        RECT field;       // the destination
        RECT browse;      // Browse button, beside the field
        RECT start_menu;  // check box row
        RECT desktop;     // check box row
        RECT progress;    // the bar, in the Installing state
        RECT report;      // the finish or failure text
        RECT primary;     // Install / Start it
        RECT secondary;   // Cancel / Close
    };

    Layout layout() {
        Layout l{};
        const int w = WINDOW_W;
        const int head_h = 56;
        l.head = { 0, 0, w, head_h };
        int y = head_h + PAD;
        l.where_label = { PAD, y, w - PAD, y + 20 };
        y += 24;
        const int browse_w = 92;
        l.field = { PAD, y, w - PAD - browse_w - PAD_ROW, y + FIELD_HEIGHT };
        l.browse = { w - PAD - browse_w, y, w - PAD, y + FIELD_HEIGHT };
        y += FIELD_HEIGHT + PAD;
        l.start_menu = { PAD, y, w - PAD, y + 24 };
        y += 28;
        l.desktop = { PAD, y, w - PAD, y + 24 };
        y += 24 + PAD;
        l.progress = { PAD, y, w - PAD, y + 10 };
        l.report = { PAD, y, w - PAD, y + 64 };
        const int bar_y = WINDOW_H - PAD - ACT_HEIGHT;
        const int half = (w - 2 * PAD - PAD_ROW) / 2;
        l.secondary = { PAD, bar_y, PAD + half, bar_y + ACT_HEIGHT };
        l.primary = { PAD + half + PAD_ROW, bar_y, w - PAD, bar_y + ACT_HEIGHT };
        return l;
    }

    RECT scaled(const RECT& r) {
        return RECT{ dp(r.left), dp(r.top), dp(r.right), dp(r.bottom) };
    }

    bool inside(const RECT& r, POINT p) {
        return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
    }

    Hit hit_test(POINT p) {
        const Layout l = layout();
        if (g_state.stage == Stage::Choose) {
            if (inside(scaled(l.browse), p)) return Hit::Browse;
            if (inside(scaled(l.start_menu), p)) return Hit::StartMenu;
            if (inside(scaled(l.desktop), p)) return Hit::Desktop;
        }
        if (g_state.stage != Stage::Installing) {
            if (inside(scaled(l.primary), p)) return Hit::Primary;
        }
        if (inside(scaled(l.secondary), p)) return Hit::Secondary;
        return Hit::None;
    }

    // ---- drawing ------------------------------------------------------------------------------

    HFONT make_font(const wchar_t* family, int size_dp, int weight) {
        return CreateFontW(-dp(size_dp), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, family);
    }

    void make_fonts() {
        for (HFONT* f : { &g_state.title_font, &g_state.body_font, &g_state.note_font, &g_state.caption_font, &g_state.field_font }) {
            if (*f != nullptr) {
                DeleteObject(*f);
                *f = nullptr;
            }
        }
        g_state.title_font = make_font(L"Georgia", SIZE_TITLE, FW_SEMIBOLD);
        g_state.body_font = make_font(L"Segoe UI", SIZE_ROW, FW_NORMAL);
        g_state.note_font = make_font(L"Segoe UI", SIZE_NOTE, FW_NORMAL);
        g_state.caption_font = make_font(L"Segoe UI", SIZE_CAPTION, FW_NORMAL);
        g_state.field_font = make_font(L"Consolas", SIZE_NOTE, FW_NORMAL);
        if (g_state.field != nullptr) {
            SendMessageW(g_state.field, WM_SETFONT, reinterpret_cast<WPARAM>(g_state.field_font), TRUE);
        }
    }

    void fill_gradient(HDC dc, const RECT& r, COLORREF top, COLORREF bottom) {
        TRIVERTEX vertices[2] = {
            { r.left, r.top, static_cast<COLOR16>(GetRValue(top) << 8), static_cast<COLOR16>(GetGValue(top) << 8), static_cast<COLOR16>(GetBValue(top) << 8), 0 },
            { r.right, r.bottom, static_cast<COLOR16>(GetRValue(bottom) << 8), static_cast<COLOR16>(GetGValue(bottom) << 8), static_cast<COLOR16>(GetBValue(bottom) << 8), 0 }
        };
        GRADIENT_RECT rect{ 0, 1 };
        GradientFill(dc, vertices, 2, &rect, 1, GRADIENT_FILL_RECT_V);
    }

    void fill_rect(HDC dc, const RECT& r, COLORREF color) {
        const HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &r, brush);
        DeleteObject(brush);
    }

    void rounded(HDC dc, const RECT& r, COLORREF fill, COLORREF edge) {
        const HBRUSH brush = CreateSolidBrush(fill);
        const HPEN pen = CreatePen(PS_SOLID, dp(RULE), edge);
        const HGDIOBJ old_brush = SelectObject(dc, brush);
        const HGDIOBJ old_pen = SelectObject(dc, pen);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, dp(RADIUS), dp(RADIUS));
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(brush);
        DeleteObject(pen);
    }

    void text(HDC dc, const RECT& r, const std::wstring& s, HFONT font, COLORREF color, UINT format) {
        const HGDIOBJ old = SelectObject(dc, font);
        SetTextColor(dc, color);
        SetBkMode(dc, TRANSPARENT);
        RECT copy = r;
        DrawTextW(dc, s.c_str(), -1, &copy, format | DT_NOPREFIX);
        SelectObject(dc, old);
    }

    // The mark: our three stacked triangles in brass (assets/ui/mark.rcss draws the same shape
    // with a shader), 24 by 20 dp. A shape, not a name.
    void draw_mark(HDC dc, int x, int y) {
        const HBRUSH brush = CreateSolidBrush(ACCENT);
        const HPEN pen = CreatePen(PS_SOLID, 1, ACCENT);
        const HGDIOBJ old_brush = SelectObject(dc, brush);
        const HGDIOBJ old_pen = SelectObject(dc, pen);
        const int w = dp(24);
        const int h = dp(20);
        const int hw = w / 2;
        const int hh = h / 2;
        // The top triangle, then the two below it, each half the width.
        POINT top[3] = { { x + hw, y }, { x + hw + hw / 2, y + hh }, { x + hw - hw / 2, y + hh } };
        POINT left[3] = { { x + hw / 2, y + hh }, { x + hw, y + h }, { x, y + h } };
        POINT right[3] = { { x + hw + hw / 2, y + hh }, { x + w, y + h }, { x + hw, y + h } };
        Polygon(dc, top, 3);
        Polygon(dc, left, 3);
        Polygon(dc, right, 3);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(brush);
        DeleteObject(pen);
    }

    void draw_check(HDC dc, const RECT& row, bool on, bool hover, const std::wstring& label) {
        const int box = dp(CHECK);
        const int top = row.top + ((row.bottom - row.top) - box) / 2;
        RECT square{ row.left, top, row.left + box, top + box };
        rounded(dc, square, on ? ACCENT : TRACK, (on || hover) ? ACCENT : LINE_BRIGHT);
        if (on) {
            // The tick: two strokes in the on-accent ink.
            const HPEN pen = CreatePen(PS_SOLID, dp(2), ON_ACCENT);
            const HGDIOBJ old = SelectObject(dc, pen);
            MoveToEx(dc, square.left + box / 4, square.top + box / 2, nullptr);
            LineTo(dc, square.left + box / 2 - dp(1), square.bottom - box / 4);
            LineTo(dc, square.right - box / 5, square.top + box / 4);
            SelectObject(dc, old);
            DeleteObject(pen);
        }
        RECT label_rect{ square.right + dp(PAD_ROW) + dp(2), row.top, row.right, row.bottom };
        text(dc, label_rect, label, g_state.body_font, hover ? TEXT : TEXT_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    void draw_button(HDC dc, const RECT& r, const std::wstring& label, bool primary, bool hover, bool enabled) {
        COLORREF fill = primary ? ACCENT : CASE_BOTTOM;
        COLORREF edge = primary ? ACCENT : LINE;
        COLORREF ink = primary ? ON_ACCENT : TEXT_DIM;
        if (!enabled) {
            fill = primary ? LINE : CASE_BOTTOM;
            edge = LINE;
            ink = TEXT_DIM;
        } else if (hover) {
            fill = primary ? BRASS_LIGHT : ROW_ON;
            edge = primary ? BRASS_LIGHT : LINE_BRIGHT;
            ink = primary ? ON_ACCENT : TEXT;
        }
        rounded(dc, r, fill, edge);
        text(dc, r, label, g_state.body_font, ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    void paint(HWND window) {
        PAINTSTRUCT ps{};
        const HDC screen = BeginPaint(window, &ps);
        RECT client{};
        GetClientRect(window, &client);

        // Off screen, so the window never flickers between the ground and what sits on it.
        const HDC dc = CreateCompatibleDC(screen);
        const HBITMAP bitmap = CreateCompatibleBitmap(screen, client.right, client.bottom);
        const HGDIOBJ old_bitmap = SelectObject(dc, bitmap);

        const Layout l = layout();
        fill_gradient(dc, client, CASE_TOP, CASE_BOTTOM);

        // The lid: the mark, the title, the version; a rule beneath, as the case's lid has.
        const RECT head = scaled(l.head);
        draw_mark(dc, dp(PAD), head.top + (head.bottom - head.top - dp(20)) / 2);
        RECT title{ dp(PAD) + dp(24) + dp(PAD_ROW) + dp(4), head.top, head.right - dp(PAD), head.bottom };
        text(dc, title, L"OoT Recompiled", g_state.title_font, TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        text(dc, title, L"Setup, version " + widen(oot::build_info::version), g_state.caption_font, TEXT_DIM,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        RECT rule{ 0, head.bottom - dp(RULE), head.right, head.bottom };
        fill_rect(dc, rule, LINE);

        const bool choosing = g_state.stage == Stage::Choose;
        text(dc, scaled(l.where_label), choosing ? L"Install to" : L"Installed to", g_state.note_font, TEXT_DIM,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // The field's frame; the EDIT sits inside it in the Choose state and the path is drawn as
        // text afterward.
        RECT field = scaled(l.field);
        if (choosing) {
            RECT frame = field;
            InflateRect(&frame, dp(1), dp(1));
            rounded(dc, frame, TRACK, LINE_BRIGHT);
            draw_button(dc, scaled(l.browse), L"Browse", false, g_state.hover == Hit::Browse, true);
        } else {
            RECT wide = field;
            wide.right = scaled(l.browse).right;
            text(dc, wide, g_state.destination, g_state.field_font, TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        }

        if (choosing) {
            draw_check(dc, scaled(l.start_menu), g_state.options.start_menu, g_state.hover == Hit::StartMenu, L"Add a Start menu entry");
            draw_check(dc, scaled(l.desktop), g_state.options.desktop, g_state.hover == Hit::Desktop, L"Add a shortcut to the desktop");
            text(dc, scaled(l.report), L"The program, its saves and its mods all live in that folder, and nothing is written anywhere else. Running this again later updates it in place.",
                 g_state.caption_font, TEXT_DIM, DT_LEFT | DT_WORDBREAK);
        } else if (g_state.stage == Stage::Installing) {
            RECT bar = scaled(l.progress);
            rounded(dc, bar, TRACK, LINE);
            const int total = g_state.total > 0 ? g_state.total : 1;
            RECT done = bar;
            done.right = bar.left + static_cast<int>((static_cast<long long>(bar.right - bar.left) * g_state.done) / total);
            if (done.right > done.left + dp(2)) {
                rounded(dc, done, ACCENT, ACCENT);
            }
            RECT under = scaled(l.report);
            under.top = bar.bottom + dp(PAD_ROW);
            text(dc, under, L"Writing the files, " + std::to_wstring(g_state.done) + L" of " + std::to_wstring(g_state.total),
                 g_state.note_font, TEXT_DIM, DT_LEFT | DT_SINGLELINE);
        } else {
            RECT report = scaled(l.report);
            const COLORREF tone = g_state.stage == Stage::Done ? OK_COLOR : BAD;
            RECT headline = report;
            headline.bottom = headline.top + dp(24);
            text(dc, headline, g_state.report, g_state.body_font, tone, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            RECT detail = report;
            detail.top = headline.bottom + dp(4);
            detail.bottom = scaled(l.primary).top - dp(PAD_ROW);
            text(dc, detail, g_state.detail, g_state.caption_font, TEXT_DIM, DT_LEFT | DT_WORDBREAK);
        }

        // The action bar, with its rule above it as the launcher's has.
        RECT bar_rule{ dp(PAD), scaled(l.primary).top - dp(PAD_ROW) - dp(RULE), client.right - dp(PAD), scaled(l.primary).top - dp(PAD_ROW) };
        fill_rect(dc, bar_rule, LINE);
        std::wstring primary_label = L"Install";
        std::wstring secondary_label = L"Cancel";
        bool primary_enabled = true;
        if (g_state.stage == Stage::Installing) {
            primary_label = L"Installing";
            primary_enabled = false;
        } else if (g_state.stage == Stage::Done) {
            primary_label = L"Start it";
            secondary_label = L"Close";
        } else if (g_state.stage == Stage::Failed) {
            primary_label = L"Try again";
            secondary_label = L"Close";
        }
        draw_button(dc, scaled(l.secondary), secondary_label, false, g_state.hover == Hit::Secondary, true);
        draw_button(dc, scaled(l.primary), primary_label, true, g_state.hover == Hit::Primary, primary_enabled);

        BitBlt(screen, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old_bitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(window, &ps);
    }

    // ---- the actions -----------------------------------------------------------------------

    std::wstring field_text() {
        if (g_state.field == nullptr) {
            return g_state.destination;
        }
        const int length = GetWindowTextLengthW(g_state.field);
        std::wstring result(static_cast<size_t>(length) + 1, L'\0');
        GetWindowTextW(g_state.field, result.data(), length + 1);
        result.resize(static_cast<size_t>(length));
        while (!result.empty() && (result.back() == L' ' || result.back() == L'\\')) {
            result.pop_back();
        }
        return result;
    }

    void place_field(HWND window) {
        if (g_state.field == nullptr) {
            return;
        }
        const RECT field = scaled(layout().field);
        // Inside the drawn frame, centered on its line.
        const int text_h = dp(SIZE_NOTE) + dp(4);
        const int top = field.top + ((field.bottom - field.top) - text_h) / 2;
        SetWindowPos(g_state.field, nullptr, field.left + dp(PAD_ROW), top, (field.right - field.left) - 2 * dp(PAD_ROW), text_h, SWP_NOZORDER);
        ShowWindow(g_state.field, g_state.stage == Stage::Choose ? SW_SHOW : SW_HIDE);
        (void)window;
    }

    // The folder picker is the operating system's, which CLAUDE.md allows where a file or folder
    // is chosen (the ROM browser's Browse opens it too).
    void browse(HWND window) {
        IFileOpenDialog* dialog = nullptr;
        const HRESULT started = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const bool ours = SUCCEEDED(started);
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
            DWORD flags = 0;
            dialog->GetOptions(&flags);
            dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
            dialog->SetTitle(L"Where OoT Recompiled installs");
            if (SUCCEEDED(dialog->Show(window))) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item))) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        // The chosen folder holds the program's own folder, so a person who picks
                        // their Documents does not get bin\ and saves\ loose in it.
                        std::filesystem::path chosen(path);
                        if (chosen.filename() != L"OoT Recompiled") {
                            chosen /= L"OoT Recompiled";
                        }
                        SetWindowTextW(g_state.field, chosen.wstring().c_str());
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            dialog->Release();
        }
        if (ours) {
            CoUninitialize();
        }
    }

    void begin_install(HWND window) {
        const std::wstring destination = field_text();
        if (destination.empty() || !std::filesystem::path(destination).is_absolute()) {
            g_state.stage = Stage::Failed;
            g_state.report = L"That is not a folder.";
            g_state.detail = L"Type a full path such as C:\\Games\\OoT Recompiled, or press Browse.";
            place_field(window);
            InvalidateRect(window, nullptr, FALSE);
            return;
        }
        g_state.destination = destination;
        g_state.stage = Stage::Installing;
        g_state.done = 0;
        g_state.total = 1;
        g_canceled.store(false);
        place_field(window);
        InvalidateRect(window, nullptr, FALSE);
        if (g_worker.joinable()) {
            g_worker.join();
        }
        g_worker = std::thread(install_worker, window, destination, g_state.options, &g_canceled);
    }

    void click(HWND window, Hit hit) {
        switch (hit) {
            case Hit::Browse:
                browse(window);
                break;
            case Hit::StartMenu:
                g_state.options.start_menu = !g_state.options.start_menu;
                InvalidateRect(window, nullptr, FALSE);
                break;
            case Hit::Desktop:
                g_state.options.desktop = !g_state.options.desktop;
                InvalidateRect(window, nullptr, FALSE);
                break;
            case Hit::Primary:
                if (g_state.stage == Stage::Choose || g_state.stage == Stage::Failed) {
                    if (g_state.stage == Stage::Failed) {
                        g_state.stage = Stage::Choose;
                        SetWindowTextW(g_state.field, g_state.destination.c_str());
                    }
                    begin_install(window);
                } else if (g_state.stage == Stage::Done) {
                    if (!start_game(g_state.installed_exe, g_state.installed_exe.parent_path())) {
                        g_state.stage = Stage::Failed;
                        g_state.report = L"Installed, but it would not start.";
                        g_state.detail = L"Open it from the folder above or from your Start menu.";
                        InvalidateRect(window, nullptr, FALSE);
                        break;
                    }
                    say("started");
                    DestroyWindow(window);
                }
                break;
            case Hit::Secondary:
                if (g_state.stage == Stage::Installing) {
                    g_canceled.store(true);
                } else {
                    DestroyWindow(window);
                }
                break;
            case Hit::None:
                break;
        }
    }

    LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_CREATE: {
                g_state.dpi = GetDpiForWindow(window);
                make_fonts();
                g_state.track_brush = CreateSolidBrush(TRACK);
                g_state.field = CreateWindowExW(0, L"EDIT", g_state.destination.c_str(),
                                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
                                                0, 0, 10, 10, window, nullptr, nullptr, nullptr);
                SendMessageW(g_state.field, WM_SETFONT, reinterpret_cast<WPARAM>(g_state.field_font), TRUE);
                place_field(window);
                return 0;
            }
            case WM_DPICHANGED: {
                g_state.dpi = HIWORD(wparam);
                make_fonts();
                const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
                SetWindowPos(window, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
                place_field(window);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            case WM_CTLCOLOREDIT: {
                const HDC dc = reinterpret_cast<HDC>(wparam);
                SetTextColor(dc, TEXT);
                SetBkColor(dc, TRACK);
                return reinterpret_cast<LRESULT>(g_state.track_brush);
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
                paint(window);
                return 0;
            case WM_MOUSEMOVE: {
                const POINT p{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
                const Hit hit = hit_test(p);
                if (hit != g_state.hover) {
                    g_state.hover = hit;
                    InvalidateRect(window, nullptr, FALSE);
                }
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = window;
                TrackMouseEvent(&track);
                return 0;
            }
            case WM_MOUSELEAVE:
                if (g_state.hover != Hit::None) {
                    g_state.hover = Hit::None;
                    InvalidateRect(window, nullptr, FALSE);
                }
                return 0;
            case WM_LBUTTONUP: {
                const POINT p{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
                click(window, hit_test(p));
                return 0;
            }
            case WM_KEYDOWN:
                if (wparam == VK_RETURN) {
                    click(window, g_state.stage == Stage::Installing ? Hit::None : Hit::Primary);
                    return 0;
                }
                if (wparam == VK_ESCAPE) {
                    click(window, Hit::Secondary);
                    return 0;
                }
                break;
            case WM_APP_PROGRESS:
                g_state.done = static_cast<int>(wparam);
                g_state.total = static_cast<int>(lparam);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            case WM_APP_FINISHED:
                if (g_worker.joinable()) {
                    g_worker.join();
                }
                g_state.stage = (wparam != 0) ? Stage::Done : Stage::Failed;
                place_field(window);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            case WM_CLOSE:
                if (g_state.stage == Stage::Installing) {
                    g_canceled.store(true);
                    return 0;
                }
                DestroyWindow(window);
                return 0;
            case WM_DESTROY:
                if (g_worker.joinable()) {
                    g_canceled.store(true);
                    g_worker.join();
                }
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

} // namespace

int main() {
    // Per monitor DPI, so the window is crisp beside the game's own on a scaled display.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const std::filesystem::path suggested = oot::places::default_install_root();
    if (suggested.empty()) {
        report_failure(L"Windows would not say where this account's application data folder is, "
                       L"so there is no folder to suggest. Nothing was installed.");
        return 1;
    }
    g_state.destination = suggested.wstring();

    // The Enter key on the EDIT would otherwise be swallowed by it; the window class handles the
    // keys for the whole window, and the EDIT hands the ones it does not use up.
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.style = CS_HREDRAW | CS_VREDRAW;
    cls.lpfnWndProc = window_proc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hIcon = LoadIconW(cls.hInstance, MAKEINTRESOURCEW(ICON_RESOURCE));
    cls.hIconSm = cls.hIcon;
    // IDC_ARROW is the ANSI form of resource 32512 here (this project spells the W functions
    // out and does not define UNICODE), so the arrow is named by its number.
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    cls.lpszClassName = WINDOW_CLASS;
    if (RegisterClassExW(&cls) == 0) {
        report_failure(L"The setup window could not be made.");
        return 1;
    }

    // Sized in dp at the primary monitor's DPI, then let WM_DPICHANGED follow the window.
    const UINT dpi = GetDpiForSystem();
    g_state.dpi = dpi;
    RECT frame{ 0, 0, dp(WINDOW_W), dp(WINDOW_H) };
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectExForDpi(&frame, style, FALSE, 0, dpi);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    const HWND window = CreateWindowExW(0, WINDOW_CLASS, L"OoT Recompiled setup", style, x, y, width, height,
                                        nullptr, nullptr, cls.hInstance, nullptr);
    if (window == nullptr) {
        report_failure(L"The setup window could not be made.");
        return 1;
    }
    ShowWindow(window, SW_SHOW);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Enter and Escape reach the window whatever has the focus, so the field never eats them.
        if (msg.message == WM_KEYDOWN && (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE)) {
            msg.hwnd = window;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
