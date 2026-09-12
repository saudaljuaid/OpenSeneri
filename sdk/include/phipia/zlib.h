/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_SDK_ZLIB_H
#define PHIPIA_SDK_ZLIB_H

#include <zlib.h>

/*
 * Prepare a zeroed Z_SOLO stream with Phipia's checked calloc/free adapter.
 * The caller still owns the matching deflateEnd/inflateEnd on every path.
 */
int phipia_zlib_stream_prepare(z_streamp stream);

#endif
