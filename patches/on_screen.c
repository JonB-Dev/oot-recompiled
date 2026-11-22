// THE ACTORS THAT ASK WHETHER THEY ARE ON SCREEN (2026-10-21, found in the sweep before 0.6.1).
//
// The game sets ACTOR_FLAG_INSIDE_CULLING_VOLUME on every actor every frame from its culling test
// (Actor_DrawAll, by way of Actor_CullingCheck), and decides from it what updates and draws. A few
// actors also READ it, to mean "the player can see me". patches/culling.c widens that test on
// purpose: out to the Render distance, and with traced shadows behind and beside the camera as
// far as the Shadow casters row says (at Everything, without bound), so a thing out of view still
// casts its shadow into the frame. So for those readers the flag stopped meaning what they ask.
//
// THE ONE THAT MATTERS: Dark Link. In the Water Temple's illusion room his spawner waits for Link
// to come near and then spawns him the moment it is OFF screen (z_en_blkobj.c,
// EnBlkobj_SpawnDarkLink), which is the turn of the camera that finds him behind you. With the
// widened test it is never off screen at Everything, and at Near it stays "on screen" within 400
// units behind the camera, so he may never come; the room never clears and the Longshot beyond it
// cannot be reached.
//
// The other reader worth the same answer: Impa leaving the castle courtyard once Link has walked
// past her and she is out of view (z_demo_im.c, func_809869F8).
//
// Left alone, on purpose: the trees a spawner made (z_en_wood02.c), whose going is decided in
// patches/culling.c; a Tektite's attack (z_en_tite.c), which follows the update and so the
// reach like every other actor kept awake for its shadow; a Peahat's dust (cosmetic); and the two
// that only clear the flag (z_en_viewer.c, z_player.c).
//
// THE ANSWER they get is the game's own test, at the frame's real width rather than the console's
// 4:3 (patches/culling.c, culling_on_screen), on the position the game projected this frame: what
// the player can actually see, which is what each of them meant.

#include "patches.h"

#include "actor.h"
#include "play_state.h"
#include "player.h"
#include "regs.h"

// Not z_demo_im.h: it includes the extracted object's header, which is made from the ROM and is
// not in this tree. Impa's check below touches only the Actor her struct begins with.
#include "overlays/actors/ovl_En_Blkobj/z_en_blkobj.h"

// patches/culling.c: the game's own culling test at the frame's width, on this frame's projection.
s32 culling_on_screen(Actor* actor);

// z_en_blkobj.c's own (exported symbols).
void EnBlkobj_SetupAction(EnBlkobj* this, EnBlkobjActionFunc actionFunc);
void EnBlkobj_DarkLinkFight(EnBlkobj* this, PlayState* play);

// @recomp Patched to spawn Dark Link when his spawner is out of the player's view, as the game
// asks, rather than out of the widened culling volume. Reproduced from z_en_blkobj.c.
RECOMP_PATCH void EnBlkobj_SpawnDarkLink(EnBlkobj* this, PlayState* play) {
    if (!culling_on_screen(&this->dyna.actor)) {
        Actor_Spawn(&play->actorCtx, play, ACTOR_EN_TORCH2, this->dyna.actor.world.pos.x, this->dyna.actor.world.pos.y,
                    this->dyna.actor.world.pos.z, 0, this->dyna.actor.yawTowardsPlayer, 0, 0);
        EnBlkobj_SetupAction(this, EnBlkobj_DarkLinkFight);
    }
}

// @recomp Patched the same way: Impa goes once Link is past her and she is out of view.
// Reproduced from z_demo_im.c, where the first parameter is a DemoIm*, whose first member is the
// Actor read here.
RECOMP_PATCH s32 func_809869F8(Actor* thisx, PlayState* play) {
    Player* player = GET_PLAYER(play);
    f32 playerPosX = player->actor.world.pos.x;
    f32 thisPosX = thisx->world.pos.x;

    if ((thisPosX - (kREG(16) + 30.0f) > playerPosX) && !culling_on_screen(thisx)) {
        return true;
    } else {
        return false;
    }
}
