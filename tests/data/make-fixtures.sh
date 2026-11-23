#!/bin/sh
# Regenerates the test images in this directory.
# Needs mksquashfs (squashfs-tools), mkdwarfs (DwarFS release tarball) and python3.
# Usage: tests/data/make-fixtures.sh [path/to/mkdwarfs]

set -eu

MKDWARFS="${1:-mkdwarfs}"
OUT="$(cd "$(dirname "$0")" && pwd)"
TREE="$(mktemp -d)"
trap 'rm -rf "$TREE"' EXIT

python3 - "$TREE" <<'EOF'
import os, struct, sys, zlib

top = sys.argv[1]
root = os.path.join(top, "root")
icons = os.path.join(root, "usr/share/icons/hicolor")
os.makedirs(os.path.join(icons, "32x32/apps"))
os.makedirs(os.path.join(icons, "scalable/apps"))

def png(w, h, pixel):
    raw = b"".join(b"\0" + b"".join(pixel(x, y) for x in range(w)) for y in range(h))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw))
            + chunk(b"IEND", b""))

# Opaque red disc on a transparent background.
def disc(x, y):
    return b"\xff\x00\x00\xff" if (x - 15.5) ** 2 + (y - 15.5) ** 2 < 14 ** 2 else b"\0\0\0\0"

with open(os.path.join(icons, "32x32/apps/app.png"), "wb") as f:
    f.write(png(32, 32, disc))

# Blue square. The comment makes it compressible and larger than one 4 KiB block.
with open(os.path.join(icons, "scalable/apps/app.svg"), "w") as f:
    f.write('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">'
            + "<!--" + "padding " * 2048 + "-->"
            + '<rect x="8" y="8" width="48" height="48" fill="#0000ff"/></svg>\n')

svg = "usr/share/icons/hicolor/scalable/apps/app.svg"
links = {
    ".DirIcon": "app.png",
    "app.png": "usr/share/icons/hicolor/32x32/apps/app.png",
    "usr/share/icons/hicolor/32x32/apps/rel.svg": "../../scalable/apps/app.svg",
    "abs.svg": "/" + svg,
    "dot.svg": "./usr//share/icons/hicolor/scalable/apps/app.svg",
    # d1 needs 4 hops (allowed), d0 needs 5 (one too many).
    "d0": "d1", "d1": "d2", "d2": "d3", "d3": "d4", "d4": svg,
    "loop": "loop",
    "missing": "nothing.png",
}
for name, target in links.items():
    os.symlink(target, os.path.join(root, name))

# uruntime metadata sidecar whose .DirIcon points into the DwarFS payload.
sidecar = os.path.join(top, "sidecar")
os.makedirs(sidecar)
os.symlink("app.png", os.path.join(sidecar, ".DirIcon"))
EOF

for comp in gzip xz zstd lz4; do
    extra=""
    [ "$comp" = xz ] && extra="-Xbcj x86"
    # shellcheck disable=SC2086
    mksquashfs "$TREE/root" "$OUT/$comp.sqfs" -noappend -quiet -no-progress -all-root \
        -all-time 0 -mkfs-time 0 -b 4096 -comp "$comp" $extra
done

for comp in zstd lzma lz4; do
    "$MKDWARFS" -i "$TREE/root" -o "$OUT/$comp.dwarfs" -f -S 12 -C "$comp" \
        --set-owner 0 --set-group 0 --set-time 0 --no-history --log-level=error
done

mksquashfs "$TREE/sidecar" "$OUT/sidecar.sqfs" -noappend -quiet -no-progress -all-root \
    -all-time 0 -mkfs-time 0
