/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/package_builder.h>

#define PACKAGE_SOURCE_INSTALLED UINT8_C(1)
#define PACKAGE_SOURCE_STAGED UINT8_C(2)

static void clear_workspace(struct package_builder_workspace *workspace)
{
    uint8_t *bytes = (uint8_t *)workspace;

    for (size_t index = 0U; index < sizeof(*workspace); ++index) {
        bytes[index] = 0U;
    }
}

static bool same_bytes(const uint8_t *left, const uint8_t *right, size_t count)
{
    uint8_t difference = 0U;

    if (left == NULL || right == NULL) {
        return false;
    }
    for (size_t index = 0U; index < count; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0U;
}

static int compare_text(
    const uint8_t *left,
    size_t left_bytes,
    const uint8_t *right,
    size_t right_bytes
)
{
    size_t count = left_bytes < right_bytes ? left_bytes : right_bytes;

    for (size_t index = 0U; index < count; ++index) {
        if (left[index] != right[index]) {
            return left[index] < right[index] ? -1 : 1;
        }
    }
    if (left_bytes == right_bytes) {
        return 0;
    }
    return left_bytes < right_bytes ? -1 : 1;
}

static bool same_manager_text(
    const struct package_manager_text *left,
    const struct package_manager_text *right
)
{
    return left->length == right->length &&
        (left->length == 0U || same_bytes(left->bytes, right->bytes,
            left->length));
}

static bool same_state_manager_text(
    const struct package_state_text *left,
    const struct package_manager_text *right
)
{
    return left->length == right->length &&
        (left->length == 0U || same_bytes(left->bytes, right->bytes,
            left->length));
}

static bool same_plan_item(
    const struct package_manager_plan_item *left,
    const struct package_manager_plan_item *right
)
{
    return left->source_index == right->source_index &&
        same_manager_text(&left->identifier, &right->identifier) &&
        same_manager_text(&left->version, &right->version) &&
        same_manager_text(&left->download_path, &right->download_path) &&
        left->package_bytes == right->package_bytes &&
        same_bytes(left->package_sha256, right->package_sha256,
            PACKAGE_MANAGER_SHA256_BYTES) &&
        same_bytes(left->publisher_key_id, right->publisher_key_id,
            PACKAGE_MANAGER_SHA256_BYTES);
}

static bool same_plan(
    const struct package_manager_plan *left,
    const struct package_manager_plan *right
)
{
    if (left->operation != right->operation || left->count != right->count ||
        left->count > PACKAGE_MANAGER_PLAN_MAX_PACKAGES ||
        !same_manager_text(&left->target, &right->target) ||
        !same_manager_text(&left->root, &right->root)) {
        return false;
    }
    for (uint32_t index = 0U; index < left->count; ++index) {
        if (!same_plan_item(&left->items[index], &right->items[index])) {
            return false;
        }
    }
    return true;
}

static bool plan_index_for(
    const struct package_manager_plan *plan,
    const uint8_t *identifier,
    size_t identifier_bytes,
    uint32_t *result
)
{
    for (uint32_t index = 0U; index < plan->count; ++index) {
        const struct package_manager_text *candidate =
            &plan->items[index].identifier;

        if (candidate->length == identifier_bytes && same_bytes(
                candidate->bytes, identifier, identifier_bytes)) {
            *result = index;
            return true;
        }
    }
    return false;
}

static enum package_manager_status installed_package_at(
    const struct package_state_database_view *installed,
    uint32_t index,
    struct package_state_package_view *result
)
{
    return package_state_database_package(installed, index, result) ==
        PACKAGE_STATE_STATUS_OK ? PACKAGE_MANAGER_STATUS_OK :
        PACKAGE_MANAGER_STATUS_STATE;
}

static bool installed_index_for(
    const struct package_state_database_view *installed,
    const uint8_t *identifier,
    size_t identifier_bytes,
    uint32_t *result
)
{
    if (installed == NULL) {
        return false;
    }
    for (uint32_t index = 0U; index < installed->package_count; ++index) {
        struct package_state_package_view package;

        if (installed_package_at(installed, index, &package) !=
                PACKAGE_MANAGER_STATUS_OK) {
            return false;
        }
        if (package.identifier.length == identifier_bytes && same_bytes(
                package.identifier.bytes, identifier, identifier_bytes)) {
            *result = index;
            return true;
        }
    }
    return false;
}

static bool keep_installed_package(
    const struct package_manager_plan *plan,
    const struct package_state_package_view *package
)
{
    uint32_t ignored;

    return !plan_index_for(plan, package->identifier.bytes,
        package->identifier.length, &ignored);
}

static enum package_manager_status authenticate_inputs(
    const struct package_manager_repository_view *repository,
    const struct package_state_database_view *installed,
    const struct package_manager_plan *plan,
    const struct package_builder_package_bytes *packages,
    uint32_t package_count,
    const struct package_manager_policy *policy,
    const struct package_manager_trust *trust,
    struct package_builder_workspace *workspace
)
{
    enum package_manager_status status;

    if (installed != NULL) {
        if (installed->bytes == NULL || package_state_database_parse(
                installed->bytes, installed->byte_count,
                &workspace->installed) != PACKAGE_STATE_STATUS_OK) {
            return PACKAGE_MANAGER_STATUS_STATE;
        }
        workspace->has_installed = true;
    }
    if (plan->operation == PACKAGE_MANAGER_PLAN_REMOVE) {
        if (!workspace->has_installed || repository != NULL || policy != NULL ||
            trust != NULL || packages != NULL || package_count != 0U) {
            return PACKAGE_MANAGER_STATUS_NULL_ARGUMENT;
        }
        status = package_manager_plan_remove(&workspace->installed,
            plan->target.bytes, plan->target.length,
            &workspace->verified_plan);
    } else if (plan->operation == PACKAGE_MANAGER_PLAN_INSTALL ||
            plan->operation == PACKAGE_MANAGER_PLAN_UPDATE) {
        if (repository == NULL || repository->bytes == NULL || policy == NULL ||
            trust == NULL || package_count != plan->count ||
            (package_count != 0U && packages == NULL)) {
            return PACKAGE_MANAGER_STATUS_NULL_ARGUMENT;
        }
        if (workspace->has_installed && workspace->installed.abi != policy->abi) {
            return PACKAGE_MANAGER_STATUS_ABI;
        }
        status = package_manager_repository_open(repository->bytes,
            repository->byte_count, policy, trust, &workspace->repository);
        if (status != PACKAGE_MANAGER_STATUS_OK) {
            return status;
        }
        status = package_manager_plan_install(&workspace->repository,
            workspace->has_installed ? &workspace->installed : NULL,
            plan->target.bytes, plan->target.length, policy, trust,
            &workspace->verified_plan);
    } else {
        return PACKAGE_MANAGER_STATUS_TABLE;
    }
    if (status != PACKAGE_MANAGER_STATUS_OK) {
        return status;
    }
    if (!same_plan(plan, &workspace->verified_plan)) {
        return PACKAGE_MANAGER_STATUS_STATE;
    }
    for (uint32_t index = 0U; index < package_count; ++index) {
        struct package_manager_catalog_entry expected;

        status = package_manager_repository_entry(&workspace->repository,
            workspace->verified_plan.items[index].source_index, &expected);
        if (status != PACKAGE_MANAGER_STATUS_OK) {
            return status;
        }
        status = package_manager_package_open(packages[index].bytes,
            packages[index].byte_count, &expected, policy, trust,
            &workspace->admitted[index]);
        if (status != PACKAGE_MANAGER_STATUS_OK) {
            return status;
        }
    }
    return PACKAGE_MANAGER_STATUS_OK;
}

static enum package_manager_status select_packages(
    struct package_builder_workspace *workspace,
    uint8_t package_sources[PACKAGE_STATE_DATABASE_MAX_PACKAGES],
    uint16_t package_source_indices[PACKAGE_STATE_DATABASE_MAX_PACKAGES]
)
{
    bool installed_used[PACKAGE_STATE_DATABASE_MAX_PACKAGES] = { false };
    bool staged_used[PACKAGE_MANAGER_PLAN_MAX_PACKAGES] = { false };
    uint32_t staged_count = workspace->verified_plan.operation ==
        PACKAGE_MANAGER_PLAN_REMOVE ? 0U : workspace->verified_plan.count;
    uint32_t desired_count = staged_count;
    uint32_t dependency_count = 0U;

    if (workspace->has_installed) {
        for (uint32_t index = 0U; index < workspace->installed.package_count;
            ++index) {
            struct package_state_package_view package;

            if (installed_package_at(&workspace->installed, index, &package) !=
                    PACKAGE_MANAGER_STATUS_OK) {
                return PACKAGE_MANAGER_STATUS_STATE;
            }
            if (keep_installed_package(&workspace->verified_plan, &package)) {
                ++desired_count;
            }
        }
    }
    if (desired_count > PACKAGE_STATE_DATABASE_MAX_PACKAGES) {
        return PACKAGE_MANAGER_STATUS_GRAPH_BOUND;
    }
    for (uint32_t output = 0U; output < desired_count; ++output) {
        uint8_t selected_kind = 0U;
        uint32_t selected_index = 0U;
        const uint8_t *selected_identifier = NULL;
        size_t selected_identifier_bytes = 0U;

        if (workspace->has_installed) {
            for (uint32_t index = 0U; index < workspace->installed.package_count;
                ++index) {
                struct package_state_package_view package;

                if (installed_used[index] || installed_package_at(
                        &workspace->installed, index, &package) !=
                        PACKAGE_MANAGER_STATUS_OK ||
                    !keep_installed_package(&workspace->verified_plan, &package)) {
                    continue;
                }
                if (selected_kind == 0U || compare_text(package.identifier.bytes,
                        package.identifier.length, selected_identifier,
                        selected_identifier_bytes) < 0) {
                    selected_kind = PACKAGE_SOURCE_INSTALLED;
                    selected_index = index;
                    selected_identifier = package.identifier.bytes;
                    selected_identifier_bytes = package.identifier.length;
                }
            }
        }
        for (uint32_t index = 0U; index < staged_count; ++index) {
            const struct package_manager_package_view *package =
                &workspace->admitted[index];

            if (!staged_used[index] && (selected_kind == 0U || compare_text(
                    package->identifier.bytes, package->identifier.length,
                    selected_identifier, selected_identifier_bytes) < 0)) {
                selected_kind = PACKAGE_SOURCE_STAGED;
                selected_index = index;
                selected_identifier = package->identifier.bytes;
                selected_identifier_bytes = package->identifier.length;
            }
        }
        if (selected_kind == 0U || (output != 0U && compare_text(
                workspace->packages[output - 1U].identifier.bytes,
                workspace->packages[output - 1U].identifier.length,
                selected_identifier, selected_identifier_bytes) >= 0)) {
            return PACKAGE_MANAGER_STATUS_STATE;
        }
        package_sources[output] = selected_kind;
        package_source_indices[output] = (uint16_t)selected_index;
        if (selected_kind == PACKAGE_SOURCE_INSTALLED) {
            struct package_state_package_view package;
            bool explicit_root;

            if (installed_package_at(&workspace->installed, selected_index,
                    &package) != PACKAGE_MANAGER_STATUS_OK) {
                return PACKAGE_MANAGER_STATUS_STATE;
            }
            installed_used[selected_index] = true;
            explicit_root = package.explicit_root ||
                (workspace->verified_plan.operation !=
                    PACKAGE_MANAGER_PLAN_REMOVE &&
                same_state_manager_text(&package.identifier,
                    &workspace->verified_plan.root));
            workspace->packages[output] =
                (struct package_generation_package){
                    package.identifier, package.version, package.package_sha256,
                    package.publisher_key_id, explicit_root, dependency_count,
                    package.dependency_count, package.file_count
                };
            for (uint32_t offset = 0U; offset < package.dependency_count;
                ++offset) {
                struct package_state_dependency_view dependency;

                if (dependency_count == PACKAGE_STATE_DATABASE_MAX_EDGES ||
                    package_state_database_dependency(&workspace->installed,
                        package.dependency_start + offset, &dependency) !=
                        PACKAGE_STATE_STATUS_OK) {
                    return PACKAGE_MANAGER_STATUS_DEPENDENCY;
                }
                workspace->dependencies[dependency_count++] =
                    (struct package_generation_dependency){
                        dependency.requested, dependency.constraint,
                        dependency.provider
                    };
            }
        } else {
            const struct package_manager_package_view *package =
                &workspace->admitted[selected_index];
            const struct package_manager_plan_item *item =
                &workspace->verified_plan.items[selected_index];
            uint32_t old_index;
            bool explicit_root = same_manager_text(&package->identifier,
                &workspace->verified_plan.root);

            staged_used[selected_index] = true;
            if (workspace->has_installed && installed_index_for(
                    &workspace->installed, package->identifier.bytes,
                    package->identifier.length, &old_index)) {
                struct package_state_package_view old_package;

                if (installed_package_at(&workspace->installed, old_index,
                        &old_package) != PACKAGE_MANAGER_STATUS_OK) {
                    return PACKAGE_MANAGER_STATUS_STATE;
                }
                explicit_root = explicit_root || old_package.explicit_root;
            }
            workspace->packages[output] =
                (struct package_generation_package){
                    { package->identifier.bytes, package->identifier.length },
                    { package->version.bytes, package->version.length },
                    item->package_sha256, item->publisher_key_id, explicit_root,
                    dependency_count, package->dependency_count,
                    package->file_count
                };
            for (uint32_t offset = 0U; offset < package->dependency_count;
                ++offset) {
                struct package_manager_relation_view dependency;
                struct package_manager_plan_binding binding;
                enum package_manager_status status;

                if (dependency_count == PACKAGE_STATE_DATABASE_MAX_EDGES) {
                    return PACKAGE_MANAGER_STATUS_GRAPH_BOUND;
                }
                status = package_manager_package_dependency(package, offset,
                    &dependency);
                if (status != PACKAGE_MANAGER_STATUS_OK) {
                    return status;
                }
                status = package_manager_plan_dependency_binding(
                    &workspace->repository, &workspace->verified_plan,
                    selected_index, offset, &binding);
                if (status != PACKAGE_MANAGER_STATUS_OK ||
                    !same_manager_text(&dependency.identifier,
                        &binding.requested) ||
                    !same_manager_text(&dependency.constraint,
                        &binding.constraint)) {
                    return PACKAGE_MANAGER_STATUS_DEPENDENCY;
                }
                workspace->dependencies[dependency_count++] =
                    (struct package_generation_dependency){
                        { dependency.identifier.bytes,
                            dependency.identifier.length },
                        { dependency.constraint.bytes,
                            dependency.constraint.length },
                        { binding.provider.bytes, binding.provider.length }
                    };
            }
        }
    }
    workspace->spec.package_count = desired_count;
    workspace->spec.dependency_count = dependency_count;
    return PACKAGE_MANAGER_STATUS_OK;
}

static bool output_index_for(
    const struct package_builder_workspace *workspace,
    const struct package_state_text *identifier,
    uint32_t *result
)
{
    for (uint32_t index = 0U; index < workspace->spec.package_count; ++index) {
        if (workspace->packages[index].identifier.length == identifier->length &&
            same_bytes(workspace->packages[index].identifier.bytes,
                identifier->bytes, identifier->length)) {
            *result = index;
            return true;
        }
    }
    return false;
}

static enum package_manager_status prune_and_validate_graph(
    struct package_builder_workspace *workspace,
    uint8_t package_sources[PACKAGE_STATE_DATABASE_MAX_PACKAGES],
    uint16_t package_source_indices[PACKAGE_STATE_DATABASE_MAX_PACKAGES]
)
{
    uint16_t incoming[PACKAGE_STATE_DATABASE_MAX_PACKAGES] = { 0U };
    uint16_t queue[PACKAGE_STATE_DATABASE_MAX_PACKAGES];
    bool removed[PACKAGE_STATE_DATABASE_MAX_PACKAGES] = { false };
    bool reachable[PACKAGE_STATE_DATABASE_MAX_PACKAGES] = { false };
    size_t head = 0U;
    size_t tail = 0U;
    size_t removed_count = 0U;

    for (uint32_t owner = 0U; owner < workspace->spec.package_count; ++owner) {
        const struct package_generation_package *package =
            &workspace->packages[owner];

        for (uint32_t offset = 0U; offset < package->dependency_count; ++offset) {
            uint32_t provider;
            const struct package_generation_dependency *dependency =
                &workspace->dependencies[package->dependency_start + offset];

            if (!output_index_for(workspace, &dependency->provider, &provider) ||
                incoming[provider] == UINT16_MAX) {
                return PACKAGE_MANAGER_STATUS_DEPENDENCY;
            }
            ++incoming[provider];
        }
    }
    for (uint32_t index = 0U; index < workspace->spec.package_count; ++index) {
        if (incoming[index] == 0U) {
            queue[tail++] = (uint16_t)index;
        }
    }
    while (head < tail) {
        uint32_t owner = queue[head++];
        const struct package_generation_package *package =
            &workspace->packages[owner];

        if (removed[owner]) {
            return PACKAGE_MANAGER_STATUS_CYCLE;
        }
        removed[owner] = true;
        ++removed_count;
        for (uint32_t offset = 0U; offset < package->dependency_count; ++offset) {
            uint32_t provider;
            const struct package_generation_dependency *dependency =
                &workspace->dependencies[package->dependency_start + offset];

            if (!output_index_for(workspace, &dependency->provider, &provider) ||
                incoming[provider] == 0U) {
                return PACKAGE_MANAGER_STATUS_DEPENDENCY;
            }
            --incoming[provider];
            if (incoming[provider] == 0U) {
                queue[tail++] = (uint16_t)provider;
            }
        }
    }
    if (removed_count != workspace->spec.package_count) {
        return PACKAGE_MANAGER_STATUS_CYCLE;
    }

    head = 0U;
    tail = 0U;
    for (uint32_t index = 0U; index < workspace->spec.package_count; ++index) {
        if (workspace->packages[index].explicit_root) {
            reachable[index] = true;
            queue[tail++] = (uint16_t)index;
        }
    }
    while (head < tail) {
        uint32_t owner = queue[head++];
        const struct package_generation_package *package =
            &workspace->packages[owner];

        for (uint32_t offset = 0U; offset < package->dependency_count; ++offset) {
            uint32_t provider;
            const struct package_generation_dependency *dependency =
                &workspace->dependencies[package->dependency_start + offset];

            if (!output_index_for(workspace, &dependency->provider, &provider)) {
                return PACKAGE_MANAGER_STATUS_DEPENDENCY;
            }
            if (!reachable[provider]) {
                reachable[provider] = true;
                queue[tail++] = (uint16_t)provider;
            }
        }
    }
    {
        uint32_t package_count = 0U;
        uint32_t dependency_count = 0U;

        for (uint32_t old = 0U; old < workspace->spec.package_count; ++old) {
            struct package_generation_package package = workspace->packages[old];

            if (!reachable[old]) {
                if (package_sources[old] == PACKAGE_SOURCE_STAGED) {
                    return PACKAGE_MANAGER_STATUS_DEPENDENCY;
                }
                continue;
            }
            for (uint32_t offset = 0U; offset < package.dependency_count;
                ++offset) {
                workspace->dependencies[dependency_count + offset] =
                    workspace->dependencies[package.dependency_start + offset];
            }
            package.dependency_start = dependency_count;
            dependency_count += package.dependency_count;
            workspace->packages[package_count] = package;
            package_sources[package_count] = package_sources[old];
            package_source_indices[package_count] =
                package_source_indices[old];
            ++package_count;
        }
        workspace->spec.package_count = package_count;
        workspace->spec.dependency_count = dependency_count;
    }
    return PACKAGE_MANAGER_STATUS_OK;
}

static enum package_manager_status append_installed_files(
    struct package_builder_workspace *workspace,
    uint32_t output_owner,
    uint32_t installed_owner,
    uint32_t *file_count
)
{
    uint32_t found = 0U;

    for (uint32_t index = 0U; index < workspace->installed.file_count; ++index) {
        struct package_state_file_view file;

        if (package_state_database_file(&workspace->installed, index, &file) !=
                PACKAGE_STATE_STATUS_OK) {
            return PACKAGE_MANAGER_STATUS_STATE;
        }
        if (file.owner_index != installed_owner) {
            continue;
        }
        if (*file_count == PACKAGE_STATE_DATABASE_MAX_FILES) {
            return PACKAGE_MANAGER_STATUS_GRAPH_BOUND;
        }
        workspace->files[*file_count] = (struct package_generation_file){
            file.path, output_owner, file.kind, file.mode, file.length,
            file.sha256, file.soname
        };
        workspace->file_sources[*file_count] =
            (struct package_builder_file_source){
                PACKAGE_BUILDER_FILE_SOURCE_INSTALLED, installed_owner, index,
                NULL, 0U
            };
        ++*file_count;
        ++found;
    }
    return found == workspace->packages[output_owner].file_count ?
        PACKAGE_MANAGER_STATUS_OK : PACKAGE_MANAGER_STATUS_PACKAGE;
}

static enum package_manager_status append_staged_files(
    struct package_builder_workspace *workspace,
    uint32_t output_owner,
    uint32_t plan_index,
    uint32_t *file_count
)
{
    const struct package_manager_package_view *package =
        &workspace->admitted[plan_index];

    for (uint32_t index = 0U; index < package->file_count; ++index) {
        struct package_manager_file_view file;
        enum package_manager_status status = package_manager_package_file(
            package, index, &file);

        if (status != PACKAGE_MANAGER_STATUS_OK) {
            return status;
        }
        if (*file_count == PACKAGE_STATE_DATABASE_MAX_FILES) {
            return PACKAGE_MANAGER_STATUS_GRAPH_BOUND;
        }
        workspace->files[*file_count] = (struct package_generation_file){
            { file.path.bytes, file.path.length }, output_owner,
            (uint16_t)file.kind, file.mode, file.payload_bytes, file.sha256,
            { file.soname.bytes, file.soname.length }
        };
        workspace->file_sources[*file_count] =
            (struct package_builder_file_source){
                PACKAGE_BUILDER_FILE_SOURCE_PAYLOAD, plan_index, index,
                file.payload, file.payload_bytes
            };
        ++*file_count;
    }
    return PACKAGE_MANAGER_STATUS_OK;
}

static int compare_files(
    const struct package_generation_file *left,
    const struct package_generation_file *right
)
{
    return compare_text(left->path.bytes, left->path.length,
        right->path.bytes, right->path.length);
}

static void swap_files(
    struct package_builder_workspace *workspace,
    uint32_t left,
    uint32_t right
)
{
    struct package_generation_file file = workspace->files[left];
    struct package_builder_file_source source = workspace->file_sources[left];

    workspace->files[left] = workspace->files[right];
    workspace->file_sources[left] = workspace->file_sources[right];
    workspace->files[right] = file;
    workspace->file_sources[right] = source;
}

static void sift_files(
    struct package_builder_workspace *workspace,
    uint32_t root,
    uint32_t count
)
{
    for (;;) {
        uint32_t child = root * 2U + 1U;

        if (child >= count) {
            return;
        }
        if (child + 1U < count && compare_files(&workspace->files[child],
                &workspace->files[child + 1U]) < 0) {
            ++child;
        }
        if (compare_files(&workspace->files[root],
                &workspace->files[child]) >= 0) {
            return;
        }
        swap_files(workspace, root, child);
        root = child;
    }
}

static enum package_manager_status build_files(
    struct package_builder_workspace *workspace,
    const uint8_t package_sources[PACKAGE_STATE_DATABASE_MAX_PACKAGES],
    const uint16_t package_source_indices[PACKAGE_STATE_DATABASE_MAX_PACKAGES]
)
{
    uint32_t file_count = 0U;

    for (uint32_t owner = 0U; owner < workspace->spec.package_count; ++owner) {
        enum package_manager_status status =
            package_sources[owner] == PACKAGE_SOURCE_INSTALLED ?
            append_installed_files(workspace, owner,
                package_source_indices[owner], &file_count) :
            append_staged_files(workspace, owner,
                package_source_indices[owner], &file_count);

        if (status != PACKAGE_MANAGER_STATUS_OK) {
            return status;
        }
    }
    if (file_count > 1U) {
        for (uint32_t start = file_count / 2U; start != 0U; --start) {
            sift_files(workspace, start - 1U, file_count);
        }
        for (uint32_t end = file_count - 1U; end != 0U; --end) {
            swap_files(workspace, 0U, end);
            sift_files(workspace, 0U, end);
        }
        for (uint32_t index = 1U; index < file_count; ++index) {
            if (compare_files(&workspace->files[index - 1U],
                    &workspace->files[index]) == 0) {
                return PACKAGE_MANAGER_STATUS_CONFLICT;
            }
        }
    }
    workspace->spec.file_count = file_count;
    return PACKAGE_MANAGER_STATUS_OK;
}

enum package_manager_status package_builder_build(
    const struct package_manager_repository_view *repository,
    const struct package_state_database_view *installed,
    const struct package_manager_plan *plan,
    const struct package_builder_package_bytes *packages,
    uint32_t package_count,
    const struct package_manager_policy *policy,
    const struct package_manager_trust *trust,
    struct package_builder_workspace *workspace
)
{
    uint8_t package_sources[PACKAGE_STATE_DATABASE_MAX_PACKAGES] = { 0U };
    uint16_t package_source_indices[PACKAGE_STATE_DATABASE_MAX_PACKAGES] = { 0U };
    size_t ignored_size;
    enum package_manager_status status;

    if (plan == NULL || workspace == NULL) {
        return PACKAGE_MANAGER_STATUS_NULL_ARGUMENT;
    }
    clear_workspace(workspace);
    status = authenticate_inputs(repository, installed, plan, packages,
        package_count, policy, trust, workspace);
    if (status == PACKAGE_MANAGER_STATUS_OK) {
        status = select_packages(workspace, package_sources,
            package_source_indices);
    }
    if (status == PACKAGE_MANAGER_STATUS_OK) {
        status = prune_and_validate_graph(workspace, package_sources,
            package_source_indices);
    }
    if (status == PACKAGE_MANAGER_STATUS_OK) {
        status = build_files(workspace, package_sources,
            package_source_indices);
    }
    if (status == PACKAGE_MANAGER_STATUS_OK && workspace->has_installed &&
        workspace->installed.generation == UINT64_MAX) {
        status = PACKAGE_MANAGER_STATUS_OVERFLOW;
    }
    if (status == PACKAGE_MANAGER_STATUS_OK) {
        workspace->spec.generation = workspace->has_installed ?
            workspace->installed.generation + 1U : 1U;
        workspace->spec.abi = workspace->has_installed ?
            workspace->installed.abi : policy->abi;
        workspace->spec.packages = workspace->packages;
        workspace->spec.dependencies = workspace->dependencies;
        workspace->spec.files = workspace->files;
        if (package_generation_size(&workspace->spec, &ignored_size) !=
                PACKAGE_STATE_STATUS_OK) {
            status = PACKAGE_MANAGER_STATUS_STATE;
        }
    }
    if (status != PACKAGE_MANAGER_STATUS_OK) {
        clear_workspace(workspace);
    }
    return status;
}

enum package_manager_status package_builder_repair(
    const struct package_state_database_view *installed,
    const struct package_builder_repair_file *replacements,
    uint32_t replacement_count,
    struct package_builder_workspace *workspace
)
{
    size_t ignored_size;
    uint32_t replacement_index = 0U;
    enum package_manager_status status = PACKAGE_MANAGER_STATUS_OK;

    if (installed == NULL || workspace == NULL ||
        (replacement_count != 0U && replacements == NULL)) {
        return PACKAGE_MANAGER_STATUS_NULL_ARGUMENT;
    }
    clear_workspace(workspace);
    if (installed->bytes == NULL || package_state_database_parse(
            installed->bytes, installed->byte_count, &workspace->installed) !=
                PACKAGE_STATE_STATUS_OK) {
        return PACKAGE_MANAGER_STATUS_STATE;
    }
    if (workspace->installed.generation == UINT64_MAX ||
        replacement_count > workspace->installed.file_count) {
        status = workspace->installed.generation == UINT64_MAX ?
            PACKAGE_MANAGER_STATUS_OVERFLOW : PACKAGE_MANAGER_STATUS_GRAPH_BOUND;
        goto refuse;
    }
    for (uint32_t index = 0U; index < replacement_count; ++index) {
        if (replacements[index].path.bytes == NULL ||
            replacements[index].path.length == 0U ||
            replacements[index].payload == NULL ||
            (index != 0U && compare_text(replacements[index - 1U].path.bytes,
                replacements[index - 1U].path.length,
                replacements[index].path.bytes,
                replacements[index].path.length) >= 0)) {
            status = PACKAGE_MANAGER_STATUS_STATE;
            goto refuse;
        }
    }
    workspace->has_installed = true;
    workspace->verified_plan.operation = PACKAGE_MANAGER_PLAN_REPAIR;
    workspace->spec = (struct package_generation_spec){
        workspace->installed.generation + 1U,
        workspace->installed.abi,
        workspace->packages,
        workspace->installed.package_count,
        workspace->dependencies,
        workspace->installed.edge_count,
        workspace->files,
        workspace->installed.file_count
    };
    for (uint32_t index = 0U; index < workspace->installed.package_count;
        ++index) {
        struct package_state_package_view package;

        if (package_state_database_package(&workspace->installed, index,
                &package) != PACKAGE_STATE_STATUS_OK) {
            status = PACKAGE_MANAGER_STATUS_STATE;
            goto refuse;
        }
        workspace->packages[index] = (struct package_generation_package){
            package.identifier, package.version, package.package_sha256,
            package.publisher_key_id, package.explicit_root,
            package.dependency_start, package.dependency_count,
            package.file_count
        };
    }
    for (uint32_t index = 0U; index < workspace->installed.edge_count; ++index) {
        struct package_state_dependency_view dependency;

        if (package_state_database_dependency(&workspace->installed, index,
                &dependency) != PACKAGE_STATE_STATUS_OK) {
            status = PACKAGE_MANAGER_STATUS_STATE;
            goto refuse;
        }
        workspace->dependencies[index] =
            (struct package_generation_dependency){
                dependency.requested, dependency.constraint,
                dependency.provider
            };
    }
    for (uint32_t index = 0U; index < workspace->installed.file_count; ++index) {
        struct package_state_file_view file;
        bool replaced = false;

        if (package_state_database_file(&workspace->installed, index, &file) !=
                PACKAGE_STATE_STATUS_OK) {
            status = PACKAGE_MANAGER_STATUS_STATE;
            goto refuse;
        }
        workspace->files[index] = (struct package_generation_file){
            file.path, file.owner_index, file.kind, file.mode, file.length,
            file.sha256, file.soname
        };
        if (replacement_index < replacement_count) {
            int order = compare_text(replacements[replacement_index].path.bytes,
                replacements[replacement_index].path.length, file.path.bytes,
                file.path.length);

            if (order < 0) {
                status = PACKAGE_MANAGER_STATUS_NOT_FOUND;
                goto refuse;
            }
            if (order == 0) {
                uint8_t digest[PACKAGE_STATE_SHA256_BYTES];

                if (replacements[replacement_index].payload_bytes !=
                        file.length ||
                    package_state_sha256(
                        replacements[replacement_index].payload,
                        replacements[replacement_index].payload_bytes,
                        digest) != PACKAGE_STATE_STATUS_OK ||
                    !same_bytes(digest, file.sha256, sizeof(digest))) {
                    status = PACKAGE_MANAGER_STATUS_DIGEST;
                    goto refuse;
                }
                workspace->file_sources[index] =
                    (struct package_builder_file_source){
                        PACKAGE_BUILDER_FILE_SOURCE_PAYLOAD, file.owner_index,
                        index, replacements[replacement_index].payload,
                        replacements[replacement_index].payload_bytes
                    };
                ++replacement_index;
                replaced = true;
            }
        }
        if (!replaced) {
            workspace->file_sources[index] =
                (struct package_builder_file_source){
                    PACKAGE_BUILDER_FILE_SOURCE_INSTALLED, file.owner_index,
                    index, NULL, 0U
                };
        }
    }
    if (replacement_index != replacement_count) {
        status = PACKAGE_MANAGER_STATUS_NOT_FOUND;
        goto refuse;
    }
    if (package_generation_size(&workspace->spec, &ignored_size) !=
            PACKAGE_STATE_STATUS_OK) {
        status = PACKAGE_MANAGER_STATUS_STATE;
        goto refuse;
    }
    return PACKAGE_MANAGER_STATUS_OK;

refuse:
    clear_workspace(workspace);
    return status;
}
