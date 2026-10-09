// Native functions that MIPS patches can call.
//
// HOW THIS WORKS, because it is not obvious from either side alone. A patch is MIPS code, so it
// cannot call a C++ function directly: a call has to be a jump to an address. So each function here
// is given a fake address in `patches/syms.ld` (0x8F0000xx, which is not memory, just a name for
// "not the game"), the patch calls it as though it were a game function, and the recompiler turns
// that jump back into a real call when it translates the patch ELF.
//
// Each one takes (rdram, ctx) because that is what a recompiled function looks like, and unpacks
// its arguments from the MIPS calling convention with librecomp's helpers.

#include "recomp.h"
#include "librecomp/helpers.hpp"
#include "librecomp/overlays.hpp"
#include "ultramodern/config.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "game/actor_registry.h"
#include "game/camera_cuts.h"
#include "game/dl_check.h"
#include "game/game_state.h"
#include "game/hud.h"
#include "game/air_movers.h"
#include "game/light_list.h"
#include "game/moments.h"
#include "game/room_pieces.h"
#include "game/rt_state.h"
#include "main/boot.h"
#include "main/input.h"
#include "main/photo.h"
#include "main/shots.h"
#include "ui/ui_settings.h"

// Tell the runtime that a region of ROM has been loaded to a region of RAM, so it can map the
// recompiled functions for that code to where the game just put it.
//
// This is THE function that makes overlays work. Without it the game DMAs an overlay into RAM and
// the runtime has no idea that the code now at that address corresponds to any recompiled
// function, so the first call into it goes nowhere.
extern "C" void recomp_load_overlays(uint8_t* rdram, recomp_context* ctx) {
    uint32_t rom = _arg<0, uint32_t>(rdram, ctx);
    PTR(void) ram = _arg<1, PTR(void)>(rdram, ctx);
    uint32_t size = _arg<2, uint32_t>(rdram, ctx);

    load_overlays(rom, ram, size);
}

// Hand the game a warp the harness asked for, or -1 when there is none.
//
// The return value goes in v0, which is r2 in the recompiled calling convention, and it has to be
// sign extended to 64 bits like any other MIPS return: a bare -1 in the low word would be read as
// a large positive entrance index, and the game would obligingly try to load it.
extern "C" void recomp_take_warp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    int32_t entrance = oot::game_state::take_pending_warp();
    if (entrance < 0) {
        entrance = oot::game_state::take_queued_warp();
    }
    ctx->r2 = static_cast<gpr>(static_cast<int64_t>(entrance));
}

// Hand the game a time of day the debug menu asked for, or -1 when there is none (the user's ask
// of 2026-09-23). Sign extended into v0 like every other MIPS return here; a bare -1 in the low
// word would read as a huge positive clock and the game would take it.
extern "C" void recomp_take_day_time(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    ctx->r2 = static_cast<gpr>(static_cast<int64_t>(oot::game_state::take_pending_day_time()));
}

// The scene's light list (upgrades phase 55, patches/light_list.c): the address of the game's
// LightContext when a scene makes its list, 0 when it tears it down. Nothing is dereferenced
// here; the reader validates every pointer when it walks the list (game/light_list.cpp).
extern "C" void recomp_light_context(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    oot::light_list::bind_context(_arg<0, uint32_t>(rdram, ctx));
    oot::light_list::bind_environment(_arg<1, uint32_t>(rdram, ctx));
    // The environment's light mode as well (2026-09-24): whether this place follows the time of
    // day, which is what says there is a sun here rather than a room's fixed fill light.
    oot::light_list::bind_light_mode(_arg<2, uint32_t>(rdram, ctx));
    // And what the place's own surfaces give as light (2026-10-08): bit 0, its lattice windows
    // are daylight (renderer patch 0030). Flags, never an address.
    oot::light_list::bind_scene_lights(_arg<3, uint32_t>(rdram, ctx));
}

// @recomp Whether the traced shadows stand (upgrades phase 57): 1 at lighting level 1 and
// above, when the game hides its own painted shadows under the actors (patches/actor_shadow.c).
extern "C" void recomp_traced_shadows(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return<int32_t>(ctx, (oot::rt_state::level() >= 1) ? 1 : 0);
}

