// The user's ROM, read where they put it.
//
// THE BUG THIS EXISTS TO FIX (user, 2026-09-20): "this ROM duplication when you select one, it
// kind of makes a copy of that ROM in the root folder. Let's just leave it wherever the user
// actually selects it. So whether they drop it into the same folder or whether they leave it in a
// different one, it should not make copies. It should just leave their exact selected copy
// retained."
//
// WHERE THE COPY CAME FROM. It is not something this project asked for. The runtime's way of
// remembering a ROM is `recomp::select_rom`, which validates the file and then writes the whole
// thing back out as `<game id>.z64` in the config directory; `load_stored_rom` reads that copy
// back on every later run. So using the runtime's own remember-it-for-next-time leaves a second
// 32 MB copy of the game beside the executable, which is exactly what the user found. The About
// surface has been promising the opposite the whole time ("Your ROM stays yours. It is read from
// where you put it and never copied"), and this module is what makes that sentence true.
//
// WHAT THIS DOES INSTEAD. The same validation, in the same order, with the write left out, and
// the PATH remembered rather than the bytes. The validation is reproduced here rather than called
// because the runtime offers no way to ask it for the verdict without also asking it for the copy.
// It is kept deliberately line for line with `librecomp/src/recomp.cpp` so the two cannot drift:
// read, pad to a multiple of four, correct the byte order, hash. If that file changes, this one is
// what has to be re-read.
//
// NOTHING ABOUT THE SAFETY CHECK CHANGES, which is the part worth being sure of. The hash is
// still verified before a single byte is parsed, against the same constant in the same game entry,
// and a file that fails is refused with the same reason as before. The only thing removed is the
// write.

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "librecomp/game.hpp"

namespace oot::rom {

    // Validate a ROM where it lies and hand back its contents, byte order corrected, ready for
    // `recomp::set_rom_contents`. Writes nothing anywhere. The error values are the runtime's own
    // so every caller's existing reporting keeps working unchanged.
    recomp::RomValidationError validate(const std::filesystem::path& path,
                                        const recomp::GameEntry& entry,
                                        std::vector<uint8_t>& contents);

    // The chosen file's PATH, kept in a one line text file beside the executable, next to
    // settings.txt and controls.txt. A path is a few dozen bytes; the copy it replaces was 32 MB.
    //
    // Remembering the path rather than the bytes does mean a ROM that is later moved, renamed or
    // deleted is not found next time. That is the correct behavior for what the user asked for:
    // their copy is the only copy, so if it moves the program should ask again rather than quietly
    // keep playing from a duplicate they thought they had got rid of.
    std::filesystem::path remembered(const std::filesystem::path& directory);
    void remember(const std::filesystem::path& directory, const std::filesystem::path& rom);
    void forget(const std::filesystem::path& directory);

    // The file the runtime's own store would have written, if an earlier build of this program
    // left one behind. Reported so the launcher can say it is there and no longer used; NEVER
    // deleted here, because a ROM is the one file in this project that must not be destroyed by
    // accident and removing it is the user's decision, not ours.
    std::filesystem::path stale_stored_copy(const std::filesystem::path& directory,
                                            const recomp::GameEntry& entry);

} // namespace oot::rom
