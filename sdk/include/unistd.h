/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_UNISTD_H
#define PHIPIA_UNISTD_H

#include <stddef.h>
#include <stdint.h>

typedef long ssize_t;
typedef int64_t off_t;

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define R_OK 4
#define W_OK 2
#define F_OK 0

ssize_t read(int descriptor, void *buffer, size_t length);
ssize_t write(int descriptor, const void *buffer, size_t length);
off_t lseek(int descriptor, off_t offset, int origin);
int close(int descriptor);
int access(const char *path, int mode);
int unlink(const char *path);
int rmdir(const char *path);
int fsync(int descriptor);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int microseconds);
int getpid(void);

#endif
