/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_DIRENT_H
#define PHIPIA_DIRENT_H

#include <stdint.h>
#include <phipia/abi.h>
struct dirent { char d_name[13]; uint8_t d_type; };
typedef struct { phipia_handle_t handle; struct dirent entry; } DIR;
#define DT_REG 1
#define DT_DIR 2
DIR *opendir(const char *path);
struct dirent *readdir(DIR *directory);
int closedir(DIR *directory);
#endif
