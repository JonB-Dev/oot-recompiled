// THE MUSIC AT 20% WHILE A MENU HOLDS THE GAME (the menus plan, phase 98; the user, 2026-10-12:
// the menu holds the game like photo mode, the audio keeps going, the music at 20%).
//
// WHICH CONTROL, AND WHY (recorded in .scaffold/menus/execution/complete.md, phase 98):
//
//   - The four volume scale slots (ActiveSequence.volScales, sequence.h) are REJECTED: the game
//     writes every one of them on the music players itself (the fanfare's duck, the river's, the
//     enemy music's crossfade, the long sound effects' duck in sfx.c), so a slot of ours would be
//     overwritten, or would overwrite the game's.
//   - The global mute with its soften behavior (audio.h, MUTE_BEHAVIOR_SOFTEN) is REJECTED: it
//     changes each player's mute behavior bits, which also stop scripts and notes, and restoring
//     them exactly is not something the game ever does.
//   - CHOSEN: each music player's fade volume scale on the audio thread (SequencePlayer
//     .fadeVolumeScale, set by AUDIOCMD_SEQPLAYER_FADE_VOLUME_SCALE). It multiplies only that
//     player; the sound effects player is a separate player and is not touched. The game sets it
//     only while one of its own volume fades is running (ActiveSequence.volTimer), so the value
//     read when the hold begins is the value to put back, and a fade the game makes during the
//     hold is followed: its own target becomes the level to come back to.
//
// The level eases down over about the slow duration (twelve updates, 0.6 s at twenty a second)
// and back up on close with a smoothstep, then the exact value read is written back and nothing
// more is done until the next hold. While the game itself is running a volume fade it writes after
// this in the same frame (Audio_Update runs after Play_Main), so the duck yields to the game's fade
// for its length; that is the one case it does not hold, and it is the game's own change winning.
#include "patches.h"

#include "audio.h"
#include "audiothread_cmd.h"
#include "sequence.h"

// The program's side (src/game/recomp_api.cpp): 1 while a menu is open in the game, and the music
// players' fade scale then, in thousandths (the square root of the level, since the audio squares
// a player's volume: 447 for 20%).
s32 recomp_menu_hold(void);
s32 recomp_menu_music_level(void);

#define MENU_MUSIC_EASE_UPDATES 12

static const u8 sMusicPlayers[] = { SEQ_PLAYER_BGM_MAIN, SEQ_PLAYER_FANFARE, SEQ_PLAYER_BGM_SUB };
#define MENU_MUSIC_PLAYERS ((s32)(sizeof(sMusicPlayers) / sizeof(sMusicPlayers[0])))

static f32 sSaved[MENU_MUSIC_PLAYERS];
static s32 sProgress = 0;   // updates into the ease, 0 to MENU_MUSIC_EASE_UPDATES
static s32 sActive = 0;     // the duck is applied or easing

void MenuMusic_Update(void) {
    s32 held = recomp_menu_hold();
    f32 level;
    f32 t;
    f32 eased;
    s32 i;

    if (!sActive) {
        if (!held) {
            return;
        }
        // The hold begins: what each music player is at now is what it goes back to.
        for (i = 0; i < MENU_MUSIC_PLAYERS; i++) {
            sSaved[i] = gAudioCtx.seqPlayers[sMusicPlayers[i]].fadeVolumeScale;
        }
        sProgress = 0;
        sActive = 1;
    }

    // A volume fade the game runs itself while held moves the level to come back to.
    for (i = 0; i < MENU_MUSIC_PLAYERS; i++) {
        if (gActiveSeqs[sMusicPlayers[i]].volTimer != 0) {
            sSaved[i] = gActiveSeqs[sMusicPlayers[i]].volCur;
        }
    }

    if (held && sProgress < MENU_MUSIC_EASE_UPDATES) {
        sProgress++;
    }
    else if (!held && sProgress > 0) {
        sProgress--;
    }

    level = recomp_menu_music_level() / 1000.0f;
    t = (f32)sProgress / MENU_MUSIC_EASE_UPDATES;
    eased = t * t * (3.0f - 2.0f * t);
    for (i = 0; i < MENU_MUSIC_PLAYERS; i++) {
        AUDIOCMD_SEQPLAYER_FADE_VOLUME_SCALE((u32)sMusicPlayers[i], sSaved[i] * (1.0f - (1.0f - level) * eased));
    }

    // Back up all the way: the exact level read was written above (eased is 0), and the duck ends.
    if (!held && sProgress == 0) {
        sActive = 0;
    }
}
