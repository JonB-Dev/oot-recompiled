"""Guard the resize path when SDL has already resized the CAMetalLayer."""

import unittest
from pathlib import Path


class SwapchainResizeTest(unittest.TestCase):
    def test_layer_already_resized_still_refreshes_drawable_descriptors(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "build-cmake/rt64-src/src/contrib/plume/plume_metal.cpp").read_text()
        resize = source.split("bool MetalSwapChain::resize() {", 1)[1].split(
            "bool MetalSwapChain::needsResize()", 1)[0]
        # The layer can already have the requested size after SDL's fullscreen
        # transition. Descriptor updates must be outside this conditional, or
        # framebuffer creation and UI/capture retain the old window dimensions.
        start = resize.index("if (const CGSize current")
        opening = resize.index("{", start)
        depth = 1
        closing = opening
        while depth:
            closing += 1
            depth += (resize[closing] == "{") - (resize[closing] == "}")
        for assignment in ("drawable.desc.width = width;", "drawable.desc.height = height;"):
            self.assertGreater(resize.index(assignment), closing)
