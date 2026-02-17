/*
 * dwarfs-extract.c - DwarFS extraction support for appimage-thumbnailer
 *
 * SPDX-License-Identifier: MIT
 */

#define _XOPEN_SOURCE 700

#include "dwarfs-extract.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>

/* Bundled tools directory - set at compile time */
#ifndef DWARFS_TOOLS_DIR
#define DWARFS_TOOLS_DIR "/usr/lib/appimage-thumbnailer"
#endif

static gchar *dwarfsextract_path = NULL;
static gchar *dwarfsck_path = NULL;
static gboolean tools_checked = FALSE;
static gboolean tools_available_cached = FALSE;

static gboolean
command_capture_dwarfs(const char *const argv[], GByteArray **output)
{
    if (argv && argv[0])
        g_debug("command_capture_dwarfs: running '%s'", argv[0]);

    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
        g_debug("command_capture_dwarfs: pipe() failed: %s", g_strerror(errno));
        return FALSE;
    }

    pid_t pid = fork();
    if (pid < 0) {
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
        execv(argv[0], (char *const *) argv);
        /* If execv fails, try execvp as fallback for system PATH */
        execvp(argv[0], (char *const *) argv);
        _exit(127);
    }

    g_debug("command_capture_dwarfs: forked child pid %d for '%s'", (int) pid, argv[0]);

    close(pipe_fd[1]);
    GByteArray *arr = g_byte_array_new();
    guchar buffer[8192];
    ssize_t bytes_read;

    while ((bytes_read = read(pipe_fd[0], buffer, sizeof buffer)) != 0) {
        if (bytes_read < 0) {
            if (errno == EINTR)
                continue;
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
        g_byte_array_unref(arr);
        return FALSE;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        g_debug("command_capture_dwarfs: '%s' exited with status %d (normal=%d)",
                argv[0], WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                WIFEXITED(status));
        g_byte_array_unref(arr);
        return FALSE;
    }

    g_debug("command_capture_dwarfs: '%s' succeeded, captured %u bytes", argv[0], arr->len);
    *output = arr;
    return TRUE;
}

static gchar *
get_self_dir(void)
{
    gchar *exe = g_file_read_link("/proc/self/exe", NULL);
    if (!exe)
        return NULL;
    gchar *dir = g_path_get_dirname(exe);
    g_free(exe);
    return dir;
}

static gchar *
find_tool(const char *name)
{
    g_debug("find_tool: searching for '%s'", name);

    /* First check bundled location (install prefix) */
    gchar *bundled = g_build_filename(DWARFS_TOOLS_DIR, name, NULL);
    if (g_file_test(bundled, G_FILE_TEST_IS_EXECUTABLE)) {
        g_debug("find_tool: found bundled '%s' at '%s'", name, bundled);
        return bundled;
    }
    g_debug("find_tool: bundled path '%s' not found or not executable", bundled);
    g_free(bundled);

    /* Check next to the executable (works from build dir) */
    gchar *self_dir = get_self_dir();
    if (self_dir) {
        /* Tools may sit alongside the binary (build/dwarfsextract)
         * or one level up (build/src/../dwarfsextract) */
        const gchar *relative_dirs[] = {".", "..", NULL};
        for (int i = 0; relative_dirs[i] != NULL; ++i) {
            gchar *candidate = g_build_filename(self_dir, relative_dirs[i], name, NULL);
            if (g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE)) {
                g_debug("find_tool: found '%s' adjacent to executable at '%s'", name, candidate);
                g_free(self_dir);
                return candidate;
            }
            g_free(candidate);
        }
        g_free(self_dir);
    } else {
        g_debug("find_tool: could not determine self directory for '%s'", name);
    }

    /* Fall back to system PATH */
    gchar *system_path = g_find_program_in_path(name);
    if (system_path)
        g_debug("find_tool: found '%s' in system PATH at '%s'", name, system_path);
    else
        g_debug("find_tool: '%s' not found anywhere", name);
    return system_path;
}

