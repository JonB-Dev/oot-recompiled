#include "main/install.h"
#include "main/places.h"

#include "build_info.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <shobjidl.h>

#include <cstdio>
#include <fstream>
#include <system_error>
#include <vector>

namespace oot::install {

    namespace {

        // The name a person reads, in the Start menu and in Windows' installed apps list. The
        // protected name is not in it and must not be: CLAUDE.md's naming rule governs everything
        // we author, and a shortcut is about as authored as it gets.
        constexpr const wchar_t* DISPLAY_NAME = L"OoT Recompiled";

        // Our own key under the CURRENT USER's hive. Never HKEY_LOCAL_MACHINE: that one needs
        // administrator rights, and this program requires none for anything.
        // The small program that finishes the job from outside the folder, installed beside the
        // game. See src/setup/finish.cpp.
        constexpr const wchar_t* FINISHER = L"uninstall.exe";

        constexpr const wchar_t* UNINSTALL_KEY =
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OoTRecompiled";

        std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
            PWSTR raw = nullptr;
            if (SHGetKnownFolderPath(id, 0, nullptr, &raw) != S_OK) {
                CoTaskMemFree(raw);
                return {};
            }
            std::filesystem::path result(raw);
            CoTaskMemFree(raw);
            return result;
        }

        // A .lnk is a COM object, so this is the shell's own IShellLink rather than a file format
        // we write ourselves. CoInitializeEx is called and matched here rather than assumed: this
        // runs before the rest of the program has set anything up.
        bool write_shortcut(const std::filesystem::path& link,
                            const std::filesystem::path& target,
                            const std::filesystem::path& working) {
            const HRESULT started = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            const bool ours = SUCCEEDED(started);
            // RPC_E_CHANGED_MODE means somebody already initialized this thread differently, which
            // is fine: we can still use COM, we just must not uninitialize it.
            if (!ours && started != RPC_E_CHANGED_MODE) {
                return false;
            }

            bool ok = false;
            IShellLinkW* shell = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_IShellLinkW, reinterpret_cast<void**>(&shell)))) {
                shell->SetPath(target.wstring().c_str());
                shell->SetWorkingDirectory(working.wstring().c_str());
                shell->SetDescription(L"The launcher for OoT Recompiled");
                // The icon is the executable's own resource 1, so the shortcut carries the mark
                // without a second copy of it anywhere.
                shell->SetIconLocation(target.wstring().c_str(), 0);

                IPersistFile* file = nullptr;
                if (SUCCEEDED(shell->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
                    ok = SUCCEEDED(file->Save(link.wstring().c_str(), TRUE));
                    file->Release();
                }
                shell->Release();
            }

            if (ours) {
                CoUninitialize();
            }
            return ok;
        }

        void set_string(HKEY key, const wchar_t* name, const std::wstring& value) {
            RegSetValueExW(key, name, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(value.c_str()),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        }

        void set_dword(HKEY key, const wchar_t* name, DWORD value) {
            RegSetValueExW(key, name, 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&value), sizeof(value));
        }

        // The size Windows shows in its list, in kilobytes, measured rather than guessed.
        DWORD installed_kilobytes() {
            std::uintmax_t total = 0;
            std::error_code ec;
            for (const std::filesystem::directory_entry& entry :
                 std::filesystem::recursive_directory_iterator(oot::places::data(), ec)) {
                if (ec) {
                    break;
                }
                if (entry.is_regular_file(ec)) {
                    total += entry.file_size(ec);
                }
            }
            return static_cast<DWORD>(total / 1024);
        }

    } // namespace

    std::filesystem::path shortcut_path() {
        const std::filesystem::path programs = known_folder(FOLDERID_Programs);
        if (programs.empty()) {
            return {};
        }
        return programs / (std::wstring(DISPLAY_NAME) + L".lnk");
    }

    std::filesystem::path desktop_shortcut_path() {
        const std::filesystem::path desktop = known_folder(FOLDERID_Desktop);
        if (desktop.empty()) {
            return {};
        }
        return desktop / (std::wstring(DISPLAY_NAME) + L".lnk");
    }

    namespace {

        // The choices, as key=value lines beside the user's other files. Read as hostile input like
        // the settings file: an unknown line is ignored, a missing file is the defaults.
        std::filesystem::path options_file() {
            return oot::places::data() / "install.txt";
        }

        // One shortcut, brought in line with its choice: written when wanted (and refreshed, so a
        // deleted one comes back and the version never goes stale), removed when not.
        void settle_shortcut(const std::filesystem::path& link, bool wanted, const std::filesystem::path& exe,
                             const char* what) {
            if (link.empty()) {
                return;
            }
            std::error_code ec;
            if (!wanted) {
                if (std::filesystem::exists(link, ec)) {
                    std::filesystem::remove(link, ec);
                    std::printf("[install] %s shortcut removed, as chosen\n", what);
                }
                return;
            }
            std::filesystem::create_directories(link.parent_path(), ec);
            if (write_shortcut(link, exe, oot::places::program())) {
                std::printf("[install] %s: %s\n", what, link.string().c_str());
            } else {
                std::fprintf(stderr, "[install] the %s shortcut could not be written\n", what);
            }
        }

    } // namespace

    Options options() {
        Options result;
        std::ifstream in(options_file(), std::ios::binary);
        std::string line;
        while (in && std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
                line.pop_back();
            }
            if (line == "start_menu=0") {
                result.start_menu = false;
            } else if (line == "start_menu=1") {
                result.start_menu = true;
            } else if (line == "desktop=0") {
                result.desktop = false;
            } else if (line == "desktop=1") {
                result.desktop = true;
            }
        }
        return result;
    }

    bool set_options(const Options& options) {
        if (oot::places::mode() != oot::places::Mode::Installed) {
            return false;
        }
        std::ofstream out(options_file(), std::ios::binary | std::ios::trunc);
        out << "start_menu=" << (options.start_menu ? 1 : 0) << "\n";
        out << "desktop=" << (options.desktop ? 1 : 0) << "\n";
        return static_cast<bool>(out);
    }

    void register_for(const std::filesystem::path& game) {
        if (oot::places::mode() != oot::places::Mode::Installed) {
            return;
        }

        // ONLY THE INSTALLED PROGRAM REGISTERS (the user, 2026-10-09: "im not opening the test on
        // though", having opened their own desktop shortcut and been given the test copy). A test
        // copy in bin-test\ carries the marker so it shares the person's saves and settings, and so
        // it registered itself on every start: after its first run the Start menu entry, the
        // desktop shortcut and the uninstall entry all pointed at it. Only the program in the data
        // folder's own bin\, where the setup puts it, registers; any other copy uses the person's
        // files and leaves their shortcuts alone.
        {
            std::error_code ec;
            if (!std::filesystem::equivalent(game.parent_path(), oot::places::installed_program(), ec)) {
                std::fprintf(stderr, "[install] not the installed program in bin\\; the shortcuts are left as they are\n");
                return;
            }
        }

        const std::filesystem::path exe = game;
        if (!exe.empty()) {
            const Options chosen = options();
            settle_shortcut(shortcut_path(), chosen.start_menu, exe, "start menu");
            settle_shortcut(desktop_shortcut_path(), chosen.desktop, exe, "desktop");
        }

        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, UNINSTALL_KEY, 0, nullptr, 0,
                            KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
            std::fprintf(stderr, "[install] the uninstall entry could not be written\n");
            return;
        }

        // --uninstall is this program removing itself, with its own confirmation surface. Windows
        // runs this string when somebody picks Uninstall in its list.
        const std::wstring command = L"\"" + exe.wstring() + L"\" --uninstall";
        set_string(key, L"DisplayName", DISPLAY_NAME);
        set_string(key, L"DisplayVersion", std::wstring(oot::build_info::version,
                                                        oot::build_info::version + std::char_traits<char>::length(oot::build_info::version)));
        set_string(key, L"Publisher", L"Jonathan Barnes");
        set_string(key, L"InstallLocation", oot::places::data().wstring());
        set_string(key, L"DisplayIcon", exe.wstring());
        set_string(key, L"UninstallString", command);
        set_dword(key, L"EstimatedSize", installed_kilobytes());
        // This program has no repair or modify path, and saying so stops Windows offering buttons
        // that would do nothing.
        set_dword(key, L"NoModify", 1);
        set_dword(key, L"NoRepair", 1);
        RegCloseKey(key);
        std::printf("[install] listed in Windows' installed apps\n");
    }

    void unregister_self() {
        std::error_code ec;
        for (const std::filesystem::path& link : { shortcut_path(), desktop_shortcut_path() }) {
            if (!link.empty()) {
                std::filesystem::remove(link, ec);
            }
        }
        RegDeleteKeyW(HKEY_CURRENT_USER, UNINSTALL_KEY);
    }

    std::string remove_everything() {
        unregister_self();

        const std::filesystem::path data = oot::places::data();
        if (oot::places::mode() != oot::places::Mode::Installed || data.empty()) {
            return "Nothing to remove: this copy runs portable, so it never installed anything.";
        }

        // THE ROM COMES OUT FIRST. If the person accepted the offer to keep a copy here, it is
        // still their file and a ROM is the one thing this program must never destroy. It moves
        // to their Downloads folder and the message says so, rather than being deleted quietly
        // along with everything else.
        std::string rescued;
        std::error_code ec;
        const std::filesystem::path roms = oot::places::roms();
        if (std::filesystem::is_directory(roms, ec)) {
            std::filesystem::path downloads = known_folder(FOLDERID_Downloads);
            if (downloads.empty()) {
                downloads = known_folder(FOLDERID_Profile);
            }
            for (const std::filesystem::directory_entry& entry :
                 std::filesystem::directory_iterator(roms, ec)) {
                if (!entry.is_regular_file(ec)) {
                    continue;
                }
                const std::filesystem::path moved = downloads / entry.path().filename();
                std::filesystem::rename(entry.path(), moved, ec);
                if (ec) {
                    // A different volume refuses a rename. Copy, and only then remove.
                    ec.clear();
                    std::filesystem::copy_file(entry.path(), moved,
                                               std::filesystem::copy_options::overwrite_existing, ec);
                    if (!ec) {
                        std::filesystem::remove(entry.path(), ec);
                    }
                }
                if (!ec) {
                    rescued = moved.string();
                }
            }
        }

        // THE REST HAS TO HAPPEN FROM OUTSIDE THE FOLDER. A running program cannot delete its own
        // executable on Windows, nor the DLLs it has loaded, nor the log it holds open, so the
        // first version of this deleted assets\ and stopped with bin\ and every user folder still
        // there and no error that said why. So: a copy of the small finisher goes to the
        // temporary directory, is started with this process id and this folder, waits for this
        // program to exit, and takes the folder away.
        ec.clear();
        const std::filesystem::path finisher = oot::places::program() / FINISHER;
        wchar_t temp_dir[MAX_PATH] = {};
        const DWORD temp_len = GetTempPathW(MAX_PATH, temp_dir);
        const std::filesystem::path aside =
            std::filesystem::path(std::wstring(temp_dir, temp_len)) /
            (L"oot-uninstall-" + std::to_wstring(GetCurrentProcessId()) + L".exe");

        std::filesystem::copy_file(finisher, aside, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            return "The shortcut and the listing were removed, but the files could not be: "
                   + ec.message();
        }

        std::wstring command = L"\"" + aside.wstring() + L"\" " +
                               std::to_wstring(GetCurrentProcessId()) + L" \"" + data.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const BOOL started = CreateProcessW(aside.wstring().c_str(), command.data(), nullptr, nullptr,
                                            FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                            &startup, &process);
        if (!started) {
            return "The shortcut and the listing were removed, but the files could not be.";
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);

        std::string message = "Removing " + data.string() + ". This will close now.";
        if (!rescued.empty()) {
            message += " Your copy of the game was moved to " + rescued + " rather than deleted.";
        }
        return message;
    }

} // namespace oot::install
