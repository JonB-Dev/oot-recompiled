// Go straight to an entrance, so a test can reach a place instead of walking to it.
//
// WHY THIS EXISTS. Phases 26 to 30 are "play this stretch of the game, fix what crashes". A
// harness that can only press buttons cannot reach a stretch of the game: left to itself it
// thrashes in whatever room it started in, which is exactly what happened. Warping turns scene
// coverage from "route the whole game first" into "visit each entrance and see what breaks", and
// that is the difference between those phases being possible and not.
//
// HOW. `Play_Main` is the game's per frame entry point and the one place that holds the PlayState
// without being enormous. In a retail build the debug blocks compile away and it is four lines, so
// it can be reproduced faithfully with one addition: ask the native side whether a warp is
// pending, and if so set the transition up exactly as the game's own exits do.
//
// The native side answers -1 almost always, so the cost when nothing is warping is one call per
// frame that returns immediately.

#include "patches.h"
#include "air_movers.h"
#include "room_pieces.h"
#include "misc_funcs.h"

#include "environment.h"
#include "play_state.h"
#include "player.h"
#include "save.h"
#include "scene.h"
#include "sequence.h"
#include "sram.h"
#include "transition.h"

// Set by the original to point at the first controller, and read elsewhere in the game. Preserved
// because this patch REPLACES Play_Main: anything the original did that is dropped here simply
// stops happening, and that is how a patch introduces a bug that looks like a game bug.
extern Input* D_8012D1F8;

void DebugDisplay_Init(void);
void Play_Update(PlayState* this);
// The free camera's right stick added to the frame's input in first person and while aiming
// (patches/free_camera.c).
void FreeCamera_FirstPersonInput(PlayState* play);
// Photo mode's camera (patches/photo_mode.c): sets the view while photo mode holds the game.
void PhotoMode_Update(PlayState* play);
void Play_Draw(PlayState* this);

// The harness asks for a warp through the native side. Returns the entrance to go to, or a
// negative number when there is nothing pending, which is the usual answer.
s32 recomp_take_warp(void);

// The transition the warp uses: the game's fade to black unless the run asked for another with
// --transition, which is how the wipe, the circle and the triforce are reached on demand.
s32 recomp_warp_transition(void);

// The time of day the debug menu has asked for, as the game's own 16 bit clock, or a negative
// number when nothing is pending. Answers negative almost always, so the cost when nobody is
// setting the clock is one call a frame that returns immediately.
s32 recomp_take_day_time(void);

// ---- saved moments (phase 75, .scaffold/states/) -------------------------------------------
//
// A MOMENT IS THE GAME'S OWN SAVE BLOCK PLUS WHERE THE PLAYER STANDS, captured here because this
// is the one place that has the PlayState, the save context and the player together once a
// frame, and the same place the warp and the clock already cross. The native side asks for a
// slot; this side judges whether the moment is safe (in play, no transition, no cutscene, no
// text, not paused, the player present and alive) and, when it is, fills two records of its own
// and hands over three ADDRESSES: the save block, the context block, the place record. The native
// side reads the bytes out of RDRAM and writes the file. Nothing is copied twice, and nothing
// here knows what a file looks like.
//
// The sizes are asserted against the real structs, because the native side carries them as bare
// numbers (src/game/moments.h) and a decompilation bump that moved a field must fail THIS build.
typedef struct {
    /* 0x00 */ s32 sceneLayer;
    /* 0x04 */ s32 respawnFlag;
    /* 0x08 */ RespawnData respawn[RESPAWN_MODE_MAX];
    /* 0x5C */ u32 nextDayTime;
    /* 0x60 */ u32 nextTransitionType;
    /* 0x64 */ u32 nextCutsceneIndex;
    /* 0x68 */ s32 magicState;
    /* 0x6C */ s32 magicCapacity;
} MomentContext; // size = 0x70

typedef struct {
    /* 0x00 */ f32 pos[3];
    /* 0x0C */ s32 yaw;
    /* 0x10 */ s32 room;
    /* 0x14 */ s32 scene;
    /* 0x18 */ s32 entrance;
    /* 0x1C */ s32 linkAge;
    /* 0x20 */ u32 dayTime;
    /* 0x24 */ u32 reserved;
} MomentPlace; // size = 0x28

_Static_assert(sizeof(Save) == 0x1354, "the save block the native side expects moved");
_Static_assert(sizeof(RespawnData) == 0x1C, "RespawnData moved");
_Static_assert(sizeof(MomentContext) == 0x70, "the context block the native side expects moved");
_Static_assert(sizeof(MomentPlace) == 0x28, "the place record the native side expects moved");

