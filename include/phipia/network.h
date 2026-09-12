/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_NETWORK_H
#define PHIPIA_NETWORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/virtio_net.h>

#define NETWORK_ABI_VERSION UINT16_C(1)
#define NETWORK_MAX_NICS 1U
#define NETWORK_ARP_CACHE_SIZE 8U
#define NETWORK_DNS_CACHE_SIZE 8U
#define NETWORK_MAX_UDP_SOCKETS 8U
#define NETWORK_MAX_TCP_CONNECTIONS 8U
#define NETWORK_MAX_ENDPOINTS \
    (NETWORK_MAX_UDP_SOCKETS + NETWORK_MAX_TCP_CONNECTIONS)
#define NETWORK_MAX_TIMERS 32U
#define NETWORK_MAX_POLL_HANDLES 8U
#define NETWORK_UDP_QUEUE_DEPTH 4U
#define NETWORK_MAX_UDP_DATAGRAM 512U
#define NETWORK_TCP_RX_BYTES 8192U
#define NETWORK_TCP_TX_BYTES 1460U
#define NETWORK_MAX_HOSTNAME 253U
#define NETWORK_MAX_DNS_MESSAGE 512U
#define NETWORK_HTTP_HEADER_BYTES 4096U
#define NETWORK_HTTP_MAX_HEADERS 32U
#define NETWORK_HTTP_MAX_REDIRECTS 4U
#define NETWORK_HTTP_MAX_DOWNLOAD_BYTES UINT32_C(16777216)
#define NETWORK_DEFAULT_CONNECT_TIMEOUT_NS UINT64_C(3000000000)
#define NETWORK_DEFAULT_READ_TIMEOUT_NS UINT64_C(3000000000)
#define NETWORK_DEFAULT_OPERATION_TIMEOUT_NS UINT64_C(5000000000)
#define NETWORK_TCP_RETRANSMISSION_LIMIT 4U
#define NETWORK_TCP_MAX_BACKLOG 4U
#define NETWORK_PING_MAX_COUNT 8U

typedef uint64_t network_handle;

enum network_status {
    NETWORK_STATUS_OK = 0,
    NETWORK_STATUS_UNAVAILABLE,
    NETWORK_STATUS_LINK_DOWN,
    NETWORK_STATUS_UNCONFIGURED,
    NETWORK_STATUS_ALREADY_INITIALIZED,
    NETWORK_STATUS_NOT_INITIALIZED,
    NETWORK_STATUS_NULL_ARGUMENT,
    NETWORK_STATUS_INVALID_ARGUMENT,
    NETWORK_STATUS_RANGE,
    NETWORK_STATUS_TIMEOUT,
    NETWORK_STATUS_CANCELLED,
    NETWORK_STATUS_WOULD_BLOCK,
    NETWORK_STATUS_NO_RESOURCES,
    NETWORK_STATUS_STALE_HANDLE,
    NETWORK_STATUS_WRONG_OWNER,
    NETWORK_STATUS_WRONG_MODE,
    NETWORK_STATUS_ALREADY_BOUND,
    NETWORK_STATUS_PORT_IN_USE,
    NETWORK_STATUS_RESET,
    NETWORK_STATUS_MALFORMED,
    NETWORK_STATUS_CHECKSUM,
    NETWORK_STATUS_FRAGMENTED,
    NETWORK_STATUS_UNSUPPORTED,
    NETWORK_STATUS_UNREACHABLE,
    NETWORK_STATUS_DHCP_NAK,
    NETWORK_STATUS_DNS_FAILURE,
    NETWORK_STATUS_CONNECTION_RESET,
    NETWORK_STATUS_CONNECTION_CLOSED,
    NETWORK_STATUS_HTTP_FAILURE,
    NETWORK_STATUS_FILESYSTEM,
    NETWORK_STATUS_TOO_LARGE,
    NETWORK_STATUS_COUNT
};

enum network_configuration_source {
    NETWORK_CONFIGURATION_NONE = 0,
    NETWORK_CONFIGURATION_STATIC,
    NETWORK_CONFIGURATION_DHCP
};

enum network_readiness {
    NETWORK_READY_NONE = 0U,
    NETWORK_READY_CONNECTED = 1U << 0,
    NETWORK_READY_READABLE = 1U << 1,
    NETWORK_READY_WRITABLE = 1U << 2,
    NETWORK_READY_PEER_CLOSED = 1U << 3,
    NETWORK_READY_ERROR = 1U << 4,
    NETWORK_READY_CANCELLED = 1U << 5,
    NETWORK_READY_TIMEOUT = 1U << 6,
    NETWORK_READY_ACCEPTABLE = 1U << 7
};

struct network_ipv4_configuration {
    uint32_t address;
    uint32_t subnet_mask;
    uint32_t gateway;
    uint32_t dns_server;
    uint32_t dhcp_server;
    uint64_t lease_expires_ns;
    uint64_t renewal_ns;
    uint64_t rebinding_ns;
    uint64_t generation;
    enum network_configuration_source source;
    bool configured;
};

struct network_poll_request {
    network_handle handle;
    uint32_t interests;
};

struct network_poll_result {
    network_handle handle;
    uint32_t ready;
    enum network_status error;
};

