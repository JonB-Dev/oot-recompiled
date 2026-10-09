#include "main/launch.h"
#include "game/moments.h"
#include "main/places.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <deque>
#include <mutex>
#include <system_error>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <commdlg.h>

#include <SDL.h>
#include <SDL_syswm.h>

#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"

#include "game/game.h"
#include "main/rom_source.h"
#include "ui/ui_settings.h"
#include "ui/ui_shell.h"

namespace oot::launch {

    namespace {

        std::u8string g_game_id;
        std::mutex g_mutex;
        RomStatus g_status;

        // The accepted ROM, held where the user put it and read from there. The bytes are kept
        // from the moment the file is accepted until Play hands them to the runtime, because the
        // alternative is reading 32 MB twice; the PATH is what is written down for next time.
        // See main/rom_source.h for why the runtime's own store is not used.
        std::vector<uint8_t> g_rom_bytes;
        std::filesystem::path g_rom_path;

        struct Request {
            enum class Kind { SelectRom, PickRom, Play, Quit, Minimize, Maximize, Rows, KeepRom } kind = Kind::Play;
            std::filesystem::path path;
            // Rows: how many list rows the document that is opening needs the window to hold.
            int rows = 0;
        };
        std::deque<Request> g_requests;
        std::atomic<bool> g_started{ false };

        // The window, and its two shapes. The launcher's size fits the case with its eleven
        // rows and the browser's list; the game's is the size the project has always opened
        // at (four times the console's lines, at its ratio), and the settings take it from
        // there (fullscreen at the desktop's mode when they say so).
        SDL_Window* g_window = nullptr;
        bool g_launcher_shape = false;
        constexpr int LAUNCHER_WIDTH = 660;
        // Thirteen rows since the frame rate counter's row (2026-09-19), and the foot's second
        // line: taller than the 540 the launcher began with.
        // THE HEIGHT FOLLOWS THE ROW COUNT, because a fixed one silently clips. It was 580 for
        // the rows the launcher had before the render distance row was added, and the window did
        // not grow with the list: the foot line ran off the bottom edge and the user photographed
        // it. Adding a settings row already touched five places; this was the sixth nobody had
        // written down.
        //
        // The chrome is the lid, the foot, the signature line, the action bar and the case's own
        // padding; the rest is rows. Measured from the shipped layout rather than derived from
        // the stylesheet, because the stylesheet is in dp and this is in pixels, and a wrong
        // conversion here is a window that clips again.
        constexpr int LAUNCHER_ROW_H = 34;
        // One foot row now, not two (the user, 2026-09-20), so the chrome lost a line.
        constexpr int LAUNCHER_CHROME_H = 178;
        // THE WINDOW NEVER OUTGROWS THE SCREEN (2026-09-24, the user: "launcher ... Now too
        // large", its foot cut off by the window's own bottom edge). The height below is the
        // list's, and the list grew by four rows this afternoon; past the display's working
        // area (the desktop less its taskbar) the window simply hangs off the screen and the
        // foot with it. Capped here, the list scrolls inside what is left, which the rows
        // document already does when its cap is smaller than its content.
        int fit_to_display(int height) {
            SDL_Rect usable{};
            const int display = (g_window != nullptr) ? SDL_GetWindowDisplayIndex(g_window) : 0;
            if (SDL_GetDisplayUsableBounds(display >= 0 ? display : 0, &usable) != 0 || usable.h <= 0) {
                return height;
            }
            // A little room so the window is not flush against the edge of the working area.
            const int most = usable.h - 48;
            return (most > LAUNCHER_CHROME_H && height > most) ? most : height;
        }

        int launcher_height() {
            // The ROM row, every setting, Controls, and Your files. Play and Quit are not counted:
            // they live in the action bar below the list, which the chrome height already covers.
            const int rows = 3 + oot::ui::row_count();
            return fit_to_display(LAUNCHER_CHROME_H + rows * LAUNCHER_ROW_H);
        }
        constexpr int GAME_WIDTH = 960;
        constexpr int GAME_HEIGHT = 720;

        // The desktop's size on the window's display, for game_window_size. Read here on the main
        // thread whenever the window is attached or reshaped, and kept for the threads that plan.
        std::atomic<int> g_desktop_width{ 0 };
        std::atomic<int> g_desktop_height{ 0 };

