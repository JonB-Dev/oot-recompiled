// Print the two things a recomp::GameEntry needs to identify a ROM: its XXH3 64 bit hash and the
// internal name in its header.
//
// Both are read from the actual file rather than copied from anywhere. The runtime rejects a ROM
// whose hash does not match, and it reads the internal name at offset 0x20 to tell "wrong version
// of the right game" from "wrong game entirely", so a guessed value here produces a confusing
// error for the user rather than an obvious one for us.
//
// Built by tools/build_rom_identity.cmd. Not part of the game build: it runs once, and its output
// is pasted into src/game/game_init.cpp with a comment saying where it came from.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#define XXH_INLINE_ALL
#include "xxhash.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: rom_identity <rom>\n");
        return 2;
    }

    std::FILE* f = std::fopen(argv[1], "rb");
    if (f == nullptr) {
        std::printf("could not open %s\n", argv[1]);
        return 2;
    }

    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (std::fread(data.data(), 1, data.size(), f) != data.size()) {
        std::printf("short read\n");
        std::fclose(f);
        return 2;
    }
    std::fclose(f);

    uint64_t hash = XXH3_64bits(data.data(), data.size());

    // The internal name is 20 bytes of space padded ASCII at 0x20.
    std::string name(reinterpret_cast<const char*>(data.data()) + 0x20, 20);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\0')) {
        name.pop_back();
    }

    std::printf("  file          %s\n", argv[1]);
    std::printf("  size          %ld bytes\n", size);
    std::printf("  internal name \"%s\"\n", name.c_str());
    std::printf("  rom_hash      0x%016llXULL\n", static_cast<unsigned long long>(hash));
    return 0;
}
