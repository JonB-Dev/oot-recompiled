// What every recompiled PATCH file includes.
//
// A patch calls game functions: the DMA manager, bzero, Main, the overlay relocator. Those are
// declared in the MAIN output's generated header, `RecompiledFuncs/funcs.h`, not in the patch
// output's own, which only declares what the patch itself defines. Without this, the recompiled
// patch compiles into a wall of "call to undeclared function" for functions that are plainly right
// there in the other header.
//
// Pointed at by `recomp_include` in config/patches.toml, the same mechanism the main recompilation
// uses for its extra declarations.

#ifndef OOT_RECOMP_PATCH_DECLS_H
#define OOT_RECOMP_PATCH_DECLS_H

#include "recomp.h"

// Every translated game function.
#include "../RecompiledFuncs/funcs.h"

// The libultra functions the recompiler calls but does not translate, two implemented by librecomp
// and two by us. Same reason as the main output needs them.
#include "game/recomp_extra_decls.h"

#ifdef __cplusplus
extern "C" {
#endif

// Native functions patches reach through the fake addresses in patches/syms.ld.
//
// Every entry here must also appear in syms.ld, be defined in src/game/recomp_api.cpp and be
// registered with register_base_export in src/main/main.cpp, or the build fails at one of four
// different places with an error that names none of the others. (This header is the one that
// fails as "call to undeclared function" inside RecompiledPatches/patches.c.)
void recomp_load_overlays(uint8_t* rdram, recomp_context* ctx);
void recomp_take_warp(uint8_t* rdram, recomp_context* ctx);
void recomp_actor_register(uint8_t* rdram, recomp_context* ctx);
void recomp_actor_index(uint8_t* rdram, recomp_context* ctx);
void recomp_actor_release(uint8_t* rdram, recomp_context* ctx);
void recomp_actor_reset(uint8_t* rdram, recomp_context* ctx);
void recomp_camera_smooth(uint8_t* rdram, recomp_context* ctx);
void recomp_prerendered_room(uint8_t* rdram, recomp_context* ctx);
void recomp_render_distance_q16(uint8_t* rdram, recomp_context* ctx);
void recomp_wide_frame_scale_q16(uint8_t* rdram, recomp_context* ctx);
void recomp_warp_transition(uint8_t* rdram, recomp_context* ctx);
void recomp_take_day_time(uint8_t* rdram, recomp_context* ctx);
void recomp_light_context(uint8_t* rdram, recomp_context* ctx);
void recomp_traced_shadows(uint8_t* rdram, recomp_context* ctx);
void recomp_note_play_tick(uint8_t* rdram, recomp_context* ctx);
void recomp_harness_frozen(uint8_t* rdram, recomp_context* ctx);
void recomp_harness_tick(uint8_t* rdram, recomp_context* ctx);
void recomp_take_seed(uint8_t* rdram, recomp_context* ctx);
void recomp_note_dropped_frame(uint8_t* rdram, recomp_context* ctx);

#ifdef __cplusplus
}
#endif

