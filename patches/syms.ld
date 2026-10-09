/* Addresses the patch link needs that are not in the game.
 *
 * Two kinds live here.
 *
 * 1. `__start`, so the linker has an entry symbol and stops warning that it cannot find one.
 *
 * 2. DUMMY ADDRESSES for functions the RUNTIME provides. A patch cannot call a native C++ function
 *    directly: it is MIPS code, and a call has to be a MIPS jump to an address. So each runtime
 *    helper is given an address in an otherwise unused region, the patch calls it as if it were a
 *    game function, and the recompiler turns that jump into a real call when it translates the
 *    patch ELF. 0x8F000000 is not memory; it is a name for "this is not the game".
 *
 * Kept deliberately short. Every entry here is a promise that something on the native side answers
 * to that address, and an entry with nothing behind it fails at link of the main binary with an
 * undefined symbol that says nothing about this file.
 */

__start = 0x80000000;

/* Dummy addresses that the recompiler turns back into calls to native functions.
 * Each one must have a matching extern "C" definition in src/game/recomp_api.cpp and be registered
 * with recomp::overlays::register_base_export, or the main binary fails to link. */
recomp_puts = 0x8F000000;
recomp_exit = 0x8F000004;

/* Maps a loaded region of ROM to its recompiled functions. The one thing overlays cannot work
 * without. */
recomp_load_overlays = 0x8F000090;

/* Libultra functions librecomp implements natively. patches/patches.h renames the ordinary names to
 * these, so a patch calling bzero() ends up calling the runtime's version. Without an address here
 * the symbol stays unresolved, the jump target is zero, and the recompiler emits a call to a bare
 * name that nothing defines. */
bzero_recomp             = 0x8F00005C;
osWritebackDCache_recomp = 0x8F000068;
osInvalICache_recomp     = 0x8F000094;
osGetTime_recomp         = 0x8F00009C;
/* The waits the frame gate in patches/frame_setup.c is made of: a private queue, a timer aimed
   at it, and a blocking receive. Blocking through the game's own call is the point, because the
   runtime then hands the emulated processor to the next game thread; a native sleep here would
   stop the audio and the scheduler with it. */
osCreateMesgQueue_recomp = 0x8F0000CC;
osRecvMesg_recomp        = 0x8F0000D0;
osSetTimer_recomp        = 0x8F0000D4;
/* The send that hands a frame's task to the scheduler, for patches/fixes/rcp_watchdog.c (the first
   patch to send on a game queue). The next free address after the moments block, not a gap. */
osSendMesg_recomp        = 0x8F00011C;

/* A frame the game dropped because a display list pool overflowed (patches/frame_setup.c):
   which pools and by how much. Counted for the frame recorder and written to the log. */
recomp_note_dropped_frame = 0x8F0000D8;
# Saved moments (phase 75): the capture request, the blocks handed over, the refusal.
recomp_moment_pending = 0x8F0000DC;
recomp_moment_captured = 0x8F0000E0;
recomp_moment_refused = 0x8F0000E4;
recomp_moment_pending_load = 0x8F0000E8;
recomp_moment_load_into = 0x8F0000EC;

/* The harness asks for a warp through this. It answers -1 almost always; a test that wants to
 * reach a scene rather than walk to it sets one pending. */
recomp_take_warp = 0x8F000098;

/* The actor registry (src/game/actor_registry.cpp), which gives each live actor a spawn index
 * that never repeats, so the renderer can interpolate its transforms by a stable identity.
 * Registered at Actor_Init, looked up when an actor is drawn, released at Actor_Delete, reset at
 * scene teardown. Addresses cross as plain integers, validated on the native side. */
recomp_actor_register = 0x8F0000A0;
recomp_actor_index    = 0x8F0000A4;
recomp_actor_release  = 0x8F0000A8;
recomp_actor_reset    = 0x8F0000AC;

/* The camera's smooth-or-cut verdict (src/game/camera_cuts.cpp): two Vec3f addresses and the
   entrance index in, 1 or 0 out. Native so its inputs can be traced. */
recomp_camera_smooth  = 0x8F0000B0;

/* How much wider than 4:3 the frame is, as 16.16 fixed point (src/game/recomp_api.cpp), for the
   3D transitions that have to cover a wide frame (phase 43). An integer, because a float has no
   agreed register on both sides of this bridge. */
recomp_wide_frame_scale_q16 = 0x8F0000B4;

/* The transition type the harness warp uses: the game's fade unless the run asked for another
   with --transition, so the wipe, the circle and the triforce can be reached on demand. */
recomp_warp_transition = 0x8F0000B8;

/* Whether the room on screen is drawn as a fixed camera over a background picture, in which case
   the view is forced to the console's 4:3 (src/ui/ui_settings.h explains why). One integer in,
   nothing out, every frame. */
recomp_prerendered_room = 0x8F0000BC;

/* How far ahead to draw, 16.16 fixed point, 65536 being the game's own distance, from the
   settings row (src/ui/ui_settings.cpp). Read once a frame by patches/culling.c, never per
   actor. */
recomp_render_distance_q16 = 0x8F0000C0;

/* The time of day the debug menu has asked for, as the game's own 16 bit clock, or -1 for none
   (the user's ask of 2026-09-23). Taken once a frame by patches/harness_warp.c beside the warp,
   so a fault that behaves differently by day and by night can be tested both ways on purpose. */
