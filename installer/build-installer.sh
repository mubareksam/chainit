#!/usr/bin/env bash
# Build ChainIt-Setup-<version>.exe - a real Windows installer with no
# third-party packaging tool. The stub is compiled from
# windows/src/chainit_installer.cpp and then the already-built
# ChainIt-Windows.exe payload is appended together with a 52-byte trailer
# (see installer/pack_installer.py).
#
#   ./installer/build-installer.sh [app-exe] [out-setup-exe]
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

ZIG="${ZIG:-/tmp/toolchain/py/ziglang/zig}"
APP="${1:-$REPO/releases/ChainIt-Windows.exe}"
VERSION="${CHAINIT_VERSION:-$(python3 - "$APP" <<'PY'
import re, sys
src = open('windows/src/chainit_win_main.cpp', encoding='utf-8').read()
m = re.search(r'kChainItAppVersion\s*=\s*"([^"]+)"', src)
print(m.group(1) if m else '0.0.0')
PY
)}"
OUT="${2:-$REPO/releases/ChainIt-Setup-${VERSION}.exe}"
STUB="${STUB:-/tmp/chainit-setup-stub.exe}"

[ -f "$APP" ] || { echo "error: app exe not found: $APP (run ./windows/build.sh first)" >&2; exit 1; }

"$ZIG" c++ \
  -target x86_64-windows-gnu -std=c++17 -O2 -s \
  -Iwindows/res \
  -DCHAINIT_INSTALLER_VERSION="\"$VERSION\"" \
  windows/src/chainit_installer.cpp \
  windows/res/chainit_installer.rc \
  -o "$STUB" \
  -Wl,--subsystem,windows \
  -lcomctl32 -lgdi32 -lshell32 -luser32 -ladvapi32 -lcrypt32 -lole32

python3 installer/pack_installer.py "$STUB" "$APP" "$OUT" --version "$VERSION"
