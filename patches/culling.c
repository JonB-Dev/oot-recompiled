// The game's own visibility tests, widened to the frame the renderer actually draws and pushed
// out to the distance the scene's own fog admits. Two reports, one file, because both are decided
// by the same two functions and only one patch may replace a function.
//
//   SIDEWAYS  things vanish a little way in from the left and right edges of a wide frame
//   FORWARD   things pop in and out in the distance
//
// The sideways half is immediately below; the forward half is under "HOW FAR AHEAD THINGS ARE
// DRAWN", further down.
//
// ---------------------------------------------------------------------------------------------
//
// THE FIRST BUG, reported by the user on 2026-09-20: "Adjust disappearing objects when using
// wider aspect ratios during normal gameplay". Trees, bushes, chests, enemies and signs vanish a
// little way in from the left and right edges and reappear as the camera turns, and the wider the
// window the further in the edge sits.
//
// WHY IT HAPPENS. The game decides what to update and draw by projecting each actor through its
// OWN view projection matrix, which is the console's 4:3 one, and asking whether the result lands
// inside the frustum. Widening the picture does not touch that matrix: the renderer widens every
// projection at draw time (RT64's aspectRatioScale, from the window's aspect over 4:3), so the
// frame shows a band of world at each side that the game has already decided is off screen and
// culled. The game is not wrong about its own 4:3 view. It is answering a question about a
// narrower frame than the one in front of the player.
//
// THE FIX is to ask the question about the real frame: multiply the horizontal half width of
// every such test by how much wider the frame is, which is exactly what
// `recomp_wide_frame_scale_q16` already reports for the 2D transitions (phase 43). At the
// console's 4:3 the factor is exactly one and every test below is the game's own, bit for bit.
// The vertical tests are untouched on purpose: the renderer widens, it does not heighten, so the
// vertical field of view is unchanged and the game's own vertical bound is already right.
//
// WHY NOT SIMPLY DISABLE FRUSTUM CULLING, which is what the reference project does for the other
// game. Because it costs what it costs: every actor within the distance limit would update and
// draw every frame, including the ones behind the camera, and this project is fixing a stutter in
// the same session. Scaling the bound adds exactly the actors the player can now see and not one
// more. The two approaches agree everywhere the player can see; they differ only in how much
// invisible work is done.
//
// THE TESTS COVERED, which is every one in this revision that compares a projected x against the
// frustum (found by reading every use of `projectedW`, `invW` and `cappedInvW` in the
// decompilation):
//
//   Actor_CullingVolumeTest   the general one, for every actor's update and draw
//   EnWood02_SpawnZoneCheck   the tree and bush spawner's own copy, which decides whether a
//                             spawned tree exists at all rather than only whether it draws.
//                             Hyrule Field is made of these, and it is where the user was.
//
// Lights_GlowCheck (the point light glows on torches) is NOT here and that is deliberate: its
// horizontal test guards an index into the game's own z buffer, sized for 320 columns, so
// widening the test alone would read outside it. It is recorded in issues.md as its own job.

#include "patches.h"
#include "actor_note.h"

#include "actor.h"
#include "play_state.h"
#include "light.h"
#include "skin_matrix.h"
#include "sys_math.h"
#include "z_lib.h"

#include "overlays/actors/ovl_En_Wood02/z_en_wood02.h"
#include "overlays/actors/ovl_Obj_Mure2/z_obj_mure2.h"
#include "cutscene.h"

// How much wider than 4:3 the frame is, 16.16 fixed point, exactly 65536 at 4:3. Native, through
// the dummy address in syms.ld. See src/game/recomp_api.cpp.
DECLARE_FUNC(u32, recomp_wide_frame_scale_q16, void);

// Whether the traced shadows stand (src/game/recomp_api.cpp): 1 at lighting level 1 and above.
// With them on, the sideways bound and the near side of the actor tests reach further than
// the frame (below), so an actor just out of view still casts its shadow into it and shows in
// the water; an actor culled is not in the traced scene at all (2026-09-24).
s32 recomp_traced_shadows(void);

