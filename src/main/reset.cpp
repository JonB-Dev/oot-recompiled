#include "reset.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include <Windows.h>

namespace oot::reset {

    namespace {

        constexpr std::string_view FLAG = "--reset-from";
        constexpr DWORD WAIT_MS = 15000;

        std::atomic<bool> g_armed{ false };

        std::wstring own_path() {
            wchar_t path[MAX_PATH * 4] = {};
            const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
            return std::wstring(path, length);
        }

        // The program again, straight into the game, waiting on the copy `id`.
        std::wstring command_line(const std::wstring& path, unsigned long id) {
            return L"\"" + path + L"\" --play " + std::wstring(FLAG.begin(), FLAG.end()) + L" " + std::to_wstring(id);
        }

    } // namespace

    void arm() {
        g_armed.store(true, std::memory_order_release);
        std::fprintf(stderr, "[reset] armed: this copy starts itself again as it exits\n");
    }

    bool armed() {
        return g_armed.load(std::memory_order_acquire);
    }

    bool relaunch_if_armed() {
        if (!armed()) {
            return false;
        }
        const std::wstring path = own_path();
        if (path.empty()) {
            std::fprintf(stderr, "[reset] the program's own path could not be read, so it was not started again\n");
            return false;
        }
        std::wstring command = command_line(path, GetCurrentProcessId());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
            std::fprintf(stderr, "[reset] the program would not start again (error %lu)\n", GetLastError());
            return false;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        std::fprintf(stderr, "[reset] started again; this copy is closing\n");
        return true;
    }

    unsigned long previous_id(int argc, char** argv) {
        for (int i = 1; i + 1 < argc; ++i) {
            if (argv[i] == FLAG) {
                char* end = nullptr;
                const unsigned long id = std::strtoul(argv[i + 1], &end, 10);
                return (end != argv[i + 1] && *end == '\0') ? id : 0;
            }
        }
        return 0;
    }

    void wait_for_previous(int argc, char** argv) {
        const unsigned long id = previous_id(argc, argv);
        if (id == 0) {
            return;
        }
        const HANDLE previous = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(id));
        if (previous == nullptr) {
            return;   // already gone
        }
        WaitForSingleObject(previous, WAIT_MS);
        CloseHandle(previous);
    }

    int selftest() {
        int failures = 0;
        const auto expect = [&failures](bool ok, const char* what) {
            if (!ok) {
                std::fprintf(stderr, "RESET_SELFTEST_FAILED: %s\n", what);
                ++failures;
            }
        };

        char program[] = "OoTRecompiled.exe";
        char flag[] = "--reset-from";
        char id[] = "4242";
        char word[] = "abc";
        char play[] = "--play";
        char* reset_start[] = { program, play, flag, id };
        char* plain_start[] = { program, play };
        char* bad_id[] = { program, flag, word };
        char* no_id[] = { program, flag };
        expect(previous_id(4, reset_start) == 4242, "the id after --reset-from was not read");
        expect(previous_id(2, plain_start) == 0, "a start without the flag read as a reset's");
        expect(previous_id(3, bad_id) == 0, "a word after the flag read as an id");
        expect(previous_id(2, no_id) == 0, "the flag with nothing after it read as an id");

        const std::wstring command = command_line(L"C:\\a b\\OoTRecompiled.exe", 77);
        expect(command == L"\"C:\\a b\\OoTRecompiled.exe\" --play --reset-from 77",
               "the reset's command is not the quoted program, --play and --reset-from with the id");

        // The wait holds until the copy it names has exited: a real copy of this program, running a
        // check that ends on its own, stands in for the one that closed.
        // It is handed this copy's output handles, so it prints here and never moves a log aside.
        const std::wstring path = own_path();
        std::wstring child = L"\"" + path + L"\" --rom-details-selftest";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION process{};
        if (CreateProcessW(path.c_str(), child.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process)) {
            std::string id_text = std::to_string(process.dwProcessId);
            char* waiting_start[] = { program, flag, id_text.data() };
            wait_for_previous(3, waiting_start);
            DWORD code = 0;
            expect(GetExitCodeProcess(process.hProcess, &code) && code != STILL_ACTIVE,
                   "the wait ended while the copy it names was still running");
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
        else {
            expect(false, "a copy of the program to wait on would not start");
        }

        char gone[] = "4294967295";
        char* gone_start[] = { program, flag, gone };
        const ULONGLONG before = GetTickCount64();
        wait_for_previous(3, gone_start);
        expect(GetTickCount64() - before < 1000, "waiting on a copy that is not there took more than a second");

        if (failures != 0) {
            return 1;
        }
        std::printf("RESET_SELFTEST_OK\n");
        return 0;
    }

} // namespace oot::reset
