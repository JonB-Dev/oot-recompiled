#!/usr/bin/env bash
# Extract the verified Apple installer into the ignored build directory.
# The caller must explicitly accept the installer license before using it.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
expected_sha256="449833211e4ef0d46bfaa94848e801acff49e226d3c348bba0673f13716be622"
package="${2:-}"
destination="$root/build/metal-converter-local"

if [[ $# -ne 2 || "${1:-}" != "--accept-license" || ! -f "$package" ]]; then
    echo "Usage: tools/setup_metal_shader_converter.sh --accept-license /path/to/Metal_Shader_Converter_4.0_beta_3.pkg" >&2
    exit 2
fi

signature="$(pkgutil --check-signature "$package")"
[[ "$signature" == *"Status: signed Apple Software"* ]] || {
    echo "The converter package is not signed as Apple Software." >&2
    exit 1
}
actual_sha256="$(shasum -a 256 "$package")"
[[ "${actual_sha256%% *}" == "$expected_sha256" ]] || {
    echo "The converter package does not match the pinned 4.0 beta 3 checksum." >&2
    exit 1
}

if [[ -e "$destination" ]]; then
    [[ -x "$destination/usr/local/bin/metal-shaderconverter" ]] || {
        echo "Existing local converter directory needs inspection: $destination" >&2
        exit 1
    }
    version="$("$destination/usr/local/bin/metal-shaderconverter" --version)"
    [[ "$version" == *"metal-irconverter version: 4.0.3"* ]] || {
        echo "Existing local converter has an unexpected version: $destination" >&2
        exit 1
    }
    echo "METAL_CONVERTER_READY $destination/usr/local/bin/metal-shaderconverter"
    exit 0
fi

mkdir -p "$root/build"
temporary="$(mktemp -d "$root/build/metal-converter-setup.XXXXXX")"
trap 'rm -rf "$temporary"' EXIT
pkgutil --expand "$package" "$temporary/expanded"
payload="$temporary/expanded/MetalShaderConverter.pkg/Payload"
[[ -f "$payload" ]] || {
    echo "The Apple package does not contain the expected converter payload." >&2
    exit 1
}
mkdir "$temporary/install"
(cd "$temporary/install" && gzip -dc "$payload" | cpio -idm 2>/dev/null)
converter="$temporary/install/usr/local/bin/metal-shaderconverter"
[[ -x "$converter" && "$("$converter" --version)" == *"metal-irconverter version: 4.0.3"* ]] || {
    echo "The extracted converter is missing or has an unexpected version." >&2
    exit 1
}
mv "$temporary/install" "$destination"
echo "METAL_CONVERTER_READY $destination/usr/local/bin/metal-shaderconverter"
