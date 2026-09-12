/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_NATIVE_HANDLE_H
#define PHIPIA_NATIVE_HANDLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/abi/base.h>

#define NATIVE_HANDLE_LIMIT 128U
#define NATIVE_RESOURCE_WORDS 4U

enum native_handle_status {
    NATIVE_HANDLE_OK = 0,
    NATIVE_HANDLE_NULL_ARGUMENT,
    NATIVE_HANDLE_BAD_LIMIT,
    NATIVE_HANDLE_BAD_TYPE,
    NATIVE_HANDLE_FULL,
    NATIVE_HANDLE_STALE,
    NATIVE_HANDLE_WRONG_TYPE,
    NATIVE_HANDLE_CLOSE_FAILED,
    NATIVE_HANDLE_STATUS_COUNT
};

struct native_resource {
    uint64_t words[NATIVE_RESOURCE_WORDS];
};

struct native_handle_slot {
    uint32_t generation;
    uint16_t object_index;
    uint8_t type;
    bool active;
};

struct native_handle_object {
    struct native_resource resource;
    uint16_t references;
    uint8_t type;
    bool active;
};

struct native_handle_table {
    struct native_handle_slot slots[NATIVE_HANDLE_LIMIT];
    struct native_handle_object objects[NATIVE_HANDLE_LIMIT];
    uint16_t limit;
    uint16_t active_handles;
    uint16_t active_objects;
    bool initialized;
};

typedef bool (*native_handle_close_fn)(
    uint8_t type,
    const struct native_resource *resource,
    void *context
);

enum native_handle_status native_handle_table_initialize(
    struct native_handle_table *table,
    uint16_t limit
);
enum native_handle_status native_handle_install(
    struct native_handle_table *table,
    uint8_t type,
    const struct native_resource *resource,
    phipia_handle_t *handle
);
enum native_handle_status native_handle_resolve(
    struct native_handle_table *table,
    phipia_handle_t handle,
    uint8_t expected_type,
    struct native_resource **resource
);
enum native_handle_status native_handle_duplicate(
    struct native_handle_table *table,
    phipia_handle_t source,
    phipia_handle_t *duplicate
);
enum native_handle_status native_handle_close(
    struct native_handle_table *table,
    phipia_handle_t handle,
    native_handle_close_fn close_resource,
    void *context
);
enum native_handle_status native_handle_close_all(
    struct native_handle_table *table,
    native_handle_close_fn close_resource,
    void *context
);
bool native_handle_self_test(size_t *completed_tests);

#endif
