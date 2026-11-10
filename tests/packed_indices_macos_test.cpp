#include "shared/rt64_packed_indices.h"
#include <array>
#include <cstdio>

int main() {
    constexpr std::array<uint32_t, 3> words = {0x00010001u, 0xffff0000u, 0x00120011u};
    constexpr std::array<uint32_t, 6> expected = {1, 1, 0, 65535, 17, 18};
    for (uint32_t index = 0; index < expected.size(); ++index) {
        const uint32_t offset = interop::packedIndex16WordOffset(index);
        if ((offset % 4) || offset >= sizeof(words) ||
            interop::unpackIndex16(words[offset / 4], index) != expected[index]) {
            std::fprintf(stderr, "Packed fog index %u decoded incorrectly\n", index);
            return 1;
        }
    }
    std::puts("PACKED_FOG_INDICES_OK");
}