recomp_take_day_time = 0x8F0000C4;
/* Counts the game's own updates, so a recording can say how many happened between two drawn
   frames. See game_state.h, play_ticks(). */
recomp_note_play_tick = 0x8F0000C8;

/* Deterministic captures (phase 52, src/main/shots.h). Whether the harness has frozen the game:
   the patched Play_Main draws without updating while this answers 1. */
recomp_harness_frozen = 0x8F0000F0;
/* The seed the harness fixed with --seed, or 0 for none; patches/harness_rand.c asks at every
   scene load in place of the clock. */
recomp_take_seed = 0x8F0000F4;
/* One call per update from the patched Play_Main with whether the scene is ready (room and
   objects loaded); the freeze counts from the first ready update. See src/main/shots.h. */
recomp_harness_tick = 0x8F0000F8;

/* The address of the scene's LightContext when its light list is made, 0 when it is torn down
   (upgrades phase 55, patches/light_list.c). The program reads the list out of the game's memory
   once a frame for the ray traced local lights; this game hands the microcode no point lights. */
recomp_light_context = 0x8F0000FC;

/* Whether the ray traced shadows stand (upgrades phase 57, patches/actor_shadow.c): 1 at lighting
   level 1 and above, when the game skips its own painted shadows under the actors. */
recomp_traced_shadows = 0x8F000100;

/* Resume on start (phase 78b): the slot staged for the console logo to hand straight to
   Play_Init, or -1 (src/game/moments.cpp, take_boot_load). Asked once per launch. */
recomp_moment_boot_load = 0x8F000104;

/* The free camera on the right stick (upgrades phase 79, patches/free_camera.c): the Free camera
   and Camera axes rows as bits (1 on, 2 left and right inverted, 4 up and down inverted); the
   right stick's two axes past the deadzone as 12 bit fractions; and whether the camera holds
   the stick this frame, so the program leaves the stick's C button bindings silent. */
recomp_free_camera_mode = 0x8F000108;
recomp_free_camera_stick = 0x8F00010C;
recomp_free_camera_holds = 0x8F000110;

/* The Shadow casters row of the lighting menu (patches/culling.c, patches/room_behind.c): how
   much of the world behind and beside the camera stays in the traced scene so its shadows
   reach the picture. 0 the game's own culling, 1 near, 2 far, 3 everything. Read once a frame. */
recomp_caster_reach = 0x8F000114;

/* The letterbox bars (patches/letterbox.c): the size the game wants, in console pixels out of the
   picture's two hundred and forty, published every frame the game draws. The answer says whether
   the program is drawing the bars itself, in which case the game's scissor stays at the full
   picture and the bars move at the rate the picture is presented instead of the game's twenty a
   second. */
recomp_letterbox = 0x8F000118;

/* Whether the traced lights stand in for the game's light effects (2026-10-08): 1 while the
   traced local lights run and the renderer turns a drawn beam into a light of its own. The floor
   zone that fakes the Temple of Time's beam by switching the whole scene's lighting is then left
   out (patches/beam_zone.c), and so is the game's whole body stand-in for the lights of the Sun's
   Song and the blue warp (patches/effect_lights.c). */
recomp_traced_light_effects = 0x8F000120;

/* The table of the actors that moved through the air this update (patches/air_movers.c,
   2026-10-08): its address, once per update. The program reads the table for the dust the actors
   push (src/game/air_movers.cpp). */
recomp_air_movers = 0x8F000124;

/* The painted elements of the rooms where the player is (patches/room_pieces.c, 2026-10-08): a
   room's display lists listed when it is loaded, the rooms loaded now (answered with whether any
   switch moved), and each list's switch from the Debugging menu (src/game/room_pieces.cpp). */
recomp_room_pieces_begin = 0x8F000128;
recomp_room_piece_add = 0x8F00012C;
recomp_room_pieces_present = 0x8F000130;
recomp_room_piece_state = 0x8F000134;

/* The Debugging menu's Glow test row (patches/light_glow.c, 2026-10-08): whether each point
   light's glow is drawn inside the renderer's depth test at its center. */
recomp_glow_test = 0x8F000138;

/* The text rows and the Low health beep row (patches/message_assist.c, patches/health_beep.c,
   2026-10-08): the Text speed and Skip text rows as bits, and whether the low health beep sounds. */
recomp_message_assist = 0x8F00013C;
recomp_low_health_beep = 0x8F000140;

/* The painted rays through the Temple of Time's side windows (patches/window_rays.c, 2026-10-09):
   whether the windows' traced shafts stand in for them. */
recomp_window_rays_hidden = 0x8F000144;

/* Photo mode (patches/photo_mode.c, patches/photo_audio.c, 2026-10-09): whether it holds the game. */
recomp_photo_mode = 0x8F000148;
recomp_photo_picture_room = 0x8F00014C;
recomp_photo_input = 0x8F000150;
recomp_photo_hud_hidden = 0x8F000154;

/* The HUD's fading (patches/hud_anchoring.c, 2026-10-09): what the game is doing, and each part's
   opacity and color as it is drawn. */
recomp_hud_note = 0x8F000158;
recomp_hud_part = 0x8F00015C;
recomp_hud_layout = 0x8F000160;
recomp_hud_place = 0x8F000164;
recomp_hud_frame_q16 = 0x8F000168;
