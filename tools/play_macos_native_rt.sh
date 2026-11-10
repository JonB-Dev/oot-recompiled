#!/bin/bash
# Native drawable output and NTSC-rate tracing, with a bounded local watchdog.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if pgrep -x OoTRecompiled >/dev/null; then
    echo "OoTRecompiled is already running; close it before starting another session." >&2
    exit 1
fi
python_bin="${PYTHON_BIN:-}"
if [[ -z "$python_bin" ]] && command -v conda >/dev/null; then
    python_bin="$(conda info --base)/bin/python"
fi
python_bin="${python_bin:-python3}"
profile="$root/data/native-rt-profile"
(cd "$root" && "$python_bin" -m tools.init_macos_play --native-rt)
for setting in 'window = 1' 'resolution = 2' 'framerate = 0' 'texturepack = 0'; do
    if ! grep -Fxq "$setting" "$profile/settings.txt"; then
        echo "The native play profile differs from the tested preset: $setting" >&2
        exit 1
    fi
done
if ! grep -Fxq 'occlusionquality = 3' "$profile/lighting.txt"; then
    echo "Select two occlusion rays in the native profile before using this preset." >&2
    exit 1
fi
echo "Native output, 20 fps target, traced shadows/local lights/AO. Texture packs off."
echo "WASD moves; Space = A; Q = Z-target; Return = Start; F1 opens the menu."
echo "The watchdog stops the game on a Metal fault, excess RSS, or after 30 minutes."
exec "$python_bin" "$root/tools/smoke_macos_gameplay.py" \
    "$root/data/roms/Legend of Zelda, The - Ocarina of Time (USA).z64" \
    --data-root "$profile" --entrance 0x266 --raytracing --rt-level 2 \
    --interactive --seconds 1800 --max-rss-mib 1536