        void note_desktop() {
            SDL_DisplayMode mode{};
            const int display = (g_window != nullptr) ? SDL_GetWindowDisplayIndex(g_window) : 0;
            if (SDL_GetDesktopDisplayMode(display >= 0 ? display : 0, &mode) == 0 && mode.w > 0 && mode.h > 0) {
                g_desktop_width.store(mode.w);
                g_desktop_height.store(mode.h);
            }
        }
        // The lid's band, draggable, and the three buttons at its right end, which are not: the
        // same numbers as tokens.rcss's lid and --win-btn, in pixels at the window's scale.
        //
        // THE BUTTON WIDTH HAS TO FOLLOW --win-btn AND IS NOT CHECKED BY ANYTHING. It was 36,
        // for the 32dp buttons the launcher used to have; the two bars now draw the same 22dp
        // button (the user, 2026-09-20: "make them both the same exact size as the now in-game
        // window bar"), so this is the game bar's number below. Left at 36 it would reserve a
        // hundred and eight pixels for seventy two of buttons, and the empty strip to their left
        // would refuse to drag the window for no reason a person could see.
        constexpr int CHROME_HEIGHT = 60;
        constexpr int CHROME_BUTTONS = 3 * 24;

        SDL_HitTestResult hit_test(SDL_Window* window, const SDL_Point* area, void*) {
            int width = 0;
            int height = 0;
            SDL_GetWindowSize(window, &width, &height);
            if (area->y < CHROME_HEIGHT && area->x < width - CHROME_BUTTONS) {
                return SDL_HITTEST_DRAGGABLE;
            }
            return SDL_HITTEST_NORMAL;
        }

        // The game window's hit test: its title bar drags (a regular bar's height, tokens.rcss's
        // --chrome-bar, with the three small buttons at its right end left to the header), and
        // the edges and corners resize, since the window has no border of the system's to take
        // hold of.
        constexpr int RESIZE_EDGE = 8;
        constexpr int TITLE_BAR = 30;
        constexpr int TITLE_BUTTONS = 3 * 24;

        // The renderer takes the window fullscreen by its own means (the window over the whole
        // display), which SDL's flags never report, so fullscreen is judged by geometry: the
        // window's position and size against its display's bounds.
        bool window_covers_display(SDL_Window* window) {
            if (window == nullptr) {
                return false;
            }
            const int index = SDL_GetWindowDisplayIndex(window);
            SDL_Rect bounds{};
            if (index < 0 || SDL_GetDisplayBounds(index, &bounds) != 0) {
                return false;
            }
            int x = 0;
            int y = 0;
            int width = 0;
            int height = 0;
            SDL_GetWindowPosition(window, &x, &y);
            SDL_GetWindowSize(window, &width, &height);
            return x <= bounds.x && y <= bounds.y &&
                   x + width >= bounds.x + bounds.w && y + height >= bounds.y + bounds.h;
        }

        // The Windows file picker (the user's rule of 2026-09-19: the platform's picker is not a
        // message box and is allowed), owned by our window, on the main thread, which is the
        // thread that made the window. The chosen file goes to the browser's path field and
        // through the same validation as a drop or a typed path.
        void pick_rom_with_dialog() {
            wchar_t file[MAX_PATH * 4] = {};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            SDL_SysWMinfo info{};
            SDL_VERSION(&info.version);
            if (g_window != nullptr && SDL_GetWindowWMInfo(g_window, &info)) {
                ofn.hwndOwner = info.info.win.window;
            }
            ofn.lpstrFilter = L"N64 ROM (*.z64;*.n64;*.v64)\0*.z64;*.n64;*.v64\0All files (*.*)\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH * 4;
            ofn.lpstrTitle = L"Choose your ROM";
            const std::wstring initial = executable_directory().wstring();
            ofn.lpstrInitialDir = initial.c_str();
            ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (!GetOpenFileNameW(&ofn)) {
                std::fprintf(stderr, "[launch] picker: nothing chosen\n");
                return;
            }
            const std::filesystem::path path(file);
            const std::u8string u8 = path.u8string();
            std::fprintf(stderr, "[launch] picker: %s\n", path.filename().string().c_str());
            oot::ui::shell::post_browse_path(std::string(u8.begin(), u8.end()));
            request_select_rom(path);
        }

