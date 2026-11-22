// THE CONTOUR SURVEY, EDGE TO EDGE (the user, 2026-11-20: "if i maximize it, it stops at its edge
// half way through and right side gets none").
//
// The drawing (assets/ui/textures/contours.svg, made by tools/generate_contours.py) is one picture
// the size of the survey tokens, and it repeats exactly at its edges. RCSS's svg decorator draws a
// picture once, stretched to one box, so a ground wider or taller than the drawing ran out of it.
// Each .survey is now a frame over its whole ground (assets/ui/cut.rcss), and `lay` fills it with
// .survey-tile copies of the drawing, as many as cover it, each the same whole number of pixels so
// their edges meet without a seam. Copies of one size share one rendered texture (the SVG plugin
// caches by file and size).

#pragma once

namespace Rml {
    class Context;
}

namespace oot::ui::survey {

    // Every .survey in the context's documents, covered at its size now. Call it after a layout
    // pass. Returns true when it changed a survey, which then needs another layout pass before it
    // is drawn; a survey already covered at its size is left alone.
    bool lay(Rml::Context* context);

} // namespace oot::ui::survey
