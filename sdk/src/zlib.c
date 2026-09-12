/* SPDX-License-Identifier: GPL-3.0-only */
#include <phipia/zlib.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static voidpf phipia_zlib_allocate(voidpf opaque, uInt items, uInt size)
{
    size_t bytes;

    (void)opaque;
    if (size != 0U && (size_t)items > SIZE_MAX / (size_t)size) {
        return Z_NULL;
    }
    bytes = (size_t)items * (size_t)size;
    return calloc(bytes == 0U ? 1U : bytes, 1U);
}

static void phipia_zlib_free(voidpf opaque, voidpf address)
{
    (void)opaque;
    free(address);
}

int phipia_zlib_stream_prepare(z_streamp stream)
{
    if (stream == Z_NULL) {
        return -1;
    }
    (void)memset(stream, 0, sizeof(*stream));
    stream->zalloc = phipia_zlib_allocate;
    stream->zfree = phipia_zlib_free;
    return 0;
}