// The actor registry, reached by the tagging patches. An actor's address arrives as a plain
// integer and is validated by the registry; nothing here dereferences it, and nothing native is
// ever handed back but an index. Returns go in v0 (r2), sign extended like every MIPS return.
extern "C" void recomp_actor_register(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t address = _arg<0, uint32_t>(rdram, ctx);
    const int32_t index = static_cast<int32_t>(oot::actor_registry::register_actor(address));
    ctx->r2 = static_cast<gpr>(static_cast<int64_t>(index));
}

extern "C" void recomp_actor_index(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t address = _arg<0, uint32_t>(rdram, ctx);
    const int32_t index = static_cast<int32_t>(oot::actor_registry::lookup(address));
    ctx->r2 = static_cast<gpr>(static_cast<int64_t>(index));

    // With the probe on, a running count of what the tagging patches ask and what they are told,
    // so "the tags are not being emitted" can be split into "the patch never asks", "it asks
    // about something that is not a live actor" and "the answer is right and the tag is lost
    // later" from one line in the trace.
    if (oot::dl_check::enabled()) {
        static uint32_t asked = 0;
        static uint32_t answered = 0;
        ++asked;
        if (index != 0) ++answered;
        if (asked <= 3 || asked % 5000 == 0) {
            std::fprintf(stderr, "[registry] lookups %u, with an index %u; last 0x%08X -> %d\n", asked, answered,
                         address, index);
        }
    }
}

extern "C" void recomp_actor_release(uint8_t* rdram, recomp_context* ctx) {
    oot::actor_registry::release_actor(_arg<0, uint32_t>(rdram, ctx));
}

extern "C" void recomp_actor_reset(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    oot::actor_registry::reset();
}

// The camera's smooth-or-cut verdict for the camera tag. Two emulated addresses of Vec3f and
// the entrance index in; 1 or 0 out in v0.
extern "C" void recomp_camera_smooth(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t eye = _arg<0, uint32_t>(rdram, ctx);
    const uint32_t at = _arg<1, uint32_t>(rdram, ctx);
    const int32_t entrance = _arg<2, int32_t>(rdram, ctx);
    const uint32_t patch_calls = _arg<3, uint32_t>(rdram, ctx);
    ctx->r2 = static_cast<gpr>(oot::camera_cuts::smooth(rdram, eye, at, entrance, patch_calls) ? 1 : 0);
}

// How much wider than the console's 4:3 the frame the renderer draws is, as a 16.16 fixed point
// number in v0: the window's aspect over 4:3 when the view is set to expand, exactly one
// otherwise (and never below one: a window narrower than 4:3 is pillarboxed, not squeezed). A
// 3D transition sized to cover the console's frame scales by this so it covers the wide one
// (post-parity phase 43). Fixed point because a float does not cross this bridge in a register
// both sides agree on; an integer does, and 65536 is exactly one on the other side.
extern "C" void recomp_wide_frame_scale_q16(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    constexpr float original = 4.0f / 3.0f;
    float scale = 1.0f;
    if (ultramodern::renderer::get_graphics_config().ar_option == ultramodern::renderer::AspectRatio::Expand) {
        int width = 0;
        int height = 0;
        oot::boot::window_size(width, height);
        if (width > 0 && height > 0) {
            scale = std::max((static_cast<float>(width) / static_cast<float>(height)) / original, 1.0f);
        }
    }
    ctx->r2 = static_cast<gpr>(static_cast<uint32_t>(scale * 65536.0f + 0.5f));
}

// The transition the harness warp uses: the game's own fade to black unless the run asked for
// another with --transition, which is how the wipe, the circle and the triforce are reached on
// demand rather than by walking to the one door in the game that uses each.
// Whether the room on screen is one the game draws as a fixed camera over a background picture
// (patches/prerender_aspect.c decides; ui_settings.h says what it does about it). One integer in,
// nothing out. Called every frame, and the native side ignores a value that has not changed.
// How far ahead the game should draw, as a 16.16 fixed point multiple of what each actor asks
// for, from the settings row. 65536 is the game's own distance. Read once a frame by the patch.
extern "C" void recomp_render_distance_q16(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    ctx->r2 = static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(oot::ui::render_distance_q16())));
}

extern "C" void recomp_prerendered_room(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    oot::ui::set_prerendered_room(_arg<0, int32_t>(rdram, ctx) != 0, _arg<1, int32_t>(rdram, ctx));
}

