#include "ui/ui_render.h"
#include "ui/ui_shell.h"
#include "game/render.h"
#include "main/probe.h"
#include "main/recorder.h"
#include "main/video.h"
#include "main/capture_size.h"
#include "ui/ui_settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "rhi/rt64_render_hooks.h"
#include "contrib/plume/plume_render_interface.h"
#include "contrib/plume/plume_render_interface_builders.h"

#include <RmlUi/Core.h>

#include "ui_vs.hlsl.dxil.h"
#include "ui_ps.hlsl.dxil.h"
#include "ui_mark_ps.hlsl.dxil.h"
#include "ui_gradient_ps.hlsl.dxil.h"
#include "ui/ui_gradient.h"

#include <chrono>

using namespace plume;

namespace {

    // The push constants, as the shader declares them: the transform, then the mark's time.
    struct PushConstants {
        float transform[16];
        float mark[4];
        // A gradient decorator's parameters (shaders/ui.hlsl, UiConstants); zero otherwise.
        float gradient[4];
        float gradient_fn[4];
        float gradient_rep[4];
    };

    // ------------------------------------------------------------------------------------------
    // The device side
    // ------------------------------------------------------------------------------------------

    struct Geometry {
        std::unique_ptr<RenderBuffer> vertices;
        std::unique_ptr<RenderBuffer> indices;
        uint32_t index_count = 0;
        uint32_t vertex_bytes = 0;
    };

    struct Texture {
        std::unique_ptr<RenderTexture> texture;
        std::unique_ptr<RenderDescriptorSet> set;
        // Pixels wait here until the next draw hook, because a texture can be asked for while a
        // document is being loaded on the main thread, where there is no command list to upload
        // with. Cleared once uploaded.
        std::vector<uint8_t> pending;
        uint32_t width = 0;
        uint32_t height = 0;
        bool uploaded = false;
    };

    RenderDevice* g_device = nullptr;

    std::unique_ptr<RenderPipelineLayout> g_layout;
    std::unique_ptr<RenderPipeline> g_pipeline;
    // The mark's pipeline (phase 50): the same layout and vertex shader, its own pixel shader.
    // Bound only for the mark's own draws, with the plain pipeline put back straight after.
    std::unique_ptr<RenderPipeline> g_mark_pipeline;
    constexpr uintptr_t MARK_SHADER_HANDLE = 1;
    // The gradient decorators' pipeline (the menus plan, phase 87): the same layout and vertex
    // shader, PSGradient. A compiled gradient is its parameters and its baked ramp texture.
    std::unique_ptr<RenderPipeline> g_gradient_pipeline;
    struct Gradient {
        float gradient[4];
        float gradient_fn[4];
        float gradient_rep[4];
        uintptr_t ramp = 0;   // a texture handle in g_textures
        std::string ramp_key; // its entry in g_ramps
    };
    std::unordered_map<uintptr_t, Gradient> g_gradients;

    // THE RAMPS ARE SHARED BY THEIR STOPS (phase 114). RmlUi compiles a gradient decorator again
    // whenever its element's decoration is rebuilt, and a fade rebuilds it every frame, during the
    // context's render, inside the render pass, where a new ramp texture cannot be uploaded until
    // the next frame: each frame's new ramp was drawn as plain white and the cut's strip showed as
    // a pale band for the length of every fade. The stops do not change with the opacity (it rides
    // on the vertex colors), so a ramp with the same stops is reused, already uploaded; one no
    // longer used is kept a little while (RAMP_IDLE_FRAMES) for the recompile that follows its
    // release, then released.
    struct SharedRamp {
        uintptr_t texture = 0;
        float start = 0.0f;
        float end = 1.0f;
        int users = 0;
        int idle_frames = 0;
    };
    std::unordered_map<std::string, SharedRamp> g_ramps;
    constexpr int RAMP_IDLE_FRAMES = 120;
    // The parameters of the gradient being drawn, copied into the push constants; null otherwise.
    const Gradient* g_drawing_gradient = nullptr;
    // Seconds since the interface came up, for the gleam. Set once per draw hook.
    float g_time_seconds = 0.0f;
    std::unique_ptr<RenderSampler> g_sampler;
    // The builder, not just the description it produces. It owns the vector of ranges that
    // `descriptorSetDesc` points into, so a builder that goes out of scope leaves the pipeline
    // layout reading freed memory: a crash at device creation, or worse, no crash at all.
    RenderDescriptorSetBuilder g_set_builder;

    // One pixel of white. Geometry with no texture is drawn against this rather than through a
    // second pipeline, which keeps the draw loop to a single state.
    std::unique_ptr<RenderTexture> g_white;
    std::unique_ptr<RenderDescriptorSet> g_white_set;
    bool g_white_uploaded = false;

    std::unordered_map<uintptr_t, Geometry> g_geometry;
    std::unordered_map<uintptr_t, Texture> g_textures;
    uintptr_t g_next_handle = 1;

    // WHAT RMLUI LETS GO OF IS KEPT A FEW FRAMES BEFORE IT IS FREED (the menus plan, phase 100).
    // A frame already submitted can still be reading a texture, a descriptor set or a vertex
    // buffer on the GPU when the library releases it during the next frame's update; freeing at
    // once let a new texture's descriptor take the old one's slot under that frame, and the rows'
    // icons were drawn for a frame in the medallions' leaves (seen by the harness, 2026-10-13, as
    // white specks round the dial when the settings' rows were rebuilt). Kept for longer than the
    // frames that can be in flight, then freed.
    constexpr int RETIRE_FRAMES = 4;
    struct RetiredTexture {
        Texture texture;
        int age = 0;
    };
    struct RetiredGeometry {
        Geometry geometry;
        int age = 0;
    };
    std::vector<RetiredTexture> g_retired_textures;
    std::vector<RetiredGeometry> g_retired_geometry;
    struct Staging {
        std::unique_ptr<RenderBuffer> buffer;
        int age = 0;
    };
    std::vector<Staging> g_staging;

    // A texture out of use: retired, so it outlives the frames still in flight, as ReleaseTexture does.
    void release_texture_now(uintptr_t handle) {
        auto found = g_textures.find(handle);
        if (found != g_textures.end()) {
            g_retired_textures.push_back({ std::move(found->second), 0 });
            g_textures.erase(found);
        }
    }

    void age_retired() {
        // Ramps nobody has used for a while go the way of any released texture.
        for (auto it = g_ramps.begin(); it != g_ramps.end();) {
            if (it->second.users <= 0 && ++it->second.idle_frames > RAMP_IDLE_FRAMES) {
                release_texture_now(it->second.texture);
                it = g_ramps.erase(it);
            }
            else {
                ++it;
            }
        }
        const auto old = [](const auto& r) { return r.age >= RETIRE_FRAMES; };
        for (auto& r : g_retired_textures) {
            ++r.age;
        }
        for (auto& r : g_retired_geometry) {
            ++r.age;
        }
        for (auto& r : g_staging) {
            ++r.age;
        }
        g_staging.erase(std::remove_if(g_staging.begin(), g_staging.end(), old), g_staging.end());
        g_retired_textures.erase(std::remove_if(g_retired_textures.begin(), g_retired_textures.end(), old),
                                 g_retired_textures.end());
        g_retired_geometry.erase(std::remove_if(g_retired_geometry.begin(), g_retired_geometry.end(), old),
                                 g_retired_geometry.end());
    }

    // RECURSIVE, and that is not defensive habit. The draw hook holds this and then calls
    // into RmlUi, which calls straight back into CompileGeometry and GenerateTexture, both
    // of which take it again. A plain std::mutex deadlocks there, and the symptom is not a
    // hang anyone would recognize: the game carries on because it runs on other threads,
    // and only the presented frame stops arriving.
    std::recursive_mutex g_mutex;

    // Set by the hook, read by the layout code.
    int g_width = 0;
    int g_height = 0;
    bool g_ready = false;

    // The card, as it described itself at init.
    std::string g_device_name = "unknown";
    uint64_t g_video_memory = 0;

    // Live only for the duration of one hook call.
    RenderCommandList* g_list = nullptr;
    RenderFramebuffer* g_framebuffer = nullptr;

    bool g_scissor_on = false;
    RenderRect g_scissor;

    // A PASS INTO A PICTURE OF ITS OWN SIZE (the window at a size of its own, below): the projection
    // stays the window's and the viewport is the picture's, so everything is drawn larger by these,
    // and the scissor, which RmlUi gives in window pixels, is scaled by them too. 0 is the screen.
    float g_pass_sx = 1.0f;
    float g_pass_sy = 1.0f;
    int g_pass_width = 0;
    int g_pass_height = 0;

