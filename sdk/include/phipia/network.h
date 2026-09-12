/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_USER_NETWORK_H
#define PHIPIA_USER_NETWORK_H

#include <stddef.h>
#include <stdint.h>
#include <phipia/abi.h>

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
long phipia_datagram_open(void);
long phipia_datagram_bind(phipia_handle_t datagram, uint16_t port);
long phipia_datagram_send(phipia_handle_t datagram,
    const struct phipia_ipv4_endpoint *destination, const void *buffer,
    size_t length, uint64_t deadline_ns);
long phipia_datagram_receive(phipia_handle_t datagram,
    struct phipia_ipv4_endpoint *source, void *buffer, size_t length,
    uint64_t deadline_ns);
long phipia_network_address(phipia_handle_t handle, int peer,
    struct phipia_ipv4_endpoint *endpoint);
long phipia_network_cancel(phipia_handle_t handle);

#endif