// THE FREE CAMERA ON THE RIGHT STICK (upgrades phase 79, patches/free_camera.c). Three calls a
// frame from the patched Camera_Normal1: the rows as bits (1 the Free camera row on, 2 left and
// right inverted, 4 up and down inverted); the right stick's axes, raw from the first pad and
// past its deadzone, as 12 bit fractions (right and up positive); and whether the camera holds
// the stick this frame, which the device layer answers by leaving the stick's C button
// bindings silent (main/input.cpp, read_pad). Integers across the bridge, as every other call.
extern "C" void recomp_free_camera_mode(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    ctx->r2 = static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(oot::ui::free_camera_mode())));
}

// The text rows (2026-10-08, patches/message_assist.c): bits 0 and 1 the Text speed row, bit 2
// the Skip text row's "always". And the Low health beep row (patches/health_beep.c): 1 sounds.
extern "C" void recomp_message_assist(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return<int32_t>(ctx, static_cast<int32_t>(oot::ui::message_assist_mode()));
}

extern "C" void recomp_low_health_beep(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return<int32_t>(ctx, oot::ui::low_health_beep_enabled() ? 1 : 0);
}

extern "C" void recomp_free_camera_stick(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    float x = 0.0f;
    float y = 0.0f;
    oot::input::devices::read_right_stick(0, &x, &y);
    const float axis = (_arg<0, int32_t>(rdram, ctx) == 0) ? x : y;
    ctx->r2 = static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(std::lround(axis * 4096.0f))));
}

extern "C" void recomp_free_camera_holds(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    oot::input::devices::set_right_stick_held_by_camera(_arg<0, int32_t>(rdram, ctx) != 0);
}

// The Shadow casters row (patches/culling.c and patches/room_behind.c, once a frame): how much
// of the world behind and beside the camera the game keeps in the traced scene.
// The letterbox bars (patches/letterbox.c, 2026-09-24). The game scissors the picture to make
// them and the scissor can only step once per game update, which is what the user saw as "very
// very chappy". The program takes the size and eases its own bars to it every presented frame.
extern "C" void recomp_letterbox(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return<int32_t>(ctx, oot::ui::letterbox_target(static_cast<int>(_arg<0, uint32_t>(rdram, ctx))) ? 1 : 0);
}

// The table of the actors that moved through the air this update (patches/air_movers.c): its
// address only, which the program reads once a frame for the dust they push (air_movers.cpp).
extern "C" void recomp_air_movers(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    oot::air_movers::bind(_arg<0, uint32_t>(rdram, ctx));
}

// The painted elements of the rooms where the player is (patches/room_pieces.c, 2026-10-08): the
// patch lists each room's display lists when the room is loaded, says which rooms are loaded once
// a frame (and hears whether any switch moved), and asks each list's switch when one did. The
// Debugging menu lists them and moves the switches (game/room_pieces.cpp).
extern "C" void recomp_room_pieces_begin(uint8_t* rdram, recomp_context* ctx) {
    oot::room_pieces::begin(_arg<0, int32_t>(rdram, ctx), _arg<1, int32_t>(rdram, ctx));
}

extern "C" void recomp_room_piece_add(uint8_t* rdram, recomp_context* ctx) {
    oot::room_pieces::add(_arg<0, int32_t>(rdram, ctx), _arg<1, uint32_t>(rdram, ctx), _arg<2, uint32_t>(rdram, ctx),
                          _arg<3, uint32_t>(rdram, ctx));
}

extern "C" void recomp_room_pieces_present(uint8_t* rdram, recomp_context* ctx) {
    _return<uint32_t>(ctx, oot::room_pieces::present(_arg<0, int32_t>(rdram, ctx), _arg<1, int32_t>(rdram, ctx),
                                                     _arg<2, int32_t>(rdram, ctx)));
}

extern "C" void recomp_room_piece_state(uint8_t* rdram, recomp_context* ctx) {
    _return<int32_t>(ctx, oot::room_pieces::state(_arg<0, int32_t>(rdram, ctx), _arg<1, uint32_t>(rdram, ctx)));
}

// The traced light effects (2026-10-08): while the traced local lights run, the renderer turns a
// drawn beam into a light of its own (renderer patch 0029), and the game leaves out what faked
// one: the Temple of Time's beam zone (patches/beam_zone.c), and the whole body stand-in for the
// lights of the Sun's Song and the blue warp (patches/effect_lights.c). Rung 37 of the
// Experiment row puts the old picture back.
extern "C" void recomp_traced_light_effects(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return<int32_t>(ctx, oot::rt_state::light_effects_traced() ? 1 : 0);
}