static MomentContext sMomentContext;
static MomentPlace sMomentPlace;

// The slot a capture is wanted into, or a negative number, which is the usual answer.
s32 recomp_moment_pending(void);
// The blocks are ready at these addresses.
void recomp_moment_captured(s32 slot, u32 saveAddr, u32 contextAddr, u32 placeAddr);
// Not this frame, for this reason (1 not running, 2 transition, 3 cutscene, 4 text, 5 paused,
// 6 not in play, 7 no player, 8 out of health).
void recomp_moment_refused(s32 slot, s32 reason);
// The slot a resume is wanted from, or a negative number.
s32 recomp_moment_pending_load(void);
// Write the staged moment into the game at these addresses: the save block, the context block
// and the place record. Returns 1 when it did, 0 when nothing was staged after all.
s32 recomp_moment_load_into(u32 saveAddr, u32 contextAddr, u32 placeAddr);
// At the console logo, before any frame: the slot of the newest moment when Resume on start
// is on and it is staged for a boot resume, or a negative number. Asked once per launch.
s32 recomp_moment_boot_load(void);

// Is this a frame a moment may be taken on, or restored into? 0 when it is; the reason when not.
static s32 Moment_Unsafe(PlayState* play) {
    Player* player = GET_PLAYER(play);

    if (gSaveContext.gameMode != GAMEMODE_NORMAL) {
        return 6;
    }
    if (!play->state.running) {
        return 1;
    }
    if (play->transitionTrigger != TRANS_TRIGGER_OFF || play->transitionMode != TRANS_MODE_OFF) {
        return 2;
    }
    if (play->csCtx.state != CS_STATE_IDLE) {
        return 3;
    }
    if (play->msgCtx.msgMode != MSGMODE_NONE) {
        return 4;
    }
    if (play->pauseCtx.state != PAUSE_STATE_OFF) {
        return 5;
    }
    if (player == NULL) {
        return 7;
    }
    if (gSaveContext.save.info.playerData.health <= 0) {
        return 8;
    }
    return 0;
}

static void Moment_WriteBack(void);

// THE RESUME IS THE WAY FARORE'S WIND RETURNS (phase 76). The game already has a path that takes
// the player to a stored spot in a stored scene without a void out and without the arrival
// cutscene: respawnFlag 3 with the TOP respawn record filled (z_player.c, the Farore's Wind
// answer; Player_Init then places the actor from that record, z_room.c opens the record's room,
// z_demo.c skips the entrance cutscene while the flag is above zero). So: the native side writes
// the moment's save block and context back into the game, this fills the TOP record from the
// place record, sets the flag, and asks for the transition exactly as the warp does. Nothing here
// invents a way to place a player.
static void Moment_Resume(PlayState* play, s32 slot) {
    s32 reason = Moment_Unsafe(play);
    RespawnData* top;
    s32 i;

    if (reason != 0) {
        recomp_moment_refused(slot, reason);
        return;
    }
    if (!recomp_moment_load_into((u32)&gSaveContext.save, (u32)&sMomentContext, (u32)&sMomentPlace)) {
        return;
    }
    Moment_WriteBack();

    // And the scene, through the game's own exit, as the warp does.
    play->nextEntranceIndex = (s16)sMomentPlace.entrance;
    play->transitionTrigger = TRANS_TRIGGER_START;
    play->transitionType = (s16)recomp_warp_transition();
}

