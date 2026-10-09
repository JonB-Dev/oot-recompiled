// RT64 behind ultramodern's RendererContext interface.
//
// Modeled on upstream/Zelda64Recomp/src/main/rt64_render_context.cpp for STRUCTURE AND TECHNIQUE.
// There are no game addresses in this file, so nothing here falls under the recompute-never-copy
// rule; what it does carry is that project's hard-won knowledge of how RT64 wants to be driven.
//
// Deliberately smaller than the reference in two places:
//   - one texture pack at a time from the person's own folder (2026-09-30), not the reference's
//     mod list, because mod support is out of scope (app-type.md)
//   - no RmlUi render hooks, which arrive with the UI at Phase 24

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

// HLSL_CPU is defined by the build for the whole target, so it is NOT defined here: doing both is
// a -Wmacro-redefined error under our warnings-as-errors, since CMake defines it as 1 and the
// reference project's bare `#define HLSL_CPU` does not match.
#include "hle/rt64_application.h"
#include "hle/rt64_workload_queue.h"
#include "shared/rt64_point_light.h"
#include "game/air_movers.h"
#include "game/light_list.h"
#include "main/recorder.h"
#include "main/shots.h"
#include "main/screenshots.h"
#include "main/video.h"
#include "main/photo.h"
#include "main/sweep.h"

#include <string>
// rt64_application.h only forward declares PresentQueue, and swap_chain_texture below reaches into
// its swap chain framebuffer list, so the definition is needed here as well.
#include "hle/rt64_present_queue.h"
// The D3D12 device behind plume's interface, for the removed device report (report_gpu_state):
// the device removed reason and the breadcrumbs are D3D12's own and have no plume surface.
#include "plume/plume_d3d12.h"

#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"

#include "ui/ui_settings.h"
#include "game/dl_check.h"
#include "game/game_state.h"
#include "game/render.h"
#include "game/rt_state.h"
#include "main/launch.h"
#include "main/probe.h"

// The RCP registers RT64 reads and writes. At GLOBAL scope rather than in the anonymous namespace
// below, because other translation units in the runtime refer to them by name: putting them in an
// anonymous namespace compiles cleanly here and then fails at link with undefined symbols that do
// not mention this file.
unsigned int MI_INTR_REG = 0;

unsigned int DPC_START_REG = 0;
unsigned int DPC_END_REG = 0;
unsigned int DPC_CURRENT_REG = 0;
unsigned int DPC_STATUS_REG = 0;
unsigned int DPC_CLOCK_REG = 0;
unsigned int DPC_BUFBUSY_REG = 0;
unsigned int DPC_PIPEBUSY_REG = 0;
unsigned int DPC_TMEM_REG = 0;

namespace {

    // RT64 reads and writes these as if they were the console's memories. They are ours to own
    // because nothing else in this program models an RCP.
    uint8_t DMEM[0x1000];
    uint8_t IMEM[0x1000];

    // The renderer never raises an interrupt here: the runtime schedules the game's threads, so
    // there is nothing to interrupt. A real function rather than a null pointer, because RT64 calls
    // it unconditionally.
    void dummy_check_interrupts() {}

    // The live application, set once setup succeeds and cleared when the context goes. Read by
    // swap_chain_texture, which RT64's draw hook reaches through a C function pointer and so
    // cannot be handed a context.
    RT64::Application* g_app = nullptr;

    // THE TEXTURE PACK, ASKED FOR ON ANY THREAD AND APPLIED ON ONE (2026-10-07). The row is
    // written from the menu and from the game thread's once a frame push, and the first version
    // handed the path straight to RT64 from whichever of those called. RT64 says its loader
    // "is assumed to be called from the only thread that is capable of submitting new textures",
    // which is the graphics thread, and the path itself was read and written by several threads
    // with no lock. The user's report the same day: "The texture pack loading is actually
    // causing the game to crash."
    //
    // So asking and applying are two things now, the way the reference project does it
    // (upstream/Zelda64Recomp, check_texture_pack_actions at the top of send_dl): any thread
    // records the wish under the lock and raises the flag, and the graphics thread takes it at the
    // top of its next frame. The wish is also how the launcher's choice survives until there is
    // a renderer to give it to.
    std::mutex g_pack_mutex;
    std::filesystem::path g_pack_wanted;     // guarded by g_pack_mutex
    int g_pack_wanted_detail = 100;          // guarded by g_pack_mutex
    bool g_pack_wanted_set = false;          // guarded by g_pack_mutex
    std::atomic<bool> g_pack_pending{ false };
    // The graphics thread's own record of what it last handed the renderer. Never read anywhere
    // else, so it needs no lock.
    std::filesystem::path g_pack_applied;
    int g_pack_applied_detail = 100;
    bool g_pack_applied_set = false;

    // Hands the renderer the pack last asked for, if it changed. Graphics thread only; defined
    // beside apply_texture_pack below.
    void take_texture_pack();

    // Phase 53: Device Removed Extended Data, asked for on the command line (--gpu-breadcrumbs)
    // and switched on before the device exists. A removed device (a hung or faulting GPU) then
    // remembers which command each command list was executing and which allocation a page fault
    // hit, and the crash handler prints both (report_gpu_state). Off by default: the breadcrumbs
    // cost a little every frame, and the report is for bring-up and for bug reports.
    bool g_gpu_breadcrumbs = false;

