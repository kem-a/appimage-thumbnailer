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

#include "dwarfs-extract.h"

#define DEFAULT_THUMBNAIL_SIZE 256
#define MAX_SYMLINK_DEPTH 5
#define POINTER_TEXT_LIMIT 1024

#ifndef APPIMAGE_THUMBNAILER_VERSION
#define APPIMAGE_THUMBNAILER_VERSION "unknown"
#endif

#define MIN_7Z_MAJOR 23
#define MIN_7Z_MINOR 1

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

static gboolean sevenz_checked = FALSE;
static gboolean sevenz_available = FALSE;

static gboolean
command_capture(const char *const argv[], GByteArray **output)
{
    if (argv && argv[0])
        g_debug("command_capture: running '%s'", argv[0]);

    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
        g_debug("command_capture: pipe() failed: %s", g_strerror(errno));
        g_printerr("Failed to create pipe: %s\n", g_strerror(errno));
        return FALSE;
    }

    pid_t pid = fork();
    if (pid < 0) {
        g_printerr("Failed to fork: %s\n", g_strerror(errno));
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return FALSE;
    }

    if (pid == 0) {
        close(pipe_fd[0]);
        if (dup2(pipe_fd[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv[0], (char *const *) argv);
        /* execvp failed - cannot use g_debug here (post-fork) */
        _exit(127);
    }

    g_debug("command_capture: forked child pid %d for '%s'", (int) pid, argv[0]);

    close(pipe_fd[1]);
    GByteArray *arr = g_byte_array_new();
    guchar buffer[8192];
    ssize_t bytes_read;

    while ((bytes_read = read(pipe_fd[0], buffer, sizeof buffer)) != 0) {
        if (bytes_read < 0) {
            if (errno == EINTR)
                continue;
            g_printerr("Failed to read command output: %s\n", g_strerror(errno));
            g_byte_array_unref(arr);
            close(pipe_fd[0]);
            waitpid(pid, NULL, 0);
            return FALSE;
        }
        g_byte_array_append(arr, buffer, (guint) bytes_read);
    }

    close(pipe_fd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        g_printerr("Failed to wait for command: %s\n", g_strerror(errno));
        g_byte_array_unref(arr);
        return FALSE;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        g_debug("command_capture: '%s' exited with status %d (normal=%d)",
                argv[0], WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                WIFEXITED(status));
        g_byte_array_unref(arr);
        return FALSE;
    }

    g_debug("command_capture: '%s' succeeded, captured %u bytes", argv[0], arr->len);
    *output = arr;
    return TRUE;
}

/**
 * Check if 7z is installed and has a version >= 23.01 (required for -tSquashFS).
 * The result is cached after the first call.
 */
static gboolean
check_7z_available(void)
{
    if (sevenz_checked)
        return sevenz_available;

    sevenz_checked = TRUE;
    sevenz_available = FALSE;

    gchar *path = g_find_program_in_path("7z");
    if (!path) {
        g_debug("check_7z_available: 7z not found in PATH");
        return FALSE;
    }
    g_debug("check_7z_available: found 7z at '%s'", path);
    g_free(path);

    /* Run '7z' with no arguments to get the version banner */
    const char *argv[] = {"7z", NULL};
    /* 7z with no args exits non-zero but still prints the banner to stdout;
     * we need to capture even on failure, so call it via a small wrapper. */
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0)
        return FALSE;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return FALSE;
    }
    if (pid == 0) {
        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp("7z", (char *const *) argv);
        _exit(127);
    }
    close(pipe_fd[1]);

    GByteArray *arr = g_byte_array_new();
    guchar buffer[4096];
    ssize_t n;
    while ((n = read(pipe_fd[0], buffer, sizeof buffer)) > 0)
        g_byte_array_append(arr, buffer, (guint) n);
    close(pipe_fd[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    /* Even exit-code != 0 is fine, we just need the banner text */
    if (arr->len == 0) {
        g_debug("check_7z_available: 7z produced no output, cannot determine version");
        g_byte_array_unref(arr);
        return FALSE;
    }

    /* Null-terminate for string scanning */
    g_byte_array_append(arr, (const guchar *) "\0", 1);
    const gchar *banner = (const gchar *) arr->data;

    /* Banner looks like: "7-Zip [64] 16.02 ..." or "7-Zip (z) 23.01 (x64) ..." */
    int major = 0, minor = 0;
    const gchar *p = banner;
    while (*p) {
        if (g_ascii_isdigit(*p)) {
            gchar *end = NULL;
            long v = strtol(p, &end, 10);
            if (end && *end == '.') {
                major = (int) v;
                minor = (int) strtol(end + 1, NULL, 10);
                break;
            }
        }
        p++;
    }

    g_byte_array_unref(arr);

    if (major == 0 && minor == 0) {
        g_debug("check_7z_available: could not parse version from 7z output");
        return FALSE;
    }

    g_debug("check_7z_available: detected 7z version %d.%02d (minimum required: %d.%02d)",
            major, minor, MIN_7Z_MAJOR, MIN_7Z_MINOR);

    if (major > MIN_7Z_MAJOR || (major == MIN_7Z_MAJOR && minor >= MIN_7Z_MINOR)) {
        sevenz_available = TRUE;
        return TRUE;
    }

    g_debug("check_7z_available: 7z version %d.%02d is too old (need >= %d.%02d for -tSquashFS)",
            major, minor, MIN_7Z_MAJOR, MIN_7Z_MINOR);
    return FALSE;
}

static gboolean
extract_entry_7z(const char *archive, const char *entry, GByteArray **output)
{
    g_debug("extract_entry_7z: attempting to extract '%s' from '%s'", entry ? entry : "(null)", archive ? archive : "(null)");

    if (!archive || !entry || *entry == '\0')
        return FALSE;

    if (!check_7z_available())
        return FALSE;

    gchar *clean_entry = NULL;
    if (entry[0] == '/')
        clean_entry = g_strdup(entry + 1);
    else
        clean_entry = g_strdup(entry);

    /* Use -tSquashFS to force correct format detection for AppImages with multiple
     * compression types (e.g., zstd-compressed ELF header + SquashFS payload) */
    const char *argv[] = {"7z", "e", "-so", "-tSquashFS", archive, clean_entry, NULL};
    gboolean ok = command_capture(argv, output);

    /* 7z may return success (exit 0) but produce no output for unsupported formats.
     * Treat empty output as failure so we can fall back to other extractors. */
    if (ok && (*output == NULL || (*output)->len == 0)) {
        g_debug("extract_entry_7z: 7z returned success but produced no output for '%s'", clean_entry);
        if (*output) {
            g_byte_array_unref(*output);
            *output = NULL;
        }
        g_free(clean_entry);
        return FALSE;
    }

    if (!ok)
        g_debug("extract_entry_7z: 7z failed to extract '%s'", clean_entry);
    else
        g_debug("extract_entry_7z: extracted '%s' (%u bytes)", clean_entry, (*output)->len);

    g_free(clean_entry);
    return ok;
}

static gboolean
extract_entry(const char *archive, const char *entry, GByteArray **output)
{
    if (!archive || !entry || *entry == '\0')
        return FALSE;

    g_debug("extract_entry: trying to extract '%s' from '%s'", entry, archive);

    /* Try 7z first (works for SquashFS-based AppImages) */
    if (check_7z_available()) {
        if (extract_entry_7z(archive, entry, output)) {
            g_debug("extract_entry: 7z succeeded for '%s'", entry);
            return TRUE;
        }
        g_debug("extract_entry: 7z failed for '%s', trying dwarfs fallback", entry);
    } else {
        g_debug("extract_entry: 7z not available, skipping SquashFS extraction");
    }

    /* Fall back to dwarfsextract (for DwarFS images) */
    if (dwarfs_tools_available()) {
        if (dwarfs_extract_entry(archive, entry, output)) {
            g_debug("extract_entry: dwarfsextract succeeded for '%s'", entry);
            return TRUE;
        }
        g_debug("extract_entry: dwarfsextract also failed for '%s'", entry);
    } else {
        g_debug("extract_entry: dwarfs tools not available, skipping dwarfs fallback");
    }

    g_debug("extract_entry: all extraction methods failed for '%s'", entry);
    return FALSE;
}

static gboolean
list_archive_paths_7z(const char *archive, GPtrArray **paths_out)
{
    /* Use -tSquashFS to force correct format detection for AppImages with multiple
     * compression types (e.g., zstd-compressed ELF header + SquashFS payload) */
    const char *argv[] = {"7z", "l", "-slt", "-tSquashFS", archive, NULL};
    GByteArray *output = NULL;
    if (!command_capture(argv, &output))
        return FALSE;

    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    gchar **lines = g_strsplit((const gchar *) output->data, "\n", -1);
    for (gint i = 0; lines[i] != NULL; ++i) {
        gchar *line = g_strstrip(lines[i]);
        if (g_str_has_prefix(line, "Path = ")) {
            const gchar *path = line + 7;
            if (*path != '\0') {
                gchar *normalized = g_strdup(path[0] == '/' ? path + 1 : path);
                g_ptr_array_add(paths, normalized);
            }
        }
    }
    g_strfreev(lines);
    g_byte_array_unref(output);

    if (paths->len == 0) {
        g_debug("list_archive_paths_7z: no paths found in archive");
        g_ptr_array_unref(paths);
        return FALSE;
    }

    g_debug("list_archive_paths_7z: found %u entries in archive", paths->len);
    *paths_out = paths;
    return TRUE;
}

static gboolean
list_archive_paths(const char *archive, GPtrArray **paths_out)
{
    g_debug("list_archive_paths: listing entries in '%s'", archive);

    /* Try 7z first (works for SquashFS) */
    if (check_7z_available()) {
        if (list_archive_paths_7z(archive, paths_out)) {
            g_debug("list_archive_paths: 7z listing succeeded");
            return TRUE;
        }
        g_debug("list_archive_paths: 7z listing failed, trying dwarfs fallback");
    } else {
        g_debug("list_archive_paths: 7z not available, skipping SquashFS listing");
    }

    /* Fall back to dwarfsck (for DwarFS images) */
    if (dwarfs_tools_available()) {
        if (dwarfs_list_paths(archive, paths_out)) {
            g_debug("list_archive_paths: dwarfsck listing succeeded");
            return TRUE;
        }
        g_debug("list_archive_paths: dwarfsck listing also failed");
    } else {
        g_debug("list_archive_paths: dwarfs tools not available, skipping dwarfs fallback");
    }

    g_debug("list_archive_paths: all listing methods failed for '%s'", archive);
    return FALSE;
}

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

    gchar *text = g_strndup((const gchar *) data, (gssize) len);
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

    const gsize probe = MIN(len, (gsize) 1024);
    gchar *lower = g_ascii_strdown((const gchar *) data, (gssize) probe);
    gboolean found = lower && g_strstr_len(lower, (gssize) probe, "<svg") != NULL;
    g_free(lower);
    g_debug("payload_is_svg: <svg> tag probe result: %s", found ? "found" : "not found");
    return found;
}