struct network_ping_result {
    uint32_t destination;
    uint32_t sent;
    uint32_t received;
    uint64_t round_trip_ns[NETWORK_PING_MAX_COUNT];
    enum network_status result[NETWORK_PING_MAX_COUNT];
};

struct network_http_result {
    uint16_t status_code;
    uint32_t body_bytes;
    uint32_t redirects;
    uint64_t elapsed_ns;
    uint64_t synchronize_ns;
    bool chunked;
    bool synchronized;
};

struct network_statistics {
    uint64_t ethernet_accepted;
    uint64_t ethernet_unsupported;
    uint64_t arp_accepted;
    uint64_t arp_conflicts;
    uint64_t ipv4_accepted;
    uint64_t ipv4_checksum_failures;
    uint64_t ipv4_fragments;
    uint64_t icmp_accepted;
    uint64_t udp_accepted;
    uint64_t dns_accepted;
    uint64_t tcp_accepted;
    uint64_t tcp_retransmissions;
    uint64_t tcp_passive_opens;
    uint64_t tcp_refusals;
    uint64_t arp_deferred;
    uint64_t malformed_packets;
    uint64_t socket_exhaustion;
    uint64_t timer_exhaustion;
    uint64_t cancellations;
    uint64_t resets;
};

struct network_state {
    struct virtio_net_state device;
    struct network_ipv4_configuration configuration;
    struct network_statistics statistics;
    size_t arp_entries;
    size_t dns_entries;
    size_t udp_sockets;
    size_t tcp_connections;
    size_t tcp_listeners;
    size_t timers;
    bool active;
};

enum network_status network_initialize(void);
enum network_status network_shutdown(void);
enum network_status network_service(void);
enum network_status network_configure_static(
    uint32_t address,
    uint32_t subnet_mask,
    uint32_t gateway,
    uint32_t dns_server
);
enum network_status network_start_dhcp(uint64_t timeout_ns);
enum network_status network_resolve(
    const char *hostname,
    uint32_t *address,
    uint64_t timeout_ns
);
enum network_status network_ping(
    uint32_t destination,
    uint32_t count,
    uint64_t timeout_ns,
    struct network_ping_result *result
);

enum network_status network_udp_open(uint64_t owner, network_handle *handle);
enum network_status network_udp_bind(
    uint64_t owner,
    network_handle handle,
    uint16_t port
);
enum network_status network_udp_send(
    uint64_t owner,
    network_handle handle,
    uint32_t destination,
    uint16_t port,
    const uint8_t *bytes,
    size_t length,
    uint64_t timeout_ns
);
enum network_status network_udp_receive(
    uint64_t owner,
    network_handle handle,
    uint32_t *source,
    uint16_t *port,
    uint8_t *bytes,
    size_t capacity,
    size_t *length,
    uint64_t timeout_ns
);

enum network_status network_tcp_open(uint64_t owner, network_handle *handle);
enum network_status network_tcp_connect(
    uint64_t owner,
    network_handle handle,
    uint32_t destination,
    uint16_t port,
    uint64_t timeout_ns
);
enum network_status network_tcp_listen(
    uint64_t owner,
    network_handle handle,
    uint16_t port,
    size_t backlog
);
enum network_status network_tcp_accept(
    uint64_t owner,
    network_handle handle,
    network_handle *accepted,
    uint32_t *source,
    uint16_t *port,
    uint64_t timeout_ns
);
enum network_status network_tcp_write(
    uint64_t owner,
    network_handle handle,
    const uint8_t *bytes,
    size_t length,
    size_t *written,
    uint64_t timeout_ns
);
enum network_status network_tcp_read(
    uint64_t owner,
    network_handle handle,
    uint8_t *bytes,
    size_t capacity,
    size_t *read_bytes,
    uint64_t timeout_ns
);
enum network_status network_tcp_shutdown(
    uint64_t owner,
    network_handle handle,
    uint64_t timeout_ns
);
enum network_status network_close(uint64_t owner, network_handle handle);
enum network_status network_cancel(uint64_t owner, network_handle handle);
enum network_status network_address(
    uint64_t owner,
    network_handle handle,
    bool peer,
    uint32_t *address,
    uint16_t *port
);
enum network_status network_poll(
    uint64_t owner,
    const struct network_poll_request *requests,
    size_t request_count,
    struct network_poll_result *results,
    size_t result_capacity,
    size_t *result_count,
    uint64_t timeout_ns
);
void network_process_terminated(uint64_t owner);

enum network_status network_http_download(
    uint64_t owner,
    const char *url,
    const char *destination,
    bool head_only,
    uint64_t timeout_ns,
    struct network_http_result *result
);
enum network_status network_http_memory(
    uint64_t owner,
    const char *url,
    bool head_only,
    uint64_t timeout_ns,
    uint8_t *bytes,
    size_t capacity,
    struct network_http_result *result
);

struct network_state network_get_state(void);
bool network_self_test(size_t *completed_tests);
const char *network_status_string(enum network_status status);
void network_format_ipv4(uint32_t address, char output[16]);
bool network_parse_ipv4(const char *text, uint32_t *address);

#endif
