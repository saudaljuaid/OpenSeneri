/* SPDX-License-Identifier: GPL-3.0-only */
/* Real signed repository/package coverage for the privileged controller. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <phipia/heap.h>
#include <phipia/package_control.h>
#include <phipia/package_generation.h>
#include <phipia/package_platform_trust.h>
#include <phipia/package_state.h>
#include <phipia/package_trust.h>
#include <phipia/wall_clock.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        (void)fprintf(stderr, \
            "package-control host check failed at line %d: %s\n", \
            __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define FIXTURE_NOW INT64_C(1800000060)
#define FIXTURE_REPOSITORY_VERSION UINT64_C(42)
#define FIXTURE_UPDATE_REPOSITORY_VERSION UINT64_C(43)
#define TEST_OWNER UINT64_C(0x123400000001)
#define OTHER_OWNER UINT64_C(0x123400000002)
#define UPLOAD_FIXTURE_LIMIT 16U
#define HEAP_FIXTURE_LIMIT 32U

struct file_bytes {
    uint8_t *bytes;
    size_t count;
};

struct upload_fixture {
    const uint8_t *bytes;
    size_t count;
    uint8_t sha256[PACKAGE_STATE_SHA256_BYTES];
    bool active;
};

static struct upload_fixture uploads[UPLOAD_FIXTURE_LIMIT];
static void *heap_pointers[HEAP_FIXTURE_LIMIT];
static size_t live_allocations;
static struct package_trust_key trust_keys[2];
static struct package_trust_store trust_store;
static struct package_manager_trust platform_trust;
static uint8_t service_current[PACKAGE_CONTROL_DATABASE_MAX_BYTES];
static uint8_t service_target[PACKAGE_CONTROL_DATABASE_MAX_BYTES];
static size_t service_current_bytes;
static size_t service_target_bytes;
static bool service_prepared;
static bool fail_commit_once;
static bool fail_floor_once;
static uint64_t service_repository_floor;

static struct file_bytes read_file(const char *path, size_t maximum)
{
    struct file_bytes result = { NULL, 0U };
    FILE *file = fopen(path, "rb");
    long length;

    if (file == NULL || fseek(file, 0L, SEEK_END) != 0) {
        if (file != NULL) {
            (void)fclose(file);
        }
        return result;
    }
    length = ftell(file);
    if (length <= 0L || (uint64_t)length > maximum ||
        fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        return result;
    }
    result.bytes = malloc((size_t)length);
    if (result.bytes == NULL ||
        fread(result.bytes, 1U, (size_t)length, file) != (size_t)length ||
        fclose(file) != 0) {
        free(result.bytes);
        result.bytes = NULL;
        return result;
    }
    result.count = (size_t)length;
    return result;
}

static package_upload_token register_upload(const struct file_bytes *file)
{
    for (size_t index = 0U; index < UPLOAD_FIXTURE_LIMIT; ++index) {
        if (!uploads[index].active) {
            uploads[index].bytes = file->bytes;
            uploads[index].count = file->count;
            if (package_state_sha256(file->bytes, file->count,
                    uploads[index].sha256) != PACKAGE_STATE_STATUS_OK) {
                return 0U;
            }
            uploads[index].active = true;
            return (package_upload_token)(index + 1U);
        }
    }
    return 0U;
}

static struct upload_fixture *resolve_upload(uint64_t owner,
    package_upload_token token)
{
    if (owner != TEST_OWNER || token == 0U ||
        token > UPLOAD_FIXTURE_LIMIT || !uploads[token - 1U].active) {
        return NULL;
    }
    return &uploads[token - 1U];
}

enum package_upload_status package_upload_inspect(
    uint64_t owner,
    package_upload_token token,
    struct package_upload_report *report
)
{
    if (report == NULL) {
        return PACKAGE_UPLOAD_STATUS_NULL_ARGUMENT;
    }
    (void)memset(report, 0, sizeof(*report));
    struct upload_fixture *upload = resolve_upload(owner, token);
    if (upload == NULL) {
        report->status = PACKAGE_UPLOAD_STATUS_STALE;
        return report->status;
    }
    report->status = PACKAGE_UPLOAD_STATUS_OK;
    report->token = token;
    report->byte_count = upload->count;
    (void)memcpy(report->sha256, upload->sha256, sizeof(report->sha256));
    report->sealed = true;
    report->durable = true;
    return report->status;
}

enum package_upload_status package_upload_read(
    uint64_t owner,
    package_upload_token token,
    uint64_t offset,
    uint8_t *bytes,
    size_t capacity,
    size_t *read_bytes,
    struct package_upload_report *report
)
{
    if (bytes == NULL || read_bytes == NULL || report == NULL) {
        return PACKAGE_UPLOAD_STATUS_NULL_ARGUMENT;
    }
    *read_bytes = 0U;
    struct upload_fixture *upload = resolve_upload(owner, token);
    if (upload == NULL || offset > upload->count) {
        (void)memset(report, 0, sizeof(*report));
        report->status = PACKAGE_UPLOAD_STATUS_STALE;
        return report->status;
    }
    size_t remaining = upload->count - (size_t)offset;
    size_t count = remaining < capacity ? remaining : capacity;
    (void)memcpy(bytes, upload->bytes + (size_t)offset, count);
    *read_bytes = count;
    return package_upload_inspect(owner, token, report);
}

enum heap_status heap_allocate(uint64_t size, void **pointer)
{
    if (pointer == NULL) {
        return HEAP_STATUS_NULL_ARGUMENT;
    }
    *pointer = NULL;
    if (size == 0U || size > SIZE_MAX ||
        live_allocations == HEAP_FIXTURE_LIMIT) {
        return HEAP_STATUS_OUT_OF_MEMORY;
    }
    void *allocation = malloc((size_t)size);
    if (allocation == NULL) {
        return HEAP_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0U; index < HEAP_FIXTURE_LIMIT; ++index) {
        if (heap_pointers[index] == NULL) {
            heap_pointers[index] = allocation;
            ++live_allocations;
            *pointer = allocation;
            return HEAP_STATUS_OK;
        }
    }
    free(allocation);
    return HEAP_STATUS_OUT_OF_BLOCKS;
}

enum heap_status heap_free(void *pointer)
{
    if (pointer == NULL) {
        return HEAP_STATUS_NULL_ARGUMENT;
    }
    for (size_t index = 0U; index < HEAP_FIXTURE_LIMIT; ++index) {
        if (heap_pointers[index] == pointer) {
            heap_pointers[index] = NULL;
            --live_allocations;
            free(pointer);
            return HEAP_STATUS_OK;
        }
    }
    return HEAP_STATUS_BAD_POINTER;
}

bool package_platform_trust_manager(struct package_manager_trust *result)
{
    if (result == NULL || platform_trust.lookup == NULL) {
        return false;
    }
    *result = platform_trust;
    return true;
}

enum wall_clock_status wall_clock_read_unix_seconds(int64_t *seconds)
{
    if (seconds == NULL) {
        return WALL_CLOCK_STATUS_NULL_ARGUMENT;
    }
    *seconds = FIXTURE_NOW;
    return WALL_CLOCK_STATUS_OK;
}

static void service_report_clear(struct package_service_report *report,
    enum package_service_status status)
{
    (void)memset(report, 0, sizeof(*report));
    report->status = status;
    report->state_status = PACKAGE_STATE_STATUS_OK;
}

enum package_service_status package_service_repository_floor_read(
    uint64_t *repository_floor,
    struct package_service_report *report
)
{
    if (repository_floor == NULL || report == NULL) {
        return PACKAGE_SERVICE_STATUS_NULL_ARGUMENT;
    }
    *repository_floor = service_repository_floor;
    service_report_clear(report, PACKAGE_SERVICE_STATUS_OK);
    report->repository_floor = service_repository_floor;
    return report->status;
}

enum package_service_status package_service_repository_floor_advance(
    uint64_t repository_version,
    struct package_service_report *report
)
{
    if (repository_version == 0U || report == NULL) {
        return PACKAGE_SERVICE_STATUS_NULL_ARGUMENT;
    }
    if (repository_version < service_repository_floor) {
        service_report_clear(report, PACKAGE_SERVICE_STATUS_STATE);
        report->repository_floor = service_repository_floor;
        return report->status;
    }
    service_repository_floor = repository_version;
    if (fail_floor_once) {
        fail_floor_once = false;
        service_report_clear(report, PACKAGE_SERVICE_STATUS_DURABILITY);
        report->repository_floor = service_repository_floor;
        return report->status;
    }
    service_report_clear(report, PACKAGE_SERVICE_STATUS_OK);
    report->repository_floor = service_repository_floor;
    return report->status;
}

enum package_service_status package_service_snapshot(
    uint8_t *database,
    size_t capacity,
    size_t *output_bytes,
    struct package_service_report *report
)
{
    if (database == NULL || output_bytes == NULL || report == NULL) {
        return PACKAGE_SERVICE_STATUS_NULL_ARGUMENT;
    }
    *output_bytes = 0U;
    if (service_current_bytes == 0U) {
        service_report_clear(report, PACKAGE_SERVICE_STATUS_ABSENT);
        return report->status;
    }
    if (capacity < service_current_bytes) {
        service_report_clear(report, PACKAGE_SERVICE_STATUS_RESOURCE);
        return report->status;
    }
    (void)memcpy(database, service_current, service_current_bytes);
    *output_bytes = service_current_bytes;
    struct package_state_database_view view;
    if (package_state_database_parse(database, *output_bytes, &view) !=
            PACKAGE_STATE_STATUS_OK) {
        service_report_clear(report, PACKAGE_SERVICE_STATUS_STATE);
        return report->status;
    }
    service_report_clear(report, PACKAGE_SERVICE_STATUS_OK);
    report->generation = view.generation;
    return report->status;
}

enum package_service_status package_service_repair_snapshot(
    uint8_t *database,
    size_t capacity,
    size_t *output_bytes,
    struct package_service_report *report
)
{
    return package_service_snapshot(database, capacity, output_bytes, report);
}

static enum package_service_status accept_service_database(
    const struct package_service_prepare_request *request,
    uint64_t expected_generation,
    struct package_service_report *report
)
{
    if (request == NULL || request->builder == NULL ||
        request->database == NULL || report == NULL ||
        request->database_bytes == 0U ||
        request->database_bytes > sizeof(service_target)) {
        return PACKAGE_SERVICE_STATUS_NULL_ARGUMENT;
    }
    struct package_state_database_view view;
    if (package_state_database_parse(request->database,
            request->database_bytes, &view) != PACKAGE_STATE_STATUS_OK ||
        view.generation != expected_generation ||
        request->builder->spec.generation != expected_generation) {
        service_report_clear(report, PACKAGE_SERVICE_STATUS_STATE);
        return report->status;
    }
    (void)memcpy(service_target, request->database, request->database_bytes);
    service_target_bytes = request->database_bytes;
    service_report_clear(report, PACKAGE_SERVICE_STATUS_OK);
    report->generation = expected_generation;
    report->prepared = true;
    return report->status;
}

enum package_service_status package_service_bootstrap(
    const struct package_service_prepare_request *request,
    struct package_service_report *report
)
{
    if (service_current_bytes != 0U || service_prepared) {
        service_report_clear(report, PACKAGE_SERVICE_STATUS_STATE);
        return report->status;
    }
    enum package_service_status status = accept_service_database(request, 1U,
        report);
    if (status == PACKAGE_SERVICE_STATUS_OK) {
        (void)memcpy(service_current, service_target, service_target_bytes);
        service_current_bytes = service_target_bytes;
        service_target_bytes = 0U;
        report->committed = true;
    }
    return status;
}

enum package_service_status package_service_prepare(
    const struct package_service_prepare_request *request,
    struct package_service_report *report
)
{
    struct package_state_database_view current;
    if (service_current_bytes == 0U || service_prepared ||
        package_state_database_parse(service_current, service_current_bytes,
            &current) != PACKAGE_STATE_STATUS_OK) {
        service_report_clear(report, PACKAGE_SERVICE_STATUS_STATE);
        return report->status;
    }
    enum package_service_status status = accept_service_database(request,
        current.generation + 1U, report);
    if (status == PACKAGE_SERVICE_STATUS_OK) {
        service_prepared = true;
    }
    return status;
}

enum package_service_status package_service_commit(
    struct package_service_report *report
)
{
    if (report == NULL) {
        return PACKAGE_SERVICE_STATUS_NULL_ARGUMENT;
    }
    if (!service_prepared || service_target_bytes == 0U) {
        service_report_clear(report, PACKAGE_SERVICE_STATUS_INCOMPLETE);
        return report->status;
    }
    struct package_state_database_view target;
    if (package_state_database_parse(service_target, service_target_bytes,
            &target) != PACKAGE_STATE_STATUS_OK) {
        service_report_clear(report, PACKAGE_SERVICE_STATUS_STATE);
        return report->status;
    }
    if (fail_commit_once) {
        fail_commit_once = false;
        service_report_clear(report, PACKAGE_SERVICE_STATUS_DURABILITY);
        report->generation = target.generation;
        report->prepared = true;
        return report->status;
    }
    (void)memcpy(service_current, service_target, service_target_bytes);
    service_current_bytes = service_target_bytes;
    service_target_bytes = 0U;
    service_prepared = false;
    service_report_clear(report, PACKAGE_SERVICE_STATUS_OK);
    report->generation = target.generation;
    report->prepared = true;
    report->committed = true;
    return report->status;
}

static bool initialize_trust(const struct file_bytes *root,
    const struct file_bytes *publisher)
{
    if (root->count != PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES ||
        publisher->count != PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES) {
        return false;
    }
    (void)memcpy(trust_keys[0].public_key, root->bytes,
        sizeof(trust_keys[0].public_key));
    (void)memcpy(trust_keys[1].public_key, publisher->bytes,
        sizeof(trust_keys[1].public_key));
    for (size_t index = 0U; index < 2U; ++index) {
        trust_keys[index].status = PACKAGE_MANAGER_KEY_TRUSTED;
        if (package_state_sha256(trust_keys[index].public_key,
                sizeof(trust_keys[index].public_key),
                trust_keys[index].key_id) != PACKAGE_STATE_STATUS_OK) {
            return false;
        }
    }
    if (memcmp(trust_keys[0].key_id, trust_keys[1].key_id,
            sizeof(trust_keys[0].key_id)) > 0) {
        struct package_trust_key temporary = trust_keys[0];
        trust_keys[0] = trust_keys[1];
        trust_keys[1] = temporary;
    }
    if (package_trust_open(trust_keys, 2U, &trust_store) !=
            PACKAGE_TRUST_STATUS_OK) {
        return false;
    }
    package_trust_manager(&trust_store, &platform_trust);
    return true;
}

static int find_plan_item(package_control_token token, const char *identifier,
    uint32_t *result)
{
    struct package_control_report report;
    struct package_control_item item;

    for (uint32_t index = 0U; index < PACKAGE_CONTROL_PLAN_MAX_PACKAGES;
            ++index) {
        enum package_control_status status = package_control_item(TEST_OWNER,
            token, index, &item, &report);
        if (status == PACKAGE_CONTROL_STATUS_RANGE) {
            break;
        }
        if (status != PACKAGE_CONTROL_STATUS_OK) {
            return 1;
        }
        if (item.identifier_bytes == strlen(identifier) &&
            memcmp(item.identifier, identifier, item.identifier_bytes) == 0) {
            *result = index;
            return 0;
        }
    }
    return 1;
}

static int attach_named(package_control_token control, const char *identifier,
    package_upload_token upload)
{
    uint32_t index;
    struct package_control_report report;

    CHECK(find_plan_item(control, identifier, &index) == 0);
    CHECK(package_control_attach(TEST_OWNER, control, index, upload, &report) ==
        PACKAGE_CONTROL_STATUS_OK);
    return 0;
}

int main(int argc, char **argv)
{
    struct file_bytes repository;
    struct file_bytes root;
    struct file_bytes publisher;
    struct file_bytes application;
    struct file_bytes library;
    struct file_bytes update_repository;
    struct file_bytes update_application;
    struct file_bytes update_library;
    struct file_bytes changed_repository;
    package_upload_token repository_upload;
    package_upload_token application_upload;
    package_upload_token library_upload;
    package_upload_token update_repository_upload;
    package_upload_token update_application_upload;
    package_upload_token update_library_upload;
    package_upload_token changed_repository_upload;
    struct package_control_report report;
    struct package_control_item item;
    package_control_token control;
    uint32_t library_index;
    struct package_state_database_view installed;

    CHECK(argc == 9);
    repository = read_file(argv[1], PACKAGE_CONTROL_REPOSITORY_MAX_BYTES);
    root = read_file(argv[2], PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES);
    publisher = read_file(argv[3], PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES);
    application = read_file(argv[4], PACKAGE_CONTROL_PAYLOAD_MAX_BYTES);
    library = read_file(argv[5], PACKAGE_CONTROL_PAYLOAD_MAX_BYTES);
    update_repository = read_file(argv[6],
        PACKAGE_CONTROL_REPOSITORY_MAX_BYTES);
    update_application = read_file(argv[7],
        PACKAGE_CONTROL_PAYLOAD_MAX_BYTES);
    update_library = read_file(argv[8], PACKAGE_CONTROL_PAYLOAD_MAX_BYTES);
    CHECK(repository.bytes != NULL && root.bytes != NULL &&
        publisher.bytes != NULL && application.bytes != NULL &&
        library.bytes != NULL && update_repository.bytes != NULL &&
        update_application.bytes != NULL && update_library.bytes != NULL &&
        initialize_trust(&root, &publisher));
    repository_upload = register_upload(&repository);
    application_upload = register_upload(&application);
    library_upload = register_upload(&library);
    update_repository_upload = register_upload(&update_repository);
    update_application_upload = register_upload(&update_application);
    update_library_upload = register_upload(&update_library);
    CHECK(repository_upload != 0U && application_upload != 0U &&
        library_upload != 0U && update_repository_upload != 0U &&
        update_application_upload != 0U && update_library_upload != 0U &&
        package_control_resources_released());

    changed_repository.count = repository.count;
    changed_repository.bytes = malloc(changed_repository.count);
    CHECK(changed_repository.bytes != NULL);
    (void)memcpy(changed_repository.bytes, repository.bytes,
        changed_repository.count);
    changed_repository.bytes[changed_repository.count - 1U] ^= UINT8_C(1);
    changed_repository_upload = register_upload(&changed_repository);
    CHECK(changed_repository_upload != 0U &&
        package_control_open_install(TEST_OWNER, changed_repository_upload,
            (const uint8_t *)"org.phipia.app", 14U, &report) ==
                PACKAGE_CONTROL_STATUS_MANAGER &&
        report.manager_status == PACKAGE_MANAGER_STATUS_DIGEST &&
        package_control_resources_released() && live_allocations == 0U);

    CHECK(package_control_open_install(TEST_OWNER, repository_upload,
        (const uint8_t *)"org.phipia.app", 14U, &report) ==
            PACKAGE_CONTROL_STATUS_OK &&
        report.repository_version == FIXTURE_REPOSITORY_VERSION &&
        report.plan_count == 2U && report.attached_count == 0U);
    control = report.token;
    CHECK(control != 0U &&
        package_control_open_install(TEST_OWNER, repository_upload,
            (const uint8_t *)"org.phipia.app", 14U, &report) ==
                PACKAGE_CONTROL_STATUS_NO_SLOT &&
        package_control_item(OTHER_OWNER, control, 0U, &item, &report) ==
            PACKAGE_CONTROL_STATUS_STALE && item.identifier_bytes == 0U &&
        find_plan_item(control, "org.phipia.lib", &library_index) == 0);
    CHECK(package_control_attach(TEST_OWNER, control, library_index,
        application_upload, &report) == PACKAGE_CONTROL_STATUS_UPLOAD &&
        (report.upload_status == PACKAGE_UPLOAD_STATUS_LENGTH ||
            report.upload_status == PACKAGE_UPLOAD_STATUS_DIGEST) &&
        report.attached_count == 0U);
    CHECK(attach_named(control, "org.phipia.lib", library_upload) == 0 &&
        package_control_attach(TEST_OWNER, control, library_index,
            library_upload, &report) == PACKAGE_CONTROL_STATUS_STATE &&
        attach_named(control, "org.phipia.app", application_upload) == 0);
    fail_floor_once = true;
    CHECK(package_control_commit(TEST_OWNER, control, &report) ==
            PACKAGE_CONTROL_STATUS_SERVICE &&
        report.service_status == PACKAGE_SERVICE_STATUS_DURABILITY &&
        !report.prepared && !report.committed &&
        service_current_bytes == 0U && service_repository_floor ==
            FIXTURE_REPOSITORY_VERSION &&
        package_control_commit(TEST_OWNER, control, &report) ==
            PACKAGE_CONTROL_STATUS_OK && report.committed &&
        report.generation == 1U && service_current_bytes != 0U &&
        package_control_close(TEST_OWNER, control, &report) ==
            PACKAGE_CONTROL_STATUS_OK && package_control_resources_released() &&
        live_allocations == 0U &&
        package_control_item(TEST_OWNER, control, 0U, &item, &report) ==
            PACKAGE_CONTROL_STATUS_STALE);

    CHECK(package_control_open_install(TEST_OWNER, repository_upload,
        (const uint8_t *)"org.phipia.app", 14U, &report) ==
            PACKAGE_CONTROL_STATUS_MANAGER &&
        report.manager_status == PACKAGE_MANAGER_STATUS_ALREADY_INSTALLED &&
        package_control_resources_released() && live_allocations == 0U);

    CHECK(package_control_open_install(TEST_OWNER, update_repository_upload,
        (const uint8_t *)"org.phipia.app", 14U, &report) ==
            PACKAGE_CONTROL_STATUS_OK && report.plan_count == 2U &&
        report.generation == 1U && report.repository_version ==
            FIXTURE_UPDATE_REPOSITORY_VERSION);
    control = report.token;
    CHECK(attach_named(control, "org.phipia.newlib", update_library_upload) ==
            0 &&
        attach_named(control, "org.phipia.app", update_application_upload) ==
            0);
    fail_commit_once = true;
    CHECK(package_control_commit(TEST_OWNER, control, &report) ==
            PACKAGE_CONTROL_STATUS_SERVICE &&
        report.service_status == PACKAGE_SERVICE_STATUS_DURABILITY &&
        report.prepared && !report.committed && report.generation == 2U &&
        service_prepared &&
        package_control_commit(TEST_OWNER, control, &report) ==
            PACKAGE_CONTROL_STATUS_OK && report.prepared && report.committed &&
        report.generation == 2U && !service_prepared &&
        package_control_close(TEST_OWNER, control, &report) ==
            PACKAGE_CONTROL_STATUS_OK && package_control_resources_released() &&
        live_allocations == 0U &&
        package_state_database_parse(service_current, service_current_bytes,
            &installed) == PACKAGE_STATE_STATUS_OK &&
        installed.generation == 2U && installed.package_count == 2U &&
        service_repository_floor == FIXTURE_UPDATE_REPOSITORY_VERSION);

    CHECK(package_control_open_install(TEST_OWNER, repository_upload,
        (const uint8_t *)"org.phipia.app", 14U, &report) ==
            PACKAGE_CONTROL_STATUS_MANAGER &&
        report.manager_status == PACKAGE_MANAGER_STATUS_ROLLBACK &&
        package_control_resources_released() && live_allocations == 0U);

    CHECK(package_control_open_repair(TEST_OWNER, update_repository_upload,
        &report) == PACKAGE_CONTROL_STATUS_OK && report.plan_count == 2U &&
        report.attached_count == 0U && report.generation == 2U);
    control = report.token;
    CHECK(attach_named(control, "org.phipia.newlib", update_library_upload) ==
            0 &&
        attach_named(control, "org.phipia.app", update_application_upload) ==
            0 &&
        package_control_commit(TEST_OWNER, control, &report) ==
            PACKAGE_CONTROL_STATUS_OK && report.prepared && report.committed &&
        report.generation == 3U && report.plan_count == 2U &&
        report.attached_count == 2U &&
        package_control_close(TEST_OWNER, control, &report) ==
            PACKAGE_CONTROL_STATUS_OK && package_control_resources_released() &&
        live_allocations == 0U &&
        package_state_database_parse(service_current, service_current_bytes,
            &installed) == PACKAGE_STATE_STATUS_OK &&
        installed.generation == 3U && installed.package_count == 2U);

    CHECK(strcmp(package_control_status_string(PACKAGE_CONTROL_STATUS_OK),
        "ok") == 0 && strcmp(package_control_status_string(
            PACKAGE_CONTROL_STATUS_COUNT), "unknown") == 0);
    free(repository.bytes);
    free(root.bytes);
    free(publisher.bytes);
    free(application.bytes);
    free(library.bytes);
    free(update_repository.bytes);
    free(update_application.bytes);
    free(update_library.bytes);
    free(changed_repository.bytes);
    CHECK(package_control_open_remove(TEST_OWNER,
        (const uint8_t *)"org.phipia.app", 14U, &report) ==
            PACKAGE_CONTROL_STATUS_OK && report.plan_count == 2U &&
        report.attached_count == 0U && report.generation == 3U);
    control = report.token;
    CHECK(package_control_commit(TEST_OWNER, control, &report) ==
            PACKAGE_CONTROL_STATUS_OK && report.prepared && report.committed &&
        report.generation == 4U && report.plan_count == 2U &&
        report.attached_count == 0U &&
        package_control_close(TEST_OWNER, control, &report) ==
            PACKAGE_CONTROL_STATUS_OK && package_control_resources_released() &&
        live_allocations == 0U &&
        package_state_database_parse(service_current, service_current_bytes,
            &installed) == PACKAGE_STATE_STATUS_OK &&
        installed.generation == 4U && installed.package_count == 0U);
    CHECK(package_control_open_remove(TEST_OWNER,
        (const uint8_t *)"org.phipia.app", 14U, &report) ==
            PACKAGE_CONTROL_STATUS_MANAGER &&
        report.manager_status == PACKAGE_MANAGER_STATUS_NOT_FOUND &&
        package_control_resources_released() && live_allocations == 0U);
    (void)puts("Phipia privileged package controller signed lifecycle tests passed");
    return 0;
}
