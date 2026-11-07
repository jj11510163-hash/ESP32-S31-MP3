#pragma once

#include <stdbool.h>
#include <stddef.h>

bool bsp_sd_fat_path(const char *vfs_path, char *out, size_t out_size);
