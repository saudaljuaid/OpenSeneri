/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <phipia/package_builder.h>
#include <phipia/package_manager.h>
#include <phipia/package_trust.h>

#define FIXTURE_NOW UINT64_C(1800000060)
#define FIXTURE_REPOSITORY_VERSION UINT64_C(42)
#define INSTALLED_PACKAGE_COUNT 2U
#define INSTALLED_EDGE_COUNT 1U
#define INSTALLED_EDGE_OFFSET (PACKAGE_STATE_DATABASE_HEADER_BYTES + \
    INSTALLED_PACKAGE_COUNT * PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES)
#define INSTALLED_BYTES (INSTALLED_EDGE_OFFSET + \
    INSTALLED_EDGE_COUNT * PACKAGE_STATE_DATABASE_EDGE_RECORD_BYTES)

#define CHECK(condition) do { \
    if (!(condition)) { \
        (void)fprintf(stderr, "package-manager host check failed at line %d: %s\n", \
            __LINE__, #condition); \
        return 1; \
    } \
} while (0)

struct file_bytes {
    uint8_t *bytes;
    size_t count;
};

enum trust_mode {
    TRUST_NORMAL = 0,
    TRUST_UNKNOWN,
    TRUST_REVOKED,
    TRUST_REJECT_SIGNATURE
};

struct trust_context {
    struct package_trust_key keys[2];
    struct package_trust_store store;
    enum trust_mode mode;
    uint32_t verification_count;
};

static bool same_bytes(const uint8_t *left, const uint8_t *right, size_t count)
{
    uint8_t difference = 0U;

    for (size_t index = 0U; index < count; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0U;
}

static bool any_bytes(const uint8_t *bytes, size_t count)
{
    uint8_t combined = 0U;

    for (size_t index = 0U; index < count; ++index) {
        combined |= bytes[index];
    }
    return combined != 0U;
}

static bool text_is(const struct package_manager_text *text, const char *value)
{
    size_t length = strlen(value);

    return text->length == length &&
        same_bytes(text->bytes, (const uint8_t *)value, length);
}

static bool state_text_is(const struct package_state_text *text, const char *value)
{
    size_t length = strlen(value);

    return text->length == length &&
        same_bytes(text->bytes, (const uint8_t *)value, length);
}

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

static enum package_manager_key_status trust_lookup(
    void *opaque,
    const uint8_t key_id[PACKAGE_MANAGER_SHA256_BYTES],
    uint8_t public_key[PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES]
)
{
    struct trust_context *context = opaque;

    if (context->mode == TRUST_UNKNOWN) {
        return PACKAGE_MANAGER_KEY_UNKNOWN;
    }
    if (context->mode == TRUST_REVOKED) {
        return PACKAGE_MANAGER_KEY_REVOKED;
    }
    return package_trust_lookup(&context->store, key_id, public_key);
}

/*
 * Retain injectable refusal modes around the production guest verifier so the
 * manager's status mapping remains independently covered.
 */
static bool trust_verify(
    void *opaque,
    const uint8_t public_key[PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES],
    const uint8_t signature[PACKAGE_MANAGER_ED25519_SIGNATURE_BYTES],
    const uint8_t *message,
    size_t message_bytes,
    size_t zero_offset,
    size_t zero_bytes
)
{
    struct trust_context *context = opaque;

    if (context->mode == TRUST_REJECT_SIGNATURE ||
        zero_bytes != PACKAGE_MANAGER_ED25519_SIGNATURE_BYTES ||
        zero_offset > message_bytes || zero_bytes > message_bytes - zero_offset ||
        signature != message + zero_offset || !any_bytes(signature, zero_bytes) ||
        (zero_offset != 232U && zero_offset != 440U) ||
        !package_trust_verify(&context->store, public_key, signature, message,
            message_bytes, zero_offset, zero_bytes)) {
        return false;
    }
    ++context->verification_count;
    return true;
}

static struct package_manager_policy normal_policy(void)
{
    const struct package_manager_policy result = {
        FIXTURE_NOW, FIXTURE_REPOSITORY_VERSION, 1U, false
    };

    return result;
}

static bool initialize_trust_context(
    struct trust_context *context,
    const uint8_t root_public[PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES],
    const uint8_t publisher_public[PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES]
)
{
    struct package_trust_key temporary;
    (void)memset(context, 0, sizeof(*context));
    (void)memcpy(context->keys[0].public_key, root_public,
        sizeof(context->keys[0].public_key));
    (void)memcpy(context->keys[1].public_key, publisher_public,
        sizeof(context->keys[1].public_key));
    for (size_t index = 0U; index < 2U; ++index) {
        context->keys[index].status = PACKAGE_MANAGER_KEY_TRUSTED;
        if (package_state_sha256(context->keys[index].public_key,
            sizeof(context->keys[index].public_key),
            context->keys[index].key_id) != PACKAGE_STATE_STATUS_OK) {
            return false;
        }
    }
    if (memcmp(context->keys[0].key_id, context->keys[1].key_id,
        sizeof(context->keys[0].key_id)) > 0) {
        temporary = context->keys[0];
        context->keys[0] = context->keys[1];
        context->keys[1] = temporary;
    }
    return package_trust_open(context->keys, 2U, &context->store) ==
        PACKAGE_TRUST_STATUS_OK;
}

static struct package_manager_trust make_trust(struct trust_context *context)
{
    const struct package_manager_trust result = {
        trust_lookup, trust_verify, context
    };

    return result;
}

static bool find_entry(
    const struct package_manager_repository_view *repository,
    const char *identifier,
    struct package_manager_catalog_entry *result
)
{
    for (uint32_t index = 0U; index < repository->package_count; ++index) {
        if (package_manager_repository_entry(repository, index, result) ==
                PACKAGE_MANAGER_STATUS_OK && text_is(&result->identifier, identifier)) {
            return true;
        }
    }
    return false;
}

static void put_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
}