    void enable_gpu_breadcrumbs() {
        ID3D12DeviceRemovedExtendedDataSettings* settings = nullptr;
        if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings))) || settings == nullptr) {
            std::fprintf(stderr, "[gpu] breadcrumbs asked for, but this D3D12 runtime keeps no removed device data\n");
            return;
        }
        settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings->Release();
        std::fprintf(stderr, "[gpu] breadcrumbs on: a removed device reports its last command and its page fault\n");
    }

    const char* breadcrumb_op_name(D3D12_AUTO_BREADCRUMB_OP op) {
        switch (op) {
            case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SetMarker";
            case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
            case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
            case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
            case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
            case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
            case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
            case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
            case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
            case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
            case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return "ResolveSubresource";
            case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRenderTargetView";
            case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "ClearUnorderedAccessView";
            case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDepthStencilView";
            case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
            case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE: return "ExecuteBundle";
            case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "Present";
            case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return "ResolveQueryData";
            case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return "BeginSubmission";
            case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return "EndSubmission";
            case D3D12_AUTO_BREADCRUMB_OP_WRITEBUFFERIMMEDIATE: return "WriteBufferImmediate";
            case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return "BuildRaytracingAccelerationStructure";
            case D3D12_AUTO_BREADCRUMB_OP_EMITRAYTRACINGACCELERATIONSTRUCTUREPOSTBUILDINFO: return "EmitRaytracingAccelerationStructurePostbuildInfo";
            case D3D12_AUTO_BREADCRUMB_OP_COPYRAYTRACINGACCELERATIONSTRUCTURE: return "CopyRaytracingAccelerationStructure";
            case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return "DispatchRays";
            case D3D12_AUTO_BREADCRUMB_OP_SETPIPELINESTATE1: return "SetPipelineState1";
            case D3D12_AUTO_BREADCRUMB_OP_DISPATCHMESH: return "DispatchMesh";
            case D3D12_AUTO_BREADCRUMB_OP_BARRIER: return "Barrier";
            default: return "other";
        }
    }

    const char* removed_reason_name(HRESULT reason) {
        switch (reason) {
            case DXGI_ERROR_DEVICE_HUNG: return "the device hung (a command never finished)";
            case DXGI_ERROR_DEVICE_REMOVED: return "the device was removed (a page fault, or the adapter went away)";
            case DXGI_ERROR_DEVICE_RESET: return "the device was reset";
            case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "a driver internal error";
            case DXGI_ERROR_INVALID_CALL: return "an invalid call";
            default: return "an unknown reason";
        }
    }

    // Phase 53: what the harness asked for on the command line, applied once after setup. When
    // given, the flag wins over the lighting file's level and view for the run (phase 57), so
    // the harness keeps its handle whatever the file says.
    int g_rt_level = 0;
    std::string g_rt_view = "final";
    bool g_rt_level_forced = false;
    bool g_rt_view_forced = false;

    // WHETHER THIS DEVICE CAN TRACE A RAY AT ALL, read once from the device's capabilities at
    // setup (2026-09-26, the user on a laptop with Intel Iris graphics: "My laptop with iris
    // graphics doesn't show shadows").
    //
    // It has to be here, beside the level, because the level is what the GAME asks about, and the
    // game was never told. push_lighting already refused to switch the traced passes on without
    // the capability, so the renderer behaved. But rt_state::level() reported the menu's level
    // regardless, and the patch that hides the game's own painted shadows under the actors reads
    // exactly that (patches/actor_shadow.c through recomp_traced_shadows). So on a device that
    // cannot trace, turning the row On hid every painted shadow in the game and drew no traced one
    // in its place: not a missing feature, a game with no shadows at all. A capability the renderer
    // knows and the game does not is the shape of that bug, so the one accessor answers for both.
    //
    // False until setup runs, which is the safe direction: before the renderer exists the answer
    // is "do not hide anything".
    bool g_rt_supported = false;

    // The lighting controls as the interface last set them (phase 57), and the picture's fixed
    // size and ratio (2026-09-24), each applied to the renderer when it exists and read at setup
    // when it does not yet.
    oot::renderer::LightingValues g_lighting;
    oot::renderer::PictureValues g_picture;   // guarded by g_picture_mutex
    std::mutex g_picture_mutex;

    oot::renderer::PictureValues picture_now() {
        std::lock_guard<std::mutex> lock(g_picture_mutex);
        return g_picture;
    }

    // THE RESOLUTION PLAN'S STATE (render.h, plan_resolution). The card's memory and the window
    // are recorded on the graphics thread and read by any thread that plans; the rest is the
    // graphics thread's own record of what the applied plan rested on, so update_screen can tell
    // when the window or the lighting has moved under it. A change to the picture only raises
    // g_replan: the configuration is rebuilt and published on the graphics thread alone, so it
    // can never land between a change of antialiasing and the targets rebuilt for it (see
    // update_config).
    std::atomic<uint64_t> g_card_memory{ 0 };
    std::atomic<int> g_window_width{ 0 };
    std::atomic<int> g_window_height{ 0 };
    std::atomic<bool> g_replan{ false };
    ultramodern::renderer::GraphicsConfig g_applied_config;
    bool g_applied_config_set = false;
    int g_planned_width = -1;
    int g_planned_height = -1;
    bool g_planned_traced = false;

    // The window the plan is for. Once the game is running, the swap chain's own size. Before
    // that the window is the launcher's, which says nothing about the picture, so the plan is for
    // the window the game WILL have: the desktop when the Window row says fullscreen, the size
    // the game opens at when it does not.
    void plan_window(int& width, int& height) {
        if (oot::launch::game_started() && (g_window_height.load() > 0)) {
            width = g_window_width.load();
            height = g_window_height.load();
            return;
        }
        oot::launch::game_window_size(oot::ui::settings().window_mode == 1, width, height);
    }

    // The picture's width over its height, as the renderer will draw it: the console's 4:3, a
    // fixed ratio, or the window's own when the view is expanded to it. The resolution plan and
    // frame_aspect (the HUD's custom places) both ask here, so they measure the same frame.
    double picture_aspect(const ultramodern::renderer::GraphicsConfig& config,
                          const oot::renderer::PictureValues& picture, int windowWidth, int windowHeight) {
        double aspect = 4.0 / 3.0;
        if (config.ar_option != ultramodern::renderer::AspectRatio::Original) {
            if (picture.aspect > 0.0) {
                aspect = picture.aspect;
            }
            else if ((windowWidth > 0) && (windowHeight > 0)) {
                aspect = double(windowWidth) / double(windowHeight);
            }
        }
        return aspect;
    }

    // Whether the traced lighting is on, for the estimate: the level the person set (or the
    // command line forced) on a device that can trace. NOT rt_state::level(), which also stands
    // the passes down in a prerendered room: planning on that would change what is drawn at every
    // such doorway, rebuilding every target twice for a room that lasts a few seconds.
    bool planned_traced() {
        return g_rt_supported && ((g_rt_level_forced ? g_rt_level : g_lighting.level) > 0);
    }

    // The debug views of lighting-design.md §8, by the name the flag takes.
    struct RtView {
        const char* name;
        int mode;   // interop::VisualizationMode, as an int so this compiles without the path
    };

    const RtView RT_VIEWS[] = {
        { "final", 0 },
        { "position", 1 },
        { "normal", 2 },
        { "specular", 3 },
        { "diffuse", 4 },
        { "instance", 5 },
        // "shadow" is the direct light AS THE PICTURE USES IT, after the history and the filter
        // (2026-09-24): it showed the raw single sample before, and the user judged the grain of
        // that pass as the picture's. "raw" is the single sample, for comparing the two.
        { "shadow", 7 },
        { "raw", 6 },
        { "direct", 7 },
        { "indirect", 8 },
        { "indirect-filtered", 9 },
        { "reflection", 10 },
        { "refraction", 11 },
        { "transparent", 12 },
        { "flow", 13 },
        { "distance", 16 },
        { "history", 20 },   // how many frames of history each pixel holds (patch 0008)
        { "hits", 21 },      // what the first ray made of each pixel: hit, dropped, or miss (patch 0008)
        { "ao", 17 },
        { "lights", 18 },
        { "scatter", 19 },
    };

    // Only the built path reads it; a build without the series (the Off proof's first half)
    // still compiles this file under warnings as errors.
    [[maybe_unused]] int rt_view_mode(const std::string& name) {
        for (const RtView& view : RT_VIEWS) {
            if (name == view.name) {
                return view.mode;
            }
        }
        return -1;
    }

    // The reference project has a compute_max_supported_aa() here, which downgrades the MSAA
    // setting to what the device actually supports. It is not carried over: it used
    // RT64::RenderSampleCounts, which does not exist in the RT64 we pin (the type moved out of
    // that namespace), and nothing in this file called it, because the capability check it fed
    // belongs with the settings UI at Phase 24 rather than with bringing a window up.
    //
    // Worth noting as a pattern: our RT64 is newer than the reference project's, so its API is
    // the thing to read, not the reference's usage of it.

    RT64::UserConfiguration::AspectRatio to_rt64(ultramodern::renderer::AspectRatio option) {
        switch (option) {
            case ultramodern::renderer::AspectRatio::Original: return RT64::UserConfiguration::AspectRatio::Original;
            case ultramodern::renderer::AspectRatio::Expand:   return RT64::UserConfiguration::AspectRatio::Expand;
            case ultramodern::renderer::AspectRatio::Manual:   return RT64::UserConfiguration::AspectRatio::Manual;
            default:                                          return RT64::UserConfiguration::AspectRatio::Original;
        }
    }

    RT64::UserConfiguration::Antialiasing to_rt64(ultramodern::renderer::Antialiasing option) {
        switch (option) {
            case ultramodern::renderer::Antialiasing::None:   return RT64::UserConfiguration::Antialiasing::None;
            case ultramodern::renderer::Antialiasing::MSAA2X: return RT64::UserConfiguration::Antialiasing::MSAA2X;
            case ultramodern::renderer::Antialiasing::MSAA4X: return RT64::UserConfiguration::Antialiasing::MSAA4X;
            case ultramodern::renderer::Antialiasing::MSAA8X: return RT64::UserConfiguration::Antialiasing::MSAA8X;
            default:                                          return RT64::UserConfiguration::Antialiasing::None;
        }
    }

    RT64::UserConfiguration::RefreshRate to_rt64(ultramodern::renderer::RefreshRate option) {
        switch (option) {
            case ultramodern::renderer::RefreshRate::Original: return RT64::UserConfiguration::RefreshRate::Original;
            case ultramodern::renderer::RefreshRate::Display:  return RT64::UserConfiguration::RefreshRate::Display;
            case ultramodern::renderer::RefreshRate::Manual:   return RT64::UserConfiguration::RefreshRate::Manual;
            default:                                           return RT64::UserConfiguration::RefreshRate::Original;
        }
    }

    RT64::UserConfiguration::InternalColorFormat to_rt64(ultramodern::renderer::HighPrecisionFramebuffer option) {
        switch (option) {
            case ultramodern::renderer::HighPrecisionFramebuffer::On:   return RT64::UserConfiguration::InternalColorFormat::High;
            case ultramodern::renderer::HighPrecisionFramebuffer::Auto: return RT64::UserConfiguration::InternalColorFormat::Automatic;
            default:                                                     return RT64::UserConfiguration::InternalColorFormat::Standard;
        }
    }

    // Graphics thread only: the constructor, update_config and update_screen's re-plan.
    void set_application_user_config(RT64::Application* application,
                                     const ultramodern::renderer::GraphicsConfig& config) {
        const oot::renderer::PictureValues picture = picture_now();
        g_applied_config = config;
        g_applied_config_set = true;

        // EVERY RESOLUTION IS THE RENDERER'S MANUAL ONE NOW, the multiple worked out by the plan
        // (render.h). Match the window was RT64's window integer scale, which works out the same
        // multiple of 240 for itself but has no way to take a downsampling factor, so the row did
        // nothing there; and a fixed height (2026-09-24: 720p, 1080p, 1440p, 4K) was capped at
        // 2880 lines drawn, after 1080p with 4x downsampling passed 17 GB at one frame a second.
        // The cap was the right idea with the wrong measure: lines say nothing about the
        // antialiasing or the traced lighting, which are most of what a line costs, nor about
        // the card. The plan's measure is the card's memory, and it covers every resolution.
        const oot::renderer::ResolutionPlan plan = oot::renderer::plan_resolution(config, picture);
        application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
        application->userConfig.resolutionMultiplier = plan.scale * plan.used;
        application->userConfig.downsampleMultiplier = plan.used;

        // On the trace whenever what is drawn changes, and never otherwise: a window dragged
        // larger re-plans every frame it moves and would fill the log.
        static int lastWidth = -1, lastHeight = -1, lastUsed = -1;
        if ((plan.width != lastWidth) || (plan.height != lastHeight) || (plan.used != lastUsed)) {
            lastWidth = plan.width;
            lastHeight = plan.height;
            lastUsed = plan.used;
            constexpr uint64_t MB = 1024ull * 1024ull;
            if (plan.budget == 0) {
                std::fprintf(stderr, "[gfx] drawing %dx%d until the card is known, then planning again\n",
                             plan.width, plan.height);
            }
            else {
                std::fprintf(stderr, "[gfx] drawing %dx%d, downsampling %dx%s; about %llu MB of a %llu MB budget\n",
                             plan.width, plan.height, plan.used,
                             (plan.used != plan.asked) ? " (asked for more, which does not fit)" : "",
                             static_cast<unsigned long long>(plan.bytes / MB),
                             static_cast<unsigned long long>(plan.budget / MB));
            }
        }

        switch (config.hr_option) {
            case ultramodern::renderer::HUDRatioMode::Clamp16x9:
                application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Manual;
                application->userConfig.extAspectTarget = 16.0 / 9.0;
                break;
            case ultramodern::renderer::HUDRatioMode::Full:
                application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Expand;
                break;
            case ultramodern::renderer::HUDRatioMode::Original:
            default:
                application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Original;
                break;
        }

        application->userConfig.aspectRatio = to_rt64(config.ar_option);
        // A fixed ratio (2026-09-24: 16:9 and the rest) is the renderer's manual aspect with that
        // target, unless a prerendered room has forced the console's own, which arrives here as
        // the runtime's Original.
        if ((picture.aspect > 0.0) && (config.ar_option != ultramodern::renderer::AspectRatio::Original)) {
            application->userConfig.aspectRatio = RT64::UserConfiguration::AspectRatio::Manual;
            application->userConfig.aspectTarget = picture.aspect;
        }
        application->userConfig.antialiasing = to_rt64(config.msaa_option);
        application->userConfig.refreshRate = to_rt64(config.rr_option);
        application->userConfig.refreshRateTarget = config.rr_manual_value;
        application->userConfig.internalColorFormat = to_rt64(config.hpfb_option);
        application->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;
    }

    // The lighting controls into the renderer's configuration (phase 57): the level and the view
    // (the command line's flags winning when given), then every strength, radius, range and fade.
    void push_lighting(RT64::Application* application) {
#if defined(RT_ENABLED) && RT_ENABLED
        const bool supported = application->device->getCapabilities().raytracing;
        // Through rt_state::level(), so the renderer, the game and the recorder cannot disagree
        // about what is running: it is zero without the capability and zero in a prerendered room.
        const int level = oot::rt_state::level();
        const std::string view = g_rt_view_forced ? g_rt_view : std::string(g_lighting.view != nullptr ? g_lighting.view : "final");
        const int mode = rt_view_mode(view);
        if (mode < 0) {
            std::fprintf(stderr, "[rt] unknown view \"%s\", using final\n", view.c_str());
        }
        RT64::RaytracingConfiguration& rt = application->rtConfig;
        rt.visualizationMode = static_cast<interop::VisualizationMode>(mode < 0 ? 0 : mode);
        rt.level = level;
        rt.occlusionSamples = std::max(g_lighting.occlusion_samples, 1);
        // EVERY LIGHT THE GAME CAN HOLD, not the renderer's own sixteen (2026-09-24, the user: a
        // fairy beside a sign lighting nothing). The traced passes walk min(lightsCount,
        // maxLights) of the list, and this was never set, so it kept RT64's default of sixteen
        // while the game's pool is thirty two (LIGHTS_BUFFER_SIZE, z_lights.c). Anything past the
        // sixteenth entry simply did not exist for the lighting, which in a scene with a few
        // torches is where the fairies are. The buffer is sized from the count each frame, so
        // nothing is allocated for lights that are not there, and a light out of reach costs one
        // compare before it is skipped, not a ray.
        rt.maxReflections = (level >= 3 && g_lighting.reflection_strength > 0.0f) ? 1 : 0;
        rt.shadowStrength = g_lighting.shadow_strength;
        rt.shadowSoftness = g_lighting.shadow_softness;
        rt.occlusionStrength = g_lighting.occlusion_strength;
        rt.occlusionRadius = g_lighting.occlusion_radius;
        rt.lightStrength = g_lighting.light_strength;
        rt.lightReach = g_lighting.light_reach;
        rt.bounceStrength = g_lighting.bounce_strength;
        rt.bounceRange = g_lighting.bounce_range;
        rt.reflectionStrength = g_lighting.reflection_strength;
        rt.reflectionRangeScale = g_lighting.reflection_range_scale;
        rt.lightGlow = g_lighting.light_glow;
        rt.sunHaze = g_lighting.sun_haze;
        rt.hazeRange = g_lighting.haze_range;
        rt.fogFade = g_lighting.fog_fade;
        rt.distanceFade = g_lighting.distance_fade;
        rt.historyBlend = g_lighting.history_blend;
        rt.filterSteps = g_lighting.filter_steps;
        rt.experiment = g_lighting.experiment;
        rt.darkness = g_lighting.darkness;
        rt.lightBrightness = g_lighting.light_brightness;
        rt.indoorDustDensity = g_lighting.indoor_dust_density;
        rt.indoorDustVisibility = g_lighting.indoor_dust_visibility;
        rt.indoorDustDrift = g_lighting.indoor_dust_drift;
        rt.outdoorDustDensity = g_lighting.outdoor_dust_density;
        rt.outdoorDustVisibility = g_lighting.outdoor_dust_visibility;
        rt.windSpeed = g_lighting.wind_speed;
        rt.windGusts = g_lighting.wind_gusts;
        rt.windX = g_lighting.wind_x;
        rt.windZ = g_lighting.wind_z;
        rt.indoorAmbienceDistance = g_lighting.indoor_ambience_distance;
        rt.outdoorAmbienceDistance = g_lighting.outdoor_ambience_distance;
        rt.indoorDustSize = g_lighting.indoor_dust_size;
        rt.outdoorDustSize = g_lighting.outdoor_dust_size;
        rt.windowAim = g_lighting.window_aim;
        rt.windowAimFromShaft = g_lighting.window_aim_from;
        rt.windowTilt = g_lighting.window_tilt;
        rt.windowTurn = g_lighting.window_turn;
        rt.windowMoveX = g_lighting.window_move_x;
        rt.windowMoveY = g_lighting.window_move_y;
        rt.windowMoveZ = g_lighting.window_move_z;
        rt.lightStone = g_lighting.light_stone;
        rt.glowTest = g_lighting.glow_test;
        rt.shaftStrength = g_lighting.shaft_strength;
        rt.shaftLength = g_lighting.shaft_length;
        rt.drawnBeam = g_lighting.drawn_beam;
        // And room for what follows them: up to eight bodies the dust collides with (renderer
        // patch 0042), and up to twelve beams and windows the renderer finds among the draws
        // (patches 0029 and 0032), so a scene whose list is full still has them all.
        rt.maxLights = 32 + 8 + 12;
        rt.shadowRays = g_lighting.shadow_rays;
        rt.indoorLight = g_lighting.indoor_light;
        rt.glowSize = g_lighting.glow_size;
        application->sharedQueueResources->setRtConfig(rt);
        const bool on = (level > 0) && supported;
        application->workloadQueue->rtEnabled = on;
        std::fprintf(stderr, "[rt] lighting level %d%s, view %s, occlusion %.2f at %.0f, bounce %.2f, reflections %.2f, glow %.4f, haze %.6f\n",
                     on ? level : 0, (level > 0 && !supported) ? " (asked for, device unsupported)" : "",
                     mode < 0 ? "final" : view.c_str(), double(rt.occlusionStrength), double(rt.occlusionRadius), double(rt.bounceStrength),
                     double(rt.reflectionStrength), double(rt.lightGlow), double(rt.sunHaze));
#else
        (void)application;
        if (g_lighting.level > 0 || g_rt_level > 0) {
            std::fprintf(stderr, "[rt] lighting asked for, but this build has no path\n");
        }
#endif
    }

    // Set from the command line before the renderer exists, read once when it is created.
    bool g_present_early_requested = false;

    // One line per configuration the renderer is given, at creation and on every change, so a
    // harness can prove a setting reached the renderer rather than only the settings file.
    // Names, never values from memory.
    void log_graphics_config(const ultramodern::renderer::GraphicsConfig& config) {
        char manual[32] = {};
        const char* rate = "original";
        switch (config.rr_option) {
            case ultramodern::renderer::RefreshRate::Display: rate = "display"; break;
            case ultramodern::renderer::RefreshRate::Manual:
                std::snprintf(manual, sizeof(manual), "manual %d", config.rr_manual_value);
                rate = manual;
                break;
            default: break;
        }
        const char* interface_ratio = "original";
        switch (config.hr_option) {
            case ultramodern::renderer::HUDRatioMode::Full:      interface_ratio = "follow";    break;
            case ultramodern::renderer::HUDRatioMode::Clamp16x9: interface_ratio = "clamp16x9"; break;
            default: break;
        }
        std::fprintf(stderr, "[gfx] refresh rate = %s, aspect = %s, interface = %s\n", rate,
                     config.ar_option == ultramodern::renderer::AspectRatio::Expand ? "expand" : "original",
                     interface_ratio);
    }

    ultramodern::renderer::SetupResult map_setup_result(RT64::Application::SetupResult r) {
        switch (r) {
            case RT64::Application::SetupResult::Success:                 return ultramodern::renderer::SetupResult::Success;
            case RT64::Application::SetupResult::DynamicLibrariesNotFound: return ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
            case RT64::Application::SetupResult::InvalidGraphicsAPI:      return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
            case RT64::Application::SetupResult::GraphicsAPINotFound:     return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
            case RT64::Application::SetupResult::GraphicsDeviceNotFound:  return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
        }
        std::fprintf(stderr, "Unhandled RT64::Application::SetupResult\n");
        return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
    }

    ultramodern::renderer::GraphicsApi map_graphics_api(RT64::UserConfiguration::GraphicsAPI api) {
        switch (api) {
            case RT64::UserConfiguration::GraphicsAPI::D3D12:  return ultramodern::renderer::GraphicsApi::D3D12;
            case RT64::UserConfiguration::GraphicsAPI::Vulkan: return ultramodern::renderer::GraphicsApi::Vulkan;
            case RT64::UserConfiguration::GraphicsAPI::Metal:  return ultramodern::renderer::GraphicsApi::Metal;
            default:                                            return ultramodern::renderer::GraphicsApi::Auto;
        }
    }

} // namespace

