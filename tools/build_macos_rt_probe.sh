#!/usr/bin/env bash
# Compile the complete macOS app with RT_ENABLED in an isolated build tree.
# The Metal device advertises ray tracing only when OOT_METAL_RAYTRACING=1.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python_bin="${PYTHON_BIN:-}"
if [[ -z "$python_bin" ]] && command -v conda >/dev/null; then
    python_bin="$(conda info --base)/bin/python"
fi
python_bin="${python_bin:-python3}"
export PYTHON_BIN="$python_bin"
source_tree="$root/build-cmake/rt64-src"
probe="$root/build/rt-compile-probe"
[[ -f "$source_tree/CMakeLists.txt" ]] || {
    echo "Run tools/prepare_rt64_macos.py and tools/build_macos.sh first." >&2
    exit 1
}
mkdir -p "$probe"
if [[ -e "$probe/rt64-src" || -L "$probe/rt64-src" ]]; then
    [[ "$(realpath "$probe/rt64-src")" == "$(realpath "$source_tree")" ]] || {
        echo "The existing probe source link points somewhere else: $probe/rt64-src" >&2
        exit 1
    }
else
    ln -s "$source_tree" "$probe/rt64-src"
fi

cmake -S "$root" -B "$probe" -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE="$python_bin" -DOOT_MACOS_BUILD_RENDERER=ON \
    -DOOT_MACOS_BUILD_RUNTIME=ON -DOOT_MACOS_BUILD_APP=ON -DRT64_RAYTRACING=ON
cmake --build "$probe" --parallel 4
ctest --test-dir "$probe" --output-on-failure
"$root/tools/probe_metal_converter_bindings.sh"
shader_resources="$probe/OoTRecompiled.app/Contents/Resources/rt64-raytracing"
mkdir -p "$shader_resources"
for shader in "$root/build/metal-rt/"*.metallib; do
    [[ "$(basename "$shader")" == TestRayGen.metallib ]] && continue
    cp "$shader" "$shader_resources/"
done
printf 'MACOS_RT_COMPILE_OK %s\n' "$probe/OoTRecompiled.app"
