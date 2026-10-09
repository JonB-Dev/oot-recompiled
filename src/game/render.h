// The renderer context: our implementation of ultramodern's RendererContext, backed by RT64.
//
// src/game is "the bridge" per structure.md: the only place that knows both the runtime API and
// this game's specifics. This file is the runtime-API half of that.
//
// Scope note. The reference project's equivalent also carries texture pack management, which is
// explicitly out of scope here (app-type.md, "out of scope for this plan": mod support). Leaving it
// out is a decision, not an omission, and re-planning is what adds it later.

#pragma once

#include <filesystem>

#include <memory>

#include "ultramodern/renderer_context.hpp"

namespace RT64 {
    // struct, not class. RT64 declares it as a struct, and under the Microsoft C++ ABI a mismatched
    // tag is not merely a style warning: it can produce linker errors, which is why clang reports it
    // as -Wmismatched-tags rather than ignoring it.
    struct Application;
}

// The render abstraction RT64 sits on. Forward declared rather than included, so that every file
// wanting the frame recorder's one accessor does not also take plume's whole header.
namespace plume {
    struct RenderTexture;
    struct RenderFramebuffer;
}

namespace oot {
    namespace renderer {

        class RT64Context final : public ultramodern::renderer::RendererContext {
        public:
            RT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);
            ~RT64Context() override;

            bool valid() override;

            bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                               const ultramodern::renderer::GraphicsConfig& new_config) override;

            void enable_instant_present() override;
            void send_dl(const OSTask* task) override;
            void send_dummy_workload(uint32_t fb_address) override;
            void update_screen() override;
            void shutdown() override;
            uint32_t get_display_framerate() const override;
            float get_resolution_scale() const override;

        private:
            std::unique_ptr<RT64::Application> app;