// HOW FAR THE REACH GOES IS THE SHADOW CASTERS ROW of the lighting menu (the user, 2026-09-24,
// Kakariko's windmill: "shadows are disappearing ... add a setting to remove it or reduce it").
// A tall thing's shadow reaches far further than its own culling volume, so as the camera turns
// the windmill leaves the widened frame, the game culls it, it leaves the traced scene, and the
// shadow it threw across the village goes with it. The row: 0 the game's own culling (the
// reaches below all zero), 1 near (the day's builds: half the frame again each side, 400 units
// behind, room pieces 1200 behind), 2 far, 3 everything (no frustum test at all for actors, and
// every room piece kept; the distance limit still holds). Read once a frame, below.
DECLARE_FUNC(s32, recomp_caster_reach, void);
static const f32 sCasterSideReach[4]   = { 1.0f, 1.5f, 2.5f, 1000.0f };
static const f32 sCasterBehindReach[4] = { 0.0f, 400.0f, 1500.0f, 1.0e9f };
static const f32 sCasterRoomReach[4]   = { 0.0f, 1200.0f, 3000.0f, 1.0e9f };
static s32 sCasterReach = 1;

static s32 caster_reach(void) {
    return recomp_traced_shadows() ? sCasterReach : 0;
}

// For patches/room_behind.c: how far behind the camera a room piece is kept.
f32 culling_room_behind_reach(void) {
    return sCasterRoomReach[caster_reach()];
}

static f32 wide_frame_scale(void) {
    return (f32)recomp_wide_frame_scale_q16() * (1.0f / 65536.0f) * sCasterSideReach[caster_reach()];
}

// THE SAME REACH UP AND DOWN (2026-09-24, the user on 0.3.1 with the row at Everything: "still
// some sort of culling for shadows in the recording?"). The row widened the sideways bound and
// the near side and left the vertical pair at the frame's own edges, so a torch on a wall above
// the view, or a thing below a ledge, was still culled and took its light and its shadow with
// it. One is the game's own bound, which is what the row's first option restores.
static f32 vertical_reach(void) {
    return sCasterSideReach[caster_reach()];
}

static f32 behind_reach(void) {
    return sCasterBehindReach[caster_reach()];
}

// THE SPAWNERS LEAVE ROOM FOR THE GAME'S OWN ACTORS (2026-10-12). The game refuses to spawn
// anything once its scene holds more than 200 actors (Actor_Spawn, ACTOR_NUMBER_MAX). The
// reaches above and the Render distance row make the ring and tree spawners fill Hyrule Field
// with their shrubs, rocks and trees, out to the fog and all round the camera, where the game
// kept them within about 1600 units in front of it (z_en_wood02.c's own note: "Presumably done
// this way to keep memory usage down in Hyrule Field"). Its 73 placed actors, 7 rings of eight
// to twelve and 27 tree spawners of up to five reach past 200, and then whatever the game spawns
// in play is refused without a word. The Sun's Song is one such thing: its effect never comes,
// so the song never ends and Link stands holding his instrument while the clock creeps on at its
// own pace (the user, 2026-10-12: "it was also removed from sun song animation which has made it
// not work and instead completelyt freezes the game entirely"; "link just stood there ... as if
// the animation was still happening but never finished").
//
// So a spawner reaches past the game's own reach only while the scene stays at or under
// SPAWN_ROOM_SPARE actors once it has spawned, and past SPAWN_ROOM_FULL what was spawned beyond
// the game's reach is given back, so the game always has room to spawn. Built with a second
// candidate (the spawners at the game's own reach) and a guard on the song as Debugging switches;
// this one alone was on, and the user confirmed it: "so is sun sung fix!" (2026-10-12).
#define SPAWN_ROOM_SPARE 170
#define SPAWN_ROOM_FULL 185

// AND THE MEMORY, NOT ONLY THE COUNT (2026-10-21, patches/actor_heap.c). In the user's Hyrule
// Field the count stayed under 170 while the actor heap ran out (its largest free block down to
// 96 bytes), and the arrow and the hookshot's hook were refused for want of memory. The heap is
// far bigger now (patches/actor_heap.c), and as the net under it a spawner also keeps to the
// game's own reach while the heap's largest free block is under SPAWN_MEMORY_SPARE, and gives
// back what it put beyond it under SPAWN_MEMORY_FULL. The spare is room for the largest things
// the game spawns in play, a horse or an owl and their code. Measured once an update
// (patches/actor_note.c).
#define SPAWN_MEMORY_SPARE 0xC000
#define SPAWN_MEMORY_FULL 0x8000

static s32 memory_short(u32 below) {
    return ActorNote_LargestFree() < below;
}

// Whether a spawner wanting `wanted` more actors keeps to the game's own reach this frame.
static s32 spawner_keeps_to_game(PlayState* play, s32 wanted) {
    return ((play->actorCtx.total + wanted) > SPAWN_ROOM_SPARE) || memory_short(SPAWN_MEMORY_SPARE);
}

