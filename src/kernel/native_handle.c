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

static bool invalid_handle_encoding(
    const struct native_handle_table *table,
    phipia_handle_t handle
)
{
    const uint64_t encoded_index = handle & HANDLE_INDEX_MASK;
    const uint8_t encoded_type = (uint8_t)(handle >> HANDLE_TYPE_SHIFT);
    const uint8_t reserved = (uint8_t)(handle >> HANDLE_RESERVED_SHIFT);
    const uint32_t generation = (uint32_t)(handle >>
        HANDLE_GENERATION_SHIFT);

    return handle == PHIPIA_HANDLE_INVALID || encoded_index == 0U ||
        encoded_index > table->limit || reserved != 0U || generation == 0U ||
        !valid_type(encoded_type);
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
    const uint32_t generation = (uint32_t)(handle >>
        HANDLE_GENERATION_SHIFT);
    size_t index;

    if (table == NULL || slot == NULL || slot_index == NULL) {
        return NATIVE_HANDLE_NULL_ARGUMENT;
    }
    if (!table->initialized || table->limit == 0U ||
        table->limit > NATIVE_HANDLE_LIMIT ||
        invalid_handle_encoding(table, handle)) {
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

static bool valid_slot_object(
    const struct native_handle_table *table,
    const struct native_handle_slot *slot,
    const struct native_handle_object **object
)
{
    const struct native_handle_object *candidate;

    if (table == NULL || slot == NULL || object == NULL ||
        slot->object_index >= table->limit) {
        return false;
    }
    candidate = &table->objects[slot->object_index];
    if (!candidate->active || candidate->references == 0U ||
        candidate->type != slot->type) {
        return false;
    }
    *object = candidate;
    return true;
}

static void retire_slot(
    struct native_handle_table *table,
    struct native_handle_slot *slot,
    struct native_handle_object *object
)
{
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
}

static void report_record(
    struct native_handle_close_report *report,
    phipia_handle_t handle,
    uint8_t type,
    uint16_t object_index,
    uint16_t references,
    enum native_handle_close_outcome outcome
)
{
    const uint16_t entry_index = report->attempted_handles;

    ++report->attempted_handles;
    switch (outcome) {
    case NATIVE_HANDLE_CLOSE_OUTCOME_CLOSED:
        ++report->closed_resources;
        break;
    case NATIVE_HANDLE_CLOSE_OUTCOME_CONSUMED_ERROR:
        ++report->consumed_error_resources;
        break;
    case NATIVE_HANDLE_CLOSE_OUTCOME_RETAINED:
        ++report->retained_resources;
        break;
    case NATIVE_HANDLE_CLOSE_OUTCOME_DUPLICATE:
        ++report->duplicate_references;
        break;
    case NATIVE_HANDLE_CLOSE_OUTCOME_STALE:
        ++report->stale_entries;
        break;
    case NATIVE_HANDLE_CLOSE_OUTCOME_INVALID:
        ++report->invalid_entries;
        break;
    default:
        ++report->invalid_entries;
        outcome = NATIVE_HANDLE_CLOSE_OUTCOME_INVALID;
        break;
    }
    if (entry_index < NATIVE_HANDLE_CLOSE_REPORT_CAPACITY) {
        report->entries[entry_index].handle = handle;
        report->entries[entry_index].object_index = object_index;
        report->entries[entry_index].references = references;
        report->entries[entry_index].type = type;
        report->entries[entry_index].outcome = (uint8_t)outcome;
    } else {
        ++report->omitted_entries;
        report->truncated = true;
    }
}

static enum native_handle_status close_valid_slot(
    struct native_handle_table *table,
    size_t slot_index,
    native_handle_close_fn close_resource,
    void *context,
    struct native_handle_close_report *report
)
{
    struct native_handle_slot *slot = &table->slots[slot_index];
    struct native_handle_object *object = &table->objects[slot->object_index];
    const phipia_handle_t handle = encode_handle(slot_index, slot->type,
        slot->generation);
    const uint8_t type = object->type;
    const uint16_t object_index = slot->object_index;
    const uint16_t references = object->references;
    enum native_resource_close_result closed;

    if (references > 1U) {
        retire_slot(table, slot, object);
        if (report != NULL) {
            report_record(report, handle, type, object_index,
                references, NATIVE_HANDLE_CLOSE_OUTCOME_DUPLICATE);
            ++report->retired_handles;
        }
        return NATIVE_HANDLE_OK;
    }
    if (close_resource != NULL) {
        if (report != NULL) {
            ++report->callback_attempts;
        }
        closed = close_resource(type, &object->resource, context);
    } else {
        closed = NATIVE_RESOURCE_CLOSED;
    }
    if (closed == NATIVE_RESOURCE_RETAINED) {
        if (report != NULL) {
            report_record(report, handle, type, object_index,
                references, NATIVE_HANDLE_CLOSE_OUTCOME_RETAINED);
        }
        return NATIVE_HANDLE_CLOSE_FAILED;
    }
    if (closed != NATIVE_RESOURCE_CLOSED &&
        closed != NATIVE_RESOURCE_CLOSED_WITH_ERROR) {
        /* Unknown callback results cannot prove that the resource was consumed. */
        if (report != NULL) {
            report_record(report, handle, type, object_index,
                references, NATIVE_HANDLE_CLOSE_OUTCOME_RETAINED);
        }
        return NATIVE_HANDLE_CLOSE_FAILED;
    }
    retire_slot(table, slot, object);
    if (report != NULL) {
        report_record(report, handle, type, object_index,
            references, closed == NATIVE_RESOURCE_CLOSED ?
                NATIVE_HANDLE_CLOSE_OUTCOME_CLOSED :
                NATIVE_HANDLE_CLOSE_OUTCOME_CONSUMED_ERROR);
        ++report->retired_handles;
    }
    return closed == NATIVE_RESOURCE_CLOSED_WITH_ERROR ?
        NATIVE_HANDLE_CLOSE_FAILED : NATIVE_HANDLE_OK;
}

enum native_handle_status native_handle_close(
    struct native_handle_table *table,
    phipia_handle_t handle,
    native_handle_close_fn close_resource,
    void *context
)
{
    struct native_handle_slot *slot;
    const struct native_handle_object *object;
    size_t slot_index;
    enum native_handle_status status = decode_slot(table, handle, &slot,
        &slot_index);

    if (status != NATIVE_HANDLE_OK) {
        return status;
    }
    if (!valid_slot_object(table, slot, &object)) {
        return NATIVE_HANDLE_STALE;
    }
    return close_valid_slot(table, slot_index, close_resource, context, NULL);
}

enum native_handle_status native_handle_close_with_report(
    struct native_handle_table *table,
    phipia_handle_t handle,
    native_handle_close_fn close_resource,
    void *context,
    struct native_handle_close_report *report
)
{
    struct native_handle_slot *slot;
    const struct native_handle_object *object;
    size_t slot_index;
    enum native_handle_status status;

    if (report == NULL) {
        return NATIVE_HANDLE_NULL_ARGUMENT;
    }
    native_handle_close_report_reset(report);
    if (table == NULL) {
        ++report->invalid_arguments;
        report->status = NATIVE_HANDLE_NULL_ARGUMENT;
        return NATIVE_HANDLE_NULL_ARGUMENT;
    }
    if (!table->initialized || table->limit == 0U ||
        table->limit > NATIVE_HANDLE_LIMIT) {
        ++report->invalid_arguments;
        report->status = NATIVE_HANDLE_BAD_LIMIT;
        return NATIVE_HANDLE_BAD_LIMIT;
    }
    report->active_handles_before = table->active_handles;
    report->active_objects_before = table->active_objects;
    status = decode_slot(table, handle, &slot, &slot_index);
    if (status != NATIVE_HANDLE_OK) {
        const uint8_t type = (uint8_t)(handle >> HANDLE_TYPE_SHIFT);

        report_record(report, handle, type, UINT16_MAX, 0U,
            invalid_handle_encoding(table, handle) ?
            NATIVE_HANDLE_CLOSE_OUTCOME_INVALID :
            NATIVE_HANDLE_CLOSE_OUTCOME_STALE);
        report->active_handles_after = table->active_handles;
        report->active_objects_after = table->active_objects;
        report->status = status;
        return status;
    }
    if (slot->generation == 0U || !valid_type(slot->type)) {
        report_record(report, handle, slot->type, slot->object_index, 0U,
            NATIVE_HANDLE_CLOSE_OUTCOME_INVALID);
        report->active_handles_after = table->active_handles;
        report->active_objects_after = table->active_objects;
        report->status = NATIVE_HANDLE_STALE;
        return NATIVE_HANDLE_STALE;
    }
    if (!valid_slot_object(table, slot, &object)) {
        report_record(report, handle, slot->type, slot->object_index,
            slot->object_index < table->limit ?
                table->objects[slot->object_index].references : 0U,
            slot->object_index >= table->limit ?
                NATIVE_HANDLE_CLOSE_OUTCOME_INVALID :
                NATIVE_HANDLE_CLOSE_OUTCOME_STALE);
        report->active_handles_after = table->active_handles;
        report->active_objects_after = table->active_objects;
        report->status = NATIVE_HANDLE_STALE;
        return NATIVE_HANDLE_STALE;
    }
    (void)object;
    status = close_valid_slot(table, slot_index, close_resource, context,
        report);
    report->active_handles_after = table->active_handles;
    report->active_objects_after = table->active_objects;
    report->retryable = report->retained_resources != 0U;
    report->progress = report->retired_handles != 0U;
    report->status = status;
    return status;
}

static enum native_handle_status close_all_impl(
    struct native_handle_table *table,
    native_handle_close_fn close_resource,
    void *context,
    struct native_handle_close_report *report
)
{
    bool failed = false;

    if (report != NULL) {
        native_handle_close_report_reset(report);
    }
    if (table == NULL) {
        if (report != NULL) {
            ++report->invalid_arguments;
            report->status = NATIVE_HANDLE_NULL_ARGUMENT;
        }
        return NATIVE_HANDLE_NULL_ARGUMENT;
    }
    if (!table->initialized || table->limit == 0U ||
        table->limit > NATIVE_HANDLE_LIMIT) {
        if (report != NULL) {
            ++report->invalid_arguments;
            report->status = NATIVE_HANDLE_BAD_LIMIT;
        }
        return NATIVE_HANDLE_BAD_LIMIT;
    }
    if (report != NULL) {
        report->active_handles_before = table->active_handles;
        report->active_objects_before = table->active_objects;
    }
    for (size_t index = 0U; index < table->limit; ++index) {
        struct native_handle_slot *slot = &table->slots[index];
        const struct native_handle_object *object;
        enum native_handle_status status;

        if (!slot->active) {
            continue;
        }
        if (slot->generation == 0U || !valid_type(slot->type)) {
            if (report != NULL) {
                report_record(report, encode_handle(index, slot->type,
                    slot->generation), slot->type, slot->object_index, 0U,
                    NATIVE_HANDLE_CLOSE_OUTCOME_INVALID);
            }
            failed = true;
            continue;
        }
        if (!valid_slot_object(table, slot, &object)) {
            if (report != NULL) {
                const enum native_handle_close_outcome outcome =
                    slot->object_index >= table->limit ?
                        NATIVE_HANDLE_CLOSE_OUTCOME_INVALID :
                        NATIVE_HANDLE_CLOSE_OUTCOME_STALE;

                report_record(report, encode_handle(index, slot->type,
                    slot->generation), slot->type, slot->object_index,
                    slot->object_index < table->limit ?
                        table->objects[slot->object_index].references : 0U,
                    outcome);
            }
            failed = true;
            continue;
        }
        status = close_valid_slot(table, index, close_resource, context,
            report);
        if (status != NATIVE_HANDLE_OK) {
            failed = true;
        }
    }
    if (report != NULL) {
        report->active_handles_after = table->active_handles;
        report->active_objects_after = table->active_objects;
        report->retryable = report->retained_resources != 0U;
        report->progress = report->retired_handles != 0U;
        report->status = failed ? NATIVE_HANDLE_CLOSE_FAILED :
            NATIVE_HANDLE_OK;
    }
    return failed ? NATIVE_HANDLE_CLOSE_FAILED : NATIVE_HANDLE_OK;
}

enum native_handle_status native_handle_close_all(
    struct native_handle_table *table,
    native_handle_close_fn close_resource,
    void *context
)
{
    return close_all_impl(table, close_resource, context, NULL);
}

enum native_handle_status native_handle_close_all_report(
    struct native_handle_table *table,
    native_handle_close_fn close_resource,
    void *context,
    bool *retryable
)
{
    struct native_handle_close_report report;
    enum native_handle_status status;

    if (retryable == NULL) {
        return NATIVE_HANDLE_NULL_ARGUMENT;
    }
    *retryable = false;
    status = native_handle_close_all_diagnostics(table, close_resource,
        context, &report);
    *retryable = report.retryable;
    return status;
}

void native_handle_close_report_reset(struct native_handle_close_report *report)
{
    if (report != NULL) {
        zero_bytes(report, sizeof(*report));
        report->status = NATIVE_HANDLE_OK;
    }
}

enum native_handle_status native_handle_close_all_diagnostics(
    struct native_handle_table *table,
    native_handle_close_fn close_resource,
    void *context,
    struct native_handle_close_report *report
)
{
    if (report == NULL) {
        return NATIVE_HANDLE_NULL_ARGUMENT;
    }
    return close_all_impl(table, close_resource, context, report);
}

static enum native_resource_close_result test_close(
    uint8_t type,
    const struct native_resource *resource,
    void *context
)
{
    size_t *closed = context;

    if (type != PHIPIA_HANDLE_FILE || resource == NULL || closed == NULL ||
        resource->words[0] != UINT64_C(0x5341504F5445)) {
        return NATIVE_RESOURCE_RETAINED;
    }
    ++*closed;
    return NATIVE_RESOURCE_CLOSED;
}

static enum native_resource_close_result self_test_close_result;

static enum native_resource_close_result self_test_close(
    uint8_t type,
    const struct native_resource *resource,
    void *context
)
{
    size_t *closed = context;

    if (type != PHIPIA_HANDLE_FILE || resource == NULL || closed == NULL ||
        resource->words[0] != UINT64_C(0x5341504F5445)) {
        return NATIVE_RESOURCE_RETAINED;
    }
    ++*closed;
    return self_test_close_result;
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
    struct native_handle_close_report report;
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
    self_test_close_result = NATIVE_RESOURCE_RETAINED;
    if (native_handle_install(&table, PHIPIA_HANDLE_FILE, &initial, &first) !=
            NATIVE_HANDLE_OK) {
        return false;
    }
    if (native_handle_close_all_diagnostics(&table, self_test_close, &closed,
            &report) != NATIVE_HANDLE_CLOSE_FAILED || !report.retryable ||
        report.attempted_handles != 1U || report.callback_attempts != 1U ||
        report.retained_resources != 1U || report.retired_handles != 0U ||
        report.active_handles_after != 1U || report.progress ||
        table.active_handles != 1U || table.active_objects != 1U ||
        closed != 2U) {
        return false;
    }
    self_test_close_result = NATIVE_RESOURCE_CLOSED_WITH_ERROR;
    if (native_handle_close_all_diagnostics(&table, self_test_close, &closed,
            &report) != NATIVE_HANDLE_CLOSE_FAILED || report.retryable ||
        report.attempted_handles != 1U || report.callback_attempts != 1U ||
        report.consumed_error_resources != 1U || report.retired_handles != 1U ||
        report.active_handles_after != 0U || !report.progress ||
        table.active_handles != 0U || table.active_objects != 0U ||
        closed != 3U) {
        return false;
    }
    ++*completed_tests;
    return true;
}
