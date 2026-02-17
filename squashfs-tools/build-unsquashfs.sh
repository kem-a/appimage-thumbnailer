#!/bin/sh
# Download and build unsquashfs from squashfs-tools source.
# Produces a statically-usable unsquashfs binary with zstd support.
#
# Usage: build-unsquashfs.sh <version> <output_dir> [CC]
#
# Requires: zlib-devel, libzstd-devel, xz-devel / liblzma-devel,
#           liblz4-devel (build dependencies).

set -e

VERSION="${1:-4.7.4}"
OUTPUT_DIR="${2:-.}"
CC="${3:-cc}"

TARBALL="squashfs-tools-${VERSION}.tar.gz"
URL="https://github.com/plougher/squashfs-tools/archive/refs/tags/${VERSION}.tar.gz"
SRC_DIR="squashfs-tools-${VERSION}/squashfs-tools"

mkdir -p "$OUTPUT_DIR"
cd "$OUTPUT_DIR"

# Download if not already present
if [ ! -f "$TARBALL" ]; then
    echo "Downloading squashfs-tools ${VERSION}..."
    curl -L -o "$TARBALL" "$URL"
fi

# Extract source
echo "Extracting squashfs-tools source..."
rm -rf "squashfs-tools-${VERSION}"
tar -xzf "$TARBALL"

# Build only unsquashfs with common compressors (zstd, gzip, xz, lz4).
# LZO is skipped to avoid the liblzo2 dependency (rarely used in AppImages).
echo "Building unsquashfs (CC=${CC})..."
cd "$SRC_DIR"

make CC="$CC" unsquashfs \
    CONFIG=1 \
    GZIP_SUPPORT=1 \
    ZSTD_SUPPORT=1 \
    XZ_SUPPORT=1 \
    LZ4_SUPPORT=0 \
    LZO_SUPPORT=0 \
    XATTR_SUPPORT=1 \
    XATTR_OS_SUPPORT=1 \
    XATTR_DEFAULT=0

# Copy binary to output directory
cp unsquashfs "../../unsquashfs"
cd ../..

# Clean up source tree (keep the tarball for reproducibility)
rm -rf "squashfs-tools-${VERSION}"

echo "unsquashfs ${VERSION} built successfully in ${OUTPUT_DIR}"
ls -la unsquashfs
