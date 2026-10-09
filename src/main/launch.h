#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct SDL_Window;

// The launcher's side of the lifecycle: where the program lives, the ROM's state, and the
// requests the documents post to the main thread, where the runtime is driven. Nothing here
// touches RmlUi; the shell reads the state and posts the requests, and the pump ticks this.
namespace oot::launch {

    // The directory the executable is in, which is where the settings, the bindings, the assets
    // and the saves live: a double click from anywhere, a shortcut, and the harness all find the
    // same files. The user's ROM is NOT among them; it is read where they put it.
    const std::filesystem::path& executable_directory();

    enum class RomState { None, Stored, Beside, Accepted, Refused };

    struct RomStatus {
        RomState state = RomState::None;
        std::string name;     // the file's name, when there is one
        std::string detail;   // why a file was refused
    };

    // Once, after the flags: registers the runtime's config directory beside the executable,
    // then finds the game in the first of these that works, validating it where it lies and
    // never copying it: the file chosen last time (its path is remembered, not its bytes), a ROM
    // given on the command line, or any file beside the executable that validates as the game.
    void init(const std::u8string& game_id, const std::filesystem::path& cli_rom);

    RomStatus rom_status();
    bool rom_ready();

    // Where the accepted ROM actually is, for the surface that tells a person where their files
    // live. Empty when none has been accepted. It is the user's own file in the user's own place:
    // this program remembers the path and never the bytes.
    const std::filesystem::path& rom_path();

    // Hand the accepted ROM's contents to the runtime, ready for start_game. One implementation
    // for both ways in, Play and --play. The bytes come from the user's own file, read where it
    // lies; this program keeps no copy of its own (main/rom_source.h).
    bool load_rom_into_runtime();

    // From any thread; the main thread's tick does the work.
    void request_select_rom(const std::filesystem::path& path);
    // The file picker (2026-09-19: the user's rule allows the platform's picker, not message
    // boxes), opened by the main thread on the next tick; the chosen file goes to the
    // browser's path field and is validated like any other.
    void request_pick_rom();
    void request_play();
    void request_quit();

    // Main thread, once per pump: validates a requested ROM, starts the game on Play, quits.
    // True on the tick that started the game, so the caller can give the window its game
    // shape and release the window mode.
    bool tick();

    // The window (main thread). As the launcher it is a small borderless window sized to the
    // case, centered, draggable by its lid through SDL's hit test, with our own minimize and
    // close in the lid; at Play it gets its border and the game's size back.
    void attach_window(SDL_Window* window);
    void shape_for_launcher();
    void shape_for_game();
    void request_minimize();
    // Size the launcher window to a document's own list length, as that document opens.
    void request_window_rows(int rows);

    // How much of the launcher's window is NOT the row list: the lid, the action bar and the
    // foot. The same number `request_window_rows` sizes the window with.
    //
    // The shell caps the launcher's scrolling list at the window height MINUS this, so the list
    // is given exactly the space the window was sized to give it. A share of the window would
    // be wrong here in a way it is not for the settings modal: this window's height is DERIVED
    // from the row count, so any fixed fraction of it is either short (the list scrolls when it
    // did not need to, which is what 58 percent did) or long (the list runs past the foot).
    int launcher_chrome_height();

    // Copy the accepted ROM into the program's own data folder and read it from there afterward,
    // because the user asked to be OFFERED that (2026-09-20: "if they would like to copy the ROM
    // into their workspace folder, this way the ROM is always available in a central location").
    //
    // THIS IS NOT THE DUPLICATION THAT WAS REMOVED. What was wrong before was the program silently
    // writing a second 32 MB copy nobody asked for and nobody was told about. This one is offered,
    // its size is stated before the question is answered, and it only happens on a yes. The
    // original is never moved, changed or deleted.
    void request_keep_rom_copy();
    void request_maximize();

    // True once the game has been started, by Play or by --play.
    bool game_started();
    void mark_started();

    // The size the GAME's window takes, fullscreen (the desktop's mode on the window's display)
    // or windowed (the size it opens at), for planning what the picture is drawn at before the
    // window has that shape (the launcher's Downsampling row, 2026-10-07). Any thread: the
    // desktop's size is read on the main thread, where SDL's window calls belong, and kept.
    void game_window_size(bool fullscreen, int& width, int& height);

    // The ROM browser's listings: the drives, and a directory's folders and ROM files.
    struct Entry {
        std::string name;
        std::filesystem::path path;
        bool directory = false;
    };
    std::vector<Entry> drives();
    std::vector<Entry> list(const std::filesystem::path& directory);
    bool is_rom_file(const std::filesystem::path& path);
    // The program's own kinds of file (an executable, a DLL, a zip, a log), never tried as a ROM.
    bool is_program_file(const std::filesystem::path& path);

    // True while the attached window covers its display. The renderer takes the window
    // fullscreen by its own means, which SDL's flags do not report, so fullscreen is judged
    // by geometry: the frame's edge and the window's drag and resize bands go by this.
    bool covers_display();

} // namespace oot::launch
