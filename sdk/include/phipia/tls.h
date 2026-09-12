/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_USER_TLS_H
#define PHIPIA_USER_TLS_H

/* BearSSL 0.6 deliberately leaves BR_DOXYGEN_IGNORE undefined while using it
 * in two #if expressions.  Isolate that exact upstream -Wundef diagnostic so
 * Phipia applications can include this public header under -Werror without
 * changing BearSSL's many #ifndef BR_DOXYGEN_IGNORE declarations. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#endif
#include <bearssl.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#include <stddef.h>
#include <stdint.h>

struct phipia_tls_client;

#define PHIPIA_HTTPS_MAX_HEADER_BYTES 4096U
#define PHIPIA_HTTPS_MAX_PATH_BYTES 1024U

enum phipia_tls_status {
    PHIPIA_TLS_OK = 0,
    PHIPIA_TLS_ARGUMENT,
    PHIPIA_TLS_NO_MEMORY,
    PHIPIA_TLS_TRUST,
    PHIPIA_TLS_CLOCK,
    PHIPIA_TLS_ENTROPY,
    PHIPIA_TLS_DNS,
    PHIPIA_TLS_TRANSPORT,
    PHIPIA_TLS_HANDSHAKE,
    PHIPIA_TLS_IO,
    PHIPIA_TLS_CLOSE
};

struct phipia_tls_client_config {
    const char *hostname;
    uint16_t port;
    uint16_t reserved;
    const br_x509_trust_anchor *trust_anchors;
    size_t trust_anchor_count;
    uint64_t deadline_ns;
};

/* Diagnostics are output-only.  They make a failed open auditable even though
 * no client object is returned to the caller. */
struct phipia_tls_diagnostics {
    int bearssl_error;
    long transport_error;
};

enum phipia_https_status {
    PHIPIA_HTTPS_OK = 0,
    PHIPIA_HTTPS_ARGUMENT,
    PHIPIA_HTTPS_NO_MEMORY,
    PHIPIA_HTTPS_TRUST,
    PHIPIA_HTTPS_CLOCK,
    PHIPIA_HTTPS_ENTROPY,
    PHIPIA_HTTPS_DNS,
    PHIPIA_HTTPS_TRANSPORT,
    PHIPIA_HTTPS_TIMEOUT,
    PHIPIA_HTTPS_CANCELED,
    PHIPIA_HTTPS_RESET,
    PHIPIA_HTTPS_TRUNCATED,
    PHIPIA_HTTPS_HOSTNAME,
    PHIPIA_HTTPS_CERTIFICATE_TIME,
    PHIPIA_HTTPS_AUTHENTICATION,
    PHIPIA_HTTPS_HANDSHAKE,
    PHIPIA_HTTPS_IO,
    PHIPIA_HTTPS_HTTP_VERSION,
    PHIPIA_HTTPS_HTTP_STATUS,
    PHIPIA_HTTPS_HTTP_HEADERS,
    PHIPIA_HTTPS_CONTENT_LENGTH_REQUIRED,
    PHIPIA_HTTPS_CONTENT_TOO_LARGE,
    PHIPIA_HTTPS_BODY_TRUNCATED,
    PHIPIA_HTTPS_BODY_EXTRA,
    PHIPIA_HTTPS_CLOSE,
    PHIPIA_HTTPS_BODY_WRITE
};

typedef long (*phipia_https_body_write_function)(
    void *context,
    const void *bytes,
    size_t byte_count
);

struct phipia_https_request {
    const char *hostname;
    uint16_t port;
    uint16_t reserved;
    const char *path;
    const br_x509_trust_anchor *trust_anchors;
    size_t trust_anchor_count;
    uint64_t deadline_ns;
    void *body;
    size_t body_capacity;
};

struct phipia_https_response {
    uint16_t status_code;
    uint16_t reserved;
    size_t content_length;
    size_t body_length;
    int bearssl_error;
    long transport_error;
};

struct phipia_https_stream_request {
    const char *hostname;
    uint16_t port;
    uint16_t reserved;
    const char *path;
    const br_x509_trust_anchor *trust_anchors;
    size_t trust_anchor_count;
    uint64_t deadline_ns;
    size_t body_limit;
    phipia_https_body_write_function write_body;
    void *write_context;
};

enum phipia_tls_status phipia_tls_client_open(
    const struct phipia_tls_client_config *config,
    struct phipia_tls_client **result);
enum phipia_tls_status phipia_tls_client_open_diagnostic(
    const struct phipia_tls_client_config *config,
    struct phipia_tls_diagnostics *diagnostics,
    struct phipia_tls_client **result);
long phipia_tls_client_read(struct phipia_tls_client *client, void *buffer,
    size_t length, uint64_t deadline_ns);
long phipia_tls_client_write(struct phipia_tls_client *client,
    const void *buffer, size_t length, uint64_t deadline_ns);
enum phipia_tls_status phipia_tls_client_flush(
    struct phipia_tls_client *client, uint64_t deadline_ns);
long phipia_tls_client_cancel(struct phipia_tls_client *client);
enum phipia_tls_status phipia_tls_client_close(
    struct phipia_tls_client *client, uint64_t deadline_ns);
enum phipia_tls_status phipia_tls_client_status(
    const struct phipia_tls_client *client);
int phipia_tls_client_bearssl_error(const struct phipia_tls_client *client);
long phipia_tls_client_transport_error(const struct phipia_tls_client *client);
const char *phipia_tls_status_string(enum phipia_tls_status status);
enum phipia_https_status phipia_https_get(
    const struct phipia_https_request *request,
    struct phipia_https_response *response);
enum phipia_https_status phipia_https_get_stream(
    const struct phipia_https_stream_request *request,
    struct phipia_https_response *response);
const char *phipia_https_status_string(enum phipia_https_status status);

#endif
