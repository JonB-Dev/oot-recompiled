#pragma once

#include <string>

// The debug menu's model: every place a player can warp to, nested by location, and the walk
// down through it.
//
// THE LIST IS GENERATED, NOT TYPED. src/game/warp_table.ntsc-1.0.h comes from the decompilation's
// own entrance table by way of tools/gen_warps.py, and that tool refuses to produce anything if a
// single scene in that table has no place in its region map. So "every single one", which is what
// was asked for, is a property of the build rather than a claim: 15 regions, 102 places, 357
// entrances.
//
// THE WALK IS A DRILL DOWN RATHER THAN A TREE THAT OPENS. Three hundred and fifty seven rows in
// one scrolling list is not a menu anybody can use with a pad, and an expanding tree needs its own
// focus model. A drill down reuses the row list this interface already has: regions, then the
// places in one region, then the doors into one place. Escape and the pad's B climb back out, and
// the level above remembers where you were standing.
//
// This holds no RmlUi state, exactly as ui_controls does, so the menu's behavior can be read
// without a renderer. Called on the render thread by the shell. Warping goes through
// game_state::set_pending_warp, which is the path the harness's --warp already takes; there is no
// second mechanism.
namespace oot::ui::warps {

    enum class Level {
        Regions,    // the 15 regions
        Places,     // the places in the chosen region
        Entrances,  // the doors into the chosen place, when it has more than one
    };

    Level level();

    // "Kakariko Village" while in a region, "Kakariko Village / Graveyard" while in a place, and
    // empty at the top. The lid shows it so a person always knows where in the nesting they are.
    std::string trail();

    int row_count();
    std::string row_label(int index);

    // The right hand column: how many places a region holds, how many ways into a place, or an
    // entrance's own index in the game's numbering. The index is shown because it is the number
    // --warp takes, so a place found by hand here can be reached again from the command line.
    std::string row_value(int index);

    // True when this row is where the game is right now, from the live entrance index. The
    // marking is the answer to "which of these am I in", which is otherwise guesswork once you
    // have warped a few times.
    bool row_is_here(int index);

    enum class Action {
        Nothing,    // nothing happened; the row was not one that does anything
        Descended,  // the level changed; the shell rebuilds the rows
        Warped,     // a warp was requested; the shell closes the menu
    };

    // Enter, or a click. A region and a multi-door place descend; a place with one way in warps
    // straight off, because making somebody open a list of one is a worse menu.
    Action activate(int index);

    // Escape, the pad's B, or left. False when already at the top, which is the shell's signal to
    // close the document rather than climb further.
    bool ascend();

    // Which row the focus is on at this level, remembered per level so climbing back out puts you
    // where you were rather than at the top of the list.
    int focus();
    void set_focus(int index);

    // Back to the regions with every focus cleared. Called when the document opens, so the menu
    // never reopens halfway down a branch the person has forgotten about.
    void reset();

    // Re-read where the game is, for row_is_here. Called when the rows are rebuilt rather than
    // every frame: it reads emulated memory, and the answer only changes on a scene transition.
    void refresh_here();

} // namespace oot::ui::warps
