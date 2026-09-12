/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/clock.h>
#include <phipia/console.h>
#include <phipia/cpu.h>
#include <phipia/fat32_fs.h>
#include <phipia/network.h>
#include <phipia/random.h>
#include <phipia/timer.h>
#include <phipia/virtio_net.h>

#define ETHERNET_HEADER_BYTES 14U
#define ETHERNET_TYPE_ARP UINT16_C(0x0806)
#define ETHERNET_TYPE_IPV4 UINT16_C(0x0800)
#define ARP_PACKET_BYTES 28U
#define ARP_HARDWARE_ETHERNET UINT16_C(1)
#define ARP_PROTOCOL_IPV4 UINT16_C(0x0800)
#define ARP_OPERATION_REQUEST UINT16_C(1)
#define ARP_OPERATION_REPLY UINT16_C(2)
#define ARP_RETRY_COUNT 3U
#define ARP_TIMEOUT_NS UINT64_C(500000000)
#define ARP_LIFETIME_NS UINT64_C(60000000000)

#define IPV4_HEADER_BYTES 20U
#define IPV4_MIN_TTL UINT8_C(1)
#define IPV4_DEFAULT_TTL UINT8_C(64)
#define IPV4_PROTOCOL_ICMP UINT8_C(1)
#define IPV4_PROTOCOL_TCP UINT8_C(6)
#define IPV4_PROTOCOL_UDP UINT8_C(17)
#define IPV4_FLAG_MORE_FRAGMENTS UINT16_C(0x2000)
#define IPV4_FRAGMENT_OFFSET UINT16_C(0x1FFF)

#define ICMP_ECHO_REPLY UINT8_C(0)
#define ICMP_DESTINATION_UNREACHABLE UINT8_C(3)
#define ICMP_ECHO_REQUEST UINT8_C(8)
#define ICMP_TIME_EXCEEDED UINT8_C(11)
#define ICMP_HEADER_BYTES 8U

#define UDP_HEADER_BYTES 8U
#define DHCP_CLIENT_PORT UINT16_C(68)
#define DHCP_SERVER_PORT UINT16_C(67)
#define DNS_SERVER_PORT UINT16_C(53)
#define DHCP_FIXED_BYTES 240U
#define DHCP_MAGIC_COOKIE UINT32_C(0x63825363)
#define DHCP_DISCOVER UINT8_C(1)
#define DHCP_OFFER UINT8_C(2)
#define DHCP_REQUEST UINT8_C(3)
#define DHCP_ACK UINT8_C(5)
#define DHCP_NAK UINT8_C(6)
#define DHCP_OPTION_PAD UINT8_C(0)
#define DHCP_OPTION_SUBNET UINT8_C(1)
#define DHCP_OPTION_ROUTER UINT8_C(3)
#define DHCP_OPTION_DNS UINT8_C(6)
#define DHCP_OPTION_REQUESTED UINT8_C(50)
#define DHCP_OPTION_LEASE UINT8_C(51)
#define DHCP_OPTION_MESSAGE UINT8_C(53)
#define DHCP_OPTION_SERVER UINT8_C(54)
#define DHCP_OPTION_PARAMETERS UINT8_C(55)
#define DHCP_OPTION_RENEWAL UINT8_C(58)
#define DHCP_OPTION_REBINDING UINT8_C(59)
#define DHCP_OPTION_CLIENT UINT8_C(61)
#define DHCP_OPTION_END UINT8_C(255)
#define DHCP_RETRIES 3U

#define DNS_HEADER_BYTES 12U
#define DNS_TYPE_A UINT16_C(1)
#define DNS_TYPE_CNAME UINT16_C(5)
#define DNS_CLASS_IN UINT16_C(1)
#define DNS_MAX_POINTERS 16U
#define DNS_MAX_CNAME_FOLLOWS 4U

#define TCP_HEADER_BYTES 20U
#define TCP_FLAG_FIN UINT8_C(0x01)
#define TCP_FLAG_SYN UINT8_C(0x02)
#define TCP_FLAG_RST UINT8_C(0x04)
#define TCP_FLAG_PSH UINT8_C(0x08)
#define TCP_FLAG_ACK UINT8_C(0x10)
#define TCP_DEFAULT_WINDOW UINT16_C(8192)
#define TCP_MSS UINT16_C(1460)
#define TCP_RETRANSMISSION_NS UINT64_C(400000000)
#define TCP_EPHEMERAL_FIRST UINT16_C(49152)
#define TCP_EPHEMERAL_LAST UINT16_C(65535)

#define HANDLE_KIND_UDP UINT8_C(1)
#define HANDLE_KIND_TCP UINT8_C(2)
#define HANDLE_INDEX_MASK UINT64_C(0xFF)
#define HANDLE_GENERATION_MASK UINT64_C(0x0000FFFFFFFFFFFF)

#define NETWORK_OWNER_SHELL UINT64_C(1)
#define BROADCAST_IPV4 UINT32_C(0xFFFFFFFF)
#define DHCP_BUFFER_BYTES 576U
#define NETWORK_WAIT_SLICE_NS UINT64_C(10000000)
#define NETWORK_WAIT_MINIMUM_NS UINT64_C(100000)

enum arp_entry_state {
    ARP_ENTRY_EMPTY = 0,
    ARP_ENTRY_PENDING,
    ARP_ENTRY_VALID
};

enum tcp_connection_state {
    TCP_CONNECTION_CLOSED = 0,
    TCP_CONNECTION_OPEN,
    TCP_CONNECTION_LISTEN,
    TCP_CONNECTION_SYN_SENT,
    TCP_CONNECTION_SYN_RECEIVED,
    TCP_CONNECTION_ESTABLISHED,
    TCP_CONNECTION_FIN_WAIT,
    TCP_CONNECTION_CLOSE_WAIT,
    TCP_CONNECTION_RESET
};

struct arp_entry {
    uint32_t address;
    uint8_t mac[6];
    uint64_t expires_ns;
    uint64_t configuration_generation;
    uint64_t device_generation;
    enum arp_entry_state state;
};

struct udp_datagram {
    uint32_t source;
    uint16_t port;
    uint16_t length;
    uint8_t bytes[NETWORK_MAX_UDP_DATAGRAM];
};

struct udp_socket {
    struct udp_datagram queue[NETWORK_UDP_QUEUE_DEPTH];
    uint64_t owner;
    uint64_t generation;
    uint64_t device_generation;
    size_t queue_head;
    size_t queue_count;
    uint16_t port;
    enum network_status error;
    bool active;
    bool bound;
    bool cancelled;
};

struct tcp_connection {
    uint8_t receive[NETWORK_TCP_RX_BYTES];
    uint8_t retransmit[NETWORK_TCP_TX_BYTES];
    uint64_t owner;
    uint64_t generation;
    uint64_t device_generation;
    uint64_t retransmit_deadline_ns;
    uint32_t remote_address;
    uint32_t send_unacknowledged;
    uint32_t send_next;
    uint32_t receive_next;
    size_t receive_bytes;
    size_t retransmit_bytes;
    uint16_t local_port;
    uint16_t remote_port;
    uint16_t peer_window;
    uint16_t peer_mss;
    uint8_t retransmit_flags;
    uint8_t retransmit_count;
    uint8_t backlog;
    uint8_t listener;
    enum tcp_connection_state state;
    enum network_status error;
    bool active;
    bool cancelled;
    bool peer_closed;
    bool fin_sent;
    bool pending;
};

struct dns_cache_entry {
    char name[NETWORK_MAX_HOSTNAME + 1U];
    uint32_t address;
    uint64_t expires_ns;
    uint64_t configuration_generation;
    uint64_t device_generation;
    uint64_t insertion;
    bool active;
    bool negative;
};

struct dhcp_pending {
    uint32_t transaction;
    uint32_t offered_address;
    uint32_t subnet_mask;
    uint32_t router;
    uint32_t dns;
    uint32_t server;
    uint32_t lease_seconds;
    uint32_t renewal_seconds;
    uint32_t rebinding_seconds;
    uint8_t message;
    bool waiting;
    bool received;
};

struct dns_pending {
    char question[NETWORK_MAX_HOSTNAME + 1U];
    uint16_t identifier;
    uint16_t local_port;
    uint32_t address;
    uint32_t ttl;
    enum network_status status;
    bool waiting;
    bool received;
};

struct ping_pending {
    uint32_t address;
    uint16_t identifier;
    uint16_t sequence;
    uint64_t sent_ns;
    uint64_t round_trip_ns;
    enum network_status status;
    bool waiting;
    bool received;
};

struct network_runtime {
    struct network_state public;
    struct arp_entry arp[NETWORK_ARP_CACHE_SIZE];
    struct udp_socket udp[NETWORK_MAX_UDP_SOCKETS];
    struct tcp_connection tcp[NETWORK_MAX_TCP_CONNECTIONS];
    struct dns_cache_entry dns[NETWORK_DNS_CACHE_SIZE];
    struct dhcp_pending dhcp;
    struct dns_pending dns_query;
    struct ping_pending ping;
    uint64_t next_socket_generation;
    uint64_t dns_insertion;
    uint16_t ipv4_identifier;
    uint16_t next_ephemeral;
    size_t timers;
    /*
     * The receive path shares one frame buffer with the transmit path, so a
     * send issued while a frame is being parsed must never re-enter the pump.
     */
    bool servicing;
};

static struct network_runtime runtime;
static uint8_t receive_frame[VIRTIO_NET_MAX_FRAME_SIZE];
static uint8_t transmit_frame[VIRTIO_NET_MAX_FRAME_SIZE];

static void zero_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void copy_bytes(uint8_t *destination, const uint8_t *source, size_t length)
{
    for (size_t index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
}

static bool bytes_equal(const uint8_t *left, const uint8_t *right, size_t length)
{
    for (size_t index = 0U; index < length; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

static size_t string_length_bounded(const char *text, size_t maximum)
{
    size_t length = 0U;

    if (text == NULL) {
        return maximum + 1U;
    }
    while (length <= maximum && text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool string_equal(const char *left, const char *right)
{
    size_t index = 0U;

    if (left == NULL || right == NULL) {
        return false;
    }
    while (left[index] != '\0' && right[index] != '\0') {
        char a = left[index];
        char b = right[index];

        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
        ++index;
    }
    return left[index] == '\0' && right[index] == '\0';
}

static void string_copy(char *destination, const char *source, size_t length)
{
    for (size_t index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
    destination[length] = '\0';
}

static uint16_t read_be16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] << 8U) | bytes[1];
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] << 24U |
        (uint32_t)bytes[1] << 16U |
        (uint32_t)bytes[2] << 8U |
        (uint32_t)bytes[3];
}

static void write_be16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8U);
    bytes[1] = (uint8_t)value;
}

static void write_be32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

static uint32_t checksum_add(uint32_t sum, const uint8_t *bytes, size_t length)
{
    size_t index = 0U;

    while (length >= 2U) {
        sum += read_be16(bytes + index);
        index += 2U;
        length -= 2U;
    }
    if (length != 0U) {
        sum += (uint32_t)bytes[index] << 8U;
    }
    return sum;
}

static uint16_t checksum_finish(uint32_t sum)
{
    while ((sum >> 16U) != 0U) {
        sum = (sum & UINT32_C(0xFFFF)) + (sum >> 16U);
    }
    return (uint16_t)~sum;
}

static uint16_t internet_checksum(const uint8_t *bytes, size_t length)
{
    return checksum_finish(checksum_add(0U, bytes, length));
}

static uint16_t transport_checksum(
    uint32_t source,
    uint32_t destination,
    uint8_t protocol,
    const uint8_t *bytes,
    size_t length
)
{
    uint8_t pseudo[12];
    uint32_t sum;

    write_be32(pseudo + 0U, source);
    write_be32(pseudo + 4U, destination);
    pseudo[8] = 0U;
    pseudo[9] = protocol;
    write_be16(pseudo + 10U, (uint16_t)length);
    sum = checksum_add(0U, pseudo, sizeof(pseudo));
    sum = checksum_add(sum, bytes, length);
    return checksum_finish(sum);
}

static bool deadline_valid(uint64_t timeout_ns)
{
    return timeout_ns != 0U && timeout_ns <= UINT64_C(30000000000);
}

/*
 * Synchronous protocol operations are permitted to block their caller, but
 * must not burn Phipia's single core while waiting for a packet or deadline.
 * Every caller invokes this only after pumping the device and rechecking its
 * completion state. A device interrupt returns immediately. A short deadline
 * timer closes the already-delivered-interrupt race and guarantees another
 * pump even when a device completes just before the CPU halts.
 */
static void network_wait_deadline(uint64_t deadline_ns, void *context)
{
    bool *expired = context;

    (void)deadline_ns;
    if (expired != NULL) {
        *expired = true;
    }
}

static bool network_wait_for_interrupt(uint64_t deadline_ns)
{
    uint64_t identifier = 0U;
    uint64_t now;
    uint64_t interval;
    uint64_t wake_deadline;
    bool expired = false;

    if (!cpu_interrupts_enabled()) {
        return false;
    }
    now = clock_monotonic_ns();
    if (deadline_ns <= now) {
        return true;
    }
    interval = deadline_ns - now;
    if (interval > NETWORK_WAIT_SLICE_NS) {
        interval = NETWORK_WAIT_SLICE_NS;
    }
    if (interval < NETWORK_WAIT_MINIMUM_NS || now > UINT64_MAX - interval) {
        return true;
    }
    wake_deadline = now + interval;
    cpu_interrupt_disable();
    const enum timer_status arm_status = timer_arm(wake_deadline,
        network_wait_deadline, &expired, &identifier);

    if (arm_status != TIMER_STATUS_OK) {
        cpu_interrupt_enable();
        /*
         * TCG can deschedule the guest between the clock sample and
         * timer_arm's independent sample.  If that consumes this bounded
         * ten-millisecond slice, the wait has elapsed rather than run out of
         * timer resources.  Let the caller pump again and apply its original
         * absolute deadline; all other arm failures remain hard failures.
         */
        if (arm_status == TIMER_STATUS_BAD_INTERVAL) {
            return true;
        }
        return false;
    }
    cpu_enable_and_halt();
    cpu_interrupt_disable();
    if (!expired && timer_cancel(identifier) != TIMER_STATUS_OK) {
        console_panic("network wait could not cancel its deadline");
    }
    cpu_interrupt_enable();
    return true;
}

static bool timer_acquire(void)
{
    if (runtime.timers >= NETWORK_MAX_TIMERS) {
        ++runtime.public.statistics.timer_exhaustion;
        return false;
    }
    ++runtime.timers;
    runtime.public.timers = runtime.timers;
    return true;
}

static void timer_release(void)
{
    if (runtime.timers != 0U) {
        --runtime.timers;
    }
    runtime.public.timers = runtime.timers;
}

static bool mac_is_broadcast(const uint8_t mac[6])
{
    static const uint8_t broadcast[6] = {
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU
    };

    return bytes_equal(mac, broadcast, sizeof(broadcast));
}

static bool mac_is_zero(const uint8_t mac[6])
{
    static const uint8_t zero[6] = {0};

    return bytes_equal(mac, zero, sizeof(zero));
}

static bool ipv4_is_multicast(uint32_t address)
{
    return (address & UINT32_C(0xF0000000)) == UINT32_C(0xE0000000);
}

static bool ipv4_is_unicast(uint32_t address)
{
    return address != 0U && address != BROADCAST_IPV4 &&
        !ipv4_is_multicast(address) &&
        (address & UINT32_C(0xFF000000)) != UINT32_C(0x7F000000);
}

static uint64_t make_handle(uint8_t kind, size_t index, uint64_t generation)
{
    return (uint64_t)(index + 1U) |
        ((generation & HANDLE_GENERATION_MASK) << 8U) |
        (uint64_t)kind << 56U;
}

static bool decode_handle(
    network_handle handle,
    uint8_t expected_kind,
    size_t maximum,
    size_t *index,
    uint64_t *generation
)
{
    const uint8_t kind = (uint8_t)(handle >> 56U);
    const uint64_t encoded_index = handle & HANDLE_INDEX_MASK;

    if (kind != expected_kind || encoded_index == 0U ||
        encoded_index > maximum || index == NULL || generation == NULL) {
        return false;
    }
    *index = (size_t)(encoded_index - 1U);
    *generation = (handle >> 8U) & HANDLE_GENERATION_MASK;
    return *generation != 0U;
}

static uint64_t allocate_generation(void)
{
    uint64_t generation = runtime.next_socket_generation++;

    if (generation == 0U || generation > HANDLE_GENERATION_MASK) {
        runtime.next_socket_generation = 2U;
        generation = 1U;
    }
    return generation;
}

static struct udp_socket *udp_for(
    uint64_t owner,
    network_handle handle,
    enum network_status *status
)
{
    size_t index;
    uint64_t generation;
    struct udp_socket *socket;

    if (!decode_handle(handle, HANDLE_KIND_UDP, NETWORK_MAX_UDP_SOCKETS,
            &index, &generation)) {
        *status = NETWORK_STATUS_STALE_HANDLE;
        return NULL;
    }
    socket = &runtime.udp[index];
    if (!socket->active || socket->generation != generation ||
        socket->device_generation != runtime.public.device.device_generation) {
        *status = NETWORK_STATUS_STALE_HANDLE;
        return NULL;
    }
    if (socket->owner != owner) {
        *status = NETWORK_STATUS_WRONG_OWNER;
        return NULL;
    }
    *status = NETWORK_STATUS_OK;
    return socket;
}

static struct tcp_connection *tcp_for(
    uint64_t owner,
    network_handle handle,
    enum network_status *status
)
{
    size_t index;
    uint64_t generation;
    struct tcp_connection *connection;

    if (!decode_handle(handle, HANDLE_KIND_TCP, NETWORK_MAX_TCP_CONNECTIONS,
            &index, &generation)) {
        *status = NETWORK_STATUS_STALE_HANDLE;
        return NULL;
    }
    connection = &runtime.tcp[index];
    if (!connection->active || connection->generation != generation ||
        connection->device_generation !=
            runtime.public.device.device_generation) {
        *status = NETWORK_STATUS_STALE_HANDLE;
        return NULL;
    }
    if (connection->owner != owner) {
        *status = NETWORK_STATUS_WRONG_OWNER;
        return NULL;
    }
    *status = NETWORK_STATUS_OK;
    return connection;
}

static enum network_status ethernet_send(
    const uint8_t destination[6],
    uint16_t type,
    const uint8_t *payload,
    size_t payload_length
)
{
    if (destination == NULL || payload == NULL ||
        payload_length > VIRTIO_NET_MAX_FRAME_SIZE - ETHERNET_HEADER_BYTES) {
        return NETWORK_STATUS_INVALID_ARGUMENT;
    }
    copy_bytes(transmit_frame + 0U, destination, 6U);
    copy_bytes(transmit_frame + 6U, runtime.public.device.mac, 6U);
    write_be16(transmit_frame + 12U, type);
    copy_bytes(transmit_frame + ETHERNET_HEADER_BYTES, payload,
        payload_length);
    enum virtio_net_status status = virtio_net_transmit(transmit_frame,
        ETHERNET_HEADER_BYTES + payload_length);

    if (status == VIRTIO_NET_STATUS_OK) {
        return NETWORK_STATUS_OK;
    }
    if (status == VIRTIO_NET_STATUS_LINK_DOWN) {
        return NETWORK_STATUS_LINK_DOWN;
    }
    if (status == VIRTIO_NET_STATUS_TX_EXHAUSTED) {
        return NETWORK_STATUS_WOULD_BLOCK;
    }
    return NETWORK_STATUS_UNAVAILABLE;
}

static void arp_invalidate(void)
{
    zero_bytes(runtime.arp, sizeof(runtime.arp));
    runtime.public.arp_entries = 0U;
}

static struct arp_entry *arp_find(uint32_t address)
{
    const uint64_t now = clock_monotonic_ns();