static gboolean
process_svg_payload(const guchar *data, gsize len, const char *out_path, int size)
{
    g_debug("process_svg_payload: processing SVG data (%" G_GSIZE_FORMAT " bytes), target size %d, output '%s'",
            len, size, out_path);

    GError *error = NULL;
    RsvgHandle *handle = rsvg_handle_new_from_data(data, len, &error);
    if (!handle) {
        g_debug("process_svg_payload: failed to parse SVG: %s", error ? error->message : "unknown error");
        g_printerr("Failed to parse SVG icon: %s\n", error ? error->message : "unknown error");
        if (error)
            g_error_free(error);
        return FALSE;
    }

    RsvgDimensionData dim;
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    rsvg_handle_get_dimensions(handle, &dim);
    G_GNUC_END_IGNORE_DEPRECATIONS

    double width = dim.width > 0 ? dim.width : size;
    double height = dim.height > 0 ? dim.height : size;
    if (width <= 0)
        width = size;
    if (height <= 0)
        height = size;

    g_debug("process_svg_payload: SVG intrinsic dimensions %dx%d, effective %.0fx%.0f",
            dim.width, dim.height, width, height);

    double scale = MIN((double) size / width, (double) size / height);
    if (!isfinite(scale) || scale <= 0)
        scale = (double) size / MAX(width, height);
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
        g_debug("process_svg_payload: render failed (render_ok=%d, surface_status=%d)",
                render_ok, cairo_surface_status(surface));
        g_printerr("Failed to render SVG icon\n");
        cairo_surface_destroy(surface);
        g_object_unref(handle);
        return FALSE;
    }

    cairo_status_t status = cairo_surface_write_to_png(surface, out_path);
    cairo_surface_destroy(surface);
    g_object_unref(handle);

    if (status != CAIRO_STATUS_SUCCESS) {
        g_debug("process_svg_payload: failed to write PNG: %s", cairo_status_to_string(status));
        g_printerr("Failed to write SVG thumbnail: %s\n", cairo_status_to_string(status));
        return FALSE;
    }

    g_debug("process_svg_payload: SVG thumbnail written successfully to '%s'", out_path);
    return TRUE;
}

