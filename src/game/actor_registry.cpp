#include "game/actor_registry.h"

#include <cstdio>
#include <unordered_map>

namespace {

    // The game's own ceiling on live actors is a few hundred. Four times that leaves a leak
    // visible in the high water mark long before it is harmful.
    constexpr uint32_t CAPACITY = 1024;

    // Where an actor can live: the cached view of the console's memory. The same plausibility
    // bound dl_check uses, for the same reason: our runtime maps far more than the console had,
    // so "is it mapped" is the wrong question.
    constexpr uint32_t RAM_BASE = 0x80000000u;
    constexpr uint32_t RAM_SIZE = 0x00800000u;

    // Address to spawn index. A map rather than a flat table because the tagging patches ask for
    // an actor's index every time it is drawn, several hundred times a frame, and a linear search
    // of a thousand entries for each of those is the kind of cost that hides until a busy scene.
    std::unordered_map<uint32_t, uint32_t> g_index;
    uint32_t g_high_water = 0;
    uint32_t g_next_index = 1;      // 0 means "no identity"
    uint32_t g_rejected = 0;        // implausible addresses
    uint32_t g_missed = 0;          // releases and lookups of addresses that were not registered
    bool g_reported_full = false;

    bool plausible(uint32_t address) {
        return address >= RAM_BASE && address < RAM_BASE + RAM_SIZE && (address & 3u) == 0;
    }

} // namespace

namespace oot::actor_registry {

    uint32_t register_actor(uint32_t actor_address) {
        if (!plausible(actor_address)) {
            ++g_rejected;
            return 0;
        }

        // Registered twice without a release in between: keep the identity it already has.
        // The game does not do this, but the honest answer if it did is "the same actor".
        const auto existing = g_index.find(actor_address);
        if (existing != g_index.end()) {
            return existing->second;
        }

        if (g_index.size() >= CAPACITY) {
            if (!g_reported_full) {
                g_reported_full = true;
                std::fprintf(stderr, "[registry] FULL at %u live actors; further actors draw untagged until the next scene\n",
                             static_cast<unsigned>(g_index.size()));
            }
            return 0;
        }

        const uint32_t index = g_next_index++;
        g_index.emplace(actor_address, index);
        if (g_index.size() > g_high_water) {
            g_high_water = static_cast<uint32_t>(g_index.size());
        }
        return index;
    }

    uint32_t lookup(uint32_t actor_address) {
        const auto it = g_index.find(actor_address);
        if (it == g_index.end()) {
            ++g_missed;
            return 0;
        }
        return it->second;
    }

    void release_actor(uint32_t actor_address) {
        if (g_index.erase(actor_address) == 0) {
            ++g_missed;
        }
    }

    void reset() {
        std::fprintf(stderr, "[registry] live %u high water %u rejected %u missed %u\n",
                     static_cast<unsigned>(g_index.size()), g_high_water, g_rejected, g_missed);
        g_index.clear();
        g_high_water = 0;
        g_rejected = 0;
        g_missed = 0;
        g_reported_full = false;
        // The index keeps counting across scenes on purpose: an ID must never repeat while the
        // renderer could still hold the previous frame's transform for it.
    }

    uint32_t live_count() {
        return static_cast<uint32_t>(g_index.size());
    }

    uint32_t high_water() {
        return g_high_water;
    }

    int self_test() {
        int failures = 0;
        g_index.reserve(CAPACITY);
        reset();

        // Implausible addresses are refused without taking a slot.
        if (register_actor(0x00000000u) != 0 || register_actor(0x80000002u) != 0 ||
            register_actor(0x80800000u) != 0) {
            std::fprintf(stderr, "[registry] self test: an implausible address was accepted\n");
            ++failures;
        }

        // Fill to capacity: every index distinct, non zero, and found again by lookup.
        uint32_t last = 0;
        for (uint32_t i = 0; i < CAPACITY; ++i) {
            const uint32_t address = RAM_BASE + 0x100000u + i * 0x200u;
            const uint32_t index = register_actor(address);
            if (index == 0 || index <= last || lookup(address) != index) {
                std::fprintf(stderr, "[registry] self test: bad index %u after %u\n", index, last);
                ++failures;
                break;
            }
            last = index;
        }
        if (live_count() != CAPACITY || high_water() != CAPACITY) {
            std::fprintf(stderr, "[registry] self test: live %u high water %u, wanted %u\n", live_count(),
                         high_water(), CAPACITY);
            ++failures;
        }

        // One past capacity: refused, and FULL reported exactly once even when asked again.
        if (register_actor(RAM_BASE + 0x700000u) != 0 || register_actor(RAM_BASE + 0x700200u) != 0) {
            std::fprintf(stderr, "[registry] self test: an actor was accepted past capacity\n");
            ++failures;
        }

        // Release one, and the slot comes back with a fresh, still increasing index.
        release_actor(RAM_BASE + 0x100000u);
        if (live_count() != CAPACITY - 1 || lookup(RAM_BASE + 0x100000u) != 0) {
            std::fprintf(stderr, "[registry] self test: release did not free the slot\n");
            ++failures;
        }
        const uint32_t again = register_actor(RAM_BASE + 0x100000u);
        if (again <= last) {
            std::fprintf(stderr, "[registry] self test: a reused slot repeated an index\n");
            ++failures;
        }

        // A release of something never registered is counted, not fatal.
        release_actor(RAM_BASE + 0x7FFFF0u);

        reset();
        if (live_count() != 0) {
            std::fprintf(stderr, "[registry] self test: reset left %u live\n", live_count());
            ++failures;
        }

        std::fprintf(stderr, "[registry] self test %s\n", failures == 0 ? "passed" : "FAILED");
        return failures == 0 ? 0 : 1;
    }

} // namespace oot::actor_registry