    for (size_t index = 0U; index < NETWORK_ARP_CACHE_SIZE; ++index) {
        struct arp_entry *entry = &runtime.arp[index];

        if (entry->state != ARP_ENTRY_EMPTY && entry->address == address &&
            entry->configuration_generation ==
                runtime.public.configuration.generation &&
            entry->device_generation ==
                runtime.public.device.device_generation) {
            if (entry->state == ARP_ENTRY_VALID && entry->expires_ns <= now) {
                zero_bytes(entry, sizeof(*entry));
                continue;
            }
            return entry;
        }
    }
    return NULL;
}

static struct arp_entry *arp_slot(uint32_t address)
{
    struct arp_entry *oldest = &runtime.arp[0];

    for (size_t index = 0U; index < NETWORK_ARP_CACHE_SIZE; ++index) {
        struct arp_entry *entry = &runtime.arp[index];

        if (entry->state == ARP_ENTRY_EMPTY) {
            oldest = entry;
            break;
        }
        if (entry->expires_ns < oldest->expires_ns) {
            oldest = entry;
        }
    }
    zero_bytes(oldest, sizeof(*oldest));
    oldest->address = address;
    oldest->configuration_generation =
        runtime.public.configuration.generation;
    oldest->device_generation = runtime.public.device.device_generation;
    return oldest;
}

static enum network_status arp_request(uint32_t address)
{
    static const uint8_t broadcast[6] = {
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU
    };
    uint8_t packet[ARP_PACKET_BYTES];

    zero_bytes(packet, sizeof(packet));
    write_be16(packet + 0U, ARP_HARDWARE_ETHERNET);
    write_be16(packet + 2U, ARP_PROTOCOL_IPV4);
    packet[4] = 6U;
    packet[5] = 4U;
    write_be16(packet + 6U, ARP_OPERATION_REQUEST);
    copy_bytes(packet + 8U, runtime.public.device.mac, 6U);
    write_be32(packet + 14U, runtime.public.configuration.address);
    write_be32(packet + 24U, address);
    return ethernet_send(broadcast, ETHERNET_TYPE_ARP, packet,
        sizeof(packet));
}

static enum network_status arp_resolve(
    uint32_t address,
    uint8_t destination[6],
    uint64_t timeout_ns
)
{
    struct arp_entry *entry;
    uint64_t deadline;

    if (address == BROADCAST_IPV4) {
        for (size_t index = 0U; index < 6U; ++index) {
            destination[index] = 0xFFU;
        }
        return NETWORK_STATUS_OK;
    }
    if (!runtime.public.configuration.configured || !ipv4_is_unicast(address) ||
        !deadline_valid(timeout_ns)) {
        return NETWORK_STATUS_INVALID_ARGUMENT;
    }
    entry = arp_find(address);
    if (entry != NULL && entry->state == ARP_ENTRY_VALID) {
        copy_bytes(destination, entry->mac, 6U);
        return NETWORK_STATUS_OK;
    }
    if (entry == NULL) {
        entry = arp_slot(address);
    }
    entry->state = ARP_ENTRY_PENDING;
    if (runtime.servicing) {
        /*
         * A send raised while a received frame is still being parsed cannot
         * wait: waiting means pumping the device, and the pump would overwrite
         * the frame its own caller is reading. Ask once and refuse now; the
         * caller's retransmission carries the send after the reply lands.
         */
        ++runtime.public.statistics.arp_deferred;
        (void)arp_request(address);
        return NETWORK_STATUS_WOULD_BLOCK;
    }
    if (!timer_acquire()) {
        return NETWORK_STATUS_NO_RESOURCES;
    }
    deadline = clock_monotonic_ns() + timeout_ns;
    for (size_t retry = 0U; retry < ARP_RETRY_COUNT; ++retry) {
        const uint64_t attempt_end = clock_monotonic_ns() + ARP_TIMEOUT_NS;

        if (arp_request(address) != NETWORK_STATUS_OK) {
            continue;
        }
        while (clock_monotonic_ns() < deadline &&
            clock_monotonic_ns() < attempt_end) {
            (void)network_service();
            if (entry->state == ARP_ENTRY_VALID) {
                copy_bytes(destination, entry->mac, 6U);
                timer_release();
                return NETWORK_STATUS_OK;
            }
            if (clock_monotonic_ns() < deadline &&
                clock_monotonic_ns() < attempt_end) {
                if (!network_wait_for_interrupt(attempt_end < deadline ?
                        attempt_end : deadline)) {
                    entry->state = ARP_ENTRY_EMPTY;
                    timer_release();
                    return NETWORK_STATUS_NO_RESOURCES;
                }
            }
        }
    }
    entry->state = ARP_ENTRY_EMPTY;
    timer_release();
    return NETWORK_STATUS_TIMEOUT;
}

static enum network_status ipv4_send(
    uint32_t source,
    uint32_t destination,
    uint8_t protocol,
    const uint8_t *payload,
    size_t payload_length,
    uint64_t timeout_ns
)
{
    uint8_t destination_mac[6];
    uint32_t next_hop = destination;
    uint8_t *header = transmit_frame + ETHERNET_HEADER_BYTES;
    size_t total_length = IPV4_HEADER_BYTES + payload_length;
    enum network_status status;

    if (payload == NULL || total_length > 1500U || total_length > UINT16_MAX) {
        return NETWORK_STATUS_TOO_LARGE;
    }
    if (destination != BROADCAST_IPV4) {
        if (!runtime.public.configuration.configured) {
            return NETWORK_STATUS_UNCONFIGURED;
        }
        if ((destination & runtime.public.configuration.subnet_mask) !=
                (runtime.public.configuration.address &
                    runtime.public.configuration.subnet_mask)) {
            next_hop = runtime.public.configuration.gateway;
            if (next_hop == 0U) {
                return NETWORK_STATUS_UNREACHABLE;
            }
        }
        status = arp_resolve(next_hop, destination_mac, timeout_ns);
        if (status != NETWORK_STATUS_OK) {
            return status;
        }
    } else {
        for (size_t index = 0U; index < sizeof(destination_mac); ++index) {
            destination_mac[index] = 0xFFU;
        }
    }
    zero_bytes(header, IPV4_HEADER_BYTES);
    header[0] = UINT8_C(0x45);
    write_be16(header + 2U, (uint16_t)total_length);
    write_be16(header + 4U, ++runtime.ipv4_identifier);
    write_be16(header + 6U, UINT16_C(0x4000));
    header[8] = IPV4_DEFAULT_TTL;
    header[9] = protocol;
    write_be32(header + 12U, source);
    write_be32(header + 16U, destination);
    write_be16(header + 10U, internet_checksum(header, IPV4_HEADER_BYTES));
    copy_bytes(header + IPV4_HEADER_BYTES, payload, payload_length);
    return ethernet_send(destination_mac, ETHERNET_TYPE_IPV4, header,
        total_length);
}

static enum network_status udp_send_raw(
    uint32_t source,
    uint16_t source_port,
    uint32_t destination,
    uint16_t destination_port,
    const uint8_t *bytes,
    size_t length,
    uint64_t timeout_ns
)
{
    uint8_t datagram[UDP_HEADER_BYTES + NETWORK_MAX_UDP_DATAGRAM];
    size_t total = UDP_HEADER_BYTES + length;
    uint16_t checksum;

    if (bytes == NULL || length > NETWORK_MAX_UDP_DATAGRAM ||
        source_port == 0U || destination_port == 0U) {
        return NETWORK_STATUS_INVALID_ARGUMENT;
    }
    write_be16(datagram + 0U, source_port);
    write_be16(datagram + 2U, destination_port);
    write_be16(datagram + 4U, (uint16_t)total);
    write_be16(datagram + 6U, 0U);
    copy_bytes(datagram + UDP_HEADER_BYTES, bytes, length);
    checksum = transport_checksum(source, destination, IPV4_PROTOCOL_UDP,
        datagram, total);
    write_be16(datagram + 6U, checksum == 0U ? UINT16_C(0xFFFF) : checksum);
    return ipv4_send(source, destination, IPV4_PROTOCOL_UDP, datagram,
        total, timeout_ns);
}

static bool udp_enqueue(
    struct udp_socket *socket,
    uint32_t source,
    uint16_t port,
    const uint8_t *bytes,
    size_t length
)
{
    size_t slot;
    struct udp_datagram *datagram;

    if (socket->queue_count >= NETWORK_UDP_QUEUE_DEPTH ||
        length > NETWORK_MAX_UDP_DATAGRAM) {
        return false;
    }
    slot = (socket->queue_head + socket->queue_count) %
        NETWORK_UDP_QUEUE_DEPTH;
    datagram = &socket->queue[slot];
    datagram->source = source;
    datagram->port = port;
    datagram->length = (uint16_t)length;
    copy_bytes(datagram->bytes, bytes, length);
    ++socket->queue_count;
    return true;
}

static void dhcp_parse(
    uint32_t source,
    const uint8_t *bytes,
    size_t length
)
{
    size_t offset = DHCP_FIXED_BYTES;
    bool seen_message = false;
    bool seen_server = false;

    if (!runtime.dhcp.waiting || length < DHCP_FIXED_BYTES || bytes[0] != 2U ||
        bytes[1] != 1U || bytes[2] != 6U ||
        read_be32(bytes + 4U) != runtime.dhcp.transaction ||
        read_be32(bytes + 236U) != DHCP_MAGIC_COOKIE ||
        !bytes_equal(bytes + 28U, runtime.public.device.mac, 6U)) {
        return;
    }
    runtime.dhcp.offered_address = read_be32(bytes + 16U);
    while (offset < length) {
        uint8_t code = bytes[offset++];
        uint8_t option_length;

        if (code == DHCP_OPTION_END) {
            break;
        }
        if (code == DHCP_OPTION_PAD) {
            continue;
        }
        if (offset >= length) {
            return;
        }
        option_length = bytes[offset++];
        if (option_length > length - offset) {
            return;
        }
        switch (code) {
        case DHCP_OPTION_MESSAGE:
            if (option_length != 1U || seen_message) { return; }
            runtime.dhcp.message = bytes[offset];
            seen_message = true;
            break;
        case DHCP_OPTION_SUBNET:
            if (option_length != 4U || runtime.dhcp.subnet_mask != 0U) {
                return;
            }
            runtime.dhcp.subnet_mask = read_be32(bytes + offset);
            break;
        case DHCP_OPTION_ROUTER:
            if (option_length < 4U || runtime.dhcp.router != 0U) { return; }
            runtime.dhcp.router = read_be32(bytes + offset);
            break;
        case DHCP_OPTION_DNS:
            if (option_length < 4U || runtime.dhcp.dns != 0U) { return; }
            runtime.dhcp.dns = read_be32(bytes + offset);
            break;
        case DHCP_OPTION_SERVER:
            if (option_length != 4U || seen_server) { return; }
            runtime.dhcp.server = read_be32(bytes + offset);
            seen_server = true;
            break;
        case DHCP_OPTION_LEASE:
            if (option_length != 4U || runtime.dhcp.lease_seconds != 0U) {
                return;
            }
            runtime.dhcp.lease_seconds = read_be32(bytes + offset);
            break;
        case DHCP_OPTION_RENEWAL:
            if (option_length != 4U || runtime.dhcp.renewal_seconds != 0U) {
                return;
            }
            runtime.dhcp.renewal_seconds = read_be32(bytes + offset);
            break;
        case DHCP_OPTION_REBINDING:
            if (option_length != 4U || runtime.dhcp.rebinding_seconds != 0U) {
                return;
            }
            runtime.dhcp.rebinding_seconds = read_be32(bytes + offset);
            break;
        default:
            break;
        }
        offset += option_length;
    }
    if (!seen_message || !seen_server || source != runtime.dhcp.server ||
        (runtime.dhcp.message != DHCP_OFFER &&
            runtime.dhcp.message != DHCP_ACK &&
            runtime.dhcp.message != DHCP_NAK)) {
        return;
    }
    runtime.dhcp.received = true;
}

static enum network_status dns_parse_response(
    uint32_t source,
    const uint8_t *bytes,
    size_t length
);
static void tcp_receive_segment(
    uint32_t source,
    uint32_t destination,
    const uint8_t *bytes,
    size_t length
);

static void udp_receive_packet(
    uint32_t source,
    uint32_t destination,
    const uint8_t *bytes,
    size_t length
)
{
    uint16_t source_port;
    uint16_t destination_port;
    uint16_t datagram_length;
    uint16_t checksum;
    const uint8_t *payload;
    size_t payload_length;

    if (length < UDP_HEADER_BYTES) {
        ++runtime.public.statistics.malformed_packets;
        return;
    }
    source_port = read_be16(bytes + 0U);
    destination_port = read_be16(bytes + 2U);
    datagram_length = read_be16(bytes + 4U);
    checksum = read_be16(bytes + 6U);
    if (source_port == 0U || destination_port == 0U ||
        datagram_length < UDP_HEADER_BYTES || datagram_length > length ||
        checksum == 0U || transport_checksum(source, destination,
            IPV4_PROTOCOL_UDP, bytes, datagram_length) != 0U) {
        ++runtime.public.statistics.malformed_packets;
        return;
    }
    payload = bytes + UDP_HEADER_BYTES;
    payload_length = datagram_length - UDP_HEADER_BYTES;
    ++runtime.public.statistics.udp_accepted;
    if (destination_port == DHCP_CLIENT_PORT &&
        source_port == DHCP_SERVER_PORT) {
        dhcp_parse(source, payload, payload_length);
        return;
    }
    if (runtime.dns_query.waiting &&
        destination_port == runtime.dns_query.local_port &&
        source_port == DNS_SERVER_PORT) {
        (void)dns_parse_response(source, payload, payload_length);
        return;
    }
    for (size_t index = 0U; index < NETWORK_MAX_UDP_SOCKETS; ++index) {
        struct udp_socket *socket = &runtime.udp[index];

        if (socket->active && socket->bound &&
            socket->port == destination_port &&
            !udp_enqueue(socket, source, source_port, payload,
                payload_length)) {
            socket->error = NETWORK_STATUS_NO_RESOURCES;
        }
    }
}

static void icmp_receive_packet(
    uint32_t source,
    uint32_t destination,
    const uint8_t *bytes,
    size_t length
)
{
    uint8_t type;

    if (length < ICMP_HEADER_BYTES || internet_checksum(bytes, length) != 0U) {
        ++runtime.public.statistics.malformed_packets;
        return;
    }
    type = bytes[0];
    if (type != ICMP_ECHO_REQUEST && type != ICMP_ECHO_REPLY &&
        type != ICMP_DESTINATION_UNREACHABLE && type != ICMP_TIME_EXCEEDED) {
        return;
    }
    ++runtime.public.statistics.icmp_accepted;
    if (type == ICMP_ECHO_REQUEST &&
        destination == runtime.public.configuration.address) {
        uint8_t reply[64];

        if (length > sizeof(reply)) {
            return;
        }
        copy_bytes(reply, bytes, length);
        reply[0] = ICMP_ECHO_REPLY;
        write_be16(reply + 2U, 0U);
        write_be16(reply + 2U, internet_checksum(reply, length));
        (void)ipv4_send(runtime.public.configuration.address, source,
            IPV4_PROTOCOL_ICMP, reply, length, ARP_TIMEOUT_NS);
    } else if (runtime.ping.waiting && source == runtime.ping.address) {
        if (type == ICMP_ECHO_REPLY &&
            read_be16(bytes + 4U) == runtime.ping.identifier &&
            read_be16(bytes + 6U) == runtime.ping.sequence) {
            runtime.ping.round_trip_ns = clock_monotonic_ns() -
                runtime.ping.sent_ns;
            runtime.ping.status = NETWORK_STATUS_OK;
            runtime.ping.received = true;
        } else if (type == ICMP_DESTINATION_UNREACHABLE) {
            runtime.ping.status = NETWORK_STATUS_UNREACHABLE;
            runtime.ping.received = true;
        } else if (type == ICMP_TIME_EXCEEDED) {
            runtime.ping.status = NETWORK_STATUS_TIMEOUT;
            runtime.ping.received = true;
        }
    }
}

static void ipv4_receive_packet(const uint8_t *bytes, size_t length)
{
    uint8_t header_length;
    uint16_t total_length;
    uint16_t fragmentation;
    uint32_t source;
    uint32_t destination;
    uint8_t protocol;

    if (length < IPV4_HEADER_BYTES || (bytes[0] >> 4U) != 4U) {
        ++runtime.public.statistics.malformed_packets;
        return;
    }
    header_length = (uint8_t)((bytes[0] & UINT8_C(0x0F)) * 4U);
    total_length = read_be16(bytes + 2U);
    if (header_length < IPV4_HEADER_BYTES || header_length > length ||
        total_length < header_length || total_length > length ||
        internet_checksum(bytes, header_length) != 0U) {
        ++runtime.public.statistics.ipv4_checksum_failures;
        return;
    }
    fragmentation = read_be16(bytes + 6U);
    if ((fragmentation & (IPV4_FLAG_MORE_FRAGMENTS |
            IPV4_FRAGMENT_OFFSET)) != 0U) {
        ++runtime.public.statistics.ipv4_fragments;
        return;
    }
    source = read_be32(bytes + 12U);
    destination = read_be32(bytes + 16U);
    protocol = bytes[9];
    if (bytes[8] < IPV4_MIN_TTL || !ipv4_is_unicast(source) ||
        (destination != runtime.public.configuration.address &&
            destination != BROADCAST_IPV4 &&
            !(runtime.dhcp.waiting && destination == 0U))) {
        ++runtime.public.statistics.malformed_packets;
        return;
    }
    ++runtime.public.statistics.ipv4_accepted;
    const uint8_t *payload = bytes + header_length;
    const size_t payload_length = total_length - header_length;

    if (protocol == IPV4_PROTOCOL_ICMP) {
        icmp_receive_packet(source, destination, payload, payload_length);
    } else if (protocol == IPV4_PROTOCOL_UDP) {
        udp_receive_packet(source, destination, payload, payload_length);
    } else if (protocol == IPV4_PROTOCOL_TCP) {
        tcp_receive_segment(source, destination, payload, payload_length);
    }
}

static void arp_receive_packet(const uint8_t *bytes, size_t length)
{
    uint16_t operation;
    uint32_t sender_address;
    uint32_t target_address;
    const uint8_t *sender_mac;
    struct arp_entry *entry;

    if (length < ARP_PACKET_BYTES ||
        read_be16(bytes + 0U) != ARP_HARDWARE_ETHERNET ||
        read_be16(bytes + 2U) != ARP_PROTOCOL_IPV4 ||
        bytes[4] != 6U || bytes[5] != 4U) {
        ++runtime.public.statistics.malformed_packets;
        return;
    }
    operation = read_be16(bytes + 6U);
    sender_mac = bytes + 8U;
    sender_address = read_be32(bytes + 14U);
    target_address = read_be32(bytes + 24U);
    if ((operation != ARP_OPERATION_REQUEST &&
            operation != ARP_OPERATION_REPLY) ||
        mac_is_zero(sender_mac) || mac_is_broadcast(sender_mac) ||
        !ipv4_is_unicast(sender_address)) {
        ++runtime.public.statistics.malformed_packets;
        return;
    }
    ++runtime.public.statistics.arp_accepted;
    entry = arp_find(sender_address);
    if (entry != NULL && entry->state == ARP_ENTRY_VALID &&
        !bytes_equal(entry->mac, sender_mac, 6U)) {
        ++runtime.public.statistics.arp_conflicts;
        return;
    }
    if (entry != NULL && entry->state == ARP_ENTRY_PENDING) {
        copy_bytes(entry->mac, sender_mac, 6U);
        entry->expires_ns = clock_monotonic_ns() + ARP_LIFETIME_NS;
        entry->state = ARP_ENTRY_VALID;
        runtime.public.arp_entries = runtime.public.arp_entries <
            NETWORK_ARP_CACHE_SIZE ? runtime.public.arp_entries + 1U :
            runtime.public.arp_entries;
    }
    if (operation == ARP_OPERATION_REQUEST &&
        runtime.public.configuration.configured &&
        target_address == runtime.public.configuration.address) {
        uint8_t reply[ARP_PACKET_BYTES];

        write_be16(reply + 0U, ARP_HARDWARE_ETHERNET);
        write_be16(reply + 2U, ARP_PROTOCOL_IPV4);
        reply[4] = 6U;
        reply[5] = 4U;
        write_be16(reply + 6U, ARP_OPERATION_REPLY);
        copy_bytes(reply + 8U, runtime.public.device.mac, 6U);
        write_be32(reply + 14U, runtime.public.configuration.address);
        copy_bytes(reply + 18U, sender_mac, 6U);
        write_be32(reply + 24U, sender_address);
        (void)ethernet_send(sender_mac, ETHERNET_TYPE_ARP, reply,
            sizeof(reply));
    }
}

