/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_ERRNO_H
#define PHIPIA_ERRNO_H

extern _Thread_local int errno;

#define EPERM 1
#define ENOENT 2
#define EIO 5
#define EBADF 9
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENOSPC 28
#define EROFS 30
#define EMFILE 24
#define EPIPE 32
#define ERANGE 34
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ENOTEMPTY 39
#define ELOOP 40
#define ENOTSUP 95
#define ETIMEDOUT 110
#define ECANCELED 125
#define ESTALE 127

#endif
