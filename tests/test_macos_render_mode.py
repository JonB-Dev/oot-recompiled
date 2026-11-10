"""Choose a reproducible lighting comparison in the same macOS app."""

import unittest

from tools.smoke_macos_gameplay import renderer_launch_mode, tutorial_input_events


class RendererLaunchModeTest(unittest.TestCase):
    def test_traced_and_untraced_use_the_same_rt_app(self):
        self.assertEqual(renderer_launch_mode(True, False), ("build/rt-compile-probe", 1))
        self.assertEqual(renderer_launch_mode(False, True), ("build/rt-compile-probe", 0))

    def test_local_lights_can_be_selected_for_comparison(self):
        self.assertEqual(renderer_launch_mode(True, False, 2), ("build/rt-compile-probe", 2))

    def test_level_requires_tracing(self):
        with self.assertRaises(ValueError):
            renderer_launch_mode(False, True, 2)

    def test_standard_raster_app_remains_available(self):
        self.assertEqual(renderer_launch_mode(False, False), ("build-cmake", None))

    def test_conflicting_modes_are_rejected(self):
        with self.assertRaises(ValueError):
            renderer_launch_mode(True, True)

    def test_comparison_inputs_can_be_shifted_past_the_later_raster_prompt(self):
        traced = tutorial_input_events(0)
        raster = tutorial_input_events(15000)
        self.assertIn("65000 axis 4 30000\n", traced)
        self.assertIn("66000 button 0 1\n", traced)
        self.assertIn("80000 axis 4 30000\n", raster)
        self.assertIn("81000 button 0 1\n", raster)
        self.assertIn("84000 axis 0 18000\n", raster)


if __name__ == "__main__":
    unittest.main()
