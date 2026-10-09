// Where every file this program uses lives, and the ONE place that decides it.
//
// The program runs in one of two shapes:
//
//   PORTABLE    Nothing was installed. Program files and user files share the executable's own
//               directory, which is how the build tree, the harness and the portable download all
//               work. Nothing is written outside that folder. This is a first class way to run
//               the program and is shipped as its own download, not a fallback.
//
//   INSTALLED   The setup file put the program in the user's own application data: the executable,
//               the DLLs and assets\ in `bin\`, and the saves, mods, settings, controls and the
//               remembered ROM path BESIDE that bin\ rather than inside it. So an update replaces
//               all of one and none of the other.
//
// THE TEST IS A MARKER FILE, `.installed`, written into bin\ by the setup stub and holding the
// version it installed. It is not "are there assets beside me", which was the first attempt and is
// wrong for a reason worth writing down: bin\ contains assets too, so the installed copy would
// have called itself portable and put the user's saves inside the folder that updates replace.
//
// The setup stub is a SEPARATE, TINY executable (src/setup/) and that is not an accident either.
// Making the game itself unpack its own DLLs cannot work: Windows resolves the import table before
// main() runs, and even with the imports delay loaded a static initializer reaches SDL before the
// first line of main, so the process dies with ERROR_MOD_NOT_FOUND having printed nothing at all.
// A stub that imports nothing but the operating system has no such problem.

#pragma once

#include <filesystem>
#include <string>

namespace oot::places {

    enum class Mode {
        // Program and user files together, beside the executable. The build tree, and the
        // portable download.
        Portable,
        // Under the user's application data, program files in bin\ and user files beside it.
        Installed,
    };

    // Decides the shape by looking for the marker beside the running executable. Call before
    // anything reads a file. Never fails: with no marker it is portable, which always works.
    void init();

    // For the setup stub, which is not the game and cannot ask "where am I": take the installed
    // layout directly, creating the folders. Returns false only when Windows will not say where
    // the user's application data is.
    bool adopt_installed();

    // The same, into a folder the person chose in the setup window (2026-09-24): that folder is
    // the data folder and bin\ goes under it. False for an empty or relative path. The game finds
    // its way afterward by the marker beside its executable, so nothing points at the folder.
    bool adopt_install_root(const std::filesystem::path& data);

    // Where adopt_installed would put it: the setup window's default, shown before anything is
    // written. Empty when Windows will not say where the application data is.
    std::filesystem::path default_install_root();

    // Write the marker that makes `init()` choose Installed next time. The stub calls this once
    // the payload is on disk, never before: a marker without the files behind it would send the
    // program looking for assets that are not there.
    bool mark_installed(const std::string& version);

    Mode mode();

    // "portable" or "installed", for the surface that tells a person where their files are.
    const char* mode_word();

    // The program's own files: the executable, the DLLs and assets\. Replaced wholesale by an
    // update, so nothing the user owns may ever be put here.
    const std::filesystem::path& program();

    // The user's files: saves, mods, the mod config, settings, controls, the remembered ROM path,
    // the log, and the ROM itself if they accepted the offer to keep a copy here. Never replaced
    // by an update, never removed by anything but an uninstall the person asked for.
    const std::filesystem::path& data();

    // Where the setup puts the program: the data folder's own bin\. A copy run from elsewhere in
    // the data folder (a test copy in bin-test\ beside it) shares the user's files but is not the
    // installed program, and must not claim its shortcuts (install::register_for).
    std::filesystem::path installed_program();

    std::filesystem::path assets();
    std::filesystem::path settings_file();
    std::filesystem::path controls_file();
    std::filesystem::path saves();
    // The saved moments (phase 75): one file per slot and its thumbnail, under saves\.
    std::filesystem::path moments();
    std::filesystem::path mods();
    // Where a downloaded, verified setup file waits for the next start (the user's ask of
    // 2026-09-23). Under the DATA folder rather than %TEMP%, because it has to survive until the
    // program next opens, and beside nothing the user owns, because an update consumes it.
    std::filesystem::path updates();

    // Where a person puts a texture pack (2026-09-30). This program only ever READS it: it scans
    // the folder for packs and hands the renderer the one that was selected. Their own material,
    // so it sits beside the saves rather than in bin\, and an update never touches it.
    std::filesystem::path texture_packs();

    // Where a copied ROM goes when the user accepts the first run offer. Their own file, their own
    // machine, their own choice: this is not the silent 32 MB duplicate an earlier build made.
    std::filesystem::path roms();

    // Where the frame recorder writes: the user's own Pictures folder, under `OoT-Recomp`, then a
    // folder per day. Outside everything else this program owns, because a recording is the
    // person's own material and because a frame of this game is Restricted in the way the ROM is
    // and must never be able to land inside the repository. See the comment on the definition.
    std::filesystem::path captures();

    // Where the video recorder writes its MP4s (main/video.h): the user's own Videos folder, under
    // `OoT-Recomp`; the captures folder if there is no Videos folder.
    std::filesystem::path videos();

    // The version recorded in the marker, or empty when running portable.
    std::string installed_version();

} // namespace oot::places
