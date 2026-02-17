/*
 * appimage-type.c - AppImage format detection for appimage-thumbnailer
 *
 * Native ELF parsing to detect AppImage type and payload format.
 * This replaces the need for libappimage (C++ / Boost dependency)
 * by implementing the subset of functions we actually need:
 *   - Type detection (AI magic at ELF e_ident[8..10])
 *   - Payload offset (ELF section header end)
 *   - Format detection (SquashFS vs DwarFS magic at payload offset)
 *
 * SPDX-License-Identifier: MIT
 */

#define _XOPEN_SOURCE 700

#include "appimage-type.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>

/* ELF magic: "\x7fELF" */
static const unsigned char ELF_MAGIC[4] = {0x7f, 'E', 'L', 'F'};

/* AppImage magic: "AI" at ELF e_ident[8..9] */
static const unsigned char AI_MAGIC[2] = {0x41, 0x49};

/* SquashFS little-endian magic: "hsqs" */
static const unsigned char SQFS_MAGIC[4] = {'h', 's', 'q', 's'};

/* DwarFS magic: "DWARFS" */
static const unsigned char DWARFS_MAGIC[6] = {'D', 'W', 'A', 'R', 'F', 'S'};

const char *
appimage_format_name(AppImageFormat format)
{
    switch (format) {
    case APPIMAGE_FORMAT_SQUASHFS:
        return "SquashFS";
    case APPIMAGE_FORMAT_DWARFS:
        return "DwarFS";
    default:
        return "Unknown";
    }
}

int
appimage_get_type(const char *path)
{
    if (!path)
        return -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        g_debug("appimage_get_type: failed to open '%s': %s", path, g_strerror(errno));
        return -1;
    }

    unsigned char header[16];
    ssize_t n = read(fd, header, sizeof(header));
    close(fd);

    if (n < 16)
        return -1;

    /* Check ELF magic */
    if (memcmp(header, ELF_MAGIC, 4) != 0) {
        g_debug("appimage_get_type: '%s' is not an ELF file", path);
        return -1;
    }

    /* Check AppImage magic ("AI") at offset 8 */
    if (memcmp(header + 8, AI_MAGIC, 2) != 0) {
        g_debug("appimage_get_type: '%s' lacks AppImage 'AI' magic at e_ident[8..9]", path);
        return -1;
    }

    int type = (int)header[10];
    g_debug("appimage_get_type: '%s' is AppImage type %d", path, type);
    return type;
}

