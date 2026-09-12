/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L

#include <phipia/abi.h>
#include <phipia/network.h>
#include <phipia/runtime.h>
#include <phipia/tls.h>

#include <errno.h>
#include <inttypes.h>
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
#define HOST_INVALID_SOCKET INVALID_SOCKET
#define HOST_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <pthread.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
typedef int host_socket_t;
#define HOST_INVALID_SOCKET (-1)
#define HOST_CLOSE close
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#include "../apps/native-https/trust_anchor.h"

/* include/phipia/network.h is the kernel-private header and intentionally
 * shadows the installed SDK header in host admission builds. */
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
static unsigned live_handles;
static uintptr_t canceled_socket_bits = (uintptr_t)HOST_INVALID_SOCKET;
static host_socket_t active_socket = HOST_INVALID_SOCKET;
static bool stream_read_waiting;

struct cancel_read_task {
    struct phipia_tls_client *client;
    uint64_t deadline_ns;
    long result;
    long transport_error;
    bool entered;
};

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
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return UINT64_MAX;
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
        (uint64_t)value.tv_nsec;
#endif
}

static void host_pause_millis(unsigned milliseconds)
{
#if defined(_WIN32)
    Sleep(milliseconds);
#else
    const struct timespec duration = {
        (time_t)(milliseconds / 1000U),
        (long)(milliseconds % 1000U) * 1000000L};

    (void)nanosleep(&duration, NULL);
#endif
}

#if defined(_WIN32)
static DWORD WINAPI cancel_reader(LPVOID opaque)
#else
static void *cancel_reader(void *opaque)
#endif
{
    struct cancel_read_task *task = opaque;
    unsigned char byte;

    __atomic_store_n(&task->entered, true, __ATOMIC_RELEASE);
    task->result = phipia_tls_client_read(task->client, &byte, 1U,
        task->deadline_ns);
    task->transport_error =
        phipia_tls_client_transport_error(task->client);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static int wait_socket(host_socket_t descriptor, bool write,
    uint64_t deadline_ns)
{
    for (;;) {
        const uint64_t now = host_now_ns();
        uint64_t remaining;
        int milliseconds;

        if (now >= deadline_ns) {
            return -1;
        }
        remaining = deadline_ns - now;
        milliseconds = remaining / UINT64_C(1000000) > INT_MAX ? INT_MAX :
            (int)(remaining / UINT64_C(1000000));
        if (milliseconds == 0) {
            milliseconds = 1;
        }
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
            return -2;
        }
#else
        {
            struct pollfd item = {
                descriptor, (short)(write ? POLLOUT : POLLIN), 0};
            const int result = poll(&item, 1, milliseconds);

            if (result > 0) {
                return 0;
            }
            if (result == 0) {
                return -1;
            }
            if (errno != EINTR) {
                return -2;
            }
        }
#endif
    }
}

static long socket_error(host_socket_t descriptor)
{
    if ((uintptr_t)descriptor ==
            __atomic_load_n(&canceled_socket_bits, __ATOMIC_ACQUIRE)) {
        return -(long)PHIPIA_ECANCELED;
    }
#if defined(_WIN32)
    switch (WSAGetLastError()) {
    case WSAETIMEDOUT:
    case WSAEWOULDBLOCK:
        return -(long)PHIPIA_ETIMEDOUT;
    case WSAECONNRESET:
        return -(long)PHIPIA_EIO;
    default:
        return -(long)PHIPIA_EPIPE;
    }
#else
    switch (errno) {
    case ETIMEDOUT:
    case EAGAIN:
        return -(long)PHIPIA_ETIMEDOUT;
    case ECONNRESET:
        return -(long)PHIPIA_EIO;
    default:
        return -(long)PHIPIA_EPIPE;
    }
#endif
}

static host_socket_t host_descriptor(phipia_handle_t handle)
{
    return handle == 1U ? active_socket : HOST_INVALID_SOCKET;
}

uint64_t phipia_monotonic_ns(void)
{
    return host_now_ns();
}

long phipia_realtime_seconds(void)
{
    return INT64_C(1788177600);
}

long phipia_random(void *buffer, size_t length)
{
    unsigned char *bytes = buffer;
    uint32_t state = UINT32_C(0x8f31a42d);

    if (buffer == NULL && length != 0U) {
        return -(long)PHIPIA_EINVAL;
    }
    for (size_t index = 0U; index < length; ++index) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        bytes[index] = (unsigned char)state;
    }
    return (long)length;
}

long phipia_random_strong(void *buffer, size_t length)
{
    return phipia_random(buffer, length);
}