oot::renderer::RT64Context::RT64Context(uint8_t* rdram,
                                        ultramodern::renderer::WindowHandle window_handle,
                                        bool developer_mode) {
    // RT64 reads a ROM header for cartridge metadata it does not need here, so it gets a zeroed
    // one. Handing it the real header would put ROM bytes somewhere they do not belong.
    static unsigned char dummy_rom_header[0x40]{};

    RT64::Application::Core appCore{};
    appCore.window = window_handle.window;
    appCore.checkInterrupts = dummy_check_interrupts;

    appCore.HEADER = dummy_rom_header;
    appCore.RDRAM = rdram;
    rdram_ = rdram;

    // Print the base of emulated memory, once, so a crash address can be DECODED rather than
    // guessed at. This costs one line and it is worth it: a faulting address on its own says
    // nothing, and subtracting this from it says immediately whether the read was inside
    // emulated memory, in the guard region the runtime leaves above it to catch bad addresses,
    // or somewhere else entirely. An evening went into fitting a base to two crash addresses
    // and reaching a confident wrong answer, which is exactly what this prevents.
    std::printf("Emulated RAM base %p. Subtract this from a crash address for the emulated one.\n",
                static_cast<const void*>(rdram));

    // The first place in the program that is handed emulated memory, so it is where the state
    // reader gets bound. Everything that wants to know what the game is doing reads it from here.
    oot::game_state::bind(rdram);

    appCore.DMEM = DMEM;
    appCore.IMEM = IMEM;

    appCore.MI_INTR_REG = &MI_INTR_REG;

    appCore.DPC_START_REG = &DPC_START_REG;
    appCore.DPC_END_REG = &DPC_END_REG;
    appCore.DPC_CURRENT_REG = &DPC_CURRENT_REG;
    appCore.DPC_STATUS_REG = &DPC_STATUS_REG;
    appCore.DPC_CLOCK_REG = &DPC_CLOCK_REG;
    appCore.DPC_BUFBUSY_REG = &DPC_BUFBUSY_REG;
    appCore.DPC_PIPEBUSY_REG = &DPC_PIPEBUSY_REG;
    appCore.DPC_TMEM_REG = &DPC_TMEM_REG;

    ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
    appCore.VI_STATUS_REG = &vi->VI_STATUS_REG;
    appCore.VI_ORIGIN_REG = &vi->VI_ORIGIN_REG;
    appCore.VI_WIDTH_REG = &vi->VI_WIDTH_REG;
    appCore.VI_INTR_REG = &vi->VI_INTR_REG;
    appCore.VI_V_CURRENT_LINE_REG = &vi->VI_V_CURRENT_LINE_REG;
    appCore.VI_TIMING_REG = &vi->VI_TIMING_REG;
    appCore.VI_V_SYNC_REG = &vi->VI_V_SYNC_REG;
    appCore.VI_H_SYNC_REG = &vi->VI_H_SYNC_REG;
    appCore.VI_LEAP_REG = &vi->VI_LEAP_REG;
    appCore.VI_H_START_REG = &vi->VI_H_START_REG;
    appCore.VI_V_START_REG = &vi->VI_V_START_REG;
    appCore.VI_V_BURST_REG = &vi->VI_V_BURST_REG;
    appCore.VI_X_SCALE_REG = &vi->VI_X_SCALE_REG;
    appCore.VI_Y_SCALE_REG = &vi->VI_Y_SCALE_REG;

    RT64::ApplicationConfiguration appConfig;
    appConfig.useConfigurationFile = false;

    app = std::make_unique<RT64::Application>(appCore, appConfig);

    const auto& cur_config = ultramodern::renderer::get_graphics_config();
    set_application_user_config(app.get(), cur_config);
    log_graphics_config(cur_config);
    app->userConfig.developerMode = developer_mode;

    // Force gbi depth branches so the game's own LODs do not kick in at a resolution the original
    // never ran at, and scale texture LODs with the output resolution. Both are the reference
    // project's settings and both are about the picture being right rather than fast.
    app->enhancementConfig.f3dex.forceBranch = true;
    app->enhancementConfig.textureLOD.scale = true;

    // Phase 52: a run that freezes itself for a capture also turns the renderer's per frame
    // dither noise off, so two captures of one frozen frame are the same bytes. The noise is
    // seeded from the renderer's own frame count, which keeps advancing while the game does
    // not, and it moved a few pixels by a few levels in every capture until this. The picture
    // the harness compares is then the picture without the noise, on both sides of every
    // comparison; a person's game is untouched.
    if (oot::shots::freeze_requested()) {
        app->emulatorConfig.dither.postBlendNoise = false;
        app->emulatorConfig.dither.postBlendNoiseNegative = false;
        std::fprintf(stderr, "[shot] renderer dither noise off for the frozen capture\n");
    }

    // Presentation mode, and this one is worth the paragraph because it was the dropped frames.
    //
    // RT64's default, skip buffering, presents the image the game's video interface points at
    // once the history shows it was drawn this frame. Present early presents each frame the
    // moment it is rendered, which is what the reference project does unconditionally ("Enable
    // the present early presentation mode for minimal latency"). This project had it behind
    // `--present-early` and took RT64's default otherwise, and the difference is not only
    // latency: the frame reaches the present queue sooner, so more of the game's 50 ms frame is
    // left to render the interpolated frames in before each is due.
    //
    // MEASURED, on the user's own settings and the same scene, counting the frames actually
    // presented in each individual second rather than the average (an average of 59.8 prints as
    // 60, which is how this hid). Skip buffering lost 0.20, 0.40, 0.58 and 0.25 percent of frames
    // across four runs, steadily, which is a dropped frame every three to eight seconds and is
    // what both play-test reports describe as hitching. Present early lost NONE.
    //
    // BUT IT CANNOT BE ON BEFORE THE GAME IS RUNNING, and turning it on here shipped a launcher
    // that was a black window. Present early presents a frame when the game RENDERS one, and in
    // the launcher no game is rendering anything; skip buffering presents the video interface's
    // framebuffer every retrace whether or not the game drew into it. The whole interface is
    // drawn inside RT64's render hook, which runs once per PRESENTED frame, so with nothing
    // presented the launcher loaded its document, reported itself ready, and drew nothing. The
    // log of a build that does this is unmistakable in hindsight and says nothing at the time:
    // "[ui] screen launch", "[ui] interface ready", and then whatever the user does next.
    //
    // So the renderer is created on RT64's default and moves to present early at the first game
    // display list (send_dl), which is the earliest moment the game is certainly rendering. The
    // flag forces it on from creation, for a run that wants to compare.
    if (g_present_early_requested) {
        app->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
    }
    std::fprintf(stderr, "[gfx] presentation = %s\n",
                 app->enhancementConfig.presentation.mode == RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly ? "present early" :
                 app->enhancementConfig.presentation.mode == RT64::EnhancementConfiguration::Presentation::Mode::SkipBuffering ? "skip buffering" : "console");

    switch (cur_config.api_option) {
        case ultramodern::renderer::GraphicsApi::D3D12:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::D3D12;
            break;
        case ultramodern::renderer::GraphicsApi::Vulkan:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;
            break;
        case ultramodern::renderer::GraphicsApi::Metal:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Metal;
            break;
        default:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Automatic;
            break;
    }

    // Before the device exists, or it is too late to ask for them.
    if (g_gpu_breadcrumbs) {
        enable_gpu_breadcrumbs();
    }

    setup_result = map_setup_result(app->setup(window_handle.thread_id));
    chosen_api = map_graphics_api(app->chosenGraphicsAPI);

    if (setup_result != ultramodern::renderer::SetupResult::Success) {
        // Release it rather than leaving a half-built application around. valid() then reports
        // false and the caller reports the setup result, which is the only thing that can say
        // anything useful about why.
        app = nullptr;
        return;
    }

    app->setFullScreen(cur_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);

    // The card's own memory, which the resolution plan's budget is half of. The configuration
    // above was planned without it (the device did not exist yet), so the first screen update
    // plans again with it.
    g_card_memory.store(app->device->getDescription().dedicatedVideoMemory);
    g_replan.store(true);

    // Phase 52: what the device can do and what this build carries, on the trace, so a capture
    // or a report can be read against both. The level is the ray tracing row, which does not
    // exist until phase 58; until then the path is built and the level is 0, Off.
    std::fprintf(stderr, "[rt] device raytracing %s\n",
                 app->device->getCapabilities().raytracing ? "supported" : "unsupported");
#if defined(RT_ENABLED) && RT_ENABLED
    // The one place the capability is recorded for everything that asks, the game included. Only
    // inside the path's own guard: a build without the series cannot trace whatever the device can,
    // and the flag stays false, which is the answer the game needs.
    g_rt_supported = app->device->getCapabilities().raytracing;
#endif
#if defined(RT_ENABLED) && RT_ENABLED
    // Phase 53 to 57: the lighting controls the interface holds (the file the launcher loaded
    // before the renderer existed), with the command line's level and view winning when given.
    // The level is the workload queue's own switch; the rest goes into the configuration the
    // inspector edits and through the same setter, so the queue picks it up on its next frame.
    std::fprintf(stderr, "[rt] path built\n");
    push_lighting(app.get());
#else
    std::fprintf(stderr, "[rt] path not built (the series was not applied)\n");
    if (g_rt_level > 0) {
        std::fprintf(stderr, "[rt] --rt-level %d ignored: this build has no path\n", g_rt_level);
    }
#endif

    // The live application, for swap_chain_texture below. A file scope pointer rather than a
    // parameter threaded through the interface, because the one caller is RT64's own draw hook,
    // which is a C function pointer and carries nothing of ours.
    g_app = app.get();

    // THE PACK THE PERSON ALREADY CHOSE, now that there is something to give it to. The row is
    // on the launcher, so the choice is nearly always made before this point and is waiting as a
    // wish. The constructor runs on the graphics thread (ultramodern's gfx_thread_func builds the
    // context there), so this is the right thread to land it on.
    take_texture_pack();
}