static GdkPixbuf *
scale_pixbuf(GdkPixbuf *pixbuf, int size)
{
    const int width = gdk_pixbuf_get_width(pixbuf);
    const int height = gdk_pixbuf_get_height(pixbuf);

    if (width <= 0 || height <= 0)
        return NULL;

    double scale = MIN((double) size / (double) width, (double) size / (double) height);
    if (!isfinite(scale) || scale <= 0)
        scale = 1.0;

    int target_w = MAX(1, (int) lround(width * scale));
    int target_h = MAX(1, (int) lround(height * scale));

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
    g_debug("process_icon_payload: processing %" G_GSIZE_FORMAT " bytes, target size %d", len, size);

    static gboolean empty_payload_warned = FALSE;
    if (!data || len == 0) {
        g_debug("process_icon_payload: payload is empty or NULL");
        if (!empty_payload_warned) {
            g_printerr("Icon payload is empty or missing\n");
            empty_payload_warned = TRUE;
        }
        return FALSE;
    }

    if (payload_is_svg(data, len)) {
        g_debug("process_icon_payload: detected SVG format, delegating to SVG processor");
        if (process_svg_payload(data, len, out_path, size))
            return TRUE;
        g_debug("process_icon_payload: SVG processing failed, trying raster fallback");
    }

    GError *error = NULL;
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();

    if (!gdk_pixbuf_loader_write(loader, data, len, &error)) {
        g_debug("process_icon_payload: pixbuf loader write failed: %s", error->message);
        g_printerr("Failed to load image bytes: %s\n", error->message);
        g_error_free(error);
        g_object_unref(loader);
        return FALSE;
    }

    if (!gdk_pixbuf_loader_close(loader, &error)) {
        g_debug("process_icon_payload: pixbuf loader close failed: %s", error->message);
        g_printerr("Failed to finalize image decode: %s\n", error->message);
        g_error_free(error);
        g_object_unref(loader);
        return FALSE;
    }

    GdkPixbuf *pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
    if (!pixbuf) {
        g_debug("process_icon_payload: pixbuf loader returned NULL pixbuf");
        g_printerr("Image loader returned NULL pixbuf\n");
        g_object_unref(loader);
        return FALSE;
    }

    g_object_ref(pixbuf);
    g_object_unref(loader);

    g_debug("process_icon_payload: loaded raster image %dx%d",
            gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf));

    GdkPixbuf *scaled = scale_pixbuf(pixbuf, size);
    if (!scaled)
        scaled = g_object_ref(pixbuf);

    g_debug("process_icon_payload: scaled to %dx%d",
            gdk_pixbuf_get_width(scaled), gdk_pixbuf_get_height(scaled));

    gboolean ok = gdk_pixbuf_save(scaled, out_path, "png", &error, NULL);
    if (!ok) {
        g_debug("process_icon_payload: failed to save PNG: %s", error->message);
        g_printerr("Failed to write thumbnail: %s\n", error->message);
        g_error_free(error);
    } else {
        g_debug("process_icon_payload: thumbnail written successfully to '%s'", out_path);
    }

    g_object_unref(scaled);
    g_object_unref(pixbuf);
    return ok;
}