long phipia_dns_resolve(const char *hostname, uint64_t deadline_ns)
{
    if (hostname == NULL || hostname[0] == '\0' ||
        deadline_ns <= host_now_ns()) {
        return -(long)PHIPIA_EINVAL;
    }
    return INT64_C(0x7f000001);
}

long phipia_stream_open(void)
{
    const host_socket_t descriptor = socket(AF_INET, SOCK_STREAM, 0);

    if (descriptor == HOST_INVALID_SOCKET ||
        active_socket != HOST_INVALID_SOCKET) {
        if (descriptor != HOST_INVALID_SOCKET) {
            (void)HOST_CLOSE(descriptor);
        }
        return -(long)PHIPIA_EIO;
    }
    active_socket = descriptor;
    ++live_handles;
    return 1;
}

long phipia_stream_connect(phipia_handle_t stream,
    const struct phipia_ipv4_endpoint *endpoint, uint64_t deadline_ns)
{
    const host_socket_t descriptor = host_descriptor(stream);
    struct sockaddr_in address;

    if (endpoint == NULL || endpoint->port != peer_port ||
        deadline_ns <= host_now_ns()) {
        return -(long)PHIPIA_EINVAL;
    }
    (void)memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(peer_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(descriptor, (const struct sockaddr *)&address,
            sizeof(address)) != 0) {
        return socket_error(descriptor);
    }
    return 0;
}

long phipia_stream_read(phipia_handle_t stream, void *buffer, size_t length,
    uint64_t deadline_ns)
{
    const host_socket_t descriptor = host_descriptor(stream);
    int count;
    int ready;

    if ((uintptr_t)descriptor ==
            __atomic_load_n(&canceled_socket_bits, __ATOMIC_ACQUIRE)) {
        return -(long)PHIPIA_ECANCELED;
    }
    __atomic_store_n(&stream_read_waiting, true, __ATOMIC_RELEASE);
    ready = wait_socket(descriptor, false, deadline_ns);
    __atomic_store_n(&stream_read_waiting, false, __ATOMIC_RELEASE);
    if (ready != 0) {
        return ready == -1 ? -(long)PHIPIA_ETIMEDOUT :
            socket_error(descriptor);
    }
    count = recv(descriptor, (char *)buffer,
        length > INT_MAX ? INT_MAX : (int)length, 0);
    if (count > 0) {
        return count;
    }
    if ((uintptr_t)descriptor ==
            __atomic_load_n(&canceled_socket_bits, __ATOMIC_ACQUIRE)) {
        return -(long)PHIPIA_ECANCELED;
    }
    return count == 0 ? -(long)PHIPIA_EPIPE : socket_error(descriptor);
}

long phipia_stream_write(phipia_handle_t stream, const void *buffer,
    size_t length, uint64_t deadline_ns)
{
    const host_socket_t descriptor = host_descriptor(stream);
    int count;
    int ready;

    if ((uintptr_t)descriptor ==
            __atomic_load_n(&canceled_socket_bits, __ATOMIC_ACQUIRE)) {
        return -(long)PHIPIA_ECANCELED;
    }
    ready = wait_socket(descriptor, true, deadline_ns);
    if (ready != 0) {
        return ready == -1 ? -(long)PHIPIA_ETIMEDOUT :
            socket_error(descriptor);
    }
    count = send(descriptor, (const char *)buffer,
        length > INT_MAX ? INT_MAX : (int)length, MSG_NOSIGNAL);
    return count > 0 ? count : socket_error(descriptor);
}

long phipia_stream_shutdown(phipia_handle_t stream, uint32_t flags,
    uint64_t deadline_ns)
{
    const host_socket_t descriptor = host_descriptor(stream);

    (void)flags;
    (void)deadline_ns;
#if defined(_WIN32)
    return shutdown(descriptor, SD_BOTH) == 0 ? 0 :
        socket_error(descriptor);
#else
    return shutdown(descriptor, SHUT_RDWR) == 0 ? 0 :
        socket_error(descriptor);
#endif
}

long phipia_network_cancel(phipia_handle_t handle)
{
    const host_socket_t descriptor = host_descriptor(handle);

    __atomic_store_n(&canceled_socket_bits, (uintptr_t)descriptor,
        __ATOMIC_RELEASE);
#if defined(_WIN32)
    (void)shutdown(descriptor, SD_BOTH);
#else
    (void)shutdown(descriptor, SHUT_RDWR);
#endif
    return 0;
}