static void put_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static void put_u64(uint8_t *bytes, uint64_t value)
{
    put_u32(bytes, (uint32_t)value);
    put_u32(bytes + 4U, (uint32_t)(value >> 32U));
}

static void put_text(uint8_t *field, size_t width, const char *text)
{
    size_t length = strlen(text);

    (void)memset(field, 0, width);
    if (length < width) {
        (void)memcpy(field, text, length);
    }
}

static void put_magic(uint8_t *field, const char *magic)
{
    (void)memcpy(field, magic, 8U);
}

static bool build_installed_database(
    uint8_t database[INSTALLED_BYTES],
    const struct package_manager_catalog_entry *application,
    const struct package_manager_catalog_entry *library
)
{
    uint8_t *application_record;
    uint8_t *library_record;
    uint8_t *edge;
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];

    (void)memset(database, 0, INSTALLED_BYTES);
    put_magic(database, "PHIPDB01");
    put_u16(database + 8U, 1U);
    put_u16(database + 10U, PACKAGE_STATE_DATABASE_HEADER_BYTES);
    put_u64(database + 16U, INSTALLED_BYTES);
    put_u64(database + 24U, 1U);
    put_u32(database + 32U, 1U);
    put_text(database + 40U, 16U, "x86_64");
    put_u64(database + 56U, PACKAGE_STATE_DATABASE_HEADER_BYTES);
    put_u32(database + 64U, INSTALLED_PACKAGE_COUNT);
    put_u32(database + 68U, PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES);
    put_u64(database + 72U, INSTALLED_EDGE_OFFSET);
    put_u32(database + 80U, INSTALLED_EDGE_COUNT);
    put_u32(database + 84U, PACKAGE_STATE_DATABASE_EDGE_RECORD_BYTES);
    put_u64(database + 88U, INSTALLED_BYTES);
    put_u32(database + 96U, 0U);
    put_u32(database + 100U, PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES);

    application_record = database + PACKAGE_STATE_DATABASE_HEADER_BYTES;
    library_record = application_record +
        PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES;
    edge = database + INSTALLED_EDGE_OFFSET;
    put_text(application_record, 64U, "org.phipia.app");
    put_text(application_record + 64U, 64U, "1.0.0");
    (void)memcpy(application_record + 128U, application->package_sha256, 32U);
    (void)memcpy(application_record + 160U, application->publisher_key_id, 32U);
    put_u32(application_record + 192U, 1U);
    put_u32(application_record + 196U, 0U);
    put_u32(application_record + 200U, 1U);
    put_u32(application_record + 204U, 0U);

    put_text(library_record, 64U, "org.phipia.lib");
    put_text(library_record + 64U, 64U, "1.0.0");
    (void)memcpy(library_record + 128U, library->package_sha256, 32U);
    (void)memcpy(library_record + 160U, library->publisher_key_id, 32U);
    put_u32(library_record + 192U, 0U);
    put_u32(library_record + 196U, 1U);
    put_u32(library_record + 200U, 0U);
    put_u32(library_record + 204U, 0U);

    put_text(edge, 64U, "org.phipia.lib");
    put_text(edge + 64U, 56U, "^1.0.0");
    put_text(edge + 120U, 64U, "org.phipia.lib");
    if (package_state_sha256(database + PACKAGE_STATE_DATABASE_HEADER_BYTES,
            INSTALLED_BYTES - PACKAGE_STATE_DATABASE_HEADER_BYTES, digest) !=
            PACKAGE_STATE_STATUS_OK) {
        return false;
    }
    (void)memcpy(database + 104U, digest, sizeof(digest));
    return true;
}

