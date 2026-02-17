/*
 * appimage-type.h - AppImage format detection for appimage-thumbnailer
 *
 * Replaces libappimage dependency with minimal native ELF parsing.
 * Detects AppImage type (1/2), payload offset, and payload format
 * (SquashFS vs DwarFS) by reading ELF headers and magic bytes.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef APPIMAGE_TYPE_H
#define APPIMAGE_TYPE_H

#include <glib.h>
#include <sys/types.h>

typedef enum {
    APPIMAGE_FORMAT_UNKNOWN  = 0,
    APPIMAGE_FORMAT_SQUASHFS = 1,
    APPIMAGE_FORMAT_DWARFS   = 2,
} AppImageFormat;

/**
 * Detect the payload format of an AppImage.
 * Parses the ELF header to determine payload offset, then checks magic bytes.
 *
 * @param path Path to the AppImage file
 * @return The detected payload format
 */
AppImageFormat appimage_detect_format(const char *path);

/**
 * Get the payload offset within an AppImage.
 * Computes: e_shoff + (e_shnum * e_shentsize) from the ELF header.
 *
 * @param path Path to the AppImage file
 * @return The byte offset where the payload begins, or -1 on failure
 */
off_t appimage_payload_offset(const char *path);

/**
 * Get the AppImage type (1 or 2).
 * Reads the ELF e_ident bytes 8-10 which encode "AI" + type for AppImages.
 *   Type 1: ISO 9660 based (legacy)
 *   Type 2: SquashFS or DwarFS embedded after ELF runtime
 *
 * @param path Path to the AppImage file
 * @return 1 or 2 for valid AppImages, -1 if not an AppImage
 */
int appimage_get_type(const char *path);

/**
 * Return a human-readable name for an AppImageFormat value.
 */
const char *appimage_format_name(AppImageFormat format);

#endif /* APPIMAGE_TYPE_H */
