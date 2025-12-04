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
#include <librsvg/rsvg.h>

#define DEFAULT_THUMBNAIL_SIZE 256
#define MAX_SYMLINK_DEPTH 5
#define POINTER_TEXT_LIMIT 1024

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

static gboolean
command_capture(const char *const argv[], GByteArray **output)
{
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
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
        _exit(127);
    }

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
        g_byte_array_unref(arr);
        return FALSE;
    }

    *output = arr;
    return TRUE;
}

static gboolean
extract_entry(const char *archive, const char *entry, GByteArray **output)
{
    if (!archive || !entry || *entry == '\0')
        return FALSE;

    gchar *clean_entry = NULL;
    if (entry[0] == '/')
        clean_entry = g_strdup(entry + 1);
    else
        clean_entry = g_strdup(entry);

    const char *argv[] = {"7z", "e", "-so", archive, clean_entry, NULL};
    gboolean ok = command_capture(argv, output);
    g_free(clean_entry);
    return ok;
}

static gboolean
list_archive_paths(const char *archive, GPtrArray **paths_out)
{
    const char *argv[] = {"7z", "l", "-slt", archive, NULL};
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
        g_ptr_array_unref(paths);
        return FALSE;
    }

    *paths_out = paths;
    return TRUE;
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
    g_free(mime);
    if (is_svg)
        return TRUE;

    const gsize probe = MIN(len, (gsize) 1024);
    gchar *lower = g_ascii_strdown((const gchar *) data, (gssize) probe);
    gboolean found = lower && g_strstr_len(lower, (gssize) probe, "<svg") != NULL;
    g_free(lower);
    return found;
}

static gboolean
process_svg_payload(const guchar *data, gsize len, const char *out_path, int size)
{
    GError *error = NULL;
    RsvgHandle *handle = rsvg_handle_new_from_data(data, len, &error);
    if (!handle) {
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

    return TRUE;
}

static GdkPixbuf *
scale_pixbuf(GdkPixbuf *pixbuf, int size)
{
    const int width = gdk_pixbuf_get_width(pixbuf);
    const int height = gdk_pixbuf_get_height(pixbuf);

    if (width <= 0 || height <= 0)
        return NULL;

    if (width <= size && height <= size) {
        g_object_ref(pixbuf);
        return pixbuf;
    }

    const double scale = MIN((double) size / (double) width, (double) size / (double) height);
    const int target_w = MAX(1, (int) lround(width * scale));
    const int target_h = MAX(1, (int) lround(height * scale));

    return gdk_pixbuf_scale_simple(pixbuf, target_w, target_h, GDK_INTERP_BILINEAR);
}

static gboolean
process_icon_payload(const guchar *data, gsize len, const char *out_path, int size)
{
    if (payload_is_svg(data, len)) {
        if (process_svg_payload(data, len, out_path, size))
            return TRUE;
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

    GdkPixbuf *scaled = scale_pixbuf(pixbuf, size);
    if (!scaled)
        scaled = g_object_ref(pixbuf);

    gboolean ok = gdk_pixbuf_save(scaled, out_path, "png", &error, NULL);
    if (!ok) {
        g_printerr("Failed to write thumbnail: %s\n", error->message);
        g_error_free(error);
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

    gchar *current = g_strdup(entry);
    for (int depth = 0; depth < MAX_SYMLINK_DEPTH && current != NULL; ++depth) {
        GByteArray *payload = NULL;
        if (!extract_entry(archive, current, &payload)) {
            g_free(current);
            return FALSE;
        }

        gchar *next = NULL;
        if (is_pointer_candidate(payload->data, payload->len, &next)) {
            g_byte_array_unref(payload);
            g_free(current);
            current = next;
            continue;
        }

        gboolean ok = process_icon_payload(payload->data, payload->len, out_path, size);
        g_byte_array_unref(payload);
        g_free(current);
        g_free(next);
        return ok;
    }

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
        if (dot && g_ascii_strcasecmp(dot, extension) == 0)
            return g_strdup(path);
    }
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

int
main(int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        g_printerr("Usage: %s <AppImage> <output.png> [size]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (!g_find_program_in_path("7z")) {
        g_printerr("7z is required but not found in PATH\n");
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

    gboolean success = process_entry_following_symlinks(input, ".DirIcon", output, size);

    if (!success) {
        GPtrArray *paths = NULL;
        if (list_archive_paths(input, &paths)) {
            gchar *svg_entry = find_root_with_extension(paths, ".svg");
            if (svg_entry) {
                success = process_entry_following_symlinks(input, svg_entry, output, size);
                g_free(svg_entry);
            }

            if (!success) {
                gchar *png_entry = find_root_with_extension(paths, ".png");
                if (png_entry) {
                    success = process_entry_following_symlinks(input, png_entry, output, size);
                    g_free(png_entry);
                }
            }

            g_ptr_array_unref(paths);
        }
    }

    if (!success)
        g_printerr("Failed to extract icon from AppImage\n");

    g_free(input);
    g_free(output);
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
