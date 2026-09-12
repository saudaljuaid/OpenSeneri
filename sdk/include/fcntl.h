/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_FCNTL_H
#define PHIPIA_FCNTL_H
#define O_RDONLY 0x0001
#define O_WRONLY 0x0002
#define O_RDWR 0x0003
#define O_CREAT 0x0100
#define O_TRUNC 0x0200
#define O_APPEND 0x0400
int open(const char *path, int flags, ...);
#endif
