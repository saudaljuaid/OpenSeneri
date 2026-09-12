/* SPDX-License-Identifier: GPL-3.0-only */
#include <phipia/zlib.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAYLOAD_BYTES 4096U
#define COMPRESSED_BYTES 8192U

struct allocation_state {
    size_t allocations;
    size_t frees;
    size_t limit;
};

static voidpf checked_allocate(voidpf opaque, uInt items, uInt size)
{
    struct allocation_state *state = opaque;
    size_t bytes;

    if (state == NULL || (size != 0U && items > SIZE_MAX / size) ||
        state->allocations >= state->limit) {
        return Z_NULL;
    }
    bytes = (size_t)items * size;
    if (bytes == 0U) {
        bytes = 1U;
    }
    void *const result = calloc(1U, bytes);
    if (result != NULL) {
        ++state->allocations;
    }
    return result;
}

static void checked_free(voidpf opaque, voidpf address)
{
    struct allocation_state *state = opaque;

    if (address != Z_NULL) {
        if (state != NULL) {
            ++state->frees;
        }
        free(address);
    }
}

static void initialize_stream(z_stream *stream, struct allocation_state *state)
{
    (void)memset(stream, 0, sizeof(*stream));
    *state = (struct allocation_state){0U, 0U, SIZE_MAX};
    stream->zalloc = checked_allocate;
    stream->zfree = checked_free;
    stream->opaque = state;
}

static int allocations_released(const struct allocation_state *state)
{
    return state->allocations == state->frees;
}

static int compress_payload(const uint8_t *input, size_t input_length,
    uint8_t *output, size_t output_capacity, size_t *output_length)
{
    struct allocation_state state;
    z_stream stream;
    int result;
    int end_result;

    if (input_length > UINT_MAX || output_capacity > UINT_MAX) {
        return 0;
    }
    initialize_stream(&stream, &state);
    result = deflateInit(&stream, Z_BEST_COMPRESSION);
    if (result != Z_OK) {
        return 0;
    }
    stream.next_in = (Bytef *)(uintptr_t)input;
    stream.avail_in = (uInt)input_length;
    stream.next_out = output;
    stream.avail_out = (uInt)output_capacity;
    result = deflate(&stream, Z_FINISH);
    *output_length = (size_t)stream.total_out;
    end_result = deflateEnd(&stream);
    return result == Z_STREAM_END && end_result == Z_OK &&
        stream.avail_in == 0U && allocations_released(&state);
}

static int inflate_payload(const uint8_t *input, size_t input_length,
    uint8_t *output, size_t output_capacity, size_t *output_length,
    int *stream_result)
{
    struct allocation_state state;
    z_stream stream;
    int result;
    int end_result;

    if (input_length > UINT_MAX || output_capacity > UINT_MAX) {
        return 0;
    }
    initialize_stream(&stream, &state);
    result = inflateInit(&stream);
    if (result != Z_OK) {
        return 0;
    }
    stream.next_in = (Bytef *)(uintptr_t)input;
    stream.avail_in = (uInt)input_length;
    stream.next_out = output;
    stream.avail_out = (uInt)output_capacity;
    result = inflate(&stream, Z_FINISH);
    *stream_result = result;
    *output_length = (size_t)stream.total_out;
    end_result = inflateEnd(&stream);
    return end_result == Z_OK && allocations_released(&state);
}

static void make_payload(uint8_t payload[PAYLOAD_BYTES])
{
    static const char phrase[] = "Phipia reproducible zlib stream\n";

    for (size_t index = 0U; index < PAYLOAD_BYTES; ++index) {
        payload[index] = (uint8_t)phrase[index % (sizeof(phrase) - 1U)];
    }
    for (size_t index = 113U; index < PAYLOAD_BYTES; index += 257U) {
        payload[index] ^= (uint8_t)(index & 0xffU);
    }
}

static int allocation_failure_is_clean(void)
{
    struct allocation_state state;
    z_stream stream;
    int result;

    initialize_stream(&stream, &state);
    state.limit = 0U;
    result = deflateInit(&stream, Z_DEFAULT_COMPRESSION);
    return result == Z_MEM_ERROR && allocations_released(&state);
}