static void ethernet_receive(const uint8_t *frame, size_t length)
{
    const uint8_t *destination;
    uint16_t type;

    if (length < ETHERNET_HEADER_BYTES) {
        ++runtime.public.statistics.malformed_packets;
        return;
    }
    destination = frame;
    if (!bytes_equal(destination, runtime.public.device.mac, 6U) &&
        !mac_is_broadcast(destination)) {
        if ((destination[0] & UINT8_C(1)) != 0U) {
            ++runtime.public.statistics.ethernet_unsupported;
        }
        return;
    }
    if (mac_is_zero(frame + 6U) || mac_is_broadcast(frame + 6U)) {
        ++runtime.public.statistics.malformed_packets;
        return;
    }
    type = read_be16(frame + 12U);
    ++runtime.public.statistics.ethernet_accepted;
    if (type == ETHERNET_TYPE_ARP) {
        arp_receive_packet(frame + ETHERNET_HEADER_BYTES,
            length - ETHERNET_HEADER_BYTES);
    } else if (type == ETHERNET_TYPE_IPV4) {
        ipv4_receive_packet(frame + ETHERNET_HEADER_BYTES,
            length - ETHERNET_HEADER_BYTES);
    } else {
        ++runtime.public.statistics.ethernet_unsupported;
    }
}

static enum network_status network_service_pump(void)
{
    enum virtio_net_status device_status;

    device_status = virtio_net_service();
    if (device_status == VIRTIO_NET_STATUS_RESET) {
        enum virtio_net_status reset_status = virtio_net_reset();

        runtime.public.device = virtio_net_get_state();
        runtime.public.configuration.configured = false;
        runtime.public.configuration.source = NETWORK_CONFIGURATION_NONE;
        ++runtime.public.configuration.generation;
        arp_invalidate();
        zero_bytes(runtime.dns, sizeof(runtime.dns));
        runtime.public.dns_entries = 0U;
        ++runtime.public.statistics.resets;
        for (size_t index = 0U; index < NETWORK_MAX_UDP_SOCKETS; ++index) {
            if (runtime.udp[index].active) {
                runtime.udp[index].device_generation =
                    runtime.public.device.device_generation;
                runtime.udp[index].error = NETWORK_STATUS_RESET;
            }
        }
        for (size_t index = 0U; index < NETWORK_MAX_TCP_CONNECTIONS;
             ++index) {
            if (runtime.tcp[index].active) {
                runtime.tcp[index].device_generation =
                    runtime.public.device.device_generation;
                runtime.tcp[index].error = NETWORK_STATUS_RESET;
                runtime.tcp[index].state = TCP_CONNECTION_RESET;
            }
        }
        return reset_status == VIRTIO_NET_STATUS_OK ||
            reset_status == VIRTIO_NET_STATUS_LINK_DOWN ?
            NETWORK_STATUS_RESET : NETWORK_STATUS_UNAVAILABLE;
    }
    if (device_status != VIRTIO_NET_STATUS_OK &&
        device_status != VIRTIO_NET_STATUS_RX_EMPTY) {
        if (device_status == VIRTIO_NET_STATUS_LINK_DOWN) {
            runtime.public.device.link_up = false;
            return NETWORK_STATUS_LINK_DOWN;
        }
        return NETWORK_STATUS_UNAVAILABLE;
    }
    runtime.public.device = virtio_net_get_state();
    for (size_t count = 0U; count < VIRTIO_NET_RX_RESERVE; ++count) {
        size_t length = 0U;

        device_status = virtio_net_receive(receive_frame,
            sizeof(receive_frame), &length);
        if (device_status == VIRTIO_NET_STATUS_RX_EMPTY) {
            break;
        }
        if (device_status != VIRTIO_NET_STATUS_OK) {
            ++runtime.public.statistics.malformed_packets;
            continue;
        }
        ethernet_receive(receive_frame, length);
    }
    if (runtime.public.configuration.configured &&
        runtime.public.configuration.source == NETWORK_CONFIGURATION_DHCP &&
        runtime.public.configuration.lease_expires_ns != 0U &&
        clock_monotonic_ns() >=
            runtime.public.configuration.lease_expires_ns) {
        runtime.public.configuration.configured = false;
        runtime.public.configuration.source = NETWORK_CONFIGURATION_NONE;
        ++runtime.public.configuration.generation;
        arp_invalidate();
    }
    return NETWORK_STATUS_OK;
}

/*
 * One receive buffer and one transmit buffer serve the stack, so the pump
 * cannot re-enter while handling a reply. Recursive service returns WOULD_BLOCK;
 * arp_resolve sends one request while the service loop owns the buffers.
 */
enum network_status network_service(void)
{
    enum network_status status;

    if (!runtime.public.active) {
        return NETWORK_STATUS_NOT_INITIALIZED;
    }
    if (runtime.servicing) {
        return NETWORK_STATUS_WOULD_BLOCK;
    }
    runtime.servicing = true;
    status = network_service_pump();
    runtime.servicing = false;
    return status;
}

enum network_status network_initialize(void)
{
    enum virtio_net_status status;
    size_t driver_tests = 0U;
    const uint64_t previous_generation = runtime.public.configuration.generation;

    if (runtime.public.active) {
        return NETWORK_STATUS_ALREADY_INITIALIZED;
    }
    if (!virtio_net_self_test(&driver_tests) || driver_tests != 14U) {
        return NETWORK_STATUS_UNAVAILABLE;
    }
    zero_bytes(&runtime, sizeof(runtime));
    runtime.next_socket_generation = 1U;
    runtime.next_ephemeral = (uint16_t)(TCP_EPHEMERAL_FIRST +
        random_u16() % (TCP_EPHEMERAL_LAST - TCP_EPHEMERAL_FIRST + 1U));
    runtime.public.configuration.generation = previous_generation + 1U;
    const bool restore_interrupts = cpu_interrupts_enabled();

    if (restore_interrupts) {
        cpu_interrupt_disable();
    }
    status = virtio_net_initialize();
    if (restore_interrupts) {
        cpu_interrupt_enable();
    }
    runtime.public.device = virtio_net_get_state();
    if (status == VIRTIO_NET_STATUS_ABSENT) {
        return NETWORK_STATUS_UNAVAILABLE;
    }
    if (status != VIRTIO_NET_STATUS_OK &&
        status != VIRTIO_NET_STATUS_LINK_DOWN) {
        console_write("Phipia: virtio-net initialization failed: ");
        console_write(virtio_net_status_string(status));
        console_putc('\n');
        return NETWORK_STATUS_UNAVAILABLE;
    }
    runtime.public.active = true;
    return status == VIRTIO_NET_STATUS_LINK_DOWN ?
        NETWORK_STATUS_LINK_DOWN : NETWORK_STATUS_OK;
}

enum network_status network_configure_static(
    uint32_t address,
    uint32_t subnet_mask,
    uint32_t gateway,
    uint32_t dns_server
)
{
    if (!runtime.public.active) {
        return NETWORK_STATUS_NOT_INITIALIZED;
    }
    if (!ipv4_is_unicast(address) || subnet_mask == 0U ||
        subnet_mask == BROADCAST_IPV4 ||
        (gateway != 0U && !ipv4_is_unicast(gateway)) ||
        (dns_server != 0U && !ipv4_is_unicast(dns_server))) {
        return NETWORK_STATUS_INVALID_ARGUMENT;
    }
    runtime.public.configuration.address = address;
    runtime.public.configuration.subnet_mask = subnet_mask;
    runtime.public.configuration.gateway = gateway;
    runtime.public.configuration.dns_server = dns_server;
    runtime.public.configuration.dhcp_server = 0U;
    runtime.public.configuration.lease_expires_ns = 0U;
    runtime.public.configuration.renewal_ns = 0U;
    runtime.public.configuration.rebinding_ns = 0U;
    ++runtime.public.configuration.generation;
    runtime.public.configuration.source = NETWORK_CONFIGURATION_STATIC;
    runtime.public.configuration.configured = true;
    arp_invalidate();
    zero_bytes(runtime.dns, sizeof(runtime.dns));
    runtime.public.dns_entries = 0U;
    return NETWORK_STATUS_OK;
}

static size_t build_dhcp(
    uint8_t *packet,
    size_t capacity,
    uint8_t message,
    uint32_t transaction,
    uint32_t requested,
    uint32_t server
)
{
    size_t offset = DHCP_FIXED_BYTES;

    if (capacity < DHCP_FIXED_BYTES + 32U) {
        return 0U;
    }
    zero_bytes(packet, capacity);
    packet[0] = 1U;
    packet[1] = 1U;
    packet[2] = 6U;
    write_be32(packet + 4U, transaction);
    write_be16(packet + 10U, UINT16_C(0x8000));
    copy_bytes(packet + 28U, runtime.public.device.mac, 6U);
    write_be32(packet + 236U, DHCP_MAGIC_COOKIE);
    packet[offset++] = DHCP_OPTION_MESSAGE;
    packet[offset++] = 1U;
    packet[offset++] = message;
    packet[offset++] = DHCP_OPTION_CLIENT;
    packet[offset++] = 7U;
    packet[offset++] = 1U;
    copy_bytes(packet + offset, runtime.public.device.mac, 6U);
    offset += 6U;
    if (requested != 0U) {
        packet[offset++] = DHCP_OPTION_REQUESTED;
        packet[offset++] = 4U;
        write_be32(packet + offset, requested);
        offset += 4U;
    }
    if (server != 0U) {
        packet[offset++] = DHCP_OPTION_SERVER;
        packet[offset++] = 4U;
        write_be32(packet + offset, server);
        offset += 4U;
    }
    packet[offset++] = DHCP_OPTION_PARAMETERS;
    packet[offset++] = 3U;
    packet[offset++] = DHCP_OPTION_SUBNET;
    packet[offset++] = DHCP_OPTION_ROUTER;
    packet[offset++] = DHCP_OPTION_DNS;
    packet[offset++] = DHCP_OPTION_END;
    return offset;
}

static enum network_status wait_dhcp(uint64_t deadline, uint8_t expected)
{
    while (clock_monotonic_ns() < deadline) {
        (void)network_service();
        if (!runtime.dhcp.received) {
            if (!network_wait_for_interrupt(deadline)) {
                return NETWORK_STATUS_NO_RESOURCES;
            }
            continue;
        }
        if (runtime.dhcp.message == DHCP_NAK) {
            return NETWORK_STATUS_DHCP_NAK;
        }
        if (runtime.dhcp.message == expected) {
            return NETWORK_STATUS_OK;
        }
        runtime.dhcp.received = false;
        if (!network_wait_for_interrupt(deadline)) {
            return NETWORK_STATUS_NO_RESOURCES;
        }
    }
    return NETWORK_STATUS_TIMEOUT;
}

enum network_status network_start_dhcp(uint64_t timeout_ns)
{
    uint8_t packet[DHCP_BUFFER_BYTES];
    uint64_t deadline;
    enum network_status status = NETWORK_STATUS_TIMEOUT;
    uint32_t offered;
    uint32_t server;

    if (!runtime.public.active) {
        return NETWORK_STATUS_NOT_INITIALIZED;
    }
    if (!runtime.public.device.link_up) {
        return NETWORK_STATUS_LINK_DOWN;
    }
    if (!deadline_valid(timeout_ns)) {
        return NETWORK_STATUS_INVALID_ARGUMENT;
    }
    if (!timer_acquire()) {
        return NETWORK_STATUS_NO_RESOURCES;
    }
    zero_bytes(&runtime.dhcp, sizeof(runtime.dhcp));
    runtime.dhcp.transaction = random_u32();
    if (runtime.dhcp.transaction == 0U) {
        runtime.dhcp.transaction = UINT32_C(0x5341504F);
    }
    runtime.dhcp.waiting = true;
    deadline = clock_monotonic_ns() + timeout_ns;
    for (size_t retry = 0U; retry < DHCP_RETRIES &&
         clock_monotonic_ns() < deadline; ++retry) {
        size_t length = build_dhcp(packet, sizeof(packet), DHCP_DISCOVER,
            runtime.dhcp.transaction, 0U, 0U);

        runtime.dhcp.received = false;
        if (udp_send_raw(0U, DHCP_CLIENT_PORT, BROADCAST_IPV4,
                DHCP_SERVER_PORT, packet, length, ARP_TIMEOUT_NS) !=
                NETWORK_STATUS_OK) {
            continue;
        }
        status = wait_dhcp(deadline, DHCP_OFFER);
        if (status == NETWORK_STATUS_OK || status == NETWORK_STATUS_DHCP_NAK ||
                status == NETWORK_STATUS_NO_RESOURCES) {
            break;
        }
    }
    if (status != NETWORK_STATUS_OK) {
        runtime.dhcp.waiting = false;
        timer_release();
        return status;
    }
    offered = runtime.dhcp.offered_address;
    server = runtime.dhcp.server;
    if (!ipv4_is_unicast(offered) || !ipv4_is_unicast(server)) {
        runtime.dhcp.waiting = false;
        timer_release();
        return NETWORK_STATUS_MALFORMED;
    }
    runtime.dhcp.received = false;
    runtime.dhcp.message = 0U;
    /* OFFER options are provisional.  Parse the ACK independently so that a
     * server cannot smuggle omitted ACK fields in from the earlier offer and
     * so duplicate-option rejection remains scoped to one message.
     */
    runtime.dhcp.subnet_mask = 0U;
    runtime.dhcp.router = 0U;
    runtime.dhcp.dns = 0U;
    runtime.dhcp.lease_seconds = 0U;
    runtime.dhcp.renewal_seconds = 0U;
    runtime.dhcp.rebinding_seconds = 0U;
    for (size_t retry = 0U; retry < DHCP_RETRIES &&
         clock_monotonic_ns() < deadline; ++retry) {
        size_t length = build_dhcp(packet, sizeof(packet), DHCP_REQUEST,
            runtime.dhcp.transaction, offered, server);

        if (udp_send_raw(0U, DHCP_CLIENT_PORT, BROADCAST_IPV4,
                DHCP_SERVER_PORT, packet, length, ARP_TIMEOUT_NS) !=
                NETWORK_STATUS_OK) {
            continue;
        }
        status = wait_dhcp(deadline, DHCP_ACK);
        if (status == NETWORK_STATUS_OK || status == NETWORK_STATUS_DHCP_NAK) {
            break;
        }
    }
    runtime.dhcp.waiting = false;
    if (status == NETWORK_STATUS_OK &&
        ipv4_is_unicast(runtime.dhcp.offered_address) &&
        runtime.dhcp.subnet_mask != 0U &&
        ipv4_is_unicast(runtime.dhcp.router) &&
        ipv4_is_unicast(runtime.dhcp.dns) &&
        runtime.dhcp.lease_seconds != 0U) {
        const uint64_t now = clock_monotonic_ns();
        const uint64_t lease = (uint64_t)runtime.dhcp.lease_seconds *
            UINT64_C(1000000000);
        const uint64_t renewal = runtime.dhcp.renewal_seconds != 0U ?
            (uint64_t)runtime.dhcp.renewal_seconds * UINT64_C(1000000000) :
            lease / 2U;
        const uint64_t rebinding = runtime.dhcp.rebinding_seconds != 0U ?
            (uint64_t)runtime.dhcp.rebinding_seconds * UINT64_C(1000000000) :
            lease * 7U / 8U;

        runtime.public.configuration.address = runtime.dhcp.offered_address;
        runtime.public.configuration.subnet_mask = runtime.dhcp.subnet_mask;
        runtime.public.configuration.gateway = runtime.dhcp.router;
        runtime.public.configuration.dns_server = runtime.dhcp.dns;
        runtime.public.configuration.dhcp_server = runtime.dhcp.server;
        runtime.public.configuration.lease_expires_ns = now + lease;
        runtime.public.configuration.renewal_ns = now + renewal;
        runtime.public.configuration.rebinding_ns = now + rebinding;
        ++runtime.public.configuration.generation;
        runtime.public.configuration.source = NETWORK_CONFIGURATION_DHCP;
        runtime.public.configuration.configured = true;
        arp_invalidate();
        zero_bytes(runtime.dns, sizeof(runtime.dns));
        runtime.public.dns_entries = 0U;
    } else if (status == NETWORK_STATUS_OK) {
        status = NETWORK_STATUS_MALFORMED;
    }
    timer_release();
    return status;
}

