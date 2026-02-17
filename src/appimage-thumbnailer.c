#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cairo.h>
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <librsvg/rsvg.h>

#include "appimage-type.h"
#include "dwarfs-extract.h"
#include "squashfs-extract.h"

#define DEFAULT_THUMBNAIL_SIZE 256
#define MAX_SYMLINK_DEPTH 5
#define POINTER_TEXT_LIMIT 1024

#ifndef APPIMAGE_THUMBNAILER_VERSION
#define APPIMAGE_THUMBNAILER_VERSION "unknown"
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/* ------------------------------------------------------------------ */
/*  Entry extraction dispatch (SquashFS via unsquashfs / DwarFS)       */
/* ------------------------------------------------------------------ */

static gboolean
extract_entry(const char *archive, const char *entry,
              AppImageFormat format, off_t offset, GByteArray **output)
{
    if (!archive || !entry || *entry == '\0')
        return FALSE;

    g_debug("extract_entry: trying '%s' from '%s' (format=%s, offset=%" G_GINT64_FORMAT ")",
            entry, archive, appimage_format_name(format), (gint64)offset);

    /* Try SquashFS extraction unless format is definitely DwarFS */
    if (format != APPIMAGE_FORMAT_DWARFS && squashfs_tools_available() && offset > 0) {
        if (squashfs_extract_entry(archive, entry, offset, output)) {
            g_debug("extract_entry: unsquashfs succeeded for '%s'", entry);
            return TRUE;
        }
        g_debug("extract_entry: unsquashfs failed for '%s'", entry);
    }

    /* Try DwarFS extraction unless format is definitely SquashFS */
    if (format != APPIMAGE_FORMAT_SQUASHFS && dwarfs_tools_available()) {
        if (dwarfs_extract_entry(archive, entry, output)) {
            g_debug("extract_entry: dwarfsextract succeeded for '%s'", entry);
            return TRUE;
        }
        g_debug("extract_entry: dwarfsextract failed for '%s'", entry);
    }

    g_debug("extract_entry: all extraction methods failed for '%s'", entry);
    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  Symlink / pointer detection                                       */
/* ------------------------------------------------------------------ */

static gboolean
is_pointer_candidate(const guchar *data, gsize len, gchar **pointer_out)
{
    if (!data || len == 0 || len > POINTER_TEXT_LIMIT)
        return FALSE;

    for (gsize i = 0; i < len; ++i) {
        if (data[i] == '\0')
            return FALSE;
        if (!g_ascii_isprint(data[i]) && !g_ascii_isspace(data[i]))
            return FALSE;
    }

    gchar *text = g_strndup((const gchar *)data, (gssize)len);
    gchar *trimmed = g_strstrip(text);
    if (*trimmed == '\0') {
        g_free(text);
        return FALSE;
    }

    for (const gchar *c = trimmed; *c != '\0'; ++c) {
        if (*c == '/' || *c == '.' || *c == '-' || *c == '_' || g_ascii_isalnum(*c))
            continue;
        g_free(text);
        return FALSE;
    }

    if (pointer_out)
        *pointer_out = g_strdup(trimmed);

    g_debug("is_pointer_candidate: detected pointer/symlink target '%s'", trimmed);
    g_free(text);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Image processing (SVG / raster)                                   */
/* ------------------------------------------------------------------ */

static gboolean
payload_is_svg(const guchar *data, gsize len)
{
    if (!data || len == 0)
        return FALSE;

    gboolean uncertain = FALSE;
    gchar *mime = g_content_type_guess(NULL, data, len, &uncertain);
    gboolean is_svg = mime && g_content_type_is_a(mime, "image/svg+xml");
    g_debug("payload_is_svg: content-type guess '%s' (uncertain=%d, is_svg=%d)",
            mime ? mime : "(null)", uncertain, is_svg);
    g_free(mime);
    if (is_svg)
        return TRUE;

    const gsize probe = MIN(len, (gsize)1024);
    gchar *lower = g_ascii_strdown((const gchar *)data, (gssize)probe);
    gboolean found = lower && g_strstr_len(lower, (gssize)probe, "<svg") != NULL;
    g_free(lower);
    g_debug("payload_is_svg: <svg> tag probe result: %s", found ? "found" : "not found");
    return found;
}

static gboolean
process_svg_payload(const guchar *data, gsize len, const char *out_path, int size)
{
    g_debug("process_svg_payload: %" G_GSIZE_FORMAT " bytes, target %d, output '%s'",
            len, size, out_path);

    GError *error = NULL;
    RsvgHandle *handle = rsvg_handle_new_from_data(data, len, &error);
    if (!handle) {
        g_debug("process_svg_payload: parse failed: %s", error ? error->message : "unknown");
        g_printerr("Failed to parse SVG icon: %s\n", error ? error->message : "unknown");
        if (error)
            g_error_free(error);
        return FALSE;
    }

    RsvgDimensionData dim;
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    rsvg_handle_get_dimensions(handle, &dim);
    G_GNUC_END_IGNORE_DEPRECATIONS

    double width  = dim.width  > 0 ? dim.width  : size;
    double height = dim.height > 0 ? dim.height : size;
    if (width  <= 0) width  = size;
    if (height <= 0) height = size;

    double scale = MIN((double)size / width, (double)size / height);
    if (!isfinite(scale) || scale <= 0)
        scale = (double)size / MAX(width, height);
    if (!isfinite(scale) || scale <= 0)
        scale = 1.0;

    const double scaled_w = width * scale;
    const double scaled_h = height * scale;
    const int target_w = size;
    const int target_h = size;

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, target_w, target_h);
    cairo_t *cr = cairo_create(surface);

    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    const double translate_x = (target_w - scaled_w) / 2.0;
    const double translate_y = (target_h - scaled_h) / 2.0;
    cairo_translate(cr, translate_x, translate_y);
    cairo_scale(cr, scale, scale);

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gboolean render_ok = rsvg_handle_render_cairo(handle, cr);
    G_GNUC_END_IGNORE_DEPRECATIONS
    cairo_destroy(cr);

    if (!render_ok || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        g_printerr("Failed to render SVG icon\n");
        cairo_surface_destroy(surface);
        g_object_unref(handle);
        return FALSE;
    }

    cairo_status_t status = cairo_surface_write_to_png(surface, out_path);
    cairo_surface_destroy(surface);
    g_object_unref(handle);

    if (status != CAIRO_STATUS_SUCCESS) {
        g_printerr("Failed to write SVG thumbnail: %s\n", cairo_status_to_string(status));
        return FALSE;
    }

    g_debug("process_svg_payload: thumbnail written to '%s'", out_path);
    return TRUE;
}

static GdkPixbuf *
scale_pixbuf(GdkPixbuf *pixbuf, int size)
{
    const int width  = gdk_pixbuf_get_width(pixbuf);
    const int height = gdk_pixbuf_get_height(pixbuf);

    if (width <= 0 || height <= 0)
        return NULL;

    double scale = MIN((double)size / (double)width, (double)size / (double)height);
    if (!isfinite(scale) || scale <= 0)
        scale = 1.0;

    int target_w = MAX(1, (int)lround(width * scale));
    int target_h = MAX(1, (int)lround(height * scale));
    target_w = MIN(target_w, size);
    target_h = MIN(target_h, size);

    if (target_w == width && target_h == height) {
        g_object_ref(pixbuf);
        return pixbuf;
    }

    return gdk_pixbuf_scale_simple(pixbuf, target_w, target_h, GDK_INTERP_BILINEAR);
}

static gboolean
process_icon_payload(const guchar *data, gsize len, const char *out_path, int size)
{
    g_debug("process_icon_payload: %" G_GSIZE_FORMAT " bytes, target size %d", len, size);

    static gboolean empty_warned = FALSE;
    if (!data || len == 0) {
        if (!empty_warned) {
            g_printerr("Icon payload is empty or missing\n");
            empty_warned = TRUE;
        }
        return FALSE;
    }

    if (payload_is_svg(data, len)) {
        g_debug("process_icon_payload: detected SVG, delegating");
        if (process_svg_payload(data, len, out_path, size))
            return TRUE;
        g_debug("process_icon_payload: SVG failed, trying raster fallback");
    }

    GError *error = NULL;
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();

    if (!gdk_pixbuf_loader_write(loader, data, len, &error)) {
        g_printerr("Failed to load image bytes: %s\n", error->message);
        g_error_free(error);
        g_object_unref(loader);
        return FALSE;
    }

    if (!gdk_pixbuf_loader_close(loader, &error)) {
        g_printerr("Failed to finalize image decode: %s\n", error->message);
        g_error_free(error);
        g_object_unref(loader);
        return FALSE;
    }

    GdkPixbuf *pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
    if (!pixbuf) {
        g_printerr("Image loader returned NULL pixbuf\n");
        g_object_unref(loader);
        return FALSE;
    }

    g_object_ref(pixbuf);
    g_object_unref(loader);

    g_debug("process_icon_payload: loaded raster %dx%d",
            gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf));

    GdkPixbuf *scaled = scale_pixbuf(pixbuf, size);
    if (!scaled)
        scaled = g_object_ref(pixbuf);

    gboolean ok = gdk_pixbuf_save(scaled, out_path, "png", &error, NULL);
    if (!ok) {
        g_printerr("Failed to write thumbnail: %s\n", error->message);
        g_error_free(error);
    } else {
        g_debug("process_icon_payload: thumbnail written to '%s'", out_path);
    }

    g_object_unref(scaled);
    g_object_unref(pixbuf);
    return ok;
}