// The context block and the place, back into the game after the native side wrote the save
// block: shared by the resume in play and the resume at start.
static void Moment_WriteBack(void) {
    RespawnData* top;
    s32 i;

    // The context block, back where it came from, word by word for the same reason the capture
    // copies it that way.
    gSaveContext.sceneLayer = sMomentContext.sceneLayer;
    {
        const u32* src = (const u32*)sMomentContext.respawn;
        volatile u32* dst = (volatile u32*)gSaveContext.respawn;
        for (i = 0; i < (s32)(sizeof(sMomentContext.respawn) / 4); i++) {
            dst[i] = src[i];
        }
    }
    gSaveContext.nextDayTime = (u16)sMomentContext.nextDayTime;
    gSaveContext.nextTransitionType = (u8)sMomentContext.nextTransitionType;
    gSaveContext.nextCutsceneIndex = (u16)sMomentContext.nextCutsceneIndex;
    gSaveContext.magicState = (s16)sMomentContext.magicState;
    gSaveContext.magicCapacity = (s16)sMomentContext.magicCapacity;

    // The spot, in the record the game reads for flag 3, standing idle.
    top = &gSaveContext.respawn[RESPAWN_MODE_TOP];
    top->pos.x = sMomentPlace.pos[0];
    top->pos.y = sMomentPlace.pos[1];
    top->pos.z = sMomentPlace.pos[2];
    top->yaw = (s16)sMomentPlace.yaw;
    top->entranceIndex = (s16)sMomentPlace.entrance;
    top->roomIndex = (u8)sMomentPlace.room;
    top->playerParams = PLAYER_PARAMS(PLAYER_START_MODE_IDLE, PLAYER_START_BG_CAM_DEFAULT);
    gSaveContext.respawnFlag = 3;

    // The clock, all three fields, as the debug menu sets it.
    gSaveContext.save.dayTime = (u16)sMomentPlace.dayTime;
    gSaveContext.skyboxTime = (u16)sMomentPlace.dayTime;
    gSaveContext.save.nightFlag =
        ((u16)sMomentPlace.dayTime > CLOCK_TIME(18, 0) || (u16)sMomentPlace.dayTime < CLOCK_TIME(6, 30)) ? 1 : 0;
}

// RESUME AT START (phase 78b, the user's ask of 2026-09-24: "open game and immediately load save
// state when that restore state on open is enabled and this way open skips title screen
// altogether"). Called from the console logo's own init, before any frame is drawn. The native
// side has staged the newest moment; this does what the file select does with a chosen file
// (FileSelect_LoadGame, less the sword fix up for a fresh file, which a live capture never
// needs and which would toggle an owned equipment bit), then what the resume in play does, and
// hands the game to Play_Init directly: the logo state's init returns with running off, so its
// main never runs, its destroy reads and checks the SRAM as it always does, and the next state
// is the scene the moment names. The title and the file select are never shown.
static void Moment_BootResume(GameState* gameState, s32 slot) {
    if (!recomp_moment_load_into((u32)&gSaveContext.save, (u32)&sMomentContext, (u32)&sMomentPlace)) {
        return;
    }

    // The file the moment was taken from, so a save from the resumed session goes back to it;
    // the native side refuses a boot resume of a moment that does not know its file.
    gSaveContext.fileNum = (s32)sMomentPlace.reserved - 1;
    gSaveContext.gameMode = GAMEMODE_NORMAL;

    gSaveContext.respawn[RESPAWN_MODE_DOWN].entranceIndex = ENTR_LOAD_OPENING;
    gSaveContext.respawnFlag = 0;
    gSaveContext.seqId = (u8)NA_BGM_DISABLED;
    gSaveContext.natureAmbienceId = 0xFF;
    gSaveContext.showTitleCard = true;
    gSaveContext.dogParams = 0;
    gSaveContext.timerState = TIMER_STATE_OFF;
    gSaveContext.subTimerState = SUBTIMER_STATE_OFF;
    gSaveContext.eventInf[0] = 0;
    gSaveContext.eventInf[1] = 0;
    gSaveContext.eventInf[2] = 0;
    gSaveContext.eventInf[3] = 0;
    gSaveContext.prevHudVisibilityMode = HUD_VISIBILITY_ALL;
    gSaveContext.nayrusLoveTimer = 0;
    gSaveContext.healthAccumulator = 0;
    gSaveContext.magicState = MAGIC_STATE_IDLE;
    gSaveContext.prevMagicState = MAGIC_STATE_IDLE;
    gSaveContext.forcedSeqId = NA_BGM_GENERAL_SFX;
    gSaveContext.skyboxTime = CLOCK_TIME(0, 0);
    gSaveContext.nextTransitionType = TRANS_NEXT_TYPE_DEFAULT;
    gSaveContext.nextCutsceneIndex = NEXT_CS_INDEX_NONE;
    gSaveContext.cutsceneTrigger = 0;
    gSaveContext.chamberCutsceneNum = CHAMBER_CS_FOREST;
    gSaveContext.nextDayTime = NEXT_TIME_NONE;
    gSaveContext.retainWeatherMode = false;
    gSaveContext.buttonStatus[0] = gSaveContext.buttonStatus[1] = gSaveContext.buttonStatus[2] =
        gSaveContext.buttonStatus[3] = gSaveContext.buttonStatus[4] = BTN_ENABLED;
    gSaveContext.forceRisingButtonAlphas = gSaveContext.nextHudVisibilityMode = gSaveContext.hudVisibilityMode =
        gSaveContext.hudVisibilityModeTimer = gSaveContext.magicCapacity = 0;
    // The magic meter fills from nothing to the saved amount, as after the file select.
    gSaveContext.magicFillTarget = gSaveContext.save.info.playerData.magic;
    gSaveContext.save.info.playerData.magicLevel = gSaveContext.save.info.playerData.magic = 0;
    gSaveContext.save.info.playerData.naviTimer = 0;

    // Then the moment's own context (which carries the magic state the capture saw) and the
    // spot, exactly as the resume in play writes them.
    Moment_WriteBack();

    // Straight into the scene: Play_Init reads the entrance from the save and places the
    // player from the TOP record under flag 3.
    gSaveContext.save.entranceIndex = (s16)sMomentPlace.entrance;
    SET_NEXT_GAMESTATE(gameState, Play_Init, PlayState);
    gameState->running = false;
}