static int expect_plan_status(
    const char *path,
    const char *identifier,
    enum package_manager_status expected,
    struct trust_context *context
)
{
    struct file_bytes index = read_file(path,
        PACKAGE_MANAGER_REPOSITORY_MAX_BYTES);
    struct package_manager_policy policy = normal_policy();
    struct package_manager_trust trust = make_trust(context);
    struct package_manager_repository_view repository;
    struct package_manager_plan plan;
    enum package_manager_status status;

    if (index.bytes == NULL || package_manager_repository_open(index.bytes,
            index.count, &policy, &trust, &repository) !=
            PACKAGE_MANAGER_STATUS_OK) {
        free(index.bytes);
        return 1;
    }
    status = package_manager_plan_install(&repository, NULL,
        (const uint8_t *)identifier, strlen(identifier), &policy, &trust, &plan);
    free(index.bytes);
    return status == expected ? 0 : 1;
}

static int test_fresh_builder(
    const struct package_manager_repository_view *repository,
    const struct package_manager_plan *plan,
    const struct file_bytes *application,
    const struct file_bytes *library,
    const struct package_manager_policy *policy,
    const struct package_manager_trust *trust
)
{
    struct package_builder_workspace *workspace = calloc(1U, sizeof(*workspace));
    struct package_builder_package_bytes packages[2];
    struct package_state_database_view encoded_view;
    struct package_state_database_view promoted_view;
    struct package_manager_plan changed_plan = *plan;
    struct package_manager_plan promotion_plan;
    struct package_manager_plan removal_plan;
    uint8_t *encoded;
    uint8_t *promoted;
    size_t encoded_bytes;

    CHECK(workspace != NULL && plan->count == 2U);
    packages[0] = (struct package_builder_package_bytes){
        library->bytes, library->count
    };
    packages[1] = (struct package_builder_package_bytes){
        application->bytes, application->count
    };
    CHECK(package_builder_build(repository, NULL, plan, packages, 2U, policy,
            trust, workspace) == PACKAGE_MANAGER_STATUS_OK &&
        workspace->spec.generation == 1U && workspace->spec.abi == 1U &&
        workspace->spec.package_count == 2U &&
        workspace->spec.dependency_count == 1U &&
        workspace->spec.file_count == 2U &&
        state_text_is(&workspace->packages[0].identifier, "org.phipia.app") &&
        workspace->packages[0].explicit_root &&
        workspace->packages[0].dependency_start == 0U &&
        workspace->packages[0].dependency_count == 1U &&
        state_text_is(&workspace->packages[1].identifier, "org.phipia.lib") &&
        !workspace->packages[1].explicit_root &&
        state_text_is(&workspace->dependencies[0].requested,
            "org.phipia.lib") &&
        state_text_is(&workspace->dependencies[0].provider,
            "org.phipia.lib") &&
        state_text_is(&workspace->files[0].path, "bin/proof-app") &&
        workspace->files[0].owner_index == 0U &&
        workspace->file_sources[0].kind ==
            PACKAGE_BUILDER_FILE_SOURCE_PAYLOAD &&
        workspace->file_sources[0].package_index == 1U &&
        workspace->file_sources[0].payload_bytes ==
            sizeof("\x7f" "ELFproof-application") - 1U &&
        state_text_is(&workspace->files[1].path, "lib/libproof.so.1") &&
        workspace->files[1].owner_index == 1U &&
        workspace->file_sources[1].kind ==
            PACKAGE_BUILDER_FILE_SOURCE_PAYLOAD &&
        workspace->file_sources[1].package_index == 0U);
    CHECK(package_generation_size(&workspace->spec, &encoded_bytes) ==
        PACKAGE_STATE_STATUS_OK);
    encoded = malloc(encoded_bytes);
    CHECK(encoded != NULL && package_generation_encode(&workspace->spec,
            encoded, encoded_bytes, &encoded_view) == PACKAGE_STATE_STATUS_OK &&
        encoded_view.generation == 1U && encoded_view.file_count == 2U);
    {
        static const uint8_t application_payload[] =
            "\x7f" "ELFproof-application";
        static const uint8_t bad_payload[] = "damaged";
        static const uint8_t application_path[] = "bin/proof-app";
        static const uint8_t unknown_path[] = "bin/unknown";
        struct package_builder_repair_file replacement = {
            { application_path, sizeof(application_path) - 1U },
            application_payload, sizeof(application_payload) - 1U
        };
        struct package_state_database_view repair_view;
        uint8_t *repair_database;

        CHECK(package_builder_repair(&encoded_view, &replacement, 1U,
                workspace) == PACKAGE_MANAGER_STATUS_OK &&
            workspace->has_installed && workspace->verified_plan.operation ==
                PACKAGE_MANAGER_PLAN_REPAIR &&
            workspace->spec.generation == 2U &&
            workspace->spec.package_count == encoded_view.package_count &&
            workspace->spec.dependency_count == encoded_view.edge_count &&
            workspace->spec.file_count == encoded_view.file_count &&
            workspace->file_sources[0].kind ==
                PACKAGE_BUILDER_FILE_SOURCE_PAYLOAD &&
            workspace->file_sources[0].payload == application_payload &&
            workspace->file_sources[1].kind ==
                PACKAGE_BUILDER_FILE_SOURCE_INSTALLED);
        repair_database = malloc(encoded_bytes);
        CHECK(repair_database != NULL && package_generation_encode(
                &workspace->spec, repair_database, encoded_bytes,
                &repair_view) == PACKAGE_STATE_STATUS_OK &&
            repair_view.generation == 2U &&
            package_generation_verify(&workspace->spec, repair_database,
                encoded_bytes, &repair_view) == PACKAGE_STATE_STATUS_OK);
        free(repair_database);
        replacement.payload = bad_payload;
        replacement.payload_bytes = sizeof(bad_payload) - 1U;
        CHECK(package_builder_repair(&encoded_view, &replacement, 1U,
                workspace) == PACKAGE_MANAGER_STATUS_DIGEST &&
            workspace->spec.package_count == 0U);
        replacement.path = (struct package_state_text){
            unknown_path, sizeof(unknown_path) - 1U
        };
        replacement.payload = application_payload;
        replacement.payload_bytes = sizeof(application_payload) - 1U;
        CHECK(package_builder_repair(&encoded_view, &replacement, 1U,
                workspace) == PACKAGE_MANAGER_STATUS_NOT_FOUND &&
            workspace->spec.package_count == 0U);
    }
    CHECK(package_manager_plan_install(repository, &encoded_view,
        (const uint8_t *)"org.phipia.lib", 14U, policy, trust,
        &promotion_plan) == PACKAGE_MANAGER_STATUS_OK &&
        promotion_plan.count == 0U);
    CHECK(package_builder_build(repository, &encoded_view, &promotion_plan,
            NULL, 0U, policy, trust, workspace) == PACKAGE_MANAGER_STATUS_OK &&
        workspace->spec.generation == 2U && workspace->spec.file_count == 2U &&
        workspace->packages[1].explicit_root &&
        workspace->file_sources[0].kind ==
            PACKAGE_BUILDER_FILE_SOURCE_INSTALLED &&
        workspace->file_sources[0].payload == NULL &&
        workspace->file_sources[1].kind ==
            PACKAGE_BUILDER_FILE_SOURCE_INSTALLED &&
        workspace->file_sources[1].payload == NULL);
    promoted = malloc(encoded_bytes);
    CHECK(promoted != NULL && package_generation_encode(&workspace->spec,
            promoted, encoded_bytes, &promoted_view) == PACKAGE_STATE_STATUS_OK);
    CHECK(package_manager_plan_remove(&promoted_view,
        (const uint8_t *)"org.phipia.app", 14U, &removal_plan) ==
            PACKAGE_MANAGER_STATUS_OK && removal_plan.count == 1U &&
        text_is(&removal_plan.items[0].identifier, "org.phipia.app"));
    CHECK(package_builder_build(NULL, &promoted_view, &removal_plan, NULL, 0U,
            NULL, NULL, workspace) == PACKAGE_MANAGER_STATUS_OK &&
        workspace->spec.generation == 3U &&
        workspace->spec.package_count == 1U &&
        state_text_is(&workspace->packages[0].identifier, "org.phipia.lib") &&
        workspace->packages[0].explicit_root &&
        workspace->spec.file_count == 1U &&
        state_text_is(&workspace->files[0].path, "lib/libproof.so.1") &&
        workspace->files[0].owner_index == 0U &&
        workspace->file_sources[0].kind ==
            PACKAGE_BUILDER_FILE_SOURCE_INSTALLED);
    free(promoted);
    free(encoded);

    changed_plan.items[1].source_index = changed_plan.items[0].source_index;
    CHECK(package_builder_build(repository, NULL, &changed_plan, packages, 2U,
            policy, trust, workspace) == PACKAGE_MANAGER_STATUS_STATE &&
        workspace->spec.package_count == 0U);
    packages[0] = (struct package_builder_package_bytes){
        application->bytes, application->count
    };
    CHECK(package_builder_build(repository, NULL, plan, packages, 2U, policy,
            trust, workspace) != PACKAGE_MANAGER_STATUS_OK &&
        workspace->spec.package_count == 0U);
    free(workspace);
    return 0;
}

