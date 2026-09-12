/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_NETWORK_SYSCALL_H
#define PHIPIA_NETWORK_SYSCALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/network.h>
#include <phipia/paging.h>

/* Experimental and intentionally private to Phipia 2.1. */
#define NETWORK_SYSCALL_ABI_VERSION UINT16_C(1)
#define NETWORK_SYSCALL_MAX_CONTEXTS 4U
#define NETWORK_SYSCALL_MAX_TRANSFER 4096U
#define NETWORK_SYSCALL_MAX_TIMEOUT_NS UINT64_C(30000000000)

enum network_syscall_operation {
    NETWORK_SYSCALL_QUERY_TIME = 0,
    NETWORK_SYSCALL_RANDOM,
    NETWORK_SYSCALL_RESOLVE,
    NETWORK_SYSCALL_STREAM_OPEN,
    NETWORK_SYSCALL_STREAM_CONNECT,
    NETWORK_SYSCALL_STREAM_READ,
    NETWORK_SYSCALL_STREAM_WRITE,
    NETWORK_SYSCALL_STREAM_SHUTDOWN,
    NETWORK_SYSCALL_STREAM_CLOSE,
    NETWORK_SYSCALL_POLL,
    NETWORK_SYSCALL_CANCEL,
    NETWORK_SYSCALL_HTTP_TO_MEMORY,
    NETWORK_SYSCALL_HTTP_TO_FILE,
    NETWORK_SYSCALL_OPERATION_COUNT
};

enum network_syscall_status {
    NETWORK_SYSCALL_STATUS_OK = 0,
    NETWORK_SYSCALL_STATUS_NULL_ARGUMENT,
    NETWORK_SYSCALL_STATUS_BAD_CONTEXT,
    NETWORK_SYSCALL_STATUS_BAD_TOKEN,
    NETWORK_SYSCALL_STATUS_BAD_VERSION,
    NETWORK_SYSCALL_STATUS_BAD_OPERATION,
    NETWORK_SYSCALL_STATUS_BAD_POINTER,
    NETWORK_SYSCALL_STATUS_BAD_LENGTH,
    NETWORK_SYSCALL_STATUS_BAD_TIMEOUT,
    NETWORK_SYSCALL_STATUS_NETWORK,
    NETWORK_SYSCALL_STATUS_NO_CONTEXTS,
    NETWORK_SYSCALL_STATUS_COUNT
};

struct network_syscall_request {
    uint16_t version;
    uint16_t size;
    uint32_t operation;
    uint64_t handle;
    uint64_t primary_address;
    uint64_t secondary_address;
    uint64_t timeout_ns;
    uint32_t primary_length;
    uint32_t secondary_length;
    uint32_t ipv4_address;
    uint32_t interests;
    uint16_t port;
    uint16_t flags;
};

struct network_syscall_response {
    uint16_t version;
    uint16_t size;
    uint32_t operation;
    enum network_syscall_status boundary_status;
    enum network_status network_status;
    uint64_t handle;
    uint64_t monotonic_ns;
    uint32_t value;
    uint32_t ready;
    uint32_t ipv4_address;
    uint16_t http_status;
    uint16_t reserved;
};

struct network_syscall_authenticator {
    uint64_t token;
    uint64_t process_generation;
};

enum network_syscall_status network_syscall_register(
    const struct paging_process_space *address_space,
    uint64_t process_generation,
    struct network_syscall_authenticator *authenticator
);
enum network_syscall_status network_syscall_dispatch(
    const struct network_syscall_authenticator *authenticator,
    uint64_t request_address,
    uint64_t response_address
);
void network_syscall_process_terminated(
    const struct network_syscall_authenticator *authenticator
);
bool network_syscall_self_test(size_t *completed_tests);
const char *network_syscall_status_string(enum network_syscall_status status);

#endif