        SDL_HitTestResult game_hit_test(SDL_Window* window, const SDL_Point* area, void*) {
            // Fullscreen has no edges to drag or resize by (the user's note of 2026-09-19).
            if (window_covers_display(window)) {
                return SDL_HITTEST_NORMAL;
            }
            int width = 0;
            int height = 0;
            SDL_GetWindowSize(window, &width, &height);
            const bool left = area->x < RESIZE_EDGE;
            const bool right = area->x >= width - RESIZE_EDGE;
            const bool top = area->y < RESIZE_EDGE;
            const bool bottom = area->y >= height - RESIZE_EDGE;
            if (top && left)     return SDL_HITTEST_RESIZE_TOPLEFT;
            if (top && right)    return SDL_HITTEST_RESIZE_TOPRIGHT;
            if (bottom && left)  return SDL_HITTEST_RESIZE_BOTTOMLEFT;
            if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
            if (top)             return SDL_HITTEST_RESIZE_TOP;
            if (bottom)          return SDL_HITTEST_RESIZE_BOTTOM;
            if (left)            return SDL_HITTEST_RESIZE_LEFT;
            if (right)           return SDL_HITTEST_RESIZE_RIGHT;
            if (area->y < TITLE_BAR && area->x < width - TITLE_BUTTONS) {
                return SDL_HITTEST_DRAGGABLE;
            }
            return SDL_HITTEST_NORMAL;
        }

        const char* error_name(recomp::RomValidationError e) {
            switch (e) {
                case recomp::RomValidationError::Good:             return "accepted";
                case recomp::RomValidationError::FailedToOpen:     return "the file could not be opened";
                case recomp::RomValidationError::NotARom:          return "not an N64 ROM";
                case recomp::RomValidationError::IncorrectRom:     return "a ROM, but not this game";
                case recomp::RomValidationError::NotYet:           return "not supported yet";
                case recomp::RomValidationError::IncorrectVersion: return "this game, but not the NTSC-U 1.0 version";
                default:                                           return "refused";
            }
        }

        void set_status(RomState state, const std::string& name, const std::string& detail) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_status.state = state;
            g_status.name = name;
            g_status.detail = detail;
        }

        // Validate where the file lies and keep it there. `state` is what an accepted one reads
        // as (found beside the program, chosen, or the one remembered from last time).
        //
        // The runtime's `select_rom` used to do this, and it also wrote a 32 MB copy of the game
        // into the config directory as a side effect, which is the duplication the user reported
        // on 2026-09-20. `oot::rom::validate` performs the identical checks, in the identical
        // order, and writes nothing.
        bool take(const std::filesystem::path& path, RomState state, bool remember_it = true) {
            const std::string name = path.filename().string();
            std::vector<uint8_t> bytes;
            const auto err = oot::rom::validate(path, oot::game_entry(), bytes);
            if (err == recomp::RomValidationError::Good) {
                g_rom_bytes = std::move(bytes);
                g_rom_path = path;
                if (remember_it) {
                    oot::rom::remember(recomp::get_config_path(), path);
                }
                set_status(state, name, "");
                std::fprintf(stderr, "[launch] rom accepted where it is: %s\n", path.string().c_str());
                return true;
            }
            set_status(RomState::Refused, name, error_name(err));
            std::fprintf(stderr, "[launch] rom refused: %s (%s)\n", name.c_str(), error_name(err));
            return false;
        }