// The game's painted rays through the Temple of Time's side windows, left out while the windows'
// traced shafts stand in for them (2026-10-09, patches/window_rays.c).
extern "C" void recomp_window_rays_hidden(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return<int32_t>(ctx, oot::rt_state::window_rays_hidden() ? 1 : 0);
}

// Photo mode (2026-10-09, main/photo.h): 1 while it holds the game, for its camera
// (patches/photo_mode.c) and its paused sound (patches/photo_audio.c).
extern "C" void recomp_photo_mode(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return<int32_t>(ctx, oot::shots::photo() ? 1 : 0);
}

// Photo mode's camera from its own bindings, one axis or the put back press a call (main/photo.h).
extern "C" void recomp_photo_input(uint8_t* rdram, recomp_context* ctx) {
    _return<int32_t>(ctx, oot::photo::camera_input(_arg<0, int32_t>(rdram, ctx)));
}

// The HUD's fading (2026-10-09, game/hud.h): what the game is doing this update, and each part's
// opacity and color as it is drawn (patches/hud_anchoring.c).
extern "C" void recomp_hud_note(uint8_t* rdram, recomp_context* ctx) {
    oot::hud::note(_arg<0, uint32_t>(rdram, ctx));
}

extern "C" void recomp_hud_part(uint8_t* rdram, recomp_context* ctx) {
    _return<uint32_t>(ctx, oot::hud::part(_arg<0, int32_t>(rdram, ctx)));
}

extern "C" void recomp_hud_layout(uint8_t* rdram, recomp_context* ctx) {
    _return<uint32_t>(ctx, oot::hud::layout(_arg<0, int32_t>(rdram, ctx)));
}

extern "C" void recomp_hud_place(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return<uint32_t>(ctx, oot::hud::place());
}

extern "C" void recomp_hud_frame_q16(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return<uint32_t>(ctx, oot::hud::frame_q16());
}

// Whether photo mode has the game's HUD hidden (main/photo.h).
extern "C" void recomp_photo_hud_hidden(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return<int32_t>(ctx, oot::photo::hud_hidden() ? 1 : 0);
}

extern "C" void recomp_photo_picture_room(uint8_t* rdram, recomp_context* ctx) {
    oot::photo::note_picture_room(_arg<0, int32_t>(rdram, ctx) != 0);
}

// The Glow test row (2026-10-08, patches/light_glow.c and renderer patch 0050).
extern "C" void recomp_glow_test(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return<int32_t>(ctx, oot::rt_state::glow_test());
}

extern "C" void recomp_caster_reach(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return<int32_t>(ctx, oot::rt_state::caster_reach());
}

extern "C" void recomp_warp_transition(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    ctx->r2 = static_cast<gpr>(static_cast<int64_t>(oot::game_state::warp_transition()));
}

// @recomp Called once per Play_Main so the recorder can count the game's own updates. See
// game_state.h, play_ticks(): it is what separates "the game ran fifty updates in one frame" from
// "the game ran one update with an enormous increment", which need opposite fixes.
extern "C" void recomp_note_play_tick(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    oot::game_state::note_play_tick();
}

// @recomp Phase 52: one call per update from the patched Play_Main, carrying whether the scene
// is ready (its room and objects loaded). The freeze counts its updates from the first ready
// one at its entrance; see main/shots.h for why that and not the entrance change.
extern "C" void recomp_harness_tick(uint8_t* rdram, recomp_context* ctx) {
    const int32_t ready = _arg<0, int32_t>(rdram, ctx);
    const int32_t gameplay_frames = _arg<1, int32_t>(rdram, ctx);
    const int32_t state_frames = _arg<2, int32_t>(rdram, ctx);
    const oot::game_state::Snapshot now = oot::game_state::read();
    const oot::shots::Tick tick = oot::shots::on_play_tick(now.entrance_index, now.game_mode, ready != 0, gameplay_frames, state_frames);
    _return<int32_t>(ctx, static_cast<int32_t>(tick));
}

// @recomp Whether the harness has frozen the game (phase 52, main/shots.h). The patched
// Play_Main skips its update while this answers 1. Answers 0 in play, always.
extern "C" void recomp_harness_frozen(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return<int32_t>(ctx, oot::shots::frozen() ? 1 : 0);
}

// @recomp The seed the harness fixed with --seed, or 0 for none. patches/harness_rand.c asks at
// every scene load, where the game would otherwise seed from its clock.
extern "C" void recomp_take_seed(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return<uint32_t>(ctx, oot::shots::seed());
}

// ---- saved moments (phase 75) ----------------------------------------------------------------

