/*
 * squashfs-extract.c - SquashFS extraction support for appimage-thumbnailer
 *
 * Uses unsquashfs (from squashfs-tools) to extract files from SquashFS-based
 * AppImages.  The -o (offset) flag is used to access the embedded SquashFS
 * without carving it to a temporary file first.
 *
 * SPDX-License-Identifier: MIT
 */

#define _XOPEN_SOURCE 700

#include "squashfs-extract.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>

/* Bundled tools directory - set at compile time */
#ifndef SQUASHFS_TOOLS_DIR
#define SQUASHFS_TOOLS_DIR "/usr/lib/appimage-thumbnailer"
#endif

static gchar *unsquashfs_path = NULL;
static gboolean tool_checked = FALSE;
static gboolean tool_available_cached = FALSE;

/* ------------------------------------------------------------------ */
/*  Helper: run a command and wait, discarding output                 */
/* ------------------------------------------------------------------ */

static gboolean
command_run_squashfs(const char *const argv[])
{
    if (argv && argv[0])
        g_debug("command_run_squashfs: running '%s'", argv[0]);

    pid_t pid = fork();
    if (pid < 0)
        return FALSE;

    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execv(argv[0], (char *const *)argv);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return FALSE;

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        g_debug("command_run_squashfs: '%s' exited with status %d",
                argv[0], WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return FALSE;
    }

    g_debug("command_run_squashfs: '%s' succeeded", argv[0]);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Tool discovery                                                     */
/* ------------------------------------------------------------------ */

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
find_unsquashfs(void)
{
    g_debug("find_unsquashfs: searching for unsquashfs");

    /* 1. Check bundled location (install prefix) */
    gchar *bundled = g_build_filename(SQUASHFS_TOOLS_DIR, "unsquashfs", NULL);
    if (g_file_test(bundled, G_FILE_TEST_IS_EXECUTABLE)) {
        g_debug("find_unsquashfs: found bundled at '%s'", bundled);
        return bundled;
    }
    g_debug("find_unsquashfs: bundled path '%s' not found", bundled);
    g_free(bundled);

    /* 2. Check next to the executable (build directory) */
    gchar *self_dir = get_self_dir();
    if (self_dir) {
        const gchar *relative_dirs[] = {".", "..", NULL};
        for (int i = 0; relative_dirs[i] != NULL; ++i) {
            gchar *candidate = g_build_filename(self_dir, relative_dirs[i], "unsquashfs", NULL);
            if (g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE)) {
                g_debug("find_unsquashfs: found adjacent '%s'", candidate);
                g_free(self_dir);
                return candidate;
            }
            g_free(candidate);
        }
        g_free(self_dir);
    }

    /* 3. System PATH */
    gchar *system_path = g_find_program_in_path("unsquashfs");
    if (system_path)
        g_debug("find_unsquashfs: found in PATH at '%s'", system_path);
    else
        g_debug("find_unsquashfs: not found anywhere");
    return system_path;
}

static void
init_tool(void)
{
    if (tool_checked)
        return;

    tool_checked = TRUE;
    unsquashfs_path = find_unsquashfs();
    tool_available_cached = (unsquashfs_path != NULL);

    g_debug("init_tool: unsquashfs='%s', available=%s",
            unsquashfs_path ? unsquashfs_path : "(not found)",
            tool_available_cached ? "yes" : "no");
}

gboolean
squashfs_tools_available(void)
{
    init_tool();
    return tool_available_cached;
}

/* ------------------------------------------------------------------ */
/*  Recursive cleanup helper                                          */
/* ------------------------------------------------------------------ */

static void
remove_directory_recursive(const gchar *dir_path)
{
    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (!dir) {
        g_unlink(dir_path);
        return;
    }

    const gchar *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        gchar *child = g_build_filename(dir_path, name, NULL);
        if (g_file_test(child, G_FILE_TEST_IS_DIR) &&
            !g_file_test(child, G_FILE_TEST_IS_SYMLINK)) {
            remove_directory_recursive(child);
        } else {
            g_unlink(child);
        }
        g_free(child);
    }
    g_dir_close(dir);
    g_rmdir(dir_path);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

