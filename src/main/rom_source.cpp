#include "rom_source.h"

#include <algorithm>
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

#include "game/game.h"

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

        // Whether the header's name is this game's, by its hash (game.h). Only read once the file
        // is known to be long enough, which the size check below guarantees.
        bool header_name_matches(const std::vector<uint8_t>& bytes) {
            return oot::fnv1a64(bytes.data() + oot::ROM_HEADER_NAME_OFFSET, oot::ROM_HEADER_NAME_LENGTH) ==
                   oot::ROM_HEADER_NAME_FNV1A;
        }

        // The header's fields, in the ROM's own byte order: the name's 20 bytes, then the game
        // code whose last two bytes are the region's letter and the version number.
        constexpr size_t HEADER_LENGTH = 0x40;
        constexpr size_t TITLE_LENGTH = 20;
        constexpr size_t REGION_OFFSET = 0x3E;
        constexpr size_t VERSION_OFFSET = 0x3F;

        // The one revision this program is built for (CLAUDE.md, "Version lock").
        constexpr char RUNS_REGION = 'E';
        constexpr uint8_t RUNS_VERSION = 0;

        std::string region_name(char code) {
            switch (code) {
                case 'E': return "USA (NTSC-U)";
                case 'J': return "Japan (NTSC-J)";
                case 'P': case 'X': case 'Y': case 'Z': return "Europe (PAL)";
                case 'D': return "Germany (PAL)";
                case 'F': return "France (PAL)";
                case 'I': return "Italy (PAL)";
                case 'S': return "Spain (PAL)";
                case 'H': return "Netherlands (PAL)";
                case 'W': return "Scandinavia (PAL)";
                case 'U': return "Australia (PAL)";
                case 'N': return "Canada (NTSC)";
                case 'A': return "Asia (NTSC)";
                case 'K': return "Korea (NTSC)";
                case 'B': return "Brazil (MPAL)";
                case 'C': return "China (iQue)";
                default: break;
            }
            if (code > ' ' && code < 0x7F) {
                return std::string("region code ") + code;
            }
            return "an unknown region";
        }

        std::string version_name(uint8_t number) {
            if (number < 10) {
                return "1." + std::to_string(number);
            }
            return "revision " + std::to_string(number);
        }

        // Printable ASCII only, up to the first NUL, with the padding trimmed: a header from
        // another game may hold anything.
        std::string title_text(const uint8_t* bytes) {
            std::string text;
            for (size_t i = 0; i < TITLE_LENGTH && bytes[i] != 0; ++i) {
                if (bytes[i] >= 0x20 && bytes[i] < 0x7F) {
                    text.push_back(static_cast<char>(bytes[i]));
                }
            }
            while (!text.empty() && text.back() == ' ') {
                text.pop_back();
            }
            return text;
        }

    } // namespace

    Details describe(const std::vector<uint8_t>& bytes) {
        Details details;
        std::vector<uint8_t> header(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(std::min(bytes.size(), HEADER_LENGTH)));
        header.resize((header.size() + 3) & ~static_cast<size_t>(3));

        switch (detect_byteswap(header)) {
            case Byteswap::Invalid:
                return details;
            case Byteswap::None:
                details.format = "z64";
                break;
            case Byteswap::Groups2:
                details.format = "v64";
                apply_byteswap(header, 1);
                break;
            case Byteswap::Groups4:
                details.format = "n64";
                apply_byteswap(header, 3);
                break;
        }
        if (bytes.size() < HEADER_LENGTH) {
            return details;
        }

        details.title = title_text(header.data() + oot::ROM_HEADER_NAME_OFFSET);
        details.region = region_name(static_cast<char>(header[REGION_OFFSET]));
        details.version = version_name(header[VERSION_OFFSET]);
        details.this_game = header_name_matches(header);
        details.runs_here = details.this_game && header[REGION_OFFSET] == RUNS_REGION &&
                            header[VERSION_OFFSET] == RUNS_VERSION;
        return details;
    }

    recomp::RomValidationError validate(const std::filesystem::path& path,
                                        const recomp::GameEntry& entry,
                                        std::vector<uint8_t>& contents,
                                        Details* details) {
        contents.clear();

        std::vector<uint8_t> bytes = read_whole_file(path);
        if (details != nullptr) {
            *details = describe(bytes);
        }
        if (bytes.empty()) {
            return recomp::RomValidationError::FailedToOpen;
        }
        return validate_bytes(std::move(bytes), entry, contents);
    }

    recomp::RomValidationError validate_bytes(std::vector<uint8_t> bytes,
                                              const recomp::GameEntry& entry,
                                              std::vector<uint8_t>& contents) {
        contents.clear();
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
            const size_t needed = oot::ROM_HEADER_NAME_OFFSET + oot::ROM_HEADER_NAME_LENGTH;
            if (bytes.size() >= needed && header_name_matches(bytes)) {
                return recomp::RomValidationError::IncorrectVersion;
            }
            return recomp::RomValidationError::IncorrectRom;
        }

        contents = std::move(bytes);
        return recomp::RomValidationError::Good;
    }

    int selftest(const std::filesystem::path& rom, const recomp::GameEntry& entry) {
        const auto fail = [](const char* what) {
            std::fprintf(stderr, "ROM_SELFTEST_FAILED: %s\n", what);
            return 1;
        };
        const std::vector<uint8_t> original = read_whole_file(rom);
        if (original.empty()) return fail("the ROM given could not be read");
        std::vector<uint8_t> contents;

        // The user's own ROM is this game, as it is on disk.
        if (validate_bytes(original, entry, contents) != recomp::RomValidationError::Good) return fail("the right ROM was refused");

        // One byte changed past the header: still this game's name, another revision.
        std::vector<uint8_t> revision = original;
        revision[revision.size() / 2] ^= 0xFF;
        if (validate_bytes(revision, entry, contents) != recomp::RomValidationError::IncorrectVersion) {
            return fail("a changed byte past the header did not read as another revision");
        }

        // One letter of the header's name changed: a different game entirely.
        std::vector<uint8_t> other = revision;
        other[oot::ROM_HEADER_NAME_OFFSET] ^= 0x01;
        if (validate_bytes(other, entry, contents) != recomp::RomValidationError::IncorrectRom) {
            return fail("a different header name did not read as another game");
        }

        // A header too short to hold the name is another game, and is not read past its end.
        const std::vector<uint8_t> stub(original.begin(), original.begin() + 0x24);
        if (validate_bytes(stub, entry, contents) != recomp::RomValidationError::IncorrectRom) {
            return fail("a file too short for the name was not another game");
        }

        // The header of the user's own ROM says it is the one revision this program runs.
        const Details own = describe(original);
        if (!own.this_game || !own.runs_here || own.region != "USA (NTSC-U)" || own.version != "1.0" || own.title.empty()) {
            return fail("the right ROM's header did not read as this game's USA 1.0");
        }

        std::printf("ROM_SELFTEST_OK\n");
        return 0;
    }

    int details_selftest() {
        int failures = 0;
        const auto expect = [&failures](bool ok, const char* what) {
            if (!ok) {
                std::fprintf(stderr, "ROM_DETAILS_SELFTEST_FAILED: %s\n", what);
                ++failures;
            }
        };

        // A made-up header in the native order: another game's name, Europe, version 1.1.
        std::vector<uint8_t> z64(HEADER_LENGTH + 8, 0);
        std::copy(FIRST_BYTES.begin(), FIRST_BYTES.end(), z64.begin());
        const std::string_view name = "SELF TEST ROM       ";
        std::copy(name.begin(), name.end(), z64.begin() + oot::ROM_HEADER_NAME_OFFSET);
        z64[REGION_OFFSET] = 'P';
        z64[VERSION_OFFSET] = 1;

        std::vector<uint8_t> v64 = z64;
        apply_byteswap(v64, 1);
        std::vector<uint8_t> n64 = z64;
        apply_byteswap(n64, 3);

        for (const auto& [bytes, format] : { std::pair{ z64, "z64" }, std::pair{ v64, "v64" }, std::pair{ n64, "n64" } }) {
            const Details d = describe(bytes);
            expect(d.format == format, "the byte order was not named");
            expect(d.title == "SELF TEST ROM", "the title was not read and trimmed");
            expect(d.region == "Europe (PAL)", "region P was not Europe (PAL)");
            expect(d.version == "1.1", "version 1 was not 1.1");
            expect(!d.this_game && !d.runs_here, "another game's header read as this game");
        }

        std::vector<uint8_t> other = z64;
        other[REGION_OFFSET] = 'E';
        other[VERSION_OFFSET] = 0;
        expect(describe(other).region == "USA (NTSC-U)" && describe(other).version == "1.0", "E and 0 were not USA 1.0");
        expect(!describe(other).runs_here, "USA 1.0 of another game read as runnable");
        other[REGION_OFFSET] = 'Q';
        other[VERSION_OFFSET] = 12;
        expect(describe(other).region == "region code Q", "an unlisted region was not named by its code");
        expect(describe(other).version == "revision 12", "a large version number was not a revision");
        other[REGION_OFFSET] = 0x01;
        expect(describe(other).region == "an unknown region", "an unprintable region code was shown");

        std::vector<uint8_t> odd = z64;
        odd[oot::ROM_HEADER_NAME_OFFSET + 1] = 0x07;
        odd[oot::ROM_HEADER_NAME_OFFSET + 5] = 0;
        expect(describe(odd).title == "SLF", "an unprintable byte or a NUL in the title was kept");

        const std::vector<uint8_t> short_file(z64.begin(), z64.begin() + 0x24);
        const Details s = describe(short_file);
        expect(s.format == "z64" && s.title.empty() && s.region.empty(), "a short file was read past its end");

        const std::string_view text = "this is a text file, not a cartridge image";
        expect(describe(std::vector<uint8_t>(text.begin(), text.end())).format.empty(), "a text file was given a byte order");
        expect(describe({}).format.empty(), "an empty file was given a byte order");

        if (failures != 0) {
            return 1;
        }
        std::printf("ROM_DETAILS_SELFTEST_OK\n");
        return 0;
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
