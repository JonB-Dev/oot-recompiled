// Making the installed copy reachable, and making it removable.
//
// Two things Windows needs to be told about, both entirely inside the user's own profile so
// nothing here ever asks for administrator rights (CLAUDE.md: "Requiring administrator rights for
// anything" is on the list of things to avoid, and the machine wide hive is where that starts):
//
//   THE START MENU ENTRY   an ordinary .lnk file in the user's own Start menu folder, so the
//                          launcher opens the way every other program does (the user, 2026-09-20:
//                          "I would love to be able to open the launcher from the start menu").
//
//   THE DESKTOP SHORTCUT   the same .lnk on the desktop, when the person asked the setup file
//                          for one (2026-09-24: the installer's two check boxes).
//
//   THE UNINSTALL ENTRY    a key under HKEY_CURRENT_USER's CurrentVersion\Uninstall, which is what
//                          puts a program in Windows' own installed apps list. Without it the only
//                          way to remove this is to know where it put itself.
//
// Both shortcuts follow the person's choices at install, kept in a small file beside their other
// files (install.txt) so the game's own refresh on every run honors them: a shortcut they turned
// off never comes back, and one they turned on comes back if it was deleted. Neither is written in
// portable mode: a build tree and a share folder install nothing, so they register nothing.
//
// WHAT AN UNINSTALL REMOVES, and this is the part worth being careful about: the data folder, the
// unpacked bin\, the registry entry and the shortcuts. **Never the user's ROM**, which is their own
// file in their own place, and never anything outside the folders this program made.

#pragma once

#include <filesystem>
#include <string>

namespace oot::install {

    // The person's choices from the setup window. The defaults are what an install before the
    // window existed had: a Start menu entry and no desktop shortcut.
    struct Options {
        bool start_menu = true;
        bool desktop = false;
    };

    // Read the choices kept beside the user's files, or the defaults when nothing was kept.
    Options options();

    // Keep the choices. Written by the setup file before it registers; a no-op in portable mode.
    bool set_options(const Options& options);

    // Write (or refresh) the shortcuts the options ask for, remove the ones they do not, and write
    // the uninstall entry, pointing at the given executable. A no-op in portable mode. Failure is
    // reported and never fatal: a program that refuses to start because it could not write a
    // shortcut is worse than one without a shortcut.
    //
    // The executable is passed in rather than read from this process, because the two callers are
    // different programs: the setup stub registers the GAME it just installed, and the game
    // refreshes the same entry on every run so a deleted shortcut comes back and the version never
    // goes stale. Both must produce the same entry, so both name the same file.
    void register_for(const std::filesystem::path& game);

    // Remove the shortcuts and the uninstall entry. Does NOT touch any file the user owns; see
    // remove_everything for that.
    void unregister_self();

    // The full removal, which is what the uninstall entry and the launcher's own button both run.
    // Deletes the data folder and everything under it EXCEPT any ROM found there, which is moved
    // to the user's Downloads folder first and reported, because a ROM is the one file in this
    // project that must never be destroyed by this program.
    // Returns a line describing what happened, for the surface that asked.
    std::string remove_everything();

    // Where the shortcuts are, whether or not they are there. For the surface that tells a person
    // where their files live.
    std::filesystem::path shortcut_path();
    std::filesystem::path desktop_shortcut_path();

} // namespace oot::install