static int test_existing_builder(
    const struct package_manager_repository_view *repository,
    const struct package_state_database_view *installed,
    const struct package_manager_plan *plan,
    const struct package_manager_policy *policy,
    const struct package_manager_trust *trust,
    bool remove_all
)
{
    struct package_builder_workspace *workspace = calloc(1U, sizeof(*workspace));
    struct package_state_database_view encoded_view;
    uint8_t *encoded;
    size_t encoded_bytes;

    CHECK(workspace != NULL);
    if (remove_all) {
        CHECK(package_builder_build(NULL, installed, plan, NULL, 0U, NULL, NULL,
                workspace) == PACKAGE_MANAGER_STATUS_OK &&
            workspace->spec.generation == installed->generation + 1U &&
            workspace->spec.package_count == 0U &&
            workspace->spec.dependency_count == 0U &&
            workspace->spec.file_count == 0U);
    } else {
        CHECK(package_builder_build(repository, installed, plan, NULL, 0U,
                policy, trust, workspace) == PACKAGE_MANAGER_STATUS_OK &&
            workspace->spec.generation == installed->generation + 1U &&
            workspace->spec.package_count == 2U &&
            workspace->spec.dependency_count == 1U &&
            workspace->spec.file_count == 0U &&
            state_text_is(&workspace->packages[0].identifier,
                "org.phipia.app") && workspace->packages[0].explicit_root &&
            state_text_is(&workspace->packages[1].identifier,
                "org.phipia.lib") && workspace->packages[1].explicit_root);
    }
    CHECK(package_generation_size(&workspace->spec, &encoded_bytes) ==
        PACKAGE_STATE_STATUS_OK);
    encoded = malloc(encoded_bytes);
    CHECK(encoded != NULL && package_generation_encode(&workspace->spec,
            encoded, encoded_bytes, &encoded_view) == PACKAGE_STATE_STATUS_OK &&
        encoded_view.generation == installed->generation + 1U);
    free(encoded);
    free(workspace);
    return 0;
}

