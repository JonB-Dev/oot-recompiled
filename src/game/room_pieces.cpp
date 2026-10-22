#include "game/room_pieces.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>

namespace {

    std::mutex g_mutex;
    int g_scene = -1;
    int g_current = -1;
    int g_previous = -1;
    // Every room's pieces as last listed, by room number, while the scene lasts.
    std::map<int, std::vector<oot::room_pieces::Piece>> g_pieces;
    // The switches, by scene, room and offset; kept for as long as the program runs.
    std::map<uint64_t, int> g_states;
    uint32_t g_generation = 1;

    uint64_t key(int scene, int room, uint32_t offset) {
        return (static_cast<uint64_t>(static_cast<uint16_t>(scene)) << 40) |
               (static_cast<uint64_t>(static_cast<uint8_t>(room)) << 32) | offset;
    }

} // namespace

namespace oot::room_pieces {

    void begin(int scene, int room) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (scene != g_scene) {
            g_scene = scene;
            g_pieces.clear();
        }
        g_pieces[room].clear();
    }

    void add(int room, uint32_t offset, uint32_t flags, uint32_t counts) {
        std::lock_guard<std::mutex> lock(g_mutex);
        Piece piece;
        piece.room = room;
        piece.offset = offset;
        piece.translucent = (flags & 1u) != 0;
        piece.depth = static_cast<int>((flags >> 8) & 0xFFu);
        piece.vertices = static_cast<int>(counts & 0xFFFFu);
        piece.white = static_cast<int>(counts >> 16);
        g_pieces[room].push_back(piece);
    }

    uint32_t present(int scene, int current, int previous) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if ((scene != g_scene) || (current != g_current) || (previous != g_previous)) {
            g_scene = scene;
            g_current = current;
            g_previous = previous;
            std::fprintf(stderr, "[pieces] scene %d, rooms loaded %d and %d\n", scene, current, previous);
        }
        return g_generation;
    }

    int state(int room, uint32_t offset) {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto found = g_states.find(key(g_scene, room, offset));
        return (found != g_states.end()) ? found->second : Shown;
    }

    std::vector<Piece> list() {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<Piece> out;
        for (int room : { g_current, g_previous }) {
            if (room < 0) {
                continue;
            }
            const auto found = g_pieces.find(room);
            if (found != g_pieces.end()) {
                out.insert(out.end(), found->second.begin(), found->second.end());
            }
            if (g_current == g_previous) {
                break;
            }
        }
        return out;
    }

    int state_of(const Piece& piece) {
        return state(piece.room, piece.offset);
    }

    void set_state(const Piece& piece, int value) {
        std::lock_guard<std::mutex> lock(g_mutex);
        const int clamped = std::clamp(value, 0, STATE_COUNT - 1);
        g_states[key(g_scene, piece.room, piece.offset)] = clamped;
        g_generation++;
        std::fprintf(stderr, "[pieces] scene %d room %d list 0x%06X: %s\n", g_scene, piece.room, piece.offset,
                     state_label(clamped).c_str());
    }

    std::string state_label(int value) {
        switch (value) {
            case PaintOut: return "Paint taken out";
            case Hidden:   return "Hidden";
            default:       return "Shown";
        }
    }

} // namespace oot::room_pieces