static bool hostname_valid(const char *hostname)
{
    const size_t length = string_length_bounded(hostname,
        NETWORK_MAX_HOSTNAME);
    size_t label = 0U;

    if (length == 0U || length > NETWORK_MAX_HOSTNAME ||
        hostname[0] == '.' || hostname[length - 1U] == '.') {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const char character = hostname[index];

        if (character == '.') {
            if (label == 0U || label > 63U) {
                return false;
            }
            label = 0U;
            continue;
        }
        if (!((character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') ||
                character == '-') ||
            (label == 0U && character == '-')) {
            return false;
        }
        ++label;
    }
    return label != 0U && label <= 63U && hostname[length - 1U] != '-';
}

static bool dns_name(
    const uint8_t *message,
    size_t length,
    size_t *offset,
    char *output,
    size_t capacity
)
{
    size_t cursor;
    size_t output_length = 0U;
    size_t return_offset = 0U;
    uint16_t visited[DNS_MAX_POINTERS];
    size_t visited_count = 0U;
    bool jumped = false;

    if (message == NULL || offset == NULL || output == NULL ||
        *offset >= length || capacity == 0U) {
        return false;
    }
    cursor = *offset;
    for (size_t steps = 0U; steps < NETWORK_MAX_HOSTNAME +
         DNS_MAX_POINTERS; ++steps) {
        uint8_t label;

        if (cursor >= length) {
            return false;
        }
        label = message[cursor++];
        if ((label & UINT8_C(0xC0)) == UINT8_C(0xC0)) {
            uint16_t pointer;

            if (cursor >= length) {
                return false;
            }
            pointer = (uint16_t)(((uint16_t)(label & UINT8_C(0x3F)) << 8U) |
                message[cursor++]);
            if (pointer >= length || visited_count >= DNS_MAX_POINTERS) {
                return false;
            }
            for (size_t index = 0U; index < visited_count; ++index) {
                if (visited[index] == pointer) {
                    return false;
                }
            }
            visited[visited_count++] = pointer;
            if (!jumped) {
                return_offset = cursor;
                jumped = true;
            }
            cursor = pointer;
            continue;
        }
        if ((label & UINT8_C(0xC0)) != 0U || label > 63U ||
            label > length - cursor) {
            return false;
        }
        if (label == 0U) {
            if (!jumped) {
                return_offset = cursor;
            }
            if (output_length == 0U) {
                return false;
            }
            output[output_length] = '\0';
            *offset = return_offset;
            return true;
        }
        if (output_length != 0U) {
            if (output_length + 1U >= capacity) {
                return false;
            }
            output[output_length++] = '.';
        }
        if (output_length + label >= capacity) {
            return false;
        }
        for (size_t index = 0U; index < label; ++index) {
            char character = (char)message[cursor + index];

            if (character >= 'A' && character <= 'Z') {
                character = (char)(character - 'A' + 'a');
            }
            output[output_length++] = character;
        }
        cursor += label;
    }
    return false;
}

static size_t dns_encode_name(
    uint8_t *message,
    size_t capacity,
    const char *hostname
)
{
    size_t input = 0U;
    size_t output = 0U;

    if (!hostname_valid(hostname)) {
        return 0U;
    }
    while (hostname[input] != '\0') {
        size_t label_start = input;
        size_t label_length;

        while (hostname[input] != '\0' && hostname[input] != '.') {
            ++input;
        }
        label_length = input - label_start;
        if (output + 1U + label_length >= capacity) {
            return 0U;
        }
        message[output++] = (uint8_t)label_length;
        for (size_t index = 0U; index < label_length; ++index) {
            char character = hostname[label_start + index];

            if (character >= 'A' && character <= 'Z') {
                character = (char)(character - 'A' + 'a');
            }
            message[output++] = (uint8_t)character;
        }
        if (hostname[input] == '.') {
            ++input;
        }
    }
    message[output++] = 0U;
    return output;
}

static struct dns_cache_entry *dns_cache_find(const char *hostname)
{
    const uint64_t now = clock_monotonic_ns();

    for (size_t index = 0U; index < NETWORK_DNS_CACHE_SIZE; ++index) {
        struct dns_cache_entry *entry = &runtime.dns[index];

        if (!entry->active || entry->expires_ns <= now ||
            entry->configuration_generation !=
                runtime.public.configuration.generation ||
            entry->device_generation !=
                runtime.public.device.device_generation) {
            entry->active = false;
            continue;
        }
        if (string_equal(entry->name, hostname)) {
            return entry;
        }
    }
    return NULL;
}

static void dns_cache_insert(
    const char *hostname,
    uint32_t address,
    uint32_t ttl,
    bool negative
)
{
    struct dns_cache_entry *slot = &runtime.dns[0];
    const size_t length = string_length_bounded(hostname,
        NETWORK_MAX_HOSTNAME);

    for (size_t index = 0U; index < NETWORK_DNS_CACHE_SIZE; ++index) {
        if (!runtime.dns[index].active) {
            slot = &runtime.dns[index];
            break;
        }
        if (runtime.dns[index].insertion < slot->insertion) {
            slot = &runtime.dns[index];
        }
    }
    zero_bytes(slot, sizeof(*slot));
    string_copy(slot->name, hostname, length);
    slot->address = address;
    slot->expires_ns = clock_monotonic_ns() +
        (uint64_t)(ttl == 0U ? 30U : ttl) * UINT64_C(1000000000);
    slot->configuration_generation =
        runtime.public.configuration.generation;
    slot->device_generation = runtime.public.device.device_generation;
    slot->insertion = ++runtime.dns_insertion;
    slot->active = true;
    slot->negative = negative;
    runtime.public.dns_entries = 0U;
    for (size_t index = 0U; index < NETWORK_DNS_CACHE_SIZE; ++index) {
        if (runtime.dns[index].active) {
            ++runtime.public.dns_entries;
        }
    }
}

static enum network_status dns_parse_response(
    uint32_t source,
    const uint8_t *bytes,
    size_t length
)
{
    uint16_t flags;
    uint16_t questions;
    uint16_t answers;
    uint16_t authority;
    uint16_t additional;
    size_t offset = DNS_HEADER_BYTES;
    char question[NETWORK_MAX_HOSTNAME + 1U];
    char target[NETWORK_MAX_HOSTNAME + 1U];
    uint32_t answer_address = 0U;
    uint32_t answer_ttl = 0U;
    size_t cname_follows = 0U;

    if (!runtime.dns_query.waiting ||
        source != runtime.public.configuration.dns_server || length < 2U ||
        read_be16(bytes + 0U) != runtime.dns_query.identifier) {
        return NETWORK_STATUS_DNS_FAILURE;
    }
    if (length < DNS_HEADER_BYTES || length > NETWORK_MAX_DNS_MESSAGE) {
        ++runtime.public.statistics.malformed_packets;
        runtime.dns_query.status = NETWORK_STATUS_DNS_FAILURE;
        runtime.dns_query.received = true;
        return NETWORK_STATUS_DNS_FAILURE;
    }
    flags = read_be16(bytes + 2U);
    questions = read_be16(bytes + 4U);
    answers = read_be16(bytes + 6U);
    authority = read_be16(bytes + 8U);
    additional = read_be16(bytes + 10U);
    if ((flags & UINT16_C(0x8000)) == 0U ||
        (flags & UINT16_C(0x0200)) != 0U ||
        (flags & UINT16_C(0x7800)) != 0U || questions != 1U ||
        (size_t)answers + authority + additional > NETWORK_HTTP_MAX_HEADERS) {
        runtime.dns_query.status = NETWORK_STATUS_DNS_FAILURE;
        runtime.dns_query.received = true;
        return NETWORK_STATUS_DNS_FAILURE;
    }
    if ((flags & UINT16_C(0x000F)) != 0U) {
        runtime.dns_query.status = NETWORK_STATUS_DNS_FAILURE;
        runtime.dns_query.received = true;
        dns_cache_insert(runtime.dns_query.question, 0U, 30U, true);
        return NETWORK_STATUS_DNS_FAILURE;
    }
    if (!dns_name(bytes, length, &offset, question, sizeof(question)) ||
        offset > length || length - offset < 4U ||
        read_be16(bytes + offset) != DNS_TYPE_A ||
        read_be16(bytes + offset + 2U) != DNS_CLASS_IN ||
        !string_equal(question, runtime.dns_query.question)) {
        runtime.dns_query.status = NETWORK_STATUS_DNS_FAILURE;
        runtime.dns_query.received = true;
        return NETWORK_STATUS_DNS_FAILURE;
    }
    offset += 4U;
    string_copy(target, question, string_length_bounded(question,
        NETWORK_MAX_HOSTNAME));
    for (size_t record = 0U; record < (size_t)answers + authority + additional;
         ++record) {
        char owner[NETWORK_MAX_HOSTNAME + 1U];
        uint16_t type;
        uint16_t class_value;
        uint32_t ttl;
        uint16_t data_length;
        size_t data_offset;

        if (!dns_name(bytes, length, &offset, owner, sizeof(owner)) ||
            offset > length || length - offset < 10U) {
            runtime.dns_query.status = NETWORK_STATUS_DNS_FAILURE;
            runtime.dns_query.received = true;
            return NETWORK_STATUS_DNS_FAILURE;
        }
        type = read_be16(bytes + offset + 0U);
        class_value = read_be16(bytes + offset + 2U);
        ttl = read_be32(bytes + offset + 4U);
        data_length = read_be16(bytes + offset + 8U);
        offset += 10U;
        if (data_length > length - offset) {
            runtime.dns_query.status = NETWORK_STATUS_DNS_FAILURE;
            runtime.dns_query.received = true;
            return NETWORK_STATUS_DNS_FAILURE;
        }
        data_offset = offset;
        if (class_value == DNS_CLASS_IN && type == DNS_TYPE_CNAME &&
            string_equal(owner, target)) {
            char cname[NETWORK_MAX_HOSTNAME + 1U];
            size_t name_offset = data_offset;

            if (++cname_follows > DNS_MAX_CNAME_FOLLOWS ||
                !dns_name(bytes, length, &name_offset, cname,
                    sizeof(cname)) || name_offset > data_offset + data_length) {
                runtime.dns_query.status = NETWORK_STATUS_DNS_FAILURE;
                runtime.dns_query.received = true;
                return NETWORK_STATUS_DNS_FAILURE;
            }
            string_copy(target, cname, string_length_bounded(cname,
                NETWORK_MAX_HOSTNAME));
        } else if (class_value == DNS_CLASS_IN && type == DNS_TYPE_A &&
            data_length == 4U && string_equal(owner, target)) {
            answer_address = read_be32(bytes + data_offset);
            answer_ttl = ttl;
        }
        offset += data_length;
    }
    if (!ipv4_is_unicast(answer_address)) {
        runtime.dns_query.status = NETWORK_STATUS_DNS_FAILURE;
    } else {
        runtime.dns_query.address = answer_address;
        runtime.dns_query.ttl = answer_ttl;
        runtime.dns_query.status = NETWORK_STATUS_OK;
        ++runtime.public.statistics.dns_accepted;
    }
    runtime.dns_query.received = true;
    return runtime.dns_query.status;
}

enum network_status network_resolve(
    const char *hostname,
    uint32_t *address,
    uint64_t timeout_ns
)
{
    uint8_t query[NETWORK_MAX_DNS_MESSAGE];
    size_t encoded;
    size_t length;
    uint64_t deadline;
    struct dns_cache_entry *cached;
    enum network_status status = NETWORK_STATUS_TIMEOUT;

    if (address == NULL || hostname == NULL) {
        return NETWORK_STATUS_NULL_ARGUMENT;
    }
    if (network_parse_ipv4(hostname, address)) {
        return NETWORK_STATUS_OK;
    }
    if (!runtime.public.active) {
        return NETWORK_STATUS_NOT_INITIALIZED;
    }
    if (!runtime.public.configuration.configured ||
        runtime.public.configuration.dns_server == 0U) {
        return NETWORK_STATUS_UNCONFIGURED;
    }
    if (!hostname_valid(hostname) || !deadline_valid(timeout_ns)) {
        return NETWORK_STATUS_INVALID_ARGUMENT;
    }
    cached = dns_cache_find(hostname);
    if (cached != NULL) {
        if (cached->negative) {
            return NETWORK_STATUS_DNS_FAILURE;
        }
        *address = cached->address;
        return NETWORK_STATUS_OK;
    }
    if (!timer_acquire()) {
        return NETWORK_STATUS_NO_RESOURCES;
    }
    zero_bytes(&runtime.dns_query, sizeof(runtime.dns_query));
    runtime.dns_query.identifier = random_u16();
    runtime.dns_query.local_port = (uint16_t)(TCP_EPHEMERAL_FIRST +
        random_u16() % (TCP_EPHEMERAL_LAST - TCP_EPHEMERAL_FIRST + 1U));
    string_copy(runtime.dns_query.question, hostname,
        string_length_bounded(hostname, NETWORK_MAX_HOSTNAME));
    runtime.dns_query.waiting = true;
    zero_bytes(query, sizeof(query));
    write_be16(query + 0U, runtime.dns_query.identifier);
    write_be16(query + 2U, UINT16_C(0x0100));
    write_be16(query + 4U, 1U);
    encoded = dns_encode_name(query + DNS_HEADER_BYTES,
        sizeof(query) - DNS_HEADER_BYTES, hostname);
    if (encoded == 0U || DNS_HEADER_BYTES + encoded + 4U > sizeof(query)) {
        runtime.dns_query.waiting = false;
        timer_release();
        return NETWORK_STATUS_INVALID_ARGUMENT;
    }
    length = DNS_HEADER_BYTES + encoded;
    write_be16(query + length, DNS_TYPE_A);
    write_be16(query + length + 2U, DNS_CLASS_IN);
    length += 4U;
    deadline = clock_monotonic_ns() + timeout_ns;
    for (size_t retry = 0U; retry < 3U && clock_monotonic_ns() < deadline;
         ++retry) {
        runtime.dns_query.received = false;
        status = udp_send_raw(runtime.public.configuration.address,
            runtime.dns_query.local_port,
            runtime.public.configuration.dns_server, DNS_SERVER_PORT,
            query, length, ARP_TIMEOUT_NS);
        if (status != NETWORK_STATUS_OK) {
            continue;
        }
        const uint64_t attempt_end = clock_monotonic_ns() +
            UINT64_C(700000000);
        while (clock_monotonic_ns() < deadline &&
            clock_monotonic_ns() < attempt_end) {
            (void)network_service();
            if (runtime.dns_query.received) {
                status = runtime.dns_query.status;
                break;
            }
            if (clock_monotonic_ns() < deadline &&
                clock_monotonic_ns() < attempt_end) {
                if (!network_wait_for_interrupt(attempt_end < deadline ?
                        attempt_end : deadline)) {
                    runtime.dns_query.waiting = false;
                    timer_release();
                    return NETWORK_STATUS_NO_RESOURCES;
                }
            }
        }
        if (runtime.dns_query.received) {
            break;
        }
        status = NETWORK_STATUS_TIMEOUT;
    }
    runtime.dns_query.waiting = false;
    if (status == NETWORK_STATUS_OK) {
        *address = runtime.dns_query.address;
        dns_cache_insert(hostname, *address, runtime.dns_query.ttl, false);
    }
    timer_release();
    return status;
}

static bool sequence_before(uint32_t left, uint32_t right)
{
    return (int32_t)(left - right) < 0;
}

static bool tcp_port_used(uint16_t port)
{
    for (size_t index = 0U; index < NETWORK_MAX_TCP_CONNECTIONS; ++index) {
        if (runtime.tcp[index].active && runtime.tcp[index].local_port == port) {
            return true;
        }
    }
    return false;
}

static uint16_t tcp_ephemeral(void)
{
    const uint32_t range = (uint32_t)TCP_EPHEMERAL_LAST -
        TCP_EPHEMERAL_FIRST + 1U;

    for (uint32_t attempt = 0U; attempt < range; ++attempt) {
        uint16_t port = runtime.next_ephemeral++;

        if (runtime.next_ephemeral < TCP_EPHEMERAL_FIRST) {
            runtime.next_ephemeral = TCP_EPHEMERAL_FIRST;
        }
        if (!tcp_port_used(port)) {
            return port;
        }
    }
    return 0U;
}

static enum network_status tcp_emit(
    struct tcp_connection *connection,
    uint8_t flags,
    const uint8_t *payload,
    size_t payload_length,
    uint32_t sequence,
    bool advance,
    bool track
)
{
    uint8_t segment[TCP_HEADER_BYTES + 4U + NETWORK_TCP_TX_BYTES];
    size_t header_length = TCP_HEADER_BYTES;
    size_t total;
    uint16_t checksum;
    enum network_status status;

    if (connection == NULL || payload_length > NETWORK_TCP_TX_BYTES ||
        (payload == NULL && payload_length != 0U)) {
        return NETWORK_STATUS_INVALID_ARGUMENT;
    }
    zero_bytes(segment, sizeof(segment));
    if ((flags & TCP_FLAG_SYN) != 0U) {
        header_length += 4U;
        segment[TCP_HEADER_BYTES + 0U] = 2U;
        segment[TCP_HEADER_BYTES + 1U] = 4U;
        write_be16(segment + TCP_HEADER_BYTES + 2U, TCP_MSS);
    }
    total = header_length + payload_length;
    write_be16(segment + 0U, connection->local_port);
    write_be16(segment + 2U, connection->remote_port);
    write_be32(segment + 4U, sequence);
    write_be32(segment + 8U, connection->receive_next);
    segment[12] = (uint8_t)((header_length / 4U) << 4U);
    segment[13] = flags;
    write_be16(segment + 14U,
        connection->receive_bytes >= NETWORK_TCP_RX_BYTES ? 0U :
        (uint16_t)(NETWORK_TCP_RX_BYTES - connection->receive_bytes));
    copy_bytes(segment + header_length, payload, payload_length);
    checksum = transport_checksum(runtime.public.configuration.address,
        connection->remote_address, IPV4_PROTOCOL_TCP, segment, total);
    write_be16(segment + 16U, checksum);
    status = ipv4_send(runtime.public.configuration.address,
        connection->remote_address, IPV4_PROTOCOL_TCP, segment, total,
        ARP_TIMEOUT_NS);
    if (status != NETWORK_STATUS_OK) {
        return status;
    }
    if (track) {
        copy_bytes(connection->retransmit, payload, payload_length);
        connection->retransmit_bytes = payload_length;
        connection->retransmit_flags = flags;
        connection->retransmit_count = 0U;
        connection->retransmit_deadline_ns = clock_monotonic_ns() +
            TCP_RETRANSMISSION_NS;
    }
    if (advance) {
        connection->send_next += (uint32_t)payload_length;
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) != 0U) {
            ++connection->send_next;
        }
    }
    return NETWORK_STATUS_OK;
}

static enum network_status tcp_ack(struct tcp_connection *connection)
{
    return tcp_emit(connection, TCP_FLAG_ACK, NULL, 0U,
        connection->send_next, false, false);
}

static enum network_status tcp_retransmit(struct tcp_connection *connection)
{
    if (connection->retransmit_flags == 0U ||
        connection->retransmit_count >= NETWORK_TCP_RETRANSMISSION_LIMIT) {
        connection->error = NETWORK_STATUS_TIMEOUT;
        return NETWORK_STATUS_TIMEOUT;
    }
    ++connection->retransmit_count;
    ++runtime.public.statistics.tcp_retransmissions;
    connection->retransmit_deadline_ns = clock_monotonic_ns() +
        TCP_RETRANSMISSION_NS;
    return tcp_emit(connection, connection->retransmit_flags,
        connection->retransmit, connection->retransmit_bytes,
        connection->send_unacknowledged, false, false);
}

static struct tcp_connection *tcp_match(
    uint32_t source,
    uint16_t source_port,
    uint16_t destination_port
)
{
    for (size_t index = 0U; index < NETWORK_MAX_TCP_CONNECTIONS; ++index) {
        struct tcp_connection *connection = &runtime.tcp[index];

        if (connection->active &&
            connection->state != TCP_CONNECTION_LISTEN &&
            connection->remote_address == source &&
            connection->remote_port == source_port &&
            connection->local_port == destination_port) {
            return connection;
        }
    }
    return NULL;
}

static struct tcp_connection *tcp_listener_for(uint16_t destination_port)
{
    for (size_t index = 0U; index < NETWORK_MAX_TCP_CONNECTIONS; ++index) {
        struct tcp_connection *connection = &runtime.tcp[index];

        if (connection->active &&
            connection->state == TCP_CONNECTION_LISTEN &&
            connection->local_port == destination_port &&
            !connection->cancelled) {
            return connection;
        }
    }
    return NULL;
}

static size_t tcp_index_of(const struct tcp_connection *connection)
{
    return (size_t)(connection - &runtime.tcp[0]);
}

static size_t tcp_pending_count(const struct tcp_connection *listener)
{
    const size_t parent = tcp_index_of(listener);
    size_t count = 0U;

    for (size_t index = 0U; index < NETWORK_MAX_TCP_CONNECTIONS; ++index) {
        const struct tcp_connection *connection = &runtime.tcp[index];

        if (connection->active && connection->pending &&
            connection->listener == parent) {
            ++count;
        }
    }
    return count;
}

/*
 * A segment nobody is listening for is answered, not swallowed. RFC 793
 * section 3.4 gives the sequence numbers: a segment carrying an
 * acknowledgement is refused at that acknowledgement, and one without is
 * refused with an acknowledgement of everything it occupied. A reset is never
 * answered with a reset, which is what keeps two closed ports from talking
 * forever.
 */
static void tcp_refuse(
    uint32_t peer,
    uint16_t peer_port,
    uint16_t local_port,
    uint32_t sequence,
    uint32_t acknowledgment,
    uint8_t flags,
    size_t payload_length
)
{
    uint8_t segment[TCP_HEADER_BYTES];
    uint32_t reset_sequence = 0U;
    uint32_t reset_acknowledgment = 0U;
    uint8_t reset_flags = TCP_FLAG_RST;

    if ((flags & TCP_FLAG_RST) != 0U ||
        !runtime.public.configuration.configured ||
        !ipv4_is_unicast(peer)) {
        return;
    }
    if ((flags & TCP_FLAG_ACK) != 0U) {
        reset_sequence = acknowledgment;
    } else {
        reset_acknowledgment = sequence + (uint32_t)payload_length +
            (((flags & TCP_FLAG_SYN) != 0U) ? 1U : 0U) +
            (((flags & TCP_FLAG_FIN) != 0U) ? 1U : 0U);
        reset_flags |= TCP_FLAG_ACK;
    }
    zero_bytes(segment, sizeof(segment));
    write_be16(segment + 0U, local_port);
    write_be16(segment + 2U, peer_port);
    write_be32(segment + 4U, reset_sequence);
    write_be32(segment + 8U, reset_acknowledgment);
    segment[12] = (uint8_t)((TCP_HEADER_BYTES / 4U) << 4U);
    segment[13] = reset_flags;
    write_be16(segment + 16U, transport_checksum(
        runtime.public.configuration.address, peer, IPV4_PROTOCOL_TCP,
        segment, sizeof(segment)));
    if (ipv4_send(runtime.public.configuration.address, peer,
            IPV4_PROTOCOL_TCP, segment, sizeof(segment),
            ARP_TIMEOUT_NS) == NETWORK_STATUS_OK) {
        ++runtime.public.statistics.tcp_refusals;
    }
}