off_t
appimage_payload_offset(const char *path)
{
    if (!path)
        return (off_t)-1;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        g_debug("appimage_payload_offset: failed to open '%s': %s", path, g_strerror(errno));
        return (off_t)-1;
    }

    unsigned char ident[16];
    ssize_t n = read(fd, ident, sizeof(ident));
    if (n < 16) {
        g_debug("appimage_payload_offset: short read on '%s'", path);
        close(fd);
        return (off_t)-1;
    }

    /* Verify ELF magic */
    if (memcmp(ident, ELF_MAGIC, 4) != 0) {
        g_debug("appimage_payload_offset: '%s' is not an ELF file", path);
        close(fd);
        return (off_t)-1;
    }

    int elf_class = ident[4]; /* 1 = 32-bit, 2 = 64-bit */
    int elf_data  = ident[5]; /* 1 = little-endian, 2 = big-endian */

    off_t shoff;
    uint16_t shentsize;
    uint16_t shnum;

    if (elf_class == 2) {
        /* ELF64: e_shoff at offset 40 (8 bytes) */
        uint64_t shoff64;
        if (lseek(fd, 40, SEEK_SET) < 0 || read(fd, &shoff64, 8) != 8) {
            close(fd);
            return (off_t)-1;
        }

        /* e_shentsize at offset 58, e_shnum at offset 60 (2 bytes each) */
        if (lseek(fd, 58, SEEK_SET) < 0 || read(fd, &shentsize, 2) != 2
            || read(fd, &shnum, 2) != 2) {
            close(fd);
            return (off_t)-1;
        }

        if (elf_data == 2) { /* big-endian */
            shoff64   = GUINT64_FROM_BE(shoff64);
            shentsize = GUINT16_FROM_BE(shentsize);
            shnum     = GUINT16_FROM_BE(shnum);
        } else {
            shoff64   = GUINT64_FROM_LE(shoff64);
            shentsize = GUINT16_FROM_LE(shentsize);
            shnum     = GUINT16_FROM_LE(shnum);
        }
        shoff = (off_t)shoff64;

    } else if (elf_class == 1) {
        /* ELF32: e_shoff at offset 32 (4 bytes) */
        uint32_t shoff32;
        if (lseek(fd, 32, SEEK_SET) < 0 || read(fd, &shoff32, 4) != 4) {
            close(fd);
            return (off_t)-1;
        }

        /* e_shentsize at offset 46, e_shnum at offset 48 */
        if (lseek(fd, 46, SEEK_SET) < 0 || read(fd, &shentsize, 2) != 2
            || read(fd, &shnum, 2) != 2) {
            close(fd);
            return (off_t)-1;
        }

        if (elf_data == 2) {
            shoff32   = GUINT32_FROM_BE(shoff32);
            shentsize = GUINT16_FROM_BE(shentsize);
            shnum     = GUINT16_FROM_BE(shnum);
        } else {
            shoff32   = GUINT32_FROM_LE(shoff32);
            shentsize = GUINT16_FROM_LE(shentsize);
            shnum     = GUINT16_FROM_LE(shnum);
        }
        shoff = (off_t)shoff32;

    } else {
        g_debug("appimage_payload_offset: unknown ELF class %d in '%s'", elf_class, path);
        close(fd);
        return (off_t)-1;
    }

    close(fd);

    off_t payload = shoff + (off_t)((uint32_t)shnum * (uint32_t)shentsize);
    g_debug("appimage_payload_offset: ELF payload at offset %" G_GINT64_FORMAT
            " (shoff=%" G_GINT64_FORMAT ", shnum=%u, shentsize=%u) for '%s'",
            (gint64)payload, (gint64)shoff, (unsigned)shnum, (unsigned)shentsize, path);
    return payload;
}

AppImageFormat
appimage_detect_format(const char *path)
{
    off_t offset = appimage_payload_offset(path);
    if (offset <= 0) {
        g_debug("appimage_detect_format: could not determine payload offset for '%s'", path);
        return APPIMAGE_FORMAT_UNKNOWN;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        g_debug("appimage_detect_format: failed to open '%s': %s", path, g_strerror(errno));
        return APPIMAGE_FORMAT_UNKNOWN;
    }

    if (lseek(fd, offset, SEEK_SET) < 0) {
        g_debug("appimage_detect_format: failed to seek to offset %" G_GINT64_FORMAT, (gint64)offset);
        close(fd);
        return APPIMAGE_FORMAT_UNKNOWN;
    }

    unsigned char magic[8];
    ssize_t n = read(fd, magic, sizeof(magic));
    close(fd);

    if (n < 4) {
        g_debug("appimage_detect_format: could not read magic bytes at offset %" G_GINT64_FORMAT,
                (gint64)offset);
        return APPIMAGE_FORMAT_UNKNOWN;
    }

    if (memcmp(magic, SQFS_MAGIC, 4) == 0) {
        g_debug("appimage_detect_format: SquashFS magic found at offset %" G_GINT64_FORMAT
                " in '%s'", (gint64)offset, path);
        return APPIMAGE_FORMAT_SQUASHFS;
    }

    if (n >= 6 && memcmp(magic, DWARFS_MAGIC, 6) == 0) {
        g_debug("appimage_detect_format: DwarFS magic found at offset %" G_GINT64_FORMAT
                " in '%s'", (gint64)offset, path);
        return APPIMAGE_FORMAT_DWARFS;
    }

    g_debug("appimage_detect_format: unknown format at offset %" G_GINT64_FORMAT
            " in '%s' (magic: %02x %02x %02x %02x)",
            (gint64)offset, path, magic[0], magic[1], magic[2], magic[3]);
    return APPIMAGE_FORMAT_UNKNOWN;
}
