/*
 * squashfs-extract.c - SquashFS extraction support for appimage-thumbnailer
 */

#define _XOPEN_SOURCE 700

#include "squashfs-extract.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>

#ifndef SQUASHFS_TOOLS_DIR
#define SQUASHFS_TOOLS_DIR "/usr/lib/appimage-thumbnailer"
#endif

#define SQFS_MAGIC "hsqs"
#define SQFS_MAGIC_LEN 4
#define SQFS_SUPERBLOCK_SIZE 96

/* SquashFS superblock structure (minimal fields for validation) */
struct squashfs_super_block {
    uint32_t s_magic;
    uint32_t inodes;
    uint32_t mkfs_time;
    uint32_t block_size;
    uint32_t fragments;
    uint16_t compression;
    uint16_t block_log;
    uint16_t flags;
    uint16_t no_ids;
    uint16_t s_major;
    uint16_t s_minor;
    /* ... rest not needed for validation */
} __attribute__((packed));

static gchar *unsquashfs_path = NULL;
static gboolean tools_checked = FALSE;
static gboolean tools_available_cached = FALSE;

static gchar *
find_tool(const char *name)
{
    /* Prefer bundled location */
    gchar *bundled = g_build_filename(SQUASHFS_TOOLS_DIR, name, NULL);
    if (g_file_test(bundled, G_FILE_TEST_IS_EXECUTABLE)) {
        return bundled;
    }
    g_free(bundled);

    /* Fallback to PATH for development environments */
    return g_find_program_in_path(name);
}

static void
init_tool_paths(void)
{
    if (tools_checked)
        return;

    tools_checked = TRUE;
    unsquashfs_path = find_tool("unsquashfs");
    tools_available_cached = (unsquashfs_path != NULL);
}

bool
squashfs_tools_available(void)
{
    init_tool_paths();
    return tools_available_cached;
}

static gboolean
validate_squashfs_superblock(int fd, off_t offset)
{
    struct squashfs_super_block sb;
    
    /* Use pread to avoid changing file position */
    if (pread(fd, &sb, sizeof(sb), offset) != sizeof(sb))
        return FALSE;
    
    /* Check magic */
    if (memcmp(&sb.s_magic, SQFS_MAGIC, SQFS_MAGIC_LEN) != 0)
        return FALSE;
    
    /* Basic sanity checks on superblock fields */
    /* Block size should be a power of 2 between 4KB and 1MB */
    if (sb.block_size < 4096 || sb.block_size > 1048576)
        return FALSE;
    
    /* Check if block_size is a power of 2 */
    if ((sb.block_size & (sb.block_size - 1)) != 0)
        return FALSE;
    
    /* Major version should be 4 (current SquashFS) */
    if (sb.s_major != 4)
        return FALSE;
    
    /* Compression type should be valid (0-8 are known types) */
    if (sb.compression > 8)
        return FALSE;
    
    return TRUE;
}

static off_t
find_squashfs_offset(const char *archive)
{
    int fd = open(archive, O_RDONLY);
    if (fd < 0)
        return -1;

    unsigned char buf[8192];
    unsigned char carry[SQFS_MAGIC_LEN - 1];
    size_t carry_len = 0;
    off_t offset = 0;
    ssize_t r;

    while ((r = read(fd, buf, sizeof buf)) > 0) {
        if (carry_len > 0 && carry_len + r >= SQFS_MAGIC_LEN) {
            unsigned char tmp[SQFS_MAGIC_LEN];
            memcpy(tmp, carry, carry_len);
            memcpy(tmp + carry_len, buf, SQFS_MAGIC_LEN - carry_len);
            if (memcmp(tmp, SQFS_MAGIC, SQFS_MAGIC_LEN) == 0) {
                off_t candidate = offset - (off_t) carry_len;
                if (validate_squashfs_superblock(fd, candidate)) {
                    close(fd);
                    return candidate;
                }
            }
        }

        for (ssize_t i = 0; i + SQFS_MAGIC_LEN <= r; ++i) {
            if (memcmp(buf + i, SQFS_MAGIC, SQFS_MAGIC_LEN) == 0) {
                off_t candidate = offset + (off_t) i;
                if (validate_squashfs_superblock(fd, candidate)) {
                    close(fd);
                    return candidate;
                }
            }
        }

        if (r >= (ssize_t)(SQFS_MAGIC_LEN - 1)) {
            memcpy(carry, buf + r - (SQFS_MAGIC_LEN - 1), SQFS_MAGIC_LEN - 1);
            carry_len = SQFS_MAGIC_LEN - 1;
        } else {
            memcpy(carry, buf, r);
            carry_len = (size_t) r;
        }
        offset += r;
    }

    close(fd);
    return -1;
}

