#!/usr/bin/env python3
"""Regenerate releases/version.txt from the binaries currently in releases/.

ChainIt updates are *in-place*: ChainIt-Windows.exe reads this manifest, compares the
published SHA-256 with the hash of the binary that is running, and only then downloads
and overwrites itself. Android's UpdateManager does the same thing with versionCode.
So the hashes here MUST match the committed binaries — run this script after every
release build (android/build-apk.sh and the Windows zig c++ command) and commit the
result together with the binaries.

It also refuses to publish a manifest for a binary whose compiled VERSIONINFO disagrees with
--version. windows/res/*.rc carry the version twice - numeric FILEVERSION/PRODUCTVERSION fields and
the FileVersion/ProductVersion strings - and a hand bump can easily update one and not the other.
Explorer's Details tab shows the string, so the mistake is invisible in the UI while
GetFileVersionInfo (and anything comparing versions numerically) still reports the old release.
v1.7.0 shipped that way once; this check makes it a build failure instead.

The version and the Android versionCode are *derived*, not remembered: `--version` defaults to
`kChainItAppVersion` in windows/src/chainit_win_main.cpp and `--code` to `android:versionCode` in
the Android manifest, so this script cannot publish a release that disagrees with the source it was
built from. Passing either flag is an override, not a requirement. It also refuses to publish when
`android:versionName` disagrees with the version being published, because Windows compares `version`
and Android compares the versionCode inside the downloaded APK - a disagreement means the two
platforms disagree about what "latest" is.

Usage:  python3 scripts/publish_release_manifest.py [--version 1.8.0] [--code 9]
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import pathlib
import re
import struct
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
RELEASES = REPO / "releases"
EXE = RELEASES / "ChainIt-Windows.exe"
APK = RELEASES / "ChainIt-Android.apk"
MANIFEST = RELEASES / "version.txt"


def setup_exe(version: str) -> pathlib.Path:
    """The packed Windows installer for this version, if it has been built."""
    return RELEASES / f"ChainIt-Setup-{version}.exe"


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


# VS_FIXEDFILEINFO starts with this signature; the next four dwords are the numeric
# file and product versions. The packed installer contains two of them (its own stub
# resource plus the appended payload's), so every occurrence has to agree.
_VS_FIXEDFILEINFO_SIG = b"\xbd\x04\xef\xfe"


def _dword_version(ms: int, ls: int) -> str:
    return f"{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}.{ls & 0xFFFF}"


def pe_versions(path: pathlib.Path) -> list[str]:
    """Every numeric file:product version compiled into a PE's VERSIONINFO resource."""
    data = path.read_bytes()
    found: list[str] = []
    idx = data.find(_VS_FIXEDFILEINFO_SIG)
    while idx != -1 and idx + 24 <= len(data):
        file_ms, file_ls, prod_ms, prod_ls = struct.unpack_from("<IIII", data, idx + 8)
        found.append(f"{_dword_version(file_ms, file_ls)}:{_dword_version(prod_ms, prod_ls)}")
        idx = data.find(_VS_FIXEDFILEINFO_SIG, idx + 1)
    return found


def check_pe_version(path: pathlib.Path, version: str) -> list[str]:
    """Fail the release if a binary's compiled version is not `version`.

    Returns the human-readable lines to print. Raises SystemExit on any disagreement,
    naming both the field and the value found, because "the manifest is wrong" is a
    much worse thing to discover on a user's machine than here.
    """
    label = path.name
    if not path.exists():
        return []
    want = f"{version}.0"
    found = pe_versions(path)
    if not found:
        print(f"ERROR: {label} has no VERSIONINFO resource at all", file=sys.stderr)
        raise SystemExit(1)
    bad = [f for f in found if f != f"{want}:{want}"]
    if bad:
        print(
            f"ERROR: {label} was compiled with the wrong version resource:\n"
            f"         expected FILEVERSION/PRODUCTVERSION {want} ({version}), found {', '.join(sorted(set(bad)))}\n"
            f"         bump the numeric FILEVERSION and PRODUCTVERSION fields in windows/res/*.rc -\n"
            f"         updating only the FileVersion/ProductVersion strings is not enough.",
            file=sys.stderr,
        )
        raise SystemExit(1)
    # The string half of the resource lives in StringFileInfo as UTF-16; a numeric-only
    # bump would leave Explorer's Details tab showing the previous release.
    data = path.read_bytes()
    if version.encode("utf-16-le") not in data:
        print(
            f"ERROR: {label} carries numeric version {want} but no \"{version}\" version string -\n"
            f"         the FileVersion/ProductVersion strings in windows/res/*.rc are stale.",
            file=sys.stderr,
        )
        raise SystemExit(1)
    return [f"  {label:<28} VERSIONINFO {want} ({len(found)} resource{'s' if len(found) > 1 else ''})"]


