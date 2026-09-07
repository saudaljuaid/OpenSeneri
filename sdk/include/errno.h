/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_ERRNO_H
#define PHIPIA_ERRNO_H

#if defined(__MINGW32__) || defined(__MINGW64__)
/* MinGW's stddef.h exports errno as a CRT accessor.  The SDK owns errno as
 * thread-local storage, so reserve the CRT guard and remove that macro before
 * declaring and using the SDK symbol. */
#ifndef _CRT_ERRNO_DEFINED
#define _CRT_ERRNO_DEFINED
#endif
#ifdef errno
#undef errno
#endif
#endif

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
#define EXDEV 18
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
