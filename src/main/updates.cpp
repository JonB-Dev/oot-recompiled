#include "main/updates.h"

#include "main/places.h"
#include "main/signature.h"
#include "ui/ui_settings.h"
#include "build_info.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace oot::updates {

    namespace {

        // The manifest is a handful of short lines. Anything bigger than this is not our file and
        // is refused before it is read, rather than after: a size cap ahead of the read is the
        // cheapest protection there is against a body that never ends.
        constexpr DWORD MAX_MANIFEST = 64 * 1024;

        // And a cap on the artifact, which is a setup file of about twenty megabytes. Generous
        // enough not to be a maintenance trap, small enough that a body that never ends cannot
        // fill somebody's disk.
        constexpr std::uintmax_t MAX_ARTIFACT = 256ull * 1024 * 1024;

        // The prefix every fetch is built from. The manifest supplies a NAME which is appended to
        // this; it never supplies a path and can never supply a host.
        constexpr const wchar_t* PREFIX = L"/oot-recompiled/";

        // WinHTTP, which is the operating system's own client. Deliberately not a library:
        // `.scaffold/dependencies.md` forbids any dependency that opens a network connection, and
        // this keeps that rule intact rather than carving an exception into it.
        constexpr const wchar_t* AGENT = L"OoTRecompiled";

        std::atomic<State> g_state{ State::Idle };
        std::mutex g_mutex;
        std::string g_available;
        std::atomic<bool> g_started{ false };
        // When the last check finished, for the four hour cadence. Zero until one has.
        std::atomic<long long> g_checked_at_s{ 0 };
        constexpr long long RECHECK_SECONDS = 4 * 60 * 60;
        long long now_s() {
            return std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // A version as three numbers. Anything that does not parse compares as nothing, so a
        // manifest full of garbage can never be read as newer.
        struct Version {
            int major = -1;
            int minor = -1;
            int patch = -1;
            bool ok() const { return major >= 0 && minor >= 0 && patch >= 0; }
        };

        Version parse_version(const std::string& text) {
            Version v;
            int part = 0;
            int value = 0;
            bool any = false;
            for (size_t i = 0; i <= text.size(); ++i) {
                const char c = i < text.size() ? text[i] : '.';
                if (c >= '0' && c <= '9') {
                    // A version number that long is not a version number.
                    if (value > 100000) {
                        return Version{};
                    }
                    value = value * 10 + (c - '0');
                    any = true;
                    continue;
                }
                if (c != '.' || !any) {
                    return Version{};
                }
                if (part == 0) {
                    v.major = value;
                }
                else if (part == 1) {
                    v.minor = value;
                }
                else if (part == 2) {
                    v.patch = value;
                }
                else {
                    return Version{};
                }
                ++part;
                value = 0;
                any = false;
                if (part == 3) {
                    break;
                }
            }
            return part == 3 ? v : Version{};
        }

        // The version a setup file's own name carries: OoT-Recompiled-<version>-windows.exe.
        // Read at boot from a file left by a previous session, when no manifest is in hand.
        Version version_in_name(const std::string& name) {
            const std::string head = "OoT-Recompiled-";
            const std::string tail = "-windows.exe";
            if (name.size() <= head.size() + tail.size() ||
                name.compare(0, head.size(), head) != 0 ||
                name.compare(name.size() - tail.size(), tail.size(), tail) != 0) {
                return Version{};
            }
            return parse_version(name.substr(head.size(), name.size() - head.size() - tail.size()));
        }

        bool newer(const Version& candidate, const Version& running) {
            if (!candidate.ok() || !running.ok()) {
                return false;
            }
            if (candidate.major != running.major) {
                return candidate.major > running.major;
            }
            if (candidate.minor != running.minor) {
                return candidate.minor > running.minor;
            }
            return candidate.patch > running.patch;
        }

        // The manifest is remote, so it is parsed the way the save file and the settings file are
        // parsed here: as hostile input. Only the one field we need is read, by a hand written
        // reader rather than a YAML library, which would be a new dependency, a new parser to
        // trust, and a poor trade for one line.
        std::string field(const std::string& body, const char* key) {
            size_t start = 0;
            while (start < body.size()) {
                size_t end = body.find('\n', start);
                if (end == std::string::npos) {
                    end = body.size();
                }
                std::string line = body.substr(start, end - start);
                start = end + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                    line.pop_back();
                }
                // Top level only: the `files:` list repeats these names indented under a
                // dash, and taking one of those would read a field of a different record.
                if (!line.empty() && (line[0] == ' ' || line[0] == '-')) {
                    continue;
                }
                const size_t key_len = std::char_traits<char>::length(key);
                if (line.compare(0, key_len, key) != 0) {
                    continue;
                }
                std::string value = line.substr(key_len);
                size_t first = value.find_first_not_of(" \t'\"");
                if (first == std::string::npos) {
                    return {};
                }
                size_t last = value.find_last_not_of(" \t'\"");
                return value.substr(first, last - first + 1);
            }
            return {};
        }

        // One GET, one fixed host, one fixed path. No redirect is followed to another host and no
        // certificate error is ignored, which are both the WinHTTP defaults and are left alone
        // deliberately: there is no flag here to relax either.
        bool fetch(std::string& body) {
            const HINTERNET session = WinHttpOpen(AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (session == nullptr) {
                return false;
            }
            // A check that hangs must not hold the program's shutdown, so every stage is bounded.
            DWORD timeout = 8000;
            WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout);

            bool ok = false;
            const HINTERNET connection = WinHttpConnect(session, HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
            if (connection != nullptr) {
                const HINTERNET request = WinHttpOpenRequest(connection, L"GET", PATH, nullptr,
                                                             WINHTTP_NO_REFERER,
                                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                             WINHTTP_FLAG_SECURE);
                if (request != nullptr) {
                    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                        WinHttpReceiveResponse(request, nullptr)) {

                        DWORD status = 0;
                        DWORD size = sizeof(status);
                        WinHttpQueryHeaders(request,
                                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                            WINHTTP_NO_HEADER_INDEX);
                        if (status == 200) {
                            std::vector<char> buffer(4096);
                            DWORD read = 0;
                            ok = true;
                            while (WinHttpReadData(request, buffer.data(),
                                                   static_cast<DWORD>(buffer.size()), &read) && read > 0) {
                                if (body.size() + read > MAX_MANIFEST) {
                                    ok = false;
                                    break;
                                }
                                body.append(buffer.data(), read);
                            }
                        }
                    }
                    WinHttpCloseHandle(request);
                }
                WinHttpCloseHandle(connection);
            }
            WinHttpCloseHandle(session);
            return ok;
        }

        // The manifest's other two fields, kept from the check so the download can be verified
        // against what the check actually saw rather than re-fetching and trusting twice.
        std::string g_file_name;
        std::string g_sha512;
        std::filesystem::path g_downloaded;

        // The artifact's file name, from the manifest. IT IS A FILE NAME AND NOT A URL, and that
        // distinction is the SSRF trap the threat model names: the field looks like a path and it
        // is tempting to treat it as one. Anything with a scheme, a host, a separator or a `..` in
        // it is refused outright rather than sanitized, because a name we cannot vouch for is not
        // a name worth repairing.
        bool safe_file_name(const std::string& name) {
            if (name.empty() || name.size() > 128) {
                return false;
            }
            if (name.find(':') != std::string::npos || name.find('/') != std::string::npos ||
                name.find('\\') != std::string::npos || name.find("..") != std::string::npos) {
                return false;
            }
            for (const char c : name) {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
                if (!ok) {
                    return false;
                }
            }
            // Only the setup file is ever fetched. The portable zip is a download a person makes
            // themselves; this program does not unpack folders under anybody.
            return name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0;
        }

        // SHA-512 through the operating system's own CNG, for the same reason the HTTP client is
        // WinHTTP: no dependency may be added for this.
        std::string sha512_of(const std::filesystem::path& file) {
            BCRYPT_ALG_HANDLE algorithm = nullptr;
            if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA512_ALGORITHM, nullptr, 0) != 0) {
                return {};
            }

            DWORD length = 0;
            DWORD ignored = 0;
            BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&length),
                              sizeof(length), &ignored, 0);

            BCRYPT_HASH_HANDLE hash = nullptr;
            std::string digest;
            if (length > 0 && BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
                std::ifstream in(file, std::ios::binary);
                std::vector<char> buffer(64 * 1024);
                bool ok = static_cast<bool>(in);
                while (ok && in) {
                    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                    const std::streamsize got = in.gcount();
                    if (got <= 0) {
                        break;
                    }
                    ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                                        static_cast<ULONG>(got), 0) == 0;
                }
                if (ok) {
                    std::vector<unsigned char> out(length);
                    if (BCryptFinishHash(hash, out.data(), length, 0) == 0) {
                        digest.assign(reinterpret_cast<const char*>(out.data()), out.size());
                    }
                }
                BCryptDestroyHash(hash);
            }
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return digest;
        }

        // The manifest carries the digest base64 encoded, the way electron-updater writes it.
        std::string from_base64(const std::string& text) {
            DWORD size = 0;
            if (!CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()),
                                      CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr)) {
                return {};
            }
            std::vector<unsigned char> out(size);
            if (!CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()),
                                      CRYPT_STRING_BASE64, out.data(), &size, nullptr, nullptr)) {
                return {};
            }
            return std::string(reinterpret_cast<const char*>(out.data()), size);
        }

        // One GET into a file, with a cap. The same fixed host, and a path built from the fixed
        // prefix plus a name that has already passed safe_file_name.
        bool fetch_to_file(const std::wstring& path, const std::filesystem::path& target,
                           std::uintmax_t limit) {
            const HINTERNET session = WinHttpOpen(AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (session == nullptr) {
                return false;
            }
            // Longer than the manifest's, because this is tens of megabytes on whatever
            // connection somebody has, but still bounded.
            WinHttpSetTimeouts(session, 15000, 15000, 30000, 120000);

            bool ok = false;
            const HINTERNET connection = WinHttpConnect(session, HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
            if (connection != nullptr) {
                const HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
                                                             WINHTTP_NO_REFERER,
                                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                             WINHTTP_FLAG_SECURE);
                if (request != nullptr) {
                    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                        WinHttpReceiveResponse(request, nullptr)) {

                        DWORD status = 0;
                        DWORD size = sizeof(status);
                        WinHttpQueryHeaders(request,
                                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                            WINHTTP_NO_HEADER_INDEX);
                        if (status == 200) {
                            std::ofstream out(target, std::ios::binary | std::ios::trunc);
                            std::vector<char> buffer(64 * 1024);
                            DWORD read = 0;
                            std::uintmax_t total = 0;
                            ok = static_cast<bool>(out);
                            while (ok && WinHttpReadData(request, buffer.data(),
                                                         static_cast<DWORD>(buffer.size()), &read) && read > 0) {
                                total += read;
                                if (total > limit) {
                                    ok = false;
                                    break;
                                }
                                out.write(buffer.data(), read);
                                ok = static_cast<bool>(out);
                            }
                        }
                    }
                    WinHttpCloseHandle(request);
                }
                WinHttpCloseHandle(connection);
            }
            WinHttpCloseHandle(session);
            return ok;
        }

        void refuse(const std::filesystem::path& file, const char* why) {
            std::error_code ec;
            std::filesystem::remove(file, ec);
            g_state.store(State::Refused, std::memory_order_release);
            std::fprintf(stderr, "[updates] REFUSED the download: %s\n", why);
        }

        void download() {
            std::string name;
            std::string want;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                name = g_file_name;
                want = g_sha512;
            }
            if (!safe_file_name(name) || want.empty()) {
                g_state.store(State::Failed, std::memory_order_release);
                std::fprintf(stderr, "[updates] the manifest did not name a file this will fetch\n");
                return;
            }

            // Into the data folder, not %TEMP%: the file has to survive until the next start,
            // which is when it is used.
            std::error_code dir_ec;
            std::filesystem::create_directories(oot::places::updates(), dir_ec);
            const std::filesystem::path target =
                oot::places::updates() / std::filesystem::path(name).filename();

            // A previous session may have fetched and verified this exact file already (the
            // person kept playing rather than restarting). If its bytes still match the digest
            // the manifest names now, there is nothing to fetch.
            {
                std::error_code ec;
                if (std::filesystem::is_regular_file(target, ec) && sha512_of(target) == from_base64(want)) {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_downloaded = target;
                    g_state.store(State::Ready, std::memory_order_release);
                    std::fprintf(stderr, "[updates] %s was already fetched and still matches\n", name.c_str());
                    return;
                }
            }

            // The URL is the compile time prefix plus a name that passed the check above. There
            // is no code path here that takes a URL from the manifest.
            std::wstring path = PREFIX;
            path.append(name.begin(), name.end());

            if (!fetch_to_file(path, target, MAX_ARTIFACT)) {
                std::error_code ec;
                std::filesystem::remove(target, ec);
                g_state.store(State::Failed, std::memory_order_release);
                std::fprintf(stderr, "[updates] the download did not complete\n");
                return;
            }

            // CHECK ONE: the bytes are the bytes the manifest named.
            if (sha512_of(target) != from_base64(want)) {
                refuse(target, "the file does not match the digest in the manifest");
                return;
            }

            // CHECK TWO: Windows trusts the signature AND it is ours. Both halves matter; either
            // alone is not enough.
            const oot::signature::Info signed_by = oot::signature::read_file(target.wstring());
            if (!signed_by.valid) {
                refuse(target, "the file's signature is missing or not trusted");
                return;
            }
            if (signed_by.signer.find(PUBLISHER) == std::string::npos) {
                refuse(target, "the file is signed by somebody else");
                return;
            }

            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_downloaded = target;
            }
            g_state.store(State::Ready, std::memory_order_release);
            std::fprintf(stderr, "[updates] verified: digest matched and signed by %s\n",
                         signed_by.signer.c_str());
        }

        // RUNNING THE SETUP FILE IS THE UPDATE. It writes the new version over bin\ and starts it,
        // exactly as it did on the first install, so there is no separate replace-yourself path
        // here to get wrong. That is the payoff for doing the packaging first. The setup file
        // deletes nothing the user owns and is itself removed by the next start's sweep, because
        // by then its version is no longer newer.
        bool run_setup(const std::filesystem::path& file) {
            std::wstring command = L"\"" + file.wstring() + L"\"";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            const BOOL started = CreateProcessW(file.wstring().c_str(), command.data(), nullptr,
                                                nullptr, FALSE, 0, nullptr, nullptr,
                                                &startup, &process);
            if (!started) {
                std::fprintf(stderr, "[updates] the installer would not start\n");
                return false;
            }
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            std::fprintf(stderr, "[updates] the installer is running; this copy is closing\n");
            return true;
        }

        void run() {
            g_checked_at_s.store(now_s(), std::memory_order_release);
            std::string body;
            if (!fetch(body)) {
                g_state.store(State::Failed, std::memory_order_release);
                std::fprintf(stderr, "[updates] the check did not complete; carrying on\n");
                return;
            }

            const std::string found = field(body, "version:");
            const Version candidate = parse_version(found);
            const Version running = parse_version(oot::build_info::version);
            if (!candidate.ok()) {
                g_state.store(State::Failed, std::memory_order_release);
                std::fprintf(stderr, "[updates] the manifest did not name a version\n");
                return;
            }

            if (newer(candidate, running)) {
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_available = found;
                    // Kept from the SAME answer the version came from, so the download is checked
                    // against what this check actually saw rather than a second fetch nobody
                    // compared with the first.
                    g_file_name = field(body, "path:");
                    g_sha512 = field(body, "sha512:");
                }
                g_state.store(State::Newer, std::memory_order_release);
                std::fprintf(stderr, "[updates] %s is available; this is %s\n",
                             found.c_str(), oot::build_info::version);
                // THE FETCH FOLLOWS THE CHECK, in the background, for an installed copy (the
                // user's decision of 2026-09-23). A portable copy is told and left alone: the
                // setup file would install into application data, which is not where a portable
                // copy lives.
                if (oot::places::mode() == oot::places::Mode::Installed) {
                    start_download();
                }
            }
            else {
                g_state.store(State::Current, std::memory_order_release);
                std::fprintf(stderr, "[updates] this is the newest release\n");
            }
        }

    } // namespace

    void start_if_enabled() {
        // THE SETTING IS THE GATE AND IT DEFAULTS TO OFF. Until the first run question has been
        // answered with a yes, this function does nothing at all, which is what makes the question
        // an honest one.
        if (oot::ui::settings().updates != 1) {
            return;
        }
        if (g_started.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        g_state.store(State::Checking, std::memory_order_release);
        // Detached, because nothing waits for it. The worst case is that the program exits while
        // it is still in flight, and WinHTTP's timeouts bound how long that can be.
        std::thread(run).detach();
    }

    State state() {
        return g_state.load(std::memory_order_acquire);
    }

    void tick() {
        if (oot::ui::settings().updates != 1) {
            return;
        }
        const State s = state();
        if (s == State::Idle) {
            // The row was switched on after launch: the first check runs now rather than at the
            // next start, which is what a person who just said yes expects to happen.
            start_if_enabled();
            return;
        }
        if (s != State::Current && s != State::Failed) {
            // A newer version is known, being fetched, ready, or refused: nothing to re-ask.
            return;
        }
        const long long last = g_checked_at_s.load(std::memory_order_acquire);
        if (last == 0 || now_s() - last < RECHECK_SECONDS) {
            return;
        }
        // Four hours on: the same check again, on its own thread, with the same silence on
        // failure. Stamped first so a slow answer cannot start a second one.
        g_checked_at_s.store(now_s(), std::memory_order_release);
        g_state.store(State::Checking, std::memory_order_release);
        std::thread(run).detach();
    }

    std::string available() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_available;
    }

    std::string line() {
        switch (state()) {
            case State::Newer:
                // Only a portable copy rests here; an installed one moves straight on.
                return available() + " is available at releases.jonbarnes.dev";
            case State::Downloading:
                return "Getting " + available() + " in the background...";
            case State::Ready:
                return available() + " is ready and installs the next time this opens. F5 installs it now";
            case State::Refused:
                // THE ONE THING HERE WORTH INTERRUPTING SOMEBODY ABOUT. A file arrived from the
                // network and failed verification, so it was deleted; saying nothing would be the
                // wrong kind of quiet.
                return "The update did not verify and was deleted. This copy is unchanged";
            default:
                // Nothing to say. A check that found nothing, or failed, is not news, and the
                // foot of the launcher is not the place to report this program's own plumbing.
                return {};
        }
    }

    bool plate(std::string& value, std::string& note) {
        switch (state()) {
            case State::Downloading:
                value = "Update";
                note = "getting " + available();
                return true;
            case State::Ready:
                value = available() + " ready";
                note = "installs at next start";
                return true;
            case State::Refused:
                value = "Update refused";
                note = "this copy is unchanged";
                return true;
            default:
                return false;
        }
    }

    void start_download() {
        // Only from Newer, and only once. Nothing here starts on its own: this is called because
        // somebody pressed the key that asks for it.
        State expected = State::Newer;
        if (!g_state.compare_exchange_strong(expected, State::Downloading,
                                             std::memory_order_acq_rel)) {
            return;
        }
        std::thread(download).detach();
    }

    bool install() {
        if (state() != State::Ready) {
            return false;
        }
        std::filesystem::path file;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            file = g_downloaded;
        }
        if (file.empty()) {
            return false;
        }

        return run_setup(file);
    }

    bool install_pending() {
        if (oot::places::mode() != oot::places::Mode::Installed) {
            return false;
        }
        const std::filesystem::path dir = oot::places::updates();
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) {
            return false;
        }
        const Version running = parse_version(oot::build_info::version);
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            const std::filesystem::path file = entry.path();
            const std::string name = file.filename().string();
            // Anything that is not a newer setup file by its name is left over from an update
            // that already happened, or is not ours at all. Either way it goes.
            if (!newer(version_in_name(name), running)) {
                std::filesystem::remove(file, ec);
                continue;
            }
            // THE SIGNATURE IS CHECKED AGAIN HERE, because this file sat on disk between two
            // sessions and the digest it was checked against belonged to a manifest that is no
            // longer in hand. Windows must trust it and it must be ours, or it is deleted.
            const oot::signature::Info signed_by = oot::signature::read_file(file.wstring());
            if (!signed_by.valid || signed_by.signer.find(PUBLISHER) == std::string::npos) {
                std::filesystem::remove(file, ec);
                std::fprintf(stderr, "[updates] a waiting setup file did not verify and was deleted\n");
                continue;
            }
            std::fprintf(stderr, "[updates] installing %s left ready by the last session\n", name.c_str());
            if (run_setup(file)) {
                return true;
            }
        }
        return false;
    }

} // namespace oot::updates