/*
 * A listener's answer to a SYN is a whole connection, so it costs a slot and
 * is bounded twice: by the listener's declared backlog and by the connection
 * table itself. The acknowledgement is armed before it is sent, because the
 * send can legitimately fail -- the peer's hardware address may not be known
 * yet -- and the handshake must survive that by retransmission rather than by
 * blocking inside the receive path.
 */
static struct tcp_connection *tcp_open_child(
    struct tcp_connection *listener,
    uint32_t source,
    uint16_t source_port,
    uint32_t sequence,
    uint16_t peer_window
)
{
    if (tcp_pending_count(listener) >= listener->backlog) {
        return NULL;
    }
    for (size_t index = 0U; index < NETWORK_MAX_TCP_CONNECTIONS; ++index) {
        struct tcp_connection *connection = &runtime.tcp[index];

        if (connection->active) {
            continue;
        }
        zero_bytes(connection, sizeof(*connection));
        connection->owner = listener->owner;
        connection->generation = allocate_generation();
        connection->device_generation =
            runtime.public.device.device_generation;
        connection->remote_address = source;
        connection->remote_port = source_port;
        connection->local_port = listener->local_port;
        connection->peer_window = peer_window;
        connection->peer_mss = TCP_MSS;
        connection->receive_next = sequence + 1U;
        connection->send_unacknowledged = random_u32();
        connection->send_next = connection->send_unacknowledged + 1U;
        connection->state = TCP_CONNECTION_SYN_RECEIVED;
        connection->listener = (uint8_t)tcp_index_of(listener);
        connection->pending = true;
        connection->active = true;
        connection->retransmit_flags = TCP_FLAG_SYN | TCP_FLAG_ACK;
        connection->retransmit_bytes = 0U;
        connection->retransmit_count = 0U;
        connection->retransmit_deadline_ns = clock_monotonic_ns() +
            TCP_RETRANSMISSION_NS;
        ++runtime.public.tcp_connections;
        (void)tcp_emit(connection, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0U,
            connection->send_unacknowledged, false, false);
        return connection;
    }
    ++runtime.public.statistics.socket_exhaustion;
    return NULL;
}

static void tcp_release(struct tcp_connection *connection)
{
    zero_bytes(connection, sizeof(*connection));
    if (runtime.public.tcp_connections != 0U) {
        --runtime.public.tcp_connections;
    }
}

static void tcp_release_children(const struct tcp_connection *listener)
{
    const size_t parent = tcp_index_of(listener);

    for (size_t slot = 0U; slot < NETWORK_MAX_TCP_CONNECTIONS; ++slot) {
        struct tcp_connection *child = &runtime.tcp[slot];

        if (!child->active || !child->pending || child->listener != parent) {
            continue;
        }
        tcp_refuse(child->remote_address, child->remote_port,
            child->local_port, child->receive_next, child->send_next,
            TCP_FLAG_ACK, 0U);
        tcp_release(child);
    }
}

static void tcp_receive_segment(
    uint32_t source,
    uint32_t destination,
    const uint8_t *bytes,
    size_t length
)
{
    uint16_t source_port;
    uint16_t destination_port;
    uint8_t header_length;
    uint8_t flags;
    uint32_t sequence;
    uint32_t acknowledgment;
    size_t payload_length;
    struct tcp_connection *connection;

    if (destination != runtime.public.configuration.address ||
        length < TCP_HEADER_BYTES || transport_checksum(source, destination,
            IPV4_PROTOCOL_TCP, bytes, length) != 0U) {
        ++runtime.public.statistics.malformed_packets;
        return;
    }
    source_port = read_be16(bytes + 0U);
    destination_port = read_be16(bytes + 2U);
    sequence = read_be32(bytes + 4U);
    acknowledgment = read_be32(bytes + 8U);
    header_length = (uint8_t)((bytes[12] >> 4U) * 4U);
    flags = bytes[13];
    if (source_port == 0U || destination_port == 0U ||
        header_length < TCP_HEADER_BYTES || header_length > length ||
        (flags & UINT8_C(0xC0)) != 0U ||
        ((flags & TCP_FLAG_SYN) != 0U &&
            (flags & (TCP_FLAG_FIN | TCP_FLAG_RST)) != 0U)) {
        ++runtime.public.statistics.malformed_packets;
        return;
    }
    payload_length = length - header_length;
    connection = tcp_match(source, source_port, destination_port);
    if (connection == NULL) {
        struct tcp_connection *listener = NULL;

        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == TCP_FLAG_SYN &&
            ipv4_is_unicast(source)) {
            listener = tcp_listener_for(destination_port);
        }
        if (listener == NULL ||
            tcp_open_child(listener, source, source_port, sequence,
                read_be16(bytes + 14U)) == NULL) {
            tcp_refuse(source, source_port, destination_port, sequence,
                acknowledgment, flags, payload_length);
            return;
        }
        ++runtime.public.statistics.tcp_accepted;
        return;
    }
    ++runtime.public.statistics.tcp_accepted;
    connection->peer_window = read_be16(bytes + 14U);
    if ((flags & TCP_FLAG_RST) != 0U) {
        connection->state = TCP_CONNECTION_RESET;
        connection->error = NETWORK_STATUS_CONNECTION_RESET;
        return;
    }
    if (connection->state == TCP_CONNECTION_LISTEN) {
        return;
    }
    if (connection->state == TCP_CONNECTION_SYN_RECEIVED) {
        /*
         * Only the acknowledgement this side asked for finishes a passive
         * open: the exact sequence the SYN acknowledged, and the exact
         * sequence number the peer's SYN promised. Anything else is a stray
         * segment wearing an established connection's four-tuple.
         */
        if ((flags & TCP_FLAG_ACK) == 0U ||
            acknowledgment != connection->send_next ||
            sequence != connection->receive_next) {
            return;
        }
        connection->send_unacknowledged = acknowledgment;
        connection->retransmit_flags = 0U;
        connection->retransmit_bytes = 0U;
        connection->state = TCP_CONNECTION_ESTABLISHED;
        ++runtime.public.statistics.tcp_passive_opens;
    }
    if (connection->state == TCP_CONNECTION_SYN_SENT) {
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) !=
                (TCP_FLAG_SYN | TCP_FLAG_ACK) ||
            acknowledgment != connection->send_next) {
            return;
        }
        connection->send_unacknowledged = acknowledgment;
        connection->receive_next = sequence + 1U;
        connection->retransmit_flags = 0U;
        connection->state = TCP_CONNECTION_ESTABLISHED;
        (void)tcp_ack(connection);
        return;
    }
    if ((flags & TCP_FLAG_ACK) != 0U &&
        !sequence_before(acknowledgment, connection->send_unacknowledged) &&
        !sequence_before(connection->send_next, acknowledgment)) {
        connection->send_unacknowledged = acknowledgment;
        if (acknowledgment == connection->send_next) {
            connection->retransmit_flags = 0U;
            connection->retransmit_bytes = 0U;
        }
    }
    if (payload_length != 0U || (flags & TCP_FLAG_FIN) != 0U) {
        if (sequence != connection->receive_next) {
            (void)tcp_ack(connection);
            return;
        }
        if (payload_length > NETWORK_TCP_RX_BYTES -
                connection->receive_bytes) {
            connection->error = NETWORK_STATUS_NO_RESOURCES;
            (void)tcp_ack(connection);
            return;
        }
        copy_bytes(connection->receive + connection->receive_bytes,
            bytes + header_length, payload_length);
        connection->receive_bytes += payload_length;
        connection->receive_next += (uint32_t)payload_length;
        if ((flags & TCP_FLAG_FIN) != 0U) {
            ++connection->receive_next;
            connection->peer_closed = true;
            connection->state = connection->fin_sent ?
                TCP_CONNECTION_CLOSED : TCP_CONNECTION_CLOSE_WAIT;
        }
        (void)tcp_ack(connection);
    }
}

static enum network_status tcp_wait_ack(
    struct tcp_connection *connection,
    uint64_t deadline
)
{
    while (clock_monotonic_ns() < deadline) {
        (void)network_service();
        if (connection->cancelled) {
            return NETWORK_STATUS_CANCELLED;
        }
        if (connection->error != NETWORK_STATUS_OK) {
            return connection->error;
        }
        if (connection->send_unacknowledged == connection->send_next) {
            return NETWORK_STATUS_OK;
        }
        if (clock_monotonic_ns() >= connection->retransmit_deadline_ns &&
            tcp_retransmit(connection) != NETWORK_STATUS_OK) {
            return connection->error;
        }
        if (connection->send_unacknowledged != connection->send_next &&
            clock_monotonic_ns() < deadline) {
            if (!network_wait_for_interrupt(deadline)) {
                return NETWORK_STATUS_NO_RESOURCES;
            }
        }
    }
    return NETWORK_STATUS_TIMEOUT;
}

enum network_status network_udp_open(uint64_t owner, network_handle *handle)
{
    if (handle == NULL || owner == 0U) {
        return NETWORK_STATUS_NULL_ARGUMENT;
    }
    if (!runtime.public.active) {
        return NETWORK_STATUS_NOT_INITIALIZED;
    }
    for (size_t index = 0U; index < NETWORK_MAX_UDP_SOCKETS; ++index) {
        struct udp_socket *socket = &runtime.udp[index];

        if (!socket->active) {
            zero_bytes(socket, sizeof(*socket));
            socket->owner = owner;
            socket->generation = allocate_generation();
            socket->device_generation =
                runtime.public.device.device_generation;
            socket->active = true;
            *handle = make_handle(HANDLE_KIND_UDP, index,
                socket->generation);
            ++runtime.public.udp_sockets;
            return NETWORK_STATUS_OK;
        }
    }
    ++runtime.public.statistics.socket_exhaustion;
    return NETWORK_STATUS_NO_RESOURCES;
}

enum network_status network_udp_bind(
    uint64_t owner,
    network_handle handle,
    uint16_t port
)
{
    enum network_status status;
    struct udp_socket *socket = udp_for(owner, handle, &status);

    if (socket == NULL) {
        return status;
    }
    if (port == 0U || port == DHCP_CLIENT_PORT) {
        return NETWORK_STATUS_INVALID_ARGUMENT;
    }
    if (socket->bound) {
        return NETWORK_STATUS_ALREADY_BOUND;
    }
    for (size_t index = 0U; index < NETWORK_MAX_UDP_SOCKETS; ++index) {
        if (runtime.udp[index].active && runtime.udp[index].bound &&
            runtime.udp[index].port == port) {
            return NETWORK_STATUS_PORT_IN_USE;
        }
    }
    socket->port = port;
    socket->bound = true;
    return NETWORK_STATUS_OK;
}

enum network_status network_udp_send(
    uint64_t owner,
    network_handle handle,
    uint32_t destination,
    uint16_t port,
    const uint8_t *bytes,
    size_t length,
    uint64_t timeout_ns
)
{
    enum network_status status;
    struct udp_socket *socket = udp_for(owner, handle, &status);

    if (socket == NULL) {
        return status;
    }
    if (!socket->bound) {
        return NETWORK_STATUS_WRONG_MODE;
    }
    if (socket->cancelled) {
        return NETWORK_STATUS_CANCELLED;
    }
    return udp_send_raw(runtime.public.configuration.address, socket->port,
        destination, port, bytes, length, timeout_ns);
}

enum network_status network_udp_receive(
    uint64_t owner,
    network_handle handle,
    uint32_t *source,
    uint16_t *port,
    uint8_t *bytes,
    size_t capacity,
    size_t *length,
    uint64_t timeout_ns
)
{
    enum network_status status;
    struct udp_socket *socket = udp_for(owner, handle, &status);
    uint64_t deadline;

    if (socket == NULL) {
        return status;
    }
    if (source == NULL || port == NULL || bytes == NULL || length == NULL ||
        !deadline_valid(timeout_ns)) {
        return NETWORK_STATUS_NULL_ARGUMENT;
    }
    if (!timer_acquire()) {
        return NETWORK_STATUS_NO_RESOURCES;
    }
    deadline = clock_monotonic_ns() + timeout_ns;
    while (socket->queue_count == 0U && clock_monotonic_ns() < deadline) {
        (void)network_service();
        if (socket->cancelled) {
            timer_release();
            return NETWORK_STATUS_CANCELLED;
        }
        if (socket->error != NETWORK_STATUS_OK) {
            timer_release();
            return socket->error;
        }
        if (socket->queue_count == 0U && clock_monotonic_ns() < deadline) {
            if (!network_wait_for_interrupt(deadline)) {
                timer_release();
                return NETWORK_STATUS_NO_RESOURCES;
            }
        }
    }
    if (socket->queue_count == 0U) {
        timer_release();
        return NETWORK_STATUS_TIMEOUT;
    }
    struct udp_datagram *datagram = &socket->queue[socket->queue_head];

    if (datagram->length > capacity) {
        timer_release();
        return NETWORK_STATUS_TOO_LARGE;
    }
    *source = datagram->source;
    *port = datagram->port;
    *length = datagram->length;
    copy_bytes(bytes, datagram->bytes, datagram->length);
    zero_bytes(datagram, sizeof(*datagram));
    socket->queue_head = (socket->queue_head + 1U) %
        NETWORK_UDP_QUEUE_DEPTH;
    --socket->queue_count;
    timer_release();
    return NETWORK_STATUS_OK;
}

enum network_status network_tcp_open(uint64_t owner, network_handle *handle)
{
    if (handle == NULL || owner == 0U) {
        return NETWORK_STATUS_NULL_ARGUMENT;
    }
    if (!runtime.public.active) {
        return NETWORK_STATUS_NOT_INITIALIZED;
    }
    for (size_t index = 0U; index < NETWORK_MAX_TCP_CONNECTIONS; ++index) {
        struct tcp_connection *connection = &runtime.tcp[index];

        if (!connection->active) {
            zero_bytes(connection, sizeof(*connection));
            connection->owner = owner;
            connection->generation = allocate_generation();
            connection->device_generation =
                runtime.public.device.device_generation;
            connection->state = TCP_CONNECTION_OPEN;
            connection->peer_mss = TCP_MSS;
            connection->active = true;
            *handle = make_handle(HANDLE_KIND_TCP, index,
                connection->generation);
            ++runtime.public.tcp_connections;
            return NETWORK_STATUS_OK;
        }
    }
    ++runtime.public.statistics.socket_exhaustion;
    return NETWORK_STATUS_NO_RESOURCES;
}

enum network_status network_tcp_connect(
    uint64_t owner,
    network_handle handle,
    uint32_t destination,
    uint16_t port,
    uint64_t timeout_ns
)
{
    enum network_status status;
    struct tcp_connection *connection = tcp_for(owner, handle, &status);
    uint64_t deadline;
    uint8_t mac[6];

    if (connection == NULL) {
        return status;
    }
    if (connection->state != TCP_CONNECTION_OPEN ||
        !ipv4_is_unicast(destination) || port == 0U ||
        !deadline_valid(timeout_ns)) {
        return NETWORK_STATUS_INVALID_ARGUMENT;
    }
    status = arp_resolve(destination, mac, timeout_ns);
    if (status != NETWORK_STATUS_OK) {
        return status;
    }
    connection->local_port = tcp_ephemeral();
    if (connection->local_port == 0U) {
        return NETWORK_STATUS_NO_RESOURCES;
    }
    connection->remote_address = destination;
    connection->remote_port = port;
    connection->send_unacknowledged = random_u32();
    connection->send_next = connection->send_unacknowledged;
    connection->state = TCP_CONNECTION_SYN_SENT;
    if (!timer_acquire()) {
        connection->state = TCP_CONNECTION_OPEN;
        return NETWORK_STATUS_NO_RESOURCES;
    }
    status = tcp_emit(connection, TCP_FLAG_SYN, NULL, 0U,
        connection->send_next, true, true);
    if (status != NETWORK_STATUS_OK) {
        timer_release();
        return status;
    }
    deadline = clock_monotonic_ns() + timeout_ns;
    while (clock_monotonic_ns() < deadline &&
        connection->state == TCP_CONNECTION_SYN_SENT) {
        (void)network_service();
        if (connection->cancelled) {
            status = NETWORK_STATUS_CANCELLED;
            break;
        }
        if (connection->error != NETWORK_STATUS_OK) {
            status = connection->error;
            break;
        }
        if (clock_monotonic_ns() >= connection->retransmit_deadline_ns &&
            tcp_retransmit(connection) != NETWORK_STATUS_OK) {
            status = connection->error;
            break;
        }
        if (connection->state == TCP_CONNECTION_SYN_SENT &&
            clock_monotonic_ns() < deadline) {
            if (!network_wait_for_interrupt(deadline)) {
                status = NETWORK_STATUS_NO_RESOURCES;
                break;
            }
        }
    }
    if (connection->state == TCP_CONNECTION_ESTABLISHED) {
        status = NETWORK_STATUS_OK;
    } else if (status == NETWORK_STATUS_OK) {
        status = NETWORK_STATUS_TIMEOUT;
    }
    timer_release();
    return status;
}

/*
 * A passive open is the one place where the stack, rather than a caller,
 * decides that a connection exists. Everything about it is therefore bounded
 * in advance: one port, a declared backlog no larger than
 * NETWORK_TCP_MAX_BACKLOG, and children drawn from the same fixed connection
 * table an active open draws from. A handshake completes on any pump, but the
 * retransmission and reaping of half-open children happen only inside
 * network_tcp_accept -- that is deliberate, and it is what keeps half-open
 * state from outliving the caller that asked for it.
 */
enum network_status network_tcp_listen(
    uint64_t owner,
    network_handle handle,
    uint16_t port,
    size_t backlog
)
{
    enum network_status status;
    struct tcp_connection *connection = tcp_for(owner, handle, &status);

    if (connection == NULL) {
        return status;
    }
    if (port == 0U || backlog == 0U || backlog > NETWORK_TCP_MAX_BACKLOG) {
        return NETWORK_STATUS_INVALID_ARGUMENT;
    }
    if (connection->state != TCP_CONNECTION_OPEN) {
        return NETWORK_STATUS_WRONG_MODE;
    }
    if (tcp_port_used(port)) {
        return NETWORK_STATUS_PORT_IN_USE;
    }
    connection->local_port = port;
    connection->backlog = (uint8_t)backlog;
    connection->state = TCP_CONNECTION_LISTEN;
    ++runtime.public.tcp_listeners;
    return NETWORK_STATUS_OK;
}

static struct tcp_connection *tcp_acceptable(
    const struct tcp_connection *listener
)
{
    const size_t parent = tcp_index_of(listener);

    for (size_t index = 0U; index < NETWORK_MAX_TCP_CONNECTIONS; ++index) {
        struct tcp_connection *connection = &runtime.tcp[index];

        if (connection->active && connection->pending &&
            connection->listener == parent &&
            connection->state == TCP_CONNECTION_ESTABLISHED) {
            return connection;
        }
    }
    return NULL;
}

/*
 * Accepting is also when a listener's half-open children are driven. Their
 * acknowledgement is retransmitted here when its deadline passes, and a child
 * that exhausts the retransmission limit is reclaimed rather than left to hold
 * a slot: a peer that opens and vanishes must cost this side nothing durable.
 */
