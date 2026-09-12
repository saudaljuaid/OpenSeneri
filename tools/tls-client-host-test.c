/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include <phipia/tls.h>

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <phipia/network.h>
#include <phipia/runtime.h>

#define TEST_DEADLINE_NS UINT64_C(3000000000)
#define TEST_DN_BYTES 256U
#define TEST_RSA_BYTES 512U

/* The host adapter intentionally uses the kernel ABI declarations from
 * include/ while host libc remains ahead of the freestanding SDK headers.
 * Declare the SDK transport entry points that this translation unit mocks. */
long phipia_dns_resolve(const char *hostname, uint64_t deadline_ns);
long phipia_stream_open(void);
long phipia_stream_connect(phipia_handle_t stream,
    const struct phipia_ipv4_endpoint *endpoint, uint64_t deadline_ns);
long phipia_stream_read(phipia_handle_t stream, void *buffer, size_t length,
    uint64_t deadline_ns);
long phipia_stream_write(phipia_handle_t stream, const void *buffer,
    size_t length, uint64_t deadline_ns);
long phipia_stream_shutdown(phipia_handle_t stream, uint32_t flags,
    uint64_t deadline_ns);
long phipia_network_cancel(phipia_handle_t handle);

static uint16_t peer_port;

static uint64_t host_now_ns(void)
{
    struct timeval value;

    if (gettimeofday(&value, NULL) != 0) {
        return UINT64_MAX;
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
        (uint64_t)value.tv_usec * UINT64_C(1000);
}

static int wait_fd(int descriptor, short events, uint64_t deadline_ns)
{
    struct pollfd item = {descriptor, events, 0};

    for (;;) {
        const uint64_t now = host_now_ns();
        uint64_t remaining;
        int milliseconds;
        int result;

        if (now == UINT64_MAX || deadline_ns <= now) {
            return -1;
        }
        remaining = deadline_ns - now;
        milliseconds = remaining / UINT64_C(1000000) > INT32_MAX ?
            INT32_MAX : (int)((remaining + UINT64_C(999999)) /
                UINT64_C(1000000));
        result = poll(&item, 1U, milliseconds);
        if (result > 0) {
            return (item.revents & events) != 0 ? 0 : -1;
        }
        if (result == 0) {
            return -1;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
}

uint64_t phipia_monotonic_ns(void)
{
    return host_now_ns();
}

long phipia_realtime_seconds(void)
{
    return 1788177600L;
}

long phipia_random(void *buffer, size_t length)
{
    uint8_t *bytes = buffer;

    if (buffer == NULL && length != 0U) {
        return -1;
    }
    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = (uint8_t)(index * 29U + 17U);
    }
    return (long)length;
}

long phipia_random_strong(void *buffer, size_t length)
{
    return phipia_random(buffer, length);
}

long phipia_dns_resolve(const char *hostname, uint64_t deadline_ns)
{
    (void)deadline_ns;
    return hostname != NULL && hostname[0] != '\0' ? INT64_C(0x7f000001) : -1;
}

long phipia_stream_open(void)
{
    return socket(AF_INET, SOCK_STREAM, 0);
}

long phipia_stream_connect(phipia_handle_t stream,
    const struct phipia_ipv4_endpoint *endpoint, uint64_t deadline_ns)
{
    const int descriptor = (int)stream;
    struct sockaddr_in address = {0};

    if (endpoint == NULL || endpoint->port != peer_port ||
        deadline_ns <= host_now_ns()) {
        return -1;
    }
    address.sin_family = AF_INET;
    address.sin_port = htons(peer_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return connect(descriptor, (const struct sockaddr *)&address,
        sizeof(address));
}

long phipia_stream_read(phipia_handle_t stream, void *buffer, size_t length,
    uint64_t deadline_ns)
{
    const int descriptor = (int)stream;
    ssize_t count;

    if (length == 0U) {
        return 0;
    }
    if (wait_fd(descriptor, POLLIN, deadline_ns) != 0) {
        return -1;
    }
    count = recv(descriptor, buffer, length, 0);
    return count > 0 ? (long)count : -1;
}

long phipia_stream_write(phipia_handle_t stream, const void *buffer,
    size_t length, uint64_t deadline_ns)
{
    const int descriptor = (int)stream;
    ssize_t count;

    if (length == 0U) {
        return 0;
    }
    if (wait_fd(descriptor, POLLOUT, deadline_ns) != 0) {
        return -1;
    }
    count = send(descriptor, buffer, length, MSG_NOSIGNAL);
    return count > 0 ? (long)count : -1;
}

long phipia_stream_shutdown(phipia_handle_t stream, uint32_t flags,
    uint64_t deadline_ns)
{
    (void)flags;
    (void)deadline_ns;
    return shutdown((int)stream, SHUT_RDWR);
}

long phipia_network_cancel(phipia_handle_t handle)
{
    return shutdown((int)handle, SHUT_RDWR);
}

long phipia_handle_close(phipia_handle_t handle)
{
    return close((int)handle);
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

static bool decode_line(const char *line, const char *prefix, uint8_t *output,
    size_t capacity, size_t *length)
{
    const size_t prefix_length = strlen(prefix);
    size_t used = 0U;

    if (strncmp(line, prefix, prefix_length) != 0) {
        return false;
    }
    line += prefix_length;
    while (line[0] != '\0' && line[0] != '\n' && line[0] != '\r') {
        const int high = hex_value(line[0]);
        const int low = hex_value(line[1]);

        if (high < 0 || low < 0 || used == capacity) {
            return false;
        }
        output[used++] = (uint8_t)((unsigned)high << 4U | (unsigned)low);
        line += 2;
    }
    *length = used;
    return used != 0U;
}

static bool load_anchor(const char *path, br_x509_trust_anchor *anchor,
    uint8_t dn[TEST_DN_BYTES], uint8_t modulus[TEST_RSA_BYTES],
    uint8_t exponent[8])
{
    FILE *input = fopen(path, "rb");
    char line[TEST_RSA_BYTES * 2U + 8U];
    size_t dn_length = 0U;
    size_t modulus_length = 0U;
    size_t exponent_length = 0U;
    bool ok;

    if (input == NULL) {
        return false;
    }
    ok = fgets(line, sizeof(line), input) != NULL &&
        decode_line(line, "dn=", dn, TEST_DN_BYTES, &dn_length) &&
        fgets(line, sizeof(line), input) != NULL &&
        decode_line(line, "n=", modulus, TEST_RSA_BYTES, &modulus_length) &&
        fgets(line, sizeof(line), input) != NULL &&
        decode_line(line, "e=", exponent, 8U, &exponent_length) &&
        fgets(line, sizeof(line), input) == NULL && !ferror(input);
    if (fclose(input) != 0) {
        ok = false;
    }
    if (!ok) {
        return false;
    }
    *anchor = (br_x509_trust_anchor){
        .dn = {dn, dn_length},
        .flags = BR_X509_TA_CA,
        .pkey = {
            .key_type = BR_KEYTYPE_RSA,
            .key = {.rsa = {modulus, modulus_length, exponent, exponent_length}}
        }
    };
    return true;
}

int main(int argc, char **argv)
{
    uint8_t dn[TEST_DN_BYTES];
    uint8_t modulus[TEST_RSA_BYTES];
    uint8_t exponent[8];
    br_x509_trust_anchor anchor;
    struct phipia_tls_client *client = NULL;
    struct phipia_tls_client_config config;
    enum phipia_tls_status status;
    char response[2];
    char *end = NULL;
    unsigned long port;
    unsigned long expected;
    uint64_t deadline;

    if (argc != 6 || !load_anchor(argv[1], &anchor, dn, modulus, exponent)) {
        fputs("TLS host test: invalid arguments or anchor\n", stderr);
        return 2;
    }
    port = strtoul(argv[2], &end, 10);
    if (end == NULL || *end != '\0' || port == 0U || port > UINT16_MAX) {
        return 2;
    }
    peer_port = (uint16_t)port;
    expected = strtoul(argv[4], &end, 10);
    if (end == NULL || *end != '\0' || expected > PHIPIA_TLS_CLOSE) {
        return 2;
    }
    deadline = host_now_ns() + TEST_DEADLINE_NS;
    config = (struct phipia_tls_client_config){
        argv[3], peer_port, 0U, &anchor, 1U, deadline};
    status = phipia_tls_client_open(&config, &client);
    if (status != (enum phipia_tls_status)expected) {
        fprintf(stderr, "TLS host test: expected %s, got %s\n",
            phipia_tls_status_string((enum phipia_tls_status)expected),
            phipia_tls_status_string(status));
        if (client != NULL) {
            (void)phipia_tls_client_close(client, deadline);
        }
        return 1;
    }
    if (status != PHIPIA_TLS_OK) {
        printf("TLS refusal: %s\n", phipia_tls_status_string(status));
        return 0;
    }
    if (strcmp(argv[5], "request") != 0 ||
        phipia_tls_client_write(client, "GET / HTTP/1.0\r\n\r\n", 18U,
            deadline) != 18 ||
        phipia_tls_client_flush(client, deadline) != PHIPIA_TLS_OK ||
        phipia_tls_client_read(client, response, sizeof(response), deadline) !=
            (long)sizeof(response) || memcmp(response, "OK", 2U) != 0 ||
        phipia_tls_client_close(client, deadline) != PHIPIA_TLS_OK) {
        fputs("TLS host test: authenticated request/close failed\n", stderr);
        return 1;
    }
    puts("TLS host test: authenticated hostname, chain, time, request, close");
    return 0;
}