// @recomp Patched: the first call to this is the console logo's, from its init, before any
// frame is drawn (the only other caller is the file select's init), and it is where a moment
// resumes at start. The body is the game's own; the addition is the block after it.
RECOMP_PATCH void Sram_Alloc(GameState* gameState, SramContext* sramCtx) {
    static s32 sBootChecked = 0;
    s32 slot;

    sramCtx->readBuff = GAME_STATE_ALLOC(gameState, SRAM_SIZE, "../z_sram.c", 1294);
    ASSERT(sramCtx->readBuff != NULL, "sram->read_buff != NULL", "../z_sram.c", 1295);

    if (!sBootChecked) {
        sBootChecked = 1;
        slot = recomp_moment_boot_load();
        if (slot >= 0) {
            Moment_BootResume(gameState, slot);
        }
    }
}

static void Moment_Capture(PlayState* play, s32 slot) {
    Player* player = GET_PLAYER(play);
    s32 reason = Moment_Unsafe(play);
    s32 i;

    if (reason != 0) {
        recomp_moment_refused(slot, reason);
        return;
    }

    // THE SCENE'S OWN FLAGS INTO THE SAVE FIRST (the user, 2026-09-24: "save states dont seem to
    // save chests or enemy or actual game progress?? just saves my location? so i have to go
    // back and redo things ive done and reopen chests"). The chests opened, the switches
    // pressed, the rooms cleared and the items collected in the scene the player stands in
    // live in the actor context while the scene runs; the game copies them into the save's
    // per scene record only on the way OUT of the scene (Play_SaveSceneFlags, from the scene
    // exit). A moment taken mid scene carried the record as it stood at entry, so the resumed
    // scene reopened every chest and put every enemy back. The game's own copy runs here first,
    // so the block handed over is what the scene exit would have written.
    Play_SaveSceneFlags(play);

    sMomentContext.sceneLayer = gSaveContext.sceneLayer;
    sMomentContext.respawnFlag = gSaveContext.respawnFlag;
    // Word by word through a volatile pointer, NOT a struct assignment: the compiler turns a
    // struct copy (or a plain loop it recognizes) into a call to memcpy, and the recompiled patch
    // then names a library function the native build has no declaration for. A volatile store
    // cannot be folded into a call, so this stays the seven stores per record it reads as.
    {
        const u32* src = (const u32*)gSaveContext.respawn;
        volatile u32* dst = (volatile u32*)sMomentContext.respawn;
        for (i = 0; i < (s32)(sizeof(sMomentContext.respawn) / 4); i++) {
            dst[i] = src[i];
        }
    }
    sMomentContext.nextDayTime = gSaveContext.nextDayTime;
    sMomentContext.nextTransitionType = gSaveContext.nextTransitionType;
    sMomentContext.nextCutsceneIndex = gSaveContext.nextCutsceneIndex;
    sMomentContext.magicState = gSaveContext.magicState;
    sMomentContext.magicCapacity = gSaveContext.magicCapacity;

    sMomentPlace.pos[0] = player->actor.world.pos.x;
    sMomentPlace.pos[1] = player->actor.world.pos.y;
    sMomentPlace.pos[2] = player->actor.world.pos.z;
    sMomentPlace.yaw = player->actor.shape.rot.y;
    sMomentPlace.room = play->roomCtx.curRoom.num;
    sMomentPlace.scene = play->sceneId;
    sMomentPlace.entrance = gSaveContext.save.entranceIndex;
    sMomentPlace.linkAge = gSaveContext.save.linkAge;
    sMomentPlace.dayTime = gSaveContext.save.dayTime;
    // The file the game is playing, plus one so that zero (a moment from before this was kept,
    // 2026-09-24) reads as unknown: the resume at start needs it to send a later save home.
    sMomentPlace.reserved = (u32)gSaveContext.fileNum + 1;

    recomp_moment_captured(slot, (u32)&gSaveContext.save, (u32)&sMomentContext, (u32)&sMomentPlace);
}

