// The last step of an uninstall, run from outside the folder being removed.
//
// A running program cannot delete its own executable on Windows, nor the DLLs it has loaded, nor
// the log file it holds open. So the removal that the program itself can do (the shortcut, the
// registry entry, rescuing any copy of the game out of the way) happens in the program, and THIS
// runs afterward from a copy of itself in the temporary directory: it waits for the program to
// exit and then takes the folder away.
//
// It is its own binary rather than the game copied aside because the game is twenty seven
// megabytes and this is a hundred kilobytes, and a copy of it is what gets left behind in the
// temporary directory. It imports nothing but the operating system.
//
//     uninstall.exe <pid to wait for> "<folder to remove>"
//
// It removes exactly the folder it is given and nothing else, and it refuses a folder that is a
// root, is empty, or does not exist. There is no path here that can be talked into removing
// something else: it is started by the program, with the program's own data folder.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

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

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        return 2;
    }

    const DWORD pid = static_cast<DWORD>(std::strtoul(argv[1], nullptr, 10));
    const std::filesystem::path folder(argv[2]);

    std::error_code ec;
    if (folder.empty() || !folder.has_parent_path() || folder == folder.root_path() ||
        !std::filesystem::is_directory(folder, ec)) {
        return 2;
    }

    wait_for(pid);

    // A handful of tries, because a process that has just been told to quit can still hold a file
    // for a moment after its window has gone.
    std::uintmax_t removed = 0;
    for (int attempt = 0; attempt < 10; ++attempt) {
        ec.clear();
        removed = std::filesystem::remove_all(folder, ec);
        if (!ec && !std::filesystem::exists(folder, ec)) {
            break;
        }
        Sleep(500);
    }

    // The report goes beside this copy of itself, in the temporary directory, because the folder
    // it would otherwise have gone in is the one that was just removed.
    std::filesystem::path report(argv[0]);
    report.replace_extension(".log");
    std::ofstream out(report, std::ios::binary | std::ios::trunc);
    if (out) {
        out << "removed " << removed << " files and folders from " << folder.string() << "\n";
        if (std::filesystem::exists(folder, ec)) {
            out << "SOME FILES REMAIN, which usually means something still had them open\n";
        }
    }
    return std::filesystem::exists(folder, ec) ? 1 : 0;
}
