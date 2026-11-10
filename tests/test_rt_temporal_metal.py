"""Regression: OoT's temporal ray shader must compile to Metal with atomic stats."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class TemporalMetalTests(unittest.TestCase):
    def test_temporal_stats_atomic_compiles(self):
        if len(sys.argv) != 3:
            self.fail("expected DXC and spirv_cross_msl paths")

        root = Path(__file__).resolve().parent.parent
        source = root / "build-cmake/rt64-src/src/shaders/RtTemporalCS.hlsl"
        dxc = Path(sys.argv[1])
        cross = Path(sys.argv[2])
        self.assertTrue(source.is_file() and dxc.is_file() and cross.is_file())

        with tempfile.TemporaryDirectory(prefix="oot-temporal-metal-") as temporary:
            output = Path(temporary)
            spirv = output / "temporal.spv"
            metal = output / "temporal.metal"
            air = output / "temporal.air"
            environment = os.environ.copy()
            dxc_lib = dxc.parent.parent.parent / "lib/arm64"
            environment["DYLD_LIBRARY_PATH"] = str(dxc_lib)

            for command in (
                [str(dxc), "-spirv", "-fspv-target-env=vulkan1.0",
                 "-fvk-use-dx-layout", f"-I{source.parents[1]}",
                 "-E", "CSMain", "-T", "cs_6_3", str(source), "-Fo", str(spirv)],
                [str(cross), str(spirv), str(metal)],
                ["xcrun", "-sdk", "macosx", "metal", "-c", str(metal), "-o", str(air)],
            ):
                completed = subprocess.run(command, env=environment, capture_output=True,
                                           text=True, timeout=60)
                self.assertEqual(completed.returncode, 0,
                                 f"{command[0]} failed: {completed.stderr[-4000:]}")
            self.assertTrue(air.is_file())


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