static void
init_tool_paths(void)
{
    if (tools_checked)
        return;

    tools_checked = TRUE;
    dwarfsextract_path = find_tool("dwarfsextract");
    dwarfsck_path = find_tool("dwarfsck");
    tools_available_cached = (dwarfsextract_path != NULL && dwarfsck_path != NULL);

    g_debug("init_tool_paths: dwarfsextract='%s'", dwarfsextract_path ? dwarfsextract_path : "(not found)");
    g_debug("init_tool_paths: dwarfsck='%s'", dwarfsck_path ? dwarfsck_path : "(not found)");
    g_debug("init_tool_paths: dwarfs tools available: %s", tools_available_cached ? "yes" : "no");
}

gboolean
dwarfs_tools_available(void)
{
    init_tool_paths();
    return tools_available_cached;
}

gboolean
dwarfs_extract_entry(const char *archive, const char *entry, GByteArray **output)
{
    g_debug("dwarfs_extract_entry: attempting to extract '%s' from '%s'",
            entry ? entry : "(null)", archive ? archive : "(null)");

    if (!archive || !entry || *entry == '\0')
        return FALSE;

    init_tool_paths();
    if (!dwarfsextract_path) {
        g_debug("dwarfs_extract_entry: dwarfsextract not available");
        return FALSE;
    }

    gchar *clean_entry = NULL;
    if (entry[0] == '/')
        clean_entry = g_strdup(entry + 1);
    else
        clean_entry = g_strdup(entry);

    /* Build the pattern for dwarfsextract */
    gchar *pattern = g_strdup(clean_entry);

    /* Extract to a temp directory and read from there */
    gchar *tmpdir = g_dir_make_tmp("appimage-thumb-XXXXXX", NULL);
    if (!tmpdir) {
        g_debug("dwarfs_extract_entry: failed to create temp directory");
        g_free(clean_entry);
        g_free(pattern);
        return FALSE;
    }

    g_debug("dwarfs_extract_entry: extracting '%s' to tmpdir '%s'", clean_entry, tmpdir);

    const char *argv[] = {
        dwarfsextract_path,
        "-i", archive,
        "-O", "auto",
        "--pattern", pattern,
        "-o", tmpdir,
        "--log-level=error",
        NULL
    };

    GByteArray *dummy = NULL;
    gboolean extract_ok = command_capture_dwarfs(argv, &dummy);
    if (dummy)
        g_byte_array_unref(dummy);

    gboolean result = FALSE;
    if (extract_ok) {
        gchar *extracted_path = g_build_filename(tmpdir, clean_entry, NULL);
        gchar *contents = NULL;
        gsize length = 0;
        GError *error = NULL;

        g_debug("dwarfs_extract_entry: checking extracted file at '%s'", extracted_path);

        /* Check if the extracted file is a symlink.
         * For symlinks, return the link target as the content (this is the "pointer"
         * that process_entry_following_symlinks will follow) */
        if (g_file_test(extracted_path, G_FILE_TEST_IS_SYMLINK)) {
            contents = g_file_read_link(extracted_path, &error);
            if (contents) {
                g_debug("dwarfs_extract_entry: '%s' is a symlink pointing to '%s'", clean_entry, contents);
                length = strlen(contents);
                *output = g_byte_array_new();
                g_byte_array_append(*output, (const guchar *) contents, (guint) length);
                g_free(contents);
                result = TRUE;
            } else {
                g_debug("dwarfs_extract_entry: failed to read symlink '%s': %s",
                        extracted_path, error ? error->message : "unknown");
                if (error)
                    g_error_free(error);
            }
        } else if (g_file_get_contents(extracted_path, &contents, &length, &error)) {
            g_debug("dwarfs_extract_entry: read %" G_GSIZE_FORMAT " bytes from '%s'", length, clean_entry);
            *output = g_byte_array_new();
            g_byte_array_append(*output, (const guchar *) contents, (guint) length);
            g_free(contents);
            result = TRUE;
        } else {
            g_debug("dwarfs_extract_entry: failed to read extracted file '%s': %s",
                    extracted_path, error ? error->message : "unknown");
            if (error)
                g_error_free(error);
        }

        g_free(extracted_path);
    }

    /* Clean up temp directory recursively */
    gchar *cleanup_path = g_build_filename(tmpdir, clean_entry, NULL);
    
    /* Handle nested paths - need to clean up parent directories */
    gchar *parent = g_path_get_dirname(cleanup_path);
    g_unlink(cleanup_path);
    
    /* Remove parent directories up to tmpdir */
    while (parent && g_strcmp0(parent, tmpdir) != 0 && g_strcmp0(parent, ".") != 0) {
        g_rmdir(parent);
        gchar *grandparent = g_path_get_dirname(parent);
        g_free(parent);
        parent = grandparent;
    }
    g_free(parent);
    
    g_free(cleanup_path);
    g_rmdir(tmpdir);
    g_free(tmpdir);

    g_free(clean_entry);
    g_free(pattern);
    return result;
}

