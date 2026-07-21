#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
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

/* Drop shadow beneath the thumbnail icon; toggled via the meson 'shadow' option. */
#ifndef APPIMAGE_ENABLE_SHADOW
#define APPIMAGE_ENABLE_SHADOW 1
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

/* ------------------------------------------------------------------ */
/*  Soft drop shadow (derived from the icon's alpha silhouette)        */
/* ------------------------------------------------------------------ */

typedef struct {
    int    blur_radius;    /* per-pass box radius, in pixels         */
    int    passes;         /* box-blur passes (3 ~= Gaussian)        */
    int    offset_x;       /* shadow offset                          */
    int    offset_y;
    double opacity;        /* shadow darkness at full coverage       */
    /* Per-side padding reserved for the shadow. Asymmetric: the shadow
       only falls below/right, so the icon sits high-and-centered and the
       large margin is reserved only where the shadow actually lands. */
    int    margin_top;
    int    margin_bottom;
    int    margin_left;
    int    margin_right;
} ShadowParams;

/* Icon placement box (origin + size) once the shadow margins are removed. */
static void
shadow_icon_box(const ShadowParams *sp, int size,
                int *bx, int *by, int *bw, int *bh)
{
    *bx = sp->margin_left;
    *by = sp->margin_top;
    *bw = MAX(1, size - sp->margin_left - sp->margin_right);
    *bh = MAX(1, size - sp->margin_top  - sp->margin_bottom);
}

static ShadowParams
shadow_params(int size)
{
#if APPIMAGE_ENABLE_SHADOW
    ShadowParams s;
    s.blur_radius = MAX(2, size / 64);
    s.passes      = 3;
    s.offset_x    = 0;
    s.offset_y    = MAX(2, size / 32);
    s.opacity     = 0.55;

    /* Reserve the blur's full support (radius per pass) so no tail clips,
       but asymmetrically: the downward offset is reserved only at the bottom,
       letting the icon sit high-and-centered and fill ~90% of the frame. */
    const int reach = s.blur_radius * s.passes;
    const int pad   = MAX(1, size / 256);

    s.margin_top    = MAX(0, reach - s.offset_y) + pad;
    s.margin_bottom = reach + s.offset_y + pad;
    s.margin_left   = MAX(0, reach - s.offset_x) + pad;
    s.margin_right  = reach + s.offset_x + pad;

    /* Never let any side eat more than a quarter of the frame. */
    const int cap = MAX(1, size / 4);
    s.margin_top    = MIN(s.margin_top,    cap);
    s.margin_bottom = MIN(s.margin_bottom, cap);
    s.margin_left   = MIN(s.margin_left,   cap);
    s.margin_right  = MIN(s.margin_right,  cap);
    return s;
#else
    /* Shadow disabled: no margins, so the icon fills the whole frame. */
    (void)size;
    const ShadowParams s = {0, 0, 0, 0, 0.0, 0, 0, 0, 0};
    return s;
#endif
}

