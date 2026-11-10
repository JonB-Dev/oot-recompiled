#!/usr/bin/env bash
# Compile and execute a small HLSL shader through Apple's converter binding model.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
rt64="$root/build-cmake/rt64-src"
dxc="$rt64/src/contrib/dxc/bin/arm64/dxc-macos"
converter="${METAL_SHADERCONVERTER:-$root/build/metal-converter-local/usr/local/bin/metal-shaderconverter}"
output="$root/build/metal-binding-probe"
[[ -x "$dxc" && -x "$converter" ]] || {
    echo "Prepare RT64 and the local Apple Metal Shader Converter first." >&2
    exit 1
}
mkdir -p "$output"
DYLD_LIBRARY_PATH="$rt64/src/contrib/dxc/lib/arm64${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
    "$dxc" -T cs_6_6 -E CSMain -Fo "$output/probe.dxil" \
    "$root/tests/fixtures/metal_binding_probe.hlsl"
"$converter" "$output/probe.dxil" --entry-point=CSMain \
    --root-signature="$root/tests/fixtures/metal_binding_root_signature.json" \
    -o "$output/probe.metallib" \
    --output-reflection-file "$output/probe.json" \
    --minimum-gpu-family=Apple9 --minimum-os-build-version=16.0.0
DYLD_LIBRARY_PATH="$rt64/src/contrib/dxc/lib/arm64${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
    "$dxc" -T cs_6_6 -E CSMain -Fo "$output/rayquery.dxil" \
    "$root/tests/fixtures/metal_rayquery_probe.hlsl"
"$converter" "$output/rayquery.dxil" --entry-point=CSMain \
    --root-signature="$root/tests/fixtures/metal_rayquery_root_signature.json" \
    -o "$output/rayquery.metallib" \
    --output-reflection-file "$output/rayquery.json" \
    --minimum-gpu-family=Apple9 --minimum-os-build-version=16.0.0
DYLD_LIBRARY_PATH="$rt64/src/contrib/dxc/lib/arm64${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
    "$dxc" -T lib_6_6 -Fo "$output/raygen.dxil" \
    "$root/tests/fixtures/metal_raygen_probe.hlsl"
mkdir -p "$root/build/metal-rt"
"$converter" "$output/raygen.dxil" --entry-point=TestRayGen \
    --root-signature="$root/tests/fixtures/metal_rayquery_root_signature.json" \
    -o "$root/build/metal-rt/TestRayGen.metallib" \
    --output-reflection-file "$output/raygen.json" \
    --minimum-gpu-family=Apple9 --minimum-os-build-version=16.0.0 \
    --rt-ray-generation-compilation=kernel --rt-intersection-compilation=ifVisibleFunction
swift "$root/tools/probe_metal_converter_bindings.swift" "$output/probe.metallib"
"$root/tools/compile_metal_rt_shaders.sh"
cmake --build "$root/build-cmake" --target metal_as_macos_test --parallel 4
OOT_METAL_RAYTRACING=1 "$root/build-cmake/metal_as_macos_test" "$output/probe.metallib" "$output/rayquery.metallib" \
    "$root/build/metal-rt"
