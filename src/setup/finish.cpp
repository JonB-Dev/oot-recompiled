// The last step of an uninstall, run from outside the folders being removed.
//
// A running program cannot delete its own executable on Windows, nor the DLLs it has loaded, nor
// the log file it holds open. So the removal that the program itself can do (the shortcut, the
// registry entry) happens in the program, and THIS runs afterward from a copy of itself in the
// temporary directory: it waits for the program to exit and then takes the program's folders away.
//
// It is its own binary rather than the game copied aside because the game is twenty seven
// megabytes and this is a hundred kilobytes, and a copy of it is what gets left behind in the
// temporary directory. It imports nothing but the operating system.
//
//     uninstall.exe <pid to wait for> "<folder to remove>" ["<folder to remove>" ...]
//
// It removes exactly the folders it is given, which are the program's own (install.h,
// remove_program): never the data folder they sit in, which holds the person's saves, ROM, packs,
// mods and settings. That folder goes only if it is left empty, which a plain remove of a folder
// with anything in it refuses. A folder that is a root, is empty, or does not exist is refused.
// There is no path here that can be talked into removing something else: it is started by the
// program, with the program's own folders.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

    // Long enough for a program to finish exiting, short enough that a wedged one does not leave
    // this waiting forever. If it has not gone by then, the removal is attempted anyway and the
    // files still in use simply stay.
    constexpr DWORD WAIT_MS = 30000;

    void wait_for(DWORD pid) {
        if (pid == 0) {
            return;
        }
        const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (process == nullptr) {
            // Already gone, which is the common case by the time this starts.
            return;
        }
        WaitForSingleObject(process, WAIT_MS);
        CloseHandle(process);
    }

    bool removable(const std::filesystem::path& folder) {
        std::error_code ec;
        return !folder.empty() && folder.has_parent_path() && folder != folder.root_path() &&
               folder.parent_path() != folder.root_path() && std::filesystem::is_directory(folder, ec);
    }

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        return 2;
    }

    const DWORD pid = static_cast<DWORD>(std::strtoul(argv[1], nullptr, 10));
    std::vector<std::filesystem::path> folders;
    for (int i = 2; i < argc; ++i) {
        const std::filesystem::path folder(argv[i]);
        if (!removable(folder)) {
            return 2;
        }
        folders.push_back(folder);
    }

    wait_for(pid);

    // The report goes beside this copy of itself, in the temporary directory, because the folder
    // it would otherwise have gone in is one of those being removed.
    std::filesystem::path report(argv[0]);
    report.replace_extension(".log");
    std::ofstream out(report, std::ios::binary | std::ios::trunc);

    bool left = false;
    std::error_code ec;
    for (const std::filesystem::path& folder : folders) {
        // A handful of tries, because a process that has just been told to quit can still hold a
        // file for a moment after its window has gone.
        std::uintmax_t removed = 0;
        for (int attempt = 0; attempt < 10; ++attempt) {
            ec.clear();
            removed = std::filesystem::remove_all(folder, ec);
            if (!ec && !std::filesystem::exists(folder, ec)) {
                break;
            }
            Sleep(500);
        }
        const bool remains = std::filesystem::exists(folder, ec);
        left = left || remains;
        if (out) {
            out << "removed " << removed << " files and folders from " << folder.string() << "\n";
            if (remains) {
                out << "SOME FILES REMAIN in it, which usually means something still had them open\n";
            }
        }

        // The folder it sat in goes too, but only if nothing is left in it: removing a folder that
        // is not empty fails, which is the whole guarantee that the person's things stay.
        ec.clear();
        if (std::filesystem::remove(folder.parent_path(), ec) && out) {
            out << "and the empty " << folder.parent_path().string() << "\n";
        }
    }
    return left ? 1 : 0;
}
