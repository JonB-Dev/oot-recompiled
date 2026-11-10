#!/usr/bin/env python3
"""Emit the global DXR root signature for OoT's Metal shader conversion.

The register ranges mirror FbRendererCommon.hlsli and FbRendererRT.hlsli. Check
their declarations so a later shader edit cannot silently leave a resource out.
"""

import json
import re
import sys
from pathlib import Path


# (register space, resource class, first register, count). Samplers need their
# own descriptor table; all other space-0 resources share one table. Preserve
# each register number as the table offset so the Metal descriptor adapter can
# translate Plume's binding numbers without renumbering. Metal Shader Converter
# tables use 24-byte entries; Plume's raster argument buffers use 8-byte entries.
TABLES = [
    [(0, "CBV", 1, 1), (0, "CBV", 25, 1),
     (0, "SRV", 2, 5), (0, "SRV", 26, 15), (0, "SRV", 67, 1),
     (0, "UAV", 41, 26), (0, "UAV", 68, 6)],
    [(0, "Sampler", 7, 18)],
    [(1, "SRV", 0, 8192)],
    [(2, "SRV", 0, 8192)],
    [(3, "CBV", 0, 1), (3, "SRV", 1, 1)],
]
REGISTER_CLASS = {"b": "CBV", "s": "Sampler", "t": "SRV", "u": "UAV"}
DECLARATION = re.compile(r"register\(([bstu])(\d+),\s*space(\d+)\)")
ARRAY = re.compile(r"\[(\d+)\]\s*:\s*register")


def covered(space: int, kind: str, first: int, count: int) -> bool:
    return any(s == space and k == kind and start <= first and first + count <= start + size
               for table in TABLES for s, k, start, size in table)


def validate_sources(shaders: Path) -> None:
    for name in ("FbRendererCommon.hlsli", "FbRendererRT.hlsli"):
        for line in (shaders / name).read_text().splitlines():
            match = DECLARATION.search(line)
            if match is None:
                continue
            kind = REGISTER_CLASS[match.group(1)]
            first = int(match.group(2))
            space = int(match.group(3))
            array = ARRAY.search(line)
            count = int(array.group(1)) if array else 1
            if not covered(space, kind, first, count):
                raise RuntimeError(f"Uncovered shader register: {name}: space{space} {kind}{first} count={count}")


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    shaders = root / "build-cmake/rt64-src/src/shaders"
    output = Path(sys.argv[1]) if len(sys.argv) == 2 else None
    if output is None:
        print("Usage: metal_rt_root_signature.py OUTPUT.json", file=sys.stderr)
        return 2

    validate_sources(shaders)

    parameters = []
    for table in TABLES:
        ranges = []
        for space, kind, first, count in table:
            ranges.append({
                "BaseShaderRegister": first,
                "Flags": "IRDescriptorRangeFlagDescriptorsVolatile",
                "NumDescriptors": count,
                "OffsetInDescriptorsFromTableStart": first,
                "RangeType": "IRDescriptorRangeType" + kind,
                "RegisterSpace": space,
            })
        parameters.append({
            "DescriptorTable": {"DescriptorRanges": ranges, "NumDescriptorRanges": len(ranges)},
            "ParameterType": "IRRootParameterTypeDescriptorTable",
            "ShaderVisibility": "IRShaderVisibilityAll",
        })

    signature = {
        "RootSignature": {
            "Flags": "IRRootSignatureFlagNone",
            "NumParameters": len(parameters),
            "NumStaticSamplers": 0,
            "Parameters": parameters,
            "StaticSamplers": [],
        },
        "version": "IRRootSignatureVersion_1_1",
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(signature, indent=2) + "\n")
    print(f"METAL_RT_ROOT_SIGNATURE_OK {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
