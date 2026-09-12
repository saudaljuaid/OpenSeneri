/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/clock.h>
#include <phipia/fat32_fs.h>
#include <phipia/memory.h>
#include <phipia/network.h>
#include <phipia/network_syscall.h>
#include <phipia/random.h>

#define TOKEN_KIND UINT64_C(0x4e53000000000000)
#define TOKEN_KIND_MASK UINT64_C(0xffff000000000000)
#define TOKEN_GENERATION_SHIFT 8U
#define TOKEN_INDEX_MASK UINT64_C(0xff)

struct syscall_context {
    const struct paging_process_space *address_space;
    uint64_t process_generation;
    uint64_t generation;
    uint64_t owner;
    bool active;
    uint8_t transfer[NETWORK_SYSCALL_MAX_TRANSFER];
    char primary_text[768];
    char secondary_text[PHIPFS_MAX_PATH + 1U];
};

static struct syscall_context contexts[NETWORK_SYSCALL_MAX_CONTEXTS];
static uint64_t next_generation = UINT64_C(1);

static void clear_bytes(void *destination, size_t length)
{
    volatile uint8_t *bytes = destination;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void clear_temporary(struct syscall_context *context)
{
    if (context == NULL) {
        return;
    }
    clear_bytes(context->transfer, sizeof(context->transfer));
    clear_bytes(context->primary_text, sizeof(context->primary_text));
    clear_bytes(context->secondary_text, sizeof(context->secondary_text));
}

static bool canonical_user(uint64_t address)
{
    return address < UINT64_C(0x0000800000000000);
}

static bool range_shape_valid(uint64_t address, size_t length)
{
    return address != 0U && length != 0U &&
        length <= NETWORK_SYSCALL_MAX_TRANSFER && canonical_user(address) &&
        address <= UINT64_MAX - (uint64_t)(length - 1U) &&
        canonical_user(address + (uint64_t)(length - 1U));
}

static bool user_range_valid(
    const struct syscall_context *context,
    uint64_t address,
    size_t length,
    bool writable
)
{
    uint64_t cursor = address;
    size_t remaining = length;

    if (context == NULL || !context->active || context->address_space == NULL ||
        !range_shape_valid(address, length)) {
        return false;
    }
    while (remaining != 0U) {
        struct paging_translation translation;
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (cursor & (PAGING_PAGE_SIZE - 1U)));

        if (chunk > remaining) {
            chunk = remaining;
        }
        if (paging_process_translate(context->address_space, cursor,
                &translation) != PAGING_STATUS_OK || !translation.user ||
            translation.level != 1U ||
            (writable && translation.permissions != PAGING_WRITE) ||
            !frame_range_overlaps_allocatable_memory(
                translation.physical_address, chunk)) {
            return false;
        }
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool copy_from_user(
    const struct syscall_context *context,
    void *destination,
    uint64_t address,
    size_t length
)
{
    uint8_t *output = destination;
    uint64_t cursor = address;
    size_t remaining = length;

    /* Validate every page before copying the first byte. */
    if (destination == NULL ||
        !user_range_valid(context, address, length, false)) {
        return false;
    }
    while (remaining != 0U) {
        struct paging_translation translation;
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (cursor & (PAGING_PAGE_SIZE - 1U)));

        if (chunk > remaining) {
            chunk = remaining;
        }
        if (paging_process_translate(context->address_space, cursor,
                &translation) != PAGING_STATUS_OK) {
            return false;
        }
        const uint8_t *input =
            (const uint8_t *)(uintptr_t)translation.physical_address;

        for (size_t index = 0U; index < chunk; ++index) {
            output[index] = input[index];
        }
        output += chunk;
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool copy_to_user(
    const struct syscall_context *context,
    uint64_t address,
    const void *source,
    size_t length
)
{
    const uint8_t *input = source;
    uint64_t cursor = address;
    size_t remaining = length;

    /* Validate every page before storing the first byte. */
    if (source == NULL || !user_range_valid(context, address, length, true)) {
        return false;
    }
    while (remaining != 0U) {
        struct paging_translation translation;
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (cursor & (PAGING_PAGE_SIZE - 1U)));

        if (chunk > remaining) {
            chunk = remaining;
        }
        if (paging_process_translate(context->address_space, cursor,
                &translation) != PAGING_STATUS_OK) {
            return false;
        }
        uint8_t *output = (uint8_t *)(uintptr_t)translation.physical_address;

        for (size_t index = 0U; index < chunk; ++index) {
            output[index] = input[index];
        }
        input += chunk;
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

static uint64_t make_token(size_t index, uint64_t generation)
{
    return TOKEN_KIND | (generation << TOKEN_GENERATION_SHIFT) |
        (uint64_t)index;
}

static struct syscall_context *authenticate(
    const struct network_syscall_authenticator *authenticator
)
{
    size_t index;
    struct syscall_context *context;

    if (authenticator == NULL ||
        (authenticator->token & TOKEN_KIND_MASK) != TOKEN_KIND) {
        return NULL;
    }
    index = (size_t)(authenticator->token & TOKEN_INDEX_MASK);
    if (index >= NETWORK_SYSCALL_MAX_CONTEXTS) {
        return NULL;
    }
    context = &contexts[index];
    if (!context->active || context->process_generation == 0U ||
        context->process_generation != authenticator->process_generation ||
        authenticator->token != make_token(index, context->generation)) {
        return NULL;
    }
    return context;
}

static enum network_syscall_status request_valid(
    const struct network_syscall_request *request
)
{
    if (request->version != NETWORK_SYSCALL_ABI_VERSION ||
        request->size != sizeof(*request)) {
        return NETWORK_SYSCALL_STATUS_BAD_VERSION;
    }
    if (request->operation >= (uint32_t)NETWORK_SYSCALL_OPERATION_COUNT) {
        return NETWORK_SYSCALL_STATUS_BAD_OPERATION;
    }
    if (request->timeout_ns > NETWORK_SYSCALL_MAX_TIMEOUT_NS) {
        return NETWORK_SYSCALL_STATUS_BAD_TIMEOUT;
    }
    if (request->primary_length > NETWORK_SYSCALL_MAX_TRANSFER ||
        request->secondary_length > NETWORK_SYSCALL_MAX_TRANSFER) {
        return NETWORK_SYSCALL_STATUS_BAD_LENGTH;
    }
    return NETWORK_SYSCALL_STATUS_OK;
}

static void initialize_response(
    const struct network_syscall_request *request,
    struct network_syscall_response *response
)
{
    response->version = NETWORK_SYSCALL_ABI_VERSION;
    response->size = sizeof(*response);
    response->operation = request->operation;
    response->boundary_status = NETWORK_SYSCALL_STATUS_OK;
    response->network_status = NETWORK_STATUS_OK;
    response->handle = request->handle;
    response->monotonic_ns = 0U;
    response->value = 0U;
    response->ready = 0U;
    response->ipv4_address = 0U;
    response->http_status = 0U;
    response->reserved = 0U;
}

static bool copy_text(
    const struct syscall_context *context,
    uint64_t address,
    size_t length,
    char *destination,
    size_t capacity
)
{
    if (length == 0U || length >= capacity ||
        !copy_from_user(context, destination, address, length)) {
        return false;
    }
    destination[length] = '\0';
    for (size_t index = 0U; index < length; ++index) {
        if (destination[index] == '\0') {
            return false;
        }
    }
    return true;
}

static enum network_syscall_status dispatch_operation(
    struct syscall_context *context,
    const struct network_syscall_request *request,
    struct network_syscall_response *response
)
{
    enum network_status status = NETWORK_STATUS_OK;

    switch ((enum network_syscall_operation)request->operation) {
        case NETWORK_SYSCALL_QUERY_TIME:
            response->monotonic_ns = clock_monotonic_ns();
            break;
        case NETWORK_SYSCALL_RANDOM:
            if (request->primary_length == 0U ||
                request->primary_length > RANDOM_MAX_REQUEST_BYTES ||
                !user_range_valid(context, request->primary_address,
                    request->primary_length, true)) {
                return NETWORK_SYSCALL_STATUS_BAD_POINTER;
            }
            if (random_bytes(context->transfer, request->primary_length) !=
                    RANDOM_STATUS_OK ||
                !copy_to_user(context, request->primary_address,
                    context->transfer, request->primary_length)) {
                return NETWORK_SYSCALL_STATUS_BAD_POINTER;
            }
            response->value = request->primary_length;
            break;
        case NETWORK_SYSCALL_RESOLVE:
            if (request->primary_length > NETWORK_MAX_HOSTNAME ||
                !copy_text(context, request->primary_address,
                    request->primary_length, context->primary_text,
                    sizeof(context->primary_text))) {
                return NETWORK_SYSCALL_STATUS_BAD_POINTER;
            }
            status = network_resolve(context->primary_text,
                &response->ipv4_address, request->timeout_ns);
            break;
        case NETWORK_SYSCALL_STREAM_OPEN:
            status = network_tcp_open(context->owner, &response->handle);
            break;
        case NETWORK_SYSCALL_STREAM_CONNECT:
            status = network_tcp_connect(context->owner, request->handle,
                request->ipv4_address, request->port, request->timeout_ns);
            break;
        case NETWORK_SYSCALL_STREAM_READ: {
            size_t completed = 0U;

            if (request->primary_length == 0U ||
                !user_range_valid(context, request->primary_address,
                    request->primary_length, true)) {
                return NETWORK_SYSCALL_STATUS_BAD_POINTER;
            }
            status = network_tcp_read(context->owner, request->handle,
                context->transfer, request->primary_length, &completed,
                request->timeout_ns);
            if (completed != 0U && !copy_to_user(context,
                    request->primary_address, context->transfer, completed)) {
                return NETWORK_SYSCALL_STATUS_BAD_POINTER;
            }
            response->value = (uint32_t)completed;
            break;
        }
        case NETWORK_SYSCALL_STREAM_WRITE: {
            size_t completed = 0U;

            if (request->primary_length == 0U ||
                !copy_from_user(context, context->transfer,
                    request->primary_address, request->primary_length)) {
                return NETWORK_SYSCALL_STATUS_BAD_POINTER;
            }
            status = network_tcp_write(context->owner, request->handle,
                context->transfer, request->primary_length, &completed,
                request->timeout_ns);
            response->value = (uint32_t)completed;
            break;
        }
        case NETWORK_SYSCALL_STREAM_SHUTDOWN:
            status = network_tcp_shutdown(context->owner, request->handle,
                request->timeout_ns);
            break;
        case NETWORK_SYSCALL_STREAM_CLOSE:
            status = network_close(context->owner, request->handle);
            break;
        case NETWORK_SYSCALL_POLL: {
            const struct network_poll_request poll_request = {
                request->handle, request->interests
            };
            struct network_poll_result poll_result;
            size_t completed = 0U;

            status = network_poll(context->owner, &poll_request, 1U,
                &poll_result, 1U, &completed, request->timeout_ns);
            if (completed == 1U) {
                response->ready = poll_result.ready;
                if (status == NETWORK_STATUS_OK) {
                    status = poll_result.error;
                }
            }
            break;
        }
        case NETWORK_SYSCALL_CANCEL:
            status = network_cancel(context->owner, request->handle);
            break;
        case NETWORK_SYSCALL_HTTP_TO_MEMORY: {
            struct network_http_result result;

            if (request->primary_length == 0U ||
                request->primary_length >= sizeof(context->primary_text) ||
                request->secondary_length == 0U ||
                !copy_text(context, request->primary_address,
                    request->primary_length, context->primary_text,
                    sizeof(context->primary_text)) ||
                !user_range_valid(context, request->secondary_address,
                    request->secondary_length, true)) {
                return NETWORK_SYSCALL_STATUS_BAD_POINTER;
            }
            status = network_http_memory(context->owner,
                context->primary_text, (request->flags & UINT16_C(1)) != 0U,
                request->timeout_ns, context->transfer,
                request->secondary_length, &result);
            if (status == NETWORK_STATUS_OK && result.body_bytes != 0U &&
                !copy_to_user(context, request->secondary_address,
                    context->transfer, result.body_bytes)) {
                return NETWORK_SYSCALL_STATUS_BAD_POINTER;
            }
            response->http_status = result.status_code;
            response->value = result.body_bytes;
            break;
        }
        case NETWORK_SYSCALL_HTTP_TO_FILE: {
            struct network_http_result result;

            if (request->primary_length >= sizeof(context->primary_text) ||
                request->secondary_length > PHIPFS_MAX_PATH ||
                !copy_text(context, request->primary_address,
                    request->primary_length, context->primary_text,
                    sizeof(context->primary_text)) ||
                !copy_text(context, request->secondary_address,
                    request->secondary_length, context->secondary_text,
                    sizeof(context->secondary_text))) {
                return NETWORK_SYSCALL_STATUS_BAD_POINTER;
            }
            status = network_http_download(context->owner,
                context->primary_text, context->secondary_text,
                (request->flags & UINT16_C(1)) != 0U, request->timeout_ns,
                &result);
            response->http_status = result.status_code;
            response->value = result.body_bytes;
            break;
        }
        case NETWORK_SYSCALL_OPERATION_COUNT:
        default:
            return NETWORK_SYSCALL_STATUS_BAD_OPERATION;
    }
    response->network_status = status;
    if (status != NETWORK_STATUS_OK) {
        response->boundary_status = NETWORK_SYSCALL_STATUS_NETWORK;
        return NETWORK_SYSCALL_STATUS_NETWORK;
    }
    return NETWORK_SYSCALL_STATUS_OK;
}

enum network_syscall_status network_syscall_register(
    const struct paging_process_space *address_space,
    uint64_t process_generation,
    struct network_syscall_authenticator *authenticator
)
{
    if (address_space == NULL || authenticator == NULL ||
        process_generation == 0U || process_generation > (UINT64_MAX >> 8U)) {
        return NETWORK_SYSCALL_STATUS_NULL_ARGUMENT;
    }
    for (size_t index = 0U; index < NETWORK_SYSCALL_MAX_CONTEXTS; ++index) {
        struct syscall_context *context = &contexts[index];

        if (!context->active) {
            clear_temporary(context);
            context->address_space = address_space;
            context->process_generation = process_generation;
            context->generation = next_generation++;
            if (next_generation == 0U ||
                next_generation >= (UINT64_C(1) << 40U)) {
                next_generation = 1U;
            }
            context->owner = (process_generation << 8U) | (index + 1U);
            context->active = true;
            authenticator->token = make_token(index, context->generation);
            authenticator->process_generation = process_generation;
            return NETWORK_SYSCALL_STATUS_OK;
        }
    }
    return NETWORK_SYSCALL_STATUS_NO_CONTEXTS;
}

enum network_syscall_status network_syscall_dispatch(
    const struct network_syscall_authenticator *authenticator,
    uint64_t request_address,
    uint64_t response_address
)
{
    struct network_syscall_request request;
    struct network_syscall_response response;
    struct syscall_context *context = authenticate(authenticator);
    enum network_syscall_status status;

    if (context == NULL) {
        return NETWORK_SYSCALL_STATUS_BAD_TOKEN;
    }
    /* The response range is authenticated before any operation can mutate. */
    if (!user_range_valid(context, response_address, sizeof(response), true) ||
        !copy_from_user(context, &request, request_address,
            sizeof(request))) {
        return NETWORK_SYSCALL_STATUS_BAD_POINTER;
    }
    status = request_valid(&request);
    initialize_response(&request, &response);
    if (status == NETWORK_SYSCALL_STATUS_OK) {
        status = dispatch_operation(context, &request, &response);
    }
    response.boundary_status = status;
    if (!copy_to_user(context, response_address, &response,
            sizeof(response))) {
        clear_temporary(context);
        return NETWORK_SYSCALL_STATUS_BAD_POINTER;
    }
    clear_temporary(context);
    return status;
}

void network_syscall_process_terminated(
    const struct network_syscall_authenticator *authenticator
)
{
    struct syscall_context *context = authenticate(authenticator);

    if (context != NULL) {
        network_process_terminated(context->owner);
        clear_temporary(context);
        context->address_space = NULL;
        context->process_generation = 0U;
        context->owner = 0U;
        context->active = false;
    }
}

bool network_syscall_self_test(size_t *completed_tests)
{
    size_t completed = 0U;
    struct network_syscall_request request = {
        NETWORK_SYSCALL_ABI_VERSION, sizeof(request),
        NETWORK_SYSCALL_QUERY_TIME, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U
    };

    if (completed_tests == NULL) {
        return false;
    }
    if (!canonical_user(UINT64_C(0x7fffffffffff)) ||
        canonical_user(UINT64_C(0x800000000000)) ||
        range_shape_valid(0U, 1U) || range_shape_valid(1U, 0U) ||
        range_shape_valid(UINT64_MAX, 2U) ||
        !range_shape_valid(UINT64_C(0x1000), 1U)) {
        return false;
    }
    completed += 6U;
    if (request_valid(&request) != NETWORK_SYSCALL_STATUS_OK) {
        return false;
    }
    ++completed;
    request.version = 0U;
    if (request_valid(&request) != NETWORK_SYSCALL_STATUS_BAD_VERSION) {
        return false;
    }
    ++completed;
    request.version = NETWORK_SYSCALL_ABI_VERSION;
    request.operation = NETWORK_SYSCALL_OPERATION_COUNT;
    if (request_valid(&request) != NETWORK_SYSCALL_STATUS_BAD_OPERATION) {
        return false;
    }
    ++completed;
    request.operation = NETWORK_SYSCALL_QUERY_TIME;
    request.timeout_ns = NETWORK_SYSCALL_MAX_TIMEOUT_NS + 1U;
    if (request_valid(&request) != NETWORK_SYSCALL_STATUS_BAD_TIMEOUT) {
        return false;
    }
    ++completed;
    if ((make_token(3U, 7U) & TOKEN_KIND_MASK) != TOKEN_KIND ||
        (make_token(3U, 7U) & TOKEN_INDEX_MASK) != 3U ||
        authenticate(NULL) != NULL) {
        return false;
    }
    completed += 3U;
    contexts[0].transfer[0] = UINT8_C(0xA5);
    contexts[0].primary_text[0] = 'A';
    contexts[0].secondary_text[0] = 'B';
    contexts[1].transfer[0] = UINT8_C(0x5A);
    contexts[1].primary_text[0] = 'C';
    contexts[1].secondary_text[0] = 'D';
    clear_temporary(&contexts[0]);
    if (contexts[0].transfer[0] != 0U ||
        contexts[0].primary_text[0] != '\0' ||
        contexts[0].secondary_text[0] != '\0' ||
        contexts[1].transfer[0] != UINT8_C(0x5A) ||
        contexts[1].primary_text[0] != 'C' ||
        contexts[1].secondary_text[0] != 'D' ||
        &contexts[0].transfer[0] == &contexts[1].transfer[0] ||
        &contexts[0].primary_text[0] == &contexts[1].primary_text[0] ||
        &contexts[0].secondary_text[0] ==
            &contexts[1].secondary_text[0]) {
        clear_temporary(&contexts[1]);
        return false;
    }
    clear_temporary(&contexts[1]);
    completed += 9U;
    *completed_tests = completed;
    return completed == 22U;
}

const char *network_syscall_status_string(enum network_syscall_status status)
{
    static const char *const messages[] = {
        "ok", "null argument", "bad context", "bad token", "bad version",
        "bad operation", "bad user pointer", "bad length", "bad timeout",
        "network operation failed", "no syscall contexts"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        (size_t)NETWORK_SYSCALL_STATUS_COUNT,
        "network syscall status messages are out of sync");
    if ((size_t)status >= sizeof(messages) / sizeof(messages[0])) {
        return "unknown network syscall status";
    }
    return messages[status];
}
