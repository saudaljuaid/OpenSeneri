/* SPDX-License-Identifier: GPL-3.0-only */
#include <phipia/tls.h>

#include <limits.h>
#include <phipia/network.h>
#include <phipia/runtime.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define PHIPIA_TLS_MAX_HOSTNAME 253U
#define PHIPIA_TLS_MAX_LABEL 63U
#define PHIPIA_TLS_MAX_TRUST_ANCHORS 16U
#define PHIPIA_TLS_MAX_TRUST_BYTES 81920U
#define PHIPIA_TLS_MAX_HANDSHAKE_STEPS 4096U
#define PHIPIA_TLS_UNIX_EPOCH_DAYS UINT64_C(719528)
#define PHIPIA_TLS_ENTROPY_BYTES 32U
#define PHIPIA_TLS_CLOCK_MIN UINT64_C(1577836800)
#define PHIPIA_TLS_CLOCK_MAX UINT64_C(4102444799)
#define PHIPIA_HTTPS_REQUEST_BYTES 1536U
#define PHIPIA_HTTPS_BODY_CHUNK_BYTES 4096U

struct phipia_tls_client {
    br_ssl_client_context ssl;
    br_x509_minimal_context x509;
    br_sslio_context io;
    unsigned char buffer[BR_SSL_BUFSIZE_BIDI];
    br_x509_trust_anchor anchors[PHIPIA_TLS_MAX_TRUST_ANCHORS];
    unsigned char *anchor_storage;
    size_t anchor_storage_length;
    char hostname[PHIPIA_TLS_MAX_HOSTNAME + 1U];
    phipia_handle_t stream;
    uint64_t deadline_ns;
    long transport_error;
    enum phipia_tls_status status;
    bool tls_ready;
    bool peer_closed;
    bool canceled;
    bool received_transport_bytes;
};

static const uint16_t suites[] = {
    BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256
};

static void secure_zero(void *memory, size_t length)
{
    volatile unsigned char *bytes = memory;

    while (length != 0U) {
        *bytes++ = 0U;
        --length;
    }
}

static bool size_add(size_t left, size_t right, size_t *result)
{
    if (result == NULL || left > SIZE_MAX - right) {
        return false;
    }
    *result = left + right;
    return true;
}

static bool hostname_valid(const char *hostname)
{
    size_t length;
    size_t label = 0U;

    if (hostname == NULL) {
        return false;
    }
    length = strlen(hostname);
    if (length == 0U || length > PHIPIA_TLS_MAX_HOSTNAME) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char value = (unsigned char)hostname[index];

        if (value == '.') {
            if (label == 0U || label > PHIPIA_TLS_MAX_LABEL ||
                hostname[index - 1U] == '-') {
                return false;
            }
            label = 0U;
            continue;
        }
        if (!((value >= 'a' && value <= 'z') ||
                (value >= '0' && value <= '9') || value == '-') ||
            (label == 0U && value == '-')) {
            return false;
        }
        ++label;
    }
    return label != 0U && label <= PHIPIA_TLS_MAX_LABEL &&
        hostname[length - 1U] != '-';
}

static bool trust_anchor_valid(const br_x509_trust_anchor *anchor)
{
    if (anchor == NULL || anchor->dn.data == NULL ||
        anchor->dn.len == 0U || anchor->dn.len > 4096U ||
        anchor->flags != BR_X509_TA_CA) {
        return false;
    }
    if (anchor->pkey.key_type == BR_KEYTYPE_RSA) {
        return anchor->pkey.key.rsa.n != NULL &&
            anchor->pkey.key.rsa.nlen >= 256U &&
            anchor->pkey.key.rsa.nlen <= 512U &&
            anchor->pkey.key.rsa.e != NULL &&
            anchor->pkey.key.rsa.elen != 0U &&
            anchor->pkey.key.rsa.elen <= 8U;
    }
    return anchor->pkey.key_type == BR_KEYTYPE_EC &&
        anchor->pkey.key.ec.curve == BR_EC_secp256r1 &&
        anchor->pkey.key.ec.q != NULL &&
        anchor->pkey.key.ec.qlen == 65U &&
        anchor->pkey.key.ec.q[0] == 4U;
}

static bool trust_storage_size(const br_x509_trust_anchor *anchors,
    size_t count, size_t *total)
{
    size_t used = 0U;

    for (size_t index = 0U; index < count; ++index) {
        const br_x509_trust_anchor *anchor = &anchors[index];
        size_t key_bytes;

        if (!trust_anchor_valid(anchor)) {
            return false;
        }
        if (anchor->pkey.key_type == BR_KEYTYPE_RSA) {
            if (!size_add(anchor->pkey.key.rsa.nlen,
                    anchor->pkey.key.rsa.elen, &key_bytes)) {
                return false;
            }
        } else {
            key_bytes = anchor->pkey.key.ec.qlen;
        }
        if (!size_add(used, anchor->dn.len, &used) ||
            !size_add(used, key_bytes, &used) ||
            used > PHIPIA_TLS_MAX_TRUST_BYTES) {
            return false;
        }
    }
    *total = used;
    return true;
}

