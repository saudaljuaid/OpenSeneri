/* SPDX-License-Identifier: GPL-3.0-only */
/* One private process, address space, ELF image, stack, and proof-return gate. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/cpu.h>
#include <phipia/dma.h>
#include <phipia/elf64.h>
#include <phipia/filesystem.h>
#include <phipia/interrupt_vector.h>
#include <phipia/interrupts.h>
#include <phipia/memory.h>
#include <phipia/msix.h>
#include <phipia/paging.h>
#include <phipia/pci_resource.h>
#include <phipia/process.h>

#define PROCESS_EXPECTED_RETURN_RIP (ELF64_ENTRY_ADDRESS + UINT64_C(7))
#define PROCESS_SENTINEL_BYTES 32U

_Static_assert(sizeof(struct elf64_validated_image) == 88U,
    "Rust/C ELF64 validated-image ABI changed");
_Static_assert(ELF64_STATUS_CODE == 29,
    "Rust/C ELF64 status ABI changed");
_Static_assert(ELF64_LOAD_ADDRESS == PAGING_PROCESS_IMAGE_ADDRESS,
    "ELF and paging image placements disagree");
_Static_assert(ELF64_PROOF_VECTOR == INTERRUPT_PROCESS_PROOF_VECTOR,
    "ELF and interrupt proof vectors disagree");
_Static_assert(PAGING_PROCESS_STACK_END % UINT64_C(16) == 0U,
    "initial user stack is not psABI aligned");
_Static_assert(PAGING_PROCESS_STACK_PAGES == 4U,
    "stack failure-injection points assume exactly four stack pages");

struct process_resource_census {
    struct frame_allocator_stats frames;
    struct paging_state paging;
    struct dma_state dma;
    struct pci_resource_state pci;
    struct interrupt_vector_state vectors;
    struct msix_state msix;
    uint64_t cr3;
    bool filesystem_released;
    bool paging_process_released;
    bool proof_gate_released;
    bool user_boundary_inactive;
    bool interrupts_enabled;
};

struct process_runtime {
    uint64_t generation;
    enum process_state state;
    enum process_image_state image_state;
    enum process_stack_state stack_state;
    struct filesystem_private_file file;
    uint8_t elf_bytes[ELF64_FILE_BYTES];
    struct elf64_validated_image image;
    uintptr_t image_frame;
    uintptr_t stack_frames[PAGING_PROCESS_STACK_PAGES];
    size_t stack_frame_count;
    size_t stack_mapped_count;
    struct paging_process_space address_space;
    struct paging_process_image_alias alias;
    struct interrupt_process_gate gate;
    enum process_status return_status;
    bool image_mapped;
    bool return_seen;
    bool return_authenticated;
    bool interrupts_were_enabled;
};

enum process_failure_point {
    PROCESS_FAILURE_NONE = 0,
    PROCESS_FAILURE_AFTER_FILESYSTEM,
    PROCESS_FAILURE_AFTER_PARSE,
    PROCESS_FAILURE_AFTER_IMAGE_ALLOCATION,
    PROCESS_FAILURE_AFTER_IMAGE_INITIALIZATION,
    PROCESS_FAILURE_AFTER_STACK_ALLOCATION_ONE,
    PROCESS_FAILURE_AFTER_STACK_ALLOCATION_TWO,
    PROCESS_FAILURE_AFTER_STACK_ALLOCATION_THREE,
    PROCESS_FAILURE_AFTER_STACK_ALLOCATION_FOUR,
    PROCESS_FAILURE_AFTER_ADDRESS_SPACE,
    PROCESS_FAILURE_AFTER_ALIAS_NARROW,
    PROCESS_FAILURE_AFTER_IMAGE_MAPPING,
    PROCESS_FAILURE_AFTER_STACK_MAPPING_ONE,
    PROCESS_FAILURE_AFTER_STACK_MAPPING_FOUR,
    PROCESS_FAILURE_AFTER_PERMISSION_WALK,
    PROCESS_FAILURE_AFTER_GATE_ARM,
    PROCESS_FAILURE_AFTER_CR3_ACTIVATION,
    PROCESS_FAILURE_POINT_COUNT
};

_Static_assert(PROCESS_FAILURE_POINT_COUNT - 1U == 16U,
    "process cleanup control count changed");
_Static_assert(ELF64_PARSER_ROBUSTNESS_CONTROLS +
    PROCESS_FAILURE_POINT_COUNT - 1U == PROCESS_CONTROLLED_ROBUSTNESS_TESTS,
    "installed process robustness total changed");

static struct process_runtime runtime;
static struct process_proof_result installed_result;
static uint64_t next_process_generation = UINT64_C(1);
static bool process_proof_active;
static uint8_t process_sentinel[PROCESS_SENTINEL_BYTES] = {
    0x50, 0x48, 0x49, 0x50, 0x49, 0x41, 0x2D, 0x50,
    0x52, 0x4F, 0x43, 0x45, 0x53, 0x53, 0x2D, 0x53,
    0x45, 0x4E, 0x54, 0x49, 0x4E, 0x45, 0x4C, 0x2D,
    0x30, 0x37, 0x2D, 0x52, 0x49, 0x4E, 0x47, 0x33
};

static void zero_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool bytes_equal(
    const uint8_t *left,
    const uint8_t *right,
    size_t length
)
{
    for (size_t index = 0U; index < length; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

static bool canonical_user(uint64_t address)
{
    return address <= UINT64_C(0x00007FFFFFFFFFFF);
}

static enum process_status failure_status(
    enum process_failure_point point
)
{
    switch (point) {
    case PROCESS_FAILURE_AFTER_FILESYSTEM:
        return PROCESS_STATUS_FILESYSTEM;
    case PROCESS_FAILURE_AFTER_PARSE:
        return PROCESS_STATUS_ELF_PARSER;
    case PROCESS_FAILURE_AFTER_IMAGE_ALLOCATION:
    case PROCESS_FAILURE_AFTER_STACK_ALLOCATION_ONE:
    case PROCESS_FAILURE_AFTER_STACK_ALLOCATION_TWO:
    case PROCESS_FAILURE_AFTER_STACK_ALLOCATION_THREE:
    case PROCESS_FAILURE_AFTER_STACK_ALLOCATION_FOUR:
        return PROCESS_STATUS_FRAME_ALLOCATION;
    case PROCESS_FAILURE_AFTER_IMAGE_INITIALIZATION:
        return PROCESS_STATUS_FRAME_INITIALIZATION;
    case PROCESS_FAILURE_AFTER_ADDRESS_SPACE:
        return PROCESS_STATUS_ADDRESS_SPACE;
    case PROCESS_FAILURE_AFTER_ALIAS_NARROW:
        return PROCESS_STATUS_IMAGE_ALIAS;
    case PROCESS_FAILURE_AFTER_IMAGE_MAPPING:
    case PROCESS_FAILURE_AFTER_STACK_MAPPING_ONE:
    case PROCESS_FAILURE_AFTER_STACK_MAPPING_FOUR:
        return PROCESS_STATUS_USER_MAPPING;
    case PROCESS_FAILURE_AFTER_PERMISSION_WALK:
        return PROCESS_STATUS_USER_WALK;
    case PROCESS_FAILURE_AFTER_GATE_ARM:
        return PROCESS_STATUS_GATE;
    case PROCESS_FAILURE_AFTER_CR3_ACTIVATION:
        return PROCESS_STATUS_ENTRY;
    case PROCESS_FAILURE_NONE:
    case PROCESS_FAILURE_POINT_COUNT:
        return PROCESS_STATUS_OK;
    }
    return PROCESS_STATUS_ROBUSTNESS;
}

static bool proof_result_zero(const struct process_proof_result *result)
{
    const uint8_t *bytes = (const uint8_t *)(const void *)result;

    for (size_t index = 0U; index < sizeof(*result); ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static enum process_status process_transition(
    enum process_state *state,
    enum process_state next
)
{
    bool allowed = false;

    if (state == NULL || next >= PROCESS_STATE_COUNT) {
        return PROCESS_STATUS_TRANSITION_INVALID;
    }
    if (*state == next) {
        return PROCESS_STATUS_TRANSITION_REPEATED;
    }
    switch (*state) {
    case PROCESS_CANDIDATE:
        allowed = next == PROCESS_BUILDING || next == PROCESS_STOPPING;
        break;
    case PROCESS_BUILDING:
        allowed = next == PROCESS_INSTALLED || next == PROCESS_STOPPING;
        break;
    case PROCESS_INSTALLED:
        allowed = next == PROCESS_RUNNING || next == PROCESS_STOPPING;
        break;
    case PROCESS_RUNNING:
        allowed = next == PROCESS_STOPPING;
        break;
    case PROCESS_STOPPING:
        allowed = next == PROCESS_RELEASED;
        break;
    case PROCESS_RELEASED:
    case PROCESS_STATE_COUNT:
        break;
    }
    if (!allowed) {
        return next < *state ? PROCESS_STATUS_TRANSITION_REVERSED :
            PROCESS_STATUS_TRANSITION_INVALID;
    }
    *state = next;
    return PROCESS_STATUS_OK;
}

static void capture_census(struct process_resource_census *census)
{
    census->frames = frame_allocator_get_stats();
    census->paging = paging_get_state();
    census->dma = dma_get_state();
    census->pci = pci_resource_get_state();
    census->vectors = interrupt_vector_get_state();
    census->msix = msix_get_state();
    census->cr3 = cpu_read_cr3();
    census->filesystem_released = filesystem_resources_released();
    census->paging_process_released = paging_process_resources_released();
    census->proof_gate_released =
        interrupt_process_gate_resources_released();
    census->user_boundary_inactive = !process_user_boundary_active();
    census->interrupts_enabled = cpu_interrupts_enabled();
}

static bool census_equal(
    const struct process_resource_census *left,
    const struct process_resource_census *right
)
{
    return left->frames.addressable_frames == right->frames.addressable_frames &&
        left->frames.allocatable_frames == right->frames.allocatable_frames &&
        left->frames.free_frames == right->frames.free_frames &&
        left->frames.allocated_frames == right->frames.allocated_frames &&
        left->frames.reserved_frames == right->frames.reserved_frames &&
        left->frames.highest_allocatable_address ==
            right->frames.highest_allocatable_address &&
        left->paging.root_physical_address ==
            right->paging.root_physical_address &&
        left->paging.table_frames == right->paging.table_frames &&
        left->paging.active == right->paging.active &&
        left->dma.active_allocations == right->dma.active_allocations &&
        left->dma.cpu_owned_allocations == right->dma.cpu_owned_allocations &&
        left->dma.device_owned_allocations ==
            right->dma.device_owned_allocations &&
        left->pci.active_claims == right->pci.active_claims &&
        left->pci.active_mappings == right->pci.active_mappings &&
        left->pci.mapped_pages == right->pci.mapped_pages &&
        left->pci.bus_masters == right->pci.bus_masters &&
        left->vectors.allocated == right->vectors.allocated &&
        left->vectors.free == right->vectors.free &&
        left->msix.active_bindings == right->msix.active_bindings &&
        left->msix.failure_injection_armed ==
            right->msix.failure_injection_armed &&
        left->cr3 == right->cr3 &&
        left->filesystem_released == right->filesystem_released &&
        left->paging_process_released == right->paging_process_released &&
        left->proof_gate_released == right->proof_gate_released &&
        left->user_boundary_inactive == right->user_boundary_inactive &&
        left->interrupts_enabled == right->interrupts_enabled;
}

static bool validated_placement(const struct elf64_validated_image *image)
{
    return image != NULL && image->valid == 1U &&
        image->segment_count == ELF64_SEGMENT_COUNT &&
        image->elf_type == 2U && image->machine == 62U &&
        image->program_flags == 5U && image->entry == ELF64_ENTRY_ADDRESS &&
        image->file_offset == 0U &&
        image->virtual_address == ELF64_LOAD_ADDRESS &&
        image->file_size == ELF64_FILE_BYTES &&
        image->memory_size == ELF64_FILE_BYTES &&
        image->alignment == ELF64_PAGE_BYTES &&
        image->mapping_start == ELF64_LOAD_ADDRESS &&
        image->mapping_end == ELF64_LOAD_ADDRESS + ELF64_PAGE_BYTES &&
        canonical_user(image->entry) && canonical_user(image->mapping_end - 1U);
}

static bool return_frame_authenticated(const struct interrupt_frame *frame)
{
    return frame != NULL && runtime.state == PROCESS_RUNNING &&
        runtime.generation != 0U && runtime.gate.active &&
        runtime.address_space.state == PAGING_PROCESS_SPACE_ACTIVE &&
        frame->vector == INTERRUPT_PROCESS_PROOF_VECTOR &&
        frame->error_code == 0U && frame->rip == PROCESS_EXPECTED_RETURN_RIP &&
        frame->cs == CPU_GDT_USER_CODE_SELECTOR &&
        (frame->rflags & ~CPU_RFLAGS_PROCESSOR_BOOKKEEPING) == UINT64_C(2) &&
        interrupt_frame_has_stack_tail(frame) &&
        interrupt_frame_stack_pointer(frame) == PAGING_PROCESS_STACK_END &&
        interrupt_frame_stack_selector(frame) == CPU_GDT_USER_DATA_SELECTOR &&
        frame->rax == ELF64_PROOF_RESULT &&
        (cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U)) ==
            runtime.address_space.root_physical_address;
}

static void process_return_interrupt(
    struct interrupt_frame *frame,
    void *context
)
{
    struct process_runtime *active = context;
    uintptr_t resume_stack;

    if (active != &runtime || runtime.return_seen) {
        runtime.return_status = PROCESS_STATUS_RETURN_AUTHENTICATION;
    } else {
        runtime.return_seen = true;
        runtime.return_authenticated = return_frame_authenticated(frame);
        runtime.return_status = runtime.return_authenticated ?
            PROCESS_STATUS_OK : PROCESS_STATUS_RETURN_AUTHENTICATION;
    }
    if (paging_process_restore_kernel(&runtime.address_space) !=
            PAGING_STATUS_OK) {
        runtime.return_status = PROCESS_STATUS_KERNEL_CR3;
    }
    resume_stack = process_user_resume_stack();
    if (interrupt_request_kernel_resume(frame, resume_stack) !=
            INTERRUPT_STATUS_OK) {
        runtime.return_status = PROCESS_STATUS_GATE;
    }
}

static bool initialize_frame(uintptr_t frame)
{
    uint8_t *bytes = (uint8_t *)(void *)frame;

    for (size_t index = 0U; index < PAGING_PAGE_SIZE; ++index) {
        bytes[index] = 0U;
    }
    return true;
}

static enum process_status release_runtime(enum process_status result)
{
    bool cleanup_failed = false;

    cpu_interrupt_disable();
    if (runtime.address_space.state == PAGING_PROCESS_SPACE_ACTIVE &&
        paging_process_restore_kernel(&runtime.address_space) !=
            PAGING_STATUS_OK) {
        cleanup_failed = true;
    }
    if (runtime.state != PROCESS_STOPPING &&
        runtime.state != PROCESS_RELEASED &&
        process_transition(&runtime.state, PROCESS_STOPPING) !=
            PROCESS_STATUS_OK) {
        cleanup_failed = true;
    }
    if (runtime.gate.active &&
        interrupt_process_gate_disarm(&runtime.gate) != INTERRUPT_STATUS_OK) {
        cleanup_failed = true;
    }
    for (size_t mapped = runtime.stack_mapped_count; mapped > 0U; --mapped) {
        const size_t index = mapped - 1U;
        const uint64_t address = PAGING_PROCESS_STACK_BASE +
            index * PAGING_PAGE_SIZE;

        if (paging_process_unmap_user_page(&runtime.address_space,
                PAGING_PROCESS_MAPPING_STACK, address) != PAGING_STATUS_OK) {
            cleanup_failed = true;
        } else {
            --runtime.stack_mapped_count;
        }
    }
    if (runtime.image_mapped) {
        if (paging_process_unmap_user_page(&runtime.address_space,
                PAGING_PROCESS_MAPPING_IMAGE, PAGING_PROCESS_IMAGE_ADDRESS) !=
                PAGING_STATUS_OK) {
            cleanup_failed = true;
        } else {
            runtime.image_mapped = false;
        }
    }
    if (runtime.alias.active &&
        paging_process_image_alias_restore(&runtime.address_space,
            &runtime.alias) != PAGING_STATUS_OK) {
        cleanup_failed = true;
    }
    if (runtime.address_space.state != PAGING_PROCESS_SPACE_INVALID &&
        runtime.address_space.state != PAGING_PROCESS_SPACE_RELEASED &&
        paging_process_space_release(&runtime.address_space) !=
            PAGING_STATUS_OK) {
        cleanup_failed = true;
    }
    for (size_t count = runtime.stack_frame_count; count > 0U; --count) {
        const size_t index = count - 1U;

        if (frame_release(runtime.stack_frames[index]) != FRAME_STATUS_OK) {
            cleanup_failed = true;
        } else {
            runtime.stack_frames[index] = 0U;
            --runtime.stack_frame_count;
        }
    }
    if (runtime.image_frame != 0U) {
        if (frame_release(runtime.image_frame) != FRAME_STATUS_OK) {
            cleanup_failed = true;
        } else {
            runtime.image_frame = 0U;
        }
    }
    zero_bytes(runtime.elf_bytes, sizeof(runtime.elf_bytes));
    zero_bytes(&runtime.image, sizeof(runtime.image));
    runtime.image_state = PROCESS_IMAGE_RECLAIMED;
    runtime.stack_state = PROCESS_STACK_RECLAIMED;
    if (runtime.file.active &&
        filesystem_private_read_close(&runtime.file) != FILESYSTEM_STATUS_OK) {
        cleanup_failed = true;
    }
    if (runtime.state == PROCESS_STOPPING &&
        process_transition(&runtime.state, PROCESS_RELEASED) !=
            PROCESS_STATUS_OK) {
        cleanup_failed = true;
    }
    process_proof_active = false;
    if (runtime.interrupts_were_enabled) {
        cpu_interrupt_enable();
    }
    return cleanup_failed ? PROCESS_STATUS_TEARDOWN : result;
}

bool process_address_space_foundation_self_test(size_t *completed_tests)
{
    enum process_state state = PROCESS_CANDIDATE;
    size_t completed = 0U;

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;
    if ((PAGING_PROCESS_IMAGE_ADDRESS & (PAGING_PAGE_SIZE - 1U)) != 0U ||
        (PAGING_PROCESS_STACK_GUARD & (PAGING_PAGE_SIZE - 1U)) != 0U) {
        return false;
    }
    ++completed;
    if (!canonical_user(PAGING_PROCESS_IMAGE_ADDRESS) ||
        !canonical_user(PAGING_PROCESS_STACK_END - 1U)) {
        return false;
    }
    ++completed;
    if (PAGING_PROCESS_IMAGE_ADDRESS + PAGING_PAGE_SIZE >
            PAGING_PROCESS_STACK_GUARD ||
        PAGING_PROCESS_STACK_BASE !=
            PAGING_PROCESS_STACK_GUARD + PAGING_PAGE_SIZE) {
        return false;
    }
    ++completed;
    if (CPU_GDT_USER_DATA_SELECTOR != UINT16_C(0x2B) ||
        CPU_GDT_USER_CODE_SELECTOR != UINT16_C(0x33)) {
        return false;
    }
    ++completed;
    if (!cpu_user_transition_contract_valid() || cpu_tss_rsp0() == 0U) {
        return false;
    }
    ++completed;
    if (process_transition(&state, PROCESS_BUILDING) != PROCESS_STATUS_OK ||
        process_transition(&state, PROCESS_INSTALLED) != PROCESS_STATUS_OK ||
        process_transition(&state, PROCESS_RUNNING) != PROCESS_STATUS_OK ||
        process_transition(&state, PROCESS_STOPPING) != PROCESS_STATUS_OK ||
        process_transition(&state, PROCESS_RELEASED) != PROCESS_STATUS_OK) {
        return false;
    }
    ++completed;
    state = PROCESS_BUILDING;
    if (process_transition(&state, PROCESS_BUILDING) !=
            PROCESS_STATUS_TRANSITION_REPEATED ||
        process_transition(&state, PROCESS_CANDIDATE) !=
            PROCESS_STATUS_TRANSITION_REVERSED) {
        return false;
    }
    ++completed;
    if (!process_resources_released()) {
        return false;
    }
    ++completed;
    *completed_tests = completed;
    return completed == PROCESS_ADDRESS_SPACE_FOUNDATION_CONTROLS;
}

bool process_elf64_foundation_self_test(size_t *completed_tests)
{
    const uint32_t completed = phipia_elf64_self_test();

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = completed;
    return completed == ELF64_PARSER_ROBUSTNESS_CONTROLS;
}

static enum process_status process_attempt(
    struct process_proof_result *result,
    enum process_failure_point failure_point
)
{
    struct process_resource_census before;
    struct process_resource_census after;
    uint8_t sentinel_before[PROCESS_SENTINEL_BYTES];
    uint32_t file_bytes = 0U;
    uint32_t file_reads = 0U;
    enum filesystem_status file_status;
    enum process_status status = PROCESS_STATUS_OK;

    if (result == NULL || failure_point >= PROCESS_FAILURE_POINT_COUNT) {
        return PROCESS_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(result, sizeof(*result));
    if (process_proof_active || !process_resources_released()) {
        return PROCESS_STATUS_BUSY;
    }
    capture_census(&before);
    for (size_t index = 0U; index < sizeof(sentinel_before); ++index) {
        sentinel_before[index] = process_sentinel[index];
    }
    zero_bytes(&runtime, sizeof(runtime));
    runtime.state = PROCESS_CANDIDATE;
    runtime.image_state = PROCESS_IMAGE_CANDIDATE;
    runtime.stack_state = PROCESS_STACK_UNALLOCATED;
    runtime.generation = next_process_generation++;
    if (next_process_generation == 0U) {
        next_process_generation = 1U;
    }
    runtime.interrupts_were_enabled = before.interrupts_enabled;
    process_proof_active = true;
    if (process_transition(&runtime.state, PROCESS_BUILDING) !=
            PROCESS_STATUS_OK) {
        status = PROCESS_STATUS_TRANSITION_INVALID;
        goto cleanup;
    }
    file_status = filesystem_private_read_open(&runtime.file,
        runtime.elf_bytes, sizeof(runtime.elf_bytes));
    if (file_status == FILESYSTEM_STATUS_ABSENT) {
        status = PROCESS_STATUS_ABSENT;
        goto cleanup;
    }
    if (file_status != FILESYSTEM_STATUS_OK) {
        status = PROCESS_STATUS_FILESYSTEM;
        goto cleanup;
    }
    file_bytes = runtime.file.file_bytes;
    file_reads = runtime.file.read_count;
    if (!runtime.file.cpu_owned || file_bytes != ELF64_FILE_BYTES ||
        file_reads != 4U || runtime.file.msix_completion_count != 4U) {
        status = PROCESS_STATUS_FILESYSTEM;
        goto cleanup;
    }
    if (failure_point == PROCESS_FAILURE_AFTER_FILESYSTEM) {
        status = failure_status(failure_point);
        goto cleanup;
    }
    if (phipia_elf64_parse(runtime.elf_bytes, sizeof(runtime.elf_bytes),
            &runtime.image) != ELF64_STATUS_OK) {
        status = PROCESS_STATUS_ELF_PARSER;
        goto cleanup;
    }
    runtime.image_state = PROCESS_IMAGE_VALIDATED;
    if (!validated_placement(&runtime.image)) {
        status = PROCESS_STATUS_ELF_PLACEMENT;
        goto cleanup;
    }
    if (failure_point == PROCESS_FAILURE_AFTER_PARSE) {
        status = failure_status(failure_point);
        goto cleanup;
    }
    runtime.image_state = PROCESS_IMAGE_EXTENT_CHECKED;
    if (frame_allocate(&runtime.image_frame) != FRAME_STATUS_OK) {
        status = PROCESS_STATUS_FRAME_ALLOCATION;
        goto cleanup;
    }
    runtime.image_state = PROCESS_IMAGE_FRAME_ALLOCATED;
    if (failure_point == PROCESS_FAILURE_AFTER_IMAGE_ALLOCATION) {
        status = failure_status(failure_point);
        goto cleanup;
    }
    if (!initialize_frame(runtime.image_frame)) {
        status = PROCESS_STATUS_FRAME_INITIALIZATION;
        goto cleanup;
    }
    for (size_t index = 0U; index < sizeof(runtime.elf_bytes); ++index) {
        ((uint8_t *)(void *)runtime.image_frame)[index] =
            runtime.elf_bytes[index];
    }
    if (!bytes_equal((const uint8_t *)(const void *)runtime.image_frame,
            runtime.elf_bytes, sizeof(runtime.elf_bytes))) {
        status = PROCESS_STATUS_FRAME_INITIALIZATION;
        goto cleanup;
    }
    runtime.image_state = PROCESS_IMAGE_INITIALIZED;
    if (failure_point == PROCESS_FAILURE_AFTER_IMAGE_INITIALIZATION) {
        status = failure_status(failure_point);
        goto cleanup;
    }
    for (size_t index = 0U; index < PAGING_PROCESS_STACK_PAGES; ++index) {
        if (frame_allocate(&runtime.stack_frames[index]) != FRAME_STATUS_OK) {
            status = PROCESS_STATUS_FRAME_ALLOCATION;
            goto cleanup;
        }
        ++runtime.stack_frame_count;
        if (failure_point ==
                PROCESS_FAILURE_AFTER_STACK_ALLOCATION_ONE + index) {
            status = failure_status(failure_point);
            goto cleanup;
        }
        if (!initialize_frame(runtime.stack_frames[index])) {
            status = PROCESS_STATUS_FRAME_INITIALIZATION;
            goto cleanup;
        }
    }
    runtime.stack_state = PROCESS_STACK_ALLOCATED;
    if (paging_process_space_build(&runtime.address_space) != PAGING_STATUS_OK) {
        status = PROCESS_STATUS_ADDRESS_SPACE;
        goto cleanup;
    }
    if (failure_point == PROCESS_FAILURE_AFTER_ADDRESS_SPACE) {
        status = failure_status(failure_point);
        goto cleanup;
    }
    cpu_interrupt_disable();
    if (paging_process_image_alias_narrow(&runtime.address_space,
            runtime.image_frame, &runtime.alias) != PAGING_STATUS_OK) {
        status = PROCESS_STATUS_IMAGE_ALIAS;
        goto cleanup;
    }
    if (failure_point == PROCESS_FAILURE_AFTER_ALIAS_NARROW) {
        status = failure_status(failure_point);
        goto cleanup;
    }
    if (paging_process_map_user_page(&runtime.address_space,
            PAGING_PROCESS_MAPPING_IMAGE, PAGING_PROCESS_IMAGE_ADDRESS,
            runtime.image_frame, PAGING_EXECUTE) != PAGING_STATUS_OK) {
        status = PROCESS_STATUS_USER_MAPPING;
        goto cleanup;
    }
    runtime.image_mapped = true;
    runtime.image_state = PROCESS_IMAGE_MAPPED;
    if (failure_point == PROCESS_FAILURE_AFTER_IMAGE_MAPPING) {
        status = failure_status(failure_point);
        goto cleanup;
    }
    runtime.stack_state = PROCESS_STACK_GUARDED;
    for (size_t index = 0U; index < PAGING_PROCESS_STACK_PAGES; ++index) {
        if (paging_process_map_user_page(&runtime.address_space,
                PAGING_PROCESS_MAPPING_STACK,
                PAGING_PROCESS_STACK_BASE + index * PAGING_PAGE_SIZE,
                runtime.stack_frames[index], PAGING_WRITE) != PAGING_STATUS_OK) {
            status = PROCESS_STATUS_USER_MAPPING;
            goto cleanup;
        }
        ++runtime.stack_mapped_count;
        if ((index == 0U && failure_point ==
                PROCESS_FAILURE_AFTER_STACK_MAPPING_ONE) ||
            (index + 1U == PAGING_PROCESS_STACK_PAGES && failure_point ==
                PROCESS_FAILURE_AFTER_STACK_MAPPING_FOUR)) {
            status = failure_status(failure_point);
            goto cleanup;
        }
    }
    runtime.stack_state = PROCESS_STACK_MAPPED;
    if (paging_process_validate(&runtime.address_space, runtime.image_frame,
            runtime.stack_frames) != PAGING_STATUS_OK) {
        status = PROCESS_STATUS_USER_WALK;
        goto cleanup;
    }
    runtime.image_state = PROCESS_IMAGE_EXECUTABLE;
    runtime.stack_state = PROCESS_STACK_ACTIVE;
    if (failure_point == PROCESS_FAILURE_AFTER_PERMISSION_WALK) {
        status = failure_status(failure_point);
        goto cleanup;
    }
    if (!cpu_user_transition_contract_valid() ||
        cpu_tss_rsp0() == 0U ||
        !canonical_user(runtime.image.entry) ||
        !canonical_user(PAGING_PROCESS_STACK_END - 1U)) {
        status = PROCESS_STATUS_CPU_CONTRACT;
        goto cleanup;
    }
    if (interrupt_process_gate_arm(process_return_interrupt, &runtime,
            &runtime.gate) != INTERRUPT_STATUS_OK ||
        interrupt_process_gate_validate(&runtime.gate) !=
            INTERRUPT_STATUS_OK ||
        runtime.gate.state != INTERRUPT_PROCESS_GATE_ARMED) {
        status = PROCESS_STATUS_GATE;
        goto cleanup;
    }
    if (failure_point == PROCESS_FAILURE_AFTER_GATE_ARM) {
        status = failure_status(failure_point);
        goto cleanup;
    }
    if (process_transition(&runtime.state, PROCESS_INSTALLED) !=
            PROCESS_STATUS_OK) {
        status = PROCESS_STATUS_TRANSITION_INVALID;
        goto cleanup;
    }
    runtime.return_status = PROCESS_STATUS_ENTRY;
    if (process_transition(&runtime.state, PROCESS_RUNNING) !=
            PROCESS_STATUS_OK ||
        paging_process_activate(&runtime.address_space) != PAGING_STATUS_OK) {
        status = PROCESS_STATUS_ENTRY;
        goto cleanup;
    }
    if (failure_point == PROCESS_FAILURE_AFTER_CR3_ACTIVATION) {
        status = failure_status(failure_point);
        goto cleanup;
    }
    process_enter_user(runtime.image.entry, PAGING_PROCESS_STACK_END);
    if (process_user_boundary_active() || !runtime.return_seen ||
        runtime.return_status != PROCESS_STATUS_OK ||
        !runtime.return_authenticated ||
        (cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U)) !=
            paging_get_state().root_physical_address ||
        interrupt_process_gate_validate(&runtime.gate) !=
            INTERRUPT_STATUS_OK ||
        runtime.gate.state != INTERRUPT_PROCESS_GATE_RETURNED) {
        status = runtime.return_status == PROCESS_STATUS_OK ?
            PROCESS_STATUS_RETURN_AUTHENTICATION : runtime.return_status;
        goto cleanup;
    }

cleanup:
    status = release_runtime(status);
    if (status == PROCESS_STATUS_TEARDOWN) {
        zero_bytes(result, sizeof(*result));
        return status;
    }
    if (paging_verify() != PAGING_STATUS_OK ||
        !process_resources_released()) {
        return PROCESS_STATUS_TEARDOWN;
    }
    capture_census(&after);
    if (!census_equal(&before, &after)) {
        return PROCESS_STATUS_RESOURCE_CENSUS;
    }
    if (!bytes_equal(process_sentinel, sentinel_before,
            sizeof(sentinel_before))) {
        return PROCESS_STATUS_SENTINEL;
    }
    if (status != PROCESS_STATUS_OK) {
        zero_bytes(result, sizeof(*result));
        return status;
    }
    if (failure_point != PROCESS_FAILURE_NONE) {
        return PROCESS_STATUS_ROBUSTNESS;
    }
    result->file_bytes = file_bytes;
    result->segment_count = ELF64_SEGMENT_COUNT;
    result->result = ELF64_PROOF_RESULT;
    result->robustness_tests = PROCESS_CONTROLLED_ROBUSTNESS_TESTS;
    result->ring_three = true;
    result->private_address_space = true;
    result->image_read_execute = true;
    result->stack_read_write_no_execute = true;
    result->guard_unmapped = true;
    result->interrupt_authenticated = true;
    result->normal_exit = runtime.state == PROCESS_RELEASED && file_reads == 4U;
    result->teardown_complete = true;
    result->resource_census_equal = true;
    installed_result = *result;
    return PROCESS_STATUS_OK;
}

enum process_status process_installed_prove(
    struct process_proof_result *result
)
{
    struct process_proof_result controlled_result;

    if (result == NULL) {
        return PROCESS_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(result, sizeof(*result));
    zero_bytes(&installed_result, sizeof(installed_result));
    for (enum process_failure_point point =
            PROCESS_FAILURE_AFTER_FILESYSTEM;
        point < PROCESS_FAILURE_POINT_COUNT;
        point = (enum process_failure_point)(point + 1)) {
        enum process_status status;

        zero_bytes(&controlled_result, sizeof(controlled_result));
        status = process_attempt(&controlled_result, point);
        if (point == PROCESS_FAILURE_AFTER_FILESYSTEM &&
            status == PROCESS_STATUS_ABSENT) {
            return status;
        }
        if (status != failure_status(point) ||
            !proof_result_zero(&controlled_result) ||
            !process_resources_released() ||
            !proof_result_zero(&installed_result)) {
            zero_bytes(result, sizeof(*result));
            return PROCESS_STATUS_ROBUSTNESS;
        }
    }
    return process_attempt(result, PROCESS_FAILURE_NONE);
}

struct process_proof_result process_get_proof_result(void)
{
    return installed_result;
}

bool process_user_context_layout_self_test(void)
{
    /*
     * The assembly boundary reads this structure by fixed byte offset, so the
     * only way the two can be kept honest is to state the offsets here as
     * well. src/arch/x86_64/process.S carries the same numbers.
     */
    return sizeof(struct process_user_context) == 144U &&
        offsetof(struct process_user_context, rax) == 0U &&
        offsetof(struct process_user_context, rbx) == 8U &&
        offsetof(struct process_user_context, rcx) == 16U &&
        offsetof(struct process_user_context, rdx) == 24U &&
        offsetof(struct process_user_context, rsi) == 32U &&
        offsetof(struct process_user_context, rdi) == 40U &&
        offsetof(struct process_user_context, rbp) == 48U &&
        offsetof(struct process_user_context, r8) == 56U &&
        offsetof(struct process_user_context, r9) == 64U &&
        offsetof(struct process_user_context, r10) == 72U &&
        offsetof(struct process_user_context, r11) == 80U &&
        offsetof(struct process_user_context, r12) == 88U &&
        offsetof(struct process_user_context, r13) == 96U &&
        offsetof(struct process_user_context, r14) == 104U &&
        offsetof(struct process_user_context, r15) == 112U &&
        offsetof(struct process_user_context, rip) == 120U &&
        offsetof(struct process_user_context, rsp) == 128U &&
        offsetof(struct process_user_context, rflags) == 136U;
}

