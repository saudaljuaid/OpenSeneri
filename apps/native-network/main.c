/* SPDX-License-Identifier: GPL-3.0-only */
#include <phipia/network.h>
#include <phipia/runtime.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_ADDRESS UINT32_C(0x0A000214)
#define OPERATION_NS UINT64_C(5000000000)

static const char expected_body[] = "hello from the Phipia network\n";

static uint64_t deadline(void)
{
    return phipia_monotonic_ns() + OPERATION_NS;
}

static int close_handle(phipia_handle_t handle)
{
    return phipia_handle_close(handle) < 0 ? -1 : 0;
}

static int verify_http(const char *response, size_t length,
    const char **body, size_t *body_length)
{
    static const char status[] = "HTTP/1.1 200 OK\r\n";
    static const char content_length[] = "Content-Length: ";
    const char *header_end;
    const char *field;
    char *number_end;
    unsigned long declared;

    if (length < sizeof(status) - 1U ||
        memcmp(response, status, sizeof(status) - 1U) != 0) {
        return -1;
    }
    header_end = strstr(response, "\r\n\r\n");
    field = strstr(response, content_length);
    if (header_end == NULL || field == NULL || field >= header_end) {
        return -1;
    }
    field += sizeof(content_length) - 1U;
    declared = strtoul(field, &number_end, 10);
    if (number_end == field || number_end + 2 > header_end ||
        number_end[0] != '\r' || number_end[1] != '\n') {
        return -1;
    }
    *body = header_end + 4;
    *body_length = length - (size_t)(*body - response);
    return declared == *body_length ? 0 : -1;
}

static int exercise_udp(uint32_t address)
{
    static const char message[] = "native udp echo";
    struct phipia_ipv4_endpoint destination = {address, 4242U, 0U};
    struct phipia_ipv4_endpoint source = {0U, 0U, 0U};
    struct phipia_ipv4_endpoint local = {0U, 0U, 0U};
    char response[32];
    const long opened = phipia_datagram_open();
    long count;

    if (opened < 0) {
        return -10;
    }
    const phipia_handle_t datagram = (phipia_handle_t)opened;
    if (phipia_datagram_bind(datagram, 50010U) < 0 ||
        phipia_network_address(datagram, 0, &local) < 0 ||
        local.port != 50010U || local.address == 0U ||
        phipia_datagram_send(datagram, &destination, message,
            sizeof(message) - 1U, deadline()) != (long)(sizeof(message) - 1U)) {
        (void)close_handle(datagram);
        return -11;
    }
    count = phipia_datagram_receive(datagram, &source, response,
        sizeof(response), deadline());
    if (count != (long)(sizeof(message) - 1U) ||
        source.address != address || source.port != 4242U ||
        memcmp(response, message, sizeof(message) - 1U) != 0 ||
        close_handle(datagram) != 0) {
        return -12;
    }
    return 0;
}

static int exercise_failures(uint32_t address)
{
    struct phipia_ipv4_endpoint endpoint = {address, 81U, 0U};
    long opened = phipia_stream_open();
    phipia_handle_t stream;

    if (opened < 0) {
        return -20;
    }
    stream = (phipia_handle_t)opened;
    if (phipia_stream_connect(stream, &endpoint, deadline()) != -PHIPIA_EIO ||
        close_handle(stream) != 0) {
        return -21;
    }

    opened = phipia_stream_open();
    if (opened < 0) {
        return -22;
    }
    stream = (phipia_handle_t)opened;
    endpoint.port = 82U;
    if (phipia_stream_connect(stream, &endpoint,
            phipia_monotonic_ns() + UINT64_C(150000000)) !=
            -PHIPIA_ETIMEDOUT || close_handle(stream) != 0) {
        return -23;
    }

    opened = phipia_stream_open();
    if (opened < 0) {
        return -24;
    }
    stream = (phipia_handle_t)opened;
    if (phipia_network_cancel(stream) < 0 ||
        phipia_stream_connect(stream, &endpoint, deadline()) !=
            -PHIPIA_ECANCELED || close_handle(stream) != 0) {
        return -25;
    }
    const long malformed = phipia_dns_resolve("malformed.test", deadline());
    if (malformed != -PHIPIA_EIO) {
        printf("PHIPIA NETAPP MALFORMED DNS result=%ld expected=%d\n",
            malformed, -PHIPIA_EIO);
        return -26;
    }
    return 0;
}