static bool copy_trust_anchors(struct phipia_tls_client *client,
    const br_x509_trust_anchor *source, size_t count)
{
    unsigned char *cursor;
    size_t total;

    if (!trust_storage_size(source, count, &total)) {
        return false;
    }
    client->anchor_storage = malloc(total);
    if (client->anchor_storage == NULL) {
        return false;
    }
    client->anchor_storage_length = total;
    cursor = client->anchor_storage;
    for (size_t index = 0U; index < count; ++index) {
        client->anchors[index] = source[index];
        (void)memcpy(cursor, source[index].dn.data, source[index].dn.len);
        client->anchors[index].dn.data = cursor;
        cursor += source[index].dn.len;
        if (source[index].pkey.key_type == BR_KEYTYPE_RSA) {
            const br_rsa_public_key *key = &source[index].pkey.key.rsa;

            (void)memcpy(cursor, key->n, key->nlen);
            client->anchors[index].pkey.key.rsa.n = cursor;
            cursor += key->nlen;
            (void)memcpy(cursor, key->e, key->elen);
            client->anchors[index].pkey.key.rsa.e = cursor;
            cursor += key->elen;
        } else {
            const br_ec_public_key *key = &source[index].pkey.key.ec;

            (void)memcpy(cursor, key->q, key->qlen);
            client->anchors[index].pkey.key.ec.q = cursor;
            cursor += key->qlen;
        }
    }
    return (size_t)(cursor - client->anchor_storage) == total;
}

static void diagnostics_clear(struct phipia_tls_diagnostics *diagnostics)
{
    if (diagnostics != NULL) {
        diagnostics->bearssl_error = 0;
        diagnostics->transport_error = 0;
    }
}

static void diagnostics_capture(const struct phipia_tls_client *client,
    struct phipia_tls_diagnostics *diagnostics)
{
    if (client != NULL && diagnostics != NULL) {
        diagnostics->bearssl_error =
            br_ssl_engine_last_error(&client->ssl.eng);
        diagnostics->transport_error = client->transport_error;
    }
}

static long deadline_error(struct phipia_tls_client *client)
{
    if (__atomic_load_n(&client->canceled, __ATOMIC_ACQUIRE)) {
        return -(long)PHIPIA_ECANCELED;
    }
    if (client->deadline_ns <= phipia_monotonic_ns()) {
        return -(long)PHIPIA_ETIMEDOUT;
    }
    return 0;
}

static bool set_operation_deadline(struct phipia_tls_client *client,
    uint64_t deadline_ns)
{
    client->deadline_ns = deadline_ns;
    client->transport_error = 0;
    if (deadline_ns <= phipia_monotonic_ns()) {
        client->transport_error = -(long)PHIPIA_ETIMEDOUT;
        client->status = PHIPIA_TLS_IO;
        return false;
    }
    return true;
}

static int transport_read(void *context, unsigned char *buffer, size_t length)
{
    struct phipia_tls_client *client = context;
    long count = deadline_error(client);

    if (count == 0) {
        count = phipia_stream_read(client->stream, buffer, length,
            client->deadline_ns);
    }
    if (count <= 0 || count > INT_MAX || (size_t)count > length) {
        if (count == -(long)PHIPIA_EIO &&
            client->received_transport_bytes) {
            /* A reset after record bytes is an authenticated-stream
             * truncation; a reset before any bytes remains a reset. */
            count = -(long)PHIPIA_EPIPE;
        }
        client->transport_error = count == 0 ? -(long)PHIPIA_EPIPE : count;
        return -1;
    }
    client->received_transport_bytes = true;
    return (int)count;
}

static int transport_write(void *context, const unsigned char *buffer,
    size_t length)
{
    struct phipia_tls_client *client = context;
    long count = deadline_error(client);

    if (count == 0) {
        count = phipia_stream_write(client->stream, buffer, length,
            client->deadline_ns);
    }
    if (count <= 0 || count > INT_MAX || (size_t)count > length) {
        client->transport_error = count == 0 ? -(long)PHIPIA_EPIPE : count;
        return -1;
    }
    return (int)count;
}

static enum phipia_tls_status drive_handshake(struct phipia_tls_client *client)
{
    for (size_t step = 0U; step < PHIPIA_TLS_MAX_HANDSHAKE_STEPS; ++step) {
        const unsigned state = br_ssl_engine_current_state(&client->ssl.eng);
        size_t length = 0U;

        if ((state & BR_SSL_CLOSED) != 0U) {
            return PHIPIA_TLS_HANDSHAKE;
        }
        if ((state & (BR_SSL_SENDAPP | BR_SSL_RECVAPP)) != 0U) {
            return PHIPIA_TLS_OK;
        }
        if ((state & BR_SSL_SENDREC) != 0U) {
            unsigned char *data = br_ssl_engine_sendrec_buf(
                &client->ssl.eng, &length);
            int count;

            if (data == NULL || length == 0U) {
                return PHIPIA_TLS_HANDSHAKE;
            }
            count = transport_write(client, data, length);
            if (count <= 0) {
                return PHIPIA_TLS_TRANSPORT;
            }
            br_ssl_engine_sendrec_ack(&client->ssl.eng, (size_t)count);
            continue;
        }
        if ((state & BR_SSL_RECVREC) != 0U) {
            unsigned char *data = br_ssl_engine_recvrec_buf(
                &client->ssl.eng, &length);
            int count;

            if (data == NULL || length == 0U) {
                return PHIPIA_TLS_HANDSHAKE;
            }
            count = transport_read(client, data, length);
            if (count <= 0) {
                return PHIPIA_TLS_TRANSPORT;
            }
            br_ssl_engine_recvrec_ack(&client->ssl.eng, (size_t)count);
            continue;
        }
        return PHIPIA_TLS_HANDSHAKE;
    }
    return PHIPIA_TLS_HANDSHAKE;
}

