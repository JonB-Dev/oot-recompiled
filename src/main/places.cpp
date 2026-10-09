#include "main/places.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shlobj.h>

#include <cstdio>
#include <fstream>

namespace oot::places {

    namespace {

        // The folder under the user's own application data. A display name rather than a slug,
        // because a person opening their AppData folder is reading it, and the protected name is
        // not in it (CLAUDE.md, "A SHAPE is not a NAME").
        constexpr const wchar_t* DATA_FOLDER = L"OoT Recompiled";

        // The program's own files, kept apart from the user's so an update can replace all of one
        // and none of the other (the user, 2026-09-20: the DLLs "under like a bin folder" for
        // "the actual game files that are needed versus sitting next to the mods and saves").
        constexpr const wchar_t* BIN_FOLDER = L"bin";

        // Written into bin\ by the setup stub, holding the version it installed. Its PRESENCE is
        // what makes this an installed run rather than a portable one.
        constexpr const wchar_t* MARKER = L".installed";

        Mode g_mode = Mode::Portable;
        std::filesystem::path g_program;
        std::filesystem::path g_data;
        std::string g_version;

        std::filesystem::path executable_directory() {
            wchar_t buffer[MAX_PATH * 4] = {};
            const DWORD length = GetModuleFileNameW(nullptr, buffer,
                                                    static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
            if (length == 0) {
                return std::filesystem::current_path();
            }
            return std::filesystem::path(std::wstring(buffer, length)).parent_path();
        }

        std::string read_line(const std::filesystem::path& file) {
            std::ifstream in(file, std::ios::binary);
            if (!in) {
                return {};
            }
            std::string text;
            std::getline(in, text);
            while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
                text.pop_back();
            }
            return text;
        }

    } // namespace

    // %LOCALAPPDATA%, from the shell rather than from the environment. CLAUDE.md rules out reading
    // configuration from environment variables, and the shell answer is also the correct one on a
    // machine whose profile has been redirected.
    std::filesystem::path local_app_data() {
        PWSTR raw = nullptr;
        if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw) != S_OK) {
            CoTaskMemFree(raw);
            return {};
        }
        std::filesystem::path result(raw);
        CoTaskMemFree(raw);
        return result;
    }

    void init() {
        const std::filesystem::path here = executable_directory();

        std::error_code ec;
        if (std::filesystem::is_regular_file(here / MARKER, ec)) {
            g_mode = Mode::Installed;
            g_program = here;
            g_data = here.parent_path();
            g_version = read_line(here / MARKER);
            return;
        }

        g_mode = Mode::Portable;
        g_program = here;
        g_data = here;
        g_version.clear();
    }

    std::filesystem::path default_install_root() {
        const std::filesystem::path root = local_app_data();
        if (root.empty()) {
            return {};
        }
        return root / DATA_FOLDER;
    }

    bool adopt_installed() {
        return adopt_install_root(default_install_root());
    }

    bool adopt_install_root(const std::filesystem::path& data) {
        if (data.empty() || !data.is_absolute()) {
            return false;
        }
        g_mode = Mode::Installed;
        g_data = data;
        g_program = g_data / BIN_FOLDER;

        std::error_code ec;
        std::filesystem::create_directories(g_program, ec);
        if (ec) {
            return false;
        }
        std::filesystem::create_directories(g_data / "saves", ec);
        std::filesystem::create_directories(g_data / "mods", ec);
        return true;
    }

    std::filesystem::path installed_program() {
        return g_data / BIN_FOLDER;
    }

    bool mark_installed(const std::string& version) {
        std::ofstream out(g_program / MARKER, std::ios::binary | std::ios::trunc);
        out << version << "\n";
        if (!out) {
            return false;
        }
        g_version = version;
        return true;
    }

    Mode mode() {
        return g_mode;
    }

    const char* mode_word() {
        return g_mode == Mode::Portable ? "portable" : "installed";
    }

    const std::filesystem::path& program() {
        return g_program;
    }

    const std::filesystem::path& data() {
        return g_data;
    }

    std::filesystem::path assets() {
        return g_program / "assets";
    }

    std::filesystem::path settings_file() {
        return g_data / "settings.txt";
    }

    std::filesystem::path controls_file() {
        return g_data / "controls.txt";
    }

    std::filesystem::path saves() {
        return g_data / "saves";
    }

    std::filesystem::path moments() {
        return g_data / "saves" / "moments";
    }

    std::filesystem::path mods() {
        return g_data / "mods";
    }

    // Where a person puts a texture pack (2026-09-30). Their own material, like the saves and
    // unlike anything in bin\: an update never touches it. The program only ever READS from here
    // and only ever the pack the person selected.
    std::filesystem::path texture_packs() {
        return g_data / "texture-packs";
    }

    std::filesystem::path updates() {
        return g_data / "updates";
    }

    std::filesystem::path roms() {
        return g_data / "rom";
    }

    std::filesystem::path captures() {
        // THE ONE PLACE THIS PROGRAM WRITES THAT IS NEITHER ITS OWN FOLDER NOR THE USER'S DATA
        // FOLDER, and it is deliberate rather than an oversight. A recording is the person's own
        // material: they asked for it, they will open it in whatever they use to look at pictures,
        // and they will delete it when they are done. Burying it under the application's data
        // folder would make all three harder for no gain.
        //
        // It is also the one place that must NEVER be inside the repository, whatever shape the
        // program is running in. A frame of this game is a picture of its assets and is Restricted
        // in exactly the way the ROM is (see .gitignore's captures section and
        // .scaffold/security/pre-app.md); the portable build runs from the build tree, so a
        // captures folder resolved relative to the executable would put hundreds of them one
        // `git add -f` away from being committed. The user's own Pictures folder cannot be.
        //
        // The shell's own answer is asked for rather than a path built from a profile directory,
        // for the same reason local_app_data does it: this is correct on a machine whose Pictures
        // folder has been redirected to another drive or to a network share, and reading an
        // environment variable is forbidden here anyway.
        PWSTR raw = nullptr;
        if (SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &raw) != S_OK) {
            CoTaskMemFree(raw);
            // No Pictures folder is not a reason to lose a recording, and it is not a reason to
            // write into the program's folder either. The data folder is the honest fallback:
            // the user has already been shown where it is on the files surface.
            return g_data / "captures";
        }
        std::filesystem::path result(raw);
        CoTaskMemFree(raw);
        return result / "OoT-Recomp";
    }

    std::filesystem::path videos() {
        // THE VIDEO RECORDER'S FILES (2026-10-09), for the reasons captures gives, in the folder a
        // video belongs in: the person's own Videos folder, as the shell answers it.
        PWSTR raw = nullptr;
        if (SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &raw) != S_OK) {
            CoTaskMemFree(raw);
            return captures();
        }
        std::filesystem::path result(raw);
        CoTaskMemFree(raw);
        return result / "OoT-Recomp";
    }

    std::string installed_version() {
        return g_version;
    }

} // namespace oot::places
