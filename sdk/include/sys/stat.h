/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_SYS_STAT_H
#define PHIPIA_SYS_STAT_H

#include <stdint.h>
typedef uint32_t mode_t;
struct stat { uint64_t st_size; mode_t st_mode; };
#define S_IFREG 0100000U
#define S_IFDIR 0040000U
#define S_IRUSR 0000400U
#define S_IWUSR 0000200U
#define S_ISREG(mode) (((mode) & S_IFREG) != 0U)
#define S_ISDIR(mode) (((mode) & S_IFDIR) != 0U)
int stat(const char *path, struct stat *result);
int mkdir(const char *path, mode_t mode);
int ftruncate(int descriptor, int64_t length);
#endif