            // The game's memory, for the light list the ray traced local lights read once a
            // frame (phase 55, game/light_list.h). The runtime owns it; this only reads.
            const uint8_t* rdram_ = nullptr;
        };

        // The callback ultramodern calls to build the context. Registered in main.
        std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
            uint8_t* rdram,
            ultramodern::renderer::WindowHandle window_handle,
            bool developer_mode);

        // Ask for RT64's present-early mode before the context exists (a command line flag; see
        // boot.cpp). The context reads the request once, when it is created.
        void request_present_early();

        // Phase 53: the ray tracing level and the debug view the harness asked for on the command
        // line (--rt-level, --rt-debug), before the context exists. Applied once, after setup,
        // when the build carries the path and the device supports it; otherwise traced and
        // ignored. The settings row of phase 58 becomes the way a person sets the level.
        void request_raytracing_level(int level);
        void request_raytracing_view(const char* view);

        // THE LIGHTING CONTROLS (phase 57, the user's sub menu of 2026-09-24). Every value the
        // traced passes take from the person: the level, the debug view, and the strengths,
        // radii, ranges and fades that were design constants. Applied live from the interface
        // thread whenever a row changes (the renderer publishes the configuration to its queues
        // under their lock), and once at setup from the settings that were loaded; the command
        // line's --rt-level and --rt-debug, when given, override the level and the view for the
        // run so the harness keeps its handle.
        struct LightingValues {
            int level = 0;                       // 0 off, 1 shadows, 2 occlusion and lights, 3 all: derived from the switches
            const char* view = "final";
            float shadow_strength = 1.0f;        // 0 leaves the sun's shadow untraced
            float shadow_softness = 1.0f;
            float occlusion_strength = 0.7f;
            float occlusion_radius = 40.0f;
            int occlusion_samples = 8;
            float light_strength = 1.0f;         // 0 leaves the local lights untraced
            float light_reach = 1.0f;
            float bounce_strength = 0.25f;
            float bounce_range = 3000.0f;
            float reflection_strength = 0.5f;
            float reflection_range_scale = 1.0f;
            float light_glow = 0.0002f;
            // The Glow size row (2026-09-24): the glow's radius as a scale on the game's own
            // light radius, apart from the reach, so a light can reach far with a small glow.
            float glow_size = 0.5f;
            float sun_haze = 0.0f;
            float haze_range = 1400.0f;
            float fog_fade = 1.0f;
            float distance_fade = 0.0f;
            float history_blend = 0.12f;
            int filter_steps = 2;
            // The Experiment row: 0 off, else a numbered variant of a disputed sum in the shaders,
            // for the person to cycle in play and name the one that looks right (2026-09-24).
            int experiment = 0;
            // The Darkness row (2026-09-24): how much of the game's own ambient is taken away
            // where no traced light (the sun under a sky, a local light) reaches; 0 off.
            float darkness = 0.0f;
            // The Light brightness row (2026-10-08, renderer patch 0033): how far a traced light
            // may lift a surface above the game's own full light; 0 as it always was, 1 no limit
            // short of four times that light.
            float light_brightness = 0.0f;
            // The dust in the light, indoors and out, and the outdoor wind (2026-10-08, renderer
            // patches 0035 and 0036): each place's density (0 off) and visibility, the indoor
            // drift's pace, the wind's speed in world units a second, its gusts, and the way it
            // blows over the ground as world x and z.
            float indoor_dust_density = 0.0f;
            float indoor_dust_visibility = 0.0f;
            float indoor_dust_drift = 1.0f;
            float outdoor_dust_density = 0.0f;
            float outdoor_dust_visibility = 0.0f;
            float wind_speed = 0.0f;
            float wind_gusts = 0.0f;
            float wind_x = 0.0f;
            float wind_z = 1.0f;
            // The ambience distance rows (renderer patches 0037 and 0041): where the dust's fade
            // ends, indoors and outdoors.
            float indoor_ambience_distance = 1200.0f;
            float outdoor_ambience_distance = 1200.0f;
            // The dust size rows (renderer patch 0038): the median mote's radius, world units.
            float indoor_dust_size = 0.3f;
            float outdoor_dust_size = 0.3f;
            // The windows' aim from the Debugging menu (renderer patch 0044): which window (0 every
            // one), whether from the old beam's direction, degrees steeper and turned, and a move of
            // its light in world units. The defaults change nothing.
            int window_aim = 0;
            int window_aim_from = 0;
            float window_tilt = 0.0f;
            float window_turn = 0.0f;
            float window_move_x = 0.0f;
            float window_move_y = 0.0f;
            float window_move_z = 0.0f;
            // Candidates on the Debugging menu (2026-10-08, renderer patches 0049 and 0050; the
            // user: "adds some debugging options to cycle through so I can test a bunch of
            // different options to see which one fixes it without regressions"). Light through
            // stone: 0 a local light stops at the first surface and so does its glow in the air,
            // 1 the same with the glow unshadowed, 2 the 48 unit clearance as in 0.4.0. Glow test:
            // 0 a light's glow is hidden by its center and Navi's core per pixel, 1 glows by their
            // center and nothing per pixel, 2 every glow per pixel as in 0.4.0, 3 nothing, as 0.3.9.
            int light_stone = 1;   // the user's pick, 2026-10-08: stops at stone, the glow in the air passes
            int glow_test = 0;
            // The windows' shafts (renderer patch 0051): the air's brightness per world unit of
            // beam (0 off), the fade's length as a share of each window's fall (renderer patch
            // 0052), and the game's drawn beam shown again.
            float shaft_strength = 0.0f;
            float shaft_length = 0.45f;
            int drawn_beam = 0;
            // The Shadow rays row (2026-09-24): samples over the sun's disc per lit pixel, a
            // moving surface twice that. One is the console's look and the noisiest.
            int shadow_rays = 4;
            // The Indoor light row (2026-09-24): a sun of our own where the place has no sky,
            // as a share of the room's own ambient, taken out of it rather than added to it.
            float indoor_light = 0.5f;
            // The Shadow casters row: 0 the game's own culling, 1 near, 2 far, 3 everything. Not
            // the renderer's: the game's culling patches read it once a frame (rt_state.h).
            int caster_reach = 1;
        };
        void apply_lighting(const LightingValues& values);

        // THE PICTURE'S FIXED SIZE AND RATIO (2026-09-24), beside the runtime's own graphics
        // configuration, which knows only the original, its double and the window: a fixed
        // height in lines (720, 1080, 1440, 2160; 0 for none) rendered as that multiple of the
        // console's 240, and a fixed aspect ratio (16 over 9; 0 for none). Set before the
        // graphics configuration is pushed, read when it is applied.
        struct PictureValues {
            int fixed_height = 0;
            double aspect = 0.0;
        };
        void set_picture(const PictureValues& values);

        // WHAT THE PICTURE IS DRAWN AT (the user's choice of 2026-10-07, "go with option one"),
        // worked out in one place for the renderer and for the Downsampling row's labels, so the
        // row can never say one thing while the renderer does another.
        //
        // Downsampling draws above the size shown and shrinks it to that size, AT MATCH THE
        // WINDOW TOO: it was not read there, so the row did nothing at the setting most people
        // play at. And the most it may draw is HALF THE CARD'S OWN MEMORY rather than a fixed
        // 2880 lines, estimated from what is on: a combination that does not fit steps its
        // downsampling down until it does, and the row says so.
        //
        // The estimate is the measured one (the harness, 2026-10-07, the game's own video memory
        // on the user's card): about 300 MB, plus per pixel drawn 8 bytes, 44 for each
        // antialiasing sample past the first, and about 1050 with the traced lighting on. It
        // fitted 3.8, 11.3 and 19.7 GB at three sizes.
        struct ResolutionPlan {
            int asked = 1;            // the downsampling factor asked for: 1, 2 or 4
            int used = 1;             // the factor that fits
            double scale = 1.0;       // the lines shown over the console's 240, before downsampling
            int width = 0;            // what is drawn at the factor used, in pixels
            int height = 0;
            uint64_t bytes = 0;       // the video memory estimated for that
            uint64_t budget = 0;      // half the card's own memory; 0 before the card is known
        };
        // For a configuration and picture as the settings would push them (the factor asked for
        // is config.ds_option). Any thread.
        ResolutionPlan plan_resolution(const ultramodern::renderer::GraphicsConfig& config,
                                       const PictureValues& picture);
        // The frame's width over its height as the renderer draws it now: the console's 4:3, a
        // fixed ratio, or the window's own when the view is expanded to it, never narrower than
        // 4:3. The HUD's custom places measure their distances from the edges against it
        // (game/hud.h). Any thread.
        double frame_aspect();
        // How many pixels of the window the interface holds at the left and the right edge (a
        // side panel, 2026-09-24): the renderer scales the whole picture, ratio kept, into the
        // rest and stands it there. Zero and zero puts it back. Any thread.
        void set_picture_inset(int left, int right);

        // The texture pack the person chose, or an empty path for none (2026-09-30). RT64 does
        // the whole job: it reads the pack's own rt64.json and swaps a texture by its hash as
        // the game asks for it, from a DIRECTORY or from a zip, so nothing is unpacked and
        // nothing is copied. Reloading is expensive, so the same path twice does nothing.
        // Any thread: this only records the choice, and the graphics thread hands it to the
        // renderer at the top of its next frame, the one thread RT64 allows (2026-10-07).
        // detail_percent is the Texture detail row, the share of the pack's own width and height
        // each texture is read at (100 as made); a change to it reloads the pack like a change
        // of pack does, because textures already loaded were read at the old size.
        void apply_texture_pack(const std::filesystem::path& pack, int detail_percent);

        // Phase 53: switch on Device Removed Extended Data before the device exists
        // (--gpu-breadcrumbs), and, from the crash handler, say whether the device is still
        // there and, when it is not, which command it stopped on and which allocation a page
        // fault hit. Prints nothing when there is no renderer.
        void request_gpu_breadcrumbs();
        void report_gpu_state();

        // THE TEXTURE BEHIND THE FRAMEBUFFER THE DRAW HOOK IS HANDED, or null when there is no
        // renderer or the framebuffer is not one of the swap chain's.
        //
        // WHY THIS EXISTS. RT64's draw hook is handed a RenderFramebuffer, and plume's
        // RenderFramebuffer exposes nothing but its width and height: there is no way from it to
        // the texture a copy would have to read. The frame recorder needs exactly that texture,
        // because the swap chain's own image IS the finished picture, interpolated frames and our
        // own interface included.
        //
        // The present queue holds both lists side by side (`swapChainFramebuffers[i]` for
        // `swapChain->getTexture(i)`), so the index is recovered by matching the pointer. That is
        // a lookup over two or three entries once a frame, and it means RT64 stays unmodified:
        // patching plume to expose the texture would make lib/rt64 a fork, which structure.md
        // says is a recorded decision rather than a convenience.
        plume::RenderTexture* swap_chain_texture(const plume::RenderFramebuffer* framebuffer);

    } // namespace renderer
} // namespace oot