        std::string lower(std::string text) {
            for (char& c : text) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return text;
        }

    } // namespace

    const std::filesystem::path& rom_path() {
        return g_rom_path;
    }

    const std::filesystem::path& executable_directory() {
        static const std::filesystem::path here = [] {
            wchar_t buffer[MAX_PATH * 4] = {};
            const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
            if (length == 0) {
                return std::filesystem::current_path();
            }
            return std::filesystem::path(std::wstring(buffer, length)).parent_path();
        }();
        return here;
    }

    void init(const std::u8string& game_id, const std::filesystem::path& cli_rom) {
        g_game_id = game_id;

        // The config directory: the saves, the mods and the remembered ROM path. This used to say
        // "beside the executable rather than in the user's profile, which is the whole program's
        // one call for this, so moving it later is this line". It was, and this is that move: the
        // answer comes from src/main/places.h now, which is the executable's own directory in a
        // build tree and the user's own application data in the shipped single file.
        recomp::register_config_path(oot::places::data());

        // An earlier build used the runtime's own ROM store, which kept a 32 MB copy of the game
        // here. Nothing reads it any more. It is reported rather than removed, because a ROM is
        // the one file in this project that must never be destroyed by accident and deleting the
        // user's data is not this program's decision to make on its own.
        const std::filesystem::path stale = oot::rom::stale_stored_copy(recomp::get_config_path(), oot::game_entry());
        if (!stale.empty()) {
            std::fprintf(stderr,
                         "[launch] a copy of the game made by an earlier version is still here and is no longer\n"
                         "[launch] used: %s\n"
                         "[launch] it can be deleted; your own ROM is not affected.\n",
                         stale.string().c_str());
        }

        // The file the user chose last time, validated again where it lies. It is re-validated
        // rather than trusted because it is a path to a file this program does not own: it can
        // have been moved, replaced or edited since, and the hash check is the only thing that
        // makes reading it safe.
        const std::filesystem::path previous = oot::rom::remembered(recomp::get_config_path());
        if (!previous.empty()) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(previous, ec)) {
                std::fprintf(stderr, "[launch] the ROM chosen last time is no longer there: %s\n",
                             previous.string().c_str());
                oot::rom::forget(recomp::get_config_path());
            }
            // Remembering it again would rewrite the same path; it is already written down.
            else if (take(previous, RomState::Stored, false)) {
                return;
            }
            else {
                // It is there but no longer the game, so stop offering it.
                oot::rom::forget(recomp::get_config_path());
            }
        }

        if (!cli_rom.empty() && take(cli_rom, RomState::Accepted)) {
            return;
        }

        // Beside the executable, the way the reference project loads one, judged by CONTENT and
        // never by name: the user asked that a legitimate copy work whatever it is called. Every
        // file there with a ROM extension is tried, then every other file of a ROM's size, and
        // the hash decides; a refused file is skipped, not reported, and the first copy that is
        // the game wins.
        //
        // The copy an earlier build's ROM store left behind is excluded by its name, and that
        // exclusion matters MORE now than it did. It is a valid ROM, so without this the program
        // would adopt the duplicate it is meant to be getting rid of and keep it alive forever.
        // A user whose only remaining copy is that file can still choose it in the browser, and
        // the line printed above tells them it is there.
        const std::string own = lower(std::string(g_game_id.begin(), g_game_id.end()) + ".z64");
        constexpr uintmax_t ROM_SIZE_LOW = 8u * 1024u * 1024u;
        constexpr uintmax_t ROM_SIZE_HIGH = 64u * 1024u * 1024u;
        std::vector<std::filesystem::path> by_extension;
        std::vector<std::filesystem::path> by_size;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(executable_directory(), ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            if (lower(entry.path().filename().string()) == own) {
                continue;
            }
            if (is_rom_file(entry.path())) {
                by_extension.push_back(entry.path());
                continue;
            }
            // A ROM under another name is tried by its size, but the program's own files (the
            // executable, a DLL, a zip, a log) are never ROMs and are large enough to qualify;
            // hashing them every start cost time and two puzzling log lines.
            if (is_program_file(entry.path())) {
                continue;
            }
            const uintmax_t size = entry.file_size(ec);
            if (!ec && size >= ROM_SIZE_LOW && size <= ROM_SIZE_HIGH) {
                by_size.push_back(entry.path());
            }
        }
        std::vector<std::filesystem::path> candidates = by_extension;
        candidates.insert(candidates.end(), by_size.begin(), by_size.end());
        for (const std::filesystem::path& path : candidates) {
            std::fprintf(stderr, "[launch] trying a file beside the program: %s\n", path.filename().string().c_str());
            if (take(path, RomState::Beside)) {
                return;
            }
        }

        if (candidates.empty()) {
            std::fprintf(stderr, "[launch] rom: nothing beside the program looks like one; the launcher will ask\n");
        }
        else {
            std::fprintf(stderr, "[launch] rom: %zu file(s) beside the program tried, none is the game; the launcher will ask\n",
                         candidates.size());
        }
        set_status(RomState::None, "", "");
    }

    RomStatus rom_status() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_status;
    }

    bool rom_ready() {
        const RomState state = rom_status().state;
        return state == RomState::Stored || state == RomState::Beside || state == RomState::Accepted;
    }

    // One implementation for both ways into the game, Play and --play, so the two cannot drift:
    // the bytes read out of the user's own file go straight to the runtime. There is no copy on
    // disk to fall back to, which is the point.
    bool load_rom_into_runtime() {
        if (!rom_ready() || g_rom_bytes.empty()) {
            std::fprintf(stderr, "[launch] no ROM to load\n");
            return false;
        }
        const size_t size = g_rom_bytes.size();
        recomp::set_rom_contents(std::move(g_rom_bytes));
        g_rom_bytes.clear();
        std::fprintf(stderr, "[launch] rom loaded, %zu bytes, read from %s\n", size, g_rom_path.string().c_str());
        return true;
    }

    void request_select_rom(const std::filesystem::path& path) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_requests.push_back(Request{ Request::Kind::SelectRom, path });
    }

    void request_pick_rom() {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_requests.push_back(Request{ Request::Kind::PickRom, std::filesystem::path() });
    }

    void request_play() {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_requests.push_back(Request{ Request::Kind::Play, std::filesystem::path() });
    }

    void request_quit() {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_requests.push_back(Request{ Request::Kind::Quit, std::filesystem::path() });
    }

    void request_minimize() {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_requests.push_back(Request{ Request::Kind::Minimize, std::filesystem::path() });
    }

    void request_maximize() {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_requests.push_back(Request{ Request::Kind::Maximize, std::filesystem::path() });
    }

    // The launcher's window is sized to its list, and the documents that share that window have
    // lists of different lengths, so the window follows whichever is up rather than being sized
    // once for the longest. Posted by the shell as a screen opens; done on the main thread here,
    // because SDL window calls belong to the thread that made the window.
    void request_keep_rom_copy() {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_requests.push_back(Request{ Request::Kind::KeepRom, std::filesystem::path() });
    }

    int launcher_chrome_height() {
        return LAUNCHER_CHROME_H;
    }

    void request_window_rows(int rows) {
        std::lock_guard<std::mutex> lock(g_mutex);
        Request r{ Request::Kind::Rows, std::filesystem::path() };
        r.rows = rows;
        g_requests.push_back(r);
    }

    void attach_window(SDL_Window* window) {
        g_window = window;
        note_desktop();
    }

    void shape_for_launcher() {
        if (g_window == nullptr) {
            return;
        }
        SDL_SetWindowBordered(g_window, SDL_FALSE);
        const int height = launcher_height();
        SDL_SetWindowSize(g_window, LAUNCHER_WIDTH, height);
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        SDL_SetWindowHitTest(g_window, hit_test, nullptr);
        g_launcher_shape = true;
        std::fprintf(stderr, "[launch] window shaped for the launcher, %d by %d\n", LAUNCHER_WIDTH, height);
    }

    void shape_for_game() {
        if (g_window == nullptr) {
            return;
        }
        // Borderless like the launcher, with our own header drawn over the picture when the
        // pointer is at the top (the shell's frame document) and the edges resizing through
        // the hit test. Coming from the launcher's shape, the window takes the game's size.
        SDL_SetWindowBordered(g_window, SDL_FALSE);
        SDL_SetWindowHitTest(g_window, game_hit_test, nullptr);
        if (g_launcher_shape) {
            SDL_SetWindowSize(g_window, GAME_WIDTH, GAME_HEIGHT);
            SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
            g_launcher_shape = false;
        }
        note_desktop();
        std::fprintf(stderr, "[launch] window shaped for the game, %d by %d\n", GAME_WIDTH, GAME_HEIGHT);
    }

    bool tick() {
        bool started_now = false;
        std::deque<Request> local;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            local.swap(g_requests);
        }
        for (const Request& r : local) {
            switch (r.kind) {
                case Request::Kind::SelectRom:
                    take(r.path, RomState::Accepted);
                    break;

                case Request::Kind::PickRom:
                    pick_rom_with_dialog();
                    break;

                case Request::Kind::Play:
                    if (g_started.load()) {
                        break;
                    }
                    if (!load_rom_into_runtime()) {
                        set_status(RomState::Refused, "", "the ROM could not be read");
                        break;
                    }
                    recomp::start_game(g_game_id, "");
                    g_started.store(true);
                    started_now = true;
                    break;

                case Request::Kind::Quit:
                    std::fprintf(stderr, "[launch] quit from the launcher\n");
                    // Save on quit (phase 78b): a moment first, when the row asks; the pump quits
                    // once it has landed. Otherwise now.
                    if (!oot::moments::on_quit_requested(game_started())) {
                        ultramodern::quit();
                    }
                    break;

                case Request::Kind::Minimize:
                    if (g_window != nullptr) {
                        SDL_MinimizeWindow(g_window);
                    }
                    break;

                case Request::Kind::KeepRom: {
                    // Copy, never move. The original is the user's file and stays exactly where
                    // it is; what changes is only which of the two this program reads next time.
                    if (g_rom_path.empty()) {
                        break;
                    }
                    const std::filesystem::path into = oot::places::roms();
                    std::error_code ec;
                    std::filesystem::create_directories(into, ec);
                    const std::filesystem::path kept = into / g_rom_path.filename();
                    std::filesystem::copy_file(g_rom_path, kept,
                                               std::filesystem::copy_options::overwrite_existing, ec);
                    if (ec) {
                        std::fprintf(stderr, "[launch] the copy could not be made: %s\n",
                                     ec.message().c_str());
                        break;
                    }
                    g_rom_path = kept;
                    oot::rom::remember(recomp::get_config_path(), kept);
                    std::fprintf(stderr, "[launch] a copy is kept here now: %s\n",
                                 kept.string().c_str());
                    break;
                }

                case Request::Kind::Rows:
                    // Only while the launcher owns the window's shape. Once the game has it, the
                    // window is the game's size and a document must never take it back.
                    if (g_window != nullptr && g_launcher_shape && r.rows > 0) {
                        const int height = fit_to_display(LAUNCHER_CHROME_H + r.rows * LAUNCHER_ROW_H);
                        SDL_SetWindowSize(g_window, LAUNCHER_WIDTH, height);
                        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
                    }
                    break;

                case Request::Kind::Maximize:
                    // Toggles: the case fills the window whatever its size.
                    if (g_window != nullptr) {
                        if ((SDL_GetWindowFlags(g_window) & SDL_WINDOW_MAXIMIZED) != 0) {
                            SDL_RestoreWindow(g_window);
                        }
                        else {
                            SDL_MaximizeWindow(g_window);
                        }
                    }
                    break;
            }
        }
        return started_now;
    }

    bool game_started() {
        return g_started.load();
    }

    void mark_started() {
        g_started.store(true);
    }

    void game_window_size(bool fullscreen, int& width, int& height) {
        width = GAME_WIDTH;
        height = GAME_HEIGHT;
        if (fullscreen && g_desktop_height.load() > 0) {
            width = g_desktop_width.load();
            height = g_desktop_height.load();
        }
    }

    bool is_rom_file(const std::filesystem::path& path) {
        const std::string ext = lower(path.extension().string());
        return ext == ".z64" || ext == ".n64" || ext == ".v64";
    }

    bool covers_display() {
        return window_covers_display(g_window);
    }

    bool is_program_file(const std::filesystem::path& path) {
        const std::string ext = lower(path.extension().string());
        return ext == ".exe" || ext == ".dll" || ext == ".pdb" || ext == ".lib" || ext == ".zip" ||
               ext == ".log" || ext == ".txt" || ext == ".json" || ext == ".bak" || ext == ".md";
    }

    std::vector<Entry> drives() {
        std::vector<Entry> out;
        const DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if ((mask & (1u << i)) == 0) {
                continue;
            }
            const std::string root = std::string(1, static_cast<char>('A' + i)) + ":\\";
            out.push_back(Entry{ root, std::filesystem::path(root), true });
        }
        return out;
    }

    std::vector<Entry> list(const std::filesystem::path& directory) {
        std::vector<Entry> folders;
        std::vector<Entry> roms;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
            const std::string name = entry.path().filename().string();
            if (entry.is_directory(ec)) {
                folders.push_back(Entry{ name, entry.path(), true });
            }
            else if (entry.is_regular_file(ec) && is_rom_file(entry.path())) {
                roms.push_back(Entry{ name, entry.path(), false });
            }
        }
        const auto by_name = [](const Entry& a, const Entry& b) { return lower(a.name) < lower(b.name); };
        std::sort(folders.begin(), folders.end(), by_name);
        std::sort(roms.begin(), roms.end(), by_name);

        std::vector<Entry> out;
        // Up one level; at a drive's root, up to the drives.
        const std::filesystem::path parent = directory.parent_path();
        if (parent.empty() || parent == directory) {
            out.push_back(Entry{ "..", std::filesystem::path(), true });
        }
        else {
            out.push_back(Entry{ "..", parent, true });
        }
        out.insert(out.end(), folders.begin(), folders.end());
        out.insert(out.end(), roms.begin(), roms.end());
        return out;
    }

} // namespace oot::launch