// Whether what a spawner put beyond the game's own reach is given back this frame.
static s32 spawner_gives_back(PlayState* play) {
    return (play->actorCtx.total > SPAWN_ROOM_FULL) || memory_short(SPAWN_MEMORY_FULL);
}

// How much wider than 4:3 the frame is, with none of the casters' reach.
static f32 frame_width_scale(void) {
    return (f32)recomp_wide_frame_scale_q16() * (1.0f / 65536.0f);
}

// The game's own culling test, but for the frame's width.
static s32 culling_game_test(Actor* actor, Vec3f* projPos, f32 projW) {
    f32 invW;

    if ((projPos->z > -actor->cullingVolumeScale) &&
        (projPos->z < (actor->cullingVolumeDistance + actor->cullingVolumeScale))) {
        invW = (projW < 1.0f) ? 1.0f : 1.0f / (f32)projW;
        if ((((fabsf(projPos->x) - actor->cullingVolumeScale) * invW) < frame_width_scale()) &&
            (((projPos->y + actor->cullingVolumeDownward) * invW) > -1.0f) &&
            (((projPos->y - actor->cullingVolumeScale) * invW) < 1.0f)) {
            return true;
        }
    }
    return false;
}

// Whether the player can see the actor: the game's own test at the frame's width, on the position
// the game projected this frame (Actor_DrawAll). For the actors that read the culling flag to mean
// "on screen", which the widened test above no longer answers (patches/on_screen.c, 2026-10-21).
s32 culling_on_screen(Actor* actor) {
    return culling_game_test(actor, &actor->projectedPos, actor->projectedW);
}

// A tree a spawner made (z_en_wood02.c: WOOD_SPAWN_SPAWNED).
static s32 spawned_tree(Actor* actor) {
    return (actor->id == ACTOR_EN_WOOD02) && (((EnWood02*)actor)->spawnType == 1);
}

// ---------------------------------------------------------------------------------------------
// HOW FAR AHEAD THINGS ARE DRAWN (the user, 2026-09-20: "there's popping trees and bushes and
// grass and rocks that are off in the distance that pop in and out. We just need to increase the
// render distance so that stuff basically renders a lot sooner or you know a lot further away and
// doesn't pop in like that").
//
// Each actor carries its own forward limit, `cullingVolumeDistance`: a thousand units by default,
// four thousand for the tree and bush spawner. Those numbers were chosen for a console with four
// megabytes of memory drawing to a 320 by 240 screen, and on a large sharp screen the moment a
// thing crosses that line is plainly visible.
//
// THE LIMIT IS THE SCENE'S OWN FOG, NOT A NUMBER PICKED HERE. `play->lightCtx.zFar` is what the
// game itself calls the draw distance, the depth at which its fog becomes opaque, and it is set
// per scene and per time of day (up to ENV_ZFAR_MAX, 12800). Drawing anything beyond it is work
// spent on what the fog already hides, and stopping short of it is what produces the popping. So
// each actor's own distance is scaled up and then clamped there.
//
// AND NEVER SHORTENED. If a scene's fog is close (indoors, or weather), zFar can be SMALLER than
// what an actor asked for, and clamping to it would cull things the game used to show, which
// would be a worse bug than the one being fixed and would appear only in particular rooms. The
// original distance is the floor.
//
// THE SCALE IS A MEASURED NUMBER, NOT A TASTE. The cost of raising it grows with the AREA the
// player can see, roughly as its square, and this project fixed a frame pacing defect the same
// morning. Measured in Hyrule Field, with the actors the renderer is handed per frame and the
// frames actually presented per second:
//
//   scale   actor groups per frame   frames short
//   (none)                    52.1          0.02%
//   3                        291.7          0.14%   <- this
//   8                        500.0          0.45%   too far: as costly as the stutter that was fixed
//
// 0.45 percent is the same order as the stutter the user reported and had fixed (0.20 to 0.58),
// so eight is not a setting, it is a regression wearing a feature's clothes.
//
// THREE IS WHERE IT STOPS PAYING, and the reason is the clamp rather than the multiplier. The
// tree and bush spawner asks for 4000, so at three it reaches 12000 and Hyrule Field's fog ends
// at 12800: trees, which are the most visible thing that pops, are already drawing as far as
// they can possibly be seen. Everything above three buys nothing for them and only pushes the
// small scenery (a thousand units by default) further out, which is where the extra two hundred
// actors a frame at eight come from. Most of the visible benefit, a fifth of the cost.
// The scale is the SETTINGS ROW now, not a constant here (the user's ask of 2026-09-20: "give
// the option for that distance so that the user can actually open the settings and enable it to
// go out farther or less ... based on what their PC can handle"). The row's options and the
// multiples they mean live together in src/ui/ui_settings.cpp so labels and numbers cannot drift.
DECLARE_FUNC(u32, recomp_render_distance_q16, void);