static void release_client(struct phipia_tls_client *client,
    uint64_t deadline_ns)
{
    if (client == NULL) {
        return;
    }
    if (client->stream != PHIPIA_HANDLE_INVALID) {
        (void)phipia_stream_shutdown(client->stream,
            PHIPIA_SHUTDOWN_WRITE | PHIPIA_SHUTDOWN_READ, deadline_ns);
        (void)phipia_handle_close(client->stream);
        client->stream = PHIPIA_HANDLE_INVALID;
    }
    if (client->anchor_storage != NULL) {
        secure_zero(client->anchor_storage, client->anchor_storage_length);
        free(client->anchor_storage);
        client->anchor_storage = NULL;
    }
    secure_zero(client, sizeof(*client));
    free(client);
}

enum phipia_tls_status phipia_tls_client_open_diagnostic(
    const struct phipia_tls_client_config *config,
    struct phipia_tls_diagnostics *diagnostics,
    struct phipia_tls_client **result)
{
    struct phipia_tls_client *client;
    struct phipia_ipv4_endpoint endpoint;
    unsigned char entropy[PHIPIA_TLS_ENTROPY_BYTES];
    size_t hostname_length;
    long realtime;
    long resolved;
    long opened;
    long connected;
    enum phipia_tls_status status;

    diagnostics_clear(diagnostics);
    if (result == NULL) {
        return PHIPIA_TLS_ARGUMENT;
    }
    *result = NULL;
    if (config == NULL || config->reserved != 0U || config->port == 0U ||
        !hostname_valid(config->hostname) ||
        config->trust_anchors == NULL || config->trust_anchor_count == 0U ||
        config->trust_anchor_count > PHIPIA_TLS_MAX_TRUST_ANCHORS ||
        config->deadline_ns <= phipia_monotonic_ns()) {
        return PHIPIA_TLS_ARGUMENT;
    }
    for (size_t index = 0U; index < config->trust_anchor_count; ++index) {
        if (!trust_anchor_valid(&config->trust_anchors[index])) {
            return PHIPIA_TLS_TRUST;
        }
    }
    realtime = phipia_realtime_seconds();
    if (realtime < 0 || (uint64_t)realtime < PHIPIA_TLS_CLOCK_MIN ||
        (uint64_t)realtime > PHIPIA_TLS_CLOCK_MAX ||
        (uint64_t)realtime / UINT64_C(86400) >
            UINT32_MAX - PHIPIA_TLS_UNIX_EPOCH_DAYS) {
        return PHIPIA_TLS_CLOCK;
    }
    client = calloc(1U, sizeof(*client));
    if (client == NULL) {
        return PHIPIA_TLS_NO_MEMORY;
    }
    client->stream = PHIPIA_HANDLE_INVALID;
    client->deadline_ns = config->deadline_ns;
    hostname_length = strlen(config->hostname);
    (void)memcpy(client->hostname, config->hostname, hostname_length + 1U);
    if (!copy_trust_anchors(client, config->trust_anchors,
            config->trust_anchor_count)) {
        release_client(client, config->deadline_ns);
        return PHIPIA_TLS_NO_MEMORY;
    }
    if (phipia_random_strong(entropy, sizeof(entropy)) !=
            (long)sizeof(entropy)) {
        secure_zero(entropy, sizeof(entropy));
        release_client(client, config->deadline_ns);
        return PHIPIA_TLS_ENTROPY;
    }
    resolved = phipia_dns_resolve(client->hostname, config->deadline_ns);
    if (resolved <= 0 || (uint64_t)resolved > UINT32_MAX) {
        secure_zero(entropy, sizeof(entropy));
        release_client(client, config->deadline_ns);
        return PHIPIA_TLS_DNS;
    }
    opened = phipia_stream_open();
    if (opened < 0) {
        secure_zero(entropy, sizeof(entropy));
        client->transport_error = opened;
        diagnostics_capture(client, diagnostics);
        release_client(client, config->deadline_ns);
        return PHIPIA_TLS_TRANSPORT;
    }
    client->stream = (phipia_handle_t)opened;
    endpoint = (struct phipia_ipv4_endpoint){
        (uint32_t)resolved, config->port, 0U};
    connected = phipia_stream_connect(client->stream, &endpoint,
        config->deadline_ns);
    if (connected < 0) {
        secure_zero(entropy, sizeof(entropy));
        client->transport_error = connected;
        diagnostics_capture(client, diagnostics);
        release_client(client, config->deadline_ns);
        return PHIPIA_TLS_TRANSPORT;
    }

