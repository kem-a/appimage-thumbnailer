/*
 * dwarfs-extract.h - DwarFS extraction support for appimage-thumbnailer
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DWARFS_EXTRACT_H
#define DWARFS_EXTRACT_H

#include <glib.h>

/**
 * Check if DwarFS tools are available.
 * Looks for bundled tools first, then system PATH.
 *
 * @return TRUE if dwarfsextract is available
 */
gboolean dwarfs_tools_available(void);

/**
 * Extract a single entry from a DwarFS archive.
 *
 * @param archive Path to the DwarFS archive (AppImage)
 * @param entry   Path of the entry to extract (without leading slash)
 * @param output  Output byte array (allocated on success)
 * @return TRUE on success, FALSE on failure
 */
gboolean dwarfs_extract_entry(const char *archive, const char *entry, GByteArray **output);

#endif /* DWARFS_EXTRACT_H */