long phipia_handle_close(phipia_handle_t handle)
{
    const host_socket_t descriptor = host_descriptor(handle);
    int result;

    result = HOST_CLOSE(descriptor);
    if (live_handles == 0U) {
        return -(long)PHIPIA_ESTALE;
    }
    --live_handles;
    active_socket = HOST_INVALID_SOCKET;
    if ((uintptr_t)descriptor ==
            __atomic_load_n(&canceled_socket_bits, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&canceled_socket_bits,
            (uintptr_t)HOST_INVALID_SOCKET, __ATOMIC_RELEASE);
    }
    return result == 0 ? 0 : -(long)PHIPIA_EIO;
}

static int parse_unsigned(const char *text, unsigned long maximum,
    unsigned long *result)
{
    char *end;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > maximum) {
        return -1;
    }
    *result = value;
    return 0;
}

struct stream_sink {
    unsigned char body[128];
    size_t used;
    bool refuse;
};

static long stream_body_write(
    void *context,
    const void *bytes,
    size_t byte_count
)
{
    struct stream_sink *sink = context;
    if (sink == NULL || bytes == NULL || sink->refuse ||
        sink->used > sizeof(sink->body) ||
        byte_count > sizeof(sink->body) - sink->used) {
        return -(long)PHIPIA_EIO;
    }
    (void)memcpy(sink->body + sink->used, bytes, byte_count);
    sink->used += byte_count;
    return (long)byte_count;
}

static int run_cancel(const char *hostname, uint64_t deadline_ns)
{
    struct phipia_tls_client *client = NULL;
    const struct phipia_tls_client_config config = {
        hostname, peer_port, 0U, phipia_https_test_anchors,
        sizeof(phipia_https_test_anchors) /
            sizeof(phipia_https_test_anchors[0]), deadline_ns};
    struct cancel_read_task task;
    long cancel_result;
    enum phipia_tls_status close_status;
    bool joined;
#if defined(_WIN32)
    HANDLE thread;
#else
    pthread_t thread;
#endif

    if (phipia_tls_client_open(&config, &client) != PHIPIA_TLS_OK ||
        client == NULL) {
        return 1;
    }
    task = (struct cancel_read_task){client, deadline_ns, 0, 0, false};
#if defined(_WIN32)
    thread = CreateThread(NULL, 0, cancel_reader, &task, 0, NULL);
    if (thread == NULL) {
        (void)phipia_tls_client_close(client, deadline_ns);
        return 1;
    }
#else
    if (pthread_create(&thread, NULL, cancel_reader, &task) != 0) {
        (void)phipia_tls_client_close(client, deadline_ns);
        return 1;
    }
#endif
    for (unsigned wait = 0U; wait < 1000U &&
            !__atomic_load_n(&stream_read_waiting, __ATOMIC_ACQUIRE); ++wait) {
        host_pause_millis(1U);
    }
    if (!__atomic_load_n(&stream_read_waiting, __ATOMIC_ACQUIRE)) {
        (void)phipia_tls_client_cancel(client);
#if defined(_WIN32)
        (void)WaitForSingleObject(thread, 2000U);
        (void)CloseHandle(thread);
#else
        (void)pthread_join(thread, NULL);
#endif
        (void)phipia_tls_client_close(client, deadline_ns);
        return 1;
    }
    cancel_result = phipia_tls_client_cancel(client);
#if defined(_WIN32)
    joined = WaitForSingleObject(thread, 2000U) == WAIT_OBJECT_0;
    (void)CloseHandle(thread);
#else
    joined = pthread_join(thread, NULL) == 0;
#endif
    close_status = phipia_tls_client_close(client, deadline_ns);
    if (cancel_result != 0 || !joined || task.result != -1 ||
        task.transport_error != -(long)PHIPIA_ECANCELED ||
        close_status != PHIPIA_TLS_IO) {
        return 1;
    }
    puts("HTTPS REFUSAL blocking TLS operation canceled across threads");
    return 0;
}

static int run_expired_operation(const char *hostname, uint64_t deadline_ns)
{
    struct phipia_tls_client *client = NULL;
    const struct phipia_tls_client_config config = {
        hostname, peer_port, 0U, phipia_https_test_anchors,
        sizeof(phipia_https_test_anchors) /
            sizeof(phipia_https_test_anchors[0]), deadline_ns};
    unsigned char byte;

    if (phipia_tls_client_open(&config, &client) != PHIPIA_TLS_OK ||
        client == NULL) {
        return 1;
    }
    while (host_now_ns() <= deadline_ns) {
        host_pause_millis(1U);
    }
    if (phipia_tls_client_read(client, &byte, 1U, deadline_ns) != -1 ||
        phipia_tls_client_transport_error(client) !=
            -(long)PHIPIA_ETIMEDOUT ||
        phipia_tls_client_close(client, deadline_ns) != PHIPIA_TLS_IO) {
        return 1;
    }
    puts("HTTPS REFUSAL expired per-operation deadline classified");
    return 0;
}