static gboolean
process_entry_following_symlinks(const char *archive,
                                 const char *entry,
                                 const char *out_path,
                                 int size)
{
    if (!entry)
        return FALSE;

    g_debug("process_entry_following_symlinks: starting with entry '%s'", entry);

    gchar *current = g_strdup(entry);
    for (int depth = 0; depth < MAX_SYMLINK_DEPTH && current != NULL; ++depth) {
        g_debug("process_entry_following_symlinks: depth %d, trying '%s'", depth, current);

        GByteArray *payload = NULL;
        if (!extract_entry(archive, current, &payload)) {
            g_debug("process_entry_following_symlinks: failed to extract '%s' at depth %d", current, depth);
            g_free(current);
            return FALSE;
        }

        gchar *next = NULL;
        if (is_pointer_candidate(payload->data, payload->len, &next)) {
            g_debug("process_entry_following_symlinks: '%s' is a pointer to '%s' (depth %d)",
                    current, next, depth);
            g_byte_array_unref(payload);
            g_free(current);
            current = next;
            continue;
        }

        g_debug("process_entry_following_symlinks: '%s' is a data entry (%u bytes), processing as icon",
                current, payload->len);
        gboolean ok = process_icon_payload(payload->data, payload->len, out_path, size);
        g_byte_array_unref(payload);
        g_free(current);
        g_free(next);
        return ok;
    }

    g_debug("process_entry_following_symlinks: exceeded max symlink depth (%d) for '%s'",
            MAX_SYMLINK_DEPTH, entry);
    g_free(current);
    return FALSE;
}