oot::renderer::RT64Context::~RT64Context() {
    g_app = nullptr;
    // A context built after this one has a fresh texture cache, so the pack is owed to it again.
    g_pack_applied_set = false;
    std::lock_guard<std::mutex> lock(g_pack_mutex);
    if (g_pack_wanted_set) {
        g_pack_pending.store(true, std::memory_order_release);
    }
}

void oot::renderer::request_raytracing_level(int level) {
    g_rt_level = level < 0 ? 0 : level;
    g_rt_level_forced = true;
}

int oot::rt_state::level() {
    // A device that cannot trace runs at level 0 whatever the menu says, so the game keeps drawing
    // its own shadows rather than hiding them for traced ones that never arrive. See g_rt_supported.
    if (!g_rt_supported) {
        return 0;
    }
    // AND SO DOES A PRERENDERED ROOM (the user, 2026-09-28). Such a room is a PICTURE with a
    // crude depth mesh laid under it for the game to sort actors against, so the rays have a
    // handful of boxes and a floor while the walls, the battlements and the towers the person
    // is looking at are paint. Every traced pass therefore lit the boxes and stopped dead along
    // the top of one, which is what he saw in the castle courtyard and proved with the white
    // pass beside it: "there is nothing to interact with". No tuning reaches paint, so the
    // passes stand down and the frame is the game's own picture, which is what that picture was
    // for. It answers here rather than only at the renderer so the game also takes its own
    // painted shadows back (patches/actor_shadow.c asks this), or an actor in the courtyard
    // would stand with no shadow at all.
    if (oot::ui::prerendered_room()) {
        return 0;
    }
    return g_rt_level_forced ? g_rt_level : g_lighting.level;
}

