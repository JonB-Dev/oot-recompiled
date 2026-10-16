// PHOTO MODE PAUSES THE SOUND (2026-10-09, the user: "freeze the game at its current frame and then
// pause audio, of course").
//
// The game's sound is made by its own audio thread, apart from the update that photo mode stops:
// on every vertical retrace it advances the music and the effects (AudioThread_Update) and hands
// the console's sound processor the task that turns them into samples. So a frozen game kept
// playing its music. While photo mode holds the game, the retrace is answered with nothing: no
// update and no task. The music stands where it was rather than running on unheard, the samples
// already queued play out, and the speakers go quiet. When photo mode ends the next retrace does
// what it always did, scheduling the task it made before the pause, whose buffers nothing has
// touched since, so the sound carries on from the very spot it stopped.
//
// The function below is the game's own, line for line, with the one question added first.
#include "patches.h"

#include "audiomgr.h"
#include "regs.h"
#include "speed_meter.h"

// The program's side (src/game/recomp_api.cpp): 1 while photo mode holds the game.
s32 recomp_photo_mode(void);

// The game's own, from audio.h and audio_thread_manager.c.
AudioTask* AudioThread_Update(void);
void AudioMgr_NotifyTaskDone(AudioMgr* audioMgr);

// @recomp The retrace, answered with nothing while photo mode holds the game.
RECOMP_PATCH void AudioMgr_HandleRetrace(AudioMgr* audioMgr) {
    AudioTask* rspTask;

    // @recomp The one addition.
    if (recomp_photo_mode()) {
        return;
    }

    if (R_AUDIOMGR_ACTIVITY_LEVEL > AUDIOMGR_ACTIVITY_LEVEL_ALL) {
        // Inhibit audio rsp task processing
        audioMgr->rspTask = NULL;
    }

    if (audioMgr->rspTask != NULL) {
        // Got an rsp task to process, build the OSScTask and forward it to the scheduler to run

        audioMgr->audioTask.next = NULL;
        audioMgr->audioTask.flags = OS_SC_NEEDS_RSP;
        audioMgr->audioTask.framebuffer = NULL;

        audioMgr->audioTask.list = audioMgr->rspTask->task;
        audioMgr->audioTask.msgQueue = &audioMgr->taskDoneQueue;

        audioMgr->audioTask.msg = NULL;
        osSendMesg(&audioMgr->sched->cmdQueue, (OSMesg)&audioMgr->audioTask, OS_MESG_BLOCK);
        Sched_Notify(audioMgr->sched);
    }

    // Update the audio driver

    gAudioThreadUpdateTimeStart = osGetTime();

    if (R_AUDIOMGR_ACTIVITY_LEVEL >= AUDIOMGR_ACTIVITY_LEVEL_NO_UPDATE) {
        // Skip update, no rsp task produced
        rspTask = NULL;
    } else {
        rspTask = AudioThread_Update();
    }

    gAudioThreadUpdateTimeAcc += osGetTime() - gAudioThreadUpdateTimeStart;
    gAudioThreadUpdateTimeStart = 0;

    if (audioMgr->rspTask != NULL) {
        // Wait for the audio rsp task scheduled on the previous retrace to complete.
        osRecvMesg(&audioMgr->taskDoneQueue, NULL, OS_MESG_BLOCK);
        // Report task done
        AudioMgr_NotifyTaskDone(audioMgr);
    }
    // Update rsp task to be scheduled on next retrace
    audioMgr->rspTask = rspTask;
}
