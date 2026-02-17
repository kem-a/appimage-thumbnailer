/*
 * squashfs-extract.h - SquashFS extraction support for appimage-thumbnailer
 *
 * Uses unsquashfs (from squashfs-tools) to extract files from SquashFS-based
 * AppImages.  Replaces 7z as the SquashFS extractor with full zstd support.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SQUASHFS_EXTRACT_H
#define SQUASHFS_EXTRACT_H

#include <glib.h>
#include <sys/types.h>

/**
 * Check if unsquashfs tool is available.
 * Looks for a bundled copy first, then the system PATH.
 *
 * @return TRUE if unsquashfs is available
 */
gboolean squashfs_tools_available(void);

/**
 * Extract a single entry from a SquashFS-based AppImage.
 *
 * If the entry is a symlink inside the SquashFS, the link target text
 * is returned as the payload (so the caller can follow it).
 *
 * @param archive Path to the AppImage file
 * @param entry   Path of the entry to extract (without leading slash)
 * @param offset  SquashFS payload offset within the AppImage
 * @param output  Output byte array (allocated on success, caller frees)
 * @return TRUE on success, FALSE on failure
 */
gboolean squashfs_extract_entry(const char *archive, const char *entry,
                                off_t offset, GByteArray **output);

#endif /* SQUASHFS_EXTRACT_H */
