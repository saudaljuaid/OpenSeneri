/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include <phipia/tls.h>

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
typedef SOCKET host_socket_t;
typedef int host_io_count_t;
#define HOST_INVALID_SOCKET INVALID_SOCKET
#define HOST_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
typedef int host_socket_t;
typedef ssize_t host_io_count_t;
#define HOST_INVALID_SOCKET (-1)
#define HOST_CLOSE close
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

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
#if defined(_WIN32)
static bool host_sockets_ready;
#endif

static uint64_t host_now_ns(void)
{
#if defined(_WIN32)
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;

    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&counter) || frequency.QuadPart <= 0 ||
        frequency.QuadPart > INT64_C(1000000000)) {
        return UINT64_MAX;
    }
    {
        const uint64_t frequency_value = (uint64_t)frequency.QuadPart;
        const uint64_t counter_value = (uint64_t)counter.QuadPart;
        const uint64_t seconds = counter_value / frequency_value;
        const uint64_t remainder = counter_value % frequency_value;

        if (seconds > UINT64_MAX / UINT64_C(1000000000)) {
            return UINT64_MAX;
        }
        return seconds * UINT64_C(1000000000) +
            remainder * UINT64_C(1000000000) / frequency_value;
    }
#else
    struct timeval value;

    if (gettimeofday(&value, NULL) != 0) {
        return UINT64_MAX;
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
        (uint64_t)value.tv_usec * UINT64_C(1000);
#endif
}

static int wait_fd(host_socket_t descriptor, bool write, uint64_t deadline_ns)
{
    for (;;) {
        const uint64_t now = host_now_ns();
        uint64_t remaining;
        int milliseconds;

        if (now == UINT64_MAX || deadline_ns <= now) {
            return -1;
        }
        remaining = deadline_ns - now;
        milliseconds = remaining / UINT64_C(1000000) > INT32_MAX ?
            INT32_MAX : (int)((remaining + UINT64_C(999999)) /
                UINT64_C(1000000));
#if defined(_WIN32)
        {
            fd_set set;
            struct timeval timeout = {
                milliseconds / 1000, (milliseconds % 1000) * 1000};
            int result;

            FD_ZERO(&set);
            FD_SET(descriptor, &set);
            result = select(0, write ? NULL : &set, write ? &set : NULL,
                NULL, &timeout);
            if (result > 0) {
                return 0;
            }
            if (result == 0) {
                return -1;
            }
            return -1;
        }
#else
        {
            struct pollfd item = {
                descriptor, (short)(write ? POLLOUT : POLLIN), 0};
            const int result = poll(&item, 1U, milliseconds);

            if (result > 0) {
                return (item.revents & item.events) != 0 ? 0 : -1;
            }
            if (result == 0) {
                return -1;
            }
            if (errno != EINTR) {
                return -1;
            }
        }
#endif
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
#if defined(_WIN32)
    if (!host_sockets_ready) {
        WSADATA sockets;

        if (WSAStartup(MAKEWORD(2, 2), &sockets) != 0) {
            return -1;
        }
        host_sockets_ready = true;
    }
#endif
    return (long)(intptr_t)socket(AF_INET, SOCK_STREAM, 0);
}

long phipia_stream_connect(phipia_handle_t stream,
    const struct phipia_ipv4_endpoint *endpoint, uint64_t deadline_ns)
{
    const host_socket_t descriptor = (host_socket_t)(uintptr_t)stream;
    struct sockaddr_in address = {0};

    if (endpoint == NULL || endpoint->port != peer_port ||
        deadline_ns <= host_now_ns()) {
        return -1;
    }
    address.sin_family = AF_INET;
    address.sin_port = htons(peer_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return connect(descriptor, (const struct sockaddr *)&address,
        sizeof(address)) == 0 ? 0 : -1;
}

long phipia_stream_read(phipia_handle_t stream, void *buffer, size_t length,
    uint64_t deadline_ns)
{
    const host_socket_t descriptor = (host_socket_t)(uintptr_t)stream;
    host_io_count_t count;

    if (length == 0U) {
        return 0;
    }
    if (wait_fd(descriptor, false, deadline_ns) != 0) {
        return -1;
    }
    count = recv(descriptor, (char *)buffer,
        length > INT_MAX ? INT_MAX : (int)length, 0);
    return count > 0 ? (long)count : -1;
}

long phipia_stream_write(phipia_handle_t stream, const void *buffer,
    size_t length, uint64_t deadline_ns)
{
    const host_socket_t descriptor = (host_socket_t)(uintptr_t)stream;
    host_io_count_t count;

    if (length == 0U) {
        return 0;
    }
    if (wait_fd(descriptor, true, deadline_ns) != 0) {
        return -1;
    }
    count = send(descriptor, (const char *)buffer,
        length > INT_MAX ? INT_MAX : (int)length, MSG_NOSIGNAL);
    return count > 0 ? (long)count : -1;
}

long phipia_stream_shutdown(phipia_handle_t stream, uint32_t flags,
    uint64_t deadline_ns)
{
    const host_socket_t descriptor = (host_socket_t)(uintptr_t)stream;

    (void)flags;
    (void)deadline_ns;
#if defined(_WIN32)
    return shutdown(descriptor, SD_BOTH);
#else
    return shutdown(descriptor, SHUT_RDWR);
#endif
}

long phipia_network_cancel(phipia_handle_t handle)
{
    const host_socket_t descriptor = (host_socket_t)(uintptr_t)handle;

#if defined(_WIN32)
    return shutdown(descriptor, SD_BOTH);
#else
    return shutdown(descriptor, SHUT_RDWR);
#endif
}

long phipia_handle_close(phipia_handle_t handle)
{
    const long result = HOST_CLOSE((host_socket_t)(uintptr_t)handle);

#if defined(_WIN32)
    if (host_sockets_ready) {
        (void)WSACleanup();
        host_sockets_ready = false;
    }
#endif
    return result;
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