gboolean
dwarfs_list_paths(const char *archive, GPtrArray **paths_out)
{
    g_debug("dwarfs_list_paths: listing entries in '%s'", archive);

    init_tool_paths();
    if (!dwarfsck_path) {
        g_debug("dwarfs_list_paths: dwarfsck not available");
        return FALSE;
    }

    /* Use -v (verbose) to get tar-style output with permissions, user/group, size, date, time, path */
    const char *argv[] = {dwarfsck_path, "-l", "-v", "--no-check", "-O", "auto", archive, NULL};
    GByteArray *output = NULL;
    if (!command_capture_dwarfs(argv, &output)) {
        g_debug("dwarfs_list_paths: dwarfsck command failed for '%s'", archive);
        return FALSE;
    }

    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    gchar **lines = g_strsplit((const gchar *) output->data, "\n", -1);

    /* dwarfsck -l output is like tar -tv format:
     * -rw-r--r--   1000/1000       1234 2024-01-01 00:00 path/to/file
     * We need to extract the path from each line */
    for (gint i = 0; lines[i] != NULL; ++i) {
        gchar *line = lines[i];
        if (*line == '\0')
            continue;

        /* Skip permission, user/group, size, date, time and get to the path
         * Format: perms user/group size date time path */
        gchar **parts = g_strsplit(line, " ", -1);
        guint non_empty = 0;
        gchar *path = NULL;

        /* Count non-empty parts to find the path (6th non-empty field) */
        for (guint j = 0; parts[j] != NULL; ++j) {
            if (*parts[j] != '\0') {
                non_empty++;
                if (non_empty >= 6) {
                    /* Everything from here is the path (may contain spaces) */
                    GString *path_str = g_string_new(parts[j]);
                    for (guint k = j + 1; parts[k] != NULL; ++k) {
                        g_string_append_c(path_str, ' ');
                        g_string_append(path_str, parts[k]);
                    }
                    path = g_string_free(path_str, FALSE);
                    break;
                }
            }
        }
        g_strfreev(parts);

        if (path && *path != '\0') {
            /* Handle symlinks: strip " -> target" suffix from path
             * e.g., ".DirIcon -> icon.svg" becomes ".DirIcon" */
            gchar *arrow = g_strstr_len(path, -1, " -> ");
            if (arrow)
                *arrow = '\0';

            /* Normalize path - remove leading slash if present */
            gchar *normalized = g_strdup(path[0] == '/' ? path + 1 : path);
            /* Remove trailing newline/whitespace */
            g_strchomp(normalized);
            if (*normalized != '\0')
                g_ptr_array_add(paths, normalized);
            else
                g_free(normalized);
            g_free(path);
        } else {
            g_free(path);
        }
    }
    g_strfreev(lines);
    g_byte_array_unref(output);

    if (paths->len == 0) {
        g_debug("dwarfs_list_paths: no paths found in archive '%s'", archive);
        g_ptr_array_unref(paths);
        return FALSE;
    }

    g_debug("dwarfs_list_paths: found %u entries in archive '%s'", paths->len, archive);
    *paths_out = paths;
    return TRUE;
}