static void tcp_service_pending(const struct tcp_connection *listener)
{
    const size_t parent = tcp_index_of(listener);
    const uint64_t now = clock_monotonic_ns();

    for (size_t index = 0U; index < NETWORK_MAX_TCP_CONNECTIONS; ++index) {
        struct tcp_connection *connection = &runtime.tcp[index];

        if (!connection->active || !connection->pending ||
            connection->listener != parent) {
            continue;
        }
        if (connection->state == TCP_CONNECTION_RESET ||
            connection->error != NETWORK_STATUS_OK) {
            tcp_release(connection);
            continue;
        }
        if (connection->state != TCP_CONNECTION_SYN_RECEIVED ||
            now < connection->retransmit_deadline_ns) {
            continue;
        }
        if (tcp_retransmit(connection) != NETWORK_STATUS_OK &&
            connection->error != NETWORK_STATUS_OK) {
            tcp_release(connection);
        }
    }
}

enum network_status network_tcp_accept(
    uint64_t owner,
    network_handle handle,
    network_handle *accepted,
    uint32_t *source,
    uint16_t *port,
    uint64_t timeout_ns
)
{
    enum network_status status;
    struct tcp_connection *listener = tcp_for(owner, handle, &status);
    uint64_t deadline;

    if (listener == NULL) {
        return status;
    }
    if (accepted == NULL || source == NULL || port == NULL ||
        !deadline_valid(timeout_ns)) {
        return NETWORK_STATUS_NULL_ARGUMENT;
    }
    *accepted = 0U;
    *source = 0U;
    *port = 0U;
    if (listener->state != TCP_CONNECTION_LISTEN) {
        return listener->error != NETWORK_STATUS_OK ? listener->error :
            NETWORK_STATUS_WRONG_MODE;
    }
    if (!timer_acquire()) {
        return NETWORK_STATUS_NO_RESOURCES;
    }
    deadline = clock_monotonic_ns() + timeout_ns;
    do {
        struct tcp_connection *child;

        (void)network_service();
        if (listener->cancelled) {
            timer_release();
            return NETWORK_STATUS_CANCELLED;
        }
        if (listener->error != NETWORK_STATUS_OK) {
            status = listener->error;
            timer_release();
            return status;
        }
        tcp_service_pending(listener);
        child = tcp_acceptable(listener);
        if (child != NULL) {
            child->pending = false;
            *accepted = make_handle(HANDLE_KIND_TCP, tcp_index_of(child),
                child->generation);
            *source = child->remote_address;
            *port = child->remote_port;
            timer_release();
            return NETWORK_STATUS_OK;
        }
        if (clock_monotonic_ns() < deadline) {
            if (!network_wait_for_interrupt(deadline)) {
                timer_release();
                return NETWORK_STATUS_NO_RESOURCES;
            }
        }
    } while (clock_monotonic_ns() < deadline);
    timer_release();
    return NETWORK_STATUS_TIMEOUT;
}

enum network_status network_tcp_write(
    uint64_t owner,
    network_handle handle,
    const uint8_t *bytes,
    size_t length,
    size_t *written,
    uint64_t timeout_ns
)
{
    enum network_status status;
    struct tcp_connection *connection = tcp_for(owner, handle, &status);
    size_t offset = 0U;
    uint64_t deadline;

    if (connection == NULL) {
        return status;
    }
    if (bytes == NULL || written == NULL || length == 0U ||
        !deadline_valid(timeout_ns)) {
        return NETWORK_STATUS_NULL_ARGUMENT;
    }
    *written = 0U;
    if (connection->state != TCP_CONNECTION_ESTABLISHED) {
        return NETWORK_STATUS_WRONG_MODE;
    }
    if (!timer_acquire()) {
        return NETWORK_STATUS_NO_RESOURCES;
    }
    deadline = clock_monotonic_ns() + timeout_ns;
    while (offset < length && clock_monotonic_ns() < deadline) {
        size_t chunk = length - offset;

        if (chunk > connection->peer_mss) {
            chunk = connection->peer_mss;
        }
        connection->send_unacknowledged = connection->send_next;
        status = tcp_emit(connection, TCP_FLAG_ACK | TCP_FLAG_PSH,
            bytes + offset, chunk, connection->send_next, true, true);
        if (status != NETWORK_STATUS_OK) {
            break;
        }
        status = tcp_wait_ack(connection, deadline);
        if (status != NETWORK_STATUS_OK) {
            break;
        }
        offset += chunk;
        *written = offset;
    }
    timer_release();
    return offset == length ? NETWORK_STATUS_OK :
        (status == NETWORK_STATUS_OK ? NETWORK_STATUS_TIMEOUT : status);
}

enum network_status network_tcp_read(
    uint64_t owner,
    network_handle handle,
    uint8_t *bytes,
    size_t capacity,
    size_t *read_bytes,
    uint64_t timeout_ns
)
{
    enum network_status status;
    struct tcp_connection *connection = tcp_for(owner, handle, &status);
    uint64_t deadline;
    size_t count;
    size_t buffered_before;

    if (connection == NULL) {
        return status;
    }
    if (bytes == NULL || read_bytes == NULL || capacity == 0U ||
        !deadline_valid(timeout_ns)) {
        return NETWORK_STATUS_NULL_ARGUMENT;
    }
    *read_bytes = 0U;
    if (connection->state == TCP_CONNECTION_LISTEN ||
        connection->state == TCP_CONNECTION_OPEN ||
        connection->state == TCP_CONNECTION_SYN_RECEIVED) {
        return NETWORK_STATUS_WRONG_MODE;
    }
    if (!timer_acquire()) {
        return NETWORK_STATUS_NO_RESOURCES;
    }
    deadline = clock_monotonic_ns() + timeout_ns;
    while (connection->receive_bytes == 0U && !connection->peer_closed &&
        connection->error == NETWORK_STATUS_OK &&
        clock_monotonic_ns() < deadline) {
        (void)network_service();
        if (connection->cancelled) {
            timer_release();
            return NETWORK_STATUS_CANCELLED;
        }
        if (connection->receive_bytes == 0U && !connection->peer_closed &&
            connection->error == NETWORK_STATUS_OK &&
            clock_monotonic_ns() < deadline) {
            if (!network_wait_for_interrupt(deadline)) {
                timer_release();
                return NETWORK_STATUS_NO_RESOURCES;
            }
        }
    }
    if (connection->error != NETWORK_STATUS_OK) {
        timer_release();
        return connection->error;
    }
    if (connection->receive_bytes == 0U) {
        timer_release();
        return connection->peer_closed ? NETWORK_STATUS_CONNECTION_CLOSED :
            NETWORK_STATUS_TIMEOUT;
    }
    count = connection->receive_bytes < capacity ?
        connection->receive_bytes : capacity;
    buffered_before = connection->receive_bytes;
    copy_bytes(bytes, connection->receive, count);
    for (size_t index = count; index < connection->receive_bytes; ++index) {
        connection->receive[index - count] = connection->receive[index];
    }
    connection->receive_bytes -= count;
    if (!connection->peer_closed &&
        buffered_before >= NETWORK_TCP_RX_BYTES / 2U &&
        connection->receive_bytes < NETWORK_TCP_RX_BYTES / 2U) {
        const enum network_status ack_status = tcp_ack(connection);

        if (ack_status != NETWORK_STATUS_OK &&
            connection->error == NETWORK_STATUS_OK) {
            connection->error = ack_status;
        }
    }
    *read_bytes = count;
    timer_release();
    return NETWORK_STATUS_OK;
}

enum network_status network_tcp_shutdown(
    uint64_t owner,
    network_handle handle,
    uint64_t timeout_ns
)
{
    enum network_status status;
    struct tcp_connection *connection = tcp_for(owner, handle, &status);
    uint64_t deadline;

    if (connection == NULL) {
        return status;
    }
    if (!deadline_valid(timeout_ns)) {
        return NETWORK_STATUS_INVALID_ARGUMENT;
    }
    if (connection->state == TCP_CONNECTION_CLOSED ||
        connection->state == TCP_CONNECTION_RESET) {
        return connection->state == TCP_CONNECTION_RESET ?
            NETWORK_STATUS_CONNECTION_RESET : NETWORK_STATUS_OK;
    }
    if (connection->state != TCP_CONNECTION_ESTABLISHED &&
        connection->state != TCP_CONNECTION_CLOSE_WAIT) {
        return NETWORK_STATUS_WRONG_MODE;
    }
    if (!timer_acquire()) {
        return NETWORK_STATUS_NO_RESOURCES;
    }
    connection->send_unacknowledged = connection->send_next;
    connection->fin_sent = true;
    connection->state = TCP_CONNECTION_FIN_WAIT;
    status = tcp_emit(connection, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0U,
        connection->send_next, true, true);
    deadline = clock_monotonic_ns() + timeout_ns;
    while (status == NETWORK_STATUS_OK && clock_monotonic_ns() < deadline &&
        connection->state != TCP_CONNECTION_CLOSED) {
        (void)network_service();
        if (connection->cancelled) {
            status = NETWORK_STATUS_CANCELLED;
            break;
        }
        if (connection->error != NETWORK_STATUS_OK) {
            status = connection->error;
            break;
        }
        if (connection->send_unacknowledged == connection->send_next &&
            connection->peer_closed) {
            connection->state = TCP_CONNECTION_CLOSED;
            break;
        }
        if (clock_monotonic_ns() >= connection->retransmit_deadline_ns &&
            tcp_retransmit(connection) != NETWORK_STATUS_OK) {
            status = connection->error;
            break;
        }
        if (connection->state != TCP_CONNECTION_CLOSED &&
            clock_monotonic_ns() < deadline) {
            if (!network_wait_for_interrupt(deadline)) {
                status = NETWORK_STATUS_NO_RESOURCES;
                break;
            }
        }
    }
    if (connection->state == TCP_CONNECTION_CLOSED) {
        status = NETWORK_STATUS_OK;
    } else if (status == NETWORK_STATUS_OK) {
        status = NETWORK_STATUS_TIMEOUT;
    }
    timer_release();
    return status;
}

enum network_status network_address(
    uint64_t owner,
    network_handle handle,
    bool peer,
    uint32_t *address,
    uint16_t *port
)
{
    enum network_status status;
    struct udp_socket *udp;
    struct tcp_connection *tcp;

    if (address == NULL || port == NULL) {
        return NETWORK_STATUS_NULL_ARGUMENT;
    }
    *address = 0U;
    *port = 0U;
    udp = udp_for(owner, handle, &status);
    if (udp != NULL) {
        if (peer) {
            return NETWORK_STATUS_WRONG_MODE;
        }
        *address = runtime.public.configuration.address;
        *port = udp->port;
        return NETWORK_STATUS_OK;
    }
    tcp = tcp_for(owner, handle, &status);
    if (tcp == NULL) {
        return status;
    }
    *address = peer ? tcp->remote_address :
        runtime.public.configuration.address;
    *port = peer ? tcp->remote_port : tcp->local_port;
    return NETWORK_STATUS_OK;
}

enum network_status network_ping(
    uint32_t destination,
    uint32_t count,
    uint64_t timeout_ns,
    struct network_ping_result *result
)
{
    if (result == NULL) {
        return NETWORK_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(result, sizeof(*result));
    result->destination = destination;
    if (!runtime.public.configuration.configured ||
        !ipv4_is_unicast(destination) || count == 0U ||
        count > NETWORK_PING_MAX_COUNT || !deadline_valid(timeout_ns)) {
        return NETWORK_STATUS_INVALID_ARGUMENT;
    }
    if (!timer_acquire()) {
        return NETWORK_STATUS_NO_RESOURCES;
    }
    runtime.ping.identifier = random_u16();
    for (uint32_t index = 0U; index < count; ++index) {
        uint8_t packet[ICMP_HEADER_BYTES + 16U];
        uint64_t deadline;

        zero_bytes(packet, sizeof(packet));
        packet[0] = ICMP_ECHO_REQUEST;
        write_be16(packet + 4U, runtime.ping.identifier);
        write_be16(packet + 6U, (uint16_t)(index + 1U));
        write_be32(packet + 8U, (uint32_t)clock_monotonic_ns());
        write_be16(packet + 2U, internet_checksum(packet, sizeof(packet)));
        runtime.ping.address = destination;
        runtime.ping.sequence = (uint16_t)(index + 1U);
        runtime.ping.sent_ns = clock_monotonic_ns();
        runtime.ping.status = NETWORK_STATUS_TIMEOUT;
        runtime.ping.waiting = true;
        runtime.ping.received = false;
        ++result->sent;
        enum network_status status = ipv4_send(
            runtime.public.configuration.address, destination,
            IPV4_PROTOCOL_ICMP, packet, sizeof(packet), timeout_ns);

        if (status != NETWORK_STATUS_OK) {
            result->result[index] = status;
            continue;
        }
        deadline = clock_monotonic_ns() + timeout_ns;
        while (!runtime.ping.received && clock_monotonic_ns() < deadline) {
            (void)network_service();
            if (!runtime.ping.received &&
                clock_monotonic_ns() < deadline) {
                if (!network_wait_for_interrupt(deadline)) {
                    runtime.ping.waiting = false;
                    timer_release();
                    return NETWORK_STATUS_NO_RESOURCES;
                }
            }
        }
        result->result[index] = runtime.ping.received ?
            runtime.ping.status : NETWORK_STATUS_TIMEOUT;
        if (runtime.ping.received &&
            runtime.ping.status == NETWORK_STATUS_OK) {
            result->round_trip_ns[index] = runtime.ping.round_trip_ns;
            ++result->received;
        }
        runtime.ping.waiting = false;
    }
    timer_release();
    return result->received == count ? NETWORK_STATUS_OK :
        NETWORK_STATUS_TIMEOUT;
}

enum network_status network_cancel(uint64_t owner, network_handle handle)
{
    enum network_status status;

    if ((uint8_t)(handle >> 56U) == HANDLE_KIND_UDP) {
        struct udp_socket *socket = udp_for(owner, handle, &status);

        if (socket == NULL) { return status; }
        socket->cancelled = true;
    } else if ((uint8_t)(handle >> 56U) == HANDLE_KIND_TCP) {
        struct tcp_connection *connection = tcp_for(owner, handle, &status);

        if (connection == NULL) { return status; }
        connection->cancelled = true;
    } else {
        return NETWORK_STATUS_STALE_HANDLE;
    }
    ++runtime.public.statistics.cancellations;
    return NETWORK_STATUS_OK;
}

enum network_status network_close(uint64_t owner, network_handle handle)
{
    enum network_status status;

    if ((uint8_t)(handle >> 56U) == HANDLE_KIND_UDP) {
        struct udp_socket *socket = udp_for(owner, handle, &status);

        if (socket == NULL) { return status; }
        zero_bytes(socket, sizeof(*socket));
        --runtime.public.udp_sockets;
        return NETWORK_STATUS_OK;
    }
    if ((uint8_t)(handle >> 56U) == HANDLE_KIND_TCP) {
        struct tcp_connection *connection = tcp_for(owner, handle, &status);

        if (connection == NULL) { return status; }
        if (connection->backlog != 0U) {
            /*
             * A listener owns every child it produced that nobody has accepted
             * yet. Closing it refuses those peers rather than orphaning their
             * slots; a child already handed out is an independent connection
             * with its own handle and is left alone. The test is the backlog
             * and not the state, because a device reset moves every connection
             * to RESET and these children are reachable through nothing else.
             */
            tcp_release_children(connection);
            if (runtime.public.tcp_listeners != 0U) {
                --runtime.public.tcp_listeners;
            }
        }
        tcp_release(connection);
        return NETWORK_STATUS_OK;
    }
    return NETWORK_STATUS_STALE_HANDLE;
}

enum network_status network_poll(
    uint64_t owner,
    const struct network_poll_request *requests,
    size_t request_count,
    struct network_poll_result *results,
    size_t result_capacity,
    size_t *result_count,
    uint64_t timeout_ns
)
{
    uint64_t deadline;

    if (requests == NULL || results == NULL || result_count == NULL) {
        return NETWORK_STATUS_NULL_ARGUMENT;
    }
    if (request_count == 0U || request_count > NETWORK_MAX_POLL_HANDLES ||
        result_capacity < request_count || !deadline_valid(timeout_ns)) {
        return NETWORK_STATUS_INVALID_ARGUMENT;
    }
    if (!timer_acquire()) {
        return NETWORK_STATUS_NO_RESOURCES;
    }
    deadline = clock_monotonic_ns() + timeout_ns;
    *result_count = 0U;
    do {
        (void)network_service();
        *result_count = 0U;
        for (size_t index = 0U; index < request_count; ++index) {
            struct network_poll_result *result = &results[index];
            enum network_status status;

            result->handle = requests[index].handle;
            result->ready = NETWORK_READY_NONE;
            result->error = NETWORK_STATUS_OK;
            if ((uint8_t)(requests[index].handle >> 56U) == HANDLE_KIND_UDP) {
                struct udp_socket *socket = udp_for(owner,
                    requests[index].handle, &status);

                if (socket == NULL) {
                    result->ready = NETWORK_READY_ERROR;
                    result->error = status;
                } else {
                    if (socket->queue_count != 0U) {
                        result->ready |= NETWORK_READY_READABLE;
                    }
                    result->ready |= NETWORK_READY_WRITABLE;
                    if (socket->cancelled) {
                        result->ready |= NETWORK_READY_CANCELLED;
                    }
                }
            } else if ((uint8_t)(requests[index].handle >> 56U) ==
                    HANDLE_KIND_TCP) {
                struct tcp_connection *connection = tcp_for(owner,
                    requests[index].handle, &status);

                if (connection == NULL) {
                    result->ready = NETWORK_READY_ERROR;
                    result->error = status;
                } else if (connection->state == TCP_CONNECTION_LISTEN) {
                    if (tcp_acceptable(connection) != NULL) {
                        result->ready |= NETWORK_READY_ACCEPTABLE |
                            NETWORK_READY_READABLE;
                    }
                    if (connection->cancelled) {
                        result->ready |= NETWORK_READY_CANCELLED;
                    }
                } else {
                    if (connection->state == TCP_CONNECTION_ESTABLISHED) {
                        result->ready |= NETWORK_READY_CONNECTED |
                            NETWORK_READY_WRITABLE;
                    }
                    if (connection->receive_bytes != 0U) {
                        result->ready |= NETWORK_READY_READABLE;
                    }
                    if (connection->peer_closed) {
                        result->ready |= NETWORK_READY_PEER_CLOSED;
                    }
                    if (connection->cancelled) {
                        result->ready |= NETWORK_READY_CANCELLED;
                    }
                    if (connection->error != NETWORK_STATUS_OK) {
                        result->ready |= NETWORK_READY_ERROR;
                        result->error = connection->error;
                    }
                }
            } else {
                result->ready = NETWORK_READY_ERROR;
                result->error = NETWORK_STATUS_STALE_HANDLE;
            }
            result->ready &= requests[index].interests |
                NETWORK_READY_ERROR | NETWORK_READY_CANCELLED |
                NETWORK_READY_PEER_CLOSED | NETWORK_READY_ACCEPTABLE;
            if (result->ready != NETWORK_READY_NONE) {
                ++*result_count;
            }
        }
        if (*result_count != 0U) {
            timer_release();
            return NETWORK_STATUS_OK;
        }
        if (clock_monotonic_ns() < deadline) {
            if (!network_wait_for_interrupt(deadline)) {
                timer_release();
                return NETWORK_STATUS_NO_RESOURCES;
            }
        }
    } while (clock_monotonic_ns() < deadline);
    for (size_t index = 0U; index < request_count; ++index) {
        results[index].ready |= NETWORK_READY_TIMEOUT;
    }
    *result_count = request_count;
    timer_release();
    return NETWORK_STATUS_TIMEOUT;
}

void network_process_terminated(uint64_t owner)
{
    if (owner == 0U) {
        return;
    }
    for (size_t index = 0U; index < NETWORK_MAX_UDP_SOCKETS; ++index) {
        if (runtime.udp[index].active && runtime.udp[index].owner == owner) {
            zero_bytes(&runtime.udp[index], sizeof(runtime.udp[index]));
            --runtime.public.udp_sockets;
        }
    }
    for (size_t index = 0U; index < NETWORK_MAX_TCP_CONNECTIONS; ++index) {
        if (runtime.tcp[index].active && runtime.tcp[index].owner == owner) {
            if (runtime.tcp[index].backlog != 0U &&
                runtime.public.tcp_listeners != 0U) {
                --runtime.public.tcp_listeners;
            }
            tcp_release(&runtime.tcp[index]);
        }
    }
}

struct parsed_http_url {
    char host[NETWORK_MAX_HOSTNAME + 1U];
    char target[512];
    uint32_t address;
    uint16_t port;
    bool numeric;
};

struct http_stream {
    uint8_t bytes[512];
    uint64_t owner;
    network_handle handle;
    size_t offset;
    size_t length;
    uint64_t deadline_ns;
};

struct http_response {
    uint64_t content_length;
    char location[768];
    uint16_t status;
    bool content_length_present;
    bool chunked;
};

struct http_sink {
    uint8_t *memory;
    size_t capacity;
    phipfs_handle file;
    uint32_t total;
    bool file_backed;
};

static bool prefix_equal(const char *text, const char *prefix)
{
    size_t index = 0U;

    while (prefix[index] != '\0') {
        char left = text[index];
        char right = prefix[index];

        if (left >= 'A' && left <= 'Z') {
            left = (char)(left - 'A' + 'a');
        }
        if (right >= 'A' && right <= 'Z') {
            right = (char)(right - 'A' + 'a');
        }
        if (left != right) {
            return false;
        }
        ++index;
    }
    return true;
}

static bool decimal_u16(const char *text, size_t length, uint16_t *value)
{
    uint32_t result = 0U;

    if (length == 0U || value == NULL) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        uint32_t digit;

        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
        digit = (uint32_t)(text[index] - '0');
        if (result > (UINT16_MAX - digit) / 10U) {
            return false;
        }
        result = result * 10U + digit;
    }
    if (result == 0U) {
        return false;
    }
    *value = (uint16_t)result;
    return true;
}

