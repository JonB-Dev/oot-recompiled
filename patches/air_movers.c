// THE BODIES THE DUST COLLIDES WITH (2026-10-08).
//
// The dust in the light (renderer patches 0035 to 0042) meets whatever stands or moves in it. It
// began as an air current (the user, 2026-10-08: "as Link runs through it ... the dust would be
// moving along with Link"; "all things that can receive a shadow or cast a shadow should also
// interact with the dust"), and once a reader fix let it reach the dust the user asked for less:
// "Can we just have normal collision? It doesn't have to cause wind gusts or anything like that".
// So this publishes BODIES, not motion: each actor near the eye that has a collision cylinder,
// standing or moving, as the cylinder the game itself collides with. A standing figure keeps the
// dust out of itself as surely as a running one.
//
// The player's cylinder is the one the player's own code sizes every update (Player's `cylinder`,
// its height and lift following the pose); every other actor's is the one its collision check
// info carries. An actor with no cylinder has no body here. The eight nearest the camera's eye are
// written to a table, and the program reads it once a frame (src/game/air_movers.cpp) and carries
// each body forward by its motion between the game's updates, so it moves smoothly in the dust.
// Fixed point, because a float has no agreed register on both sides of this bridge: positions and
// sizes in quarter units, motion in sixty-fourths of a unit an update.
#include "patches.h"
#include "air_movers.h"

#include "actor.h"
#include "play_state.h"
#include "player.h"

void recomp_air_movers(u32 address);

#define AIR_MOVERS_MAX 8
#define AIR_MOVERS_FIELDS 8
#define AIR_MOVERS_TELEPORT 10000.0f // squared units an update: a hundred units is a jump, not a motion
#define AIR_MOVERS_RANGE 2250000.0f  // squared units from the eye: fifteen hundred
#define AIR_MOVERS_MAX_RADIUS 60     // some actors use the cylinder's numbers for other things
#define AIR_MOVERS_MAX_HEIGHT 150

// [0] how many follow, then per body its foot x, y, z and its radius and height (quarter units),
// and its motion this update x, y, z (sixty-fourths of a unit).
static s32 sAirMovers[1 + AIR_MOVERS_MAX * AIR_MOVERS_FIELDS];

// The game's update count when the table was last written. An update that did not move the
// actors (the pause screen) leaves their last motion in prevPos, so the bodies are written still.
static u32 sLastGameplayFrames;

// The cylinder an actor collides with, or false when it has none worth the name.
static s32 AirMovers_Cylinder(Actor* actor, s32* radius, s32* height, s32* lift) {
    if (actor->category == ACTORCAT_PLAYER) {
        Player* player = (Player*)actor;

        *radius = player->cylinder.dim.radius;
        *height = player->cylinder.dim.height;
        *lift = player->cylinder.dim.yShift;
    } else {
        *radius = actor->colChkInfo.cylRadius;
        *height = actor->colChkInfo.cylHeight;
        *lift = actor->colChkInfo.cylYShift;
    }

    return (*radius > 0) && (*height > 0) && (*radius <= AIR_MOVERS_MAX_RADIUS) && (*height <= AIR_MOVERS_MAX_HEIGHT);
}

void AirMovers_Publish(PlayState* play) {
    Actor* nearest[AIR_MOVERS_MAX];
    f32 nearestDistance[AIR_MOVERS_MAX];
    s32 count = 0;
    s32 category;
    s32 moving = ((u32)play->gameplayFrames != sLastGameplayFrames);
    s32 i;

    sLastGameplayFrames = (u32)play->gameplayFrames;
    for (category = 0; category < ACTORCAT_MAX; category++) {
        Actor* actor;

        for (actor = play->actorCtx.actorLists[category].head; actor != NULL; actor = actor->next) {
            f32 ex;
            f32 ey;
            f32 ez;
            f32 distance;
            s32 radius;
            s32 height;
            s32 lift;
            s32 slot;

            if (!AirMovers_Cylinder(actor, &radius, &height, &lift)) {
                continue;
            }

            ex = actor->world.pos.x - play->view.eye.x;
            ey = actor->world.pos.y - play->view.eye.y;
            ez = actor->world.pos.z - play->view.eye.z;
            distance = (ex * ex) + (ey * ey) + (ez * ez);
            // The player always, and first, whatever stands nearer the eye.
            if (actor->category == ACTORCAT_PLAYER) {
                distance = -1.0f;
            } else if (distance >= AIR_MOVERS_RANGE) {
                continue;
            }

            // Kept nearest first: the new one goes where it belongs and the farthest falls off.
            slot = count;
            while ((slot > 0) && (nearestDistance[slot - 1] > distance)) {
                if (slot < AIR_MOVERS_MAX) {
                    nearest[slot] = nearest[slot - 1];
                    nearestDistance[slot] = nearestDistance[slot - 1];
                }
                slot--;
            }

            if (slot < AIR_MOVERS_MAX) {
                nearest[slot] = actor;
                nearestDistance[slot] = distance;
                if (count < AIR_MOVERS_MAX) {
                    count++;
                }
            }
        }
    }

    sAirMovers[0] = count;
    for (i = 0; i < count; i++) {
        Actor* actor = nearest[i];
        s32* entry = &sAirMovers[1 + i * AIR_MOVERS_FIELDS];
        f32 dx = actor->world.pos.x - actor->prevPos.x;
        f32 dy = actor->world.pos.y - actor->prevPos.y;
        f32 dz = actor->world.pos.z - actor->prevPos.z;
        s32 radius;
        s32 height;
        s32 lift;

        AirMovers_Cylinder(actor, &radius, &height, &lift);
        if (!moving || (((dx * dx) + (dy * dy) + (dz * dz)) >= AIR_MOVERS_TELEPORT)) {
            dx = 0.0f;
            dy = 0.0f;
            dz = 0.0f;
        }

        entry[0] = (s32)(actor->world.pos.x * 4.0f);
        entry[1] = (s32)((actor->world.pos.y + (f32)lift) * 4.0f);
        entry[2] = (s32)(actor->world.pos.z * 4.0f);
        entry[3] = radius * 4;
        entry[4] = height * 4;
        entry[5] = (s32)(dx * 64.0f);
        entry[6] = (s32)(dy * 64.0f);
        entry[7] = (s32)(dz * 64.0f);
    }

    recomp_air_movers((u32)sAirMovers);
}