    br_ssl_client_init_full(&client->ssl, &client->x509,
        client->anchors, config->trust_anchor_count);
    br_ssl_engine_set_versions(&client->ssl.eng, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(&client->ssl.eng, suites,
        sizeof(suites) / sizeof(suites[0]));
    br_ssl_engine_set_all_flags(&client->ssl.eng, BR_OPT_NO_RENEGOTIATION);
    br_ssl_engine_set_buffer(&client->ssl.eng, client->buffer,
        sizeof(client->buffer), 1);
    br_x509_minimal_set_minrsa(&client->x509, 256);
    br_x509_minimal_set_time(&client->x509,
        (uint32_t)((uint64_t)realtime / UINT64_C(86400) +
            PHIPIA_TLS_UNIX_EPOCH_DAYS),
        (uint32_t)((uint64_t)realtime % UINT64_C(86400)));
    br_ssl_engine_inject_entropy(&client->ssl.eng, entropy, sizeof(entropy));
    secure_zero(entropy, sizeof(entropy));
    if (!br_ssl_client_reset(&client->ssl, client->hostname, 0)) {
        diagnostics_capture(client, diagnostics);
        release_client(client, config->deadline_ns);
        return PHIPIA_TLS_HANDSHAKE;
    }
    status = drive_handshake(client);
    if (status != PHIPIA_TLS_OK) {
        client->status = status;
        diagnostics_capture(client, diagnostics);
        release_client(client, config->deadline_ns);
        return status;
    }
    br_sslio_init(&client->io, &client->ssl.eng, transport_read, client,
        transport_write, client);
    client->tls_ready = true;
    client->status = PHIPIA_TLS_OK;
    *result = client;
    return PHIPIA_TLS_OK;
}

enum phipia_tls_status phipia_tls_client_open(
    const struct phipia_tls_client_config *config,
    struct phipia_tls_client **result)
{
    return phipia_tls_client_open_diagnostic(config, NULL, result);
}

long phipia_tls_client_read(struct phipia_tls_client *client, void *buffer,
    size_t length, uint64_t deadline_ns)
{
    int count;

    if (client == NULL || !client->tls_ready || client->peer_closed ||
        (buffer == NULL && length != 0U) || length > INT_MAX) {
        return -1;
    }
    if (!set_operation_deadline(client, deadline_ns)) {
        return -1;
    }
    if (length == 0U) {
        return 0;
    }
    count = br_sslio_read(&client->io, buffer, length);
    if (count < 0) {
        if (client->transport_error == 0 &&
            br_ssl_engine_last_error(&client->ssl.eng) == BR_ERR_OK) {
            client->peer_closed = true;
            return 0;
        }
        client->status = PHIPIA_TLS_IO;
        return -1;
    }
    return count;
}

long phipia_tls_client_write(struct phipia_tls_client *client,
    const void *buffer, size_t length, uint64_t deadline_ns)
{
    int count;

    if (client == NULL || !client->tls_ready || client->peer_closed ||
        (buffer == NULL && length != 0U) || length > INT_MAX) {
        return -1;
    }
    if (!set_operation_deadline(client, deadline_ns)) {
        return -1;
    }
    if (length == 0U) {
        return 0;
    }
    count = br_sslio_write(&client->io, buffer, length);
    if (count < 0) {
        client->status = PHIPIA_TLS_IO;
        return -1;
    }
    return count;
}

enum phipia_tls_status phipia_tls_client_flush(
    struct phipia_tls_client *client, uint64_t deadline_ns)
{
    if (client == NULL || !client->tls_ready || client->peer_closed) {
        return PHIPIA_TLS_ARGUMENT;
    }
    if (!set_operation_deadline(client, deadline_ns)) {
        return PHIPIA_TLS_IO;
    }
    if (br_sslio_flush(&client->io) != 0) {
        client->status = PHIPIA_TLS_IO;
    }
    return client->status;
}

long phipia_tls_client_cancel(struct phipia_tls_client *client)
{
    long result;

    if (client == NULL || client->stream == PHIPIA_HANDLE_INVALID) {
        return -(long)PHIPIA_EINVAL;
    }
    __atomic_store_n(&client->canceled, true, __ATOMIC_RELEASE);
    result = phipia_network_cancel(client->stream);
    return result;
}

static enum phipia_tls_status close_client(
    struct phipia_tls_client *client, uint64_t deadline_ns,
    struct phipia_tls_diagnostics *diagnostics)
{
    enum phipia_tls_status status;