// THE GAME'S OWN sinf AND cosf, as a patch sees them. The main recompilation renames the game's
// sinf and cosf to sinf_recomp and cosf_recomp (their names clash with the C library's), but the
// patch recompilation does not apply the rename: a patch that calls the game's sinf is emitted
// as `sinf(rdram, ctx)`, which to this compiler is the C library's one-argument sinf, and the
// build stops with "too many arguments". These two lines sit AFTER every real header on purpose,
// so the only code they touch is the generated patch code that follows them, and that code
// Saved moments (phase 75): the slot to capture into or -1; the three block addresses; a refusal.
void recomp_moment_pending(uint8_t* rdram, recomp_context* ctx);
void recomp_moment_captured(uint8_t* rdram, recomp_context* ctx);
void recomp_moment_refused(uint8_t* rdram, recomp_context* ctx);
void recomp_moment_pending_load(uint8_t* rdram, recomp_context* ctx);
void recomp_moment_load_into(uint8_t* rdram, recomp_context* ctx);
void recomp_moment_boot_load(uint8_t* rdram, recomp_context* ctx);
// The free camera on the right stick (upgrades phase 79): the rows as bits, the stick's axes,
// and whether the camera holds the stick this frame.
void recomp_free_camera_mode(uint8_t* rdram, recomp_context* ctx);
// The text rows (Text speed, Skip text) and the Low health beep row.
void recomp_message_assist(uint8_t* rdram, recomp_context* ctx);
void recomp_low_health_beep(uint8_t* rdram, recomp_context* ctx);
void recomp_free_camera_stick(uint8_t* rdram, recomp_context* ctx);
void recomp_free_camera_holds(uint8_t* rdram, recomp_context* ctx);
// The Shadow casters row: how far behind and beside the camera the world stays in the traced
// scene (0 the game's own culling, 1 near, 2 far, 3 everything).
void recomp_caster_reach(uint8_t* rdram, recomp_context* ctx);
// The letterbox bars: the size the game wants, and whether the program is drawing them itself.
void recomp_letterbox(uint8_t* rdram, recomp_context* ctx);
// Whether the traced lights stand in for the game's light effects: the beams, the Sun's Song,
// the blue warp.
void recomp_traced_light_effects(uint8_t* rdram, recomp_context* ctx);
// The Debugging menu's Glow test row: how a point light's glow is hidden.
void recomp_glow_test(uint8_t* rdram, recomp_context* ctx);
// Whether the windows' traced shafts stand in for the game's painted rays through the side windows.
void recomp_window_rays_hidden(uint8_t* rdram, recomp_context* ctx);
// Whether photo mode holds the game.
void recomp_photo_mode(uint8_t* rdram, recomp_context* ctx);
// Whether the room photo mode froze is drawn over a picture.
void recomp_photo_picture_room(uint8_t* rdram, recomp_context* ctx);
// Photo mode's camera from its own bindings.
void recomp_photo_input(uint8_t* rdram, recomp_context* ctx);
// Whether photo mode has the game's HUD hidden.
void recomp_photo_hud_hidden(uint8_t* rdram, recomp_context* ctx);
// The HUD's fading: what the game is doing, and each part's opacity and color.
void recomp_hud_note(uint8_t* rdram, recomp_context* ctx);
void recomp_hud_part(uint8_t* rdram, recomp_context* ctx);
// Each part's size and place.
void recomp_hud_layout(uint8_t* rdram, recomp_context* ctx);
// Where the HUD sits when HUD sits is Custom, and the frame its distances are shares of.
void recomp_hud_place(uint8_t* rdram, recomp_context* ctx);
void recomp_hud_frame_q16(uint8_t* rdram, recomp_context* ctx);
// The table of the actors that moved through the air this update, for the dust they push.
void recomp_air_movers(uint8_t* rdram, recomp_context* ctx);
// The painted elements of the rooms where the player is, for the Debugging menu's switches.
void recomp_room_pieces_begin(uint8_t* rdram, recomp_context* ctx);
void recomp_room_piece_add(uint8_t* rdram, recomp_context* ctx);
void recomp_room_pieces_present(uint8_t* rdram, recomp_context* ctx);
void recomp_room_piece_state(uint8_t* rdram, recomp_context* ctx);

// never means the C library. (Phase 39's matrix library patches were the first to take a sine
// inside a patch.)
#define sinf sinf_recomp
#define cosf cosf_recomp

// NOT the place for libultra functions librecomp reimplements (osGetTime, osRecvMesg, osSetTimer
// and friends). Those are renamed on the PATCH side, in patches/patches.h, with an address for
// the renamed symbol in patches/syms.ld, and the generated code then arrives already carrying the
// `_recomp` suffix. A define here would work too, and did for one build, which is exactly the
// second mechanism for one job that the patches.h block exists to prevent.

#endif // OOT_RECOMP_PATCH_DECLS_H
