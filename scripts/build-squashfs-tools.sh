#!/bin/sh
# Build unsquashfs from plougher/squashfs-tools
# Usage: build-squashfs-tools.sh <version> <output_dir>

set -e

VERSION="${1:-4.6.1}"
OUTPUT_DIR="${2:-.}"
REPO_URL="https://github.com/plougher/squashfs-tools"
TARBALL="squashfs-tools-${VERSION}.tar.gz"
SRC_DIR="squashfs-tools-${VERSION}"

mkdir -p "$OUTPUT_DIR"
cd "$OUTPUT_DIR"

if [ ! -d "$SRC_DIR" ]; then
    if [ ! -f "$TARBALL" ]; then
        echo "Downloading squashfs-tools ${VERSION}..."
        curl -L -o "$TARBALL" "${REPO_URL}/archive/refs/tags/${VERSION}.tar.gz"
    fi
    echo "Extracting squashfs-tools ${VERSION}..."
    tar -xzf "$TARBALL"
fi

# Build unsquashfs
BUILD_SUBDIR="$SRC_DIR/squashfs-tools"
if [ ! -d "$BUILD_SUBDIR" ]; then
    echo "Expected source directory $BUILD_SUBDIR not found"
    exit 1
fi

echo "Building unsquashfs..."
make -C "$BUILD_SUBDIR" unsquashfs -j"$(getconf _NPROCESSORS_ONLN || echo 1)"

# Copy resulting binary to output directory root for Meson to pick up
cp "$BUILD_SUBDIR/unsquashfs" "$OUTPUT_DIR/unsquashfs"

# Print a quick summary
ls -l "$OUTPUT_DIR/unsquashfs"
