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
#define MAX_DIRICON_CHAIN 8
#define POINTER_TEXT_LIMIT 4096

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
        // Child
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
    if (entry == NULL || *entry == '\0')
        return FALSE;

    gchar *clean_entry = NULL;
    if (entry[0] == '/') {
        clean_entry = g_strdup(entry + 1);
    } else {
        clean_entry = g_strdup(entry);
    }

    if (clean_entry[0] == '.' && clean_entry[1] == '/' && clean_entry[2] == '\0') {
        g_printerr("Invalid archive entry %s\n", entry);
        g_free(clean_entry);
        return FALSE;
    }

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
                gchar *normalized = g_strdup(path);
                if (normalized[0] == '/') {
                    size_t len = strlen(normalized + 1) + 1;
                    memmove(normalized, normalized + 1, len);
                }
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
    if (len == 0 || len > POINTER_TEXT_LIMIT)
        return FALSE;

    for (gsize i = 0; i < len; ++i) {
        if (data[i] == '\0')
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
        if (g_ascii_isspace(*c))
            break;
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
process_icon_payload(const guchar *data, gsize len, const char *out_path, int size, gchar **pointer_out)
{
    if (pointer_out)
        *pointer_out = NULL;

    if (is_pointer_candidate(data, len, pointer_out))
        return FALSE;

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
try_process_entry_once(const char *archive, const char *entry, const char *out_path, int size)
{
    GByteArray *payload = NULL;
    if (!extract_entry(archive, entry, &payload))
        return FALSE;

    gboolean ok = process_icon_payload(payload->data, payload->len, out_path, size, NULL);
    g_byte_array_unref(payload);
    return ok;
}

static gboolean
try_process_entry_variants(const char *archive, const char *entry, const char *out_path, int size)
{
    if (!entry || *entry == '\0')
        return FALSE;

    gchar *normalized = g_strdup(entry);
    gchar *trimmed = g_strstrip(normalized);

    const gchar *candidates[4] = {trimmed, NULL, NULL, NULL};
    if (*trimmed == '/')
        candidates[1] = trimmed + 1;
    else
        candidates[1] = trimmed;

    if (!g_str_has_prefix(candidates[1], "./"))
        candidates[2] = g_strconcat("./", candidates[1], NULL);

    gboolean success = FALSE;
    for (int i = 0; i < 3 && candidates[i] != NULL; ++i) {
        if (try_process_entry_once(archive, candidates[i], out_path, size)) {
            success = TRUE;
            break;
        }
    }

    if (candidates[2])
        g_free((gpointer) candidates[2]);
    g_free(normalized);
    return success;
}

static gboolean
try_dir_icon(const char *archive, const char *out_path, int size)
{
    gchar *current = g_strdup(".DirIcon");

    for (int depth = 0; depth < MAX_DIRICON_CHAIN && current != NULL; ++depth) {
        GByteArray *payload = NULL;
        if (!extract_entry(archive, current, &payload)) {
            g_free(current);
            return FALSE;
        }

        gchar *next = NULL;
        gboolean ok = process_icon_payload(payload->data, payload->len, out_path, size, &next);
        g_byte_array_unref(payload);

        if (ok) {
            g_free(current);
            g_free(next);
            return TRUE;
        }

        g_free(current);
        current = next;
    }

    g_printerr("Failed to resolve .DirIcon within %d hops\n", MAX_DIRICON_CHAIN);
    g_free(current);
    return FALSE;
}

static gchar *
find_first_desktop_entry(GPtrArray *paths)
{
    for (guint i = 0; i < paths->len; ++i) {
        const gchar *path = paths->pdata[i];
        const gchar *suffix = g_strrstr(path, ".desktop");
        if (suffix && g_ascii_strcasecmp(suffix, ".desktop") == 0)
            return g_strdup(path);
    }
    return NULL;
}

static gchar *
extract_icon_key(GByteArray *desktop_payload)
{
    gchar **lines = g_strsplit((const gchar *) desktop_payload->data, "\n", -1);
    gchar *icon = NULL;

    for (gint i = 0; lines[i] != NULL && icon == NULL; ++i) {
        gchar *line = g_strstrip(lines[i]);
        if (*line == '#' || *line == '\0')
            continue;
        if (g_ascii_strncasecmp(line, "Icon=", 5) == 0) {
            icon = g_strdup(line + 5);
        }
    }

    g_strfreev(lines);
    return icon;
}

static gchar *
icon_basename(const gchar *icon_name)
{
    gchar *copy = g_strdup(icon_name);
    gchar *trimmed = g_strstrip(copy);
    while (*trimmed == '/')
        trimmed++;
    gchar *result = g_strdup(trimmed);
    g_free(copy);
    return result;
}

static gchar *
remove_extension(const gchar *name)
{
    gchar *copy = g_strdup(name);
    gchar *dot = strrchr(copy, '.');
    if (dot)
        *dot = '\0';
    return copy;
}

static gboolean
entry_basename_matches(const gchar *entry, const gchar *needle, const gchar *ext)
{
    if (!g_str_has_suffix(entry, ext))
        return FALSE;

    gchar *basename = g_path_get_basename(entry);
    const gsize ext_len = strlen(ext);
    const gsize base_len = strlen(basename);
    if (base_len <= ext_len) {
        g_free(basename);
        return FALSE;
    }

    basename[base_len - ext_len] = '\0';
    gboolean match = g_ascii_strcasecmp(basename, needle) == 0;
    g_free(basename);
    return match;
}

static gboolean
search_icon_roots(const char *archive,
                  GPtrArray *paths,
                  const gchar *icon_root,
                  const char *out_path,
                  int size)
{
    static const gchar *extensions[] = {".svg", ".png", ".xpm"};
    static const gchar *roots[] = {
        ".local/share/icons",
        "usr/share/icons",
        "usr/share/pixmaps"
    };

    gchar *base_only = g_path_get_basename(icon_root);
    gchar *needle = remove_extension(base_only);
    g_free(base_only);

    for (guint r = 0; r < G_N_ELEMENTS(roots); ++r) {
        for (guint p = 0; p < paths->len; ++p) {
            const gchar *entry = paths->pdata[p];
            if (!g_str_has_prefix(entry, roots[r]))
                continue;
            for (guint e = 0; e < G_N_ELEMENTS(extensions); ++e) {
                if (entry_basename_matches(entry, needle, extensions[e])) {
                    if (try_process_entry_variants(archive, entry, out_path, size)) {
                        g_free(needle);
                        return TRUE;
                    }
                }
            }
        }
    }

    g_free(needle);
    return FALSE;
}

static gboolean
try_icon_from_listing(const char *archive,
                      GPtrArray *paths,
                      const gchar *icon_name,
                      const char *out_path,
                      int size)
{
    static const gchar *extensions[] = {".svg", ".png", ".xpm"};

    gchar *normalized = icon_basename(icon_name);

    if (try_process_entry_variants(archive, normalized, out_path, size)) {
        g_free(normalized);
        return TRUE;
    }

    gchar *without_ext = remove_extension(normalized);

    for (guint i = 0; i < G_N_ELEMENTS(extensions); ++i) {
        gchar *candidate = g_strconcat(without_ext, extensions[i], NULL);
        gboolean ok = try_process_entry_variants(archive, candidate, out_path, size);
        g_free(candidate);
        if (ok) {
            g_free(without_ext);
            g_free(normalized);
            return TRUE;
        }
    }

    gboolean found = search_icon_roots(archive, paths, normalized, out_path, size);

    g_free(without_ext);
    g_free(normalized);
    return found;
}

static gboolean
try_desktop_fallback(const char *archive, const char *out_path, int size)
{
    GPtrArray *paths = NULL;
    if (!list_archive_paths(archive, &paths))
        return FALSE;

    gchar *desktop_entry = find_first_desktop_entry(paths);
    if (!desktop_entry) {
        g_ptr_array_unref(paths);
        return FALSE;
    }

    GByteArray *desktop_payload = NULL;
    gboolean ok = extract_entry(archive, desktop_entry, &desktop_payload);
    if (!ok) {
        g_free(desktop_entry);
        g_ptr_array_unref(paths);
        return FALSE;
    }

    gchar *icon_key = extract_icon_key(desktop_payload);
    g_byte_array_unref(desktop_payload);

    if (!icon_key) {
        g_free(desktop_entry);
        g_ptr_array_unref(paths);
        return FALSE;
    }

    gboolean result = try_icon_from_listing(archive, paths, icon_key, out_path, size);

    g_free(icon_key);
    g_free(desktop_entry);
    g_ptr_array_unref(paths);
    return result;
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

    gboolean success = FALSE;

    if (try_dir_icon(input, output, size)) {
        success = TRUE;
    } else if (try_desktop_fallback(input, output, size)) {
        success = TRUE;
    }

    if (!success) {
        g_printerr("Failed to extract icon from AppImage\n");
        g_free(input);
        g_free(output);
        return EXIT_FAILURE;
    }

    g_free(input);
    g_free(output);
    return EXIT_SUCCESS;
}
