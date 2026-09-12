/* SPDX-License-Identifier: GPL-3.0-only */
/* Process-local, typed, generation-protected native capability handles. */

#include <phipia/native_handle.h>

#define HANDLE_INDEX_MASK UINT64_C(0xFFFF)
#define HANDLE_TYPE_SHIFT 16U
#define HANDLE_RESERVED_SHIFT 24U
#define HANDLE_GENERATION_SHIFT 32U

static void zero_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool valid_type(uint8_t type)
{
    return type >= PHIPIA_HANDLE_FILE &&
        type <= PHIPIA_HANDLE_PACKAGE_CONTROL;
}

static phipia_handle_t encode_handle(
    size_t index,
    uint8_t type,
    uint32_t generation
)
{
    return ((uint64_t)generation << HANDLE_GENERATION_SHIFT) |
        ((uint64_t)type << HANDLE_TYPE_SHIFT) | (uint64_t)(index + 1U);
}

static enum native_handle_status decode_slot(
    struct native_handle_table *table,
    phipia_handle_t handle,
    struct native_handle_slot **slot,
    size_t *slot_index
)
{
    const uint64_t encoded_index = handle & HANDLE_INDEX_MASK;
    const uint8_t encoded_type = (uint8_t)(handle >> HANDLE_TYPE_SHIFT);
    const uint8_t reserved = (uint8_t)(handle >> HANDLE_RESERVED_SHIFT);
    const uint32_t generation = (uint32_t)(handle >>
        HANDLE_GENERATION_SHIFT);
    size_t index;

    if (table == NULL || slot == NULL || slot_index == NULL) {
        return NATIVE_HANDLE_NULL_ARGUMENT;
    }
    if (!table->initialized || handle == PHIPIA_HANDLE_INVALID ||
        encoded_index == 0U || encoded_index > table->limit ||
        reserved != 0U || generation == 0U || !valid_type(encoded_type)) {
        return NATIVE_HANDLE_STALE;
    }
    index = (size_t)(encoded_index - 1U);
    if (!table->slots[index].active ||
        table->slots[index].generation != generation ||
        table->slots[index].type != encoded_type) {
        return NATIVE_HANDLE_STALE;
    }
    *slot = &table->slots[index];
    *slot_index = index;
    return NATIVE_HANDLE_OK;
}

enum native_handle_status native_handle_table_initialize(
    struct native_handle_table *table,
    uint16_t limit
)
{
    if (table == NULL) {
        return NATIVE_HANDLE_NULL_ARGUMENT;
    }
    if (limit == 0U || limit > NATIVE_HANDLE_LIMIT) {
        return NATIVE_HANDLE_BAD_LIMIT;
    }
    zero_bytes(table, sizeof(*table));
    table->limit = limit;
    table->initialized = true;
    for (size_t index = 0U; index < table->limit; ++index) {
        table->slots[index].generation = 1U;
    }
    return NATIVE_HANDLE_OK;
}

enum native_handle_status native_handle_install(
    struct native_handle_table *table,
    uint8_t type,
    const struct native_resource *resource,
    phipia_handle_t *handle
)
{
    size_t slot_index = SIZE_MAX;
    size_t object_index = SIZE_MAX;

    if (table == NULL || resource == NULL || handle == NULL) {
        return NATIVE_HANDLE_NULL_ARGUMENT;
    }
    *handle = PHIPIA_HANDLE_INVALID;
    if (!table->initialized) {
        return NATIVE_HANDLE_BAD_LIMIT;
    }
    if (!valid_type(type)) {
        return NATIVE_HANDLE_BAD_TYPE;
    }
    for (size_t index = 0U; index < table->limit; ++index) {
        if (!table->slots[index].active && slot_index == SIZE_MAX) {
            slot_index = index;
        }
        if (!table->objects[index].active && object_index == SIZE_MAX) {
            object_index = index;
        }
    }
    if (slot_index == SIZE_MAX || object_index == SIZE_MAX) {
        return NATIVE_HANDLE_FULL;
    }
    table->objects[object_index].resource = *resource;
    table->objects[object_index].references = 1U;
    table->objects[object_index].type = type;
    table->objects[object_index].active = true;
    table->slots[slot_index].object_index = (uint16_t)object_index;
    table->slots[slot_index].type = type;
    table->slots[slot_index].active = true;
    ++table->active_handles;
    ++table->active_objects;
    *handle = encode_handle(slot_index, type,
        table->slots[slot_index].generation);
    return NATIVE_HANDLE_OK;
}