#if APPIMAGE_ENABLE_SHADOW
/* Separable box blur on an 8-bit alpha buffer, in place. */
static void
box_blur_a8(unsigned char *data, int w, int h, int stride, int radius, int passes)
{
    if (radius < 1 || w <= 0 || h <= 0)
        return;

    unsigned char *tmp = g_malloc((size_t)stride * (size_t)h);
    const int win = 2 * radius + 1;

    for (int p = 0; p < passes; ++p) {
        /* horizontal: data -> tmp */
        for (int y = 0; y < h; ++y) {
            const unsigned char *row = data + (size_t)y * stride;
            unsigned char *out = tmp + (size_t)y * stride;
            int sum = 0;
            for (int i = -radius; i <= radius; ++i) {
                int xi = i < 0 ? 0 : (i >= w ? w - 1 : i);
                sum += row[xi];
            }
            for (int x = 0; x < w; ++x) {
                out[x] = (unsigned char)(sum / win);
                int add = x + radius + 1; if (add > w - 1) add = w - 1;
                int rem = x - radius;     if (rem < 0)     rem = 0;
                sum += row[add] - row[rem];
            }
        }
        /* vertical: tmp -> data */
        for (int x = 0; x < w; ++x) {
            int sum = 0;
            for (int i = -radius; i <= radius; ++i) {
                int yi = i < 0 ? 0 : (i >= h ? h - 1 : i);
                sum += tmp[(size_t)yi * stride + x];
            }
            for (int y = 0; y < h; ++y) {
                data[(size_t)y * stride + x] = (unsigned char)(sum / win);
                int add = y + radius + 1; if (add > h - 1) add = h - 1;
                int rem = y - radius;     if (rem < 0)     rem = 0;
                sum += tmp[(size_t)add * stride + x] - tmp[(size_t)rem * stride + x];
            }
        }
    }

    g_free(tmp);
}
#endif /* APPIMAGE_ENABLE_SHADOW */

/* Blit a (already scaled) pixbuf onto an ARGB32 Cairo surface, centered
   within the box (box_x, box_y, box_w, box_h). */
static void
paint_pixbuf_in_box(cairo_surface_t *dst, GdkPixbuf *pixbuf, int surf_size,
                    int box_x, int box_y, int box_w, int box_h)
{
    const int pw = gdk_pixbuf_get_width(pixbuf);
    const int ph = gdk_pixbuf_get_height(pixbuf);
    const int nch = gdk_pixbuf_get_n_channels(pixbuf);
    const gboolean has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);
    const int src_stride = gdk_pixbuf_get_rowstride(pixbuf);
    const guchar *src = gdk_pixbuf_get_pixels(pixbuf);

    cairo_surface_flush(dst);
    unsigned char *dbuf = cairo_image_surface_get_data(dst);
    const int dstride = cairo_image_surface_get_stride(dst);

    int ox = box_x + (box_w - pw) / 2; if (ox < 0) ox = 0;
    int oy = box_y + (box_h - ph) / 2; if (oy < 0) oy = 0;
    const int copy_w = MIN(pw, surf_size - ox);
    const int copy_h = MIN(ph, surf_size - oy);

    for (int y = 0; y < copy_h; ++y) {
        const guchar *srow = src + (size_t)y * src_stride;
        uint32_t *drow = (uint32_t *)(dbuf + (size_t)(oy + y) * dstride);
        for (int x = 0; x < copy_w; ++x) {
            const guchar *ppix = srow + (size_t)x * nch;
            const guchar r = ppix[0], g = ppix[1], b = ppix[2];
            const guchar a = has_alpha ? ppix[3] : 0xFF;
            /* CAIRO_FORMAT_ARGB32 stores premultiplied, alpha in the high byte */
            const guchar pr = (guchar)((r * a + 127) / 255);
            const guchar pg = (guchar)((g * a + 127) / 255);
            const guchar pb = (guchar)((b * a + 127) / 255);
            drow[ox + x] = ((uint32_t)a  << 24) | ((uint32_t)pr << 16) |
                           ((uint32_t)pg << 8)  |  (uint32_t)pb;
        }
    }

    cairo_surface_mark_dirty(dst);
}