    diagnostics_clear(diagnostics);
    if (client == NULL) {
        return PHIPIA_TLS_ARGUMENT;
    }
    status = client->status;
    if (client->tls_ready &&
        !__atomic_load_n(&client->canceled, __ATOMIC_ACQUIRE)) {
        if (!set_operation_deadline(client, deadline_ns)) {
            status = PHIPIA_TLS_IO;
        } else if (br_sslio_close(&client->io) == 0 &&
                status == PHIPIA_TLS_OK) {
            status = PHIPIA_TLS_CLOSE;
        }
    }
    diagnostics_capture(client, diagnostics);
    release_client(client, deadline_ns);
    return status;
}

enum phipia_tls_status phipia_tls_client_close(
    struct phipia_tls_client *client, uint64_t deadline_ns)
{
    return close_client(client, deadline_ns, NULL);
}

enum phipia_tls_status phipia_tls_client_status(
    const struct phipia_tls_client *client)
{
    return client == NULL ? PHIPIA_TLS_ARGUMENT : client->status;
}

int phipia_tls_client_bearssl_error(const struct phipia_tls_client *client)
{
    return client == NULL ? -1 : br_ssl_engine_last_error(&client->ssl.eng);
}

long phipia_tls_client_transport_error(const struct phipia_tls_client *client)
{
    return client == NULL ? 0 : client->transport_error;
}

const char *phipia_tls_status_string(enum phipia_tls_status status)
{
    static const char *const names[] = {
        "ok", "invalid TLS argument", "TLS allocation failed",
        "invalid or empty TLS trust store", "realtime clock unavailable",
        "kernel entropy unavailable", "DNS resolution failed",
        "TCP transport failed", "TLS handshake or authentication failed",
        "TLS application I/O failed", "authenticated TLS close failed"
    };

    return (unsigned)status < sizeof(names) / sizeof(names[0]) ?
        names[status] : "unknown TLS status";
}

static enum phipia_https_status transport_https_status(long error,
    enum phipia_https_status fallback)
{
    if (error == -(long)PHIPIA_ETIMEDOUT) {
        return PHIPIA_HTTPS_TIMEOUT;
    }
    if (error == -(long)PHIPIA_ECANCELED) {
        return PHIPIA_HTTPS_CANCELED;
    }
    if (error == -(long)PHIPIA_EIO) {
        return PHIPIA_HTTPS_RESET;
    }
    if (error == -(long)PHIPIA_EPIPE) {
        return PHIPIA_HTTPS_TRUNCATED;
    }
    return fallback;
}

static enum phipia_https_status handshake_https_status(
    enum phipia_tls_status status, const struct phipia_tls_diagnostics *details)
{
    if (status == PHIPIA_TLS_ARGUMENT) {
        return PHIPIA_HTTPS_ARGUMENT;
    }
    if (status == PHIPIA_TLS_NO_MEMORY) {
        return PHIPIA_HTTPS_NO_MEMORY;
    }
    if (status == PHIPIA_TLS_TRUST) {
        return PHIPIA_HTTPS_TRUST;
    }
    if (status == PHIPIA_TLS_CLOCK) {
        return PHIPIA_HTTPS_CLOCK;
    }
    if (status == PHIPIA_TLS_ENTROPY) {
        return PHIPIA_HTTPS_ENTROPY;
    }
    if (status == PHIPIA_TLS_DNS) {
        return PHIPIA_HTTPS_DNS;
    }
    if (details->bearssl_error == BR_ERR_X509_BAD_SERVER_NAME ||
        details->bearssl_error == BR_ERR_X509_DN_MISMATCH) {
        return PHIPIA_HTTPS_HOSTNAME;
    }
    if (details->bearssl_error == BR_ERR_X509_EXPIRED ||
        details->bearssl_error == BR_ERR_X509_BAD_TIME ||
        details->bearssl_error == BR_ERR_X509_TIME_UNKNOWN) {
        return PHIPIA_HTTPS_CERTIFICATE_TIME;
    }
    if (details->bearssl_error == BR_ERR_X509_NOT_TRUSTED ||
        details->bearssl_error == BR_ERR_X509_BAD_SIGNATURE ||
        details->bearssl_error == BR_ERR_X509_NOT_CA) {
        return PHIPIA_HTTPS_AUTHENTICATION;
    }
    if (status == PHIPIA_TLS_TRANSPORT) {
        return transport_https_status(details->transport_error,
            PHIPIA_HTTPS_TRANSPORT);
    }
    return PHIPIA_HTTPS_HANDSHAKE;
}

static enum phipia_https_status client_io_status(
    struct phipia_tls_client *client, enum phipia_https_status fallback)
{
    return transport_https_status(
        phipia_tls_client_transport_error(client), fallback);
}

static void abort_client(struct phipia_tls_client *client,
    uint64_t deadline_ns)
{
    if (client != NULL) {
        (void)phipia_tls_client_cancel(client);
        release_client(client, deadline_ns);
    }
}

static bool path_valid(const char *path, size_t *length)
{
    size_t used;

    if (path == NULL || length == NULL) {
        return false;
    }
    used = strlen(path);
    if (used == 0U || used > PHIPIA_HTTPS_MAX_PATH_BYTES || path[0] != '/') {
        return false;
    }
    for (size_t index = 0U; index < used; ++index) {
        const unsigned char value = (unsigned char)path[index];

        if (value < 0x21U || value > 0x7EU || value == '#') {
            return false;
        }
    }
    *length = used;
    return true;
}

static bool append_bytes(char *output, size_t capacity, size_t *used,
    const char *bytes, size_t length)
{
    if (*used > capacity || length > capacity - *used) {
        return false;
    }
    (void)memcpy(output + *used, bytes, length);
    *used += length;
    return true;
}

static bool append_literal(char *output, size_t capacity, size_t *used,
    const char *literal)
{
    return append_bytes(output, capacity, used, literal, strlen(literal));
}

static bool append_port(char *output, size_t capacity, size_t *used,
    uint16_t port)
{
    char digits[5];
    size_t count = 0U;
    uint16_t value = port;

    do {
        digits[count++] = (char)('0' + value % 10U);
        value = (uint16_t)(value / 10U);
    } while (value != 0U);
    if (*used > capacity || count > capacity - *used) {
        return false;
    }
    while (count != 0U) {
        output[(*used)++] = digits[--count];
    }
    return true;
}

static bool make_http_request(const struct phipia_https_stream_request *request,
    char *output, size_t capacity, size_t *length)
{
    size_t path_length;
    size_t used = 0U;

    if (!path_valid(request->path, &path_length) ||
        !append_literal(output, capacity, &used, "GET ") ||
        !append_bytes(output, capacity, &used, request->path, path_length) ||
        !append_literal(output, capacity, &used, " HTTP/1.1\r\nHost: ") ||
        !append_literal(output, capacity, &used, request->hostname)) {
        return false;
    }
    if (request->port != 443U &&
        (!append_literal(output, capacity, &used, ":") ||
         !append_port(output, capacity, &used, request->port))) {
        return false;
    }
    if (!append_literal(output, capacity, &used,
            "\r\nAccept: application/octet-stream\r\n"
            "Accept-Encoding: identity\r\nConnection: close\r\n\r\n")) {
        return false;
    }
    *length = used;
    return true;
}

static bool ascii_equal_case(const unsigned char *left, size_t length,
    const char *right)
{
    if (strlen(right) != length) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        unsigned char value = left[index];

        if (value >= 'A' && value <= 'Z') {
            value = (unsigned char)(value + ('a' - 'A'));
        }
        if (value != (unsigned char)right[index]) {
            return false;
        }
    }
    return true;
}