static bool parse_http_url(const char *url, struct parsed_http_url *parsed)
{
    const size_t length = string_length_bounded(url, 767U);
    size_t authority_start = 7U;
    size_t authority_end;
    size_t colon = SIZE_MAX;
    size_t host_end;
    size_t host_length;
    size_t target_length;

    if (parsed == NULL || length <= authority_start || length > 767U ||
        !prefix_equal(url, "http://") || prefix_equal(url, "https://")) {
        return false;
    }
    zero_bytes(parsed, sizeof(*parsed));
    authority_end = authority_start;
    while (authority_end < length && url[authority_end] != '/') {
        const char character = url[authority_end];

        if (character == '@' || character == '[' || character == ']' ||
            character <= ' ' || character == 127) {
            return false;
        }
        if (character == ':') {
            if (colon != SIZE_MAX) {
                return false;
            }
            colon = authority_end;
        }
        ++authority_end;
    }
    host_end = colon == SIZE_MAX ? authority_end : colon;
    host_length = host_end - authority_start;
    if (host_length == 0U || host_length > NETWORK_MAX_HOSTNAME) {
        return false;
    }
    string_copy(parsed->host, url + authority_start, host_length);
    if (colon != SIZE_MAX) {
        if (!decimal_u16(url + colon + 1U, authority_end - colon - 1U,
                &parsed->port)) {
            return false;
        }
    } else {
        parsed->port = 80U;
    }
    parsed->numeric = network_parse_ipv4(parsed->host, &parsed->address);
    if (!parsed->numeric && !hostname_valid(parsed->host)) {
        return false;
    }
    target_length = authority_end == length ? 1U : length - authority_end;
    if (target_length >= sizeof(parsed->target)) {
        return false;
    }
    if (authority_end == length) {
        parsed->target[0] = '/';
        parsed->target[1] = '\0';
    } else {
        for (size_t index = 0U; index < target_length; ++index) {
            const char character = url[authority_end + index];

            if (character <= ' ' || character == 127) {
                return false;
            }
            parsed->target[index] = character;
        }
        parsed->target[target_length] = '\0';
    }
    return true;
}

static enum network_status http_stream_byte(
    struct http_stream *stream,
    uint8_t *byte
)
{
    if (stream == NULL || byte == NULL) {
        return NETWORK_STATUS_NULL_ARGUMENT;
    }
    if (stream->offset == stream->length) {
        size_t received = 0U;
        uint64_t now = clock_monotonic_ns();
        enum network_status status;

        if (now >= stream->deadline_ns) {
            return NETWORK_STATUS_TIMEOUT;
        }
        status = network_tcp_read(stream->owner, stream->handle,
            stream->bytes, sizeof(stream->bytes), &received,
            stream->deadline_ns - now);
        if (status != NETWORK_STATUS_OK) {
            return status;
        }
        stream->offset = 0U;
        stream->length = received;
    }
    *byte = stream->bytes[stream->offset++];
    return NETWORK_STATUS_OK;
}

static enum network_status http_line(
    struct http_stream *stream,
    char *line,
    size_t capacity,
    size_t *length,
    size_t *header_bytes
)
{
    size_t used = 0U;
    bool carriage = false;

    if (line == NULL || length == NULL || header_bytes == NULL ||
        capacity < 2U) {
        return NETWORK_STATUS_NULL_ARGUMENT;
    }
    for (;;) {
        uint8_t byte;
        enum network_status status = http_stream_byte(stream, &byte);

        if (status != NETWORK_STATUS_OK) {
            return status;
        }
        if (++*header_bytes > NETWORK_HTTP_HEADER_BYTES) {
            return NETWORK_STATUS_TOO_LARGE;
        }
        if (carriage) {
            if (byte != '\n') {
                return NETWORK_STATUS_MALFORMED;
            }
            line[used] = '\0';
            *length = used;
            return NETWORK_STATUS_OK;
        }
        if (byte == '\r') {
            carriage = true;
            continue;
        }
        if (byte == '\n' || byte == 0U || byte < 32U || byte == 127U ||
            used + 1U >= capacity) {
            return NETWORK_STATUS_MALFORMED;
        }
        line[used++] = (char)byte;
    }
}

