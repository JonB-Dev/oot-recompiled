#pragma once

#include <cstdint>
#include <string>

// Our interface draws over the finished game frame through RT64's render hook, which hands us the
// command list and the swap chain framebuffer once per presented frame. That is the only place a
// draw may happen: the hook runs on RT64's own render thread, and touching the device from
// anywhere else is a race we would find as a corrupted frame months later.
//
// Nothing here fails the program. If the device rejects a pipeline, or a shader will not load, the
// interface stays dark and the game carries on. A settings menu that cannot be drawn is an
// annoyance; a game that will not boot because of one is a defect.
namespace oot::ui::render {

    // Installs the RT64 render hooks. Call once, before the runtime starts, because RT64 reads the
    // hooks when it creates its device.
    void install_hooks();

    // True once the device side is up and a document could actually appear on screen.
    bool ready();

    // The last framebuffer size the hook saw, for laying the interface out at the right scale.
    void last_size(int& width, int& height);

    // The card, as the device described itself when the hook came up: its name and its own
    // memory. The launcher's defaults for a fresh install are judged by the memory.
    const std::string& device_name();
    uint64_t video_memory_bytes();

} // namespace oot::ui::render