CPP_MAIN = REPO / "windows" / "src" / "chainit_win_main.cpp"
ANDROID_MANIFEST = REPO / "android" / "app" / "src" / "main" / "AndroidManifest.xml"


def source_version() -> str:
    """The version the Windows engine actually reports (`kChainItAppVersion`)."""
    text = CPP_MAIN.read_text(encoding="utf-8")
    m = re.search(r'kChainItAppVersion\s*=\s*"([^"]+)"', text)
    if not m:
        raise SystemExit(f"ERROR: cannot find kChainItAppVersion in {CPP_MAIN}")
    return m.group(1)


def source_version_code() -> int:
    """The versionCode the APK is built with, straight from the Android manifest."""
    text = ANDROID_MANIFEST.read_text(encoding="utf-8")
    m = re.search(r'android:versionCode="(\d+)"', text)
    if not m:
        raise SystemExit(f"ERROR: cannot find android:versionCode in {ANDROID_MANIFEST}")
    return int(m.group(1))


def source_version_name() -> str:
    """The versionName the APK is built with, straight from the Android manifest."""
    text = ANDROID_MANIFEST.read_text(encoding="utf-8")
    m = re.search(r'android:versionName="([^"]+)"', text)
    return m.group(1) if m else ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--version",
        default=None,
        help="human version label (default: kChainItAppVersion in the Windows source)",
    )
    parser.add_argument(
        "--code",
        type=int,
        default=None,
        help="Android versionCode (default: android:versionCode in the Android manifest)",
    )
    parser.add_argument("--channel", default="stable")
    args = parser.parse_args()

    # The defaults used to be frozen literals (`--version 1.3.0 --code 4`), so every release had
    # to remember to pass both flags. `windows/build.sh` calls this script with no arguments, which
    # meant the documented reproducible build ended in "compiled with the wrong version resource"
    # and advice to bump the .rc files - true-sounding, and exactly the wrong fix. Deriving both
    # from the source of truth means the script cannot disagree with the code it is publishing, and
    # an explicit flag is now an override rather than a requirement.
    if args.version is None:
        args.version = source_version()
    if args.code is None:
        args.code = source_version_code()

    missing = [p for p in (EXE, APK) if not p.exists()]
    if missing:
        print("ERROR: missing release binaries:", ", ".join(str(p) for p in missing), file=sys.stderr)
        return 1

    # Both updaters read this one manifest: Windows compares `version`, Android compares the
    # versionCode inside the APK it downloads. If the APK's own versionName disagrees with the
    # version being published, the two platforms disagree about what "latest" means - a phone could
    # be told it is up to date while the PC downloads a different release.
    manifest_name = source_version_name()
    if manifest_name and manifest_name != args.version:
        print(
            f"ERROR: android:versionName is {manifest_name} but this manifest publishes "
            f"{args.version}.\n"
            f"       Bump android/app/src/main/AndroidManifest.xml and rebuild the APK "
            f"(./android/build-apk.sh), or pass --version {manifest_name}.",
            file=sys.stderr,
        )
        return 1

    exe_hash, apk_hash = sha256(EXE), sha256(APK)
    today = _dt.date.today().isoformat()

    # Refuse to publish a manifest whose binaries disagree with the version being published.
    pe_lines = check_pe_version(EXE, args.version)
    pe_lines += check_pe_version(setup_exe(args.version), args.version)

    # The setup package is optional but published when present: it carries the very
    # same ChainIt-Windows.exe payload, so its hash is recorded for completeness.
    setup = setup_exe(args.version)
    setup_lines: list[str] = []
    if setup.exists():
        setup_lines = [
            f"setup={setup.name}",
            f"setup_sha256={sha256(setup)}",
            f"setup_bytes={setup.stat().st_size}",
            "setup_silent_flag=/S",
            "setup_uninstall_flag=/uninstall",
        ]
    else:
        print(
            f"note: {setup.name} not found - run ./installer/build-installer.sh to publish the installer",
            file=sys.stderr,
        )

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
            + setup_lines
            + [""]
        ),
        encoding="utf-8",
    )

    print(f"Wrote {MANIFEST.relative_to(REPO)}")
    print(f"  version   {args.version} (code {args.code})")
    print(f"  exe       {EXE.stat().st_size} bytes  {exe_hash}")
    print(f"  apk       {APK.stat().st_size} bytes  {apk_hash}")
    for line in pe_lines:
        print(line)
    if setup_lines:
        print(f"  setup     {setup.stat().st_size} bytes  {setup_lines[1].split('=', 1)[1]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
