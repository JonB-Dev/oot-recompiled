#include "main/places.h"

#include <mach-o/dyld.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <fstream>
#include <system_error>
#include <vector>

namespace oot::places {

namespace {
    std::filesystem::path g_program;
    std::filesystem::path g_data;

    std::filesystem::path home_directory() {
        const passwd* user = getpwuid(getuid());
        return user && user->pw_dir ? std::filesystem::path(user->pw_dir) : std::filesystem::path{};
    }

    std::filesystem::path executable_directory() {
        uint32_t length = 0;
        _NSGetExecutablePath(nullptr, &length);
        if (length == 0) return {};
        std::vector<char> buffer(length);
        if (_NSGetExecutablePath(buffer.data(), &length) != 0) return {};
        std::error_code ec;
        const auto canonical = std::filesystem::weakly_canonical(buffer.data(), ec);
        return (ec ? std::filesystem::path(buffer.data()) : canonical).parent_path();
    }

    bool create_user_folders(const std::filesystem::path& data) {
        std::error_code ec;
        std::filesystem::create_directories(data / "saves", ec);
        if (ec) return false;
        std::filesystem::create_directories(data / "mods", ec);
        if (ec) return false;
        std::filesystem::create_directories(data / "texture-packs", ec);
        return !ec;
    }
}

std::filesystem::path default_install_root() {
    const auto home = home_directory();
    return home.empty() ? std::filesystem::path{} : home / "Library/Application Support/OoT Recompiled";
}

void init() {
    g_program = executable_directory();
    g_data = default_install_root();
    if (!g_data.empty()) create_user_folders(g_data);
}

bool adopt_install_root(const std::filesystem::path& data) {
    if (data.empty() || !data.is_absolute() || !create_user_folders(data)) return false;
    g_data = data;
    return true;
}

bool adopt_installed() { return adopt_install_root(default_install_root()); }
bool mark_installed(const std::string&) { return false; }
Mode mode() { return Mode::Installed; }
const char* mode_word() { return "installed"; }
const std::filesystem::path& program() { return g_program; }
const std::filesystem::path& data() { return g_data; }
std::filesystem::path installed_program() { return g_program; }

std::filesystem::path assets() {
    if (g_program.filename() == "MacOS" && g_program.parent_path().filename() == "Contents") {
        return g_program.parent_path() / "Resources/assets";
    }
    return g_program / "assets";
}

std::filesystem::path settings_file() { return g_data / "settings.txt"; }
std::filesystem::path controls_file() { return g_data / "controls.txt"; }
std::filesystem::path saves() { return g_data / "saves"; }
std::filesystem::path moments() { return saves() / "moments"; }
std::filesystem::path mods() { return g_data / "mods"; }
std::filesystem::path texture_packs() { return g_data / "texture-packs"; }
std::filesystem::path updates() { return g_data / "updates"; }
std::filesystem::path roms() { return g_data / "rom"; }
std::filesystem::path captures() { return g_data / "captures"; }
std::filesystem::path videos() { return g_data / "videos"; }
std::string installed_version() { return {}; }

} // namespace oot::places