bool oot::rt_state::supported() {
    return g_rt_supported;
}

int oot::rt_state::caster_reach() {
    return g_lighting.caster_reach;
}

bool oot::rt_state::light_effects_traced() {
    // The Experiment row's rung that puts the old picture back; the renderer reads the same number
    // from its configuration (renderer patch 0029). The local lights pass is the one that draws
    // these lights, and it runs at level 2 and above with the Light strength row above nothing
    // (Raytracing.hlsl, LightsRayGen), so below that the game keeps its own.
    constexpr int effects_untraced_rung = 37;
    return (oot::rt_state::level() >= 2) && (g_lighting.light_strength > 0.0f) && (g_lighting.experiment != effects_untraced_rung);
}

bool oot::rt_state::window_rays_hidden() {
    return oot::rt_state::light_effects_traced() && (g_lighting.shaft_strength > 0.0f) && (g_lighting.drawn_beam == 0);
}

int oot::rt_state::glow_test() {
    return g_lighting.glow_test;
}
void oot::renderer::request_raytracing_view(const char* view) {
    g_rt_view = (view != nullptr) ? view : "final";
    g_rt_view_forced = true;
}

void oot::renderer::apply_lighting(const LightingValues& values) {
    g_lighting = values;
    if (g_app != nullptr && g_app->device != nullptr) {
        push_lighting(g_app);
    }
}

// THE TEXTURE PACK (2026-09-30). RT64 already carries the whole replacement system, so this is
// a wiring job and deliberately nothing more: `ReplacementDirectory` takes the pack's folder or
// its zip, the texture cache reads the pack's own rt64.json, and a texture is swapped by its
// hash as the game asks for it. The reference project does the same call from its mod list
// (upstream/Zelda64Recomp/src/main/rt64_render_context.cpp), which is where the technique comes
// from; the list of paths is ours.
//
// ASKED FOR HERE, ON ANY THREAD; APPLIED BY take_texture_pack, ON THE GRAPHICS THREAD (see the
// note on g_pack_mutex). The row is written on every settings write, and the same path twice is
// no change, so a person moving the volume never reloads every texture in the game.
void oot::renderer::apply_texture_pack(const std::filesystem::path& pack, int detail_percent) {
    std::lock_guard<std::mutex> lock(g_pack_mutex);
    if (g_pack_wanted_set && (g_pack_wanted == pack) && (g_pack_wanted_detail == detail_percent)) {
        return;
    }
    g_pack_wanted = pack;
    g_pack_wanted_detail = detail_percent;
    g_pack_wanted_set = true;
    g_pack_pending.store(true, std::memory_order_release);
}

namespace {

    // PLAN AGAIN WHEN WHAT THE PLAN RESTS ON HAS MOVED: the window (a resize, fullscreen, the
    // launcher becoming the game), the traced lighting switched on or off, or a picture change
    // the settings raised. Graphics thread, once per screen update. Published only when what is
    // drawn actually changed, since a window dragged by a few pixels mostly does not cross a
    // step of 240 lines, and a publish discards every framebuffer.
    //
    // It applies the configuration update_config last applied, never the runtime's newest: a
    // newer one may carry an antialiasing change whose targets update_config has not rebuilt
    // yet, and publishing that first is the crash described there.
    void replan_if_needed(RT64::Application* app) {
        if ((app == nullptr) || !g_applied_config_set || (app->sharedQueueResources == nullptr)) {
            return;
        }
        g_window_width.store(static_cast<int>(app->sharedQueueResources->swapChainWidth));
        g_window_height.store(static_cast<int>(app->sharedQueueResources->swapChainHeight));

        int width = 0;
        int height = 0;
        plan_window(width, height);
        const bool traced = planned_traced();
        const bool asked = g_replan.exchange(false);
        if (!asked && (width == g_planned_width) && (height == g_planned_height) && (traced == g_planned_traced)) {
            return;
        }
        g_planned_width = width;
        g_planned_height = height;
        g_planned_traced = traced;

        const double multiplierBefore = app->userConfig.resolutionMultiplier;
        const int downsampleBefore = app->userConfig.downsampleMultiplier;
        set_application_user_config(app, g_applied_config);
        if (asked || (app->userConfig.resolutionMultiplier != multiplierBefore) ||
            (app->userConfig.downsampleMultiplier != downsampleBefore)) {
            app->updateUserConfig(true);
        }
    }

    void take_texture_pack() {
        if (!g_pack_pending.load(std::memory_order_acquire)) {
            return;
        }
        if ((g_app == nullptr) || (g_app->textureCache == nullptr)) {
            // No renderer yet: the wish stays pending for the context that is about to exist.
            return;
        }
        g_pack_pending.store(false, std::memory_order_release);
        std::filesystem::path pack;
        int detail = 100;
        {
            std::lock_guard<std::mutex> lock(g_pack_mutex);
            pack = g_pack_wanted;
            detail = g_pack_wanted_detail;
        }
        // The detail only matters while a pack is on: Off with a different detail is still Off.
        if (g_pack_applied_set && (g_pack_applied == pack) &&
            (pack.empty() || (g_pack_applied_detail == detail))) {
            return;
        }
        g_pack_applied = pack;
        g_pack_applied_detail = detail;
        g_pack_applied_set = true;

        // RT64's loader expects every texture already queued to have reached the GPU before the
        // replacements under it are swapped (its own words, in clearReplacementDirectories). This
        // thread is the one that queues them, so nothing new can arrive while it waits.
        g_app->textureCache->waitForGPUUploads();
        const auto start = std::chrono::steady_clock::now();
        const auto took = [&start]() {
            return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count());
        };

        if (pack.empty()) {
            g_app->textureCache->clearReplacementDirectories();
            std::fprintf(stderr, "[packs] none, in %lld ms\n", took());
            return;
        }
        // The Texture detail row, set before the load so every texture the pack streams from here
        // on is read at it. Patch 0025 of the renderer series carries the setting; a renderer
        // built without the series reads every pack as made.
#ifdef RT64_REPLACEMENT_DETAIL
        g_app->textureCache->setReplacementDetail(static_cast<uint32_t>(detail));
#endif
        // A PACK IS A STRANGER'S FOLDER AND MAY NOT BE READABLE, so this can never be allowed to
        // take the program down with it. The renderer walks the pack with std::filesystem, which
        // THROWS rather than returning a code: the first pack tried here was 45,055 files deep
        // enough that its paths passed MAX_PATH, and `FileSystemDirectoryIterator::increment`
        // threw out through `loadReplacementDirectory` and killed the process before a frame was
        // drawn. A person who drops a folder in and finds the game will not start has no way at
        // all to connect the two.
        //
        // So a refused pack is put down at once and the reason goes in the log. It is not tried
        // again while the row stays on it, because the wish has not changed; the row moving, or
        // the next start, tries again, and costs a log line rather than the program.
        bool ok = false;
        try {
            ok = g_app->textureCache->loadReplacementDirectory(RT64::ReplacementDirectory(pack));
        }
        catch (const std::exception& e) {
            std::fprintf(stderr, "[packs] REFUSED %s: %s\n", pack.string().c_str(), e.what());
            try {
                g_app->textureCache->clearReplacementDirectories();
            }
            catch (const std::exception&) {
            }
            return;
        }
        std::fprintf(stderr, "[packs] %s %s at %d%% detail, in %lld ms\n", ok ? "loaded" : "REFUSED",
                     pack.string().c_str(), detail, took());
    }

} // namespace

void oot::renderer::set_picture_inset(int left, int right) {
    if (g_app == nullptr || g_app->sharedQueueResources == nullptr) {
        return;
    }
    g_app->sharedQueueResources->setPictureInset(static_cast<uint32_t>(std::max(left, 0)), static_cast<uint32_t>(std::max(right, 0)));
}

void oot::renderer::set_picture(const PictureValues& values) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_picture_mutex);
        changed = (values.fixed_height != g_picture.fixed_height) || (values.aspect != g_picture.aspect);
        g_picture = values;
    }
    // The runtime's configuration does not carry these, so a change to them alone would never
    // reach update_config. It used to be applied right here, on whichever thread pushed the
    // settings; it is the graphics thread's job now (update_screen's re-plan), for the reason
    // at g_replan.
    if (changed) {
        g_replan.store(true);
    }
}

double oot::renderer::frame_aspect() {
    int windowWidth = 0;
    int windowHeight = 0;
    plan_window(windowWidth, windowHeight);
    // Never narrower than 4:3: an expanded view in a tall window keeps the console's width, as
    // the renderer's own Expand does.
    return std::max(picture_aspect(ultramodern::renderer::get_graphics_config(), picture_now(), windowWidth, windowHeight),
                    4.0 / 3.0);
}

