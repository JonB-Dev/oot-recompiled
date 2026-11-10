import unittest

from tools.check_metal_ui_bindings import validate


class MetalUiBindingsTest(unittest.TestCase):
    def test_rejects_texture_sampler_alias(self):
        with self.assertRaisesRegex(ValueError, "overlap"):
            validate("texture2d<float> image [[id(0)]]; // Overlapping binding: sampler [[id(0)]];")

    def test_rejects_reinterpreting_texture_as_sampler(self):
        with self.assertRaises(ValueError):
            validate("reinterpret_cast<constant sampler &>(args.image)")

    def test_accepts_separate_texture_and_sampler_slots(self):
        validate("texture2d<float> image [[id(0)]]; sampler filtering [[id(1)]];")
