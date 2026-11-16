#!/usr/bin/env python3
"""Regenerate releases/version.txt from the binaries currently in releases/.

ChainIt updates are *in-place*: ChainIt-Windows.exe reads this manifest, compares the
published SHA-256 with the hash of the binary that is running, and only then downloads
and overwrites itself. Android's UpdateManager does the same thing with versionCode.
So the hashes here MUST match the committed binaries — run this script after every
release build (android/build-apk.sh and the Windows zig c++ command) and commit the
result together with the binaries.

Usage:  python3 scripts/publish_release_manifest.py [--version 1.2.0] [--code 3]
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
RELEASES = REPO / "releases"
EXE = RELEASES / "ChainIt-Windows.exe"
APK = RELEASES / "ChainIt-Android.apk"
MANIFEST = RELEASES / "version.txt"


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", default="1.2.0", help="human version label (versionName)")
    parser.add_argument("--code", type=int, default=3, help="Android versionCode")
    parser.add_argument("--channel", default="stable")
    args = parser.parse_args()

    missing = [p for p in (EXE, APK) if not p.exists()]
    if missing:
        print("ERROR: missing release binaries:", ", ".join(str(p) for p in missing), file=sys.stderr)
        return 1

    exe_hash, apk_hash = sha256(EXE), sha256(APK)
    today = _dt.date.today().isoformat()

    MANIFEST.write_text(
        "\n".join(
            [
                "# ChainIt release manifest - consumed by the in-place updaters:",
                "#   windows/src/chainit_win_main.cpp  (performAppUpdate: version + exe_sha256)",
                "#   android/.../update/UpdateManager.kt (versionCode inside the APK itself)",
                "# Regenerate with: python3 scripts/publish_release_manifest.py",
                f"version={args.version}",
                f"channel={args.channel}",
                f"released={today}",
                "",
                "exe=ChainIt-Windows.exe",
                f"exe_sha256={exe_hash}",
                f"exe_bytes={EXE.stat().st_size}",
                "",
                "apk=ChainIt-Android.apk",
                f"apk_sha256={apk_hash}",
                f"apk_bytes={APK.stat().st_size}",
                f"apk_version_code={args.code}",
                "",
            ]
        ),
        encoding="utf-8",
    )

    print(f"Wrote {MANIFEST.relative_to(REPO)}")
    print(f"  version   {args.version} (code {args.code})")
    print(f"  exe       {EXE.stat().st_size} bytes  {exe_hash}")
    print(f"  apk       {APK.stat().st_size} bytes  {apk_hash}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