oot::renderer::ResolutionPlan oot::renderer::plan_resolution(const ultramodern::renderer::GraphicsConfig& config,
                                                             const PictureValues& picture) {
    // The console drew 240 lines; every size here is a multiple of that.
    constexpr int CONSOLE_LINES = 240;
    constexpr uint64_t MB = 1024ull * 1024ull;

    ResolutionPlan plan;
    plan.asked = std::max(config.ds_option, 1);

    int windowWidth = 0;
    int windowHeight = 0;
    plan_window(windowWidth, windowHeight);

    // The lines shown, before downsampling. Match the window is the smallest multiple of 240
    // that covers the window, which is what RT64's window integer scale worked out for itself.
    int lines = CONSOLE_LINES;
    if (picture.fixed_height > 0) {
        lines = picture.fixed_height;
    }
    else if (config.res_option == ultramodern::renderer::Resolution::Original2x) {
        lines = 2 * CONSOLE_LINES;
    }
    else if (config.res_option == ultramodern::renderer::Resolution::Auto) {
        lines = CONSOLE_LINES * std::max((windowHeight + CONSOLE_LINES - 1) / CONSOLE_LINES, 1);
    }
    plan.scale = double(lines) / double(CONSOLE_LINES);

    const double aspect = picture_aspect(config, picture, windowWidth, windowHeight);

    int samples = 1;
    switch (config.msaa_option) {
        case ultramodern::renderer::Antialiasing::MSAA2X: samples = 2; break;
        case ultramodern::renderer::Antialiasing::MSAA4X: samples = 4; break;
        case ultramodern::renderer::Antialiasing::MSAA8X: samples = 8; break;
        default: break;
    }
    const uint64_t perPixel = 8 + uint64_t(samples - 1) * 44 + (planned_traced() ? 1050 : 0);
    const auto estimate = [&](int factor, int& width, int& height) {
        height = lines * factor;
        width = static_cast<int>(double(height) * aspect + 0.5);
        return 300 * MB + uint64_t(width) * uint64_t(height) * perPixel;
    };

    // Half the card: the rest is the texture pack's (RT64 lets its pool grow to two thirds of
    // the card, though the packs measured use one to two GB), the interface, the other programs
    // on the desktop, and room for the driver. Before the card is known nothing is drawn above
    // the size shown, which is only ever the moment before the device exists.
    plan.budget = g_card_memory.load() / 2;
    plan.used = plan.asked;
    if (plan.budget == 0) {
        plan.used = 1;
    }
    while ((plan.used > 1) && (estimate(plan.used, plan.width, plan.height) > plan.budget)) {
        plan.used /= 2;
    }
    plan.bytes = estimate(plan.used, plan.width, plan.height);
    return plan;
}

void oot::renderer::request_gpu_breadcrumbs() {
    g_gpu_breadcrumbs = true;
}

void oot::renderer::report_gpu_state() {
    if (g_app == nullptr || g_app->device == nullptr) {
        return;
    }
    if (g_app->chosenGraphicsAPI != RT64::UserConfiguration::GraphicsAPI::D3D12) {
        return;
    }

    // This renderer is D3D12 only (stack.md), so the device behind plume's interface is its D3D12
    // one, and the removed reason and the breadcrumbs are that device's own.
    auto* device = static_cast<plume::D3D12Device*>(g_app->device.get());
    if (device->d3d == nullptr) {
        return;
    }
    const HRESULT reason = device->d3d->GetDeviceRemovedReason();
    if (SUCCEEDED(reason)) {
        std::fprintf(stderr, "\n  the graphics device is present\n");
        return;
    }
    std::fprintf(stderr, "\n  THE GRAPHICS DEVICE WAS REMOVED: %s (0x%08lX)\n",
                 removed_reason_name(reason), static_cast<unsigned long>(reason));

    ID3D12DeviceRemovedExtendedData1* dred = nullptr;
    if (FAILED(device->d3d->QueryInterface(IID_PPV_ARGS(&dred))) || dred == nullptr) {
        std::fprintf(stderr, "  this D3D12 runtime keeps no removed device data\n");
        return;
    }

    // Each command list that was executing: how many commands it finished, and the ones around
    // the one it stopped on. A list that ran to its end, or never started, is not the one.
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs = {};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&crumbs))) {
        bool any = false;
        for (const D3D12_AUTO_BREADCRUMB_NODE1* node = crumbs.pHeadAutoBreadcrumbNode; node != nullptr; node = node->pNext) {
            const UINT done = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
            if (done == 0 || done >= node->BreadcrumbCount || node->pCommandHistory == nullptr) {
                continue;
            }
            any = true;
            std::fprintf(stderr, "  command list %ls stopped at command %u of %u (the marked one was running):\n",
                         node->pCommandListDebugNameW != nullptr ? node->pCommandListDebugNameW : L"(unnamed)",
                         done, node->BreadcrumbCount);
            const UINT first = done > 40 ? done - 40 : 0;
            const UINT last = std::min(node->BreadcrumbCount, done + 3);
            for (UINT i = first; i < last; ++i) {
                std::fprintf(stderr, "    %c %4u  %s\n", i == done ? '>' : ' ', i, breadcrumb_op_name(node->pCommandHistory[i]));
            }
        }
        if (!any) {
            std::fprintf(stderr, "  no command list was mid-way (breadcrumbs %s)\n",
                         g_gpu_breadcrumbs ? "on, every list had finished" : "off: run with --gpu-breadcrumbs");
        }
    }
    else {
        std::fprintf(stderr, "  breadcrumbs off: run with --gpu-breadcrumbs for the command the GPU stopped on\n");
    }

    // The page fault, when there was one: the address, the allocations alive around it and
    // the ones freed shortly before, by the names the renderer gave them.
    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault = {};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault))) {
        std::fprintf(stderr, "  page fault at GPU address 0x%llX\n", static_cast<unsigned long long>(fault.PageFaultVA));
        for (const D3D12_DRED_ALLOCATION_NODE1* node = fault.pHeadExistingAllocationNode; node != nullptr; node = node->pNext) {
            std::fprintf(stderr, "    live allocation: %ls (type %d)\n",
                         node->ObjectNameW != nullptr ? node->ObjectNameW : L"(unnamed)", static_cast<int>(node->AllocationType));
        }
        for (const D3D12_DRED_ALLOCATION_NODE1* node = fault.pHeadRecentFreedAllocationNode; node != nullptr; node = node->pNext) {
            std::fprintf(stderr, "    recently freed: %ls (type %d)\n",
                         node->ObjectNameW != nullptr ? node->ObjectNameW : L"(unnamed)", static_cast<int>(node->AllocationType));
        }
    }
    dred->Release();
}

plume::RenderTexture* oot::renderer::swap_chain_texture(const plume::RenderFramebuffer* framebuffer) {
    if (g_app == nullptr || framebuffer == nullptr || g_app->presentQueue == nullptr) {
        return nullptr;
    }
    RT64::PresentQueue& queue = *g_app->presentQueue;
    if (queue.ext.swapChain == nullptr) {
        return nullptr;
    }
    const size_t count = queue.swapChainFramebuffers.size();
    for (size_t i = 0; i < count; ++i) {
        if (queue.swapChainFramebuffers[i].get() == framebuffer) {
            return queue.ext.swapChain->getTexture(static_cast<uint32_t>(i));
        }
    }
    return nullptr;
}

bool oot::renderer::RT64Context::valid() {
    return static_cast<bool>(app);
}

namespace {
    // The graphics thread's own clock, for the spans the probe records. Only this thread touches
    // these, so they are plain statics rather than atomics.
    std::chrono::steady_clock::time_point g_last_dl_start{};
    bool g_have_last_dl = false;

    // Whether the move to present early has been made. See the presentation paragraph in the
    // constructor: the mode cannot be on while the launcher is up, and the first game display
    // list is the earliest moment the game is certainly rendering.
    bool g_present_early_done = false;
} // namespace

void oot::renderer::RT64Context::send_dl(const OSTask* task) {
    // A texture pack the row asked for since the last frame, landed here before this frame's
    // textures are queued, which is the one moment RT64 allows it (see g_pack_mutex).
    take_texture_pack();

    // The game is rendering, so the presentation mode that keeps the frames evenly paced can go
    // on now. Once, on the graphics thread, which is the thread that owns this.
    if (!g_present_early_done) {
        g_present_early_done = true;
        if (app->enhancementConfig.presentation.mode !=
            RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly) {
            app->enhancementConfig.presentation.mode =
                RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
            app->updateEnhancementConfig();
            std::fprintf(stderr, "[gfx] presentation = present early (the game is rendering now)\n");
        }
    }

    // The gap since the PREVIOUS list arrived, which is the game thread's period as seen from
    // here (probe.h, Span::Submit). Taken before any work so it measures the wait, not the work.
    const auto dl_start = std::chrono::steady_clock::now();
    if (g_have_last_dl) {
        oot::probe::note_span(oot::probe::Span::Submit,
                              static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                        dl_start - g_last_dl_start).count()));
    }
    g_last_dl_start = dl_start;
    g_have_last_dl = true;

    app->state->rsp->reset();
    app->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);
    const uint32_t list = task->t.data_ptr & 0x3FFFFFF;

    // Record what we are about to hand over, always. The walk below is off unless the probe
    // is on; this is not, because a crash does not wait for the probe to be enabled.
    oot::dl_check::note_task(static_cast<uint32_t>(task->t.ucode),
                             static_cast<uint32_t>(task->t.data_ptr),
                             static_cast<uint32_t>(task->t.data_size));

    // Look at the list before the renderer walks it. Off unless the probe is on.
    oot::dl_check::scan(app->core.RDRAM, list);

    // Checksum the list either side of the walk. If it differs, something wrote to it while the
    // renderer was reading, which would explain both the intermittency and why every static check
    // of the list comes back clean: at the moment we look, it IS clean.
    const uint32_t before = oot::dl_check::checksum(app->core.RDRAM, list);

    // THE RIGHT THING TO WATCH, which the checksum above was not.
    //
    // The list's own bytes are stable; that was measured and is not in doubt. What can move under
    // us is the MEMORY THE LIST POINTS AT. The game rebuilds its object bank during a scene
    // transition, and if it does that while the renderer is still walking the previous frame's
    // list, every segmented address in that list now refers to memory that has been reused.
    //
    // So snapshot the segment table either side of the walk. A change here means the bank moved
    // while it was being read, which is a race at a level the earlier test never looked at.
    const oot::game_state::Snapshot segs_before = oot::game_state::read();

    // ONLY THE RENDERER'S OWN WALK IS TIMED, and the boundary matters: the guard scan above runs
    // only when the probe is on, so timing the whole function would make every probe run report
    // the probe's own cost as the renderer's. The first version of this measurement did exactly
    // that and put the mean at 3 ms.
    const auto walk_start = std::chrono::steady_clock::now();
    app->processDisplayLists(app->core.RDRAM, list, 0, true);
    oot::probe::note_span(oot::probe::Span::DisplayList,
                          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                    std::chrono::steady_clock::now() - walk_start).count()));

    const oot::game_state::Snapshot segs_after = oot::game_state::read();
    oot::dl_check::note_segment_move(segs_before, segs_after);

    oot::dl_check::note_mismatch(before, oot::dl_check::checksum(app->core.RDRAM, list));
}