// READ ONCE A FRAME, NOT ONCE PER ACTOR. The test below runs for every actor of every frame, and
// hundreds of calls across the native bridge per frame is a cost this project measured and would
// rather not pay; `culling_frame_begin` refreshes this from Graph_Update, where the prerendered
// room check already runs. It starts at the default the measurements chose, so the very first
// frame, before any refresh, behaves as the shipped default rather than as no scaling at all.
static f32 sRenderDistanceScale = 3.0f;

void culling_frame_begin(void) {
    sRenderDistanceScale = (f32)recomp_render_distance_q16() * (1.0f / 65536.0f);
    if (sRenderDistanceScale < 1.0f) {
        sRenderDistanceScale = 1.0f;
    }
    sCasterReach = recomp_caster_reach();
    if (sCasterReach < 0) {
        sCasterReach = 0;
    }
    if (sCasterReach > 3) {
        sCasterReach = 3;
    }
}

static f32 render_distance(PlayState* play, f32 asked) {
    f32 limit = asked * sRenderDistanceScale;
    f32 sceneFar = (f32)play->lightCtx.zFar;

    if (limit > sceneFar) {
        limit = sceneFar;
    }
    if (limit < asked) {
        limit = asked;
    }
    return limit;
}

// @recomp Patched to test against the frame's real width rather than the console's 4:3.
// Reproduced from the decompilation's z_actor.c with the horizontal bound scaled.
RECOMP_PATCH s32 Actor_CullingVolumeTest(PlayState* play, Actor* actor, Vec3f* projPos, f32 projW) {
    f32 invW;

    // @recomp A spawned tree outside the game's own reach goes when the spawners give back
    // (2026-10-12, above): EnWood02_Update removes a spawned tree that leaves its culling volume.
    if (spawned_tree(actor) && spawner_gives_back(play)) {
        return culling_game_test(actor, projPos, projW);
    }

    // @recomp Behind the camera as well while the lighting is traced (its shadow reaches into
    // the frame); the game's own near side otherwise.
    if ((projPos->z > -actor->cullingVolumeScale - behind_reach()) &&
        // @recomp The forward limit only: as far as the scene's own fog, never less than the game
        // asked for. The near side is untouched.
        (projPos->z < (render_distance(play, actor->cullingVolumeDistance) + actor->cullingVolumeScale))) {
        // Clamping `projW` affects points behind the camera, so that the culling volume has
        // a frustum shape in front of the camera and a box shape behind the camera.
        invW = (projW < 1.0f) ? 1.0f : 1.0f / (f32)projW;

        // @recomp The horizontal bound only. 1.0 is the edge of the console's frame in projected
        // space; the frame the player sees reaches this much further.
        if ((((fabsf(projPos->x) - actor->cullingVolumeScale) * invW) < wide_frame_scale()) &&
            (((projPos->y + actor->cullingVolumeDownward) * invW) > -vertical_reach()) &&
            (((projPos->y - actor->cullingVolumeScale) * invW) < vertical_reach())) {
            return true;
        }
    }

    return false;
}