static int leave_handles_for_process_teardown(void)
{
    const long stream = phipia_stream_open();
    const long datagram = phipia_datagram_open();

    if (stream < 0 || datagram < 0 ||
        phipia_datagram_bind((phipia_handle_t)datagram, 50011U) < 0) {
        if (stream >= 0) (void)close_handle((phipia_handle_t)stream);
        if (datagram >= 0) (void)close_handle((phipia_handle_t)datagram);
        return -1;
    }
    /* The kernel completion proof requires both objects to die with process. */
    return 0;
}

int main(int argc, char **argv, char **environment)
{
    static const char request[] =
        "GET /welcome.txt HTTP/1.1\r\n"
        "Host: phipia.test\r\n"
        "Connection: close\r\n\r\n";
    struct phipia_ipv4_endpoint endpoint;
    struct phipia_ipv4_endpoint peer;
    struct phipia_ipv4_endpoint local;
    char response[768];
    const char *body;
    size_t body_length;
    size_t received = 0U;
    long resolved;
    long opened;
    phipia_handle_t stream;
    FILE *output;

    (void)argc;
    (void)argv;
    (void)environment;
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    puts("PHIPIA NETAPP PHASE start");
    resolved = phipia_dns_resolve("phipia.test", deadline());
    if (resolved != (long)HTTP_ADDRESS) {
        return 30;
    }
    puts("PHIPIA NETAPP PHASE dns PASS");
    endpoint = (struct phipia_ipv4_endpoint){(uint32_t)resolved, 80U, 0U};
    opened = phipia_stream_open();
    if (opened < 0) {
        return 31;
    }
    stream = (phipia_handle_t)opened;
    if (phipia_stream_connect(stream, &endpoint, deadline()) < 0 ||
        phipia_network_address(stream, 1, &peer) < 0 ||
        phipia_network_address(stream, 0, &local) < 0 ||
        peer.address != HTTP_ADDRESS || peer.port != 80U ||
        local.address == 0U || local.port == 0U ||
        phipia_stream_write(stream, request, sizeof(request) - 1U,
            deadline()) != (long)(sizeof(request) - 1U)) {
        (void)close_handle(stream);
        return 32;
    }
    puts("PHIPIA NETAPP PHASE tcp-connect-write PASS");
    while (received < sizeof(response) - 1U) {
        const long count = phipia_stream_read(stream, response + received,
            sizeof(response) - 1U - received, deadline());

        if (count > 0) {
            received += (size_t)count;
        } else if (count == -PHIPIA_EPIPE) {
            break;
        } else {
            (void)close_handle(stream);
            return 33;
        }
    }
    response[received] = '\0';
    if (verify_http(response, received, &body, &body_length) != 0 ||
        body_length != sizeof(expected_body) - 1U ||
        memcmp(body, expected_body, sizeof(expected_body) - 1U) != 0 ||
        phipia_stream_shutdown(stream, PHIPIA_SHUTDOWN_WRITE, deadline()) < 0 ||
        close_handle(stream) != 0) {
        return 34;
    }
    puts("PHIPIA NETAPP PHASE http-framing-shutdown PASS");
    output = fopen("HTTP.TXT", "w");
    if (output == NULL || fwrite(body, 1U, body_length, output) != body_length ||
        fflush(output) != 0 || fclose(output) != 0 ||
        phipia_syscall1(PHIPIA_SYS_VOLUME_SYNC, PHIPIA_VOLUME_DATA) < 0) {
        return 35;
    }
    puts("PHIPIA NETAPP PHASE data-sync PASS");
    {
        const int udp = exercise_udp(HTTP_ADDRESS);
        const int failures = udp == 0 ? exercise_failures(HTTP_ADDRESS) : 0;
        const int teardown = udp == 0 && failures == 0 ?
            leave_handles_for_process_teardown() : 0;

        if (udp != 0 || failures != 0 || teardown != 0) {
            printf("PHIPIA NETAPP FAILURE udp=%d failures=%d teardown=%d\n",
                udp, failures, teardown);
            return 36;
        }
    }
    puts("PHIPIA NETAPP PHASE udp-failures-teardown PASS");
    printf("PHIPIA NETAPP PASS dns=10.0.2.20 http=%u udp=echo timeout reset cancel malformed-dns\n",
        (unsigned int)body_length);
    return 0;
}