void oot::renderer::RT64Context::send_dummy_workload(uint32_t fb_address) {
    // fb_address is a FRAMEBUFFER address, not a display list.
    //
    // The runtime calls this once per VI while `is_game_started()` is still false, having set a
    // dummy VI mode and origin, so that something is presented before the game's own rendering
    // starts. See ultramodern/src/events.cpp, "If the game hasn't started yet".
    //
    // The first version of this function passed fb_address to processDisplayLists, which read it as
    // a display list pointer and crashed instantly with an access violation reading 0x18, inside
    // RT64::Interpreter::processDisplayLists on the graphics thread. It is worth spelling that out
    // because the parameter's TYPE gives no hint: both are uint32_t game addresses.
    //
    // Doing nothing is correct here. The frame that follows comes from update_screen, which the
    // runtime calls on its own, and there is no game content to draw yet by definition.
    (void)fb_address;
}

void oot::renderer::RT64Context::update_screen() {
    // The launcher has no display lists, only screen updates, so a pack chosen there lands here.
    // Same thread as send_dl, the graphics thread.
    take_texture_pack();
    replan_if_needed(app.get());

    const auto start = std::chrono::steady_clock::now();

    // A recording asked for on the command line starts when the game has run enough updates in
    // play (phase 57, recorder.h). Nothing happens when none was asked.
    oot::recorder::tick(oot::game_state::play_ticks());
    // A video asked for on the command line, the same way (main/video.h).
    oot::video::tick(oot::game_state::play_ticks());
    // Photo mode ends when its row is turned Off or play is left (main/photo.h).
    oot::photo::tick();
    // The lighting sweep the person can start from the Lighting menu (main/sweep.h): one step
    // per presented frame, nothing when none is running.
    oot::sweep::tick();
    // A screenshot or the comparison pair the person asked for (main/screenshots.h): one step per
    // presented frame, nothing when none is under way.
    oot::screenshots::tick();

#if defined(RT_ENABLED) && RT_ENABLED
    // Phase 55: the game's point lights for the traced passes, read out of its light list once
    // a frame and handed to the render queues under their lock. The renderer's own light manager
    // never sees one: this game gives the microcode a per actor directional stand-in for each
    // (patches/light_list.c). A frame of latency against the display list is fine for a torch.
    //
    // AT THE LEVEL IN FORCE, which is the lighting menu's unless the command line forced one.
    // Until 2026-09-24 this read g_rt_level alone, the command line's number, which is 0 in
    // every ordinary launch: the list was read in harness runs and never in the user's play, so
    // no torch and no fairy ever reached the traced passes from the share ("navi only seems to
    // affect link and no other surroundings", the user; the highlight on Link is the game's
    // own actor lighting).
    // THE ROOM CAN CHANGE THE LEVEL, and only this loop sees it happen: entering or leaving a
    // prerendered room takes the passes down or brings them back, so the configuration is
    // pushed on the change and never every frame. The answer read here is already debounced
    // (ui_settings.cpp holds a change for four game frames), so a doorway cannot flicker it.
    {
        static int applied = -1;
        const int now = oot::rt_state::level();
        if (now != applied) {
            applied = now;
            push_lighting(g_app);
        }
    }
    if (oot::rt_state::level() > 0) {
        static std::vector<oot::light_list::Light> found;
        static std::vector<interop::PointLight> lights;
        oot::light_list::read(rdram_, found);
        lights.clear();
        lights.reserve(found.size());
        for (const oot::light_list::Light& l : found) {
            interop::PointLight light{};
            light.position = interop::float3(l.x, l.y, l.z);
            light.direction = interop::float3(0.0f, -1.0f, 0.0f);
            light.diffuseColor = interop::float3(l.r, l.g, l.b);
            light.specularColor = interop::float3(0.0f, 0.0f, 0.0f);
            // A FAIRY REACHES FURTHER THAN THE GAME SAYS. Navi and the other fairies (En_Elf) set a
            // white glow light of radius 100, a few steps, because the console lit only the
            // actors with it; traced, a light of that radius shows on nothing but the ground at
            // her feet (the user, 2026-09-24: "fairies are not adding shadow casting to objects
            // around them ... light sources that must affect all objects and characters and
            // scenery nearby"). A small white glow light is a fairy, and it gets three times its
            // radius; torches (200 and 300, colored) are untouched. The Light reach row scales
            // all of them on top. Over that reach the light falls off as a real light does
            // (Raytracing.hlsl, lightFalloff: an inverse square under a window), so the wider
            // radius is a reach, not "a big expanded circle of light" (the user, 2026-09-24).
            const bool fairy = l.glow && (l.radius <= 100.0f) && (l.r > 0.9f) && (l.g > 0.9f) && (l.b > 0.9f);
            // Twice her radius since 14:10 (three times was "way too big" once the light actually
            // reached the shader; the user, 2026-09-24): bright within fifty units, gone at two hundred.
            light.attenuationRadius = fairy ? l.radius * 2.0f : l.radius;
            light.pointRadius = 6.0f;   // the flame's size, which softens its shadow
            // THE CORE OF THE FALLOFF, the game's own radius, in the exponent field these passes
            // use for nothing else (Raytracing.hlsl, lightFalloff). It is the light's power and
            // never scales with the reach: the reach and the fairy's three times widen only how
            // far the light is allowed to go (the user, 2026-09-24: Navi at Triple reach
            // brightened everything). Half the radius was tried first and left Navi at a fifth
            // of her strength a hundred units from a wall, a few percent on a fogged one, which
            // the user read as "navi still adds no light to walls"; at the whole radius she is at
            // half there, four fifths at fifty units.
            // Half the game's own radius (14:10). The whole radius was set at 13:20 to make her show
            // at all, while an override below (since removed) was holding the core at one unit; with
            // that gone the whole radius lit everything in reach too much ("maybe too much").
            light.attenuationExponent = l.radius * 0.5f;
            // (Until 13:55 a later line set this field to 1 again, the interop default, so every
            // local light's core was one unit and the light died a step from the flame: the
            // sweep's Lights view was black with a dot at Navi. Nothing below may touch it.)
            light.spotFalloffCosine = 1.0f;
            light.spotMaxCosine = 1.0f;
            light.shadowOffset = 0.0f;
            light.flickerIntensity = 0.0f;
            // BIT 4 SAYS THE LIGHT HAS NO HOLDER. The traced local light pass leaves the last
            // 48 units before a light untraced so a flame is not shadowed by its own sconce, and
            // a light hovering closer than that to what it lights is entirely inside it, so
            // nothing can ever shadow it (the user, 2026-09-24: "the light should never pass
            // through an object to reach it").
            //
            // THE TEST IS THE COLOR, not the `fairy` test above, and the game's own source says
            // why. A fairy (z_en_elf.c, EnElf_UpdateLights) carries TWO white lights: a glow of
            // radius 100, which is the disc you see, and, while she is following Link, a second
            // of radius 200 with no glow, placed sixty units above HIM. The second is the one
            // lighting the floor he stands on, and the fairy test misses it on both counts, so
            // marking only the first left his shadow exactly as absent as before. Every other
            // fairy carries the same white pair. The torches in this game are orange (200 and
            // 300 units), so color separates a flame in a holder from a light floating in the
            // air, which is the distinction the clearance is actually about.
            const bool holderless = (l.r > 0.9f) && (l.g > 0.9f) && (l.b > 0.9f);
            light.groupBits = (l.glow ? 2u : 1u) | (holderless ? 4u : 0u);
            lights.push_back(light);
        }

        // THE BODIES THE DUST COLLIDES WITH (2026-10-08, renderer patch 0042; the user, after the
        // first push reached the dust: "not smooth and it feels very choppy ... pushing way past
        // where his body is ... Can we just have normal collision?"). The game's table of the
        // bodies near the eye (patches/air_movers.c: each actor's own collision cylinder, standing
        // or moving) is read every frame, and each body follows the light list with group bit 32,
        // which every light loop in the shaders passes over: no light, only the cylinder's foot,
        // its radius (attenuation radius) and its height (point radius).
        //
        // CARRIED FORWARD BETWEEN UPDATES. The game moves its actors twenty times a second and the
        // picture is drawn far more often, so a body that jumped at each update dragged the motes
        // on its skin with it in steps; that and the old trail's tenth of a second sampling were
        // the choppiness. A table that changed is an update; until the next one each body is moved
        // on by its own motion, for at most one update's time, so when the next arrives it is
        // about where the body was already drawn.
        //
        // SWITCHED OFF FOR NOW (the user, 2026-10-08: "It just doesn't need to interact with
        // elements like enemies or link or whatever. We just can't get it to look right, at least
        // not right now. You can just maybe disable it in place, but we can enable it later"). With
        // no bodies in the list the shaders push no mote; everything else stands as it was, so
        // setting this true brings the collision back exactly as built. The dust's floors and walls
        // are untouched: a mote is still never drawn past the surface the eye sees.
        constexpr bool DustCollidesWithFigures = false;
        if (DustCollidesWithFigures) {
            static std::vector<oot::air_movers::Mover> bodies;
            static std::vector<oot::air_movers::Mover> reading;
            static auto updatedAt = std::chrono::steady_clock::now();
            constexpr float UpdatesPerSecond = 20.0f;
            const auto now = std::chrono::steady_clock::now();
            oot::air_movers::read(rdram_, reading);
            const auto same = [](const oot::air_movers::Mover& a, const oot::air_movers::Mover& b) {
                return (a.x == b.x) && (a.y == b.y) && (a.z == b.z) && (a.dx == b.dx) && (a.dy == b.dy) && (a.dz == b.dz) &&
                       (a.radius == b.radius) && (a.height == b.height);
            };
            if (!std::equal(reading.begin(), reading.end(), bodies.begin(), bodies.end(), same)) {
                // How many bodies, and the first (the player's when there is one), whenever the
                // count or the first one's size changes: enough to tell from the log whether the
                // dust has anything to collide with.
                // (Not its height: the player's follows the pose and changed the line every update.)
                if ((reading.size() != bodies.size()) || (!reading.empty() && !bodies.empty() && (reading[0].radius != bodies[0].radius))) {
                    if (reading.empty()) {
                        std::fprintf(stderr, "[dust] no bodies\n");
                    }
                    else {
                        std::fprintf(stderr, "[dust] %zu bodies; the first %.0f across, %.0f tall, at %.0f %.0f %.0f\n", reading.size(),
                                     reading[0].radius * 2.0f, reading[0].height, reading[0].x, reading[0].y, reading[0].z);
                    }
                }
                bodies = reading;
                updatedAt = now;
            }

            const float since = std::min(std::chrono::duration<float>(now - updatedAt).count(), 1.0f / UpdatesPerSecond);
            for (const oot::air_movers::Mover& body : bodies) {
                interop::PointLight light{};
                const float ahead = since * UpdatesPerSecond;
                light.position = interop::float3(body.x + body.dx * ahead, body.y + body.dy * ahead, body.z + body.dz * ahead);
                light.direction = interop::float3(body.dx * UpdatesPerSecond, body.dy * UpdatesPerSecond, body.dz * UpdatesPerSecond);
                light.diffuseColor = interop::float3(0.0f, 0.0f, 0.0f);
                light.specularColor = interop::float3(0.0f, 0.0f, 0.0f);
                light.attenuationRadius = body.radius;
                light.pointRadius = body.height;
                light.groupBits = 32u;
                lights.push_back(light);
            }
        }

        app->sharedQueueResources->setRtLights(lights);

        // The environment's own sun and ambient (the user's report of 2026-09-24: Navi's light
        // was swinging the searched sun). The brighter of the game's two directional lights is
        // the sun; the other is its fill and stays in the game's own shading.
        oot::light_list::Environment environment{};
        RT64::RaytracingEnvironment rtEnvironment{};
        if (oot::light_list::read_environment(rdram_, environment)) {
            const float sum1 = environment.color1[0] + environment.color1[1] + environment.color1[2];
            const float sum2 = environment.color2[0] + environment.color2[1] + environment.color2[2];
            const float* dir = (sum1 >= sum2) ? environment.dir1 : environment.dir2;
            const float* color = (sum1 >= sum2) ? environment.color1 : environment.color2;
            rtEnvironment.sunDirection = hlslpp::float3(dir[0], dir[1], dir[2]);
            rtEnvironment.sunColor = hlslpp::float3(color[0], color[1], color[2]);
            rtEnvironment.ambientColor = hlslpp::float3(environment.ambient[0], environment.ambient[1], environment.ambient[2]);
            rtEnvironment.valid = (dir[0] != 0.0f) || (dir[1] != 0.0f) || (dir[2] != 0.0f);
            // WHETHER THERE IS A SUN HERE, from the game's own light mode rather than from the
            // renderer's sniff at the draw list (2026-09-24). Outdoors the place follows the time
            // of day; indoors it holds a fixed setting and its directional light is the room's
            // fill, which must be traced as a short contact shadow and not as a sun to the
            // horizon. Unreadable leaves the flag unknown and the renderer's own look stands.
            bool followsTime = false;
            rtEnvironment.sunShadowsKnown = oot::light_list::read_sun_follows_time(rdram_, followsTime);
            rtEnvironment.sunShadows = followsTime;
            // Whether this place's lattice windows are daylight (renderer patch 0030), from the
            // game's own scene; the renderer also leaves them out on the Experiment row's 37.
            rtEnvironment.windowLights = oot::light_list::windows_give_light();
        }
        app->sharedQueueResources->setRtEnvironment(rtEnvironment);

        // Once a second, what the list held, so a light that lands nowhere can be told apart
        // from a light the passes mishandle.
        static auto lastTrace = std::chrono::steady_clock::now() - std::chrono::seconds(2);
        const auto now = std::chrono::steady_clock::now();
        if ((now - lastTrace) >= std::chrono::seconds(1)) {
            lastTrace = now;
            std::fprintf(stderr, "[lights] %zu in the game's list", found.size());
            for (size_t i = 0; (i < found.size()) && (i < 4); i++) {
                std::fprintf(stderr, "; (%.0f %.0f %.0f) r %.0f (%.2f %.2f %.2f)%s", found[i].x, found[i].y, found[i].z,
                             found[i].radius, found[i].r, found[i].g, found[i].b, found[i].glow ? " glow" : "");
            }
            std::fprintf(stderr, "\n");
        }
    }
#endif

    app->updateScreen();
    oot::probe::note_span(oot::probe::Span::Screen,
                          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                    std::chrono::steady_clock::now() - start).count()));
}

