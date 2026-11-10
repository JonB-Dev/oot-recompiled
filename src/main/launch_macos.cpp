#include "main/launch.h"

#include "game/game.h"
#include "game/moments.h"
#include "main/places.h"
#include "main/rom_source.h"

#include <SDL.h>
#include <librecomp/game.hpp>
#include <ultramodern/ultramodern.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <system_error>

namespace oot::launch {
std::filesystem::path choose_rom_with_native_picker();
namespace {
    enum class RequestKind { Select, Play, Quit, Pick, Minimize, Maximize, Show };
    struct Request { RequestKind kind; std::filesystem::path path; };
    std::mutex mutex;
    std::deque<Request> requests;
    RomStatus status;
    std::filesystem::path selected_rom;
    std::vector<uint8_t> rom_bytes;
    std::u8string game_id;
    SDL_Window* window = nullptr;
    std::atomic<bool> started{false};
    std::atomic<bool> shown{false};

    std::string lower(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    const char* error_name(recomp::RomValidationError error) {
        switch (error) {
            case recomp::RomValidationError::FailedToOpen: return "the file could not be opened";
            case recomp::RomValidationError::NotARom: return "not an N64 ROM";
            case recomp::RomValidationError::IncorrectRom: return "a ROM, but not this game";
            case recomp::RomValidationError::IncorrectVersion: return "this game, but not NTSC-U 1.0";
            default: return "ROM validation failed";
        }
    }

    bool accept(const std::filesystem::path& path, RomState state, bool remember) {
        std::vector<uint8_t> bytes;
        const auto error = oot::rom::validate(path, oot::game_entry(), bytes);
        std::lock_guard lock(mutex);
        if (error != recomp::RomValidationError::Good) {
            status = {RomState::Refused, path.filename().string(), error_name(error)};
            return false;
        }
        selected_rom = path;
        rom_bytes = std::move(bytes);
        status = {state, path.filename().string(), {}};
        if (remember) oot::rom::remember(oot::places::data(), path);
        return true;
    }

    void post(RequestKind kind, std::filesystem::path path = {}) {
        std::lock_guard lock(mutex);
        requests.push_back({kind, std::move(path)});
    }
}

const std::filesystem::path& executable_directory() {
    return oot::places::program();
}
const std::filesystem::path& rom_path() { return selected_rom; }

void init(const std::u8string& id, const std::filesystem::path& cli_rom) {
    game_id = id;
    recomp::register_config_path(oot::places::data());
    if (!cli_rom.empty()) {
        accept(cli_rom, RomState::Accepted, true);
        return;
    }
    const auto previous = oot::rom::remembered(oot::places::data());
    if (!previous.empty() && accept(previous, RomState::Stored, false)) return;
    if (!previous.empty()) oot::rom::forget(oot::places::data());
    // A development bundle may be started from Finder; its working directory is unreliable.
    // Search only the bundle's program directory. The ROM is never copied into the bundle.
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(executable_directory(), ec)) {
        if (item.is_regular_file(ec) && is_rom_file(item.path()) &&
            accept(item.path(), RomState::Beside, true)) return;
    }
    std::lock_guard lock(mutex);
    status = {};
}

RomStatus rom_status() { std::lock_guard lock(mutex); return status; }
bool rom_ready() {
    const auto state = rom_status().state;
    return state == RomState::Accepted || state == RomState::Stored || state == RomState::Beside;
}
bool load_rom_into_runtime() {
    if (!rom_ready() || rom_bytes.empty()) return false;
    recomp::set_rom_contents(std::move(rom_bytes));
    rom_bytes.clear();
    return true;
}

void request_select_rom(const std::filesystem::path& path) { post(RequestKind::Select, path); }
void request_pick_rom() { post(RequestKind::Pick); }
void request_play() { post(RequestKind::Play); }
void request_quit() { post(RequestKind::Quit); }
void request_minimize() { post(RequestKind::Minimize); }
void request_maximize() { post(RequestKind::Maximize); }
void request_show_window() { post(RequestKind::Show); }
void request_maximize_launcher() { post(RequestKind::Maximize); }
void request_keep_rom_copy() {
    // The local macOS build keeps the original ROM at its chosen path.
}

void attach_window(SDL_Window* value) { window = value; }
void shape_for_launcher() {
    if (!window) return;
    SDL_SetWindowSize(window, 960, 620);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}
void shape_for_game() {
    if (!window) return;
    SDL_SetWindowSize(window, 960, 720);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}
void hold_window_hidden() { shown.store(false); }
bool window_shown() { return shown.load(); }
float launcher_scale_now() { return 1.0f; }
int launcher_chrome_height() { return 178; }
void request_window_rows(int) {}
void display_size(int& width, int& height) {
    SDL_DisplayMode mode{};
    const int display = window ? std::max(0, SDL_GetWindowDisplayIndex(window)) : 0;
    if (SDL_GetDesktopDisplayMode(display, &mode) == 0) {
        width = mode.w;
        height = mode.h;
    }
    else { width = 1920; height = 1080; }
}
void game_window_size(bool fullscreen, int& width, int& height) {
    if (fullscreen) display_size(width, height);
    else { width = 960; height = 720; }
}
bool game_started() { return started.load(); }
void mark_started() { started.store(true); }
bool covers_display() {
    if (!window) return false;
    int w = 0, h = 0, dw = 0, dh = 0;
    SDL_GetWindowSize(window, &w, &h);
    display_size(dw, dh);
    return w >= dw && h >= dh;
}

bool tick() {
    std::deque<Request> pending;
    { std::lock_guard lock(mutex); pending.swap(requests); }
    bool started_now = false;
    for (const auto& request : pending) {
        switch (request.kind) {
            case RequestKind::Select:
                accept(request.path, RomState::Accepted, true);
                break;
            case RequestKind::Play:
                if (!started && load_rom_into_runtime()) {
                    recomp::start_game(game_id, "");
                    started.store(true);
                    started_now = true;
                }
                break;
            case RequestKind::Quit:
                if (!oot::moments::on_quit_requested(game_started())) ultramodern::quit();
                break;
            case RequestKind::Pick:
                if (const auto chosen = choose_rom_with_native_picker(); !chosen.empty()) {
                    accept(chosen, RomState::Accepted, true);
                }
                break;
            case RequestKind::Minimize:
                if (window) SDL_MinimizeWindow(window);
                break;
            case RequestKind::Maximize:
                if (window) SDL_MaximizeWindow(window);
                break;
            case RequestKind::Show:
                if (window) SDL_ShowWindow(window);
                shown.store(true);
                break;
        }
    }
    return started_now;
}

bool is_rom_file(const std::filesystem::path& path) {
    const auto extension = lower(path.extension().string());
    return extension == ".z64" || extension == ".n64" || extension == ".v64";
}
bool is_program_file(const std::filesystem::path& path) {
    const auto extension = lower(path.extension().string());
    return extension == ".app" || extension == ".dylib" || extension == ".zip" ||
           extension == ".log" || extension == ".txt" || extension == ".json";
}
std::vector<Entry> drives() {
    return {{"Home", std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "/"), true},
            {"Root", "/", true}};
}
std::vector<Entry> list(const std::filesystem::path& directory) {
    std::vector<Entry> entries;
    if (directory.empty()) return drives();
    const auto parent = directory.parent_path();
    entries.push_back({"..", parent == directory ? std::filesystem::path() : parent, true});
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(directory, ec)) {
        if (item.is_directory(ec) || (item.is_regular_file(ec) && is_rom_file(item.path()))) {
            entries.push_back({item.path().filename().string(), item.path(), item.is_directory(ec)});
        }
    }
    std::sort(entries.begin() + 1, entries.end(), [](const Entry& a, const Entry& b) {
        if (a.directory != b.directory) return a.directory;
        return lower(a.name) < lower(b.name);
    });
    return entries;
}
} // namespace oot::launch