// The slot to capture into, or -1 almost always. Sign extended like every MIPS return.
extern "C" void recomp_moment_pending(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return<int32_t>(ctx, oot::moments::take_pending_capture());
}

namespace {
    // A block of 32 bit words out of RDRAM at a game address. Read as words through MEM_W so the
    // bytes come back in the order the game sees them; written back the same way in phase 76, so
    // the file round trips exactly whatever the runtime's byte order in memory is.
    std::vector<uint32_t> words_at(uint8_t* rdram, uint32_t address, uint32_t bytes) {
        std::vector<uint32_t> out(bytes / 4);
        const gpr base = static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(address)));
        for (uint32_t i = 0; i < bytes / 4; ++i) {
            out[i] = static_cast<uint32_t>(MEM_W(static_cast<gpr>(i) * 4, base));
        }
        return out;
    }
}

// The patch judged the moment safe and filled its records: the save block, the context block and
// the place record are at these game addresses. The sizes are the constants the patch asserted.
extern "C" void recomp_moment_captured(uint8_t* rdram, recomp_context* ctx) {
    const int32_t slot = _arg<0, int32_t>(rdram, ctx);
    const uint32_t save_addr = _arg<1, uint32_t>(rdram, ctx);
    const uint32_t context_addr = _arg<2, uint32_t>(rdram, ctx);
    const uint32_t place_addr = _arg<3, uint32_t>(rdram, ctx);

    const std::vector<uint32_t> save_words = words_at(rdram, save_addr, oot::moments::SAVE_SIZE);
    const std::vector<uint32_t> context_words = words_at(rdram, context_addr, oot::moments::CONTEXT_SIZE);
    const std::vector<uint32_t> place_words = words_at(rdram, place_addr, sizeof(oot::moments::Place));
    oot::moments::Place place{};
    std::memcpy(&place, place_words.data(), sizeof(place));

    oot::moments::on_captured(slot, save_words, context_words, place);
}

// The slot staged for a resume at start, or -1 (moments.h, take_boot_load).
extern "C" void recomp_moment_boot_load(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return<int32_t>(ctx, oot::moments::take_boot_load());
}

// The slot staged for a resume, or -1. Same gate as the capture.
extern "C" void recomp_moment_pending_load(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return<int32_t>(ctx, oot::moments::take_pending_load());
}

// Write the staged moment into the game: the save block and the context block over the game's,
// the place record into the patch's own, each as the words the capture read. Returns 1 when
// written, 0 when nothing was staged.
extern "C" void recomp_moment_load_into(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t save_addr = _arg<0, uint32_t>(rdram, ctx);
    const uint32_t context_addr = _arg<1, uint32_t>(rdram, ctx);
    const uint32_t place_addr = _arg<2, uint32_t>(rdram, ctx);

    std::vector<uint32_t> save_words;
    std::vector<uint32_t> context_words;
    oot::moments::Place place{};
    if (!oot::moments::take_staged(save_words, context_words, place)) {
        _return<int32_t>(ctx, 0);
        return;
    }
    const auto write_words = [rdram](uint32_t address, const uint32_t* words, size_t count) {
        const gpr base = static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(address)));
        for (size_t i = 0; i < count; ++i) {
            MEM_W(static_cast<gpr>(i) * 4, base) = static_cast<int32_t>(words[i]);
        }
    };
    write_words(save_addr, save_words.data(), save_words.size());
    write_words(context_addr, context_words.data(), context_words.size());
    std::vector<uint32_t> place_words(sizeof(place) / 4);
    std::memcpy(place_words.data(), &place, sizeof(place));
    write_words(place_addr, place_words.data(), place_words.size());
    oot::moments::note_resumed();
    _return<int32_t>(ctx, 1);
}

extern "C" void recomp_moment_refused(uint8_t* rdram, recomp_context* ctx) {
    const int32_t slot = _arg<0, int32_t>(rdram, ctx);
    const int32_t reason = _arg<1, int32_t>(rdram, ctx);
    oot::moments::note_refused(slot, reason);
}

// @recomp The game dropped a frame because a display list pool overflowed. See game_state.h,
// dropped_frames(): this is what the teleporting turned out to be, and counting it is how a
// recording shows whether it is still happening.
extern "C" void recomp_note_dropped_frame(uint8_t* rdram, recomp_context* ctx) {
    oot::game_state::note_dropped_frame(_arg<0, uint32_t>(rdram, ctx), _arg<1, int32_t>(rdram, ctx));
}
