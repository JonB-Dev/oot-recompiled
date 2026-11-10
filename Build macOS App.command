#!/bin/bash
# Double-click in Finder. The native builder is compiled locally, never downloaded.
set -euo pipefail
root="$(cd "$(dirname "$0")" && pwd)"
export PATH="/opt/homebrew/bin:/opt/homebrew/opt/make/libexec/gnubin:$PATH"
app="$root/build/OoT Mac Builder.app"
mkdir -p "$app/Contents/MacOS"
if ! xcrun --find swiftc >/dev/null 2>&1; then
    echo 'Install and open Xcode first, then double-click this file again.'
    open https://developer.apple.com/xcode/
    exit 1
fi
python_bin="${PYTHON_BIN:-}"
if [[ -z "$python_bin" ]] && command -v conda >/dev/null; then
    python_bin="$(conda info --base)/bin/python"
fi
if [[ -z "$python_bin" ]]; then
    for candidate in /opt/homebrew/Caskroom/miniforge/base/bin/python "$HOME/miniforge3/bin/python" "$HOME/miniconda3/bin/python"; do
        if [[ -x "$candidate" ]]; then python_bin="$candidate"; break; fi
    done
fi
python_bin="${python_bin:-$(command -v python3)}"
xcrun swiftc "$root/tools/macos_builder.swift" -o "$app/Contents/MacOS/OoTMacBuilder"
cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>OoTMacBuilder</string>
<key>CFBundleIdentifier</key><string>org.ootrecompiled.localbuilder</string>
<key>CFBundleName</key><string>OoT Mac Builder</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
open "$app" --args "$root" "$python_bin"