static int test_update_builder(
    const struct file_bytes *repository_bytes,
    const struct package_state_database_view *installed,
    const struct file_bytes *application,
    const struct file_bytes *library,
    const struct package_manager_policy *policy,
    const struct package_manager_trust *trust
)
{
    struct package_builder_workspace *workspace = calloc(1U, sizeof(*workspace));
    struct package_manager_repository_view repository;
    struct package_manager_plan plan;
    struct package_builder_package_bytes packages[2];
    struct package_state_database_view encoded_view;
    uint8_t *encoded;
    size_t encoded_bytes;

    CHECK(workspace != NULL && package_manager_repository_open(
        repository_bytes->bytes, repository_bytes->count, policy, trust,
        &repository) == PACKAGE_MANAGER_STATUS_OK);
    CHECK(package_manager_plan_install(&repository, installed,
        (const uint8_t *)"org.phipia.app", 14U, policy, trust, &plan) ==
            PACKAGE_MANAGER_STATUS_OK &&
        plan.operation == PACKAGE_MANAGER_PLAN_UPDATE && plan.count == 2U &&
        text_is(&plan.items[0].identifier, "org.phipia.newlib") &&
        text_is(&plan.items[1].identifier, "org.phipia.app"));
    packages[0] = (struct package_builder_package_bytes){
        library->bytes, library->count
    };
    packages[1] = (struct package_builder_package_bytes){
        application->bytes, application->count
    };
    CHECK(package_builder_build(&repository, installed, &plan, packages, 2U,
            policy, trust, workspace) == PACKAGE_MANAGER_STATUS_OK &&
        workspace->spec.generation == installed->generation + 1U &&
        workspace->spec.package_count == 2U &&
        workspace->spec.dependency_count == 1U &&
        workspace->spec.file_count == 2U &&
        state_text_is(&workspace->packages[0].identifier, "org.phipia.app") &&
        state_text_is(&workspace->packages[0].version, "2.0.0") &&
        workspace->packages[0].explicit_root &&
        state_text_is(&workspace->packages[1].identifier,
            "org.phipia.newlib") &&
        !workspace->packages[1].explicit_root &&
        state_text_is(&workspace->dependencies[0].provider,
            "org.phipia.newlib") &&
        state_text_is(&workspace->files[0].path, "bin/proof-app") &&
        state_text_is(&workspace->files[1].path, "lib/libnew.so.2") &&
        workspace->file_sources[0].kind ==
            PACKAGE_BUILDER_FILE_SOURCE_PAYLOAD &&
        workspace->file_sources[1].kind ==
            PACKAGE_BUILDER_FILE_SOURCE_PAYLOAD);
    CHECK(package_generation_size(&workspace->spec, &encoded_bytes) ==
        PACKAGE_STATE_STATUS_OK);
    encoded = malloc(encoded_bytes);
    CHECK(encoded != NULL && package_generation_encode(&workspace->spec,
            encoded, encoded_bytes, &encoded_view) == PACKAGE_STATE_STATUS_OK &&
        encoded_view.package_count == 2U && encoded_view.file_count == 2U);
    free(encoded);
    free(workspace);
    return 0;
}

