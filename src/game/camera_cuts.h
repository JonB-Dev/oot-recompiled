#pragma once

#include <cstdint>

// Decide, once per game frame, whether the camera moved smoothly or cut, for the camera tag in
// patches/camera_tagging.c.
//
// WHY NATIVE. The rule needs last frame's eye and focus, and a patch keeping them in its own
// static storage gave a verdict nobody could see: the first two rules written there called every
// frame of a first person pan a cut, and finding that out took a temporary fork of the renderer.
// Here the state is native, the arithmetic is native, and when the probe is on the inputs and the
// verdict are one line in the trace. The patch hands over two addresses and an entrance index and
// gets back a yes or a no.
//
// THE RULE judges what a person sees. A camera moved smoothly when its EYE is near where its own
// velocity predicted it (a follow camera drifts a few units a frame, a lock-on swing perhaps a
// hundred, a cut thousands) and its VIEW DIRECTION turned by less than a threshold (a pan is a
// few degrees a frame, a lock-on snap perhaps fifteen, a cutscene angle change tens). The focus
// point's distance from the eye does not enter into it, which is what went wrong before: a far
// focus point moves a long way for a small turn.
namespace oot::camera_cuts {

    // eye_address and at_address are emulated addresses of two Vec3f (three floats each), read
    // through the runtime's memory and never trusted to be anywhere but the cached view of the
    // console's memory. entrance is the game's current entrance index; a change is a cut and
    // resets the tracking. Returns true when the renderer should draw the frames in between.
    // patch_calls is a counter the patch keeps in its own static storage, printed beside the
    // native call count: equal means a patch's statics persist, which later phases rely on.
    bool smooth(const uint8_t* rdram, uint32_t eye_address, uint32_t at_address, int32_t entrance,
                uint32_t patch_calls);

    // Print the inputs and the verdict, rate limited, when on.
    void enable(bool on);

    // How many times the question has been asked and how it was answered. Read by the recorder so
    // every captured frame carries the running cut count: a frame the count steps on is a frame
    // the renderer did NOT interpolate, which is a frame the world was allowed to jump on. That
    // turns "it skips sometimes" into a list of frame numbers to go and look at.
    struct Counts {
        uint32_t calls = 0;
        uint32_t smooth = 0;
        uint32_t cuts = 0;
    };
    Counts counts();

} // namespace oot::camera_cuts