// Counts this function's own calls natively, which IS the game's update count: Play_Main is
// the update. A recording can then say how many updates happened between two drawn frames,
// which is what separates a backlog being worked off from one update with a huge increment.
void recomp_note_play_tick(void);

// Whether the harness has frozen the game (phase 52, src/main/shots.h): 1 once the run has
// arrived at the entrance it asked for and updated the number of times it asked for. While it
// answers 1 the update is skipped and the draw runs on, so every presented frame is the same
// frame and a capture is the same picture whenever it is taken. Answers 0 in play, always.
s32 recomp_harness_frozen(void);

// One call per update with whether the scene is ready (its room and objects loaded) and the
// game's own update count. Answers 1 when the update is to be HELD for a load still in flight
// (only ever in a run that asked for a freeze), 0 otherwise. See the block in Play_Main.
s32 recomp_harness_tick(s32 ready, s32 gameplayFrames, s32 stateFrames);

// The game's own: the two things Play_Update does for a load in flight, done here while the
// update is held so the load can finish.
void Object_UpdateEntries(ObjectContext* objectCtx);
s32 Room_ProcessRoomRequest(PlayState* play, RoomContext* roomCtx);

// The game state's frames counter on the frozen frame, pinned every frame after (see below).
static u32 sFrozenFrames;
static s32 sFrozenFramesValid = 0;

// The game's own, patched in harness_rand.c to take the harness's seed in place of the clock.
void Rand_Seed(u32 seed);