static bool decimal_size(const unsigned char *text, size_t length,
    size_t *value)
{
    size_t result = 0U;

    if (length == 0U) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char digit = text[index];

        if (digit < '0' || digit > '9' ||
            result > (SIZE_MAX - (size_t)(digit - '0')) / 10U) {
            return false;
        }
        result = result * 10U + (size_t)(digit - '0');
    }
    *value = result;
    return true;
}

static enum phipia_https_status parse_http_headers(
    const unsigned char *header, size_t length, size_t body_capacity,
    struct phipia_https_response *response)
{
    size_t line_start = 0U;
    size_t line_end = 0U;
    bool content_length_seen = false;

    for (size_t index = 0U; index < length; ++index) {
        const unsigned char value = header[index];

        if ((value < 0x20U && value != '\r' && value != '\n' &&
                value != '\t') || value > 0x7EU ||
            (value == '\r' &&
             (index + 1U >= length || header[index + 1U] != '\n')) ||
            (value == '\n' && (index == 0U || header[index - 1U] != '\r'))) {
            return PHIPIA_HTTPS_HTTP_HEADERS;
        }
    }
    while (line_end + 1U < length &&
        !(header[line_end] == '\r' && header[line_end + 1U] == '\n')) {
        ++line_end;
    }
    if (line_end + 1U >= length || line_end < 12U ||
        memcmp(header, "HTTP/1.1 ", 9U) != 0 ||
        header[9] < '0' || header[9] > '9' ||
        header[10] < '0' || header[10] > '9' ||
        header[11] < '0' || header[11] > '9' ||
        (line_end > 12U && header[12] != ' ')) {
        return PHIPIA_HTTPS_HTTP_VERSION;
    }
    response->status_code = (uint16_t)((header[9] - '0') * 100U +
        (header[10] - '0') * 10U + (header[11] - '0'));
    if (response->status_code != 200U) {
        return PHIPIA_HTTPS_HTTP_STATUS;
    }
    line_start = line_end + 2U;
    while (line_start + 1U < length) {
        size_t colon;
        size_t value_start;
        size_t value_end;

        if (header[line_start] == '\r' && header[line_start + 1U] == '\n') {
            break;
        }
        line_end = line_start;
        while (line_end + 1U < length &&
            !(header[line_end] == '\r' && header[line_end + 1U] == '\n')) {
            ++line_end;
        }
        if (line_end + 1U >= length || line_end == line_start ||
            header[line_start] == ' ' || header[line_start] == '\t') {
            return PHIPIA_HTTPS_HTTP_HEADERS;
        }
        colon = line_start;
        while (colon < line_end && header[colon] != ':') {
            const unsigned char value = header[colon];

            if (!((value >= 'a' && value <= 'z') ||
                    (value >= 'A' && value <= 'Z') ||
                    (value >= '0' && value <= '9') || value == '-')) {
                return PHIPIA_HTTPS_HTTP_HEADERS;
            }
            ++colon;
        }
        if (colon == line_start || colon == line_end) {
            return PHIPIA_HTTPS_HTTP_HEADERS;
        }
        value_start = colon + 1U;
        while (value_start < line_end &&
            (header[value_start] == ' ' || header[value_start] == '\t')) {
            ++value_start;
        }
        value_end = line_end;
        while (value_end > value_start &&
            (header[value_end - 1U] == ' ' ||
             header[value_end - 1U] == '\t')) {
            --value_end;
        }
        if (ascii_equal_case(header + line_start, colon - line_start,
                "content-length")) {
            if (content_length_seen ||
                !decimal_size(header + value_start, value_end - value_start,
                    &response->content_length)) {
                return PHIPIA_HTTPS_HTTP_HEADERS;
            }
            content_length_seen = true;
        } else if (ascii_equal_case(header + line_start,
                colon - line_start, "transfer-encoding")) {
            return PHIPIA_HTTPS_HTTP_HEADERS;
        } else if (ascii_equal_case(header + line_start,
                colon - line_start, "content-encoding") &&
            !ascii_equal_case(header + value_start, value_end - value_start,
                "identity")) {
            return PHIPIA_HTTPS_HTTP_HEADERS;
        }
        line_start = line_end + 2U;
    }
    if (!content_length_seen) {
        return PHIPIA_HTTPS_CONTENT_LENGTH_REQUIRED;
    }
    if (response->content_length > body_capacity) {
        return PHIPIA_HTTPS_CONTENT_TOO_LARGE;
    }
    return PHIPIA_HTTPS_OK;
}