/* ------------------------------------------------------------------ */
/*  Symlink-following entry processor (up to MAX_SYMLINK_DEPTH)       */
/* ------------------------------------------------------------------ */

static gboolean
process_entry_following_symlinks(const char *archive, const char *entry,
                                 const char *out_path, int size,
                                 AppImageFormat format, off_t offset)
{
    if (!entry)
        return FALSE;

    g_debug("process_entry_following_symlinks: starting with '%s'", entry);

    gchar *current = g_strdup(entry);
    for (int depth = 0; depth < MAX_SYMLINK_DEPTH && current != NULL; ++depth) {
        g_debug("process_entry_following_symlinks: depth %d, trying '%s'", depth, current);

        GByteArray *payload = NULL;
        if (!extract_entry(archive, current, format, offset, &payload)) {
            g_debug("process_entry_following_symlinks: extraction failed for '%s' at depth %d",
                    current, depth);
            g_free(current);
            return FALSE;
        }

        gchar *next = NULL;
        if (is_pointer_candidate(payload->data, payload->len, &next)) {
            g_debug("process_entry_following_symlinks: '%s' -> '%s' (depth %d)",
                    current, next, depth);
            g_byte_array_unref(payload);
            g_free(current);
            current = next;
            continue;
        }

        g_debug("process_entry_following_symlinks: '%s' is data (%u bytes), processing",
                current, payload->len);
        gboolean ok = process_icon_payload(payload->data, payload->len, out_path, size);
        g_byte_array_unref(payload);
        g_free(current);
        g_free(next);
        return ok;
    }

    g_debug("process_entry_following_symlinks: exceeded max depth (%d) for '%s'",
            MAX_SYMLINK_DEPTH, entry);
    g_free(current);
    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  CLI helpers                                                        */
/* ------------------------------------------------------------------ */

static char *
canonicalize_path(const char *path)
{
    char *resolved = realpath(path, NULL);
    if (resolved)
        return resolved;
    return g_canonicalize_filename(path, NULL);
}

static int
parse_size_argument(const char *arg)
{
    if (!arg)
        return DEFAULT_THUMBNAIL_SIZE;
    char *endptr = NULL;
    long value = strtol(arg, &endptr, 10);
    if (endptr == arg || value <= 0 || value > 4096)
        return DEFAULT_THUMBNAIL_SIZE;
    return (int)value;
}

static void
print_usage(const char *progname)
{
    g_print("Usage: %s [OPTIONS] <APPIMAGE> <OUTPUT> [SIZE]\n", progname);
    g_print("\n");
    g_print("Extract the embedded icon from an AppImage and write it as a PNG thumbnail.\n");
    g_print("Uses unsquashfs for SquashFS-based AppImages and bundled DwarFS tools for\n");
    g_print("DwarFS-based AppImages.\n");
    g_print("\n");
    g_print("Arguments:\n");
    g_print("  <APPIMAGE>        Path to the AppImage file\n");
    g_print("  <OUTPUT>          Path to the output PNG thumbnail\n");
    g_print("  [SIZE]            Thumbnail size in pixels (default: 256, range: 1-4096)\n");
    g_print("\n");
    g_print("Options:\n");
    g_print("  -h, --help        Print this help message and exit\n");
    g_print("  -V, --version     Print version information and exit\n");
    g_print("\n");
    g_print("Examples:\n");
    g_print("  %s app.AppImage thumbnail.png\n", progname);
    g_print("  %s app.AppImage thumbnail.png 128\n", progname);
    g_print("\n");
    g_print("Conforms to the freedesktop.org thumbnail specification:\n");
    g_print("  <https://specifications.freedesktop.org/thumbnail-spec/latest>\n");
    g_print("\n");
    g_print("License:\n");
    g_print("  MIT License\n");
    g_print("  Copyright (c) Arnis Kemlers\n");
    g_print("  <https://github.com/kem-a/appimage-thumbnailer>\n");
    g_print("\n");
    g_print("Third-party components:\n");
    g_print("  Includes prebuilt DwarFS binaries from <https://github.com/mhx/dwarfs>\n");
    g_print("  DwarFS is distributed under the MIT and GPL-3.0 licenses.\n");
    g_print("  Uses unsquashfs from squashfs-tools <https://github.com/plougher/squashfs-tools>\n");
    g_print("  squashfs-tools is distributed under the GPL-2.0 license.\n");
}

static void
print_version(void)
{
    g_print("appimage-thumbnailer %s\n", APPIMAGE_THUMBNAILER_VERSION);
    g_print("Copyright (c) Arnis Kemlers\n");
    g_print("License: MIT\n");
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
    if (argc == 2) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
            print_version();
            return EXIT_SUCCESS;
        }
    }

    if (argc < 3 || argc > 4) {
        g_printerr("Usage: %s <AppImage> <output.png> [size]\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* Detect AppImage format and payload offset */
    char *input  = canonicalize_path(argv[1]);
    char *output = g_canonicalize_filename(argv[2], NULL);
    if (!input || !output) {
        g_printerr("Failed to resolve paths\n");
        g_free(input);
        g_free(output);
        return EXIT_FAILURE;
    }

    const int size = parse_size_argument(argc == 4 ? argv[3] : NULL);

    AppImageFormat format = appimage_detect_format(input);
    off_t offset = appimage_payload_offset(input);

    g_debug("main: input='%s', output='%s', size=%d", input, output, size);
    g_debug("main: format=%s, offset=%" G_GINT64_FORMAT,
            appimage_format_name(format), (gint64)offset);

    gboolean have_squashfs = squashfs_tools_available();
    gboolean have_dwarfs   = dwarfs_tools_available();

    g_debug("main: unsquashfs available: %s", have_squashfs ? "yes" : "no");
    g_debug("main: dwarfs tools available: %s", have_dwarfs  ? "yes" : "no");

    if (!have_squashfs && !have_dwarfs) {
        g_printerr("Neither unsquashfs (squashfs-tools) nor dwarfs tools are available.\n");
        g_printerr("Install squashfs-tools for SquashFS AppImages or dwarfs for DwarFS AppImages.\n");
        g_free(input);
        g_free(output);
        return EXIT_FAILURE;
    }

    if (format == APPIMAGE_FORMAT_SQUASHFS && !have_squashfs) {
        g_printerr("SquashFS AppImage detected but unsquashfs is not available.\n");
        g_printerr("Install squashfs-tools to handle this AppImage.\n");
        g_free(input);
        g_free(output);
        return EXIT_FAILURE;
    }

    if (format == APPIMAGE_FORMAT_DWARFS && !have_dwarfs) {
        g_printerr("DwarFS AppImage detected but dwarfs tools are not available.\n");
        g_free(input);
        g_free(output);
        return EXIT_FAILURE;
    }

    /* Extract .DirIcon (required by AppImage spec) */
    g_debug("main: trying .DirIcon");
    gboolean success = process_entry_following_symlinks(
        input, ".DirIcon", output, size, format, offset);

    if (!success) {
        g_debug("main: .DirIcon not found or extraction failed for '%s'", input);
        g_printerr("Failed to extract .DirIcon from AppImage\n");
    } else {
        g_debug("main: thumbnail generated at '%s'", output);
    }

    g_free(input);
    g_free(output);
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
