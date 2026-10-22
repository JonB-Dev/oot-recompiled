// The letterbox bars, handed to the program so they can move at the rate the picture is presented
// (upgrades, the camera work; the user, 2026-09-24: "the black bar at top and bottom of screen
// animates in very very chappy rather than smooth when z targeting").
//
// WHY THE GAME CANNOT DO THIS ITSELF. The bars are not drawn. The game SCISSORS the picture:
// View_ApplyLetterbox takes Letterbox_GetSize() and emits one gDPSetScissor for the opaque list
// and one for the translucent, and everything outside that rectangle is simply not drawn, which
// is what reads as a black bar. The size steps in shrink_window.c by thirty over the update rate,
// which is ten console pixels per game update, and a game update happens twenty times a second
// however often the picture is presented. So a bar coming in to thirty two pixels takes four
// steps and shows four positions, and at sixty or a hundred and forty four frames a second every
// one of them is held for several frames. No amount of work inside the game moves it more often,
// because the display list carrying that scissor is built once per update.
//
// SO THE PROGRAM DRAWS THEM. This patch publishes the size the game wants and holds the scissor
// still at the full picture; the program eases its own pair of bars toward that size every frame
// it presents (src/ui/ui_shell.cpp), which is as smooth as the display allows. The picture behind
// them is drawn in full and covered, rather than clipped, which looks the same and costs nothing.
//
// The Letterbox bars row turns this off, and then the scissor is the game's own again.

#include "patches.h"

#include "gfx.h"
#include "letterbox.h"
#include "view.h"
#include "z_lib.h"

// The program's side (src/game/recomp_api.cpp). The size the game wants, in console pixels out of
// the picture's two hundred and forty, and the answer is whether the program is drawing the bars
// itself: 1 to leave the scissor alone, 0 to let the game scissor as it always did.
DECLARE_FUNC(s32, recomp_letterbox, u32 size);

// @recomp The game's own function, with the size published and, while the program draws the bars,
// held at zero so the scissor no longer steps.
RECOMP_PATCH void View_ApplyLetterbox(View* view) {
    GraphicsContext* gfxCtx = view->gfxCtx;
    s32 pillarboxSize;
    s32 letterboxSize;
    s32 ulx;
    s32 uly;
    s32 lrx;
    s32 lry;

    letterboxSize = Letterbox_GetSize();

    // @recomp Publish it, and take it out of the scissor when the program has the bars.
    if (recomp_letterbox((u32)letterboxSize) != 0) {
        letterboxSize = 0;
    }

    pillarboxSize = 0;

    if (letterboxSize < 0) {
        letterboxSize = 0;
    }
    if (letterboxSize > SCREEN_HEIGHT / 2) {
        letterboxSize = SCREEN_HEIGHT / 2;
    }

    ulx = view->viewport.leftX + pillarboxSize;
    uly = view->viewport.topY + letterboxSize;
    lrx = view->viewport.rightX - pillarboxSize;
    lry = view->viewport.bottomY - letterboxSize;

    OPEN_DISPS(gfxCtx, "../z_view.c", 459);

    gDPPipeSync(POLY_OPA_DISP++);
    gDPSetScissor(POLY_OPA_DISP++, G_SC_NON_INTERLACE, ulx, uly, lrx, lry);
    gDPPipeSync(POLY_XLU_DISP++);
    gDPSetScissor(POLY_XLU_DISP++, G_SC_NON_INTERLACE, ulx, uly, lrx, lry);

    CLOSE_DISPS(gfxCtx, "../z_view.c", 472);
}