static void capture_response_diagnostics(struct phipia_tls_client *client,
    struct phipia_https_response *response)
{
    response->bearssl_error = phipia_tls_client_bearssl_error(client);
    response->transport_error = phipia_tls_client_transport_error(client);
}

enum phipia_https_status phipia_https_get_stream(
    const struct phipia_https_stream_request *request,
    struct phipia_https_response *response)
{
    struct phipia_tls_client_config tls_config;
    struct phipia_tls_diagnostics diagnostics;
    struct phipia_tls_client *client = NULL;
    unsigned char header[PHIPIA_HTTPS_MAX_HEADER_BYTES];
    unsigned char body[PHIPIA_HTTPS_BODY_CHUNK_BYTES];
    char wire_request[PHIPIA_HTTPS_REQUEST_BYTES];
    size_t request_length;
    size_t sent = 0U;
    size_t header_length = 0U;
    unsigned delimiter = 0U;
    enum phipia_tls_status tls_status;
    enum phipia_https_status status;

    if (response == NULL) {
        return PHIPIA_HTTPS_ARGUMENT;
    }
    (void)memset(response, 0, sizeof(*response));
    if (request == NULL || request->reserved != 0U ||
        !hostname_valid(request->hostname) || request->port == 0U ||
        request->trust_anchors == NULL || request->trust_anchor_count == 0U ||
        request->deadline_ns <= phipia_monotonic_ns() ||
        request->write_body == NULL ||
        !make_http_request(request, wire_request, sizeof(wire_request),
            &request_length)) {
        return PHIPIA_HTTPS_ARGUMENT;
    }
    tls_config = (struct phipia_tls_client_config){
        request->hostname, request->port, 0U, request->trust_anchors,
        request->trust_anchor_count, request->deadline_ns};
    tls_status = phipia_tls_client_open_diagnostic(&tls_config, &diagnostics,
        &client);
    if (tls_status != PHIPIA_TLS_OK) {
        response->bearssl_error = diagnostics.bearssl_error;
        response->transport_error = diagnostics.transport_error;
        return handshake_https_status(tls_status, &diagnostics);
    }
    while (sent < request_length) {
        const long count = phipia_tls_client_write(client,
            wire_request + sent, request_length - sent, request->deadline_ns);

        if (count <= 0) {
            status = client_io_status(client, PHIPIA_HTTPS_IO);
            capture_response_diagnostics(client, response);
            abort_client(client, request->deadline_ns);
            return status;
        }
        sent += (size_t)count;
    }
    if (phipia_tls_client_flush(client, request->deadline_ns) !=
            PHIPIA_TLS_OK) {
        status = client_io_status(client, PHIPIA_HTTPS_IO);
        capture_response_diagnostics(client, response);
        abort_client(client, request->deadline_ns);
        return status;
    }
    while (header_length < sizeof(header) && delimiter != 4U) {
        long count = phipia_tls_client_read(client, &header[header_length], 1U,
            request->deadline_ns);

        if (count <= 0) {
            status = count == 0 ? PHIPIA_HTTPS_TRUNCATED :
                client_io_status(client, PHIPIA_HTTPS_IO);
            capture_response_diagnostics(client, response);
            abort_client(client, request->deadline_ns);
            return status;
        }
        if ((delimiter == 0U || delimiter == 2U) &&
            header[header_length] == '\r') {
            ++delimiter;
        } else if ((delimiter == 1U || delimiter == 3U) &&
            header[header_length] == '\n') {
            ++delimiter;
        } else {
            delimiter = header[header_length] == '\r' ? 1U : 0U;
        }
        ++header_length;
    }
    if (delimiter != 4U) {
        abort_client(client, request->deadline_ns);
        return PHIPIA_HTTPS_HTTP_HEADERS;
    }
    status = parse_http_headers(header, header_length,
        request->body_limit, response);
    if (status != PHIPIA_HTTPS_OK) {
        capture_response_diagnostics(client, response);
        abort_client(client, request->deadline_ns);
        return status;
    }
    while (response->body_length < response->content_length) {
        size_t remaining = response->content_length - response->body_length;
        size_t requested = remaining < sizeof(body) ? remaining : sizeof(body);
        long count = phipia_tls_client_read(client,
            body, requested, request->deadline_ns);

        if (count <= 0) {
            if (count == 0 || phipia_tls_client_transport_error(client) ==
                    -(long)PHIPIA_EPIPE) {
                status = PHIPIA_HTTPS_BODY_TRUNCATED;
            } else {
                status = client_io_status(client,
                    PHIPIA_HTTPS_BODY_TRUNCATED);
            }
            capture_response_diagnostics(client, response);
            abort_client(client, request->deadline_ns);
            return status;
        }
        if (request->write_body(request->write_context, body,
                (size_t)count) != count) {
            capture_response_diagnostics(client, response);
            abort_client(client, request->deadline_ns);
            return PHIPIA_HTTPS_BODY_WRITE;
        }
        response->body_length += (size_t)count;
    }
    {
        unsigned char extra;
        const long count = phipia_tls_client_read(client, &extra, 1U,
            request->deadline_ns);

        if (count > 0) {
            capture_response_diagnostics(client, response);
            abort_client(client, request->deadline_ns);
            return PHIPIA_HTTPS_BODY_EXTRA;
        }
        if (count < 0) {
            status = client_io_status(client, PHIPIA_HTTPS_TRUNCATED);
            capture_response_diagnostics(client, response);
            abort_client(client, request->deadline_ns);
            return status;
        }
    }
    tls_status = close_client(client, request->deadline_ns, &diagnostics);
    response->bearssl_error = diagnostics.bearssl_error;
    response->transport_error = diagnostics.transport_error;
    return tls_status == PHIPIA_TLS_OK ? PHIPIA_HTTPS_OK :
        transport_https_status(diagnostics.transport_error,
            PHIPIA_HTTPS_CLOSE);
}

