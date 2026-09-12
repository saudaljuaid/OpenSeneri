/* SPDX-License-Identifier: GPL-3.0-only */
/* Privileged bounded package install/update session controller. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/heap.h>
#include <phipia/package_builder.h>
#include <phipia/package_control.h>
#include <phipia/package_generation.h>
#include <phipia/package_manager.h>
#include <phipia/package_platform_trust.h>
#include <phipia/package_service.h>
#include <phipia/package_state.h>
#include <phipia/package_upload.h>
#include <phipia/wall_clock.h>

struct control_session {
    struct package_manager_policy policy;
    struct package_manager_trust trust;
    struct package_manager_repository_view repository;
    struct package_state_database_view installed;
    struct package_manager_plan plan;
    struct package_builder_package_bytes
        packages[PACKAGE_CONTROL_PLAN_MAX_PACKAGES];
    uint8_t *repository_bytes;
    uint8_t *installed_bytes;
    uint8_t target[PACKAGE_CONTROL_TEXT_BYTES];
    size_t repository_byte_count;
    size_t installed_byte_count;
    size_t payload_bytes;
    uint64_t owner;
    uint64_t result_generation;
    uint32_t generation;
    uint32_t attached_count;
    bool attached[PACKAGE_CONTROL_PLAN_MAX_PACKAGES];
    bool active;
    bool has_installed;
    bool prepared;
    bool committed;
};

static struct control_session sessions[PACKAGE_CONTROL_SESSION_LIMIT];
static bool servicing;

_Static_assert(PACKAGE_CONTROL_PLAN_MAX_PACKAGES <=
    PACKAGE_MANAGER_PLAN_MAX_PACKAGES,
    "controller plan bound exceeds the authenticated planner bound");
_Static_assert(PACKAGE_CONTROL_DATABASE_MAX_BYTES <=
    PACKAGE_SERVICE_MAX_DATABASE_BYTES,
    "controller database bound exceeds the transaction service bound");

static void zero_bytes(void *destination, size_t count)
{
    uint8_t *bytes = destination;

    for (size_t index = 0U; index < count; ++index) {
        bytes[index] = 0U;
    }
}

static void copy_bytes(uint8_t *destination, const uint8_t *source,
    size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        destination[index] = source[index];
    }
}

static bool equal_bytes(const uint8_t *left, const uint8_t *right, size_t count)
{
    uint8_t difference = 0U;

    for (size_t index = 0U; index < count; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0U;
}

static package_control_token encode_token(size_t index, uint32_t generation)
{
    return (uint64_t)generation << 32U | (uint64_t)(index + 1U);
}

static void initialize_generations(void)
{
    for (size_t index = 0U; index < PACKAGE_CONTROL_SESSION_LIMIT; ++index) {
        if (sessions[index].generation == 0U) {
            sessions[index].generation = 1U;
        }
    }
}

static void clear_report(struct package_control_report *report)
{
    if (report != NULL) {
        zero_bytes(report, sizeof(*report));
        report->status = PACKAGE_CONTROL_STATUS_STATE;
        report->upload_status = PACKAGE_UPLOAD_STATUS_OK;
        report->manager_status = PACKAGE_MANAGER_STATUS_OK;
        report->service_status = PACKAGE_SERVICE_STATUS_OK;
    }
}

static enum package_control_status finish(
    struct package_control_report *report,
    enum package_control_status status,
    const struct control_session *session,
    size_t index
)
{
    if (report != NULL) {
        report->status = status;
        if (session != NULL) {
            report->token = encode_token(index, session->generation);
            report->repository_version = session->repository.repository_version;
            report->generation = session->result_generation != 0U ?
                session->result_generation : (session->has_installed ?
                    session->installed.generation : 0U);
            report->plan_count = session->plan.count;
            report->attached_count = session->attached_count;
            report->prepared = session->prepared;
            report->committed = session->committed;
        }
    }
    return status;
}

static enum package_control_status resolve_session(
    uint64_t owner,
    package_control_token token,
    struct control_session **session,
    size_t *session_index
)
{
    uint32_t encoded_index = (uint32_t)token;
    uint32_t generation = (uint32_t)(token >> 32U);
    size_t index;

    initialize_generations();
    if (owner == 0U || session == NULL || session_index == NULL ||
        encoded_index == 0U || encoded_index > PACKAGE_CONTROL_SESSION_LIMIT ||
        generation == 0U) {
        return PACKAGE_CONTROL_STATUS_STALE;
    }
    index = (size_t)(encoded_index - 1U);
    if (!sessions[index].active || sessions[index].owner != owner ||
        sessions[index].generation != generation) {
        return PACKAGE_CONTROL_STATUS_STALE;
    }
    *session = &sessions[index];
    *session_index = index;
    return PACKAGE_CONTROL_STATUS_OK;
}

static enum package_control_status allocate_buffer(size_t count,
    uint8_t **buffer)
{
    if (count == 0U || heap_allocate(count, (void **)buffer) !=
            HEAP_STATUS_OK) {
        return PACKAGE_CONTROL_STATUS_RESOURCE;
    }
    return PACKAGE_CONTROL_STATUS_OK;
}

static bool release_buffer(uint8_t **buffer)
{
    if (buffer == NULL || *buffer == NULL) {
        return true;
    }
    if (heap_free(*buffer) != HEAP_STATUS_OK) {
        return false;
    }
    *buffer = NULL;
    return true;
}

static enum package_control_status release_session(
    struct control_session *session
)
{
    bool clean = release_buffer(&session->repository_bytes);

    if (!release_buffer(&session->installed_bytes)) {
        clean = false;
    }

    for (size_t index = 0U; index < PACKAGE_CONTROL_PLAN_MAX_PACKAGES;
            ++index) {
        if (!release_buffer((uint8_t **)&session->packages[index].bytes)) {
            clean = false;
        }
    }
    if (!clean) {
        return PACKAGE_CONTROL_STATUS_RESOURCE;
    }
    uint32_t generation = session->generation + 1U;
    if (generation == 0U) {
        generation = 1U;
    }
    zero_bytes(session, sizeof(*session));
    session->generation = generation;
    return PACKAGE_CONTROL_STATUS_OK;
}

static enum package_control_status load_upload(
    uint64_t owner,
    package_upload_token token,
    size_t maximum,
    uint8_t **bytes,
    size_t *byte_count,
    struct package_control_report *report
)
{
    struct package_upload_report upload;

    if (bytes == NULL || byte_count == NULL || *bytes != NULL) {
        return PACKAGE_CONTROL_STATUS_STATE;
    }
    *byte_count = 0U;
    enum package_upload_status upload_status = package_upload_inspect(owner,
        token, &upload);

    report->upload_status = upload_status;
    if (upload_status != PACKAGE_UPLOAD_STATUS_OK) {
        return PACKAGE_CONTROL_STATUS_UPLOAD;
    }
    if (upload.byte_count == 0U || upload.byte_count > maximum ||
        upload.byte_count > SIZE_MAX) {
        return PACKAGE_CONTROL_STATUS_RANGE;
    }
    enum package_control_status status = allocate_buffer(
        (size_t)upload.byte_count, bytes);
    if (status != PACKAGE_CONTROL_STATUS_OK) {
        return status;
    }
    size_t total = 0U;
    while (total < (size_t)upload.byte_count) {
        size_t chunk = (size_t)upload.byte_count - total;
        size_t read_bytes = 0U;

        if (chunk > PACKAGE_UPLOAD_WRITE_MAX) {
            chunk = PACKAGE_UPLOAD_WRITE_MAX;
        }
        upload_status = package_upload_read(owner, token, total,
            *bytes + total, chunk, &read_bytes, &upload);
        if (upload_status != PACKAGE_UPLOAD_STATUS_OK ||
            read_bytes == 0U || read_bytes > chunk) {
            report->upload_status = upload_status;
            return release_buffer(bytes) ? PACKAGE_CONTROL_STATUS_UPLOAD :
                PACKAGE_CONTROL_STATUS_RESOURCE;
        }
        total += read_bytes;
    }
    *byte_count = total;
    return PACKAGE_CONTROL_STATUS_OK;
}

static enum package_control_status load_installed_snapshot(
    struct control_session *session,
    struct package_control_report *report,
    bool repair
)
{
    struct package_service_report service_report;
    size_t database_bytes = 0U;
    enum package_control_status status = allocate_buffer(
        PACKAGE_CONTROL_DATABASE_MAX_BYTES, &session->installed_bytes);

    if (status != PACKAGE_CONTROL_STATUS_OK) {
        return status;
    }
    report->service_status = repair ? package_service_repair_snapshot(
        session->installed_bytes, PACKAGE_CONTROL_DATABASE_MAX_BYTES,
        &database_bytes, &service_report) : package_service_snapshot(
            session->installed_bytes, PACKAGE_CONTROL_DATABASE_MAX_BYTES,
            &database_bytes, &service_report);
    if (report->service_status == PACKAGE_SERVICE_STATUS_ABSENT) {
        (void)release_buffer(&session->installed_bytes);
        return PACKAGE_CONTROL_STATUS_OK;
    }
    if (report->service_status != PACKAGE_SERVICE_STATUS_OK) {
        (void)release_buffer(&session->installed_bytes);
        return PACKAGE_CONTROL_STATUS_SERVICE;
    }
    session->installed_byte_count = database_bytes;
    session->has_installed = true;
    enum package_state_status state_status = package_state_database_parse(
        session->installed_bytes, database_bytes, &session->installed);

    return state_status == PACKAGE_STATE_STATUS_OK ?
        PACKAGE_CONTROL_STATUS_OK : PACKAGE_CONTROL_STATUS_STATE;
}

static enum package_control_status load_installed(
    struct control_session *session,
    struct package_control_report *report
)
{
    return load_installed_snapshot(session, report, false);
}

static bool repair_text_equal(
    const struct package_state_text *left,
    const struct package_manager_text *right
)
{
    return left != NULL && right != NULL && left->bytes != NULL &&
        right->bytes != NULL && left->length == right->length &&
        equal_bytes(left->bytes, right->bytes, left->length);
}

static bool repair_entry_matches(
    const struct package_state_package_view *installed,
    const struct package_manager_catalog_entry *entry
)
{
    return repair_text_equal(&installed->identifier, &entry->identifier) &&
        repair_text_equal(&installed->version, &entry->version) &&
        equal_bytes(installed->package_sha256, entry->package_sha256,
            PACKAGE_MANAGER_SHA256_BYTES) &&
        equal_bytes(installed->publisher_key_id, entry->publisher_key_id,
            PACKAGE_MANAGER_SHA256_BYTES);
}

static enum package_control_status plan_repair(
    struct control_session *session,
    struct package_control_report *report
)
{
    if (!session->has_installed || session->installed.package_count == 0U) {
        report->manager_status = PACKAGE_MANAGER_STATUS_NOT_FOUND;
        return PACKAGE_CONTROL_STATUS_MANAGER;
    }
    if (session->installed.package_count >
            PACKAGE_CONTROL_PLAN_MAX_PACKAGES) {
        return PACKAGE_CONTROL_STATUS_RANGE;
    }
    zero_bytes(&session->plan, sizeof(session->plan));
    session->plan.operation = PACKAGE_MANAGER_PLAN_REPAIR;
    for (uint32_t installed_index = 0U;
         installed_index < session->installed.package_count;
         ++installed_index) {
        struct package_state_package_view installed;
        struct package_manager_catalog_entry selected = {0};
        bool found = false;

        if (package_state_database_package(&session->installed,
                installed_index, &installed) != PACKAGE_STATE_STATUS_OK) {
            return PACKAGE_CONTROL_STATUS_STATE;
        }
        for (uint32_t repository_index = 0U;
             repository_index < session->repository.package_count;
             ++repository_index) {
            struct package_manager_catalog_entry candidate;

            report->manager_status = package_manager_repository_entry(
                &session->repository, repository_index, &candidate);
            if (report->manager_status != PACKAGE_MANAGER_STATUS_OK) {
                return PACKAGE_CONTROL_STATUS_MANAGER;
            }
            if (!repair_entry_matches(&installed, &candidate)) {
                continue;
            }
            if (found) {
                report->manager_status =
                    PACKAGE_MANAGER_STATUS_AMBIGUOUS_PROVIDER;
                return PACKAGE_CONTROL_STATUS_MANAGER;
            }
            selected = candidate;
            found = true;
        }
        if (!found) {
            report->manager_status = PACKAGE_MANAGER_STATUS_NOT_FOUND;
            return PACKAGE_CONTROL_STATUS_MANAGER;
        }
        session->plan.items[installed_index] =
            (struct package_manager_plan_item){
                selected.repository_index, selected.identifier,
                selected.version, selected.download_path,
                selected.package_bytes, selected.package_sha256,
                selected.publisher_key_id
            };
        if (selected.package_bytes == 0U || selected.package_bytes >
                PACKAGE_CONTROL_PAYLOAD_MAX_BYTES - session->payload_bytes) {
            return PACKAGE_CONTROL_STATUS_RANGE;
        }
        session->payload_bytes += (size_t)selected.package_bytes;
    }
    session->plan.count = session->installed.package_count;
    return PACKAGE_CONTROL_STATUS_OK;
}

enum package_control_status package_control_open_remove(
    uint64_t owner,
    const uint8_t *identifier,
    size_t identifier_bytes,
    struct package_control_report *report
)
{
    size_t index = PACKAGE_CONTROL_SESSION_LIMIT;
    struct control_session *session;

    clear_report(report);
    if (report == NULL || owner == 0U || identifier == NULL ||
        identifier_bytes == 0U || identifier_bytes >=
            PACKAGE_CONTROL_TEXT_BYTES) {
        return PACKAGE_CONTROL_STATUS_NULL_ARGUMENT;
    }
    if (servicing) {
        return finish(report, PACKAGE_CONTROL_STATUS_BUSY, NULL, 0U);
    }
    initialize_generations();
    for (size_t candidate = 0U; candidate < PACKAGE_CONTROL_SESSION_LIMIT;
            ++candidate) {
        if (!sessions[candidate].active) {
            index = candidate;
            break;
        }
    }
    if (index == PACKAGE_CONTROL_SESSION_LIMIT) {
        return finish(report, PACKAGE_CONTROL_STATUS_NO_SLOT, NULL, 0U);
    }
    servicing = true;
    session = &sessions[index];
    uint32_t generation = session->generation;
    zero_bytes(session, sizeof(*session));
    session->generation = generation;
    session->owner = owner;
    session->active = true;
    copy_bytes(session->target, identifier, identifier_bytes);
    enum package_control_status status = load_installed(session, report);

    if (status != PACKAGE_CONTROL_STATUS_OK) {
        goto refuse;
    }
    if (!session->has_installed) {
        report->manager_status = PACKAGE_MANAGER_STATUS_NOT_FOUND;
        status = PACKAGE_CONTROL_STATUS_MANAGER;
        goto refuse;
    }
    report->manager_status = package_manager_plan_remove(&session->installed,
        session->target, identifier_bytes, &session->plan);
    if (report->manager_status != PACKAGE_MANAGER_STATUS_OK) {
        status = PACKAGE_CONTROL_STATUS_MANAGER;
        goto refuse;
    }
    session->plan.target.bytes = session->target;
    if (session->plan.count == 0U ||
        session->plan.count > PACKAGE_CONTROL_PLAN_MAX_PACKAGES) {
        status = PACKAGE_CONTROL_STATUS_RANGE;
        goto refuse;
    }
    servicing = false;
    return finish(report, PACKAGE_CONTROL_STATUS_OK, session, index);

refuse:
    (void)release_session(session);
    servicing = false;
    return finish(report, status, NULL, 0U);
}

enum package_control_status package_control_open_install(
    uint64_t owner,
    package_upload_token repository_upload,
    const uint8_t *identifier,
    size_t identifier_bytes,
    struct package_control_report *report
)
{
    size_t index = PACKAGE_CONTROL_SESSION_LIMIT;
    struct control_session *session;
    struct package_service_report service_report;
    uint64_t repository_floor = 0U;
    int64_t now;

    clear_report(report);
    if (report == NULL || owner == 0U || identifier == NULL ||
        identifier_bytes == 0U || identifier_bytes >=
            PACKAGE_CONTROL_TEXT_BYTES) {
        return PACKAGE_CONTROL_STATUS_NULL_ARGUMENT;
    }
    if (servicing) {
        return finish(report, PACKAGE_CONTROL_STATUS_BUSY, NULL, 0U);
    }
    initialize_generations();
    for (size_t candidate = 0U; candidate < PACKAGE_CONTROL_SESSION_LIMIT;
            ++candidate) {
        if (!sessions[candidate].active) {
            index = candidate;
            break;
        }
    }
    if (index == PACKAGE_CONTROL_SESSION_LIMIT) {
        return finish(report, PACKAGE_CONTROL_STATUS_NO_SLOT, NULL, 0U);
    }
    servicing = true;
    session = &sessions[index];
    uint32_t generation = session->generation;
    zero_bytes(session, sizeof(*session));
    session->generation = generation;
    session->owner = owner;
    session->active = true;
    copy_bytes(session->target, identifier, identifier_bytes);
    enum package_control_status status = load_upload(owner, repository_upload,
        PACKAGE_CONTROL_REPOSITORY_MAX_BYTES, &session->repository_bytes,
        &session->repository_byte_count, report);

    if (status != PACKAGE_CONTROL_STATUS_OK) {
        goto refuse;
    }
    if (!package_platform_trust_manager(&session->trust)) {
        status = PACKAGE_CONTROL_STATUS_TRUST;
        goto refuse;
    }
    if (wall_clock_read_unix_seconds(&now) != WALL_CLOCK_STATUS_OK || now < 0) {
        status = PACKAGE_CONTROL_STATUS_CLOCK;
        goto refuse;
    }
    status = load_installed(session, report);
    if (status != PACKAGE_CONTROL_STATUS_OK) {
        goto refuse;
    }
    report->service_status = package_service_repository_floor_read(
        &repository_floor, &service_report);
    if (report->service_status != PACKAGE_SERVICE_STATUS_OK) {
        status = PACKAGE_CONTROL_STATUS_SERVICE;
        goto refuse;
    }
    session->policy = (struct package_manager_policy){
        (uint64_t)now, repository_floor, 1U, false
    };
    report->manager_status = package_manager_repository_open(
        session->repository_bytes, session->repository_byte_count,
        &session->policy, &session->trust, &session->repository);
    if (report->manager_status != PACKAGE_MANAGER_STATUS_OK) {
        status = PACKAGE_CONTROL_STATUS_MANAGER;
        goto refuse;
    }
    report->manager_status = package_manager_plan_install(&session->repository,
        session->has_installed ? &session->installed : NULL, session->target,
        identifier_bytes, &session->policy, &session->trust, &session->plan);
    if (report->manager_status != PACKAGE_MANAGER_STATUS_OK) {
        status = PACKAGE_CONTROL_STATUS_MANAGER;
        goto refuse;
    }
    session->plan.target.bytes = session->target;
    if (session->plan.count > PACKAGE_CONTROL_PLAN_MAX_PACKAGES) {
        status = PACKAGE_CONTROL_STATUS_RANGE;
        goto refuse;
    }
    for (uint32_t item = 0U; item < session->plan.count; ++item) {
        if (session->plan.items[item].package_bytes == 0U ||
            session->plan.items[item].package_bytes >
                PACKAGE_CONTROL_PAYLOAD_MAX_BYTES - session->payload_bytes) {
            status = PACKAGE_CONTROL_STATUS_RANGE;
            goto refuse;
        }
        session->payload_bytes +=
            (size_t)session->plan.items[item].package_bytes;
    }
    servicing = false;
    return finish(report, PACKAGE_CONTROL_STATUS_OK, session, index);

refuse:
    (void)release_session(session);
    servicing = false;
    return finish(report, status, NULL, 0U);
}

enum package_control_status package_control_open_repair(
    uint64_t owner,
    package_upload_token repository_upload,
    struct package_control_report *report
)
{
    size_t index = PACKAGE_CONTROL_SESSION_LIMIT;
    struct control_session *session;
    struct package_service_report service_report;
    uint64_t repository_floor = 0U;
    int64_t now;

    clear_report(report);
    if (report == NULL || owner == 0U) {
        return PACKAGE_CONTROL_STATUS_NULL_ARGUMENT;
    }
    if (servicing) {
        return finish(report, PACKAGE_CONTROL_STATUS_BUSY, NULL, 0U);
    }
    initialize_generations();
    for (size_t candidate = 0U; candidate < PACKAGE_CONTROL_SESSION_LIMIT;
            ++candidate) {
        if (!sessions[candidate].active) {
            index = candidate;
            break;
        }
    }
    if (index == PACKAGE_CONTROL_SESSION_LIMIT) {
        return finish(report, PACKAGE_CONTROL_STATUS_NO_SLOT, NULL, 0U);
    }
    servicing = true;
    session = &sessions[index];
    uint32_t generation = session->generation;
    zero_bytes(session, sizeof(*session));
    session->generation = generation;
    session->owner = owner;
    session->active = true;
    enum package_control_status status = load_upload(owner, repository_upload,
        PACKAGE_CONTROL_REPOSITORY_MAX_BYTES, &session->repository_bytes,
        &session->repository_byte_count, report);

    if (status != PACKAGE_CONTROL_STATUS_OK) {
        goto refuse;
    }
    if (!package_platform_trust_manager(&session->trust)) {
        status = PACKAGE_CONTROL_STATUS_TRUST;
        goto refuse;
    }
    if (wall_clock_read_unix_seconds(&now) != WALL_CLOCK_STATUS_OK || now < 0) {
        status = PACKAGE_CONTROL_STATUS_CLOCK;
        goto refuse;
    }
    status = load_installed_snapshot(session, report, true);
    if (status != PACKAGE_CONTROL_STATUS_OK) {
        goto refuse;
    }
    report->service_status = package_service_repository_floor_read(
        &repository_floor, &service_report);
    if (report->service_status != PACKAGE_SERVICE_STATUS_OK) {
        status = PACKAGE_CONTROL_STATUS_SERVICE;
        goto refuse;
    }
    session->policy = (struct package_manager_policy){
        (uint64_t)now, repository_floor, 1U, false
    };
    report->manager_status = package_manager_repository_open(
        session->repository_bytes, session->repository_byte_count,
        &session->policy, &session->trust, &session->repository);
    if (report->manager_status != PACKAGE_MANAGER_STATUS_OK) {
        status = PACKAGE_CONTROL_STATUS_MANAGER;
        goto refuse;
    }
    status = plan_repair(session, report);
    if (status != PACKAGE_CONTROL_STATUS_OK) {
        goto refuse;
    }
    servicing = false;
    return finish(report, PACKAGE_CONTROL_STATUS_OK, session, index);

refuse:
    (void)release_session(session);
    servicing = false;
    return finish(report, status, NULL, 0U);
}

static bool copy_text(char *destination, size_t capacity,
    const struct package_manager_text *source, uint32_t *length)
{
    if (source == NULL || length == NULL || source->bytes == NULL ||
        source->length == 0U || source->length >= capacity ||
        source->length > UINT32_MAX) {
        return false;
    }
    copy_bytes((uint8_t *)destination, source->bytes, source->length);
    destination[source->length] = '\0';
    *length = (uint32_t)source->length;
    return true;
}

enum package_control_status package_control_item(
    uint64_t owner,
    package_control_token token,
    uint32_t index,
    struct package_control_item *item,
    struct package_control_report *report
)
{
    struct control_session *session;
    size_t session_index;

    clear_report(report);
    if (report == NULL || item == NULL) {
        return PACKAGE_CONTROL_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(item, sizeof(*item));
    enum package_control_status status = resolve_session(owner, token,
        &session, &session_index);
    if (status != PACKAGE_CONTROL_STATUS_OK) {
        return finish(report, status, NULL, 0U);
    }
    if (servicing) {
        return finish(report, PACKAGE_CONTROL_STATUS_BUSY, session,
            session_index);
    }
    if (index >= session->plan.count) {
        return finish(report, PACKAGE_CONTROL_STATUS_RANGE, session,
            session_index);
    }
    const struct package_manager_plan_item *source = &session->plan.items[index];
    item->index = index;
    item->package_bytes = source->package_bytes;
    copy_bytes(item->package_sha256, source->package_sha256,
        sizeof(item->package_sha256));
    if (!copy_text(item->identifier, sizeof(item->identifier),
            &source->identifier, &item->identifier_bytes) ||
        !copy_text(item->version, sizeof(item->version), &source->version,
            &item->version_bytes) ||
        !copy_text(item->download_path, sizeof(item->download_path),
            &source->download_path, &item->path_bytes)) {
        zero_bytes(item, sizeof(*item));
        return finish(report, PACKAGE_CONTROL_STATUS_STATE, session,
            session_index);
    }
    return finish(report, PACKAGE_CONTROL_STATUS_OK, session, session_index);
}

enum package_control_status package_control_attach(
    uint64_t owner,
    package_control_token token,
    uint32_t index,
    package_upload_token package_upload,
    struct package_control_report *report
)
{
    struct control_session *session;
    size_t session_index;

    clear_report(report);
    if (report == NULL) {
        return PACKAGE_CONTROL_STATUS_NULL_ARGUMENT;
    }
    enum package_control_status status = resolve_session(owner, token,
        &session, &session_index);
    if (status != PACKAGE_CONTROL_STATUS_OK) {
        return finish(report, status, NULL, 0U);
    }
    if (servicing) {
        return finish(report, PACKAGE_CONTROL_STATUS_BUSY, session,
            session_index);
    }
    if (session->prepared || session->committed || index >= session->plan.count ||
        session->attached[index]) {
        return finish(report, PACKAGE_CONTROL_STATUS_STATE, session,
            session_index);
    }
    servicing = true;
    struct package_upload_report upload;
    report->upload_status = package_upload_inspect(owner, package_upload,
        &upload);
    const struct package_manager_plan_item *expected =
        &session->plan.items[index];
    if (report->upload_status != PACKAGE_UPLOAD_STATUS_OK) {
        servicing = false;
        return finish(report, PACKAGE_CONTROL_STATUS_UPLOAD, session,
            session_index);
    }
    if (upload.byte_count != expected->package_bytes) {
        report->upload_status = PACKAGE_UPLOAD_STATUS_LENGTH;
        servicing = false;
        return finish(report, PACKAGE_CONTROL_STATUS_UPLOAD, session,
            session_index);
    }
    if (!equal_bytes(upload.sha256, expected->package_sha256,
            sizeof(upload.sha256))) {
        report->upload_status = PACKAGE_UPLOAD_STATUS_DIGEST;
        servicing = false;
        return finish(report, PACKAGE_CONTROL_STATUS_UPLOAD, session,
            session_index);
    }
    size_t loaded = 0U;
    status = load_upload(owner, package_upload,
        (size_t)expected->package_bytes, (uint8_t **)&session->packages[index].bytes,
        &loaded, report);
    if (status != PACKAGE_CONTROL_STATUS_OK) {
        servicing = false;
        return finish(report, status, session, session_index);
    }
    session->packages[index].byte_count = loaded;
    struct package_manager_catalog_entry entry;
    struct package_manager_package_view admitted;
    report->manager_status = package_manager_repository_entry(
        &session->repository, expected->source_index, &entry);
    if (report->manager_status == PACKAGE_MANAGER_STATUS_OK) {
        report->manager_status = package_manager_package_open(
            session->packages[index].bytes,
            session->packages[index].byte_count, &entry, &session->policy,
            &session->trust, &admitted);
    }
    if (report->manager_status != PACKAGE_MANAGER_STATUS_OK) {
        if (!release_buffer((uint8_t **)&session->packages[index].bytes)) {
            servicing = false;
            return finish(report, PACKAGE_CONTROL_STATUS_RESOURCE, session,
                session_index);
        }
        session->packages[index].byte_count = 0U;
        servicing = false;
        return finish(report, PACKAGE_CONTROL_STATUS_MANAGER, session,
            session_index);
    }
    session->attached[index] = true;
    ++session->attached_count;
    servicing = false;
    return finish(report, PACKAGE_CONTROL_STATUS_OK, session, session_index);
}

static enum package_control_status build_repair_workspace(
    struct control_session *session,
    struct package_builder_workspace *workspace,
    struct package_control_report *report
)
{
    struct package_builder_repair_file *replacements = NULL;
    enum package_control_status status = PACKAGE_CONTROL_STATUS_OK;

    if (session->installed.file_count != 0U &&
        heap_allocate((uint64_t)session->installed.file_count *
                sizeof(*replacements), (void **)&replacements) !=
            HEAP_STATUS_OK) {
        return PACKAGE_CONTROL_STATUS_RESOURCE;
    }
    for (uint32_t file_index = 0U;
         status == PACKAGE_CONTROL_STATUS_OK &&
            file_index < session->installed.file_count; ++file_index) {
        struct package_state_file_view installed_file;
        struct package_manager_catalog_entry entry;
        struct package_manager_package_view package;
        struct package_manager_file_view selected;
        bool found = false;

        if (package_state_database_file(&session->installed, file_index,
                &installed_file) != PACKAGE_STATE_STATUS_OK ||
            installed_file.owner_index >= session->plan.count ||
            !session->attached[installed_file.owner_index]) {
            status = PACKAGE_CONTROL_STATUS_STATE;
            break;
        }
        const uint32_t owner = installed_file.owner_index;
        report->manager_status = package_manager_repository_entry(
            &session->repository, session->plan.items[owner].source_index,
            &entry);
        if (report->manager_status == PACKAGE_MANAGER_STATUS_OK) {
            report->manager_status = package_manager_package_open(
                session->packages[owner].bytes,
                session->packages[owner].byte_count, &entry,
                &session->policy, &session->trust, &package);
        }
        if (report->manager_status != PACKAGE_MANAGER_STATUS_OK) {
            status = PACKAGE_CONTROL_STATUS_MANAGER;
            break;
        }
        for (uint32_t package_file = 0U;
             package_file < package.file_count; ++package_file) {
            struct package_manager_file_view candidate;

            report->manager_status = package_manager_package_file(&package,
                package_file, &candidate);
            if (report->manager_status != PACKAGE_MANAGER_STATUS_OK) {
                status = PACKAGE_CONTROL_STATUS_MANAGER;
                break;
            }
            if (!repair_text_equal(&installed_file.path, &candidate.path)) {
                continue;
            }
            if (found || candidate.payload_bytes != installed_file.length ||
                !equal_bytes(candidate.sha256, installed_file.sha256,
                    PACKAGE_MANAGER_SHA256_BYTES)) {
                report->manager_status = found ?
                    PACKAGE_MANAGER_STATUS_CONFLICT :
                    PACKAGE_MANAGER_STATUS_DIGEST;
                status = PACKAGE_CONTROL_STATUS_MANAGER;
                break;
            }
            selected = candidate;
            found = true;
        }
        if (status != PACKAGE_CONTROL_STATUS_OK) {
            break;
        }
        if (!found) {
            report->manager_status = PACKAGE_MANAGER_STATUS_NOT_FOUND;
            status = PACKAGE_CONTROL_STATUS_MANAGER;
            break;
        }
        replacements[file_index] = (struct package_builder_repair_file){
            installed_file.path, selected.payload, selected.payload_bytes
        };
    }
    if (status == PACKAGE_CONTROL_STATUS_OK) {
        report->manager_status = package_builder_repair(&session->installed,
            replacements, session->installed.file_count, workspace);
        if (report->manager_status != PACKAGE_MANAGER_STATUS_OK) {
            status = PACKAGE_CONTROL_STATUS_MANAGER;
        }
    }
    if (replacements != NULL && heap_free(replacements) != HEAP_STATUS_OK) {
        status = PACKAGE_CONTROL_STATUS_RESOURCE;
    }
    return status;
}

enum package_control_status package_control_commit(
    uint64_t owner,
    package_control_token token,
    struct package_control_report *report
)
{
    struct control_session *session;
    size_t session_index;
    struct package_service_report service_report;

    clear_report(report);
    if (report == NULL) {
        return PACKAGE_CONTROL_STATUS_NULL_ARGUMENT;
    }
    enum package_control_status status = resolve_session(owner, token,
        &session, &session_index);
    if (status != PACKAGE_CONTROL_STATUS_OK) {
        return finish(report, status, NULL, 0U);
    }
    if (servicing) {
        return finish(report, PACKAGE_CONTROL_STATUS_BUSY, session,
            session_index);
    }
    if (session->committed) {
        return finish(report, PACKAGE_CONTROL_STATUS_STATE, session,
            session_index);
    }
    if (session->prepared) {
        servicing = true;
        report->service_status = package_service_commit(&service_report);
        if (report->service_status == PACKAGE_SERVICE_STATUS_OK) {
            session->committed = true;
            session->result_generation = service_report.generation;
        }
        servicing = false;
        return finish(report, report->service_status == PACKAGE_SERVICE_STATUS_OK ?
            PACKAGE_CONTROL_STATUS_OK : PACKAGE_CONTROL_STATUS_SERVICE,
            session, session_index);
    }
    if (session->plan.operation != PACKAGE_MANAGER_PLAN_REMOVE &&
        session->attached_count != session->plan.count) {
        return finish(report, PACKAGE_CONTROL_STATUS_STATE, session,
            session_index);
    }
    servicing = true;
    struct package_builder_workspace *workspace = NULL;
    uint8_t *database = NULL;
    size_t database_bytes = 0U;

    if (session->plan.operation != PACKAGE_MANAGER_PLAN_REMOVE) {
        report->service_status = package_service_repository_floor_advance(
            session->repository.repository_version, &service_report);
        if (report->service_status != PACKAGE_SERVICE_STATUS_OK) {
            status = PACKAGE_CONTROL_STATUS_SERVICE;
            goto release;
        }
    }
    if (heap_allocate(sizeof(*workspace), (void **)&workspace) !=
            HEAP_STATUS_OK) {
        status = PACKAGE_CONTROL_STATUS_RESOURCE;
        goto release;
    }
    if (session->plan.operation == PACKAGE_MANAGER_PLAN_REPAIR) {
        status = build_repair_workspace(session, workspace, report);
        if (status != PACKAGE_CONTROL_STATUS_OK) {
            goto release;
        }
    } else {
        report->manager_status = package_builder_build(
            session->plan.operation == PACKAGE_MANAGER_PLAN_REMOVE ? NULL :
                &session->repository,
            session->has_installed ? &session->installed : NULL,
            &session->plan,
            session->plan.operation == PACKAGE_MANAGER_PLAN_REMOVE ? NULL :
                session->packages,
            session->plan.operation == PACKAGE_MANAGER_PLAN_REMOVE ? 0U :
                session->plan.count,
            session->plan.operation == PACKAGE_MANAGER_PLAN_REMOVE ? NULL :
                &session->policy,
            session->plan.operation == PACKAGE_MANAGER_PLAN_REMOVE ? NULL :
                &session->trust,
            workspace);
        if (report->manager_status != PACKAGE_MANAGER_STATUS_OK) {
            status = PACKAGE_CONTROL_STATUS_MANAGER;
            goto release;
        }
    }
    enum package_state_status state_status = package_generation_size(
        &workspace->spec, &database_bytes);
    if (state_status != PACKAGE_STATE_STATUS_OK || database_bytes == 0U ||
        database_bytes > PACKAGE_CONTROL_DATABASE_MAX_BYTES) {
        status = PACKAGE_CONTROL_STATUS_RANGE;
        goto release;
    }
    if (allocate_buffer(database_bytes, &database) !=
            PACKAGE_CONTROL_STATUS_OK) {
        status = PACKAGE_CONTROL_STATUS_RESOURCE;
        goto release;
    }
    struct package_state_database_view encoded;
    state_status = package_generation_encode(&workspace->spec, database,
        database_bytes, &encoded);
    if (state_status != PACKAGE_STATE_STATUS_OK) {
        status = PACKAGE_CONTROL_STATUS_STATE;
        goto release;
    }
    const struct package_service_prepare_request request = {
        workspace, database, database_bytes
    };
    if (!session->has_installed) {
        report->service_status = package_service_bootstrap(&request,
            &service_report);
        if (report->service_status == PACKAGE_SERVICE_STATUS_OK) {
            session->committed = true;
            session->result_generation = service_report.generation;
        }
    } else {
        report->service_status = package_service_prepare(&request,
            &service_report);
        if (report->service_status == PACKAGE_SERVICE_STATUS_OK) {
            session->prepared = true;
            session->result_generation = service_report.generation;
            report->service_status = package_service_commit(&service_report);
            if (report->service_status == PACKAGE_SERVICE_STATUS_OK) {
                session->committed = true;
                session->result_generation = service_report.generation;
            }
        }
    }
    status = report->service_status == PACKAGE_SERVICE_STATUS_OK ?
        PACKAGE_CONTROL_STATUS_OK : PACKAGE_CONTROL_STATUS_SERVICE;

release:
    if (database != NULL && !release_buffer(&database)) {
        status = PACKAGE_CONTROL_STATUS_RESOURCE;
    }
    if (workspace != NULL && heap_free(workspace) != HEAP_STATUS_OK) {
        status = PACKAGE_CONTROL_STATUS_RESOURCE;
    }
    servicing = false;
    return finish(report, status, session, session_index);
}

enum package_control_status package_control_close(
    uint64_t owner,
    package_control_token token,
    struct package_control_report *report
)
{
    struct control_session *session;
    size_t session_index;

    clear_report(report);
    if (report == NULL) {
        return PACKAGE_CONTROL_STATUS_NULL_ARGUMENT;
    }
    enum package_control_status status = resolve_session(owner, token,
        &session, &session_index);
    if (status != PACKAGE_CONTROL_STATUS_OK) {
        return finish(report, status, NULL, 0U);
    }
    if (servicing) {
        return finish(report, PACKAGE_CONTROL_STATUS_BUSY, session,
            session_index);
    }
    servicing = true;
    status = release_session(session);
    servicing = false;
    return finish(report, status, NULL, 0U);
}

bool package_control_resources_released(void)
{
    if (servicing) {
        return false;
    }
    for (size_t index = 0U; index < PACKAGE_CONTROL_SESSION_LIMIT; ++index) {
        if (sessions[index].active) {
            return false;
        }
    }
    return true;
}

const char *package_control_status_string(enum package_control_status status)
{
    static const char *const names[PACKAGE_CONTROL_STATUS_COUNT] = {
        [PACKAGE_CONTROL_STATUS_OK] = "ok",
        [PACKAGE_CONTROL_STATUS_NULL_ARGUMENT] = "null argument",
        [PACKAGE_CONTROL_STATUS_BUSY] = "busy",
        [PACKAGE_CONTROL_STATUS_NO_SLOT] = "no slot",
        [PACKAGE_CONTROL_STATUS_STALE] = "stale",
        [PACKAGE_CONTROL_STATUS_STATE] = "state",
        [PACKAGE_CONTROL_STATUS_RANGE] = "range",
        [PACKAGE_CONTROL_STATUS_RESOURCE] = "resource",
        [PACKAGE_CONTROL_STATUS_CLOCK] = "clock",
        [PACKAGE_CONTROL_STATUS_TRUST] = "trust",
        [PACKAGE_CONTROL_STATUS_UPLOAD] = "upload",
        [PACKAGE_CONTROL_STATUS_MANAGER] = "manager",
        [PACKAGE_CONTROL_STATUS_SERVICE] = "service"
    };

    return status < PACKAGE_CONTROL_STATUS_COUNT && names[status] != NULL ?
        names[status] : "unknown";
}
