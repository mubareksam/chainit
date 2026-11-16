#!/usr/bin/env bash
# Reproducible cross-compile of ChainIt-Windows.exe (no Visual Studio needed).
#
#   ./windows/build.sh [output-path]
#
# Needs the local toolchain laid out by the session (/tmp/toolchain/py/ziglang).
# The .rc file embeds windows/res/chainit.ico (the same chain-link artwork as the
# Android launcher icon) so the exe, its window, the tray icon and any desktop
# shortcut all show the ChainIt mark.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$REPO/releases/ChainIt-Windows.exe}"
ZIG="${ZIG:-/tmp/toolchain/py/ziglang/zig}"

cd "$REPO"
mkdir -p "$(dirname "$OUT")"

# Refresh the embedded web UI from the latest vite build.
npm run build
python3 scripts/gen_embedded_assets.py app/dist /tmp/win-build/embedded_assets.h

"$ZIG" c++ \
  -target x86_64-windows-gnu -std=c++17 -O2 -s \
  -I/tmp/win-build -Iwindows/res \
  windows/src/chainit_win_main.cpp \
  windows/res/chainit.rc \
  -o "$OUT" \
  -Wl,--subsystem,windows \
  -lws2_32 -liphlpapi -lshell32 -luser32 -ladvapi32 -lcrypt32 -lurlmon -lwininet -lole32 -lgdi32

# Keep the in-place update manifest in sync with the binaries we just produced.
python3 scripts/publish_release_manifest.py

echo "OK  $OUT ($(stat -c%s "$OUT") bytes)"