static gboolean
command_capture_unsquashfs(char *const argv[], GByteArray **output)
{
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
        if (dup2(pipe_fd[1], STDOUT_FILENO) < 0)
            _exit(127);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execv(argv[0], argv);
        execvp(argv[0], argv);
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
        g_byte_array_unref(arr);
        return FALSE;
    }

    *output = arr;
    return TRUE;
}

bool
squashfs_extract_entry(const char *archive, const char *entry, GByteArray **output)
{
    if (!archive || !entry || *entry == '\0')
        return FALSE;

    init_tool_paths();
    if (!unsquashfs_path)
        return FALSE;

    off_t offset = find_squashfs_offset(archive);
    if (offset < 0)
        return FALSE;

    gchar *offset_str = g_strdup_printf("%" G_GINT64_FORMAT, (gint64) offset);

    /* Note: options precede filesystem argument, files follow. */
    char *argv[] = {
        unsquashfs_path,
        "-offset", offset_str,
        "-processors", "1",
        "-q",
        "-n",
        "-cat",
        (char *) archive,
        (char *) entry,
        NULL
    };

    gboolean ok = command_capture_unsquashfs(argv, output);
    g_free(offset_str);

    if (ok && (*output == NULL || (*output)->len == 0)) {
        if (*output) {
            g_byte_array_unref(*output);
            *output = NULL;
        }
        return FALSE;
    }

    return ok;
}

bool
squashfs_list_paths(const char *archive, GPtrArray **paths_out)
{
    if (!archive)
        return FALSE;

    init_tool_paths();
    if (!unsquashfs_path)
        return FALSE;

    off_t offset = find_squashfs_offset(archive);
    if (offset < 0)
        return FALSE;

    gchar *offset_str = g_strdup_printf("%" G_GINT64_FORMAT, (gint64) offset);

    char *argv[] = {
        unsquashfs_path,
        "-offset", offset_str,
        "-processors", "1",
        "-q",
        "-n",
        "-lls",
        (char *) archive,
        NULL
    };

    GByteArray *output = NULL;
    gboolean ok = command_capture_unsquashfs(argv, &output);
    g_free(offset_str);

    if (!ok || output == NULL)
        return FALSE;

    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    gchar **lines = g_strsplit((const gchar *) output->data, "\n", -1);
    for (gint i = 0; lines[i] != NULL; ++i) {
        gchar *line = g_strstrip(lines[i]);
        if (*line == '\0')
            continue;

        gchar **parts = g_strsplit_set(line, " \t", -1);
        guint non_empty = 0;
        GString *path_str = NULL;

        for (gint j = 0; parts[j] != NULL; ++j) {
            if (*parts[j] == '\0')
                continue;
            non_empty++;
            if (non_empty >= 6) {
                path_str = g_string_new(parts[j]);
                for (gint k = j + 1; parts[k] != NULL; ++k) {
                    if (*parts[k] == '\0')
                        continue;
                    g_string_append_c(path_str, ' ');
                    g_string_append(path_str, parts[k]);
                }
                break;
            }
        }

        g_strfreev(parts);
        if (!path_str)
            continue;

        gchar *path = path_str->str;
        const char *prefix = "squashfs-root/";
        if (g_str_has_prefix(path, prefix)) {
            path += strlen(prefix);
        } else if (strcmp(path, "squashfs-root") == 0) {
            g_string_free(path_str, TRUE);
            continue;
        }

        gchar *normalized = g_strdup(path[0] == '/' ? path + 1 : path);
        g_ptr_array_add(paths, normalized);
        g_string_free(path_str, TRUE);
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