bool process_resources_released(void)
{
    const struct paging_state paging = paging_get_state();

    return !process_proof_active && paging_process_resources_released() &&
        interrupt_process_gate_resources_released() &&
        filesystem_resources_released() && !process_user_boundary_active() &&
        (!paging.active ||
            (cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U)) ==
                paging.root_physical_address);
}

const char *process_status_string(enum process_status status)
{
    static const char *const messages[PROCESS_STATUS_COUNT] = {
        "ok",
        "process proof fixture is absent",
        "null process argument",
        "the bounded process proof is already active",
        "process proof prerequisites are incomplete",
        "private filesystem read failed",
        "Rust ELF64 parser refused the file",
        "validated ELF placement is outside the fixed policy",
        "process frame allocation failed",
        "process frame initialization failed",
        "private process address-space construction failed",
        "executable identity-alias narrowing failed",
        "fixed user mapping installation failed",
        "effective user permission walk failed",
        "CPL3 descriptor or TSS contract failed",
        "private proof gate operation failed",
        "IRETQ user entry failed",
        "CPL3 proof return was not authenticated",
        "kernel CR3 restoration failed",
        "process transition was repeated",
        "process transition was reversed",
        "process transition is invalid",
        "process teardown leaked or failed",
        "process pre/post resource census differs",
        "process proof changed the kernel sentinel",
        "controlled process robustness cleanup failed"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        PROCESS_STATUS_COUNT, "process status messages are out of sync");
    if (status < PROCESS_STATUS_OK || status >= PROCESS_STATUS_COUNT) {
        return "unknown process status";
    }
    return messages[status];
}