void oot::renderer::RT64Context::shutdown() {
    if (app != nullptr) {
        app->end();
    }
}

bool oot::renderer::RT64Context::update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                                               const ultramodern::renderer::GraphicsConfig& new_config) {
    if (old_config == new_config) {
        return false;
    }

    if (new_config.wm_option != old_config.wm_option) {
        app->setFullScreen(new_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
    }

    set_application_user_config(app.get(), new_config);

    // THE ORDER OF THESE TWO IS THE WHOLE FIX for the antialiasing crash a tester reported on
    // 2026-09-20 ("if I turn the antialiasing to off the screen flashes black for a second and
    // it crashes"), reproduced here by walking the row 8x, 4x, 2x, Off: it dies on the step to
    // Off with an access violation reading 0x2C, on the PRESENT thread, inside
    // RT64::VIRenderer::render by way of the D3D12 driver.
    //
    // `updateUserConfig` publishes the new configuration to the render queues under their lock.
    // `updateMultisampling` destroys and rebuilds every render target for the new sample count.
    // Published first, there is a window in which the present thread believes multisampling is
    // off while the targets are still the eight sample ones it was given, and the two disagree
    // about which interpolated target each presented frame should read: the present loop indexes
    // them as `usingMSAA ? i : (i - 1)` and allocates one fewer at None. It then renders from a
    // target that is about to stop existing. That window is why turning antialiasing DOWN
    // crashes and turning it UP does not, and why it needs a few steps to catch: going up only
    // ever adds targets.
    //
    // Rebuilt first and published second, there is no such window, and this is RT64's own order:
    // its inspector calls `app->updateMultisampling()` and only then `setUserConfig`
    // (rt64_state.cpp, at the end of State::inspect). The rebuild reads the sample count from
    // the Application's own copy of the configuration, which `set_application_user_config` above
    // has already updated, so it builds the pattern being switched to rather than the old one.
    if (new_config.msaa_option != old_config.msaa_option) {
        app->updateMultisampling();
    }
    app->updateUserConfig(true);

    log_graphics_config(new_config);
    return true;
}

void oot::renderer::request_present_early() {
    g_present_early_requested = true;
}

void oot::renderer::RT64Context::enable_instant_present() {
    app->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
    app->updateEnhancementConfig();
}

uint32_t oot::renderer::RT64Context::get_display_framerate() const {
    return app->presentQueue->ext.sharedResources->swapChainRate;
}

float oot::renderer::RT64Context::get_resolution_scale() const {
    // The console rendered 240 lines. Everything here is a multiple of that.
    constexpr int ReferenceHeight = 240;

    switch (app->userConfig.resolution) {
        case RT64::UserConfiguration::Resolution::WindowIntegerScale:
            if (app->sharedQueueResources->swapChainHeight > 0) {
                return std::max(
                    float((app->sharedQueueResources->swapChainHeight + ReferenceHeight - 1) / ReferenceHeight),
                    1.0f);
            }
            return 1.0f;
        case RT64::UserConfiguration::Resolution::Manual:
            return float(app->userConfig.resolutionMultiplier);
        default:
            return 1.0f;
    }
}

std::unique_ptr<ultramodern::renderer::RendererContext> oot::renderer::create_render_context(
    uint8_t* rdram,
    ultramodern::renderer::WindowHandle window_handle,
    bool developer_mode) {
    return std::make_unique<oot::renderer::RT64Context>(rdram, window_handle, developer_mode);
}
