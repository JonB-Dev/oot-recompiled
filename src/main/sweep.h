#pragma once

#include <string>

// THE SWEEP: an in game harness the person triggers (the user, 2026-09-24: "it runs through
// each calculation, each setting that could cause it, and after each it screen captures a snap
// of to see what changes and logs the screenshot and the setting each time in sequence").
//
// Started from the Lighting menu's Sweep row. It walks every option of every lighting row, the
// Inspect views and the Experiment variants included, one step per option: the option is put
// to the renderer alone (the file is not touched), the picture is given time for the history
// to settle, one full frame is captured, and the next step follows. At the end the person's
// own values are put back and a log names, in order, each step's row, option and file. The
// frames and the log go where recordings go, under one stem, so the folder reads as one run.
//
// Ticked once per presented frame from the renderer's update, like the recorder; nothing here
// blocks, and a step whose frame cannot be taken (a snapshot already waiting) simply waits.
namespace oot::sweep {

    // Plan the steps, make the folder, start. False when a sweep is already running or the
    // folder could not be made; the reason is in the log line either way.
    bool start();

    // One presented frame has gone by. Cheap when nothing is running.
    void tick();

    bool running();

    // "Sweep 12 of 90: Light strength = Double", or empty when nothing is running; for a plate.
    std::string status();

} // namespace oot::sweep
