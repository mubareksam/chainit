#!/usr/bin/env python3
"""Render the ChainIt chain-link mark into a Windows icon (.ico) and PNGs.

The artwork is the *same* vector as the Android launcher icon
(android/app/src/main/res/drawable/ic_launcher.xml), so the phone app, the desktop
exe, its window/tray icons and the desktop shortcut all share one identity.

Pure Python + Pillow: the SVG-ish path data is flattened (cubic beziers -> polygons)
and rasterized at 8x, then downscaled with a Lanczos filter for crisp small sizes.

Usage:  python3 windows/res/make_icon.py
Writes: windows/res/chainit.ico
        windows/res/chainit-icon-256.png
        app/public/chainit-icon-512.png
"""

from __future__ import annotations

import math
import pathlib
import re
import struct
from typing import List, Tuple

from PIL import Image, ImageDraw

REPO = pathlib.Path(__file__).resolve().parents[2]
OUT_DIR = REPO / "windows" / "res"
PUBLIC_DIR = REPO / "app" / "public"

VIEWPORT = 108.0
SUPERSAMPLE = 8

# --- artwork, copied verbatim from android/.../drawable/ic_launcher.xml -----------------
TILE_FILL = "#0B1018"          # slightly lifted vs #090D16 so the tile reads on dark taskbars
TILE_STROKE = "#1E293B"
PATH_EMERALD = (
    "M42,36h24c9.9,0 18,8.1 18,18s-8.1,18 -18,18h-10v-8h10c5.5,0 10,-4.5 10,-10"
    "s-4.5,-10 -10,-10h-24c-5.5,0 -10,4.5 -10,10v2h-8v-2c0,-9.9 8.1,-18 18,-18z"
)
PATH_CYAN = (
    "M66,72h-24c-9.9,0 -18,-8.1 -18,-18s8.1,-18 18,-18h10v8h-10c-5.5,0 -10,4.5 -10,10"
    "s4.5,10 10,10h24c5.5,0 10,-4.5 10,-10v-2h8v2c0,9.9 -8.1,18 -18,18z"
)
EMERALD = (16, 185, 129, 255)   # #10B981
CYAN = (6, 182, 212, 255)       # #06B6D4
EMERALD_HI = (52, 211, 153, 255)
CYAN_HI = (34, 211, 238, 255)

TOKEN = re.compile(r"[MmLlHhVvCcSsQqTtZz]|-?\d*\.?\d+(?:e[-+]?\d+)?")


def _numbers(raw: List[str]) -> List[float]:
    return [float(v) for v in raw]


def flatten(path: str, steps: int = 24) -> List[List[Tuple[float, float]]]:
    """Turn SVG-ish path data into one or more closed polygons (viewport units)."""
    tokens = TOKEN.findall(path)
    polys: List[List[Tuple[float, float]]] = []
    current: List[Tuple[float, float]] = []
    x = y = 0.0
    start = (0.0, 0.0)
    last_ctrl: Tuple[float, float] | None = None
    i = 0

    def take(n: int) -> List[float]:
        nonlocal i
        vals = _numbers(tokens[i : i + n])
        if len(vals) != n:
            raise ValueError("path ended early: %r" % path)
        i += n
        return vals

    while i < len(tokens):
        cmd = tokens[i]
        if not re.match(r"[A-Za-z]", cmd):
            raise ValueError("expected a command, got %r" % cmd)
        i += 1
        rel = cmd.islower()
        c = cmd.upper()

        if c == "M":
            nx, ny = take(2)
            if rel:
                nx, ny = x + nx, y + ny
            x, y = nx, ny
            start = (x, y)
            if current:
                polys.append(current)
            current = [(x, y)]
            last_ctrl = None
        elif c == "L":
            nx, ny = take(2)
            if rel:
                nx, ny = x + nx, y + ny
            x, y = nx, ny
            current.append((x, y))
            last_ctrl = None
        elif c == "H":
            (nx,) = take(1)
            if rel:
                nx = x + nx
            x = nx
            current.append((x, y))
            last_ctrl = None
        elif c == "V":
            (ny,) = take(1)
            if rel:
                ny = y + ny
            y = ny
            current.append((x, y))
            last_ctrl = None
        elif c in ("C", "S"):
            if c == "C":
                x1, y1, x2, y2, nx, ny = take(6)
                if rel:
                    x1, y1 = x + x1, y + y1
                    x2, y2 = x + x2, y + y2
                    nx, ny = x + nx, y + ny
            else:  # smooth cubic: mirror the previous control point
                x2, y2, nx, ny = take(4)
                if rel:
                    x2, y2 = x + x2, y + y2
                    nx, ny = x + nx, y + ny
                if last_ctrl is None:
                    x1, y1 = x, y
                else:
                    x1, y1 = 2 * x - last_ctrl[0], 2 * y - last_ctrl[1]
            p0 = (x, y)
            for k in range(1, steps + 1):
                t = k / steps
                mt = 1 - t
                px = mt**3 * p0[0] + 3 * mt**2 * t * x1 + 3 * mt * t**2 * x2 + t**3 * nx
                py = mt**3 * p0[1] + 3 * mt**2 * t * y1 + 3 * mt * t**2 * y2 + t**3 * ny
                current.append((px, py))
            last_ctrl = (x2, y2)
            x, y = nx, ny
        elif c in ("Q", "T"):
            raise ValueError("quadratic segments are not used by the ChainIt mark")
        elif c == "Z":
            if current:
                polys.append(current)
            current = []
            x, y = start
            last_ctrl = None
        else:
            raise ValueError("unsupported path command %r" % cmd)

    if current:
        polys.append(current)
    return polys