    // RmlUi's transform for the geometry that follows (a rotated glyph, an animated element),
    // row major, identity when none. Composed into the push constants per draw, the way the
    // library's own backends do: projection, then the transform, then the translation.
    float g_transform[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
    bool g_has_transform = false;

    void multiply(const float a[16], const float b[16], float out[16]) {
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                                 a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
            }
        }
    }

    void say(const char* what) {
        std::fprintf(stderr, "[ui] %s\n", what);
    }

    // D3D12 wants each row of a texture upload aligned. Round up to the alignment so the copy
    // footprint is legal; a tighter packing here is the classic source of a texture that arrives
    // skewed by a few pixels per row.
    constexpr uint32_t ROW_ALIGNMENT = 256;

    uint32_t aligned_row(uint32_t width_texels) {
        const uint32_t bytes = width_texels * 4;
        return (bytes + ROW_ALIGNMENT - 1) / ROW_ALIGNMENT * ROW_ALIGNMENT;
    }

    std::unique_ptr<RenderDescriptorSet> make_set(const RenderTexture* texture) {
        auto set = g_set_builder.create(g_device);
        if (set != nullptr) {
            set->setTexture(0, texture, RenderTextureLayout::SHADER_READ);
            set->setSampler(1, g_sampler.get());
        }
        return set;
    }

    void upload(RenderCommandList* list, RenderTexture* texture, const std::vector<uint8_t>& pixels,
                uint32_t width, uint32_t height) {
        if (texture == nullptr || pixels.empty()) {
            return;
        }

        const uint32_t row = aligned_row(width);
        const uint64_t size = static_cast<uint64_t>(row) * height;

        auto staging = g_device->createBuffer(RenderBufferDesc::UploadBuffer(size));
        if (staging == nullptr) {
            say("a texture upload buffer could not be created");
            return;
        }

        uint8_t* dst = static_cast<uint8_t*>(staging->map());
        if (dst == nullptr) {
            say("a texture upload buffer could not be mapped");
            return;
        }
        for (uint32_t y = 0; y < height; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * row,
                        pixels.data() + static_cast<size_t>(y) * width * 4,
                        static_cast<size_t>(width) * 4);
        }
        staging->unmap();

        list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(texture, RenderTextureLayout::COPY_DEST));
        list->copyTextureRegion(
            RenderTextureCopyLocation::Subresource(texture),
            RenderTextureCopyLocation::PlacedFootprint(staging.get(), RenderFormat::R8G8B8A8_UNORM,
                                                       width, height, 1, row / 4));
        list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(texture, RenderTextureLayout::SHADER_READ));

        // The staging buffer has to outlive the copy, which has only been recorded, not executed.
        // KEPT BY AGE, NOT BY COUNT (the menus plan, phase 100). It was the last sixty four, which
        // was right while uploads happened a handful at a time at document load; the shell draws
        // dozens of medallion layers and a page of row glyphs in one frame, past sixty four, and
        // the oldest buffers were freed before their copies ran, so other textures' pixels landed
        // in the medallions' leaves (the harness saw specks round the dial, 2026-10-13). Each is
        // now freed once more frames than can be in flight have passed (age_retired).
        g_staging.push_back({ std::move(staging), 0 });
    }

    // ------------------------------------------------------------------------------------------
    // The frame recorder's capture (the user's ask of 2026-09-22)
    //
    // WHAT IS CAPTURED IS THE SWAP CHAIN ITSELF, which means the recording is the finished
    // picture: the game, our interface over it, and the interpolated frames the renderer inserts
    // between the game's twenty updates a second. That is the whole point. A capture taken from
    // the game's own color target would be the twenty a second the game draws, and a recording
    // made to count how many frames a problem lasts would then be counting the wrong frames.
    //
    // THE COPY IS ONE FRAME BEHIND, ON PURPOSE. copyTextureRegion only RECORDS a copy; the bytes
    // are not there until the GPU has run the list. The present queue executes and then waits
    // (rt64_present_queue.cpp), so by the next call to this hook the previous frame's buffer is
    // finished and can simply be read. A fence per frame would be the alternative and would cost
    // a stall on the present thread every frame, recording or not.
    // ------------------------------------------------------------------------------------------

    struct Capture {
        std::unique_ptr<RenderBuffer> buffer;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t pitch = 0;     // bytes per row, aligned
        bool pending = false;   // holds a copy the GPU has now finished
        uint64_t presented_us = 0;
    };

    // Two, alternating: one being read while the other is being written into.
    Capture g_capture[2];
    int g_capture_slot = 0;

    void release_captures() {
        for (Capture& c : g_capture) {
            c.buffer.reset();
            c.pending = false;
            c.width = 0;
            c.height = 0;
        }
    }

    // Hand the finished copy to the recorder, then free the mapping for reuse.
    void drain_capture(Capture& slot) {
        if (!slot.pending) {
            return;
        }
        slot.pending = false;
        if (slot.buffer == nullptr) {
            return;
        }
        const uint8_t* pixels = static_cast<const uint8_t*>(slot.buffer->map());
        if (pixels != nullptr) {
            oot::recorder::submit(pixels, static_cast<int>(slot.width), static_cast<int>(slot.height),
                                  slot.pitch, slot.presented_us);
            slot.buffer->unmap();
        }
    }

    // Record the copy for this frame into the given slot.
    void take_capture(Capture& slot, RenderCommandList* list, RenderFramebuffer* framebuffer,
                      uint32_t width, uint32_t height, uint64_t presented_us) {
        RenderTexture* texture = oot::renderer::swap_chain_texture(framebuffer);
        if (texture == nullptr) {
            static bool said = false;
            if (!said) {
                said = true;
                say("the presented frame could not be found in the swap chain; nothing to record");
            }
            return;
        }

        const uint32_t pitch = aligned_row(width);
        const uint64_t size = static_cast<uint64_t>(pitch) * height;

        // The picture's size changes when the window does, so the buffer is remade rather than
        // reused when it no longer fits.
        if (slot.buffer == nullptr || slot.width != width || slot.height != height) {
            slot.buffer = g_device->createBuffer(RenderBufferDesc::ReadbackBuffer(size));
            slot.width = width;
            slot.height = height;
            slot.pitch = pitch;
            if (slot.buffer == nullptr) {
                say("a frame readback buffer could not be created");
                return;
            }
        }

        // The swap chain image is in COLOR_WRITE here (the present queue put it there before
        // calling the hook) and must go back to it: the queue's own next barrier moves it from
        // COLOR_WRITE to PRESENT, and leaving it in COPY_SOURCE would make that transition a lie
        // the debug layer catches and the driver does not.
        RenderTextureCopyLocation destination = RenderTextureCopyLocation::PlacedFootprint(
            slot.buffer.get(), RenderFormat::B8G8R8A8_UNORM, width, height, 1, pitch / 4);

        // THE DESTINATION IS A BUFFER AND IT IS GIVEN A TEXTURE ANYWAY. This looks wrong and is
        // load bearing.
        //
        // plume's D3D12 backend ends copyTextureRegion with
        //
        //     setSamplePositions(dstLocation.texture);
        //
        // unconditionally, and setSamplePositions dereferences what it is handed after an assert
        // that is compiled out of a release build. A placed footprint destination is a BUFFER, so
        // that field is null, and the call reads offset 0x6C off a null pointer and takes the
        // present thread with it. The crash handler's report was
        //
        //     access violation (0xC0000005) while reading 0x6C
        //     plume::D3D12CommandList::copyTextureRegion
        //
        // and the recording it was started for produced a folder and no frames.
        //
        // The copy itself never looks at this field: toD3D12 reads `buffer` and the footprint for
        // a PLACED_FOOTPRINT and ignores `texture` entirely. So handing it the source texture
        // satisfies the stray call with an object that is genuinely valid, and multisampling is
        // off on a swap chain image so it takes the branch that resets and does nothing.
        //
        // Done HERE rather than in plume because lib/ is vendored unmodified: a local change there
        // is a fork, which structure.md makes a recorded decision rather than a convenience. If
        // plume is ever updated, check whether this is still needed before removing it.
        destination.texture = texture;

        list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(texture, RenderTextureLayout::COPY_SOURCE));
        list->copyTextureRegion(destination, RenderTextureCopyLocation::Subresource(texture));
        list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(texture, RenderTextureLayout::COLOR_WRITE));

        slot.pending = true;
        slot.presented_us = presented_us;
    }

    // ------------------------------------------------------------------------------------------
    // The video recorder's pictures (2026-10-09, main/video.h)
    //
    // The renderer draws the game's picture a second time at the recording's size when asked
    // (renderer patch 0053) and hands it here in COPY_SOURCE. It is copied into one of a few
    // readback buffers, ONE FRAME BEHIND like the frame recorder's above: the next present finds
    // the copy finished and gives the mapped buffer to the encoder, which converts it on its own
    // thread and hands the slot back. A frame with no free slot is not taken, and the encoder
    // repeats the one before in its place, so the video keeps its time when the encoder is behind.
    // ------------------------------------------------------------------------------------------

    struct VideoSlot {
        std::unique_ptr<RenderBuffer> buffer;
        const uint8_t* mapped = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t pitch = 0;
        uint64_t index = 0;
        // 0 free, 1 holding a copy the GPU runs this frame, 2 with the encoder.
        std::atomic<int> state{ 0 };
    };

    constexpr int VIDEO_SLOTS = 4;
    VideoSlot g_video[VIDEO_SLOTS];
    int g_video_claimed = -1;   // the slot this frame's picture goes into

    void release_video_slot(VideoSlot& slot) {
        if (slot.buffer != nullptr && slot.mapped != nullptr) {
            slot.buffer->unmap();
        }
        slot.mapped = nullptr;
        slot.buffer.reset();
        slot.width = 0;
        slot.height = 0;
    }

    // Give back every buffer the encoder is not holding: when nothing is recording, and when the
    // device goes. The encoder hands its slots back as soon as it has read them, so a short wait
    // covers the one it may be converting.
    void release_video_slots(bool wait_for_encoder) {
        for (VideoSlot& slot : g_video) {
            for (int tries = 0; wait_for_encoder && slot.state.load(std::memory_order_acquire) == 2 && tries < 200; ++tries) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (slot.state.load(std::memory_order_acquire) == 2) {
                // Still being read: the buffer is left to the process rather than freed under it.
                (void)slot.buffer.release();
                slot.mapped = nullptr;
                continue;
            }
            slot.state.store(0, std::memory_order_release);
            release_video_slot(slot);
        }
        g_video_claimed = -1;
    }

    // Last frame's copies are finished: they go to the encoder. Called by whichever capture point
    // comes first in a frame (the game's picture in the present, or the whole window in the draw
    // hook); a slot is handed over once. With no video wanted, buffers nobody holds are given back.
    void drain_video_slots() {
        for (VideoSlot& slot : g_video) {
            if (slot.state.load(std::memory_order_acquire) == 1) {
                slot.state.store(2, std::memory_order_release);
                std::atomic<int>* state = &slot.state;
                oot::video::submit_picture(slot.mapped, slot.pitch, slot.width, slot.height, slot.index,
                                           [state] { state->store(0, std::memory_order_release); });
            }
        }
        if (!oot::video::wants_pictures()) {
            bool any_held = false;
            bool any_buffer = false;
            for (VideoSlot& slot : g_video) {
                any_held = any_held || (slot.state.load(std::memory_order_acquire) != 0);
                any_buffer = any_buffer || (slot.buffer != nullptr);
            }
            if (any_buffer && !any_held) {
                release_video_slots(false);
            }
        }
    }

    // A free slot for this frame's picture, at the recording's own size, the game's picture alone or
    // the whole window drawn again at that size (the window at size, below). -1 when no slot is free
    // (the encoder is behind, and repeats the last picture for this one) or none is wanted.
    int claim_video_slot(uint32_t window_width, uint32_t window_height, uint32_t& copy_width, uint32_t& copy_height) {
        int free_slot = -1;
        for (int i = 0; i < VIDEO_SLOTS; ++i) {
            if (g_video[i].state.load(std::memory_order_acquire) == 0) {
                free_slot = i;
                break;
            }
        }
        if (free_slot < 0) {
            return -1;
        }

        uint32_t w = 0;
        uint32_t h = 0;
        uint64_t index = 0;
        if (!oot::video::wants_picture(window_width, window_height, w, h, index)) {
            return -1;
        }

        VideoSlot& slot = g_video[free_slot];
        if (slot.buffer == nullptr || slot.width != w || slot.height != h) {
            release_video_slot(slot);
            slot.pitch = aligned_row(w);
            slot.buffer = g_device->createBuffer(RenderBufferDesc::ReadbackBuffer(static_cast<uint64_t>(slot.pitch) * h));
            if (slot.buffer == nullptr) {
                say("a video readback buffer could not be created");
                return -1;
            }
            // Mapped for as long as it lives: a readback buffer may stay mapped, and the encoder
            // reads it from its own thread.
            slot.mapped = static_cast<const uint8_t*>(slot.buffer->map());
            if (slot.mapped == nullptr) {
                say("a video readback buffer could not be mapped");
                slot.buffer.reset();
                return -1;
            }
            slot.width = w;
            slot.height = h;
        }
        slot.index = index;
        copy_width = w;
        copy_height = h;
        return free_slot;
    }

    // A SCREENSHOT (2026-10-09, the Screenshots show row; 2026-10-14, Screenshot size): the game's
    // picture drawn again through the same hook as the video's, so none of this program's interface
    // is in it and an open settings panel does not narrow it, or the whole window drawn again (the
    // window at size, below). Either way at Screenshot size, however small the window. One buffer
    // of its own, copied one frame and handed to the recorder's writer the next, which reads it off
    // the render thread and gives it back (the user, 2026-10-14: "it never freezes the UI"). A video
    // recording the game alone gives up that one frame, and repeats its last picture for it.
    struct ShotCapture {
        std::unique_ptr<RenderBuffer> buffer;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t pitch = 0;
        // 0 nothing, 1 claimed for this frame's picture, 2 copied (the GPU runs it), 3 with the writer.
        int state = 0;
        // Set by the writer, from its own thread, once it has read the pixels.
        std::shared_ptr<std::atomic<bool>> returned;
    };
    ShotCapture g_shot;

    // The largest picture a screenshot is drawn at either way: a texture's own ceiling.
    constexpr uint32_t SHOT_MAX = 16384;

    void release_shot() {
        if (g_shot.state == 3 && g_shot.buffer != nullptr && !(g_shot.returned && g_shot.returned->load(std::memory_order_acquire))) {
            // Still being read (the device going while the writer works): left to the process rather
            // than freed under it, as the video's slots are.
            (void)g_shot.buffer.release();
        }
        else if (g_shot.state == 3 && g_shot.buffer != nullptr) {
            g_shot.buffer->unmap();
        }
        g_shot.buffer.reset();
        g_shot.returned.reset();
        g_shot.state = 0;
    }

    // Once a frame: last frame's copy, finished, goes to the writer; a buffer the writer has given
    // back is let go; and a claim the renderer never filled (no picture drawn that frame) is dropped
    // so the next frame can claim again rather than the screenshot waiting forever.
    void finish_shot() {
        if (g_shot.state == 1) {
            release_shot();
            return;
        }
        if (g_shot.state == 3) {
            if (g_shot.returned && g_shot.returned->load(std::memory_order_acquire)) {
                release_shot();
            }
            return;
        }
        if (g_shot.state != 2 || g_shot.buffer == nullptr) {
            return;
        }
        const uint8_t* pixels = static_cast<const uint8_t*>(g_shot.buffer->map());
        if (pixels == nullptr) {
            release_shot();
            return;
        }
        std::shared_ptr<std::atomic<bool>> returned = std::make_shared<std::atomic<bool>>(false);
        g_shot.returned = returned;
        g_shot.state = 3;
        oot::recorder::submit_snapshot(pixels, static_cast<int>(g_shot.width), static_cast<int>(g_shot.height), g_shot.pitch,
                                       [returned] { returned->store(true, std::memory_order_release); });
    }

    // A readback buffer for a screenshot of width by height, claimed for this frame.
    bool claim_shot(uint32_t width, uint32_t height) {
        g_shot.pitch = aligned_row(width);
        g_shot.buffer = g_device->createBuffer(RenderBufferDesc::ReadbackBuffer(static_cast<uint64_t>(g_shot.pitch) * height));
        if (g_shot.buffer == nullptr) {
            say("a screenshot readback buffer could not be created");
            return false;
        }
        g_shot.width = width;
        g_shot.height = height;
        g_shot.state = 1;
        return true;
    }

    // ------------------------------------------------------------------------------------------
    // THE WINDOW AT A SIZE OF ITS OWN (2026-10-14, the user: screenshots and videos at any size
    // "regardless of whether it's game only or whole window. And if it's on whole window, it even
    // got captures the UI of like the launcher and the menu and all of that stuff in the same
    // resolution, too")
    //
    // Not the window stretched: the window drawn again into a picture of the size asked for. The
    // game's picture is drawn again at the size of the box it fills in the window (all of it, or
    // what a side panel leaves) through the same hook as the video's, and copied into place; the
    // rest is cleared to the black the window shows there; then the interface is drawn over it a
    // second time with the window's projection and the picture's viewport, so every shape and edge
    // is drawn at the larger size rather than scaled up afterward. Its text and images are the
    // window's own glyphs and pictures, sampled larger. Before the game runs (the launcher) there
    // is no game picture, and the interface is drawn over black. One picture a frame, for a
    // screenshot (Screenshots show) or a video (Video shows) set to The whole window, into the same
    // readback buffers as theirs and a frame behind like them, so nothing waits on the GPU.
    // ------------------------------------------------------------------------------------------

    enum class WindowFor { None, Shot, Video };
    struct WindowCapture {
        std::unique_ptr<RenderTexture> texture;
        std::unique_ptr<RenderFramebuffer> framebuffer;
        uint32_t width = 0;
        uint32_t height = 0;
        WindowFor target = WindowFor::None;   // this frame's
        int slot = -1;                        // the video's slot, for a video
        float sx = 1.0f;                      // the picture's size over the window's
        float sy = 1.0f;
        uint32_t box_left = 0;                // where the game's picture goes in it
        uint32_t box_width = 0;
        bool awaiting_picture = false;        // asked of the picture hook this frame
        bool picture_placed = false;          // and copied in
        int idle_frames = 0;                  // frames since one was last taken
    };
    WindowCapture g_window;
    // Whether the renderer's picture hook ran this frame. When it did not (the launcher, before the
    // game), the window at size is decided in the draw hook instead, with no game picture in it.
    bool g_picture_hook_ran = false;
    // A box narrower than this is not drawn: the panel has taken the window.
    constexpr uint32_t MIN_BOX = 16;
    // The texture is let go after this many frames with no window taken (about two seconds), rather
    // than held at a 4K picture's size for one screenshot.
    constexpr int WINDOW_IDLE_FRAMES = 120;

    void release_window() {
        g_window.framebuffer.reset();
        g_window.texture.reset();
        g_window.width = 0;
        g_window.height = 0;
        g_window.target = WindowFor::None;
        g_window.slot = -1;
        g_window.awaiting_picture = false;
        g_window.picture_placed = false;
    }

    // Is a whole window wanted this frame, a screenshot's first, else a video's? If so its buffer is
    // claimed, its picture made ready at the size, and where the game goes worked out.
    bool decide_window(uint32_t window_width, uint32_t window_height) {
        g_window.target = WindowFor::None;
        g_window.slot = -1;
        g_window.awaiting_picture = false;
        g_window.picture_placed = false;
        if (window_width == 0 || window_height == 0) {
            return false;
        }
        uint32_t w = 0;
        uint32_t h = 0;
        WindowFor target = WindowFor::None;
        int slot = -1;
        if (oot::recorder::snapshot_wants(oot::recorder::Shot::WindowAtSize) && g_shot.state == 0) {
            oot::capture::size_at(oot::ui::settings().screenshot_size, window_width, window_height, SHOT_MAX, SHOT_MAX, w, h);
            if (!claim_shot(w, h)) {
                return false;
            }
            target = WindowFor::Shot;
        }
        else if (oot::video::wants_pictures() && oot::video::records_window()) {
            slot = claim_video_slot(window_width, window_height, w, h);
            if (slot < 0) {
                return false;
            }
            target = WindowFor::Video;
        }
        else {
            return false;
        }

        if (g_window.texture == nullptr || g_window.width != w || g_window.height != h) {
            release_window();
            g_window.texture = g_device->createTexture(RenderTextureDesc::ColorTarget(w, h, RenderFormat::B8G8R8A8_UNORM));
            const RenderTexture* targets[] = { g_window.texture.get() };
            g_window.framebuffer = (g_window.texture != nullptr)
                                       ? g_device->createFramebuffer(RenderFramebufferDesc(targets, 1))
                                       : nullptr;
            if (g_window.framebuffer == nullptr) {
                say("a picture of the window at its own size could not be made");
                release_window();
                if (target == WindowFor::Shot) {
                    release_shot();
                }
                return false;
            }
            g_window.width = w;
            g_window.height = h;
        }

        g_window.target = target;
        g_window.slot = slot;
        g_window.idle_frames = 0;
        g_window.sx = static_cast<float>(w) / static_cast<float>(window_width);
        g_window.sy = static_cast<float>(h) / static_cast<float>(window_height);
        int left = 0;
        int right = 0;
        oot::renderer::picture_inset(left, right);
        const uint32_t box_left = std::min(w, static_cast<uint32_t>(std::lround(left * g_window.sx)));
        const uint32_t box_right_inset = std::min(w - box_left, static_cast<uint32_t>(std::lround(right * g_window.sx)));
        g_window.box_left = box_left;
        g_window.box_width = w - box_left - box_right_inset;
        return true;
    }

    bool video_picture_size(uint32_t window_width, uint32_t window_height, uint32_t* width, uint32_t* height) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (g_device == nullptr) {
            return false;
        }
        g_picture_hook_ran = true;
        drain_video_slots();
        g_video_claimed = -1;

        finish_shot();
        if (oot::recorder::snapshot_wants(oot::recorder::Shot::Game) && g_shot.state == 0) {
            uint32_t w = 0;
            uint32_t h = 0;
            oot::capture::size_at(oot::ui::settings().screenshot_size, window_width, window_height, SHOT_MAX, SHOT_MAX, w, h);
            if (claim_shot(w, h)) {
                *width = w;
                *height = h;
                return true;
            }
        }
        // The whole window, a screenshot's or a video's, at its own size: its game picture is asked
        // for here and the rest is drawn in the draw hook, after the interface (the window at size).
        if (decide_window(window_width, window_height)) {
            if (g_window.box_width < MIN_BOX) {
                return false;   // the panel leaves no game to draw: the interface over black
            }
            g_window.awaiting_picture = true;
            *width = g_window.box_width;
            *height = g_window.height;
            return true;
        }
        if (!oot::video::wants_pictures() || oot::video::records_window()) {
            return false;
        }
        uint32_t w = 0;
        uint32_t h = 0;
        const int slot = claim_video_slot(window_width, window_height, w, h);
        if (slot < 0) {
            return false;
        }
        g_video_claimed = slot;
        *width = w;
        *height = h;
        return true;
    }

    // This frame's whole window into its readback, after the interface has drawn on the screen:
    // black where the game's picture is not, the interface drawn over it again at the picture's
    // size, and the copy to the screenshot's or the video's buffer. The screen's framebuffer is
    // bound again afterward, with its viewport and scissor, as the draw left them.
    void draw_window(RenderCommandList* list, RenderFramebuffer* screen) {
        RenderTexture* texture = g_window.texture.get();
        const uint32_t w = g_window.width;
        const uint32_t h = g_window.height;
        list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(texture, RenderTextureLayout::COLOR_WRITE));
        list->setFramebuffer(g_window.framebuffer.get());

        const RenderColor black(0.0f, 0.0f, 0.0f, 1.0f);
        if (g_window.picture_placed) {
            RenderRect strips[2];
            uint32_t count = 0;
            const uint32_t right = g_window.box_left + g_window.box_width;
            if (g_window.box_left > 0) {
                strips[count++] = RenderRect(0, 0, static_cast<int>(g_window.box_left), static_cast<int>(h));
            }
            if (right < w) {
                strips[count++] = RenderRect(static_cast<int>(right), 0, static_cast<int>(w), static_cast<int>(h));
            }
            if (count > 0) {
                list->clearColor(0, black, strips, count);
            }
        }
        else {
            list->clearColor(0, black);
        }

        // The interface again, larger: the window's projection on the picture's viewport.
        g_list = list;
        g_framebuffer = g_window.framebuffer.get();
        g_pass_sx = g_window.sx;
        g_pass_sy = g_window.sy;
        g_pass_width = static_cast<int>(w);
        g_pass_height = static_cast<int>(h);
        list->setGraphicsPipelineLayout(g_layout.get());
        list->setPipeline(g_pipeline.get());
        list->setViewports(RenderViewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)));
        list->setScissors(RenderRect(0, 0, static_cast<int>(w), static_cast<int>(h)));
        oot::ui::shell::render_again();
        g_pass_sx = 1.0f;
        g_pass_sy = 1.0f;
        g_pass_width = 0;
        g_pass_height = 0;
        g_list = nullptr;
        g_framebuffer = nullptr;

        list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(texture, RenderTextureLayout::COPY_SOURCE));
        RenderBuffer* buffer = nullptr;
        uint32_t pitch = 0;
        if (g_window.target == WindowFor::Shot && g_shot.state == 1 && g_shot.width == w && g_shot.height == h) {
            buffer = g_shot.buffer.get();
            pitch = g_shot.pitch;
        }
        else if (g_window.target == WindowFor::Video && g_window.slot >= 0 && g_video[g_window.slot].width == w &&
                 g_video[g_window.slot].height == h) {
            buffer = g_video[g_window.slot].buffer.get();
            pitch = g_video[g_window.slot].pitch;
        }
        if (buffer != nullptr) {
            RenderTextureCopyLocation destination =
                RenderTextureCopyLocation::PlacedFootprint(buffer, RenderFormat::B8G8R8A8_UNORM, w, h, 1, pitch / 4);
            destination.texture = texture;   // the stray read take_capture explains
            list->copyTextureRegion(destination, RenderTextureCopyLocation::Subresource(texture));
            if (g_window.target == WindowFor::Shot) {
                g_shot.state = 2;
            }
            else {
                g_video[g_window.slot].state.store(1, std::memory_order_release);
            }
        }

        list->setFramebuffer(screen);
        list->setViewports(RenderViewport(0.0f, 0.0f, static_cast<float>(g_width), static_cast<float>(g_height)));
        list->setScissors(RenderRect(0, 0, g_width, g_height));
        g_window.target = WindowFor::None;
        g_window.slot = -1;
    }

    void video_picture(RenderCommandList* list, RenderTexture* picture, uint32_t width, uint32_t height) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        // The whole window's game picture, into its box. Only a copy here: this runs inside the
        // renderer's own present, which a bound framebuffer would disturb, so the rest is the draw
        // hook's (draw_window).
        if (g_window.awaiting_picture) {
            g_window.awaiting_picture = false;
            if (g_window.texture != nullptr && width == g_window.box_width && height == g_window.height) {
                list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(g_window.texture.get(), RenderTextureLayout::COPY_DEST));
                list->copyTextureRegion(RenderTextureCopyLocation::Subresource(g_window.texture.get()),
                                        RenderTextureCopyLocation::Subresource(picture), g_window.box_left, 0);
                g_window.picture_placed = true;
            }
            return;
        }
        if (g_shot.state == 1) {
            if (g_shot.buffer != nullptr && g_shot.width == width && g_shot.height == height) {
                RenderTextureCopyLocation destination = RenderTextureCopyLocation::PlacedFootprint(
                    g_shot.buffer.get(), RenderFormat::B8G8R8A8_UNORM, width, height, 1, g_shot.pitch / 4);
                destination.texture = picture;   // the stray read take_capture explains
                list->copyTextureRegion(destination, RenderTextureCopyLocation::Subresource(picture));
                g_shot.state = 2;
            }
            else {
                release_shot();
            }
            return;
        }
        if (g_video_claimed < 0) {
            return;
        }
        VideoSlot& slot = g_video[g_video_claimed];
        g_video_claimed = -1;
        if (slot.buffer == nullptr || slot.width != width || slot.height != height) {
            return;
        }

        RenderTextureCopyLocation destination = RenderTextureCopyLocation::PlacedFootprint(
            slot.buffer.get(), RenderFormat::B8G8R8A8_UNORM, width, height, 1, slot.pitch / 4);
        // The same stray read in plume's copy as take_capture explains above: the destination is
        // a buffer and is given the source texture so the call it makes has something valid.
        destination.texture = picture;
        list->copyTextureRegion(destination, RenderTextureCopyLocation::Subresource(picture));
        slot.state.store(1, std::memory_order_release);
    }

    // ------------------------------------------------------------------------------------------
    // The RmlUi side
    // ------------------------------------------------------------------------------------------

    class Backend final : public Rml::RenderInterface {
    public:
        Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                    Rml::Span<const int> indices) override {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            if (g_device == nullptr || vertices.empty() || indices.empty()) {
                return 0;
            }

            Geometry geo;
            geo.index_count = static_cast<uint32_t>(indices.size());
            geo.vertex_bytes = static_cast<uint32_t>(vertices.size() * sizeof(Rml::Vertex));

            geo.vertices = g_device->createBuffer(
                RenderBufferDesc::VertexBuffer(geo.vertex_bytes, RenderHeapType::UPLOAD));
            geo.indices = g_device->createBuffer(
                RenderBufferDesc::IndexBuffer(indices.size() * sizeof(int), RenderHeapType::UPLOAD));
            if (geo.vertices == nullptr || geo.indices == nullptr) {
                return 0;
            }

            void* v = geo.vertices->map();
            if (v != nullptr) {
                std::memcpy(v, vertices.data(), geo.vertex_bytes);
                geo.vertices->unmap();
            }
            void* i = geo.indices->map();
            if (i != nullptr) {
                std::memcpy(i, indices.data(), indices.size() * sizeof(int));
                geo.indices->unmap();
            }

            const uintptr_t handle = g_next_handle++;
            g_geometry[handle] = std::move(geo);
            return static_cast<Rml::CompiledGeometryHandle>(handle);
        }

        void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            auto found = g_geometry.find(static_cast<uintptr_t>(handle));
            if (found != g_geometry.end()) {
                g_retired_geometry.push_back({ std::move(found->second), 0 });
                g_geometry.erase(found);
            }
        }

        void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
                            Rml::TextureHandle texture) override {
            draw_geometry(handle, translation, texture);
        }

        // RmlUi's shader decorator (`decorator: shader(mark)` in mark.rcss). The one shader we
        // have is the mark; anything else is refused with a line in the trace, so a typo in a
        // stylesheet reads as a missing element rather than as a blank nobody can explain.
        Rml::CompiledShaderHandle CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) override {
            Rml::String value;
            auto found = parameters.find("value");
            if (found != parameters.end()) {
                value = found->second.Get<Rml::String>();
            }
            if (name == "shader" && value == "mark" && g_mark_pipeline != nullptr) {
                return static_cast<Rml::CompiledShaderHandle>(MARK_SHADER_HANDLE);
            }
            if ((name == "linear-gradient" || name == "radial-gradient") && g_gradient_pipeline != nullptr) {
                return compile_gradient(name == "radial-gradient", parameters);
            }
            std::fprintf(stderr, "[ui] no shader named '%s' ('%s'), so that decorator was not drawn\n",
                         value.c_str(), name.c_str());
            return 0;
        }

        void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry,
                          Rml::Vector2f translation, Rml::TextureHandle texture) override {
            if (g_list == nullptr) {
                return;
            }
            auto gradient = g_gradients.find(static_cast<uintptr_t>(shader));
            if (gradient != g_gradients.end()) {
                g_list->setPipeline(g_gradient_pipeline.get());
                g_drawing_gradient = &gradient->second;
                draw_geometry(geometry, translation, static_cast<Rml::TextureHandle>(gradient->second.ramp));
                g_drawing_gradient = nullptr;
                g_list->setPipeline(g_pipeline.get());
                return;
            }
            if (static_cast<uintptr_t>(shader) != MARK_SHADER_HANDLE || g_mark_pipeline == nullptr) {
                return;
            }
            g_list->setPipeline(g_mark_pipeline.get());
            draw_geometry(geometry, translation, texture);
            g_list->setPipeline(g_pipeline.get());
        }

        void ReleaseShader(Rml::CompiledShaderHandle shader) override {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            auto found = g_gradients.find(static_cast<uintptr_t>(shader));
            if (found != g_gradients.end()) {
                auto ramp = g_ramps.find(found->second.ramp_key);
                if (ramp != g_ramps.end() && --ramp->second.users <= 0) {
                    ramp->second.users = 0;
                    ramp->second.idle_frames = 0;
                }
                g_gradients.erase(found);
            }
        }

        // A gradient decorator: its stops baked into a ramp texture once, and its geometry's
        // parameters kept for the draw. Read the way RmlUi's reference renderer reads them
        // (Backends/RmlUi_Renderer_GL3.cpp): linear takes p0 and p1, radial a center and a radius,
        // both a color_stop_list whose positions are plain numbers along the gradient.
        Rml::CompiledShaderHandle compile_gradient(bool radial, const Rml::Dictionary& parameters) {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            auto list = parameters.find("color_stop_list");
            if (list == parameters.end() || list->second.GetType() != Rml::Variant::COLORSTOPLIST) {
                return 0;
            }
            std::vector<oot::ui::gradient::Stop> stops;
            for (const Rml::ColorStop& stop : list->second.GetReference<Rml::ColorStopList>()) {
                oot::ui::gradient::Stop s{};
                s.position = stop.position.number;
                s.rgba[0] = stop.color.red / 255.0f;
                s.rgba[1] = stop.color.green / 255.0f;
                s.rgba[2] = stop.color.blue / 255.0f;
                s.rgba[3] = stop.color.alpha / 255.0f;
                stops.push_back(s);
            }
            if (stops.empty()) {
                return 0;
            }
            // The stops as a key: their positions and colors exactly as RmlUi gave them.
            std::string key;
            for (const oot::ui::gradient::Stop& st : stops) {
                char part[96];
                std::snprintf(part, sizeof(part), "%.6g:%.4g,%.4g,%.4g,%.4g;", st.position, st.rgba[0], st.rgba[1],
                              st.rgba[2], st.rgba[3]);
                key += part;
            }
            auto shared = g_ramps.find(key);
            if (shared == g_ramps.end()) {
                const oot::ui::gradient::Ramp baked = oot::ui::gradient::bake(stops);
                const Rml::TextureHandle made = GenerateTexture(
                    Rml::Span<const Rml::byte>(baked.pixels.data(), baked.pixels.size()),
                    Rml::Vector2i(oot::ui::gradient::RAMP_WIDTH, 1));
                if (made == 0) {
                    return 0;
                }
                shared = g_ramps.emplace(key, SharedRamp{ static_cast<uintptr_t>(made), baked.start, baked.end, 0, 0 }).first;
            }
            ++shared->second.users;
            shared->second.idle_frames = 0;
            struct {
                float start;
                float end;
            } ramp{ shared->second.start, shared->second.end };
            const uintptr_t texture = shared->second.texture;

            Gradient g{};
            const bool repeating = Rml::Get(parameters, "repeating", false);
            if (radial) {
                const Rml::Vector2f center = Rml::Get(parameters, "center", Rml::Vector2f(0.f));
                const Rml::Vector2f radius = Rml::Get(parameters, "radius", Rml::Vector2f(1.f));
                g.gradient[0] = center.x;
                g.gradient[1] = center.y;
                g.gradient[2] = radius.x != 0.f ? 1.f / radius.x : 0.f;
                g.gradient[3] = radius.y != 0.f ? 1.f / radius.y : 0.f;
                g.gradient_fn[0] = 2.f;
            }
            else {
                const Rml::Vector2f p0 = Rml::Get(parameters, "p0", Rml::Vector2f(0.f));
                const Rml::Vector2f p1 = Rml::Get(parameters, "p1", Rml::Vector2f(0.f));
                g.gradient[0] = p0.x;
                g.gradient[1] = p0.y;
                g.gradient[2] = p1.x - p0.x;
                g.gradient[3] = p1.y - p0.y;
                g.gradient_fn[0] = 1.f;
            }
            g.gradient_fn[1] = repeating ? 1.f : 0.f;
            g.gradient_fn[2] = ramp.start;
            g.gradient_fn[3] = ramp.end;
            g.gradient_rep[0] = stops.front().position;
            g.gradient_rep[1] = stops.back().position;
            g.ramp = texture;
            g.ramp_key = key;

            uintptr_t handle = g_next_handle++;
            if (handle == MARK_SHADER_HANDLE) {
                handle = g_next_handle++;
            }
            g_gradients[handle] = g;
            return static_cast<Rml::CompiledShaderHandle>(handle);
        }

    private:
        void draw_geometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
                           Rml::TextureHandle texture) {
            if (g_list == nullptr) {
                return;
            }

            auto found = g_geometry.find(static_cast<uintptr_t>(handle));
            if (found == g_geometry.end()) {
                return;
            }
            const Geometry& geo = found->second;

            // A TEXTURE NOT YET ON THE CARD IS NOT DRAWN, rather than drawn with the white pixel
            // (the user, 2026-10-13: "pressing R1 and L1 causes a white square flash on center
            // medallion as it switches"). RmlUi rasterizes an SVG decorator at its new size during
            // its own render, inside the render pass, where the pixels cannot be copied up until the
            // next frame begins; the medallion that becomes current grows, so every turn of the dial
            // made new layer textures and each was drawn for that one frame as a white square. Every
            // texture here is GenerateTexture's and is uploaded at the start of the next frame, so
            // the cost is that layer missing for one frame. The white pixel stays for geometry that
            // asks for no texture at all.
            RenderDescriptorSet* set = g_white_set.get();
            if (texture != 0) {
                auto t = g_textures.find(static_cast<uintptr_t>(texture));
                if (t == g_textures.end() || t->second.set == nullptr || !t->second.uploaded) {
                    return;
                }
                set = t->second.set.get();
            }
            if (set == nullptr) {
                return;
            }

            // Pixels to clip space. Y is flipped because the interface measures downward from
            // the top left and clip space does not. The translation is applied to the vertices
            // first, then RmlUi's transform when it has set one, then the projection: clip =
            // P * T * Tr * v, composed here so the push constants stay a single range.
            const float w = (g_width > 0) ? static_cast<float>(g_width) : 1.0f;
            const float h = (g_height > 0) ? static_cast<float>(g_height) : 1.0f;
            const float projection[16] = {
                2.0f / w, 0.0f,      0.0f, -1.0f,
                0.0f,     -2.0f / h, 0.0f, 1.0f,
                0.0f,     0.0f,      1.0f, 0.0f,
                0.0f,     0.0f,      0.0f, 1.0f,
            };
            const float translate[16] = {
                1.0f, 0.0f, 0.0f, translation.x,
                0.0f, 1.0f, 0.0f, translation.y,
                0.0f, 0.0f, 1.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f,
            };

            // Row major, and the shader says `row_major` so it is read the way it is written.
            // HLSL packs a float4x4 column major by default, which silently transforms every
            // vertex by the transpose and puts the whole interface somewhere off screen.
            PushConstants constants = {};
            if (g_has_transform) {
                float pt[16];
                multiply(projection, g_transform, pt);
                multiply(pt, translate, constants.transform);
            }
            else {
                multiply(projection, translate, constants.transform);
            }
            constants.mark[0] = g_time_seconds;
            if (g_drawing_gradient != nullptr) {
                std::memcpy(constants.gradient, g_drawing_gradient->gradient, sizeof(constants.gradient));
                std::memcpy(constants.gradient_fn, g_drawing_gradient->gradient_fn, sizeof(constants.gradient_fn));
                std::memcpy(constants.gradient_rep, g_drawing_gradient->gradient_rep, sizeof(constants.gradient_rep));
            }

            g_list->setGraphicsPushConstants(0, &constants, 0, sizeof(constants));
            g_list->setGraphicsDescriptorSet(set, 0);

            const RenderVertexBufferView vertex_view(geo.vertices->at(0), geo.vertex_bytes);
            const RenderInputSlot slot(0, sizeof(Rml::Vertex));
            g_list->setVertexBuffers(0, &vertex_view, 1, &slot);

            const RenderIndexBufferView index_view(geo.indices->at(0),
                                                   geo.index_count * sizeof(int),
                                                   RenderFormat::R32_UINT);
            g_list->setIndexBuffer(&index_view);
            g_list->drawIndexedInstanced(geo.index_count, 1, 0, 0, 0);
        }

    public:
        Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override {
            // ONE bitmap format, and it is ours: the raw ".rgba" the recorder writes beside a saved
            // moment's thumbnail ("RGBA", width, height, the pixels), read here and handed to
            // GenerateTexture like any rasterized SVG. Everything else stays deliberately
            // unsupported: the interface is drawn from color, type and vector, and a PNG decoder is
            // a dependency this project does not take. Saying so is better than a blank texture.
            // A "?v=..." after the name is the picture's version (reflect_moments), there so a
            // picture written again under the same name is a new source to the interface's cache
            // rather than the old one; it is not part of the file's name.
            const std::string file = source.substr(0, source.find('?'));
            if (file.size() > 5 && file.compare(file.size() - 5, 5, ".rgba") == 0) {
                std::ifstream in(std::filesystem::path(file), std::ios::binary);
                char magic[4] = {};
                uint32_t dims[2] = {};
                if (in.read(magic, 4) && in.read(reinterpret_cast<char*>(dims), sizeof(dims)) &&
                    magic[0] == 'R' && magic[1] == 'G' && magic[2] == 'B' && magic[3] == 'A' &&
                    dims[0] > 0 && dims[0] <= 1024 && dims[1] > 0 && dims[1] <= 1024) {
                    std::vector<Rml::byte> pixels(static_cast<size_t>(dims[0]) * dims[1] * 4);
                    if (in.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()))) {
                        dimensions = Rml::Vector2i(static_cast<int>(dims[0]), static_cast<int>(dims[1]));
                        return GenerateTexture(Rml::Span<const Rml::byte>(pixels.data(), pixels.size()), dimensions);
                    }
                }
                std::fprintf(stderr, "[ui] the thumbnail '%s' could not be read\n", source.c_str());
                return 0;
            }
            std::fprintf(stderr, "[ui] no bitmap loader, so '%s' was not drawn\n", source.c_str());
            return 0;
        }

        Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
                                           Rml::Vector2i dimensions) override {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            if (g_device == nullptr || dimensions.x <= 0 || dimensions.y <= 0) {
                return 0;
            }

            Texture tex;
            tex.width = static_cast<uint32_t>(dimensions.x);
            tex.height = static_cast<uint32_t>(dimensions.y);
            tex.texture = g_device->createTexture(
                RenderTextureDesc::Texture2D(tex.width, tex.height, 1, RenderFormat::R8G8B8A8_UNORM));
            if (tex.texture == nullptr) {
                return 0;
            }
            tex.pending.assign(source.begin(), source.end());
            tex.set = make_set(tex.texture.get());
            if (tex.set == nullptr) {
                return 0;
            }

            const uintptr_t handle = g_next_handle++;
            g_textures[handle] = std::move(tex);
            return static_cast<Rml::TextureHandle>(handle);
        }

        void ReleaseTexture(Rml::TextureHandle handle) override {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            auto found = g_textures.find(static_cast<uintptr_t>(handle));
            if (found != g_textures.end()) {
                g_retired_textures.push_back({ std::move(found->second), 0 });
                g_textures.erase(found);
            }
        }

        void EnableScissorRegion(bool enable) override {
            g_scissor_on = enable;
            apply_scissor();
        }

        // RmlUi's transform property (a rotated glyph, an animated element). Null is none. The
        // library's matrix is read row by row into our row major form.
        void SetTransform(const Rml::Matrix4f* transform) override {
            g_has_transform = (transform != nullptr);
            if (transform == nullptr) {
                return;
            }
            for (int r = 0; r < 4; ++r) {
                const Rml::Vector4f row = transform->GetRow(r);
                g_transform[r * 4 + 0] = row.x;
                g_transform[r * 4 + 1] = row.y;
                g_transform[r * 4 + 2] = row.z;
                g_transform[r * 4 + 3] = row.w;
            }
        }

        void SetScissorRegion(Rml::Rectanglei region) override {
            g_scissor = RenderRect(region.Left(), region.Top(), region.Right(), region.Bottom());
            apply_scissor();
        }

    private:
        static void apply_scissor() {
            if (g_list == nullptr) {
                return;
            }
            if (g_pass_width > 0) {
                g_list->setScissors(g_scissor_on
                    ? RenderRect(static_cast<int>(std::floor(g_scissor.left * g_pass_sx)),
                                 static_cast<int>(std::floor(g_scissor.top * g_pass_sy)),
                                 static_cast<int>(std::ceil(g_scissor.right * g_pass_sx)),
                                 static_cast<int>(std::ceil(g_scissor.bottom * g_pass_sy)))
                    : RenderRect(0, 0, g_pass_width, g_pass_height));
            }
            else if (g_scissor_on) {
                g_list->setScissors(g_scissor);
            }
            else {
                g_list->setScissors(RenderRect(0, 0, g_width, g_height));
            }
        }
    };

    Backend g_backend;

    // ------------------------------------------------------------------------------------------
    // The hooks
    // ------------------------------------------------------------------------------------------

    void hook_init(RenderInterface* rhi, RenderDevice* device) {
        g_device = device;
        if (g_device == nullptr) {
            say("no device, so the interface will stay dark");
            return;
        }

        {
            const RenderDeviceDescription& description = g_device->getDescription();
            g_device_name = description.name;
            g_video_memory = description.dedicatedVideoMemory;
            std::fprintf(stderr, "[ui] device %s, %llu MB\n", g_device_name.c_str(),
                         static_cast<unsigned long long>(g_video_memory / (1024ull * 1024ull)));
        }

        const RenderShaderFormat format = rhi->getCapabilities().shaderFormat;
        if (format != RenderShaderFormat::DXIL) {
            say("this build only carries DXIL shaders, so the interface will stay dark");
            return;
        }

        auto vs = g_device->createShader(ui_vs_dxil, ui_vs_dxil_size, "VSMain", format);
        auto ps = g_device->createShader(ui_ps_dxil, ui_ps_dxil_size, "PSMain", format);
        if (vs == nullptr || ps == nullptr) {
            say("the interface shaders would not load");
            return;
        }
        // The mark's pixel shader. Its absence costs the mark, not the interface.
        auto mark_ps = g_device->createShader(ui_mark_ps_dxil, ui_mark_ps_dxil_size, "PSMark", format);
        // The gradients' pixel shader. Its absence costs the gradient decorators, not the interface.
        auto gradient_ps = g_device->createShader(ui_gradient_ps_dxil, ui_gradient_ps_dxil_size, "PSGradient", format);
        if (mark_ps == nullptr) {
            say("the mark's shader would not load, so the mark will not be drawn");
        }

        g_sampler = g_device->createSampler(RenderSamplerDesc());

        // Binding IS the shader register here: plume sets BaseShaderRegister from it. Texture
        // and sampler both take 0 because D3D12 gives t and s their own register spaces, and the
        // shader declares t0 and s0.
        //
        // The INDEX passed to setTexture and setSampler is a different number: it is the position
        // in the set treated as one contiguous array, so texture is 0 and sampler is 1 whatever
        // their bindings are. Confusing the two is how a sampler ends up bound to nothing.
        g_set_builder.begin();
        g_set_builder.addTexture(0);
        g_set_builder.addSampler(0);
        g_set_builder.end();

        // The transform for the vertex stage and the mark's time for the pixel stage, one range.
        const RenderPushConstantRange push(0, 0, 0, sizeof(PushConstants),
                                           RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);

        RenderPipelineLayoutDesc layout_desc;
        layout_desc.pushConstantRanges = &push;
        layout_desc.pushConstantRangesCount = 1;
        layout_desc.descriptorSetDescs = &g_set_builder.descriptorSetDesc;
        layout_desc.descriptorSetDescsCount = 1;
        layout_desc.allowInputLayout = true;
        g_layout = g_device->createPipelineLayout(layout_desc);
        if (g_layout == nullptr) {
            say("the interface pipeline layout was refused");
            return;
        }

        const RenderInputSlot slot(0, sizeof(Rml::Vertex));
        const RenderInputElement elements[] = {
            RenderInputElement("POSITION", 0, 0, RenderFormat::R32G32_FLOAT, 0, 0),
            RenderInputElement("COLOR", 0, 1, RenderFormat::R8G8B8A8_UNORM, 0, 8),
            RenderInputElement("TEXCOORD", 0, 2, RenderFormat::R32G32_FLOAT, 0, 12),
        };

        RenderGraphicsPipelineDesc desc;
        desc.pipelineLayout = g_layout.get();
        desc.vertexShader = vs.get();
        desc.pixelShader = ps.get();
        desc.inputSlots = &slot;
        desc.inputSlotsCount = 1;
        desc.inputElements = elements;
        desc.inputElementsCount = 3;
        desc.renderTargetFormat[0] = RenderFormat::B8G8R8A8_UNORM;
        desc.renderTargetCount = 1;
        desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        desc.depthEnabled = false;
        desc.depthWriteEnabled = false;
        desc.cullMode = RenderCullMode::NONE;

        // RmlUi premultiplies alpha, so source is ONE rather than SRC_ALPHA. Getting this wrong
        // shows up as everything translucent looking washed out rather than as an error.
        RenderBlendDesc blend;
        blend.blendEnabled = true;
        blend.srcBlend = RenderBlend::ONE;
        blend.dstBlend = RenderBlend::INV_SRC_ALPHA;
        blend.blendOp = RenderBlendOperation::ADD;
        blend.srcBlendAlpha = RenderBlend::ONE;
        blend.dstBlendAlpha = RenderBlend::INV_SRC_ALPHA;
        blend.blendOpAlpha = RenderBlendOperation::ADD;
        desc.renderTargetBlend[0] = blend;

        g_pipeline = g_device->createGraphicsPipeline(desc);
        if (g_pipeline == nullptr) {
            say("the interface pipeline was refused");
            return;
        }

        // The mark's pipeline differs in its pixel shader only.
        if (mark_ps != nullptr) {
            desc.pixelShader = mark_ps.get();
            g_mark_pipeline = g_device->createGraphicsPipeline(desc);
            if (g_mark_pipeline == nullptr) {
                say("the mark's pipeline was refused, so the mark will not be drawn");
            }
        }
        if (gradient_ps != nullptr) {
            desc.pixelShader = gradient_ps.get();
            g_gradient_pipeline = g_device->createGraphicsPipeline(desc);
            if (g_gradient_pipeline == nullptr) {
                say("the gradient pipeline was refused, so gradient decorators will not be drawn");
            }
        }

        g_white = g_device->createTexture(
            RenderTextureDesc::Texture2D(1, 1, 1, RenderFormat::R8G8B8A8_UNORM));
        if (g_white != nullptr) {
            g_white_set = make_set(g_white.get());
        }

        g_ready = (g_white_set != nullptr);
        if (g_ready) {
            oot::ui::shell::on_renderer_ready(&g_backend);
        }
    }

    void hook_draw(RenderCommandList* list, RenderFramebuffer* framebuffer) {
        // THE PICTURE'S OWN CADENCE, measured here because this is the one place in the program
        // that is called once for every frame that reaches the display, interpolated frames
        // included (which is also why the frame rate counter counts here). Taken before the
        // early return, so a frame the interface skips still counts as a presented frame.
        // See probe.h, Span::Present.
        {
            static std::chrono::steady_clock::time_point last{};
            static bool have_last = false;
            const auto now = std::chrono::steady_clock::now();
            if (have_last) {
                oot::probe::note_span(oot::probe::Span::Present,
                                      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                                now - last).count()));
            }
            last = now;
            have_last = true;
        }

        // How long everything below takes, reported as Span::Interface. This runs on the present
        // thread, so whatever it costs is added to the frame the player is waiting for.
        const auto ui_start = std::chrono::steady_clock::now();
        struct TimeToProbe {
            std::chrono::steady_clock::time_point start;
            ~TimeToProbe() {
                oot::probe::note_span(oot::probe::Span::Interface,
                                      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                                std::chrono::steady_clock::now() - start).count()));
            }
        } ui_timer{ ui_start };

        if (!g_ready || list == nullptr || framebuffer == nullptr) {
            return;
        }

        g_width = static_cast<int>(framebuffer->getWidth());
        g_height = static_cast<int>(framebuffer->getHeight());

        // The interface's clock, for the mark's gleam: seconds since the first draw.
        static const auto started = std::chrono::steady_clock::now();
        g_time_seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();

        std::lock_guard<std::recursive_mutex> lock(g_mutex);

        // THE FRAME CAPTURED LAST TIME ROUND GOES TO THE RECORDER FIRST, before anything below can
        // change what the recorder is doing. The pump a few lines down is what delivers a click on
        // the recorder's plate, so draining after it would throw away the copy already taken and
        // make a recording one frame shorter than it claims. This ordering is load bearing; see
        // recorder.h, time_is_up.
        drain_capture(g_capture[g_capture_slot ^ 1]);

        // Uploads first, while the framebuffer is not bound: a copy in the middle of a render pass
        // is not legal, and the symptom is a device removal rather than a message.
        if (!g_white_uploaded && g_white != nullptr) {
            const std::vector<uint8_t> white(4, 0xFF);
            upload(list, g_white.get(), white, 1, 1);
            g_white_uploaded = true;
        }
        age_retired();
        const auto upload_pending = [&]() {
            for (auto& entry : g_textures) {
                if (!entry.second.uploaded && !entry.second.pending.empty()) {
                    upload(list, entry.second.texture.get(), entry.second.pending,
                           entry.second.width, entry.second.height);
                    entry.second.pending.clear();
                    entry.second.pending.shrink_to_fit();
                    entry.second.uploaded = true;
                }
            }
        };
        upload_pending();

        // The pump runs whether or not anything is on screen, because it is what reads the
        // command that puts something on screen. Gating it behind wants_draw was a deadlock that
        // presented as an interface which reported itself ready and then never appeared.
        oot::ui::shell::pump(g_width, g_height);
        // AND AGAIN AFTER IT (phase 114): what the pump's update made this frame is drawn this
        // frame. A gradient's ramp is a texture made when its decorator compiles, and a fade
        // recompiles it every frame (its colors carry the opacity), so with one upload before the
        // pump each new ramp was drawn as plain white: the cut's strip showed as a pale band for
        // the length of every fade. Still outside the render pass, which is what makes it legal.
        upload_pending();

        if (oot::ui::shell::wants_draw()) {
            g_list = list;
            g_framebuffer = framebuffer;

            list->setFramebuffer(framebuffer);
            list->setGraphicsPipelineLayout(g_layout.get());
            list->setPipeline(g_pipeline.get());
            list->setViewports(RenderViewport(0.0f, 0.0f, static_cast<float>(g_width),
                                              static_cast<float>(g_height)));
            list->setScissors(RenderRect(0, 0, g_width, g_height));

            oot::ui::shell::draw(g_width, g_height);

            g_list = nullptr;
            g_framebuffer = nullptr;
        }

        // THE WINDOW AT ITS OWN SIZE, a screenshot's or a video's, now that the screen has its
        // interface. Decided here when the renderer's picture hook did not run this frame (the
        // launcher), which also leaves last frame's shot and video copies for this hook to hand on.
        if (!g_picture_hook_ran) {
            drain_video_slots();
            finish_shot();
            decide_window(static_cast<uint32_t>(g_width), static_cast<uint32_t>(g_height));
        }
        g_picture_hook_ran = false;
        if (g_window.target != WindowFor::None) {
            draw_window(list, framebuffer);
        }
        else if (g_window.texture != nullptr && ++g_window.idle_frames > WINDOW_IDLE_FRAMES) {
            release_window();
        }

        // THE CAPTURE GOES LAST, so what is recorded is what is presented: the game, and our own
        // interface over it. This used to sit behind the early return that wants_draw once was,
        // which would have recorded nothing at all whenever no menu was open, which is every frame
        // anybody actually wants to record.
        if (oot::recorder::wants_frame()) {
            if (oot::recorder::time_is_up()) {
                oot::recorder::stop();
            }
            else {
                take_capture(g_capture[g_capture_slot], list, framebuffer,
                             static_cast<uint32_t>(g_width), static_cast<uint32_t>(g_height),
                             static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch()).count()));
                g_capture_slot ^= 1;
            }
        }
        else if (g_capture[0].buffer != nullptr || g_capture[1].buffer != nullptr) {
            // Nothing is recording, so the two readback buffers (which are the size of the window,
            // twice) are given back rather than held for a recording that may never come.
            release_captures();
        }
    }

    void hook_deinit() {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_ready = false;
        // The readback buffers belong to the device that is going away, so they go first. A
        // recording still running is ended by main's shutdown, which drains the encoders.
        release_captures();
        // A video still recording is stopped and saved, and its buffers wait for the encoder to
        // let go of them before they go with the device.
        oot::video::renderer_gone();
        release_video_slots(true);
        release_shot();
        release_window();
        oot::ui::shell::on_renderer_gone();
        g_geometry.clear();
        g_textures.clear();
        g_retired_geometry.clear();
        g_retired_textures.clear();
        g_staging.clear();
        g_white_set.reset();
        g_white.reset();
        g_gradients.clear();
        g_ramps.clear();
        g_gradient_pipeline.reset();
        g_mark_pipeline.reset();
        g_pipeline.reset();
        g_layout.reset();
        g_sampler.reset();
        g_device = nullptr;
    }

} // namespace

namespace oot::ui::render {

    void install_hooks() {
        RT64::SetRenderHooks(hook_init, hook_draw, hook_deinit);
        RT64::SetRenderHookPicture(video_picture_size, video_picture);
    }

    bool ready() {
        return g_ready;
    }

    void last_size(int& width, int& height) {
        width = g_width;
        height = g_height;
    }

    const std::string& device_name() {
        return g_device_name;
    }

    uint64_t video_memory_bytes() {
        return g_video_memory;
    }

} // namespace oot::ui::render
