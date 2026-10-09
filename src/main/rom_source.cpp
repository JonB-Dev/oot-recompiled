#include "rom_source.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

// The same hash the runtime uses, from the same header it uses. Header only, so including it here
// costs a copy of the implementation rather than a link dependency, which is what librecomp does
// with it too.
#include "xxHash/xxh3.h"

namespace oot::rom {

    namespace {

        // The file that holds the chosen ROM's path. One line, plain text, written beside
        // whatever the runtime's config directory is, so if that directory ever moves this
        // follows it without another change.
        constexpr const char* POINTER_FILE = "rom.txt";

        // The first four bytes of an N64 ROM in the correct byte order.
        constexpr std::array<uint8_t, 4> FIRST_BYTES{ 0x80, 0x37, 0x12, 0x40 };

        enum class Byteswap { None, Groups4, Groups2, Invalid };

        std::vector<uint8_t> read_whole_file(const std::filesystem::path& path) {
            std::vector<uint8_t> bytes;
            std::ifstream file{ path, std::ios::binary };
            if (file.good()) {
                file.seekg(0, std::ios::end);
                const std::streamoff size = file.tellg();
                if (size > 0) {
                    bytes.resize(static_cast<size_t>(size));
                    file.seekg(0, std::ios::beg);
                    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                }
            }
            return bytes;
        }

        Byteswap detect_byteswap(const std::vector<uint8_t>& bytes) {
            if (bytes.size() < 4) {
                return Byteswap::Invalid;
            }
            const auto matches = [&](size_t a, size_t b, size_t c, size_t d) {
                return bytes[0] == FIRST_BYTES[a] && bytes[1] == FIRST_BYTES[b] &&
                       bytes[2] == FIRST_BYTES[c] && bytes[3] == FIRST_BYTES[d];
            };
            if (matches(0, 1, 2, 3)) return Byteswap::None;
            if (matches(3, 2, 1, 0)) return Byteswap::Groups4;
            if (matches(1, 0, 3, 2)) return Byteswap::Groups2;
            return Byteswap::Invalid;
        }

        // Reproduced from the runtime's byteswap_data. The exclusive or is the whole trick: 3
        // reverses each group of four, 1 swaps each pair.
        void apply_byteswap(std::vector<uint8_t>& bytes, size_t index_xor) {
            for (size_t pos = 0; pos + 3 < bytes.size(); pos += 4) {
                const uint8_t b0 = bytes[pos + 0];
                const uint8_t b1 = bytes[pos + 1];
                const uint8_t b2 = bytes[pos + 2];
                const uint8_t b3 = bytes[pos + 3];
                bytes[pos + (0 ^ index_xor)] = b0;
                bytes[pos + (1 ^ index_xor)] = b1;
                bytes[pos + (2 ^ index_xor)] = b2;
                bytes[pos + (3 ^ index_xor)] = b3;
            }
        }

        // The internal name sits at 0x20 in the header. Only read once the file is known to be
        // long enough, which the size check below guarantees.
        std::string_view internal_name_of(const std::vector<uint8_t>& bytes, size_t length) {
            return std::string_view{ reinterpret_cast<const char*>(bytes.data()) + 0x20, length };
        }

    } // namespace

    recomp::RomValidationError validate(const std::filesystem::path& path,
                                        const recomp::GameEntry& entry,
                                        std::vector<uint8_t>& contents) {
        contents.clear();

        std::vector<uint8_t> bytes = read_whole_file(path);
        if (bytes.empty()) {
            return recomp::RomValidationError::FailedToOpen;
        }

        // Pad to a multiple of four, as the runtime does, so the byte order pass and the hash see
        // the same shape whatever the file's length.
        bytes.resize((bytes.size() + 3) & ~static_cast<size_t>(3));

        switch (detect_byteswap(bytes)) {
            case Byteswap::Invalid:
                return recomp::RomValidationError::NotARom;
            case Byteswap::Groups2:
                apply_byteswap(bytes, 1);
                break;
            case Byteswap::Groups4:
                apply_byteswap(bytes, 3);
                break;
            case Byteswap::None:
                break;
        }

        if (XXH3_64bits(bytes.data(), bytes.size()) != entry.rom_hash) {
            // A header long enough to hold the name, or there is nothing to tell the user beyond
            // "not this game". The runtime reads this offset without checking; a truncated file
            // reaching here would read past the end, so the length is checked first.
            const size_t needed = 0x20 + entry.internal_name.size();
            if (bytes.size() >= needed && internal_name_of(bytes, entry.internal_name.size()) == entry.internal_name) {
                return recomp::RomValidationError::IncorrectVersion;
            }
            return recomp::RomValidationError::IncorrectRom;
        }

        contents = std::move(bytes);
        return recomp::RomValidationError::Good;
    }

    std::filesystem::path remembered(const std::filesystem::path& directory) {
        std::ifstream file{ directory / POINTER_FILE };
        if (!file.good()) {
            return {};
        }
        std::string line;
        if (!std::getline(file, line)) {
            return {};
        }
        // Trim the line ending and any stray whitespace: this file is hand editable, and a path
        // with a trailing carriage return simply does not exist.
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (line.empty()) {
            return {};
        }
        return std::filesystem::path(line);
    }

    void remember(const std::filesystem::path& directory, const std::filesystem::path& rom) {
        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(rom, ec);
        const std::filesystem::path& to_write = ec ? rom : absolute;

        std::ofstream file{ directory / POINTER_FILE, std::ios::trunc };
        if (!file.good()) {
            std::fprintf(stderr, "[rom] the chosen ROM's path could not be saved; it will be asked for again\n");
            return;
        }
        file << to_write.string() << '\n';
    }

    void forget(const std::filesystem::path& directory) {
        std::error_code ec;
        std::filesystem::remove(directory / POINTER_FILE, ec);
    }

    std::filesystem::path stale_stored_copy(const std::filesystem::path& directory,
                                            const recomp::GameEntry& entry) {
        const std::filesystem::path candidate = directory / entry.stored_filename();
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
        return {};
    }

} // namespace oot::rom