int main(int argc, char **argv)
{
    struct file_bytes repository_bytes;
    struct file_bytes root_key;
    struct file_bytes publisher_key;
    struct file_bytes application_bytes;
    struct file_bytes library_bytes;
    struct file_bytes update_repository_bytes;
    struct file_bytes update_application_bytes;
    struct file_bytes update_library_bytes;
    struct trust_context context;
    struct package_manager_trust trust;
    struct package_manager_policy policy = normal_policy();
    struct package_manager_repository_view repository;
    struct package_manager_catalog_entry application_entry;
    struct package_manager_catalog_entry library_entry;
    struct package_manager_package_view application_package;
    struct package_manager_package_view library_package;
    struct package_manager_package_view changed_package;
    struct package_manager_file_view application_file;
    struct package_manager_file_view library_file;
    struct package_manager_relation_view application_dependency;
    struct package_manager_plan_binding application_binding;
    struct package_manager_search_results search;
    struct package_manager_plan plan;
    struct package_state_database_view installed;
    uint8_t installed_bytes[INSTALLED_BYTES];
    uint8_t *changed;

    CHECK(argc == 15);
    repository_bytes = read_file(argv[1], PACKAGE_MANAGER_REPOSITORY_MAX_BYTES);
    root_key = read_file(argv[2], PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES);
    publisher_key = read_file(argv[3], PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES);
    application_bytes = read_file(argv[4], PACKAGE_MANAGER_PACKAGE_MAX_BYTES);
    library_bytes = read_file(argv[5], PACKAGE_MANAGER_PACKAGE_MAX_BYTES);
    update_repository_bytes = read_file(argv[12],
        PACKAGE_MANAGER_REPOSITORY_MAX_BYTES);
    update_application_bytes = read_file(argv[13],
        PACKAGE_MANAGER_PACKAGE_MAX_BYTES);
    update_library_bytes = read_file(argv[14],
        PACKAGE_MANAGER_PACKAGE_MAX_BYTES);
    CHECK(repository_bytes.bytes != NULL &&
        root_key.count == PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES &&
        publisher_key.count == PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES &&
        application_bytes.bytes != NULL && library_bytes.bytes != NULL &&
        update_repository_bytes.bytes != NULL &&
        update_application_bytes.bytes != NULL && update_library_bytes.bytes != NULL);
    CHECK(initialize_trust_context(&context, root_key.bytes,
        publisher_key.bytes));
    trust = make_trust(&context);

    CHECK(package_manager_repository_open(repository_bytes.bytes,
        repository_bytes.count, &policy, &trust, &repository) ==
        PACKAGE_MANAGER_STATUS_OK);
    CHECK(repository.repository_version == FIXTURE_REPOSITORY_VERSION &&
        repository.package_count == 2U && context.verification_count == 1U &&
        find_entry(&repository, "org.phipia.app", &application_entry) &&
        find_entry(&repository, "org.phipia.lib", &library_entry));
    CHECK(package_manager_repository_search(&repository,
        (const uint8_t *)"LIB", 3U, &search) == PACKAGE_MANAGER_STATUS_OK &&
        search.count == 1U && search.repository_indices[0] ==
            library_entry.repository_index);
    CHECK(package_manager_plan_install(&repository, NULL,
        (const uint8_t *)"org.phipia.app", 14U, &policy, &trust, &plan) ==
            PACKAGE_MANAGER_STATUS_OK &&
        plan.operation == PACKAGE_MANAGER_PLAN_INSTALL &&
        text_is(&plan.target, "org.phipia.app") &&
        text_is(&plan.root, "org.phipia.app") && plan.count == 2U &&
        text_is(&plan.items[0].identifier, "org.phipia.lib") &&
        text_is(&plan.items[1].identifier, "org.phipia.app"));
    CHECK(package_manager_plan_dependency_binding(&repository, &plan, 1U, 0U,
            &application_binding) == PACKAGE_MANAGER_STATUS_OK &&
        application_binding.plan == &plan && application_binding.plan_index == 1U &&
        text_is(&application_binding.requested, "org.phipia.lib") &&
        text_is(&application_binding.constraint, ">=1.0.0,<2.0.0") &&
        text_is(&application_binding.provider, "org.phipia.lib") &&
        package_manager_plan_dependency_binding(&repository, &plan, 0U, 0U,
            &application_binding) == PACKAGE_MANAGER_STATUS_TABLE &&
        application_binding.plan == NULL &&
        package_manager_plan_dependency_binding(&repository, &plan, 1U, 1U,
            &application_binding) == PACKAGE_MANAGER_STATUS_TABLE);
    {
        struct package_manager_plan changed_plan = plan;

        changed_plan.items[1].source_index = plan.items[0].source_index;
        CHECK(package_manager_plan_dependency_binding(&repository,
            &changed_plan, 1U, 0U, &application_binding) ==
                PACKAGE_MANAGER_STATUS_STATE &&
            application_binding.plan == NULL);
    }

    CHECK(package_manager_package_open(application_bytes.bytes,
        application_bytes.count, &application_entry, &policy, &trust,
        &application_package) == PACKAGE_MANAGER_STATUS_OK &&
        text_is(&application_package.identifier, "org.phipia.app") &&
        application_package.file_count == 1U &&
        package_manager_package_file(&application_package, 0U,
            &application_file) == PACKAGE_MANAGER_STATUS_OK &&
        application_file.package == &application_package &&
        application_file.package_index == 0U &&
        text_is(&application_file.path, "bin/proof-app") &&
        application_file.kind == PACKAGE_MANAGER_FILE_EXECUTABLE &&
        application_file.mode == 0555U &&
        application_file.payload_bytes == sizeof("\x7f" "ELFproof-application") - 1U &&
        memcmp(application_file.payload, "\x7f" "ELFproof-application",
            application_file.payload_bytes) == 0 &&
        package_manager_package_dependency(&application_package, 0U,
            &application_dependency) == PACKAGE_MANAGER_STATUS_OK &&
        application_dependency.package == &application_package &&
        application_dependency.package_index == 0U &&
        text_is(&application_dependency.identifier, "org.phipia.lib") &&
        text_is(&application_dependency.constraint, ">=1.0.0,<2.0.0") &&
        package_manager_package_dependency(&application_package, 1U,
            &application_dependency) == PACKAGE_MANAGER_STATUS_TABLE &&
        package_manager_package_conflict(&application_package, 0U,
            &application_dependency) == PACKAGE_MANAGER_STATUS_TABLE);
    CHECK(package_manager_package_open(library_bytes.bytes, library_bytes.count,
        &library_entry, &policy, &trust, &library_package) ==
            PACKAGE_MANAGER_STATUS_OK &&
        text_is(&library_package.identifier, "org.phipia.lib") &&
        library_package.file_count == 1U &&
        package_manager_package_file(&library_package, 0U, &library_file) ==
            PACKAGE_MANAGER_STATUS_OK &&
        text_is(&library_file.path, "lib/libproof.so.1") &&
        text_is(&library_file.soname, "libproof.so.1") &&
        library_file.kind == PACKAGE_MANAGER_FILE_LIBRARY &&
        library_file.mode == 0444U &&
        library_file.payload_bytes == sizeof("\x7f" "ELFproof-library") - 1U &&
        memcmp(library_file.payload, "\x7f" "ELFproof-library",
            library_file.payload_bytes) == 0 &&
        context.verification_count == 3U);
    CHECK(test_fresh_builder(&repository, &plan, &application_bytes,
        &library_bytes, &policy, &trust) == 0);

    CHECK(build_installed_database(installed_bytes, &application_entry,
        &library_entry));
    CHECK(package_state_database_parse(installed_bytes, INSTALLED_BYTES,
        &installed) == PACKAGE_STATE_STATUS_OK);
    CHECK(package_manager_plan_install(&repository, &installed,
        (const uint8_t *)"org.phipia.app", 14U, &policy, &trust, &plan) ==
            PACKAGE_MANAGER_STATUS_ALREADY_INSTALLED);
    CHECK(package_manager_plan_remove(&installed,
        (const uint8_t *)"org.phipia.app", 14U, &plan) ==
            PACKAGE_MANAGER_STATUS_OK &&
        text_is(&plan.target, "org.phipia.app") && plan.count == 2U &&
        text_is(&plan.root, "org.phipia.app") &&
        text_is(&plan.items[0].identifier, "org.phipia.app") &&
        text_is(&plan.items[1].identifier, "org.phipia.lib"));
    CHECK(test_existing_builder(NULL, &installed, &plan, NULL, NULL, true) == 0);
    CHECK(package_manager_plan_remove(&installed,
        (const uint8_t *)"org.phipia.lib", 14U, &plan) ==
            PACKAGE_MANAGER_STATUS_IN_USE);
    CHECK(package_manager_installed_search(&installed,
        (const uint8_t *)"APP", 3U, &search) == PACKAGE_MANAGER_STATUS_OK &&
        search.count == 1U && search.repository_indices[0] == 0U);

    CHECK(package_manager_plan_install(&repository, &installed,
        (const uint8_t *)"virtual.proof", 13U, &policy, &trust, &plan) ==
            PACKAGE_MANAGER_STATUS_OK &&
        plan.operation == PACKAGE_MANAGER_PLAN_INSTALL &&
        text_is(&plan.target, "virtual.proof") &&
        text_is(&plan.root, "org.phipia.lib") && plan.count == 0U);
    CHECK(test_existing_builder(&repository, &installed, &plan, &policy,
        &trust, false) == 0);
    CHECK(test_update_builder(&update_repository_bytes, &installed,
        &update_application_bytes, &update_library_bytes, &policy, &trust) == 0);

    CHECK(package_manager_repository_open(repository_bytes.bytes,
        repository_bytes.count, &policy, NULL, &repository) ==
            PACKAGE_MANAGER_STATUS_CRYPTO_UNAVAILABLE);
    context.mode = TRUST_UNKNOWN;
    CHECK(package_manager_repository_open(repository_bytes.bytes,
        repository_bytes.count, &policy, &trust, &repository) ==
            PACKAGE_MANAGER_STATUS_UNKNOWN_KEY);
    context.mode = TRUST_REVOKED;
    CHECK(package_manager_repository_open(repository_bytes.bytes,
        repository_bytes.count, &policy, &trust, &repository) ==
            PACKAGE_MANAGER_STATUS_REVOKED_KEY);
    context.mode = TRUST_REJECT_SIGNATURE;
    CHECK(package_manager_repository_open(repository_bytes.bytes,
        repository_bytes.count, &policy, &trust, &repository) ==
            PACKAGE_MANAGER_STATUS_SIGNATURE);
    context.mode = TRUST_NORMAL;
    policy.minimum_repository_version = FIXTURE_REPOSITORY_VERSION + 1U;
    CHECK(package_manager_repository_open(repository_bytes.bytes,
        repository_bytes.count, &policy, &trust, &repository) ==
            PACKAGE_MANAGER_STATUS_ROLLBACK);
    policy = normal_policy();
    policy.now = FIXTURE_NOW - 1000U;
    CHECK(package_manager_repository_open(repository_bytes.bytes,
        repository_bytes.count, &policy, &trust, &repository) ==
            PACKAGE_MANAGER_STATUS_FRESHNESS);
    policy = normal_policy();
    policy.abi = 2U;
    CHECK(package_manager_repository_open(repository_bytes.bytes,
        repository_bytes.count, &policy, &trust, &repository) ==
            PACKAGE_MANAGER_STATUS_ABI);
    policy = normal_policy();

    changed = malloc(repository_bytes.count);
    CHECK(changed != NULL);
    (void)memcpy(changed, repository_bytes.bytes, repository_bytes.count);
    changed[repository_bytes.count - 1U] ^= UINT8_C(1);
    CHECK(package_manager_repository_open(changed, repository_bytes.count,
        &policy, &trust, &repository) == PACKAGE_MANAGER_STATUS_DIGEST);
    (void)memcpy(changed, repository_bytes.bytes, repository_bytes.count);
    (void)memset(changed + 232U, 0, PACKAGE_MANAGER_ED25519_SIGNATURE_BYTES);
    CHECK(package_manager_repository_open(changed, repository_bytes.count,
        &policy, &trust, &repository) == PACKAGE_MANAGER_STATUS_RESERVED);
    free(changed);

    context.mode = TRUST_REJECT_SIGNATURE;
    CHECK(package_manager_package_open(application_bytes.bytes,
        application_bytes.count, &application_entry, &policy, &trust,
        &application_package) == PACKAGE_MANAGER_STATUS_SIGNATURE);
    context.mode = TRUST_NORMAL;
    changed = malloc(application_bytes.count);
    CHECK(changed != NULL);
    (void)memcpy(changed, application_bytes.bytes, application_bytes.count);
    changed[application_bytes.count - 1U] ^= UINT8_C(1);
    changed_package = application_package;
    changed_package.bytes = changed;
    CHECK(package_manager_package_file(&changed_package, 0U,
        &application_file) == PACKAGE_MANAGER_STATUS_DIGEST);
    CHECK(package_manager_package_open(changed, application_bytes.count,
        &application_entry, &policy, &trust, &application_package) ==
            PACKAGE_MANAGER_STATUS_DIGEST);
    free(changed);

    CHECK(expect_plan_status(argv[6], "org.phipia.a",
        PACKAGE_MANAGER_STATUS_CYCLE, &context) == 0);
    CHECK(expect_plan_status(argv[7], "org.phipia.conflict-app",
        PACKAGE_MANAGER_STATUS_CONFLICT, &context) == 0);
    CHECK(expect_plan_status(argv[8], "org.phipia.ambiguous-app",
        PACKAGE_MANAGER_STATUS_AMBIGUOUS_PROVIDER, &context) == 0);
    CHECK(expect_plan_status(argv[9], "org.phipia.unsatisfied",
        PACKAGE_MANAGER_STATUS_DEPENDENCY, &context) == 0);
    CHECK(expect_plan_status(argv[10], "org.phipia.backtrack",
        PACKAGE_MANAGER_STATUS_OK, &context) == 0);
    CHECK(expect_plan_status(argv[11], "org.phipia.chain00",
        PACKAGE_MANAGER_STATUS_GRAPH_BOUND, &context) == 0);

    free(repository_bytes.bytes);
    free(root_key.bytes);
    free(publisher_key.bytes);
    free(application_bytes.bytes);
    free(library_bytes.bytes);
    free(update_repository_bytes.bytes);
    free(update_application_bytes.bytes);
    free(update_library_bytes.bytes);
    (void)puts("Phipia bounded guest package-manager and generation-builder "
        "tests passed");
    return 0;
}