static int sdk_allocator_round_trip(void)
{
    static const uint8_t source[] = "Phipia SDK zlib allocator";
    uint8_t compressed[128];
    uint8_t decoded[sizeof(source)];
    z_stream encode;
    z_stream decode;
    size_t compressed_length;
    int result;

    if (phipia_zlib_stream_prepare(NULL) != -1 ||
        phipia_zlib_stream_prepare(&encode) != 0 ||
        encode.zalloc == Z_NULL || encode.zfree == Z_NULL) {
        return 0;
    }
    result = deflateInit(&encode, Z_DEFAULT_COMPRESSION);
    if (result != Z_OK) {
        return 0;
    }
    encode.next_in = (Bytef *)(uintptr_t)source;
    encode.avail_in = (uInt)sizeof(source);
    encode.next_out = compressed;
    encode.avail_out = (uInt)sizeof(compressed);
    result = deflate(&encode, Z_FINISH);
    compressed_length = (size_t)encode.total_out;
    if (deflateEnd(&encode) != Z_OK || result != Z_STREAM_END ||
        compressed_length == 0U ||
        phipia_zlib_stream_prepare(&decode) != 0) {
        return 0;
    }
    result = inflateInit(&decode);
    if (result != Z_OK) {
        return 0;
    }
    decode.next_in = compressed;
    decode.avail_in = (uInt)compressed_length;
    decode.next_out = decoded;
    decode.avail_out = (uInt)sizeof(decoded);
    result = inflate(&decode, Z_FINISH);
    return inflateEnd(&decode) == Z_OK && result == Z_STREAM_END &&
        decode.total_out == sizeof(source) &&
        memcmp(decoded, source, sizeof(source)) == 0;
}

int main(void)
{
    uint8_t payload[PAYLOAD_BYTES];
    uint8_t compressed_a[COMPRESSED_BYTES];
    uint8_t compressed_b[COMPRESSED_BYTES];
    uint8_t decoded[PAYLOAD_BYTES];
    uint8_t corrupted[COMPRESSED_BYTES];
    size_t compressed_a_length = 0U;
    size_t compressed_b_length = 0U;
    size_t decoded_length = 0U;
    int stream_result = Z_OK;

    make_payload(payload);
    if (!compress_payload(payload, sizeof(payload), compressed_a,
            sizeof(compressed_a), &compressed_a_length) ||
        !compress_payload(payload, sizeof(payload), compressed_b,
            sizeof(compressed_b), &compressed_b_length) ||
        compressed_a_length != compressed_b_length ||
        memcmp(compressed_a, compressed_b, compressed_a_length) != 0) {
        fputs("zlib host test: deterministic deflate failed\n", stderr);
        return 1;
    }
    if (!inflate_payload(compressed_a, compressed_a_length, decoded,
            sizeof(decoded), &decoded_length, &stream_result) ||
        stream_result != Z_STREAM_END || decoded_length != sizeof(payload) ||
        memcmp(decoded, payload, sizeof(payload)) != 0) {
        fputs("zlib host test: inflate round trip failed\n", stderr);
        return 1;
    }
    (void)memcpy(corrupted, compressed_a, compressed_a_length);
    corrupted[compressed_a_length / 2U] ^= UINT8_C(0x40);
    if (!inflate_payload(corrupted, compressed_a_length, decoded,
            sizeof(decoded), &decoded_length, &stream_result) ||
        stream_result == Z_STREAM_END) {
        fputs("zlib host test: corrupt stream was accepted\n", stderr);
        return 1;
    }
    if (!inflate_payload(compressed_a, compressed_a_length - 1U, decoded,
            sizeof(decoded), &decoded_length, &stream_result) ||
        stream_result == Z_STREAM_END) {
        fputs("zlib host test: truncated stream was accepted\n", stderr);
        return 1;
    }
    if (!allocation_failure_is_clean()) {
        fputs("zlib host test: allocation failure leaked or misreported\n",
            stderr);
        return 1;
    }
    if (!sdk_allocator_round_trip()) {
        fputs("zlib host test: SDK allocator adapter failed\n", stderr);
        return 1;
    }
    printf("zlib host test: %zu-byte deterministic stream, round trip, "
        "corruption, truncation, allocator cleanup, SDK adapter\n",
        compressed_a_length);
    return 0;
}
