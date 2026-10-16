// THE GAME'S OWN SOUNDS FOR THE MENUS (the menus plan, phase 112; the user's answer: "Game's own,
// ours as fallback"). While a menu is open in the game, a move, a choice, a step back and the menu
// opening and closing ask the game to play its own system sounds, by the game's own sound engine,
// so nothing of the game's is ever read, decoded or stored by the program.
//
// The program queues an index per press (src/ui/sound/game_queue.h, in the order of its Cue); this
// drains a few each frame on the game thread and plays the sound the table below names for it.
// The index is checked against the table here as well, so only these seven sounds can ever be
// asked for. It runs every frame from the patched Play_Main, held or not, since the audio thread
// keeps running while a menu holds the game; the draining itself tells the program the game is
// taking cues, and where it is not (the title, the file select) the program's own set plays.
#include "patches.h"

#include "sfx.h"

// The program's side (src/game/recomp_api.cpp): the next cue in the low byte and its volume in
// thousandths above it, or -1 when none is waiting.
s32 recomp_menu_sound_next(void);

// At most this many a frame; the queue drops the oldest beyond its own room, so a held key never
// floods the sound engine.
#define MENU_SOUNDS_PER_FRAME 3

// In the program's Cue order: move, select, back, toggle, refuse, open, close.
static const u16 sMenuSounds[] = {
    NA_SE_SY_CURSOR, NA_SE_SY_DECIDE, NA_SE_SY_CANCEL, NA_SE_SY_CURSOR,
    NA_SE_SY_ERROR,  NA_SE_SY_WIN_OPEN, NA_SE_SY_WIN_CLOSE,
};
#define MENU_SOUND_COUNT ((s32)(sizeof(sMenuSounds) / sizeof(sMenuSounds[0])))

// The volume the sound engine reads through its pointer for as long as the sound plays, so it
// lives here rather than on the stack; the Menu sound volume row sets it.
static f32 sMenuSoundVolume = 1.0f;

void MenuSounds_Update(void) {
    s32 i;

    for (i = 0; i < MENU_SOUNDS_PER_FRAME; i++) {
        s32 next = recomp_menu_sound_next();
        s32 cue;

        if (next < 0) {
            return;
        }
        cue = next & 0xFF;
        if (cue >= MENU_SOUND_COUNT) {
            continue;
        }
        sMenuSoundVolume = (next >> 8) / 1000.0f;
        Audio_PlaySfxGeneral(sMenuSounds[cue], &gSfxDefaultPos, 4, &gSfxDefaultFreqAndVolScale, &sMenuSoundVolume,
                             &gSfxDefaultReverb);
    }
}