// @recomp Patched the same way. This one decides whether a tree or bush is SPAWNED, so without it
// the trees at the sides of a wide frame in Hyrule Field are not merely undrawn, they do not
// exist. Reproduced from the decompilation's z_en_wood02.c.
RECOMP_PATCH s32 EnWood02_SpawnZoneCheck(EnWood02* this, PlayState* play, Vec3f* pos) {
    f32 phi_f12;

    SkinMatrix_Vec3fMtxFMultXYZW(&play->viewProjectionMtxF, pos, &this->actor.projectedPos, &this->actor.projectedW);

    phi_f12 = ((this->actor.projectedW == 0.0f) ? 1000.0f : fabsf(1.0f / this->actor.projectedW));

    // @recomp The game's own reach, at the frame's width, while the spawners keep to it
    // (2026-10-12, "THE SPAWNERS LEAVE ROOM" above). A spawner adds one tree at a time.
    if (spawner_keeps_to_game(play, 1)) {
        return (-this->actor.cullingVolumeScale < this->actor.projectedPos.z) &&
               (this->actor.projectedPos.z < (this->actor.cullingVolumeDistance + this->actor.cullingVolumeScale)) &&
               (((fabsf(this->actor.projectedPos.x) - this->actor.cullingVolumeScale) * phi_f12) < frame_width_scale()) &&
               (((this->actor.projectedPos.y + this->actor.cullingVolumeDownward) * phi_f12) > -1.0f) &&
               (((this->actor.projectedPos.y - this->actor.cullingVolumeScale) * phi_f12) < 1.0f);
    }

    // @recomp Behind the camera as well while the row asks for it: a tree behind the player
    // still throws its shadow forward into the frame, and this test decides whether it exists.
    if ((-this->actor.cullingVolumeScale - behind_reach() < this->actor.projectedPos.z) &&
        // @recomp The same forward limit. This one decides whether a tree EXISTS, so it is what
        // stops a line of them appearing out of nothing as the player walks toward it.
        (this->actor.projectedPos.z <
         (render_distance(play, this->actor.cullingVolumeDistance) + this->actor.cullingVolumeScale)) &&
        // @recomp The horizontal bound only, as above.
        (((fabsf(this->actor.projectedPos.x) - this->actor.cullingVolumeScale) * phi_f12) < wide_frame_scale()) &&
        (((this->actor.projectedPos.y + this->actor.cullingVolumeDownward) * phi_f12) > -vertical_reach()) &&
        (((this->actor.projectedPos.y - this->actor.cullingVolumeScale) * phi_f12) < vertical_reach())) {
        return true;
    }
    return false;
}

// THE RINGS OF BUSHES AND ROCKS (the user, 2026-10-09, at Render distance 8x: "the shrubs that you
// can cut down outside in [Hyrule] Field ... those shrubs are disappearing ... I can visibly see
// them pop in and out"). Those shrubs are not placed in the scene. Obj_Mure2, the spawner for a
// circle or a scatter of shrubs or a circle of rocks, creates them when the camera comes within
// 1600 units and removes them past 1705 (z_obj_mure2.c: sDistSquared1 and sDistSquared2, times
// unk_184, which is 1 in play and 4, twice the distance, in a cutscene). That test is its own, so
// the row never reached it: the trees drew out to the fog while the shrubs still came and went at
// 1600. Scaled here by the same row and clamped to the same fog as everything above, never less
// than the game's own.
//
// AND THE SPAWNER HAS TO BE AWAKE TO SPAWN. Until it has spawned, it updates only inside its own
// culling volume (100 forward plus a scale of 2100), so its distance test never runs beyond that.
// Its forward distance is raised to cover the spawning distance, only when the row has carried
// the spawning distance past the game's own, and only ever raised, so at Original nothing differs.
// Done as the spawner is created too (Actor_Init, actor_registry_patches.c), because one created
// beyond its small volume would otherwise never update to raise it.
//
// AND IT LEAVES ROOM (2026-10-12, "THE SPAWNERS LEAVE ROOM" above). A ring not yet spawned (it sets
// ACTOR_FLAG_UPDATE_CULLING_DISABLED as it spawns and clears it as it gives its children back)
// reaches past the game's own only if its twelve at most still leave room; a spawned ring past the
// game's own reach gives them back while the scene is full, by its own distance test.
void culling_mure2_reach(Actor* thisx, PlayState* play) {
    ObjMure2* this = (ObjMure2*)thisx;
    f32 own = (play->csCtx.state == CS_STATE_IDLE) ? 1600.0f : 3200.0f;
    f32 reach = render_distance(play, own) / 1600.0f;
    s32 spawned = (this->actor.flags & ACTOR_FLAG_UPDATE_CULLING_DISABLED) != 0;

    // Awake out to the far reach either way, so a ring kept back now can still spawn once there is room.
    if (reach * 1600.0f > own) {
        f32 needed = (reach * 1600.0f) - this->actor.cullingVolumeScale;

        if (needed > this->actor.cullingVolumeDistance) {
            this->actor.cullingVolumeDistance = needed;
        }
    }

    if (spawned ? spawner_gives_back(play) : spawner_keeps_to_game(play, 12)) {
        reach = own / 1600.0f;
    }
    this->unk_184 = reach * reach;
}

// @recomp Reproduced from the decompilation's z_obj_mure2.c with the distance scaled.
RECOMP_PATCH void ObjMure2_Update(Actor* thisx, PlayState* play) {
    ObjMure2* this = (ObjMure2*)thisx;

    culling_mure2_reach(thisx, play);
    this->actionFunc(this, play);
}
