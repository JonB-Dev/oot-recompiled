#pragma once

#include <cstdint>

// Reading the game's own state out of emulated memory.
//
// THIS IS THE THING A RECOMPILATION CAN DO THAT AN EMULATOR BOT CANNOT. The game's entire state is
// in this process's address space and we have the symbol table for it, so a test can assert on the
// NUMBER rather than squint at a screenshot. "Link has three hearts and is at entrance 0x00BB" is
// a fact; "the picture looks like his house" is a guess, and a guess is how a driver ended up
// opening and closing the pause menu for two minutes while every screenshot looked like progress.
//
// All game knowledge lives here on purpose: the one address, the field offsets, and the rules for
// getting at them. Everything else in the program takes plain integers from this file and knows
// nothing about the game's memory.
namespace oot::game_state {

    struct Snapshot {
        bool valid = false;

        int32_t entrance_index = 0;     // where we are, and it changes on every scene transition
        int32_t link_age = 0;           // 0 adult, 1 child
        int32_t game_mode = 0;          // normal play, file select, and so on
        int16_t health = 0;             // 16 per heart
        int16_t health_capacity = 0;
        int16_t rupees = 0;
        int16_t saved_scene = 0;        // the scene the file was last saved in

        // THE GAME'S OWN CLOCK, and the reason it is here is a fault report rather than
        // curiosity. Kakariko is reported to bounce the view, eject the player, AND move the time
        // of day on its own, differently by day and by night. A time that changes when nothing
        // asked it to is the one of those three that can be watched as a NUMBER rather than
        // guessed at from the color of the sky.
        //
        // dayTime is the game's 16 bit clock: 0 is midnight and 0x10000 would be the next, so an
        // hour is 0x2AAA. nightFlag is what the game derives from it (night above 18:00 or below
        // 06:30, in the game's own words, z_play.c), carried alongside because the two disagreeing
        // would itself be worth seeing.
        uint16_t day_time = 0;
        int32_t night_flag = 0;
        // gSaveContext.nextDayTime. The game checks it on EVERY scene init and, when it is
        // anything but 0xFFFF, forces dayTime and skyboxTime to it (z_play.c:395). A value left
        // armed here makes the clock jump on every transition and stay there, so it is read and
        // shown rather than reasoned about.
        int32_t next_day_time = -1;

        // What the boot left behind, as the game's anti-piracy checks see it: `osCicId`
        // (0x80000310, 6105 on this cartridge) and `gCICBootMagic0` (0x800088A0 in our own
        // symbol export, the word the boot code saves from physical 0x2FB1F4; 0xAD090010 is
        // what the fishing pond compares it against). Printed once so a trace proves the
        // application's deposits reached the game (src/game/game_init.cpp).
        int32_t cic_id = 0;
        uint32_t boot_magic0 = 0;

        // The game's segment table. Watched because a display list that outlives a move of
        // the object bank still carries the base it was built with, and seeing WHEN the base
        // moves is what separates that from a coincidence of sampling.
        uint32_t segment[16] = {};

        // R_UPDATE_RATE: how many video retraces each game frame lasts. 3 in ordinary play
        // (twenty frames a second), 1 in menus and some transitions. The renderer is told the
        // game's rate from this each frame, and a wrong value here is a picture that will not
        // smooth, so the harness watches it change.
        int16_t update_rate = 0;
        // Where the sun's lens flare last projected on screen, in pixels: the coordinates the
        // environment's graph callback reads the depth buffer at every frame. Read because a
        // wild value here is a read outside the console's memory, and one crash was exactly that.
        int16_t sun_test_x = 0;
        int16_t sun_test_y = 0;
    };

    // Called once, from wherever the runtime first hands us emulated memory.
    void bind(uint8_t* rdram);
    bool bound();

    // Safe to call from any thread and at any time. Returns `valid = false` rather than reading
    // anything if memory is not bound yet, which it is not until the renderer comes up.
    Snapshot read();

    // A warp the harness has asked for, or a negative number when there is none. Taking it
    // CLEARS it, so a single request warps once rather than every frame forever.
    void set_pending_warp(int32_t entrance);

    // HOW MANY TIMES THE GAME HAS UPDATED. Incremented once per Play_Main, which IS the
    // game's update, so the difference between two drawn frames is the number of updates that
    // happened between them. One every three frames is ordinary play at twenty a second; a
    // burst says a backlog was worked off in one frame, which moves everything the game
    // integrates by that multiple at once.
    uint32_t play_ticks();
    void note_play_tick();

    // HOW MANY VIDEO RETRACES HAVE HAPPENED. The game's twenty a second comes from asking the
    // runtime for one message every third retrace, so this is the clock that gate runs on.
    // Recorded beside the update count because the two together say WHICH of two things went
    // wrong during a burst: updates racing ahead while retraces tick along normally means the
    // gate was bypassed, and both racing together means the retrace clock itself jumped.
    uint32_t vi_count();
    void note_vi();

    // HOW MANY FRAMES THE GAME HAS DROPPED because a display list pool overflowed. THE FINDING OF
    // 2026-09-23: this is what the teleporting was. An overflowed frame is not submitted and, in
    // the game's own graph.c, not waited for either, so the loop ran thousands of updates a second
    // with the picture frozen on its last frame. patches/gfx_pools.c removes the overflow;
    // this count, written beside every recorded frame, is how a recording says whether it is
    // still happening. `which` is a bit per pool (opaque, translucent, overlay, work) and
    // `overrun` the worst overflow in bytes, both logged. Game thread writes, present thread
    // reads, plain like play_ticks and for the same reason.
    uint32_t dropped_frames();
    void note_dropped_frame(uint32_t which, int32_t overrun);

    // The time of day the debug menu has asked for, as the game's own 16 bit clock, or a
    // negative number for none. Taken by the patch on the game thread and CLEARED by the taking,
    // so one request sets the clock once rather than pinning it there every frame.
    void set_pending_day_time(int32_t day_time);
    int32_t take_pending_day_time();

    // Queue several. One process then visits many scenes, which matters because launching a
    // process per entrance costs about twenty five seconds of boot and file load each, and
    // because the transitions BETWEEN scenes are themselves worth exercising.
    void queue_pending_warp(int32_t entrance);
    int pending_warp_count();
    int32_t take_queued_warp();

    // Game frames to leave between queued warps. The game runs its logic at twenty a second.
    void set_warp_dwell(int frames);
    int32_t take_pending_warp();

    // How many warps the patch has taken so far, pending or queued (phase 52, for the freeze).
    uint32_t warps_taken();

    // The transition type the warps use, as the game numbers them: its own fade to black
    // unless --transition asked for another. How the wipe, the circle and the triforce are
    // reached on demand (post-parity phase 43).
    void set_warp_transition(int32_t type);
    int32_t warp_transition();

} // namespace oot::game_state