// @recomp Patched to take a pending warp from the harness before the frame runs.
RECOMP_PATCH void Play_Main(GameState* thisx) {
    PlayState* this = (PlayState*)thisx;
    s32 entrance;
    s32 day_time;
    s32 slot;
    s32 stall;

    // @recomp One call, first thing, so the count is of updates that STARTED.
    recomp_note_play_tick();

    // @recomp Phase 52: whether the scene is READY, for the freeze's own count. The room's DMA
    // is done (status 0, a segment set) and no object slot is still waiting on its load (a
    // negative id is a request in flight). The runtime serves those loads from the ROM file on
    // another thread, so the update the actors spawn on moves with the wall clock, and a count
    // from the entrance change froze the same scene a frame apart from run to run.
    {
        s32 i;
        s32 ready = (this->roomCtx.status == 0) && (this->roomCtx.curRoom.segment != NULL);

        for (i = 0; ready && (i < this->objectCtx.numEntries); i++) {
            if (this->objectCtx.slots[i].id < 0) {
                ready = 0;
            }
        }
        stall = recomp_harness_tick(ready, (s32)this->gameplayFrames, (s32)this->state.frames);

        // 2 is the first ready update at the entrance: the game's running update count is set
        // to a fixed value, because actors time their blinking and idling from it and it
        // reached the entrance carrying however many updates the file's scene ran before the
        // warp, which no two runs make the same. Then the ordinary update.
        if (stall == 2) {
            this->gameplayFrames = 0;
            stall = 0;
        }
    }

    D_8012D1F8 = &this->state.input[0];

    DebugDisplay_Init();

    // @recomp The one addition.
    //
    // Set up exactly as one of the game's own exits does, rather than by writing the entrance
    // index somewhere and hoping: the trigger is what makes the game run its own transition, load
    // the scene, and place the player. Anything less leaves half the state from the old scene.
    entrance = recomp_take_warp();
    if (entrance >= 0) {
        this->nextEntranceIndex = (s16)entrance;
        this->transitionTrigger = TRANS_TRIGGER_START;
        this->transitionType = (s16)recomp_warp_transition();
    }

    // @recomp The second addition (the user's ask of 2026-09-23): set the clock on request, so a
    // fault that behaves differently by day and by night can be tested both ways on purpose
    // rather than waited for.
    //
    // THREE FIELDS, NOT ONE, and the other two are the reason this is done here in the game's own
    // types rather than by writing a number into memory from the native side.
    //   - dayTime is the clock itself.
    //   - skyboxTime is what the sky is drawn from, and z_play.c sets it alongside dayTime every
    //     time it moves the clock. Left behind, the sky keeps the old hour while the world takes
    //     the new one.
    //   - nightFlag is DERIVED, and the game recomputes it from exactly this test (z_play.c:
    //     night above 18:00 or below 06:30). It is set here so the frame the clock changes on is
    //     already consistent, rather than spending one frame with a noon sky and night actors.
    day_time = recomp_take_day_time();
    if (day_time >= 0) {
        gSaveContext.save.dayTime = (u16)day_time;
        gSaveContext.skyboxTime = (u16)day_time;
        gSaveContext.save.nightFlag =
            ((u16)day_time > CLOCK_TIME(18, 0) || (u16)day_time < CLOCK_TIME(6, 30)) ? 1 : 0;
    }

    // @recomp The third addition (phase 76): resume a saved moment, when one is staged and the
    // frame is safe. BEFORE the capture below on purpose: a run that resumes one slot and
    // captures into another on the same frame then sees the resume's transition and waits for
    // the far side, which is the spot the moment describes.
    slot = recomp_moment_pending_load();
    if (slot >= 0) {
        Moment_Resume(this, slot);
    }

    // @recomp The fourth addition (phase 75): a saved moment, when one is asked for and the frame
    // is a safe one to keep. Before the update, so the state kept is the state the last drawn
    // frame showed, which is what the thumbnail the native side takes will show too.
    slot = recomp_moment_pending();
    if (slot >= 0) {
        Moment_Capture(this, slot);
    }

    // @recomp The fifth addition (phase 52): a frozen game draws and does not update, so the A
    // against B captures compare one frame with itself and not with the frame after it.
    //
    // TWO COUNTERS, NOT ONE. gameplayFrames advances in Play_Update and stops with it. The game
    // state's own frames counter advances in GameState_Update, outside this function, and actors
    // animate their textures and their blinking from it in their DRAW, so a frozen scene still
    // shimmered (the lake's water, the shopkeeper's face) until it was pinned too. It is set back
    // to the value it had on the frozen frame, here, before the draw, every frame.
    //
    // AND THE LOADS COME FIRST. The runtime serves the room and the objects from the ROM file
    // on another thread, and an actor begins updating on the update its object arrives, so two
    // runs of one scene had actors a frame apart in their animation whatever the count was
    // anchored on. While the harness says so (stall: a freeze was asked for, the scene is the
    // one asked for, and a load is still in flight) the update is held and only the two things
    // the update would have done for the loads are done, so every actor begins on the same
    // update in every run. The game itself never sees this: with no freeze asked for the
    // native side answers 0, always.
    if (stall) {
        Object_UpdateEntries(&this->objectCtx);
        Room_ProcessRoomRequest(this, &this->roomCtx);
    }
    else if (recomp_harness_frozen()) {
        if (!sFrozenFramesValid) {
            sFrozenFrames = this->state.frames;
            sFrozenFramesValid = 1;
        }
        this->state.frames = sFrozenFrames;

        // THE THIRD COUNTER IS THE RANDOM GENERATOR. Some actors draw with random numbers (a
        // lantern's flicker, a flame's height), so every drawn frame moves the generator on and
        // a capture lands on whichever flicker the wall clock reached. Reseeded here every
        // frozen frame, so each frame draws the same random numbers as the last. The patched
        // Rand_Seed substitutes the seed the harness fixed, whatever is passed.
        Rand_Seed(sFrozenFrames);
    }
    else {
        // @recomp A freeze that has ended is forgotten (2026-10-09): the program can now hold the
        // game for a comparison and let it go again (main/shots.h, set_hold), and the next hold
        // must pin the frame it begins on, not the first one ever frozen.
        sFrozenFramesValid = 0;

        // @recomp The free camera's right stick joins the left stick's look in first person and
        // while aiming (patches/free_camera.c), on the frame's input, before the update reads it.
        FreeCamera_FirstPersonInput(this);
        Play_Update(this);

        // @recomp The things that moved through the air this update, for the dust they push
        // (patches/air_movers.c).
        AirMovers_Publish(this);
    }

    // @recomp The rooms' painted elements, each as the Debugging menu's switch for it says
    // (patches/room_pieces.c), before the frame that draws them.
    RoomPieces_Update(this);

    // @recomp Photo mode (2026-10-09, patches/photo_mode.c): while it holds the game the frozen
    // frame is drawn from its own camera, which the player moves; otherwise nothing.
    PhotoMode_Update(this);

    Play_Draw(this);
}
