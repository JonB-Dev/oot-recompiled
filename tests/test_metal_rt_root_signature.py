"""Catch shader register changes before Apple Metal Shader Converter runs."""

import importlib.util
import json
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "metal_rt_root_signature", ROOT / "tools/metal_rt_root_signature.py"
)
SIGNATURE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SIGNATURE)


class MetalRTRootSignatureTest(unittest.TestCase):
    def test_covers_current_oot_shader_registers(self):
        SIGNATURE.validate_sources(ROOT / "build-cmake/rt64-src/src/shaders")

    def test_rejects_new_unmapped_register(self):
        with tempfile.TemporaryDirectory() as directory:
            shaders = Path(directory)
            (shaders / "FbRendererCommon.hlsli").write_text(
                "Texture2D unexpected : register(t74, space0);\n"
            )
            (shaders / "FbRendererRT.hlsli").write_text("")
            with self.assertRaisesRegex(RuntimeError, "Uncovered shader register"):
                SIGNATURE.validate_sources(shaders)

    def test_rejects_array_past_table_end(self):
        with tempfile.TemporaryDirectory() as directory:
            shaders = Path(directory)
            (shaders / "FbRendererCommon.hlsli").write_text(
                "Texture2D oversized[8193] : register(t0, space1);\n"
            )
            (shaders / "FbRendererRT.hlsli").write_text("")
            with self.assertRaisesRegex(RuntimeError, "Uncovered shader register"):
                SIGNATURE.validate_sources(shaders)

    def test_table_offsets_preserve_plume_binding_numbers_for_adapter(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "root.json"
            subprocess.run(
                ["python3", str(ROOT / "tools/metal_rt_root_signature.py"), str(output)],
                check=True, capture_output=True, text=True,
            )
            parameters = json.loads(output.read_text())["RootSignature"]["Parameters"]
            for parameter in parameters:
                for descriptor in parameter["DescriptorTable"]["DescriptorRanges"]:
                    self.assertEqual(
                        descriptor["OffsetInDescriptorsFromTableStart"],
                        descriptor["BaseShaderRegister"],
                    )


if __name__ == "__main__":
    unittest.main()
