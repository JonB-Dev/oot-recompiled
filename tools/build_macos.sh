#!/usr/bin/env bash
# Build the local Apple Silicon app from the user's own verified NTSC-U 1.0 ROM.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
rom="${1:-}"
python_bin="${PYTHON_BIN:-}"
if [[ -z "$python_bin" ]] && command -v conda >/dev/null; then
    python_bin="$(conda info --base)/bin/python"
fi
python_bin="${python_bin:-python3}"

fail() { printf 'MACOS_BUILD_BLOCKED: %s\n' "$*" >&2; exit 1; }
[[ "$(uname -s)" == Darwin && "$(uname -m)" == arm64 ]] || fail "Requires Apple Silicon macOS."
[[ -n "$rom" ]] || fail "Usage: tools/build_macos.sh /path/to/your/USA-1.0.z64"
[[ -f "$rom" ]] || fail "ROM file does not exist: $rom"
command -v "$python_bin" >/dev/null || fail "Python 3 is required."
"$python_bin" "$root/tools/verify_rom.py" --file "$rom" --expect-compressed || fail "ROM checksum mismatch."

developer_dir="$(xcode-select -p 2>/dev/null || true)"
[[ "$developer_dir" == *.app/Contents/Developer ]] || fail "Full Xcode is required; select it with xcode-select after installing."
for tool in cmake ninja git make brew; do
    command -v "$tool" >/dev/null || fail "$tool is required."
done
xcrun --sdk macosx metal --version >/dev/null 2>&1 || fail "Xcode's Metal Toolchain is unavailable; run xcodebuild -downloadComponent MetalToolchain."

mkdir -p "$root/build"
if [[ ! -d "$root/upstream/oot/.git" ]]; then
    git clone --filter=blob:none https://github.com/zeldaret/oot.git "$root/upstream/oot"
    git -C "$root/upstream/oot" checkout --detach 14759abf9abb4295de142cdc1452998630ea70fb
fi
decomp_pin="14759abf9abb4295de142cdc1452998630ea70fb"
[[ "$(git -C "$root/upstream/oot" rev-parse HEAD)" == "$decomp_pin" ]] || \
    fail "The local zeldaret/oot checkout is not at the pinned commit $decomp_pin."
if [[ ! -d "$root/build/pydeps/crunch64" ]]; then
    "$python_bin" -m pip install --disable-pip-version-check --target "$root/build/pydeps" \
        -r "$root/tools/requirements-macos.txt"
fi

cmake -S "$root/lib/N64ModernRuntime/N64Recomp" -B "$root/build/n64recomp-macos" \
    -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build "$root/build/n64recomp-macos" --target N64RecompCLI RSPRecomp --parallel 8
"$python_bin" "$root/tools/decompress_rom_macos.py" "$rom"
(
    cd "$root/config"
    "$root/build/n64recomp-macos/N64Recomp" ntsc-1.0.toml
    "$root/build/n64recomp-macos/RSPRecomp" aspMain.ntsc-1.0.toml
    "$root/build/n64recomp-macos/RSPRecomp" njpgdspMain.ntsc-1.0.toml
)

llvm_clang="$(brew --prefix llvm)/bin/clang"
lld_linker="$(brew --prefix lld)/bin/ld.lld"
[[ -x "$llvm_clang" && -x "$lld_linker" ]] || fail "Homebrew llvm and lld are required for MIPS patches."
if ! make -C "$root/patches" --jobs 8 CC="$llvm_clang" LD="$lld_linker" > "$root/build/patches-macos-build.log" 2>&1; then
    tail -40 "$root/build/patches-macos-build.log" >&2
    fail "MIPS patch compilation failed."
fi
(
    cd "$root/config"
    "$root/build/n64recomp-macos/N64Recomp" patches.toml
)

"$python_bin" "$root/tools/prepare_rt64_macos.py"
cmake -S "$root" -B "$root/build-cmake" -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE="$python_bin" -DOOT_MACOS_BUILD_RENDERER=ON \
    -DOOT_MACOS_BUILD_RUNTIME=ON -DOOT_MACOS_BUILD_APP=ON -DRT64_RAYTRACING=OFF
cmake --build "$root/build-cmake" --parallel 8
ctest --test-dir "$root/build-cmake" --output-on-failure
[[ -f "$root/build-cmake/OoTRecompiled.app/Contents/MacOS/OoTRecompiled" ]] || fail "The native app bundle was not produced."
printf 'MACOS_BUILD_OK %s\n' "$root/build-cmake/OoTRecompiled.app"