int main(int argc, char **argv)
{
    unsigned long parsed_port;
    unsigned long expected;
    const char *mode;
    const char *hostname;
    uint64_t deadline_ns;
    int result = 0;

#if defined(_WIN32)
    WSADATA sockets;

    if (WSAStartup(MAKEWORD(2, 2), &sockets) != 0) {
        return 2;
    }
#endif
    if (argc != 5 || parse_unsigned(argv[1], UINT16_MAX, &parsed_port) != 0 ||
        parsed_port == 0U ||
        parse_unsigned(argv[4], PHIPIA_HTTPS_BODY_WRITE, &expected) != 0) {
        fprintf(stderr, "usage: %s PORT HOSTNAME MODE EXPECTED_STATUS\n",
            argv[0]);
        result = 2;
        goto finished;
    }
    peer_port = (uint16_t)parsed_port;
    hostname = argv[2];
    mode = argv[3];
    deadline_ns = host_now_ns() +
        (strcmp(mode, "timeout") == 0 ? UINT64_C(300000000) :
         strcmp(mode, "expired-operation") == 0 ? UINT64_C(1000000000) :
            UINT64_C(3000000000));
    if (strcmp(mode, "cancel") == 0) {
        result = expected == PHIPIA_HTTPS_CANCELED ?
            run_cancel(hostname, deadline_ns) : 1;
    } else if (strcmp(mode, "expired-operation") == 0) {
        result = expected == PHIPIA_HTTPS_TIMEOUT ?
            run_expired_operation(hostname, deadline_ns) : 1;
    } else if (strcmp(mode, "stream-success") == 0 ||
            strcmp(mode, "stream-refusal") == 0) {
        struct stream_sink sink = {{0U}, 0U,
            strcmp(mode, "stream-refusal") == 0};
        struct phipia_https_response response;
        const struct phipia_https_stream_request request = {
            hostname, peer_port, 0U, "/artifact.bin",
            phipia_https_test_anchors,
            sizeof(phipia_https_test_anchors) /
                sizeof(phipia_https_test_anchors[0]),
            deadline_ns, sizeof(sink.body), stream_body_write, &sink};
        const enum phipia_https_status status =
            phipia_https_get_stream(&request, &response);
        static const unsigned char expected_body[] =
            "hello from the Phipia HTTPS peer\n";

        printf("HTTPS STREAM RESULT %u %s tls=%d transport=%ld handles=%u\n",
            (unsigned)status, phipia_https_status_string(status),
            response.bearssl_error, response.transport_error, live_handles);
        if ((unsigned long)status != expected ||
            (status == PHIPIA_HTTPS_OK &&
             (response.status_code != 200U ||
              response.body_length != sizeof(expected_body) - 1U ||
              sink.used != sizeof(expected_body) - 1U ||
              memcmp(sink.body, expected_body,
                  sizeof(expected_body) - 1U) != 0)) ||
            (status == PHIPIA_HTTPS_BODY_WRITE && sink.used != 0U)) {
            result = 1;
        }
    } else {
        unsigned char body[128];
        struct phipia_https_response response;
        const struct phipia_https_request request = {
            hostname, peer_port, 0U, "/artifact.bin",
            phipia_https_test_anchors,
            sizeof(phipia_https_test_anchors) /
                sizeof(phipia_https_test_anchors[0]),
            deadline_ns, body, sizeof(body)};
        const enum phipia_https_status status =
            phipia_https_get(&request, &response);
        static const unsigned char expected_body[] =
            "hello from the Phipia HTTPS peer\n";

        printf("HTTPS RESULT %u %s tls=%d transport=%ld handles=%u\n",
            (unsigned)status, phipia_https_status_string(status),
            response.bearssl_error, response.transport_error, live_handles);
        if ((unsigned long)status != expected ||
            (status == PHIPIA_HTTPS_OK &&
             (response.status_code != 200U ||
              response.body_length != sizeof(expected_body) - 1U ||
              memcmp(body, expected_body, sizeof(expected_body) - 1U) != 0))) {
            result = 1;
        }
    }
    if (live_handles != 0U) {
        fprintf(stderr, "HTTPS HANDLE LEAK %u\n", live_handles);
        result = 1;
    } else {
        puts("HTTPS HANDLES clean");
    }
finished:
#if defined(_WIN32)
    (void)WSACleanup();
#endif
    return result;
}
