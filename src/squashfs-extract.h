#pragma once

#include <glib.h>
#include <stdbool.h>

bool squashfs_tools_available(void);
bool squashfs_extract_entry(const char *archive, const char *entry, GByteArray **output);
bool squashfs_list_paths(const char *archive, GPtrArray **paths_out);