def rounded_rect_points(size: float, radius: float, inset: float = 0.0, steps: int = 16):
    """Corner-rounded rectangle as a point list (pixel units)."""
    lo, hi = inset, size - inset
    r = min(radius, (hi - lo) / 2)
    pts: List[Tuple[float, float]] = []
    corners = [(hi - r, lo + r, 270, 360), (hi - r, hi - r, 0, 90), (lo + r, hi - r, 90, 180), (lo + r, lo + r, 180, 270)]
    for cx, cy, a0, a1 in corners:
        for k in range(steps + 1):
            a = math.radians(a0 + (a1 - a0) * k / steps)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def render(size: int) -> Image.Image:
    """Rasterize the mark at `size` x `size` pixels."""
    ss = size * SUPERSAMPLE
    img = Image.new("RGBA", (ss, ss), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    scale = ss / VIEWPORT

    # Rounded dark tile with a hairline border (matches the web header mark).
    tile_radius = 24.0 / VIEWPORT * ss
    tile = rounded_rect_points(float(ss), tile_radius, inset=max(0.0, ss * 0.004))
    draw.polygon(tile, fill=(11, 16, 24, 255))
    draw.line(tile + [tile[0]], fill=(30, 41, 59, 255), width=max(1, int(ss * 0.012)), joint="curve")

    def paint(path: str, fill: Tuple[int, int, int, int], highlight: Tuple[int, int, int, int]):
        for poly in flatten(path):
            scaled = [(px * scale, py * scale) for px, py in poly]
            draw.polygon(scaled, fill=fill)
        # A short bright edge on the upper-left gives the link a little depth without noise.
        for poly in flatten(path):
            scaled = [(px * scale, py * scale) for px, py in poly]
            draw.line(scaled[: max(2, len(scaled) // 3)], fill=highlight,
                      width=max(1, int(ss * 0.012)), joint="curve")

    paint(PATH_EMERALD, EMERALD, EMERALD_HI)
    paint(PATH_CYAN, CYAN, CYAN_HI)

    return img.resize((size, size), Image.LANCZOS)


def build_ico(frames: List[Tuple[int, Image.Image]]) -> bytes:
    """Assemble a multi-size .ico by hand (32bpp BMP entries = widest Windows support)."""
    count = len(frames)
    out = bytearray()
    out += struct.pack("<HHH", 0, 1, count)  # reserved, type=icon, count
    offset = 6 + 16 * count
    entries = bytearray()
    payloads = bytearray()
    for size, img in frames:
        img = img.convert("RGBA")
        if size >= 128:
            # Large entries are stored PNG-compressed (standard since Vista) to keep the
            # embedded resource small; small sizes stay uncompressed BMP for max compatibility.
            import io

            buf = io.BytesIO()
            img.save(buf, format="PNG", optimize=True)
            payload = buf.getvalue()
            dim = 0 if size >= 256 else size
            entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(payload), offset)
            payloads += payload
            offset += len(payload)
            continue
        px = img.load()
        xor = bytearray()
        for y in range(size - 1, -1, -1):          # bottom-up rows
            for x in range(size):
                r, g, b, a = px[x, y]
                xor += bytes((b, g, r, a))        # BGRA
        and_mask = bytearray()
        row_bytes = ((size + 31) // 32) * 4        # 1bpp, 4-byte aligned rows
        for _ in range(size):
            and_mask += b"\x00" * row_bytes
        header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0,
                             len(xor) + len(and_mask), 0, 0, 0, 0)
        payload = header + bytes(xor) + bytes(and_mask)
        dim = 0 if size >= 256 else size           # 0 means 256 in the directory entry
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(payload), offset)
        payloads += payload
        offset += len(payload)
    out += entries
    out += payloads
    return bytes(out)


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    PUBLIC_DIR.mkdir(parents=True, exist_ok=True)

    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    frames = [(s, render(s)) for s in sizes]

    ico_path = OUT_DIR / "chainit.ico"
    ico_path.write_bytes(build_ico(frames))

    png256 = OUT_DIR / "chainit-icon-256.png"
    frames[-1][1].save(png256, format="PNG")

    png512 = PUBLIC_DIR / "chainit-icon-512.png"
    render(512).save(png512, format="PNG")

    print("Wrote %s (%d bytes, sizes %s)" % (ico_path.relative_to(REPO), ico_path.stat().st_size,
                                             ", ".join(str(s) for s in sizes)))
    print("Wrote %s" % png256.relative_to(REPO))
    print("Wrote %s" % png512.relative_to(REPO))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