gboolean
squashfs_extract_entry(const char *archive, const char *entry,
                       off_t offset, GByteArray **output)
{
    g_debug("squashfs_extract_entry: extracting '%s' from '%s' at offset %" G_GINT64_FORMAT,
            entry ? entry : "(null)", archive ? archive : "(null)", (gint64)offset);

    if (!archive || !entry || *entry == '\0' || offset <= 0)
        return FALSE;

    init_tool();
    if (!unsquashfs_path) {
        g_debug("squashfs_extract_entry: unsquashfs not available");
        return FALSE;
    }

    gchar *clean_entry = NULL;
    if (entry[0] == '/')
        clean_entry = g_strdup(entry + 1);
    else
        clean_entry = g_strdup(entry);

    /* Create a temporary directory for extraction */
    gchar *tmpdir = g_dir_make_tmp("appimage-sqfs-XXXXXX", NULL);
    if (!tmpdir) {
        g_debug("squashfs_extract_entry: failed to create temp directory");
        g_free(clean_entry);
        return FALSE;
    }

    /* unsquashfs wants to create (-d) a new directory; use a subdir */
    gchar *extract_dir = g_build_filename(tmpdir, "root", NULL);

    /* Format offset as string */
    gchar *offset_str = g_strdup_printf("%" G_GINT64_FORMAT, (gint64)offset);

    g_debug("squashfs_extract_entry: running unsquashfs -o %s -d '%s' '%s' '%s'",
            offset_str, extract_dir, archive, clean_entry);

    const char *argv[] = {
        unsquashfs_path,
        "-o", offset_str,
        "-no-progress",
        "-d", extract_dir,
        archive,
        clean_entry,
        NULL
    };

    gboolean extract_ok = command_run_squashfs(argv);
    gboolean result = FALSE;

    if (extract_ok) {
        gchar *extracted_path = g_build_filename(extract_dir, clean_entry, NULL);

        g_debug("squashfs_extract_entry: checking extracted file at '%s'", extracted_path);

        /* If the entry is a symlink, return its target as content.
         * The caller (process_entry_following_symlinks) will follow it. */
        if (g_file_test(extracted_path, G_FILE_TEST_IS_SYMLINK)) {
            gchar *link_target = g_file_read_link(extracted_path, NULL);
            if (link_target) {
                g_debug("squashfs_extract_entry: '%s' is a symlink -> '%s'",
                        clean_entry, link_target);
                gsize len = strlen(link_target);
                *output = g_byte_array_new();
                g_byte_array_append(*output, (const guchar *)link_target, (guint)len);
                g_free(link_target);
                result = TRUE;
            }
        } else if (g_file_test(extracted_path, G_FILE_TEST_EXISTS)) {
            gchar *contents = NULL;
            gsize length = 0;
            GError *error = NULL;

            if (g_file_get_contents(extracted_path, &contents, &length, &error)) {
                g_debug("squashfs_extract_entry: read %" G_GSIZE_FORMAT " bytes from '%s'",
                        length, clean_entry);
                *output = g_byte_array_new();
                g_byte_array_append(*output, (const guchar *)contents, (guint)length);
                g_free(contents);
                result = TRUE;
            } else {
                g_debug("squashfs_extract_entry: failed to read '%s': %s",
                        extracted_path, error ? error->message : "unknown");
                if (error)
                    g_error_free(error);
            }
        } else {
            g_debug("squashfs_extract_entry: extracted file not found at '%s'", extracted_path);
        }

        g_free(extracted_path);
    } else {
        g_debug("squashfs_extract_entry: unsquashfs command failed");
    }

    /* Clean up temp directory */
    remove_directory_recursive(tmpdir);

    g_free(offset_str);
    g_free(extract_dir);
    g_free(tmpdir);
    g_free(clean_entry);
    return result;
}


