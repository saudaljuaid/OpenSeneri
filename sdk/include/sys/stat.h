/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_SYS_STAT_H
#define PHIPIA_SYS_STAT_H

#include <stdint.h>
#include <time.h>
typedef uint32_t mode_t;
struct stat {
    uint64_t st_size;
    mode_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t st_nlink;
    uint64_t st_ino;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
};
#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec
#define S_IFMT  0170000U
#define S_IFREG 0100000U
#define S_IFDIR 0040000U
#define S_IFLNK 0120000U
#define S_IRUSR 0000400U
#define S_IWUSR 0000200U
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#define S_ISLNK(mode) (((mode) & S_IFMT) == S_IFLNK)
int stat(const char *path, struct stat *result);
int lstat(const char *path, struct stat *result);
int mkdir(const char *path, mode_t mode);
int chmod(const char *path, mode_t mode);
int ftruncate(int descriptor, int64_t length);
#endif
