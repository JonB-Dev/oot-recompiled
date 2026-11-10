#!/usr/bin/env bash
# Compile OoT's existing ray tracing shader library for Apple's Metal converter.
# All generated shader bytes stay under the ignored build directory.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
rt64="$root/build-cmake/rt64-src"
dxc="$rt64/src/contrib/dxc/bin/arm64/dxc-macos"
dxc_libraries="$rt64/src/contrib/dxc/lib/arm64"
source="$rt64/src/shaders/Raytracing.hlsl"
output="$root/build/metal-rt"
python_bin="${PYTHON_BIN:-}"
if [[ -z "$python_bin" ]] && command -v conda >/dev/null; then
    python_bin="$(conda info --base)/bin/python"
fi
python_bin="${python_bin:-python3}"

if [[ $# -gt 1 || ( $# -eq 1 && "$1" != "--dxil-only" ) ]]; then
    echo "Usage: tools/compile_metal_rt_shaders.sh [--dxil-only]" >&2
    exit 2
fi

[[ -f "$source" && -x "$dxc" ]] || {
    echo "Prepare the patched RT64 build copy with tools/prepare_rt64_macos.py first." >&2
    exit 1
}
mkdir -p "$output"

DYLD_LIBRARY_PATH="$dxc_libraries${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
    "$dxc" -I"$rt64/src" -D LIBRARY -T lib_6_3 -Wno-ignored-attributes \
    -Fo "$output/Raytracing.dxil" "$source"
[[ -s "$output/Raytracing.dxil" ]] || {
    echo "DXIL compilation produced no shader library." >&2
    exit 1
}
echo "DXIL_RAYTRACING_OK $output/Raytracing.dxil"

if [[ "${1:-}" == "--dxil-only" ]]; then
    exit 0
fi
local_converter="$root/build/metal-converter-local/usr/local/bin/metal-shaderconverter"
converter="${METAL_SHADERCONVERTER:-}"
if [[ -z "$converter" && -x "$local_converter" ]]; then
    converter="$local_converter"
fi
converter="${converter:-metal-shaderconverter}"
command -v "$converter" >/dev/null || {
    echo "Install Apple's Metal Shader Converter, or set METAL_SHADERCONVERTER to its executable." >&2
    exit 1
}
"$python_bin" "$root/tools/metal_rt_root_signature.py" "$output/root-signature.json"

# Metal Shader Converter processes one DXR entry point per invocation. Its visible-function
# intersection mode works on this M5; the 4.0 beta 3 intersection-function-buffer mode crashes
# on this library, so keep the explicit compatibility mode until that compiler is fixed.
entries=(
    PrimaryRayGen DirectRayGen IndirectRayGen ReflectionRayGen RefractionRayGen
    OcclusionRayGen LightsRayGen ScatterRayGen
    SurfaceMiss ShadowMiss SurfaceClosestHit SurfaceAnyHit ShadowAnyHit
)
for entry in "${entries[@]}"; do
    "$converter" "$output/Raytracing.dxil" --entry-point="$entry" \
        --root-signature="$output/root-signature.json" \
        -o "$output/$entry.metallib" \
        --output-reflection-file "$output/$entry.json" \
        --minimum-gpu-family=Apple9 --minimum-os-build-version=16.0.0 \
        --rt-ray-generation-compilation=kernel \
        --rt-intersection-compilation=ifVisibleFunction
    [[ -s "$output/$entry.metallib" && -s "$output/$entry.json" ]] || {
        echo "Metal Shader Converter produced no output for $entry." >&2
        exit 1
    }
done
"$converter" --synthesize-indirect-intersection-function \
    --rt-hit-group-type=triangles \
    --minimum-gpu-family=Apple9 --minimum-os-build-version=16.0.0 \
    -o "$output/IndirectTriangleIntersection.metallib"
[[ -s "$output/IndirectTriangleIntersection.metallib" ]] || {
    echo "Metal Shader Converter produced no triangle intersection wrapper." >&2
    exit 1
}
swift "$root/tools/verify_metal_rt_shaders.swift" "$output"
echo "METAL_RAYTRACING_SHADERS_OK entries=${#entries[@]} dir=$output"