static bool token_equal(
    const char *value,
    size_t length,
    const char *expected
)
{
    size_t expected_length = string_length_bounded(expected, length + 1U);

    if (expected_length != length) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        char left = value[index];
        char right = expected[index];

        if (left >= 'A' && left <= 'Z') {
            left = (char)(left - 'A' + 'a');
        }
        if (right >= 'A' && right <= 'Z') {
            right = (char)(right - 'A' + 'a');
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

static bool parse_u64_decimal(const char *text, size_t length, uint64_t *value)
{
    uint64_t result = 0U;

    if (length == 0U || value == NULL) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        uint64_t digit;

        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
        digit = (uint64_t)(text[index] - '0');
        if (result > (UINT64_MAX - digit) / 10U) {
            return false;
        }
        result = result * 10U + digit;
    }
    *value = result;
    return true;
}

static enum network_status http_parse_headers(
    struct http_stream *stream,
    struct http_response *response
)
{
    char line[513];
    size_t length;
    size_t header_bytes = 0U;
    size_t header_count = 0U;

    zero_bytes(response, sizeof(*response));
    enum network_status status = http_line(stream, line, sizeof(line),
        &length, &header_bytes);

    if (status != NETWORK_STATUS_OK || length < 12U ||
        !prefix_equal(line, "HTTP/1.1 ") || line[9] < '1' ||
        line[9] > '5' || line[10] < '0' || line[10] > '9' ||
        line[11] < '0' || line[11] > '9' ||
        (length > 12U && line[12] != ' ')) {
        return NETWORK_STATUS_MALFORMED;
    }
    response->status = (uint16_t)((line[9] - '0') * 100 +
        (line[10] - '0') * 10 + (line[11] - '0'));
    for (;;) {
        size_t colon = SIZE_MAX;
        size_t value_start;
        size_t value_end;

        status = http_line(stream, line, sizeof(line), &length,
            &header_bytes);
        if (status != NETWORK_STATUS_OK) {
            return status;
        }
        if (length == 0U) {
            break;
        }
        if (++header_count > NETWORK_HTTP_MAX_HEADERS) {
            return NETWORK_STATUS_TOO_LARGE;
        }
        for (size_t index = 0U; index < length; ++index) {
            const char character = line[index];

            if (character == ':' && colon == SIZE_MAX) {
                colon = index;
                continue;
            }
            if (colon == SIZE_MAX && !((character >= 'a' &&
                    character <= 'z') || (character >= 'A' &&
                    character <= 'Z') || (character >= '0' &&
                    character <= '9') || character == '-')) {
                return NETWORK_STATUS_MALFORMED;
            }
        }
        if (colon == SIZE_MAX || colon == 0U) {
            return NETWORK_STATUS_MALFORMED;
        }
        value_start = colon + 1U;
        while (value_start < length &&
            (line[value_start] == ' ' || line[value_start] == '\t')) {
            ++value_start;
        }
        value_end = length;
        while (value_end > value_start &&
            (line[value_end - 1U] == ' ' || line[value_end - 1U] == '\t')) {
            --value_end;
        }
        if (token_equal(line, colon, "content-length")) {
            uint64_t parsed;

            if (!parse_u64_decimal(line + value_start,
                    value_end - value_start, &parsed) ||
                parsed > NETWORK_HTTP_MAX_DOWNLOAD_BYTES ||
                (response->content_length_present &&
                    response->content_length != parsed)) {
                return NETWORK_STATUS_MALFORMED;
            }
            response->content_length = parsed;
            response->content_length_present = true;
        } else if (token_equal(line, colon, "transfer-encoding")) {
            if (!token_equal(line + value_start, value_end - value_start,
                    "chunked")) {
                return NETWORK_STATUS_UNSUPPORTED;
            }
            response->chunked = true;
        } else if (token_equal(line, colon, "location")) {
            if (value_end == value_start ||
                value_end - value_start >= sizeof(response->location)) {
                return NETWORK_STATUS_MALFORMED;
            }
            string_copy(response->location, line + value_start,
                value_end - value_start);
        }
    }
    if (response->chunked && response->content_length_present) {
        return NETWORK_STATUS_MALFORMED;
    }
    return NETWORK_STATUS_OK;
}

static bool destination_valid(const char *path)
{
    const size_t length = string_length_bounded(path, PHIPFS_MAX_PATH);
    size_t component = 0U;

    if (length == 0U || length > PHIPFS_MAX_PATH || path[0] == '/') {
        return false;
    }
    for (size_t index = 0U; index <= length; ++index) {
        if (path[index] == '/' || path[index] == '\0') {
            if (component == 0U ||
                (component == 1U && path[index - 1U] == '.') ||
                (component == 2U && path[index - 2U] == '.' &&
                    path[index - 1U] == '.')) {
                return false;
            }
            component = 0U;
        } else {
            ++component;
        }
    }
    return true;
}

static bool download_sibling_paths(
    const char *destination,
    char temporary[PHIPFS_MAX_PATH + 1U],
    char backup[PHIPFS_MAX_PATH + 1U]
)
{
    const size_t length = string_length_bounded(destination, PHIPFS_MAX_PATH);
    size_t slash = SIZE_MAX;
    size_t prefix;
    static const char temp_name[] = "PHIPDL.TMP";
    static const char backup_name[] = "PHIPDL.BAK";

    for (size_t index = 0U; index < length; ++index) {
        if (destination[index] == '/') {
            slash = index;
        }
    }
    prefix = slash == SIZE_MAX ? 0U : slash + 1U;
    if (prefix + sizeof(temp_name) - 1U > PHIPFS_MAX_PATH ||
        prefix + sizeof(backup_name) - 1U > PHIPFS_MAX_PATH) {
        return false;
    }
    for (size_t index = 0U; index < prefix; ++index) {
        temporary[index] = destination[index];
        backup[index] = destination[index];
    }
    string_copy(temporary + prefix, temp_name, sizeof(temp_name) - 1U);
    string_copy(backup + prefix, backup_name, sizeof(backup_name) - 1U);
    return !string_equal(destination, temporary) &&
        !string_equal(destination, backup);
}

static enum network_status filesystem_status(enum phipfs_status status)
{
    return status == PHIPFS_STATUS_FULL ? NETWORK_STATUS_TOO_LARGE :
        NETWORK_STATUS_FILESYSTEM;
}

static enum network_status http_write_bytes(
    struct http_sink *sink,
    const uint8_t *bytes,
    size_t length
)
{
    size_t written = 0U;
    enum phipfs_status status;

    if (sink == NULL || bytes == NULL ||
        length > NETWORK_HTTP_MAX_DOWNLOAD_BYTES - sink->total) {
        return NETWORK_STATUS_TOO_LARGE;
    }
    if (sink->file_backed) {
        status = phipfs_write(sink->file, bytes, length, &written);
        if (status != PHIPFS_STATUS_OK || written != length) {
            return filesystem_status(status);
        }
    } else {
        if (length > sink->capacity - sink->total) {
            return NETWORK_STATUS_TOO_LARGE;
        }
        copy_bytes(sink->memory + sink->total, bytes, length);
        written = length;
    }
    sink->total += (uint32_t)written;
    return NETWORK_STATUS_OK;
}

static enum network_status http_fixed_body(
    struct http_stream *stream,
    struct http_sink *sink,
    uint64_t length
)
{
    uint8_t buffer[512];
    uint64_t remaining = length;

    while (remaining != 0U) {
        size_t chunk = remaining < sizeof(buffer) ?
            (size_t)remaining : sizeof(buffer);

        for (size_t index = 0U; index < chunk; ++index) {
            enum network_status status = http_stream_byte(stream,
                &buffer[index]);

            if (status != NETWORK_STATUS_OK) {
                return status;
            }
        }
        enum network_status status = http_write_bytes(sink, buffer, chunk);
        if (status != NETWORK_STATUS_OK) {
            return status;
        }
        remaining -= chunk;
    }
    return NETWORK_STATUS_OK;
}

static bool parse_chunk_size(const char *line, size_t length, uint64_t *value)
{
    uint64_t result = 0U;
    size_t digits = 0U;

    for (size_t index = 0U; index < length && line[index] != ';'; ++index) {
        uint8_t digit;

        if (line[index] >= '0' && line[index] <= '9') {
            digit = (uint8_t)(line[index] - '0');
        } else if (line[index] >= 'a' && line[index] <= 'f') {
            digit = (uint8_t)(line[index] - 'a' + 10);
        } else if (line[index] >= 'A' && line[index] <= 'F') {
            digit = (uint8_t)(line[index] - 'A' + 10);
        } else {
            return false;
        }
        if (result > (UINT64_MAX - digit) / 16U) {
            return false;
        }
        result = result * 16U + digit;
        ++digits;
    }
    if (digits == 0U || result > NETWORK_HTTP_MAX_DOWNLOAD_BYTES) {
        return false;
    }
    *value = result;
    return true;
}

static enum network_status http_chunked_body(
    struct http_stream *stream,
    struct http_sink *sink
)
{
    char line[128];
    size_t length;
    size_t header_bytes = 0U;

    for (;;) {
        uint64_t chunk;
        enum network_status status = http_line(stream, line, sizeof(line),
            &length, &header_bytes);

        if (status != NETWORK_STATUS_OK ||
            !parse_chunk_size(line, length, &chunk)) {
            return NETWORK_STATUS_MALFORMED;
        }
        if (chunk == 0U) {
            do {
                status = http_line(stream, line, sizeof(line), &length,
                    &header_bytes);
                if (status != NETWORK_STATUS_OK) {
                    return status;
                }
            } while (length != 0U);
            return NETWORK_STATUS_OK;
        }
        status = http_fixed_body(stream, sink, chunk);
        if (status != NETWORK_STATUS_OK) {
            return status;
        }
        uint8_t carriage;
        uint8_t newline;
        if (http_stream_byte(stream, &carriage) != NETWORK_STATUS_OK ||
            http_stream_byte(stream, &newline) != NETWORK_STATUS_OK ||
            carriage != '\r' || newline != '\n') {
            return NETWORK_STATUS_MALFORMED;
        }
    }
}

static enum network_status http_connection_body(
    struct http_stream *stream,
    struct http_sink *sink
)
{
    uint8_t buffer[512];

    for (;;) {
        size_t received = 0U;
        enum network_status status;

        while (stream->offset < stream->length) {
            size_t available = stream->length - stream->offset;
            status = http_write_bytes(sink,
                stream->bytes + stream->offset, available);
            if (status != NETWORK_STATUS_OK) {
                return status;
            }
            stream->offset = stream->length;
        }
        uint64_t now = clock_monotonic_ns();
        if (now >= stream->deadline_ns) {
            return NETWORK_STATUS_TIMEOUT;
        }
        status = network_tcp_read(stream->owner, stream->handle, buffer,
            sizeof(buffer), &received, stream->deadline_ns - now);
        if (status == NETWORK_STATUS_CONNECTION_CLOSED) {
            return NETWORK_STATUS_OK;
        }
        if (status != NETWORK_STATUS_OK) {
            return status;
        }
        status = http_write_bytes(sink, buffer, received);
        if (status != NETWORK_STATUS_OK) {
            return status;
        }
    }
}

static enum network_status http_open_request(
    uint64_t owner,
    const struct parsed_http_url *url,
    bool head_only,
    uint64_t deadline,
    struct http_stream *stream,
    struct http_response *response
)
{
    char request[1024];
    size_t used = 0U;
    size_t written = 0U;
    uint32_t address = url->address;
    network_handle handle;
    enum network_status status;
    const char *method = head_only ? "HEAD " : "GET ";
    static const char version[] = " HTTP/1.1\r\nHost: ";
    static const char tail[] =
        "\r\nUser-Agent: Phipia/2.1\r\nConnection: close\r\n\r\n";

    if (!url->numeric) {
        uint64_t now = clock_monotonic_ns();
        if (now >= deadline) { return NETWORK_STATUS_TIMEOUT; }
        status = network_resolve(url->host, &address, deadline - now);
        if (status != NETWORK_STATUS_OK) { return status; }
    }
    status = network_tcp_open(owner, &handle);
    if (status != NETWORK_STATUS_OK) { return status; }
    uint64_t now = clock_monotonic_ns();
    if (now >= deadline) {
        (void)network_close(owner, handle);
        return NETWORK_STATUS_TIMEOUT;
    }
    status = network_tcp_connect(owner, handle, address, url->port,
        deadline - now);
    if (status != NETWORK_STATUS_OK) {
        (void)network_close(owner, handle);
        return status;
    }
    const size_t method_length = string_length_bounded(method, 8U);
    const size_t target_length = string_length_bounded(url->target, 511U);
    const size_t version_length = sizeof(version) - 1U;
    const size_t host_length = string_length_bounded(url->host,
        NETWORK_MAX_HOSTNAME);
    const size_t tail_length = sizeof(tail) - 1U;
    if (method_length + target_length + version_length + host_length +
            tail_length + 8U >= sizeof(request)) {
        (void)network_close(owner, handle);
        return NETWORK_STATUS_TOO_LARGE;
    }
    for (size_t index = 0U; index < method_length; ++index) {
        request[used++] = method[index];
    }
    for (size_t index = 0U; index < target_length; ++index) {
        request[used++] = url->target[index];
    }
    for (size_t index = 0U; index < version_length; ++index) {
        request[used++] = version[index];
    }
    for (size_t index = 0U; index < host_length; ++index) {
        request[used++] = url->host[index];
    }
    if (url->port != 80U) {
        char digits[5];
        size_t digit_count = 0U;
        uint16_t port = url->port;
        request[used++] = ':';
        do {
            digits[digit_count++] = (char)('0' + port % 10U);
            port = (uint16_t)(port / 10U);
        } while (port != 0U);
        while (digit_count != 0U) {
            request[used++] = digits[--digit_count];
        }
    }
    for (size_t index = 0U; index < tail_length; ++index) {
        request[used++] = tail[index];
    }
    now = clock_monotonic_ns();
    status = now >= deadline ? NETWORK_STATUS_TIMEOUT :
        network_tcp_write(owner, handle, (const uint8_t *)request, used,
            &written, deadline - now);
    if (status != NETWORK_STATUS_OK || written != used) {
        (void)network_close(owner, handle);
        return status == NETWORK_STATUS_OK ? NETWORK_STATUS_HTTP_FAILURE :
            status;
    }
    zero_bytes(stream, sizeof(*stream));
    stream->owner = owner;
    stream->handle = handle;
    stream->deadline_ns = deadline;
    status = http_parse_headers(stream, response);
    if (status != NETWORK_STATUS_OK) {
        (void)network_close(owner, handle);
    }
    return status;
}

static enum network_status finalize_download(
    const char *destination,
    const char *temporary,
    const char *backup
)
{
    struct phipfs_stat existing;
    enum phipfs_status status = phipfs_stat_path(PHIPFS_VOLUME_DATA,
        destination, &existing);
    bool had_existing = status == PHIPFS_STATUS_OK;

    if (had_existing && existing.directory) {
        return NETWORK_STATUS_FILESYSTEM;
    }
    if (status != PHIPFS_STATUS_OK && status != PHIPFS_STATUS_NOT_FOUND) {
        return filesystem_status(status);
    }
    (void)phipfs_unlink(PHIPFS_VOLUME_DATA, backup);
    if (had_existing && phipfs_rename(PHIPFS_VOLUME_DATA, destination,
            backup) != PHIPFS_STATUS_OK) {
        return NETWORK_STATUS_FILESYSTEM;
    }
    if (phipfs_rename(PHIPFS_VOLUME_DATA, temporary, destination) !=
            PHIPFS_STATUS_OK) {
        if (had_existing) {
            (void)phipfs_rename(PHIPFS_VOLUME_DATA, backup, destination);
        }
        return NETWORK_STATUS_FILESYSTEM;
    }
    if (phipfs_sync(PHIPFS_VOLUME_DATA) != PHIPFS_STATUS_OK) {
        if (had_existing) {
            (void)phipfs_rename(PHIPFS_VOLUME_DATA, destination, temporary);
            (void)phipfs_rename(PHIPFS_VOLUME_DATA, backup, destination);
            (void)phipfs_sync(PHIPFS_VOLUME_DATA);
        }
        return NETWORK_STATUS_FILESYSTEM;
    }
    if (had_existing) {
        if (phipfs_unlink(PHIPFS_VOLUME_DATA, backup) != PHIPFS_STATUS_OK ||
            phipfs_sync(PHIPFS_VOLUME_DATA) != PHIPFS_STATUS_OK) {
            return NETWORK_STATUS_FILESYSTEM;
        }
    }
    return NETWORK_STATUS_OK;
}

enum network_status network_http_download(
    uint64_t owner,
    const char *url,
    const char *destination,
    bool head_only,
    uint64_t timeout_ns,
    struct network_http_result *result
)
{
    char current[768];
    char seen[NETWORK_HTTP_MAX_REDIRECTS + 1U][768];
    char temporary[PHIPFS_MAX_PATH + 1U];
    char backup[PHIPFS_MAX_PATH + 1U];
    uint64_t deadline;
    uint64_t request_started;
    size_t url_length;

    if (url == NULL || destination == NULL || result == NULL || owner == 0U) {
        return NETWORK_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(result, sizeof(*result));
    url_length = string_length_bounded(url, sizeof(current) - 1U);
    if (url_length == 0U || url_length >= sizeof(current) ||
        !destination_valid(destination) || !deadline_valid(timeout_ns) ||
        !download_sibling_paths(destination, temporary, backup)) {
        return NETWORK_STATUS_INVALID_ARGUMENT;
    }
    if (!runtime.public.active || !runtime.public.configuration.configured) {
        return NETWORK_STATUS_UNCONFIGURED;
    }
    request_started = clock_monotonic_ns();
    string_copy(current, url, url_length);
    deadline = request_started + timeout_ns;
    for (size_t redirect = 0U; redirect <= NETWORK_HTTP_MAX_REDIRECTS;
         ++redirect) {
        struct parsed_http_url parsed;
        struct http_stream stream;
        struct http_response response;
        enum network_status status;

        if (!parse_http_url(current, &parsed)) {
            return NETWORK_STATUS_UNSUPPORTED;
        }
        for (size_t index = 0U; index < redirect; ++index) {
            if (string_equal(seen[index], current)) {
                return NETWORK_STATUS_HTTP_FAILURE;
            }
        }
        string_copy(seen[redirect], current,
            string_length_bounded(current, sizeof(seen[redirect]) - 1U));
        status = http_open_request(owner, &parsed, head_only, deadline,
            &stream, &response);
        if (status != NETWORK_STATUS_OK) {
            return status;
        }
        result->status_code = response.status;
        if (response.status == 301U || response.status == 302U ||
            response.status == 303U || response.status == 307U ||
            response.status == 308U) {
            if (response.location[0] == '\0' ||
                redirect == NETWORK_HTTP_MAX_REDIRECTS) {
                (void)network_close(owner, stream.handle);
                return NETWORK_STATUS_HTTP_FAILURE;
            }
            string_copy(current, response.location,
                string_length_bounded(response.location,
                    sizeof(response.location) - 1U));
            ++result->redirects;
            (void)network_tcp_shutdown(owner, stream.handle,
                NETWORK_DEFAULT_READ_TIMEOUT_NS);
            (void)network_close(owner, stream.handle);
            continue;
        }
        if (response.status != 200U || head_only) {
            (void)network_tcp_shutdown(owner, stream.handle,
                NETWORK_DEFAULT_READ_TIMEOUT_NS);
            (void)network_close(owner, stream.handle);
            return head_only && response.status >= 200U &&
                response.status < 400U ? NETWORK_STATUS_OK :
                NETWORK_STATUS_HTTP_FAILURE;
        }
        (void)phipfs_unlink(PHIPFS_VOLUME_DATA, temporary);
        if (phipfs_create(PHIPFS_VOLUME_DATA, temporary) != PHIPFS_STATUS_OK) {
            (void)network_close(owner, stream.handle);
            return NETWORK_STATUS_FILESYSTEM;
        }
        phipfs_handle file;
        enum phipfs_status fs_status = phipfs_open(PHIPFS_VOLUME_DATA, temporary,
            PHIPFS_ACCESS_WRITE, &file);
        struct http_sink sink = {
            NULL, 0U, file, 0U, true
        };
        if (fs_status != PHIPFS_STATUS_OK) {
            (void)phipfs_unlink(PHIPFS_VOLUME_DATA, temporary);
            (void)network_close(owner, stream.handle);
            return filesystem_status(fs_status);
        }
        result->chunked = response.chunked;
        if (response.chunked) {
            status = http_chunked_body(&stream, &sink);
        } else if (response.content_length_present) {
            status = http_fixed_body(&stream, &sink,
                response.content_length);
        } else {
            status = http_connection_body(&stream, &sink);
        }
        result->body_bytes = sink.total;
        fs_status = phipfs_close(file);
        (void)network_tcp_shutdown(owner, stream.handle,
            NETWORK_DEFAULT_READ_TIMEOUT_NS);
        (void)network_close(owner, stream.handle);
        if (status != NETWORK_STATUS_OK || fs_status != PHIPFS_STATUS_OK ||
            (response.content_length_present &&
                result->body_bytes != response.content_length)) {
            (void)phipfs_unlink(PHIPFS_VOLUME_DATA, temporary);
            return status != NETWORK_STATUS_OK ? status :
                NETWORK_STATUS_FILESYSTEM;
        }
        const uint64_t synchronize_started = clock_monotonic_ns();

        if (phipfs_sync(PHIPFS_VOLUME_DATA) != PHIPFS_STATUS_OK) {
            (void)phipfs_unlink(PHIPFS_VOLUME_DATA, temporary);
            return NETWORK_STATUS_FILESYSTEM;
        }
        status = finalize_download(destination, temporary, backup);
        if (status != NETWORK_STATUS_OK) {
            (void)phipfs_unlink(PHIPFS_VOLUME_DATA, temporary);
            return status;
        }
        result->synchronize_ns = clock_monotonic_ns() - synchronize_started;
        result->elapsed_ns = clock_monotonic_ns() - request_started;
        result->synchronized = true;
        return NETWORK_STATUS_OK;
    }
    return NETWORK_STATUS_HTTP_FAILURE;
}

enum network_status network_http_memory(
    uint64_t owner,
    const char *url,
    bool head_only,
    uint64_t timeout_ns,
    uint8_t *bytes,
    size_t capacity,
    struct network_http_result *result
)
{
    char current[768];
    char seen[NETWORK_HTTP_MAX_REDIRECTS + 1U][768];
    uint64_t deadline;
    uint64_t request_started;
    size_t url_length;

    if (url == NULL || bytes == NULL || result == NULL || owner == 0U) {
        return NETWORK_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(result, sizeof(*result));
    url_length = string_length_bounded(url, sizeof(current) - 1U);
    if (url_length == 0U || url_length >= sizeof(current) || capacity == 0U ||
        capacity > NETWORK_HTTP_MAX_DOWNLOAD_BYTES ||
        !deadline_valid(timeout_ns)) {
        return NETWORK_STATUS_INVALID_ARGUMENT;
    }
    if (!runtime.public.active || !runtime.public.configuration.configured) {
        return NETWORK_STATUS_UNCONFIGURED;
    }
    request_started = clock_monotonic_ns();
    string_copy(current, url, url_length);
    deadline = request_started + timeout_ns;
    for (size_t redirect = 0U; redirect <= NETWORK_HTTP_MAX_REDIRECTS;
         ++redirect) {
        struct parsed_http_url parsed;
        struct http_stream stream;
        struct http_response response;
        struct http_sink sink = {bytes, capacity, 0U, 0U, false};
        enum network_status status;

        if (!parse_http_url(current, &parsed)) {
            return NETWORK_STATUS_UNSUPPORTED;
        }
        for (size_t index = 0U; index < redirect; ++index) {
            if (string_equal(seen[index], current)) {
                return NETWORK_STATUS_HTTP_FAILURE;
            }
        }
        string_copy(seen[redirect], current,
            string_length_bounded(current, sizeof(seen[redirect]) - 1U));
        status = http_open_request(owner, &parsed, head_only, deadline,
            &stream, &response);
        if (status != NETWORK_STATUS_OK) {
            return status;
        }
        result->status_code = response.status;
        if (response.status == 301U || response.status == 302U ||
            response.status == 303U || response.status == 307U ||
            response.status == 308U) {
            if (response.location[0] == '\0' ||
                redirect == NETWORK_HTTP_MAX_REDIRECTS) {
                (void)network_close(owner, stream.handle);
                return NETWORK_STATUS_HTTP_FAILURE;
            }
            string_copy(current, response.location,
                string_length_bounded(response.location,
                    sizeof(response.location) - 1U));
            ++result->redirects;
            (void)network_tcp_shutdown(owner, stream.handle,
                NETWORK_DEFAULT_READ_TIMEOUT_NS);
            (void)network_close(owner, stream.handle);
            continue;
        }
        if (response.status != 200U || head_only) {
            (void)network_tcp_shutdown(owner, stream.handle,
                NETWORK_DEFAULT_READ_TIMEOUT_NS);
            (void)network_close(owner, stream.handle);
            return head_only && response.status >= 200U &&
                response.status < 400U ? NETWORK_STATUS_OK :
                NETWORK_STATUS_HTTP_FAILURE;
        }
        result->chunked = response.chunked;
        if (response.chunked) {
            status = http_chunked_body(&stream, &sink);
        } else if (response.content_length_present) {
            if (response.content_length > capacity) {
                status = NETWORK_STATUS_TOO_LARGE;
            } else {
                status = http_fixed_body(&stream, &sink,
                    response.content_length);
            }
        } else {
            status = http_connection_body(&stream, &sink);
        }
        result->body_bytes = sink.total;
        (void)network_tcp_shutdown(owner, stream.handle,
            NETWORK_DEFAULT_READ_TIMEOUT_NS);
        (void)network_close(owner, stream.handle);
        if (status != NETWORK_STATUS_OK ||
            (response.content_length_present &&
                result->body_bytes != response.content_length)) {
            zero_bytes(bytes, sink.total);
            result->body_bytes = 0U;
            return status != NETWORK_STATUS_OK ? status :
                NETWORK_STATUS_HTTP_FAILURE;
        }
        result->elapsed_ns = clock_monotonic_ns() - request_started;
        return NETWORK_STATUS_OK;
    }
    return NETWORK_STATUS_HTTP_FAILURE;
}

enum network_status network_shutdown(void)
{
    enum virtio_net_status status;

    if (!runtime.public.active) {
        return NETWORK_STATUS_NOT_INITIALIZED;
    }
    for (size_t index = 0U; index < NETWORK_MAX_UDP_SOCKETS; ++index) {
        if (runtime.udp[index].active) {
            zero_bytes(&runtime.udp[index], sizeof(runtime.udp[index]));
        }
    }
    for (size_t index = 0U; index < NETWORK_MAX_TCP_CONNECTIONS; ++index) {
        if (runtime.tcp[index].active) {
            zero_bytes(&runtime.tcp[index], sizeof(runtime.tcp[index]));
        }
    }
    runtime.public.udp_sockets = 0U;
    runtime.public.tcp_connections = 0U;
    runtime.public.tcp_listeners = 0U;
    runtime.timers = 0U;
    runtime.public.timers = 0U;
    ++runtime.public.statistics.resets;
    status = virtio_net_shutdown();
    runtime.public.active = false;
    runtime.public.configuration.configured = false;
    ++runtime.public.configuration.generation;
    arp_invalidate();
    zero_bytes(runtime.dns, sizeof(runtime.dns));
    runtime.public.dns_entries = 0U;
    return status == VIRTIO_NET_STATUS_OK ? NETWORK_STATUS_OK :
        NETWORK_STATUS_UNAVAILABLE;
}

struct network_state network_get_state(void)
{
    return runtime.public;
}

void network_format_ipv4(uint32_t address, char output[16])
{
    size_t used = 0U;

    if (output == NULL) {
        return;
    }
    for (size_t octet = 0U; octet < 4U; ++octet) {
        uint8_t value = (uint8_t)(address >> (24U - octet * 8U));
        char digits[3];
        size_t count = 0U;

        do {
            digits[count++] = (char)('0' + value % 10U);
            value = (uint8_t)(value / 10U);
        } while (value != 0U);
        while (count != 0U) {
            output[used++] = digits[--count];
        }
        if (octet != 3U) {
            output[used++] = '.';
        }
    }
    output[used] = '\0';
}

bool network_parse_ipv4(const char *text, uint32_t *address)
{
    uint32_t result = 0U;
    size_t index = 0U;

    if (text == NULL || address == NULL) {
        return false;
    }
    for (size_t octet = 0U; octet < 4U; ++octet) {
        uint32_t value = 0U;
        size_t digits = 0U;

        while (text[index] >= '0' && text[index] <= '9') {
            value = value * 10U + (uint32_t)(text[index] - '0');
            if (value > 255U || ++digits > 3U) {
                return false;
            }
            ++index;
        }
        if (digits == 0U || (octet != 3U && text[index++] != '.') ||
            (octet == 3U && text[index] != '\0')) {
            return false;
        }
        result = (result << 8U) | value;
    }
    *address = result;
    return true;
}

bool network_self_test(size_t *completed_tests)
{
    uint8_t sample[20];
    char formatted[16];
    uint32_t address;
    uint64_t value;
    size_t completed = 0U;

    if (completed_tests == NULL) {
        return false;
    }
    zero_bytes(sample, sizeof(sample));
    sample[0] = 0x45U;
    write_be16(sample + 2U, sizeof(sample));
    sample[8] = 64U;
    sample[9] = IPV4_PROTOCOL_UDP;
    write_be32(sample + 12U, UINT32_C(0x0A00020F));
    write_be32(sample + 16U, UINT32_C(0x0A000203));
    write_be16(sample + 10U, internet_checksum(sample, sizeof(sample)));
    if (internet_checksum(sample, sizeof(sample)) != 0U) { return false; }
    ++completed;
    sample[2] ^= 1U;
    if (internet_checksum(sample, sizeof(sample)) == 0U) { return false; }
    ++completed;
    if (!network_parse_ipv4("10.0.2.15", &address) ||
        address != UINT32_C(0x0A00020F) ||
        network_parse_ipv4("256.0.0.1", &address) ||
        network_parse_ipv4("1.2.3", &address)) { return false; }
    completed += 3U;
    network_format_ipv4(UINT32_C(0x0A00020F), formatted);
    if (!string_equal(formatted, "10.0.2.15")) { return false; }
    ++completed;
    if (!hostname_valid("phipia.test") || hostname_valid("-phipia.test") ||
        hostname_valid("phipia..test")) { return false; }
    completed += 3U;
    if (!parse_u64_decimal("16777216", 8U, &value) ||
        value != NETWORK_HTTP_MAX_DOWNLOAD_BYTES ||
        parse_u64_decimal("18446744073709551616", 20U, &value)) {
        return false;
    }
    completed += 2U;
    if (!destination_valid("downloads/welcome.txt") ||
        destination_valid("../escape.txt") ||
        destination_valid("/absolute.txt")) { return false; }
    completed += 3U;
    struct parsed_http_url url;
    if (!parse_http_url("http://phipia.test/welcome.txt", &url) ||
        url.port != 80U || !string_equal(url.host, "phipia.test") ||
        parse_http_url("https://phipia.test/", &url) ||
        parse_http_url("http://user@phipia.test/", &url)) { return false; }
    completed += 3U;
    if (!sequence_before(UINT32_MAX - 1U, 1U) ||
        sequence_before(1U, UINT32_MAX - 1U)) { return false; }
    ++completed;
    if (NETWORK_MAX_UDP_SOCKETS != 8U ||
        NETWORK_MAX_TCP_CONNECTIONS != 8U ||
        NETWORK_MAX_TIMERS != 32U || NETWORK_MAX_POLL_HANDLES != 8U) {
        return false;
    }
    completed += 4U;
    if (NETWORK_TCP_MAX_BACKLOG == 0U ||
        NETWORK_TCP_MAX_BACKLOG > NETWORK_MAX_TCP_CONNECTIONS - 1U) {
        return false;
    }
    ++completed;
    if (TCP_CONNECTION_LISTEN == TCP_CONNECTION_OPEN ||
        TCP_CONNECTION_SYN_RECEIVED == TCP_CONNECTION_SYN_SENT ||
        TCP_CONNECTION_SYN_RECEIVED == TCP_CONNECTION_ESTABLISHED) {
        return false;
    }
    ++completed;
    if (runtime.servicing) {
        return false;
    }
    ++completed;
    *completed_tests = completed;
    return completed == 25U;
}

const char *network_status_string(enum network_status status)
{
    static const char *const messages[NETWORK_STATUS_COUNT] = {
        "ok", "network unavailable", "link down", "network unconfigured",
        "network already initialized", "network not initialized",
        "null network argument", "invalid network argument",
        "network value outside bounds", "network operation timed out",
        "network operation cancelled", "network operation would block",
        "network resources exhausted", "stale network handle",
        "network handle belongs to another process",
        "network handle is in the wrong mode", "socket already bound",
        "port already in use", "network device reset", "malformed packet",
        "invalid packet checksum", "fragmented IPv4 packet unsupported",
        "network operation unsupported", "destination unreachable",
        "DHCP server rejected the request", "DNS resolution failed",
        "connection reset", "connection closed", "HTTP request failed",
        "filesystem operation failed", "network value is too large"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        NETWORK_STATUS_COUNT, "network status messages drifted");
    if (status < NETWORK_STATUS_OK || status >= NETWORK_STATUS_COUNT) {
        return "unknown network status";
    }
    return messages[status];
}