struct https_buffer_sink {
    unsigned char *bytes;
    size_t capacity;
    size_t used;
};

static long write_buffer_body(
    void *context,
    const void *bytes,
    size_t byte_count
)
{
    struct https_buffer_sink *sink = context;
    if (sink == NULL || (bytes == NULL && byte_count != 0U) ||
        sink->used > sink->capacity || byte_count > sink->capacity - sink->used ||
        byte_count > LONG_MAX) {
        return -1;
    }
    (void)memcpy(sink->bytes + sink->used, bytes, byte_count);
    sink->used += byte_count;
    return (long)byte_count;
}

enum phipia_https_status phipia_https_get(
    const struct phipia_https_request *request,
    struct phipia_https_response *response)
{
    struct https_buffer_sink sink;
    struct phipia_https_stream_request stream;
    if (response == NULL) {
        return PHIPIA_HTTPS_ARGUMENT;
    }
    (void)memset(response, 0, sizeof(*response));
    if (request == NULL ||
        (request->body == NULL && request->body_capacity != 0U)) {
        return PHIPIA_HTTPS_ARGUMENT;
    }
    sink = (struct https_buffer_sink){
        request->body, request->body_capacity, 0U
    };
    stream = (struct phipia_https_stream_request){
        request->hostname, request->port, request->reserved, request->path,
        request->trust_anchors, request->trust_anchor_count,
        request->deadline_ns, request->body_capacity, write_buffer_body, &sink
    };
    return phipia_https_get_stream(&stream, response);
}

const char *phipia_https_status_string(enum phipia_https_status status)
{
    static const char *const names[] = {
        "ok", "invalid HTTPS argument", "HTTPS allocation failed",
        "invalid or empty immutable trust anchors",
        "realtime clock is unavailable or implausible",
        "kernel entropy unavailable", "DNS resolution failed",
        "TCP transport failed", "deadline expired", "operation canceled",
        "TCP connection reset", "TLS record stream truncated",
        "certificate hostname mismatch", "certificate time invalid",
        "certificate authentication failed", "TLS handshake failed",
        "TLS application I/O failed", "HTTP version/status line refused",
        "HTTP status is not 200", "malformed or unsupported HTTP headers",
        "Content-Length is required", "Content-Length exceeds output bound",
        "HTTP body shorter than Content-Length",
        "HTTP body longer than Content-Length",
        "authenticated TLS close failed",
        "HTTPS body sink refused bytes"
    };

    return (unsigned)status < sizeof(names) / sizeof(names[0]) ?
        names[status] : "unknown HTTPS status";
}
