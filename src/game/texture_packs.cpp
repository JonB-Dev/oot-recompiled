#include "game/texture_packs.h"

#include "main/places.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

namespace oot::packs {

namespace {

    std::vector<Pack> g_packs;
    std::string g_off = "Off";

    // A pack in RT64's own format is a folder holding `rt64.json`, its texture database. That is
    // the whole test, and it is the renderer's test rather than one of ours: `ReplacementDirectory`
    // is handed the folder and reads that file.
    bool holds_database(const std::filesystem::path& dir) {
        std::error_code ec;
        return std::filesystem::is_regular_file(dir / "rt64.json", ec);
    }

    // The name a pack calls itself, out of `mod.json`'s display_name. Read with a deliberately
    // small parser rather than a JSON library: this is a single string from a file a stranger
    // wrote, the program has no JSON dependency, and a malformed file must produce no name rather
    // than an exception. Anything unexpected returns empty and the caller falls back to the
    // folder's own name, which always exists.
    std::string declared_name(const std::filesystem::path& dir) {
        std::error_code ec;
        const std::filesystem::path file = dir / "mod.json";
        if (!std::filesystem::is_regular_file(file, ec)) {
            return {};
        }
        // A mod.json is a few hundred bytes. A cap rather than a trust: a huge file here is not a
        // mod.json and is not worth reading into memory to find out.
        const std::uintmax_t size = std::filesystem::file_size(file, ec);
        if (ec || (size > 64 * 1024)) {
            return {};
        }
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            return {};
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string key = "\"display_name\"";
        const std::size_t at = text.find(key);
        if (at == std::string::npos) {
            return {};
        }
        std::size_t i = text.find(':', at + key.size());
        if (i == std::string::npos) {
            return {};
        }
        i = text.find('"', i);
        if (i == std::string::npos) {
            return {};
        }
        const std::size_t start = i + 1;
        const std::size_t end = text.find('"', start);
        if (end == std::string::npos) {
            return {};
        }
        std::string name = text.substr(start, end - start);
        // One line, printable, and short enough for a row. A name is going on screen, so it is
        // treated as hostile input like every other file this program reads.
        std::string clean;
        for (const char c : name) {
            if ((static_cast<unsigned char>(c) >= 0x20) && (c != '\n') && (c != '\r')) {
                clean.push_back(c);
            }
            if (clean.size() >= 48) {
                break;
            }
        }
        return clean;
    }

    bool is_zip(const std::filesystem::path& file) {
        std::string ext = file.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        // `.rtz` is the reference project's name for the same thing: a zipped pack.
        return (ext == ".zip") || (ext == ".rtz");
    }

} // namespace

    void ensure_folder() {
        std::error_code ec;
        std::filesystem::create_directories(oot::places::texture_packs(), ec);

        // A NOTE IN THE FOLDER (the user, 2026-10-07: "Since we aren't actually bundling anything
        // into the app, I would rather just ... link directly to it and leave a note in there
        // saying if you would like to install higher quality texture pack ... go visit ... his Git
        // page to actually download it"). The program ships no pack and fetches none; the note
        // only says where the author publishes one, with the credit theirs. Written when it is not
        // there, never over a copy the person has edited. A note, not a pack: the scan looks for
        // folders holding rt64.json and for archives, so a text file is never mistaken for one.
        const std::filesystem::path note = oot::places::texture_packs() / "README.txt";
        if (std::filesystem::exists(note, ec)) {
            return;
        }
        std::ofstream out(note, std::ios::binary);
        if (!out) {
            return;
        }
        out << "Texture packs\r\n"
               "\r\n"
               "Put a texture pack in this folder, then choose it on the Texture pack row of the\r\n"
               "settings, on the launcher or in the menu during play. A pack can be a .rtz file, a\r\n"
               ".zip, or a folder holding rt64.json.\r\n"
               "\r\n"
               "Would you like a higher quality texture pack? OoT Reloaded by GhostlyDark is made for\r\n"
               "this renderer. Download its rt64 version from the author's GitHub releases page:\r\n"
               "\r\n"
               "    https://github.com/GhostlyDark/OoT-Reloaded/releases\r\n"
               "\r\n"
               "The rt64 version comes in parts (.zip.001, .zip.002, ...). Open the first part with\r\n"
               "7-Zip and put the .rtz file inside it here.\r\n"
               "\r\n"
               "All credit for that pack belongs to GhostlyDark and the artists named in its readme.\r\n"
               "This program includes no texture pack and downloads none.\r\n";
    }

    void rescan() {
        g_packs.clear();
        ensure_folder();

        const std::filesystem::path root = oot::places::texture_packs();
        std::error_code ec;
        std::filesystem::directory_iterator it(root, ec);
        if (ec) {
            return;
        }
        for (const std::filesystem::directory_entry& entry : it) {
            const std::filesystem::path p = entry.path();
            if (entry.is_directory(ec)) {
                // THREE SHAPES, because a person unzips a pack however their tool unzips it and
                // none of them is wrong. The folder itself may hold the database; or it may hold
                // ONE folder that holds it, which is what his own pack does (the zip contains
                // `THE LEGEND OF ZELDA\rt64.json` under the version named folder); or it is not a
                // pack at all and is passed over in silence rather than reported as an error,
                // because a person's folder is theirs and may hold anything.
                if (holds_database(p)) {
                    std::string name = declared_name(p);
                    g_packs.push_back({ name.empty() ? p.filename().string() : name, p });
                    continue;
                }
                std::filesystem::directory_iterator inner(p, ec);
                if (ec) {
                    continue;
                }
                for (const std::filesystem::directory_entry& child : inner) {
                    if (child.is_directory(ec) && holds_database(child.path())) {
                        std::string name = declared_name(child.path());
                        // The OUTER folder's name is the one the person recognizes, since it is
                        // what they dropped in; the inner one is usually the game's own title.
                        // The pack's declared name still wins when it has one.
                        g_packs.push_back({ name.empty() ? p.filename().string() : name, child.path() });
                        break;
                    }
                }
                continue;
            }
            if (entry.is_regular_file(ec) && is_zip(p)) {
                // A zip is handed over whole: RT64's ReplacementDirectory reads one directly, so
                // nothing is unpacked onto the person's disk.
                g_packs.push_back({ p.stem().string(), p });
            }
        }

        std::sort(g_packs.begin(), g_packs.end(), [](const Pack& a, const Pack& b) {
            return a.name < b.name;
        });
    }

    const std::vector<Pack>& list() {
        return g_packs;
    }

    int option_count() {
        return static_cast<int>(g_packs.size()) + 1;
    }

    std::filesystem::path path_for(int value) {
        if ((value <= 0) || (value > static_cast<int>(g_packs.size()))) {
            return {};
        }
        return g_packs[static_cast<std::size_t>(value - 1)].path;
    }

    const char* label_for(int value) {
        if ((value <= 0) || (value > static_cast<int>(g_packs.size()))) {
            // Off, and also what a settings file naming a pack that has since been deleted reads
            // as. Falling back to Off rather than to a stale name means the row can never claim a
            // pack is on when the folder no longer holds it.
            return g_off.c_str();
        }
        return g_packs[static_cast<std::size_t>(value - 1)].name.c_str();
    }

} // namespace oot::packs