enum native_handle_status native_handle_resolve(
    struct native_handle_table *table,
    phipia_handle_t handle,
    uint8_t expected_type,
    struct native_resource **resource
)
{
    struct native_handle_slot *slot;
    size_t slot_index;
    enum native_handle_status status;
    struct native_handle_object *object;

    if (resource == NULL) {
        return NATIVE_HANDLE_NULL_ARGUMENT;
    }
    *resource = NULL;
    status = decode_slot(table, handle, &slot, &slot_index);
    if (status != NATIVE_HANDLE_OK) {
        return status;
    }
    (void)slot_index;
    if (expected_type != 0U && slot->type != expected_type) {
        return NATIVE_HANDLE_WRONG_TYPE;
    }
    if (slot->object_index >= table->limit) {
        return NATIVE_HANDLE_STALE;
    }
    object = &table->objects[slot->object_index];
    if (!object->active || object->references == 0U ||
        object->type != slot->type) {
        return NATIVE_HANDLE_STALE;
    }
    *resource = &object->resource;
    return NATIVE_HANDLE_OK;
}

enum native_handle_status native_handle_duplicate(
    struct native_handle_table *table,
    phipia_handle_t source,
    phipia_handle_t *duplicate
)
{
    struct native_handle_slot *source_slot;
    size_t source_index;
    size_t target_index = SIZE_MAX;
    enum native_handle_status status;
    struct native_handle_object *object;

    if (duplicate == NULL) {
        return NATIVE_HANDLE_NULL_ARGUMENT;
    }
    *duplicate = PHIPIA_HANDLE_INVALID;
    status = decode_slot(table, source, &source_slot, &source_index);
    if (status != NATIVE_HANDLE_OK) {
        return status;
    }
    (void)source_index;
    for (size_t index = 0U; index < table->limit; ++index) {
        if (!table->slots[index].active) {
            target_index = index;
            break;
        }
    }
    if (target_index == SIZE_MAX || table->active_handles >= table->limit) {
        return NATIVE_HANDLE_FULL;
    }
    object = &table->objects[source_slot->object_index];
    if (!object->active || object->references == UINT16_MAX) {
        return NATIVE_HANDLE_STALE;
    }
    ++object->references;
    table->slots[target_index].object_index = source_slot->object_index;
    table->slots[target_index].type = source_slot->type;
    table->slots[target_index].active = true;
    ++table->active_handles;
    *duplicate = encode_handle(target_index, source_slot->type,
        table->slots[target_index].generation);
    return NATIVE_HANDLE_OK;
}

enum native_handle_status native_handle_close(
    struct native_handle_table *table,
    phipia_handle_t handle,
    native_handle_close_fn close_resource,
    void *context
)
{
    struct native_handle_slot *slot;
    struct native_handle_object *object;
    size_t slot_index;
    enum native_handle_status status = decode_slot(table, handle, &slot,
        &slot_index);

    if (status != NATIVE_HANDLE_OK) {
        return status;
    }
    object = &table->objects[slot->object_index];
    if (!object->active || object->references == 0U) {
        return NATIVE_HANDLE_STALE;
    }
    if (object->references == 1U && close_resource != NULL &&
        !close_resource(object->type, &object->resource, context)) {
        return NATIVE_HANDLE_CLOSE_FAILED;
    }
    slot->active = false;
    slot->type = 0U;
    slot->object_index = 0U;
    ++slot->generation;
    if (slot->generation == 0U) {
        slot->generation = 1U;
    }
    --table->active_handles;
    --object->references;
    if (object->references == 0U) {
        zero_bytes(object, sizeof(*object));
        --table->active_objects;
    }
    (void)slot_index;
    return NATIVE_HANDLE_OK;
}

