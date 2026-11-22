#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The painted elements of the rooms loaded where the player is, and a switch for each (2026-10-08,
// the user: "I can actually just go down the list and turn on and off a painted element within
// the place that I'm currently at"). patches/room_pieces.c lists every display list of every
// loaded room here when the room is loaded, and asks for each one's switch whenever a switch moves;
// the Debugging menu lists them and moves the switches.
//
// The switches are kept by scene, room and the list's place in the room's file, so a room loaded
// again comes back as it was left, for as long as the program runs. Nothing is saved: these are
// tools for finding what draws something, not settings.
namespace oot::room_pieces {

    enum State : int {
        Shown = 0,      // as the game draws it
        PaintOut = 1,   // its vertices painted white take the color of those around them
        Hidden = 2,     // not drawn, nor anything it calls
    };

    constexpr int STATE_COUNT = 3;

    struct Piece {
        int room = 0;
        uint32_t offset = 0;     // the list's place in the room's file
        bool translucent = false;
        int depth = 0;           // 0 a list the room's shape names, 1 one it calls, and so on
        int vertices = 0;        // loaded by this list itself
        int white = 0;           // of those, painted white
    };

    // From the game thread (the patch's exports).
    void begin(int scene, int room);
    void add(int room, uint32_t offset, uint32_t flags, uint32_t counts);
    uint32_t present(int scene, int current, int previous);
    int state(int room, uint32_t offset);

    // From the menu: the pieces of the rooms loaded now, in the order the game draws them.
    std::vector<Piece> list();
    int state_of(const Piece& piece);
    void set_state(const Piece& piece, int state);
    std::string state_label(int state);

} // namespace oot::room_pieces
