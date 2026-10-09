#include "ui/ui_render.h"
#include "ui/ui_shell.h"
#include "game/render.h"
#include "main/probe.h"
#include "main/recorder.h"
#include "main/video.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <cstring>
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

#include <chrono>

using namespace plume;

namespace {

    // The push constants, as the shader declares them: the transform, then the mark's time.
    struct PushConstants {
        float transform[16];
        float mark[4];
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
        // Holding it until the next frame is the cheap correct answer; the alternative is a fence
        // per upload, and these happen a handful of times at document load.
        static std::vector<std::unique_ptr<RenderBuffer>> keep_alive;
        keep_alive.push_back(std::move(staging));
        if (keep_alive.size() > 64) {
            keep_alive.erase(keep_alive.begin(), keep_alive.begin() + 32);
        }
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

    // A free slot for this frame's picture, sized for the copy: the recording's own size for the
    // game's picture, the window's for the whole window (scaled by the encoder). -1 when no slot
    // is free (the encoder is behind, and repeats the last picture for this one) or none is wanted.
    int claim_video_slot(uint32_t window_width, uint32_t window_height, bool whole_window, uint32_t& copy_width,
                         uint32_t& copy_height) {
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
        if (whole_window) {
            w = window_width;
            h = window_height;
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

    // A SCREENSHOT OF THE GAME ALONE (2026-10-09, the Screenshots show row): the game's picture drawn
    // again at the window's size through the same hook as the video's, so none of this program's
    // interface is in it and an open settings panel does not narrow it. One buffer of its own,
    // copied one frame and written the next, the way the frame recorder's copies are. A video
    // recording the game alone gives up that one frame, and repeats its last picture for it.
    struct ShotCapture {
        std::unique_ptr<RenderBuffer> buffer;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t pitch = 0;
        int state = 0;   // 0 nothing, 1 claimed for this frame's picture, 2 copied (the GPU runs it)
    };
    ShotCapture g_shot;

    void release_shot() {
        g_shot.buffer.reset();
        g_shot.state = 0;
    }

    // Last frame's copy is finished: written, and the buffer given back.
    void finish_shot() {
        if (g_shot.state != 2 || g_shot.buffer == nullptr) {
            return;
        }
        const uint8_t* pixels = static_cast<const uint8_t*>(g_shot.buffer->map());
        if (pixels != nullptr) {
            oot::recorder::submit_snapshot(pixels, static_cast<int>(g_shot.width), static_cast<int>(g_shot.height), g_shot.pitch);
            g_shot.buffer->unmap();
        }
        release_shot();
    }

    bool video_picture_size(uint32_t window_width, uint32_t window_height, uint32_t* width, uint32_t* height) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (g_device == nullptr) {
            return false;
        }
        drain_video_slots();
        g_video_claimed = -1;

        finish_shot();
        if (oot::recorder::snapshot_wants_game_alone() && g_shot.state == 0) {
            g_shot.pitch = aligned_row(window_width);
            g_shot.buffer = g_device->createBuffer(RenderBufferDesc::ReadbackBuffer(static_cast<uint64_t>(g_shot.pitch) * window_height));
            if (g_shot.buffer != nullptr) {
                g_shot.width = window_width;
                g_shot.height = window_height;
                g_shot.state = 1;
                *width = window_width;
                *height = window_height;
                return true;
            }
            say("a screenshot readback buffer could not be created");
        }
        // The whole window is taken in the draw hook, after the interface (take_window_video).
        if (!oot::video::wants_pictures() || oot::video::records_window()) {
            return false;
        }
        uint32_t w = 0;
        uint32_t h = 0;
        const int slot = claim_video_slot(window_width, window_height, false, w, h);
        if (slot < 0) {
            return false;
        }
        g_video_claimed = slot;
        *width = w;
        *height = h;
        return true;
    }

    // THE WHOLE WINDOW (Video shows, 2026-10-09): the swap chain image once everything is drawn on
    // it, the game and this program's interface over it, copied the way take_capture copies it
    // for the frame recorder, and scaled to the recording's size by the encoder.
    void take_window_video(RenderCommandList* list, RenderFramebuffer* framebuffer, uint32_t width, uint32_t height) {
        RenderTexture* texture = oot::renderer::swap_chain_texture(framebuffer);
        if (texture == nullptr) {
            return;
        }
        drain_video_slots();
        uint32_t w = 0;
        uint32_t h = 0;
        const int index = claim_video_slot(width, height, true, w, h);
        if (index < 0) {
            return;
        }
        VideoSlot& slot = g_video[index];
        RenderTextureCopyLocation destination = RenderTextureCopyLocation::PlacedFootprint(
            slot.buffer.get(), RenderFormat::B8G8R8A8_UNORM, w, h, 1, slot.pitch / 4);
        // The same stray read in plume's copy as take_capture explains.
        destination.texture = texture;
        list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(texture, RenderTextureLayout::COPY_SOURCE));
        list->copyTextureRegion(destination, RenderTextureCopyLocation::Subresource(texture));
        list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(texture, RenderTextureLayout::COLOR_WRITE));
        slot.state.store(1, std::memory_order_release);
    }

    void video_picture(RenderCommandList* list, RenderTexture* picture, uint32_t width, uint32_t height) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
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
            g_geometry.erase(static_cast<uintptr_t>(handle));
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
            std::fprintf(stderr, "[ui] no shader named '%s' ('%s'), so that decorator was not drawn\n",
                         value.c_str(), name.c_str());
            return 0;
        }

        void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry,
                          Rml::Vector2f translation, Rml::TextureHandle texture) override {
            if (g_list == nullptr || static_cast<uintptr_t>(shader) != MARK_SHADER_HANDLE || g_mark_pipeline == nullptr) {
                return;
            }
            g_list->setPipeline(g_mark_pipeline.get());
            draw_geometry(geometry, translation, texture);
            g_list->setPipeline(g_pipeline.get());
        }

        void ReleaseShader(Rml::CompiledShaderHandle shader) override {
            (void)shader;   // nothing was allocated for it
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

            RenderDescriptorSet* set = g_white_set.get();
            if (texture != 0) {
                auto t = g_textures.find(static_cast<uintptr_t>(texture));
                if (t != g_textures.end() && t->second.set != nullptr && t->second.uploaded) {
                    set = t->second.set.get();
                }
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
            if (source.size() > 5 && source.compare(source.size() - 5, 5, ".rgba") == 0) {
                std::ifstream in(std::filesystem::path(source), std::ios::binary);
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
            g_textures.erase(static_cast<uintptr_t>(handle));
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
            if (g_scissor_on) {
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
        for (auto& entry : g_textures) {
            if (!entry.second.uploaded && !entry.second.pending.empty()) {
                upload(list, entry.second.texture.get(), entry.second.pending,
                       entry.second.width, entry.second.height);
                entry.second.pending.clear();
                entry.second.pending.shrink_to_fit();
                entry.second.uploaded = true;
            }
        }

        // The pump runs whether or not anything is on screen, because it is what reads the
        // command that puts something on screen. Gating it behind wants_draw was a deadlock that
        // presented as an interface which reported itself ready and then never appeared.
        oot::ui::shell::pump(g_width, g_height);

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

        // The video recorder's whole window (Video shows), now that everything is drawn on it.
        if (oot::video::wants_pictures() && oot::video::records_window()) {
            take_window_video(list, framebuffer, static_cast<uint32_t>(g_width), static_cast<uint32_t>(g_height));
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
        oot::ui::shell::on_renderer_gone();
        g_geometry.clear();
        g_textures.clear();
        g_white_set.reset();
        g_white.reset();
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