enum native_handle_status native_handle_close_all(
    struct native_handle_table *table,
    native_handle_close_fn close_resource,
    void *context
)
{
    bool failed = false;

    if (table == NULL) {
        return NATIVE_HANDLE_NULL_ARGUMENT;
    }
    if (!table->initialized) {
        return NATIVE_HANDLE_BAD_LIMIT;
    }
    for (size_t index = 0U; index < table->limit; ++index) {
        if (table->slots[index].active) {
            const phipia_handle_t handle = encode_handle(index,
                table->slots[index].type, table->slots[index].generation);

            if (native_handle_close(table, handle, close_resource, context) !=
                    NATIVE_HANDLE_OK) {
                failed = true;
            }
        }
    }
    return failed ? NATIVE_HANDLE_CLOSE_FAILED : NATIVE_HANDLE_OK;
}

static bool test_close(
    uint8_t type,
    const struct native_resource *resource,
    void *context
)
{
    size_t *closed = context;

    if (type != PHIPIA_HANDLE_FILE || resource == NULL || closed == NULL ||
        resource->words[0] != UINT64_C(0x5341504F5445)) {
        return false;
    }
    ++*closed;
    return true;
}

bool native_handle_self_test(size_t *completed_tests)
{
    /* Keep the 6 KiB table off the finite boot/interrupt stack. */
    static struct native_handle_table table;
    const struct native_resource initial = {
        { UINT64_C(0x5341504F5445), 0U, 0U, 0U }
    };
    struct native_resource *resolved;
    phipia_handle_t first;
    phipia_handle_t duplicate;
    size_t closed = 0U;

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;
    if (!valid_type(PHIPIA_HANDLE_PACKAGE_CONTROL) ||
        valid_type((uint8_t)(PHIPIA_HANDLE_PACKAGE_CONTROL + 1U))) {
        return false;
    }
    ++*completed_tests;
    if (native_handle_table_initialize(&table, 2U) != NATIVE_HANDLE_OK ||
        native_handle_install(&table, PHIPIA_HANDLE_FILE, &initial, &first) !=
            NATIVE_HANDLE_OK ||
        native_handle_resolve(&table, first, PHIPIA_HANDLE_FILE, &resolved) !=
            NATIVE_HANDLE_OK || resolved->words[0] != initial.words[0]) {
        return false;
    }
    ++*completed_tests;
    if (native_handle_resolve(&table, first, PHIPIA_HANDLE_TIMER, &resolved) !=
            NATIVE_HANDLE_WRONG_TYPE ||
        native_handle_duplicate(&table, first, &duplicate) !=
            NATIVE_HANDLE_OK || first == duplicate ||
        table.active_handles != 2U || table.active_objects != 1U) {
        return false;
    }
    ++*completed_tests;
    if (native_handle_close(&table, first, test_close, &closed) !=
            NATIVE_HANDLE_OK || closed != 0U ||
        native_handle_resolve(&table, first, PHIPIA_HANDLE_FILE, &resolved) !=
            NATIVE_HANDLE_STALE ||
        native_handle_close(&table, first, test_close, &closed) !=
            NATIVE_HANDLE_STALE) {
        return false;
    }
    ++*completed_tests;
    if (native_handle_close_all(&table, test_close, &closed) !=
            NATIVE_HANDLE_OK || closed != 1U || table.active_handles != 0U ||
        table.active_objects != 0U ||
        native_handle_resolve(&table, duplicate, PHIPIA_HANDLE_FILE,
            &resolved) != NATIVE_HANDLE_STALE) {
        return false;
    }
    ++*completed_tests;
    return true;
}