/* Composite a soft shadow (from `icon`'s alpha) under `icon`, write PNG. */
static gboolean
compose_shadow_and_write(cairo_surface_t *icon, int size, const char *out_path)
{
    if (cairo_surface_status(icon) != CAIRO_STATUS_SUCCESS)
        return FALSE;

#if !APPIMAGE_ENABLE_SHADOW
    /* Shadow disabled: the icon already fills the frame; write it as-is. */
    (void)size;
    const cairo_status_t st = cairo_surface_write_to_png(icon, out_path);
    if (st != CAIRO_STATUS_SUCCESS) {
        g_printerr("Failed to write thumbnail: %s\n", cairo_status_to_string(st));
        return FALSE;
    }
    return TRUE;
#else
    const ShadowParams sp = shadow_params(size);

    cairo_surface_t *mask = cairo_image_surface_create(CAIRO_FORMAT_A8, size, size);
    if (cairo_surface_status(mask) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(mask);
        return FALSE;
    }

    /* Copy the icon's alpha channel into the A8 mask, then blur it. */
    cairo_surface_flush(icon);
    const unsigned char *ibuf = cairo_image_surface_get_data(icon);
    const int istride = cairo_image_surface_get_stride(icon);
    unsigned char *mbuf = cairo_image_surface_get_data(mask);
    const int mstride = cairo_image_surface_get_stride(mask);

    for (int y = 0; y < size; ++y) {
        const uint32_t *irow = (const uint32_t *)(ibuf + (size_t)y * istride);
        unsigned char *mrow = mbuf + (size_t)y * mstride;
        for (int x = 0; x < size; ++x)
            mrow[x] = (unsigned char)((irow[x] >> 24) & 0xFF);
    }

    box_blur_a8(mbuf, size, size, mstride, sp.blur_radius, sp.passes);
    cairo_surface_mark_dirty(mask);

    cairo_surface_t *canvas = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(canvas);

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, sp.opacity);
    cairo_mask_surface(cr, mask, sp.offset_x, sp.offset_y);

    cairo_set_source_surface(cr, icon, 0, 0);
    cairo_paint(cr);

    cairo_destroy(cr);
    cairo_surface_destroy(mask);

    const cairo_status_t status = cairo_surface_write_to_png(canvas, out_path);
    cairo_surface_destroy(canvas);

    if (status != CAIRO_STATUS_SUCCESS) {
        g_printerr("Failed to write thumbnail: %s\n", cairo_status_to_string(status));
        return FALSE;
    }
    return TRUE;
#endif /* APPIMAGE_ENABLE_SHADOW */
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

    /* Leave a margin so the drop shadow has room to spread. */
    const ShadowParams sp = shadow_params(size);
    int bx, by, bw, bh;
    shadow_icon_box(&sp, size, &bx, &by, &bw, &bh);

    double scale = MIN((double)bw / width, (double)bh / height);
    if (!isfinite(scale) || scale <= 0)
        scale = (double)MIN(bw, bh) / MAX(width, height);
    if (!isfinite(scale) || scale <= 0)
        scale = 1.0;

    const double scaled_w = width * scale;
    const double scaled_h = height * scale;

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(surface);

    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    const double translate_x = bx + (bw - scaled_w) / 2.0;
    const double translate_y = by + (bh - scaled_h) / 2.0;
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

    gboolean ok = compose_shadow_and_write(surface, size, out_path);
    cairo_surface_destroy(surface);
    g_object_unref(handle);

    if (ok)
        g_debug("process_svg_payload: thumbnail written to '%s'", out_path);
    return ok;
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

    /* Leave a margin so the drop shadow has room to spread. */
    const ShadowParams sp = shadow_params(size);
    int bx, by, bw, bh;
    shadow_icon_box(&sp, size, &bx, &by, &bw, &bh);

    GdkPixbuf *scaled = scale_pixbuf(pixbuf, MIN(bw, bh));
    if (!scaled)
        scaled = g_object_ref(pixbuf);

    cairo_surface_t *icon = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    gboolean ok = FALSE;
    if (cairo_surface_status(icon) == CAIRO_STATUS_SUCCESS) {
        paint_pixbuf_in_box(icon, scaled, size, bx, by, bw, bh);
        ok = compose_shadow_and_write(icon, size, out_path);
        if (ok)
            g_debug("process_icon_payload: thumbnail written to '%s'", out_path);
    } else {
        g_printerr("Failed to allocate image surface for thumbnail\n");
    }
    cairo_surface_destroy(icon);

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