static gboolean
path_is_root_file(const gchar *path)
{
    return path && strchr(path, '/') == NULL;
}

static gchar *
find_root_with_extension(GPtrArray *paths, const char *extension)
{
    if (!paths || !extension)
        return NULL;
    for (guint i = 0; i < paths->len; ++i) {
        const gchar *path = paths->pdata[i];
        if (!path_is_root_file(path))
            continue;
        const gchar *dot = strrchr(path, '.');
        if (dot && g_ascii_strcasecmp(dot, extension) == 0) {
            g_debug("find_root_with_extension: found '%s' matching extension '%s'", path, extension);
            return g_strdup(path);
        }
    }
    g_debug("find_root_with_extension: no root-level file with extension '%s' found", extension);
    return NULL;
}

static char *
canonicalize_path(const char *path)
{
    char *resolved = realpath(path, NULL);
    if (resolved)
        return resolved;
    gchar *absolute = g_canonicalize_filename(path, NULL);
    return absolute;
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
    return (int) value;
}

static void
print_usage(const char *progname)
{
    g_print("Usage: %s [OPTIONS] <APPIMAGE> <OUTPUT> [SIZE]\n", progname);
    g_print("\n");
    g_print("Extract the embedded icon from an AppImage and write it as a PNG thumbnail. It uses 7z and bundled DwarFS binaries to achieve this.\n");
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
}

static void
print_version(void)
{
    g_print("appimage-thumbnailer %s\n", APPIMAGE_THUMBNAILER_VERSION);
    g_print("Copyright (c) Arnis Kemlers\n");
    g_print("License: MIT\n");
}

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

    gboolean have_7z = check_7z_available();
    gboolean have_dwarfs = dwarfs_tools_available();

    g_debug("main: 7z (>= %d.%02d) available: %s", MIN_7Z_MAJOR, MIN_7Z_MINOR, have_7z ? "yes" : "no");
    g_debug("main: dwarfs tools available: %s", have_dwarfs ? "yes" : "no");

    if (!have_7z && !have_dwarfs) {
        g_printerr("Neither 7z (>= %d.%02d) nor dwarfs tools (dwarfsextract, dwarfsck) found.\n",
                   MIN_7Z_MAJOR, MIN_7Z_MINOR);
        g_printerr("Install p7zip (>= %d.%02d) for SquashFS AppImages or dwarfs for DwarFS AppImages.\n",
                   MIN_7Z_MAJOR, MIN_7Z_MINOR);
        return EXIT_FAILURE;
    }

    char *input = canonicalize_path(argv[1]);
    char *output = g_canonicalize_filename(argv[2], NULL);
    if (!input || !output) {
        g_printerr("Failed to resolve paths\n");
        g_free(input);
        g_free(output);
        return EXIT_FAILURE;
    }

    const int size = parse_size_argument(argc == 4 ? argv[3] : NULL);

    g_debug("main: input='%s', output='%s', size=%d", input, output, size);
    g_debug("main: trying .DirIcon first");

    gboolean success = process_entry_following_symlinks(input, ".DirIcon", output, size);

    if (!success) {
        g_debug("main: .DirIcon extraction failed, trying fallback icon search");
        GPtrArray *paths = NULL;
        if (list_archive_paths(input, &paths)) {
            g_debug("main: archive listing succeeded, searching for root-level SVG icon");
            gchar *svg_entry = find_root_with_extension(paths, ".svg");
            if (svg_entry) {
                g_debug("main: trying SVG fallback '%s'", svg_entry);
                success = process_entry_following_symlinks(input, svg_entry, output, size);
                g_free(svg_entry);
            }

            if (!success) {
                g_debug("main: SVG fallback failed or not found, searching for root-level PNG icon");
                gchar *png_entry = find_root_with_extension(paths, ".png");
                if (png_entry) {
                    g_debug("main: trying PNG fallback '%s'", png_entry);
                    success = process_entry_following_symlinks(input, png_entry, output, size);
                    g_free(png_entry);
                } else {
                    g_debug("main: no PNG fallback found either");
                }
            }

            g_ptr_array_unref(paths);
        } else {
            g_debug("main: failed to list archive contents, no fallback possible");
        }
    }

    if (!success) {
        g_debug("main: all icon extraction attempts failed for '%s'", input);
        g_printerr("Failed to extract icon from AppImage\n");
    } else {
        g_debug("main: thumbnail generated successfully at '%s'", output);
    }

    g_free(input);
    g_free(output);
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}