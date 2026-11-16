#!/usr/bin/env python3
import os
import sys

dist_dir = sys.argv[1] if len(sys.argv) > 1 else "app/dist"
out_header = sys.argv[2] if len(sys.argv) > 2 else "/tmp/win-build/embedded_assets.h"

os.makedirs(os.path.dirname(out_header), exist_ok=True)

MIME_MAP = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".svg": "image/svg+xml",
    ".json": "application/json",
    ".png": "image/png",
    ".ico": "image/x-icon",
    ".webmanifest": "application/manifest+json",
    ".woff2": "font/woff2",
}

assets = []
for root, _, files in os.walk(dist_dir):
    for fname in sorted(files):
        full = os.path.join(root, fname)
        rel = "/" + os.path.relpath(full, dist_dir).replace("\\", "/")
        ext = os.path.splitext(fname)[1].lower()
        mime = MIME_MAP.get(ext, "application/octet-stream")
        with open(full, "rb") as f:
            data = f.read()
        assets.append((rel, mime, data))

with open(out_header, "w", encoding="utf-8") as out:
    out.write("#pragma once\n#include <cstddef>\n#include <cstdint>\n\n")
    out.write("struct EmbeddedAsset {\n  const char* url_path;\n  const char* mime_type;\n  const uint8_t* data;\n  size_t size;\n};\n\n")
    for idx, (rel, mime, data) in enumerate(assets):
        out.write(f"static const uint8_t kAssetData_{idx}[] = {{\n  ")
        for b_i, b in enumerate(data):
            out.write(f"0x{b:02x},")
            if (b_i + 1) % 24 == 0:
                out.write("\n  ")
        out.write("\n};\n\n")

    out.write("static const EmbeddedAsset kEmbeddedAssets[] = {\n")
    for idx, (rel, mime, data) in enumerate(assets):
        out.write(f'  {{"{rel}", "{mime}", kAssetData_{idx}, {len(data)}}},\n')
    out.write("};\n")
    out.write(f"static const size_t kEmbeddedAssetCount = {len(assets)};\n")

print(f"Generated {out_header} with {len(assets)} embedded UI assets.")
