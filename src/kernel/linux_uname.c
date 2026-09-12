/* SPDX-License-Identifier: GPL-3.0-only */
/* One private static BusyBox uname process and reverse-order teardown. */

#include <phipia/linux_uname.h>

#include <phipia/console.h>
#include <phipia/cpu.h>
#include <phipia/dma.h>
#include <phipia/filesystem.h>
#include <phipia/fat32.h>
#include <phipia/interrupt_vector.h>
#include <phipia/linux_elf64.h>
#include <phipia/linux_syscall.h>
#include <phipia/memory.h>
#include <phipia/msix.h>
#include <phipia/paging.h>
#include <phipia/pci_resource.h>

#define LINUX_ARGUMENT_BYTES 17U
#define LINUX_INITIAL_STACK_WORDS 10U
#define LINUX_PAGING_FAILURE_CEILING 64U
#define LINUX_INITIAL_USER_PAGES \
    (PAGING_LINUX_UNAME_IMAGE_PAGES + PAGING_LINUX_STACK_PAGES)

_Static_assert(LINUX_UNAME_ABI_IMAGE_BYTES == LINUX_UNAME_ELF64_FILE_BYTES &&
    LINUX_UNAME_ABI_IMAGE_BYTES == LINUX_UNAME_FAT16_FILE_BYTES,
    "Linux ABI image-byte contracts diverged");
_Static_assert(sizeof(struct linux_fat16_chain) == 5136U &&
    _Alignof(struct linux_fat16_chain) == 8U &&
    offsetof(struct linux_fat16_chain, clusters) == 0U &&
    offsetof(struct linux_fat16_chain, lbas) == 1024U &&
    offsetof(struct linux_fat16_chain, cluster_count) == 5120U &&
    offsetof(struct linux_fat16_chain, file_bytes) == 5124U &&
    offsetof(struct linux_fat16_chain, final_cluster_bytes) == 5128U &&
    offsetof(struct linux_fat16_chain, valid) == 5132U,
    "Rust/C Linux FAT16 chain ABI changed");
_Static_assert(sizeof(struct linux_fat16_payload) == 40U &&
    _Alignof(struct linux_fat16_payload) == 4U &&
    offsetof(struct linux_fat16_payload, sha256) == 0U &&
    offsetof(struct linux_fat16_payload, byte_count) == 32U &&
    offsetof(struct linux_fat16_payload, deterministic) == 36U,
    "Rust/C Linux FAT16 payload ABI changed");
_Static_assert(sizeof(struct linux_elf64_segment) == 56U &&
    _Alignof(struct linux_elf64_segment) == 8U &&
    offsetof(struct linux_elf64_segment, file_offset) == 0U &&
    offsetof(struct linux_elf64_segment, virtual_address) == 8U &&
    offsetof(struct linux_elf64_segment, file_size) == 16U &&
    offsetof(struct linux_elf64_segment, memory_size) == 24U &&
    offsetof(struct linux_elf64_segment, mapping_start) == 32U &&
    offsetof(struct linux_elf64_segment, mapping_end) == 40U &&
    offsetof(struct linux_elf64_segment, flags) == 48U &&
    offsetof(struct linux_elf64_segment, reserved) == 52U,
    "Rust/C Linux ELF64 segment ABI changed");
_Static_assert(sizeof(struct linux_elf64_validated_image) == 248U &&
    _Alignof(struct linux_elf64_validated_image) == 8U &&
    offsetof(struct linux_elf64_validated_image, valid) == 0U &&
    offsetof(struct linux_elf64_validated_image, program_header_count) == 4U &&
    offsetof(struct linux_elf64_validated_image, segment_count) == 8U &&
    offsetof(struct linux_elf64_validated_image, non_load_count) == 12U &&
    offsetof(struct linux_elf64_validated_image, entry) == 16U &&
    offsetof(struct linux_elf64_validated_image, segments) == 24U,
    "Rust/C Linux ELF64 validated-image ABI changed");
_Static_assert(LINUX_UNAME_ELF64_PARSER_ROBUSTNESS_CONTROLS +
    LINUX_UNAME_FAT16_ROBUSTNESS_CONTROLS +
    LINUX_UNAME_ABI_STACK_FOUNDATION_CONTROLS +
    LINUX_SYSCALL_CPU_FOUNDATION_CONTROLS +
    LINUX_UNAME_SYSCALL_SEMANTIC_CONTROLS +
    LINUX_UNAME_COPYOUT_CONTROLS +
    LINUX_UNAME_ABI_LIFECYCLE_CONTROLS ==
        LINUX_UNAME_ABI_CONTROLLED_ROBUSTNESS_TESTS,
    "Linux ABI robustness matrix cardinality changed");
_Static_assert(LINUX_UNAME_ABI_IMAGE_UTS_FOUNDATION_CONTROLS ==
    LINUX_UNAME_ELF64_PARSER_ROBUSTNESS_CONTROLS +
        LINUX_UNAME_ABI_STACK_FOUNDATION_CONTROLS +
        LINUX_UNAME_SYSCALL_SEMANTIC_CONTROLS,
    "Linux uname image/UTS foundation total changed");
_Static_assert(LINUX_INITIAL_USER_PAGES <=
    PAGING_PROCESS_EXPECTED_MAX_PAGES,
    "Linux initial mappings exceed paging audit capacity");

struct linux_resource_census {
    struct frame_allocator_stats frames;
    struct paging_state paging;
    struct dma_state dma;
    struct pci_resource_state pci;
    struct interrupt_vector_state vectors;
    struct msix_state msix;
    uint64_t cr3;
    bool filesystem_released;
    bool paging_process_released;
    bool syscall_released;
    bool user_boundary_inactive;
    bool interrupts_enabled;
};

struct linux_runtime {
    uint64_t generation;
    enum linux_uname_process_state state;
    enum linux_uname_executable_state executable_state;
    enum linux_uname_stack_state stack_state;
    struct filesystem_linux_file file;
    uint8_t elf_bytes[LINUX_UNAME_ELF64_FILE_BYTES];
    struct linux_elf64_validated_image image;
    uintptr_t image_frames[PAGING_LINUX_UNAME_IMAGE_PAGES];
    uintptr_t stack_frames[PAGING_LINUX_STACK_PAGES];
    uintptr_t heap_frames[PAGING_LINUX_HEAP_PAGES];
    uintptr_t anonymous_frame;
    size_t image_frame_count;
    size_t stack_frame_count;
    size_t heap_frame_count;
    size_t image_mapped_count;
    size_t stack_mapped_count;
    struct paging_process_space address_space;
    struct paging_process_alias_set aliases;
    struct paging_process_expected_page
        expected_pages[LINUX_INITIAL_USER_PAGES];
    uint64_t initial_stack_pointer;
    struct linux_syscall_result syscall_result;
    bool interrupts_were_enabled;
    bool active;
};

enum linux_failure_point {
    LINUX_FAILURE_NONE = 0,
    LINUX_FAILURE_AFTER_FILESYSTEM,
    LINUX_FAILURE_AFTER_FILESYSTEM_OPEN,
    LINUX_FAILURE_AFTER_FILESYSTEM_READ_ONE,
    LINUX_FAILURE_AFTER_FILESYSTEM_READ_TWO,
    LINUX_FAILURE_AFTER_FILESYSTEM_READ_THREE,
    LINUX_FAILURE_AFTER_FILESYSTEM_READ_FOUR,
    LINUX_FAILURE_AFTER_FILESYSTEM_READ_FIVE,
    LINUX_FAILURE_AFTER_FILESYSTEM_READ_SIX,
    LINUX_FAILURE_AFTER_FILESYSTEM_READ_SEVEN,
    LINUX_FAILURE_AFTER_FILESYSTEM_READ_EIGHT,
    LINUX_FAILURE_AFTER_FILESYSTEM_READ_NINE,
    LINUX_FAILURE_AFTER_FILESYSTEM_READ_TEN,
    LINUX_FAILURE_AFTER_FILESYSTEM_READ_ELEVEN,
    LINUX_FAILURE_AFTER_FILESYSTEM_READ_TWELVE,
    LINUX_FAILURE_AFTER_FILESYSTEM_READ_THIRTEEN,
    LINUX_FAILURE_AFTER_PARSE,
    LINUX_FAILURE_AFTER_IMAGE_FRAME_ONE,
    LINUX_FAILURE_AFTER_IMAGE_FRAME_TWO,
    LINUX_FAILURE_AFTER_IMAGE_FRAME_THREE,
    LINUX_FAILURE_AFTER_IMAGE_FRAME_FOUR,
    LINUX_FAILURE_AFTER_IMAGE_FRAME_FIVE,
    LINUX_FAILURE_AFTER_IMAGE_FRAME_SIX,
    LINUX_FAILURE_AFTER_IMAGE_FRAME_SEVEN,
    LINUX_FAILURE_AFTER_IMAGE_FRAME_EIGHT,
    LINUX_FAILURE_AFTER_IMAGE_FRAME_NINE,
    LINUX_FAILURE_AFTER_IMAGE_FRAME_TEN,
    LINUX_FAILURE_AFTER_IMAGE_FRAME_ELEVEN,
    LINUX_FAILURE_AFTER_STACK_FRAME_ONE,
    LINUX_FAILURE_AFTER_STACK_FRAME_TWO,
    LINUX_FAILURE_AFTER_STACK_FRAME_THREE,
    LINUX_FAILURE_AFTER_STACK_FRAME_FOUR,
    LINUX_FAILURE_AFTER_HEAP_FRAME_ONE,
    LINUX_FAILURE_AFTER_HEAP_FRAME_TWO,
    LINUX_FAILURE_AFTER_ANONYMOUS_FRAME,
    LINUX_FAILURE_AFTER_STACK_STRING_ZERO,
    LINUX_FAILURE_AFTER_STACK_STRING_ONE,
    LINUX_FAILURE_AFTER_STACK_STRING_TWO,
    LINUX_FAILURE_AFTER_STACK_WORD_ONE,
    LINUX_FAILURE_AFTER_STACK_WORD_TWO,
    LINUX_FAILURE_AFTER_STACK_WORD_THREE,
    LINUX_FAILURE_AFTER_STACK_WORD_FOUR,
    LINUX_FAILURE_AFTER_STACK_WORD_FIVE,
    LINUX_FAILURE_AFTER_STACK_WORD_SIX,
    LINUX_FAILURE_AFTER_STACK_WORD_SEVEN,
    LINUX_FAILURE_AFTER_STACK_WORD_EIGHT,
    LINUX_FAILURE_AFTER_STACK_WORD_NINE,
    LINUX_FAILURE_AFTER_STACK_WORD_TEN,
    LINUX_FAILURE_AFTER_IMAGE_INSTALL,
    LINUX_FAILURE_AFTER_STACK_INSTALL,
    LINUX_FAILURE_AFTER_ADDRESS_SPACE,
    LINUX_FAILURE_AFTER_ALIAS_NARROW,
    LINUX_FAILURE_AFTER_IMAGE_MAPPING_ONE,
    LINUX_FAILURE_AFTER_IMAGE_MAPPING_TWO,
    LINUX_FAILURE_AFTER_IMAGE_MAPPING_THREE,
    LINUX_FAILURE_AFTER_IMAGE_MAPPING_FOUR,
    LINUX_FAILURE_AFTER_IMAGE_MAPPING_FIVE,
    LINUX_FAILURE_AFTER_IMAGE_MAPPING_SIX,
    LINUX_FAILURE_AFTER_IMAGE_MAPPING_SEVEN,
    LINUX_FAILURE_AFTER_IMAGE_MAPPING_EIGHT,
    LINUX_FAILURE_AFTER_IMAGE_MAPPING_NINE,
    LINUX_FAILURE_AFTER_IMAGE_MAPPING_TEN,
    LINUX_FAILURE_AFTER_IMAGE_MAPPING_ELEVEN,
    LINUX_FAILURE_AFTER_STACK_MAPPING_ONE,
    LINUX_FAILURE_AFTER_STACK_MAPPING_TWO,
    LINUX_FAILURE_AFTER_STACK_MAPPING_THREE,
    LINUX_FAILURE_AFTER_STACK_MAPPING_FOUR,
    LINUX_FAILURE_AFTER_PERMISSION_AUDIT,
    LINUX_FAILURE_AFTER_SYSCALL_ARM,
    LINUX_FAILURE_AFTER_CR3_ACTIVATION,
    LINUX_FAILURE_BEFORE_SYSCALL_ONE,
    LINUX_FAILURE_BEFORE_SYSCALL_TWO,
    LINUX_FAILURE_BEFORE_SYSCALL_THREE,
    LINUX_FAILURE_BEFORE_SYSCALL_FOUR,
    LINUX_FAILURE_BEFORE_SYSCALL_FIVE,
    LINUX_FAILURE_BEFORE_SYSCALL_SIX,
    LINUX_FAILURE_AFTER_SYSCALL_ONE,
    LINUX_FAILURE_AFTER_SYSCALL_TWO,
    LINUX_FAILURE_AFTER_SYSCALL_THREE,
    LINUX_FAILURE_AFTER_SYSCALL_FOUR,
    LINUX_FAILURE_AFTER_SYSCALL_FIVE,
    LINUX_FAILURE_AFTER_SYSCALL_SIX,
    LINUX_FAILURE_POINT_COUNT
};

_Static_assert(LINUX_FAILURE_POINT_COUNT - 1U == 81U,
    "Linux uname installed cleanup control count changed");

static struct linux_runtime runtime;
static struct linux_uname_abi_proof_result installed_result;
static uint64_t next_generation = UINT64_C(1);
static bool proof_active;
static const uint8_t linux_argv_zero[] = "busybox";
static const uint8_t linux_argv_one[] = "uname";
static const uint8_t linux_argv_two[] = "-s";

static bool failure_in_range(
    enum linux_failure_point point,
    enum linux_failure_point first,
    enum linux_failure_point last
)
{
    return point >= first && point <= last;
}

static enum linux_uname_abi_status failure_status(enum linux_failure_point point)
{
    if (failure_in_range(point, LINUX_FAILURE_AFTER_FILESYSTEM,
            LINUX_FAILURE_AFTER_FILESYSTEM_READ_THIRTEEN)) {
        return LINUX_UNAME_ABI_STATUS_FILESYSTEM;
    }
    if (point == LINUX_FAILURE_AFTER_PARSE) {
        return LINUX_UNAME_ABI_STATUS_ELF;
    }
    if (failure_in_range(point, LINUX_FAILURE_AFTER_IMAGE_FRAME_ONE,
            LINUX_FAILURE_AFTER_ANONYMOUS_FRAME)) {
        return LINUX_UNAME_ABI_STATUS_FRAME_ALLOCATION;
    }
    if (point == LINUX_FAILURE_AFTER_IMAGE_INSTALL) {
        return LINUX_UNAME_ABI_STATUS_ELF_INSTALL;
    }
    if (failure_in_range(point, LINUX_FAILURE_AFTER_STACK_STRING_ZERO,
            LINUX_FAILURE_AFTER_STACK_WORD_TEN) ||
        point == LINUX_FAILURE_AFTER_STACK_INSTALL) {
        return LINUX_UNAME_ABI_STATUS_STACK;
    }
    if (point == LINUX_FAILURE_AFTER_ADDRESS_SPACE) {
        return LINUX_UNAME_ABI_STATUS_ADDRESS_SPACE;
    }
    if (point == LINUX_FAILURE_AFTER_ALIAS_NARROW) {
        return LINUX_UNAME_ABI_STATUS_ALIAS;
    }
    if (failure_in_range(point, LINUX_FAILURE_AFTER_IMAGE_MAPPING_ONE,
            LINUX_FAILURE_AFTER_STACK_MAPPING_FOUR)) {
        return LINUX_UNAME_ABI_STATUS_MAPPING;
    }
    if (point == LINUX_FAILURE_AFTER_PERMISSION_AUDIT) {
        return LINUX_UNAME_ABI_STATUS_PERMISSION_AUDIT;
    }
    if (point == LINUX_FAILURE_AFTER_SYSCALL_ARM) {
        return LINUX_UNAME_ABI_STATUS_SYSCALL_CPU;
    }
    if (point == LINUX_FAILURE_AFTER_CR3_ACTIVATION) {
        return LINUX_UNAME_ABI_STATUS_ENTRY;
    }
    if (failure_in_range(point, LINUX_FAILURE_BEFORE_SYSCALL_ONE,
            LINUX_FAILURE_AFTER_SYSCALL_SIX)) {
        return LINUX_UNAME_ABI_STATUS_SYSCALL_CONTROL;
    }
    return LINUX_UNAME_ABI_STATUS_OK;
}

static uint32_t failure_before_ordinal(enum linux_failure_point point)
{
    return failure_in_range(point, LINUX_FAILURE_BEFORE_SYSCALL_ONE,
        LINUX_FAILURE_BEFORE_SYSCALL_SIX) ?
            (uint32_t)(point - LINUX_FAILURE_BEFORE_SYSCALL_ONE + 1U) : 0U;
}

static uint32_t filesystem_failure_boundary(enum linux_failure_point point)
{
    if (point == LINUX_FAILURE_AFTER_FILESYSTEM_OPEN) {
        return 14U;
    }
    return failure_in_range(point, LINUX_FAILURE_AFTER_FILESYSTEM_READ_ONE,
        LINUX_FAILURE_AFTER_FILESYSTEM_READ_THIRTEEN) ?
            (uint32_t)(point - LINUX_FAILURE_AFTER_FILESYSTEM_READ_ONE + 1U) :
            0U;
}

static uint32_t failure_after_ordinal(enum linux_failure_point point)
{
    return failure_in_range(point, LINUX_FAILURE_AFTER_SYSCALL_ONE,
        LINUX_FAILURE_AFTER_SYSCALL_SIX) ?
            (uint32_t)(point - LINUX_FAILURE_AFTER_SYSCALL_ONE + 1U) : 0U;
}

static void zero_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool proof_result_zero(const struct linux_uname_abi_proof_result *result)
{
    const uint8_t *bytes = (const uint8_t *)(const void *)result;

    for (size_t index = 0U; index < sizeof(*result); ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static void report_control_failure(
    const char *kind,
    uint64_t ordinal,
    enum linux_uname_abi_status status,
    enum linux_uname_abi_status expected,
    bool observed
)
{
    console_write("Phipia: Linux ABI control failure ");
    console_write(kind);
    console_write(" ordinal ");
    console_write_u64(ordinal);
    console_write(" status ");
    console_write_u64((uint64_t)status);
    console_write(" expected ");
    console_write_u64((uint64_t)expected);
    console_write(" observed ");
    console_write(observed ? "yes\n" : "no\n");
}

static bool canonical_user(uint64_t address)
{
    return address <= UINT64_C(0x00007FFFFFFFFFFF);
}

static enum linux_uname_abi_status transition_process(
    enum linux_uname_process_state *state,
    enum linux_uname_process_state next
)
{
    bool allowed = false;

    if (state == NULL || next >= LINUX_UNAME_PROCESS_STATE_COUNT) {
        return LINUX_UNAME_ABI_STATUS_TRANSITION_INVALID;
    }
    if (*state == next) {
        return LINUX_UNAME_ABI_STATUS_TRANSITION_REPEATED;
    }
    switch (*state) {
    case LINUX_UNAME_PROCESS_CANDIDATE:
        allowed = next == LINUX_UNAME_PROCESS_BUILDING ||
            next == LINUX_UNAME_PROCESS_STOPPING;
        break;
    case LINUX_UNAME_PROCESS_BUILDING:
        allowed = next == LINUX_UNAME_PROCESS_INSTALLED ||
            next == LINUX_UNAME_PROCESS_STOPPING;
        break;
    case LINUX_UNAME_PROCESS_INSTALLED:
        allowed = next == LINUX_UNAME_PROCESS_RUNNING ||
            next == LINUX_UNAME_PROCESS_STOPPING;
        break;
    case LINUX_UNAME_PROCESS_RUNNING:
        allowed = next == LINUX_UNAME_PROCESS_EXITING ||
            next == LINUX_UNAME_PROCESS_STOPPING;
        break;
    case LINUX_UNAME_PROCESS_EXITING:
        allowed = next == LINUX_UNAME_PROCESS_STOPPING;
        break;
    case LINUX_UNAME_PROCESS_STOPPING:
        allowed = next == LINUX_UNAME_PROCESS_RELEASED;
        break;
    case LINUX_UNAME_PROCESS_RELEASED:
    case LINUX_UNAME_PROCESS_STATE_COUNT:
        break;
    }
    if (!allowed) {
        return next < *state ? LINUX_UNAME_ABI_STATUS_TRANSITION_REVERSED :
            LINUX_UNAME_ABI_STATUS_TRANSITION_INVALID;
    }
    *state = next;
    return LINUX_UNAME_ABI_STATUS_OK;
}

static enum linux_uname_abi_status transition_executable(
    enum linux_uname_executable_state *state,
    enum linux_uname_executable_state next
)
{
    bool allowed = false;

    if (state == NULL || next >= LINUX_UNAME_EXECUTABLE_STATE_COUNT) {
        return LINUX_UNAME_ABI_STATUS_TRANSITION_INVALID;
    }
    if (*state == next) {
        return LINUX_UNAME_ABI_STATUS_TRANSITION_REPEATED;
    }
    switch (*state) {
    case LINUX_UNAME_EXECUTABLE_CANDIDATE:
        allowed = next == LINUX_UNAME_EXECUTABLE_VALIDATED ||
            next == LINUX_UNAME_EXECUTABLE_RELEASED;
        break;
    case LINUX_UNAME_EXECUTABLE_VALIDATED:
        allowed = next == LINUX_UNAME_EXECUTABLE_INSTALLED ||
            next == LINUX_UNAME_EXECUTABLE_RELEASED;
        break;
    case LINUX_UNAME_EXECUTABLE_INSTALLED:
        allowed = next == LINUX_UNAME_EXECUTABLE_RELEASED;
        break;
    case LINUX_UNAME_EXECUTABLE_RELEASED:
    case LINUX_UNAME_EXECUTABLE_STATE_COUNT:
        break;
    }
    if (!allowed) {
        return next < *state ? LINUX_UNAME_ABI_STATUS_TRANSITION_REVERSED :
            LINUX_UNAME_ABI_STATUS_TRANSITION_INVALID;
    }
    *state = next;
    return LINUX_UNAME_ABI_STATUS_OK;
}

static enum linux_uname_abi_status transition_stack(
    enum linux_uname_stack_state *state,
    enum linux_uname_stack_state next
)
{
    bool allowed = false;

    if (state == NULL || next >= LINUX_UNAME_STACK_STATE_COUNT) {
        return LINUX_UNAME_ABI_STATUS_TRANSITION_INVALID;
    }
    if (*state == next) {
        return LINUX_UNAME_ABI_STATUS_TRANSITION_REPEATED;
    }
    switch (*state) {
    case LINUX_UNAME_STACK_CANDIDATE:
        allowed = next == LINUX_UNAME_STACK_BUILDING ||
            next == LINUX_UNAME_STACK_RELEASED;
        break;
    case LINUX_UNAME_STACK_BUILDING:
        allowed = next == LINUX_UNAME_STACK_INSTALLED ||
            next == LINUX_UNAME_STACK_RELEASED;
        break;
    case LINUX_UNAME_STACK_INSTALLED:
        allowed = next == LINUX_UNAME_STACK_RELEASED;
        break;
    case LINUX_UNAME_STACK_RELEASED:
    case LINUX_UNAME_STACK_STATE_COUNT:
        break;
    }
    if (!allowed) {
        return next < *state ? LINUX_UNAME_ABI_STATUS_TRANSITION_REVERSED :
            LINUX_UNAME_ABI_STATUS_TRANSITION_INVALID;
    }
    *state = next;
    return LINUX_UNAME_ABI_STATUS_OK;
}

static void capture_census(struct linux_resource_census *census)
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
    census->syscall_released = linux_syscall_resources_released();
    census->user_boundary_inactive = !linux_process_boundary_active();
    census->interrupts_enabled = cpu_interrupts_enabled();
}

static bool census_equal(
    const struct linux_resource_census *left,
    const struct linux_resource_census *right
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
        left->syscall_released == right->syscall_released &&
        left->user_boundary_inactive == right->user_boundary_inactive &&
        left->interrupts_enabled == right->interrupts_enabled;
}

static bool validated_placement(
    const struct linux_elf64_validated_image *image
)
{
    if (image == NULL || image->valid != 1U ||
        image->program_header_count != LINUX_UNAME_ELF64_PROGRAM_HEADERS ||
        image->segment_count != LINUX_UNAME_ELF64_LOAD_SEGMENTS ||
        image->non_load_count != 1U || image->entry != LINUX_UNAME_ELF64_ENTRY) {
        return false;
    }
    for (size_t index = 0U; index < LINUX_UNAME_ELF64_LOAD_SEGMENTS; ++index) {
        const struct linux_elf64_segment *segment = &image->segments[index];

        if (segment->reserved != 0U || segment->file_size == 0U ||
            segment->memory_size < segment->file_size ||
            segment->mapping_start < PAGING_LINUX_UNAME_IMAGE_BASE ||
            segment->mapping_end > PAGING_LINUX_UNAME_IMAGE_END ||
            segment->mapping_start >= segment->mapping_end ||
            !canonical_user(segment->virtual_address) ||
            !canonical_user(segment->mapping_end - 1U)) {
            return false;
        }
    }
    return true;
}

static bool initialize_frame(uintptr_t frame)
{
    uint8_t *bytes = (uint8_t *)(void *)frame;

    if (frame == 0U) {
        return false;
    }
    for (size_t index = 0U; index < PAGING_PAGE_SIZE; ++index) {
        bytes[index] = 0U;
    }
    return true;
}

static bool image_write(
    uint64_t virtual_address,
    const uint8_t *source,
    size_t length
)
{
    uint64_t cursor = virtual_address;
    size_t remaining = length;

    if (source == NULL || length == 0U ||
        virtual_address < PAGING_LINUX_UNAME_IMAGE_BASE ||
        virtual_address > PAGING_LINUX_UNAME_IMAGE_END - length) {
        return false;
    }
    while (remaining > 0U) {
        const size_t page = (size_t)((cursor - PAGING_LINUX_UNAME_IMAGE_BASE) /
            PAGING_PAGE_SIZE);
        const size_t page_offset = (size_t)(cursor &
            (PAGING_PAGE_SIZE - 1U));
        size_t chunk = (size_t)PAGING_PAGE_SIZE - page_offset;
        uint8_t *destination;

        if (page >= runtime.image_frame_count ||
            runtime.image_frames[page] == 0U) {
            return false;
        }
        if (chunk > remaining) {
            chunk = remaining;
        }
        destination = (uint8_t *)(void *)runtime.image_frames[page] +
            page_offset;
        for (size_t index = 0U; index < chunk; ++index) {
            destination[index] = source[index];
        }
        source += chunk;
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool install_image(void)
{
    for (size_t segment_index = 0U;
         segment_index < LINUX_UNAME_ELF64_LOAD_SEGMENTS; ++segment_index) {
        const struct linux_elf64_segment *segment =
            &runtime.image.segments[segment_index];
        const size_t offset = (size_t)segment->file_offset;
        const size_t file_size = (size_t)segment->file_size;

        if (offset > sizeof(runtime.elf_bytes) ||
            file_size > sizeof(runtime.elf_bytes) - offset ||
            !image_write(segment->virtual_address,
                &runtime.elf_bytes[offset], file_size)) {
            return false;
        }
    }
    return true;
}

static bool stack_write(uint64_t address, const uint8_t *source, size_t length)
{
    uint64_t cursor = address;
    size_t remaining = length;

    if (source == NULL || length == 0U || address < PAGING_LINUX_STACK_BASE ||
        address > PAGING_LINUX_STACK_END - length) {
        return false;
    }
    while (remaining > 0U) {
        const size_t page = (size_t)((cursor - PAGING_LINUX_STACK_BASE) /
            PAGING_PAGE_SIZE);
        const size_t offset = (size_t)(cursor & (PAGING_PAGE_SIZE - 1U));
        size_t chunk = (size_t)PAGING_PAGE_SIZE - offset;
        uint8_t *destination;

        if (page >= runtime.stack_frame_count ||
            runtime.stack_frames[page] == 0U) {
            return false;
        }
        if (chunk > remaining) {
            chunk = remaining;
        }
        destination = (uint8_t *)(void *)runtime.stack_frames[page] + offset;
        for (size_t index = 0U; index < chunk; ++index) {
            destination[index] = source[index];
        }
        source += chunk;
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool stack_write_u64(uint64_t address, uint64_t value)
{
    uint8_t bytes[sizeof(uint64_t)];

    for (size_t index = 0U; index < sizeof(bytes); ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8U));
    }
    return stack_write(address, bytes, sizeof(bytes));
}

static bool stack_read(uint8_t *destination, uint64_t address, size_t length)
{
    uint64_t cursor = address;
    size_t remaining = length;

    if (destination == NULL || length == 0U ||
        address < PAGING_LINUX_STACK_BASE ||
        address > PAGING_LINUX_STACK_END - length) {
        return false;
    }
    while (remaining > 0U) {
        const size_t page = (size_t)((cursor - PAGING_LINUX_STACK_BASE) /
            PAGING_PAGE_SIZE);
        const size_t offset = (size_t)(cursor & (PAGING_PAGE_SIZE - 1U));
        size_t chunk = (size_t)PAGING_PAGE_SIZE - offset;
        const uint8_t *source;

        if (page >= runtime.stack_frame_count ||
            runtime.stack_frames[page] == 0U) {
            return false;
        }
        if (chunk > remaining) {
            chunk = remaining;
        }
        source = (const uint8_t *)(const void *)runtime.stack_frames[page] +
            offset;
        for (size_t index = 0U; index < chunk; ++index) {
            destination[index] = source[index];
        }
        destination += chunk;
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool initial_stack_layout_valid(
    const uint64_t words[LINUX_INITIAL_STACK_WORDS],
    uint64_t vector_address
)
{
    const uint64_t argv_two_address = PAGING_LINUX_STACK_END -
        sizeof(linux_argv_two);
    const uint64_t argv_one_address = argv_two_address -
        sizeof(linux_argv_one);
    const uint64_t argv_zero_address = argv_one_address -
        sizeof(linux_argv_zero);
    const uint64_t expected[LINUX_INITIAL_STACK_WORDS] = {
        UINT64_C(3), argv_zero_address, argv_one_address, argv_two_address,
        UINT64_C(0), UINT64_C(0), UINT64_C(6), PAGING_PAGE_SIZE,
        UINT64_C(0), UINT64_C(0)
    };

    if (words == NULL || (vector_address & UINT64_C(15)) != 0U ||
        !canonical_user(vector_address) ||
        vector_address < PAGING_LINUX_STACK_BASE ||
        vector_address > argv_zero_address -
            LINUX_INITIAL_STACK_WORDS * sizeof(uint64_t) ||
        argv_zero_address < PAGING_LINUX_STACK_BASE ||
        !canonical_user(PAGING_LINUX_STACK_END - 1U)) {
        return false;
    }
    for (size_t index = 0U; index < LINUX_INITIAL_STACK_WORDS; ++index) {
        if (words[index] != expected[index]) {
            return false;
        }
    }
    return true;
}

static bool initial_stack_installed_valid(uint64_t vector_address)
{
    uint64_t words[LINUX_INITIAL_STACK_WORDS];
    uint8_t bytes[LINUX_ARGUMENT_BYTES];
    const uint64_t strings_address = PAGING_LINUX_STACK_END -
        LINUX_ARGUMENT_BYTES;
    const uint8_t expected[LINUX_ARGUMENT_BYTES] = {
        'b', 'u', 's', 'y', 'b', 'o', 'x', 0,
        'u', 'n', 'a', 'm', 'e', 0,
        '-', 's', 0
    };

    if (!stack_read((uint8_t *)(void *)words, vector_address,
            sizeof(words)) || !initial_stack_layout_valid(words,
                vector_address) ||
        !stack_read(bytes, strings_address, sizeof(bytes))) {
        return false;
    }
    for (size_t index = 0U; index < sizeof(bytes); ++index) {
        if (bytes[index] != expected[index]) {
            return false;
        }
    }
    return true;
}

static bool build_initial_stack(enum linux_failure_point failure_point)
{
    const uint64_t argv_two_address = PAGING_LINUX_STACK_END -
        sizeof(linux_argv_two);
    const uint64_t argv_one_address = argv_two_address -
        sizeof(linux_argv_one);
    const uint64_t argv_zero_address = argv_one_address -
        sizeof(linux_argv_zero);
    const uint64_t vector_address = (argv_zero_address & ~UINT64_C(15)) -
        LINUX_INITIAL_STACK_WORDS * sizeof(uint64_t);
    const uint64_t words[LINUX_INITIAL_STACK_WORDS] = {
        UINT64_C(3), argv_zero_address, argv_one_address, argv_two_address,
        UINT64_C(0), UINT64_C(0), UINT64_C(6), PAGING_PAGE_SIZE,
        UINT64_C(0), UINT64_C(0)
    };

    if ((vector_address & UINT64_C(15)) != 0U ||
        vector_address < PAGING_LINUX_STACK_BASE ||
        sizeof(linux_argv_zero) + sizeof(linux_argv_one) +
            sizeof(linux_argv_two) !=
            LINUX_ARGUMENT_BYTES ||
        !initial_stack_layout_valid(words, vector_address)) {
        return false;
    }
    if (!stack_write(argv_zero_address, linux_argv_zero,
            sizeof(linux_argv_zero)) ||
        failure_point == LINUX_FAILURE_AFTER_STACK_STRING_ZERO) {
        return false;
    }
    if (!stack_write(argv_one_address, linux_argv_one,
            sizeof(linux_argv_one)) ||
        failure_point == LINUX_FAILURE_AFTER_STACK_STRING_ONE) {
        return false;
    }
    if (!stack_write(argv_two_address, linux_argv_two,
            sizeof(linux_argv_two)) ||
        failure_point == LINUX_FAILURE_AFTER_STACK_STRING_TWO) {
        return false;
    }
    for (size_t index = 0U; index < LINUX_INITIAL_STACK_WORDS; ++index) {
        if (!stack_write_u64(vector_address + index * sizeof(uint64_t),
                words[index]) ||
            failure_point == LINUX_FAILURE_AFTER_STACK_WORD_ONE + index) {
            return false;
        }
    }
    if (!initial_stack_installed_valid(vector_address)) {
        return false;
    }
    runtime.initial_stack_pointer = vector_address;
    return true;
}

static uint32_t page_permissions(size_t page)
{
    for (size_t segment_index = 0U;
         segment_index < LINUX_UNAME_ELF64_LOAD_SEGMENTS; ++segment_index) {
        const struct linux_elf64_segment *segment =
            &runtime.image.segments[segment_index];
        const uint64_t address = PAGING_LINUX_UNAME_IMAGE_BASE +
            page * PAGING_PAGE_SIZE;

        if (address >= segment->mapping_start &&
            address < segment->mapping_end) {
            if (segment->flags == UINT32_C(5)) {
                return PAGING_EXECUTE;
            }
            if (segment->flags == UINT32_C(6)) {
                return PAGING_WRITE;
            }
            return PAGING_READ;
        }
    }
    return UINT32_MAX;
}

static bool map_initial_pages(enum linux_failure_point failure_point)
{
    size_t expected = 0U;

    for (size_t page = 0U; page < PAGING_LINUX_UNAME_IMAGE_PAGES; ++page) {
        const uint64_t address = PAGING_LINUX_UNAME_IMAGE_BASE +
            page * PAGING_PAGE_SIZE;
        const uint32_t permissions = page_permissions(page);

        if (permissions == UINT32_MAX ||
            paging_process_map_user_page(&runtime.address_space,
                PAGING_PROCESS_MAPPING_LINUX_UNAME_IMAGE, address,
                runtime.image_frames[page], permissions) != PAGING_STATUS_OK) {
            return false;
        }
        ++runtime.image_mapped_count;
        runtime.expected_pages[expected].virtual_address = address;
        runtime.expected_pages[expected].physical_address =
            runtime.image_frames[page];
        runtime.expected_pages[expected].permissions = permissions;
        ++expected;
        if (failure_point ==
                LINUX_FAILURE_AFTER_IMAGE_MAPPING_ONE + page) {
            return false;
        }
    }
    for (size_t page = 0U; page < PAGING_LINUX_STACK_PAGES; ++page) {
        const uint64_t address = PAGING_LINUX_STACK_BASE +
            page * PAGING_PAGE_SIZE;

        if (paging_process_map_user_page(&runtime.address_space,
                PAGING_PROCESS_MAPPING_LINUX_STACK, address,
                runtime.stack_frames[page], PAGING_WRITE) !=
                PAGING_STATUS_OK) {
            return false;
        }
        ++runtime.stack_mapped_count;
        runtime.expected_pages[expected].virtual_address = address;
        runtime.expected_pages[expected].physical_address =
            runtime.stack_frames[page];
        runtime.expected_pages[expected].permissions = PAGING_WRITE;
        ++expected;
        if (failure_point ==
                LINUX_FAILURE_AFTER_STACK_MAPPING_ONE + page) {
            return false;
        }
    }
    return expected == LINUX_INITIAL_USER_PAGES;
}

static bool exit_observed(uint64_t process_generation)
{
    return runtime.active && runtime.state == LINUX_UNAME_PROCESS_RUNNING &&
        runtime.generation == process_generation &&
        runtime.address_space.state == PAGING_PROCESS_SPACE_ACTIVE &&
        (cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U)) ==
            runtime.address_space.root_physical_address &&
        transition_process(&runtime.state, LINUX_UNAME_PROCESS_EXITING) ==
            LINUX_UNAME_ABI_STATUS_OK;
}

static bool unmap_if_present(
    enum paging_process_mapping_kind kind,
    uint64_t address
)
{
    struct paging_translation translation;
    const enum paging_status status = paging_process_translate(
        &runtime.address_space, address, &translation);

    if (status == PAGING_STATUS_NOT_MAPPED) {
        return true;
    }
    return status == PAGING_STATUS_OK && translation.user &&
        paging_process_unmap_user_page(&runtime.address_space, kind, address) ==
            PAGING_STATUS_OK;
}

static enum linux_uname_abi_status release_runtime(enum linux_uname_abi_status result)
{
    bool cleanup_failed = false;

    cpu_interrupt_disable();
    if (runtime.address_space.state == PAGING_PROCESS_SPACE_ACTIVE &&
        paging_process_restore_kernel(&runtime.address_space) !=
            PAGING_STATUS_OK) {
        cleanup_failed = true;
    }
    if (runtime.state != LINUX_UNAME_PROCESS_STOPPING &&
        runtime.state != LINUX_UNAME_PROCESS_RELEASED &&
        transition_process(&runtime.state, LINUX_UNAME_PROCESS_STOPPING) !=
            LINUX_UNAME_ABI_STATUS_OK) {
        cleanup_failed = true;
    }
    if (!linux_syscall_resources_released() &&
        linux_syscall_disarm() != LINUX_SYSCALL_STATUS_OK) {
        cleanup_failed = true;
    }
    if (runtime.address_space.state != PAGING_PROCESS_SPACE_INVALID &&
        runtime.address_space.state != PAGING_PROCESS_SPACE_RELEASED) {
        if (!unmap_if_present(PAGING_PROCESS_MAPPING_LINUX_ANON,
                PAGING_LINUX_ANON_ADDRESS)) {
            cleanup_failed = true;
        }
        for (size_t remaining = PAGING_LINUX_HEAP_PAGES;
             remaining > 0U; --remaining) {
            if (!unmap_if_present(PAGING_PROCESS_MAPPING_LINUX_HEAP,
                    PAGING_LINUX_HEAP_BASE +
                        (remaining - 1U) * PAGING_PAGE_SIZE)) {
                cleanup_failed = true;
            }
        }
    }
    for (size_t mapped = runtime.stack_mapped_count; mapped > 0U; --mapped) {
        const size_t page = mapped - 1U;

        if (paging_process_unmap_user_page(&runtime.address_space,
                PAGING_PROCESS_MAPPING_LINUX_STACK,
                PAGING_LINUX_STACK_BASE + page * PAGING_PAGE_SIZE) !=
                PAGING_STATUS_OK) {
            cleanup_failed = true;
        } else {
            --runtime.stack_mapped_count;
        }
    }
    for (size_t mapped = runtime.image_mapped_count; mapped > 0U; --mapped) {
        const size_t page = mapped - 1U;

        if (paging_process_unmap_user_page(&runtime.address_space,
                PAGING_PROCESS_MAPPING_LINUX_UNAME_IMAGE,
                PAGING_LINUX_UNAME_IMAGE_BASE + page * PAGING_PAGE_SIZE) !=
                PAGING_STATUS_OK) {
            cleanup_failed = true;
        } else {
            --runtime.image_mapped_count;
        }
    }
    if (runtime.aliases.active &&
        paging_process_alias_set_restore(&runtime.address_space,
            &runtime.aliases) != PAGING_STATUS_OK) {
        cleanup_failed = true;
    }
    if (runtime.address_space.state != PAGING_PROCESS_SPACE_INVALID &&
        runtime.address_space.state != PAGING_PROCESS_SPACE_RELEASED &&
        paging_process_space_release(&runtime.address_space) !=
            PAGING_STATUS_OK) {
        cleanup_failed = true;
    }
    if (paging_process_table_failure_armed() &&
        !paging_process_table_failure_disarm()) {
        cleanup_failed = true;
    }
    if (runtime.anonymous_frame != 0U) {
        if (frame_release(runtime.anonymous_frame) != FRAME_STATUS_OK) {
            cleanup_failed = true;
        } else {
            runtime.anonymous_frame = 0U;
        }
    }
    for (size_t count = runtime.heap_frame_count; count > 0U; --count) {
        const size_t page = count - 1U;

        if (frame_release(runtime.heap_frames[page]) != FRAME_STATUS_OK) {
            cleanup_failed = true;
        } else {
            runtime.heap_frames[page] = 0U;
            --runtime.heap_frame_count;
        }
    }
    for (size_t count = runtime.stack_frame_count; count > 0U; --count) {
        const size_t page = count - 1U;

        if (frame_release(runtime.stack_frames[page]) != FRAME_STATUS_OK) {
            cleanup_failed = true;
        } else {
            runtime.stack_frames[page] = 0U;
            --runtime.stack_frame_count;
        }
    }
    for (size_t count = runtime.image_frame_count; count > 0U; --count) {
        const size_t page = count - 1U;

        if (frame_release(runtime.image_frames[page]) != FRAME_STATUS_OK) {
            cleanup_failed = true;
        } else {
            runtime.image_frames[page] = 0U;
            --runtime.image_frame_count;
        }
    }
    zero_bytes(runtime.elf_bytes, sizeof(runtime.elf_bytes));
    if (runtime.executable_state != LINUX_UNAME_EXECUTABLE_RELEASED &&
        transition_executable(&runtime.executable_state,
            LINUX_UNAME_EXECUTABLE_RELEASED) != LINUX_UNAME_ABI_STATUS_OK) {
        cleanup_failed = true;
    }
    if (runtime.stack_state != LINUX_UNAME_STACK_RELEASED &&
        transition_stack(&runtime.stack_state, LINUX_UNAME_STACK_RELEASED) !=
            LINUX_UNAME_ABI_STATUS_OK) {
        cleanup_failed = true;
    }
    if (runtime.file.active &&
        filesystem_linux_uname_read_close(&runtime.file) != FILESYSTEM_STATUS_OK) {
        cleanup_failed = true;
    }
    if (runtime.state == LINUX_UNAME_PROCESS_STOPPING &&
        transition_process(&runtime.state, LINUX_UNAME_PROCESS_RELEASED) !=
            LINUX_UNAME_ABI_STATUS_OK) {
        cleanup_failed = true;
    }
    runtime.active = false;
    proof_active = false;
    if (runtime.interrupts_were_enabled) {
        cpu_interrupt_enable();
    }
    return cleanup_failed ? LINUX_UNAME_ABI_STATUS_TEARDOWN : result;
}

static bool initial_stack_foundation_self_test(void)
{
    const uint64_t argv_two_address = PAGING_LINUX_STACK_END -
        sizeof(linux_argv_two);
    const uint64_t argv_one_address = argv_two_address -
        sizeof(linux_argv_one);
    const uint64_t argv_zero_address = argv_one_address -
        sizeof(linux_argv_zero);
    const uint64_t vector_address = (argv_zero_address & ~UINT64_C(15)) -
        LINUX_INITIAL_STACK_WORDS * sizeof(uint64_t);
    const uint64_t valid[LINUX_INITIAL_STACK_WORDS] = {
        UINT64_C(3), argv_zero_address, argv_one_address, argv_two_address,
        UINT64_C(0), UINT64_C(0), UINT64_C(6), PAGING_PAGE_SIZE,
        UINT64_C(0), UINT64_C(0)
    };
    static const size_t mutation_indices[7] = {
        0U, 1U, 4U, 5U, 6U, 7U, 8U
    };

    if (!initial_stack_layout_valid(valid, vector_address) ||
        initial_stack_layout_valid(valid, vector_address + 8U)) {
        return false;
    }
    for (size_t mutation = 0U;
         mutation < sizeof(mutation_indices) / sizeof(mutation_indices[0]);
         ++mutation) {
        uint64_t changed[LINUX_INITIAL_STACK_WORDS];

        for (size_t index = 0U; index < LINUX_INITIAL_STACK_WORDS; ++index) {
            changed[index] = valid[index];
        }
        changed[mutation_indices[mutation]] ^= UINT64_C(1);
        if (initial_stack_layout_valid(changed, vector_address)) {
            return false;
        }
    }
    return true;
}

bool linux_uname_image_uts_foundation_self_test(size_t *completed_tests)
{
    enum linux_uname_process_state state = LINUX_UNAME_PROCESS_CANDIDATE;
    enum linux_uname_executable_state executable = LINUX_UNAME_EXECUTABLE_CANDIDATE;
    enum linux_uname_stack_state stack = LINUX_UNAME_STACK_CANDIDATE;

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;
    if (phipia_linux_uname_elf64_self_test() !=
            LINUX_UNAME_ELF64_PARSER_ROBUSTNESS_CONTROLS) {
        return false;
    }
    *completed_tests = LINUX_UNAME_ELF64_PARSER_ROBUSTNESS_CONTROLS;
    if ((PAGING_LINUX_UNAME_IMAGE_BASE & (PAGING_PAGE_SIZE - 1U)) != 0U ||
        (PAGING_LINUX_STACK_GUARD & (PAGING_PAGE_SIZE - 1U)) != 0U ||
        (PAGING_LINUX_STACK_END & UINT64_C(15)) != 0U ||
        !canonical_user(PAGING_LINUX_UNAME_IMAGE_BASE) ||
        !canonical_user(PAGING_LINUX_STACK_END - 1U) ||
        PAGING_LINUX_UNAME_IMAGE_END > PAGING_LINUX_HEAP_BASE ||
        PAGING_LINUX_HEAP_BASE +
            PAGING_LINUX_HEAP_PAGES * PAGING_PAGE_SIZE >
                PAGING_LINUX_ANON_ADDRESS ||
        PAGING_LINUX_ANON_ADDRESS + PAGING_PAGE_SIZE >
            PAGING_LINUX_STACK_GUARD ||
        LINUX_ARGUMENT_BYTES != 17U || LINUX_INITIAL_STACK_WORDS != 10U ||
        !initial_stack_foundation_self_test()) {
        return false;
    }
    *completed_tests += LINUX_UNAME_ABI_STACK_FOUNDATION_CONTROLS;
    if (!linux_syscall_enosys_self_test() ||
        !linux_syscall_uname_semantic_self_test()) {
        return false;
    }
    *completed_tests += LINUX_UNAME_SYSCALL_SEMANTIC_CONTROLS;
    if (transition_process(&state, LINUX_UNAME_PROCESS_BUILDING) !=
            LINUX_UNAME_ABI_STATUS_OK ||
        transition_process(&state, LINUX_UNAME_PROCESS_INSTALLED) !=
            LINUX_UNAME_ABI_STATUS_OK ||
        transition_process(&state, LINUX_UNAME_PROCESS_RUNNING) !=
            LINUX_UNAME_ABI_STATUS_OK ||
        transition_process(&state, LINUX_UNAME_PROCESS_EXITING) !=
            LINUX_UNAME_ABI_STATUS_OK ||
        transition_process(&state, LINUX_UNAME_PROCESS_STOPPING) !=
            LINUX_UNAME_ABI_STATUS_OK ||
        transition_process(&state, LINUX_UNAME_PROCESS_RELEASED) !=
            LINUX_UNAME_ABI_STATUS_OK ||
        transition_executable(&executable, LINUX_UNAME_EXECUTABLE_VALIDATED) !=
            LINUX_UNAME_ABI_STATUS_OK ||
        transition_executable(&executable, LINUX_UNAME_EXECUTABLE_INSTALLED) !=
            LINUX_UNAME_ABI_STATUS_OK ||
        transition_executable(&executable, LINUX_UNAME_EXECUTABLE_RELEASED) !=
            LINUX_UNAME_ABI_STATUS_OK ||
        transition_stack(&stack, LINUX_UNAME_STACK_BUILDING) !=
            LINUX_UNAME_ABI_STATUS_OK ||
        transition_stack(&stack, LINUX_UNAME_STACK_INSTALLED) !=
            LINUX_UNAME_ABI_STATUS_OK ||
        transition_stack(&stack, LINUX_UNAME_STACK_RELEASED) !=
            LINUX_UNAME_ABI_STATUS_OK) {
        return false;
    }
    state = LINUX_UNAME_PROCESS_BUILDING;
    if (transition_process(&state, LINUX_UNAME_PROCESS_BUILDING) !=
            LINUX_UNAME_ABI_STATUS_TRANSITION_REPEATED ||
        transition_process(&state, LINUX_UNAME_PROCESS_CANDIDATE) !=
            LINUX_UNAME_ABI_STATUS_TRANSITION_REVERSED ||
        transition_executable(&executable, LINUX_UNAME_EXECUTABLE_RELEASED) !=
            LINUX_UNAME_ABI_STATUS_TRANSITION_REPEATED ||
        transition_executable(&executable, LINUX_UNAME_EXECUTABLE_CANDIDATE) !=
            LINUX_UNAME_ABI_STATUS_TRANSITION_REVERSED ||
        transition_stack(&stack, LINUX_UNAME_STACK_RELEASED) !=
            LINUX_UNAME_ABI_STATUS_TRANSITION_REPEATED ||
        transition_stack(&stack, LINUX_UNAME_STACK_CANDIDATE) !=
            LINUX_UNAME_ABI_STATUS_TRANSITION_REVERSED ||
        !linux_uname_abi_resources_released()) {
        return false;
    }
    *completed_tests = LINUX_UNAME_ABI_IMAGE_UTS_FOUNDATION_CONTROLS;
    return true;
}

static enum linux_uname_abi_status allocate_runtime_frames(
    enum linux_failure_point failure_point
)
{
    for (size_t page = 0U; page < PAGING_LINUX_UNAME_IMAGE_PAGES; ++page) {
        if (frame_allocate(&runtime.image_frames[page]) != FRAME_STATUS_OK ||
            !initialize_frame(runtime.image_frames[page])) {
            return LINUX_UNAME_ABI_STATUS_FRAME_ALLOCATION;
        }
        ++runtime.image_frame_count;
        if (failure_point == LINUX_FAILURE_AFTER_IMAGE_FRAME_ONE + page) {
            return LINUX_UNAME_ABI_STATUS_FRAME_ALLOCATION;
        }
    }
    for (size_t page = 0U; page < PAGING_LINUX_STACK_PAGES; ++page) {
        if (frame_allocate(&runtime.stack_frames[page]) != FRAME_STATUS_OK ||
            !initialize_frame(runtime.stack_frames[page])) {
            return LINUX_UNAME_ABI_STATUS_FRAME_ALLOCATION;
        }
        ++runtime.stack_frame_count;
        if (failure_point == LINUX_FAILURE_AFTER_STACK_FRAME_ONE + page) {
            return LINUX_UNAME_ABI_STATUS_FRAME_ALLOCATION;
        }
    }
    for (size_t page = 0U; page < PAGING_LINUX_HEAP_PAGES; ++page) {
        if (frame_allocate(&runtime.heap_frames[page]) != FRAME_STATUS_OK ||
            !initialize_frame(runtime.heap_frames[page])) {
            return LINUX_UNAME_ABI_STATUS_FRAME_ALLOCATION;
        }
        ++runtime.heap_frame_count;
        if (failure_point == LINUX_FAILURE_AFTER_HEAP_FRAME_ONE + page) {
            return LINUX_UNAME_ABI_STATUS_FRAME_ALLOCATION;
        }
    }
    if (frame_allocate(&runtime.anonymous_frame) != FRAME_STATUS_OK ||
        !initialize_frame(runtime.anonymous_frame)) {
        return LINUX_UNAME_ABI_STATUS_FRAME_ALLOCATION;
    }
    if (failure_point == LINUX_FAILURE_AFTER_ANONYMOUS_FRAME) {
        return LINUX_UNAME_ABI_STATUS_FRAME_ALLOCATION;
    }
    return LINUX_UNAME_ABI_STATUS_OK;
}

static enum linux_uname_abi_status linux_attempt(
    struct linux_uname_abi_proof_result *result,
    enum linux_failure_point failure_point,
    size_t table_failure_ordinal,
    bool *table_failure_observed
)
{
    struct linux_resource_census before;
    struct linux_resource_census after;
    struct linux_syscall_context syscall_context;
    uint64_t executable_aliases[PAGING_LINUX_UNAME_IMAGE_EXECUTE_PAGES];
    uint32_t file_bytes = 0U;
    uint32_t file_clusters = 0U;
    enum linux_uname_abi_status status = LINUX_UNAME_ABI_STATUS_OK;
    enum filesystem_status filesystem_status;

    if (result == NULL || table_failure_observed == NULL ||
        failure_point >= LINUX_FAILURE_POINT_COUNT ||
        table_failure_ordinal > LINUX_PAGING_FAILURE_CEILING ||
        (failure_point != LINUX_FAILURE_NONE &&
            table_failure_ordinal != 0U)) {
        return LINUX_UNAME_ABI_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(result, sizeof(*result));
    *table_failure_observed = false;
    if (proof_active || !linux_uname_abi_resources_released()) {
        return LINUX_UNAME_ABI_STATUS_BUSY;
    }
    capture_census(&before);
    zero_bytes(&runtime, sizeof(runtime));
    runtime.state = LINUX_UNAME_PROCESS_CANDIDATE;
    runtime.executable_state = LINUX_UNAME_EXECUTABLE_CANDIDATE;
    runtime.stack_state = LINUX_UNAME_STACK_CANDIDATE;
    runtime.generation = next_generation++;
    if (next_generation == 0U) {
        next_generation = 1U;
    }
    runtime.interrupts_were_enabled = before.interrupts_enabled;
    runtime.active = true;
    proof_active = true;
    if (transition_process(&runtime.state, LINUX_UNAME_PROCESS_BUILDING) !=
            LINUX_UNAME_ABI_STATUS_OK) {
        status = LINUX_UNAME_ABI_STATUS_TRANSITION_INVALID;
        goto cleanup;
    }
    filesystem_status = filesystem_linux_uname_read_open(&runtime.file,
        runtime.elf_bytes, sizeof(runtime.elf_bytes),
        filesystem_failure_boundary(failure_point));
    if (filesystem_status == FILESYSTEM_STATUS_ABSENT) {
        status = LINUX_UNAME_ABI_STATUS_ABSENT;
        goto cleanup;
    }
    bool fat32_file = runtime.file.fat32;
    uint32_t fat32_clusters =
        (LINUX_UNAME_ELF64_FILE_BYTES + FAT32_CLUSTER_BYTES - 1U) /
            FAT32_CLUSTER_BYTES;
    if (filesystem_status != FILESYSTEM_STATUS_OK ||
        !runtime.file.cpu_owned ||
        runtime.file.file_bytes != LINUX_UNAME_ELF64_FILE_BYTES ||
        (fat32_file && runtime.file.cluster_count != fat32_clusters) ||
        (!fat32_file &&
            runtime.file.cluster_count != LINUX_UNAME_FAT16_FILE_CLUSTERS) ||
        (!fat32_file &&
            (runtime.file.read_count != 3U + runtime.file.cluster_count ||
             runtime.file.msix_completion_count !=
                3U + runtime.file.cluster_count)) ||
        (fat32_file && (runtime.file.read_count == 0U ||
            runtime.file.msix_completion_count != runtime.file.read_count))) {
        if (filesystem_status != FILESYSTEM_STATUS_CONTROLLED_FAILURE) {
            console_write("Phipia: Linux uname filesystem unexpected ");
            console_write(filesystem_status_string(filesystem_status));
            console_write(" boundary ");
            console_write_u64(filesystem_failure_boundary(failure_point));
            console_write(" reads ");
            console_write_u64(runtime.file.read_count);
            console_write(" completions ");
            console_write_u64(runtime.file.msix_completion_count);
            console_write(" bytes ");
            console_write_u64(runtime.file.file_bytes);
            console_write(" clusters ");
            console_write_u64(runtime.file.cluster_count);
            console_putc('\n');
        }
        status = LINUX_UNAME_ABI_STATUS_FILESYSTEM;
        goto cleanup;
    }
    file_bytes = runtime.file.file_bytes;
    file_clusters = runtime.file.cluster_count;
    if (failure_point == LINUX_FAILURE_AFTER_FILESYSTEM) {
        status = failure_status(failure_point);
        goto cleanup;
    }
    if (phipia_linux_uname_elf64_parse(runtime.elf_bytes,
            sizeof(runtime.elf_bytes), &runtime.image) !=
            LINUX_ELF64_STATUS_OK || !validated_placement(&runtime.image)) {
        status = LINUX_UNAME_ABI_STATUS_ELF;
        goto cleanup;
    }
    if (transition_executable(&runtime.executable_state,
            LINUX_UNAME_EXECUTABLE_VALIDATED) != LINUX_UNAME_ABI_STATUS_OK) {
        status = LINUX_UNAME_ABI_STATUS_TRANSITION_INVALID;
        goto cleanup;
    }
    if (failure_point == LINUX_FAILURE_AFTER_PARSE) {
        status = failure_status(failure_point);
        goto cleanup;
    }
    status = allocate_runtime_frames(failure_point);
    if (status != LINUX_UNAME_ABI_STATUS_OK) {
        goto cleanup;
    }
    if (!install_image()) {
        status = LINUX_UNAME_ABI_STATUS_ELF_INSTALL;
        goto cleanup;
    }
    if (transition_executable(&runtime.executable_state,
            LINUX_UNAME_EXECUTABLE_INSTALLED) != LINUX_UNAME_ABI_STATUS_OK) {
        status = LINUX_UNAME_ABI_STATUS_TRANSITION_INVALID;
        goto cleanup;
    }
    if (failure_point == LINUX_FAILURE_AFTER_IMAGE_INSTALL) {
        status = failure_status(failure_point);
        goto cleanup;
    }
    if (transition_stack(&runtime.stack_state, LINUX_UNAME_STACK_BUILDING) !=
            LINUX_UNAME_ABI_STATUS_OK) {
        status = LINUX_UNAME_ABI_STATUS_TRANSITION_INVALID;
        goto cleanup;
    }
    if (!build_initial_stack(failure_point)) {
        status = LINUX_UNAME_ABI_STATUS_STACK;
        goto cleanup;
    }
    if (transition_stack(&runtime.stack_state, LINUX_UNAME_STACK_INSTALLED) !=
            LINUX_UNAME_ABI_STATUS_OK) {
        status = LINUX_UNAME_ABI_STATUS_TRANSITION_INVALID;
        goto cleanup;
    }
    if (failure_point == LINUX_FAILURE_AFTER_STACK_INSTALL) {
        status = failure_status(failure_point);
        goto cleanup;
    }
    if (table_failure_ordinal != 0U &&
        !paging_process_table_failure_arm(table_failure_ordinal)) {
        status = LINUX_UNAME_ABI_STATUS_ROBUSTNESS;
        goto cleanup;
    }
    if (paging_process_space_build(&runtime.address_space) !=
            PAGING_STATUS_OK) {
        status = LINUX_UNAME_ABI_STATUS_ADDRESS_SPACE;
        goto cleanup;
    }
    if (failure_point == LINUX_FAILURE_AFTER_ADDRESS_SPACE) {
        status = failure_status(failure_point);
        goto cleanup;
    }
    cpu_interrupt_disable();
    for (size_t page = 0U; page < PAGING_LINUX_UNAME_IMAGE_EXECUTE_PAGES; ++page) {
        executable_aliases[page] = runtime.image_frames[
            page + PAGING_LINUX_UNAME_IMAGE_EXECUTE_FIRST_PAGE];
    }
    if (paging_process_alias_set_narrow(&runtime.address_space,
            executable_aliases, PAGING_LINUX_UNAME_IMAGE_EXECUTE_PAGES,
            &runtime.aliases) != PAGING_STATUS_OK) {
        status = LINUX_UNAME_ABI_STATUS_ALIAS;
        goto cleanup;
    }
    if (failure_point == LINUX_FAILURE_AFTER_ALIAS_NARROW) {
        status = failure_status(failure_point);
        goto cleanup;
    }
    if (!map_initial_pages(failure_point)) {
        status = LINUX_UNAME_ABI_STATUS_MAPPING;
        goto cleanup;
    }
    if (paging_process_validate_linux(&runtime.address_space,
            runtime.expected_pages, LINUX_INITIAL_USER_PAGES) !=
            PAGING_STATUS_OK) {
        status = LINUX_UNAME_ABI_STATUS_PERMISSION_AUDIT;
        goto cleanup;
    }
    if (failure_point == LINUX_FAILURE_AFTER_PERMISSION_AUDIT) {
        status = failure_status(failure_point);
        goto cleanup;
    }
    zero_bytes(&syscall_context, sizeof(syscall_context));
    syscall_context.profile = LINUX_SYSCALL_PROFILE_UNAME;
    syscall_context.address_space = &runtime.address_space;
    syscall_context.process_generation = runtime.generation;
    syscall_context.executable_start = LINUX_UNAME_ABI_EXECUTABLE_START;
    syscall_context.executable_end = LINUX_UNAME_ABI_EXECUTABLE_END;
    syscall_context.stack_start = PAGING_LINUX_STACK_BASE;
    syscall_context.stack_end = PAGING_LINUX_STACK_END;
    syscall_context.fs_address = LINUX_UNAME_ABI_FS_ADDRESS;
    syscall_context.tid_address = LINUX_UNAME_ABI_TID_ADDRESS;
    for (size_t page = 0U; page < PAGING_LINUX_HEAP_PAGES; ++page) {
        syscall_context.heap_frames[page] = runtime.heap_frames[page];
    }
    for (size_t page = 0U; page < PAGING_LINUX_STACK_PAGES; ++page) {
        syscall_context.stack_frames[page] = runtime.stack_frames[page];
    }
    syscall_context.anonymous_frame = runtime.anonymous_frame;
    syscall_context.exit_observed = exit_observed;
    syscall_context.failure_before_ordinal =
        failure_before_ordinal(failure_point);
    syscall_context.failure_after_ordinal = failure_after_ordinal(failure_point);
    syscall_context.controlled_run = failure_point != LINUX_FAILURE_NONE ||
        table_failure_ordinal != 0U;
    syscall_context.publish_stdout = failure_point == LINUX_FAILURE_NONE &&
        table_failure_ordinal == 0U;
    if (linux_syscall_arm(&syscall_context) != LINUX_SYSCALL_STATUS_OK ||
        linux_syscall_validate_armed() != LINUX_SYSCALL_STATUS_OK) {
        status = LINUX_UNAME_ABI_STATUS_SYSCALL_CPU;
        goto cleanup;
    }
    {
        size_t copyout_controls = 0U;

        if (!linux_syscall_uname_copyout_self_test(&copyout_controls) ||
            copyout_controls != LINUX_UNAME_COPYOUT_CONTROLS) {
            status = LINUX_UNAME_ABI_STATUS_ROBUSTNESS;
            goto cleanup;
        }
    }
    if (failure_point == LINUX_FAILURE_AFTER_SYSCALL_ARM) {
        status = failure_status(failure_point);
        goto cleanup;
    }
    if (transition_process(&runtime.state, LINUX_UNAME_PROCESS_INSTALLED) !=
            LINUX_UNAME_ABI_STATUS_OK ||
        paging_process_activate(&runtime.address_space) != PAGING_STATUS_OK ||
        transition_process(&runtime.state, LINUX_UNAME_PROCESS_RUNNING) !=
            LINUX_UNAME_ABI_STATUS_OK) {
        status = LINUX_UNAME_ABI_STATUS_ENTRY;
        goto cleanup;
    }
    if (failure_point == LINUX_FAILURE_AFTER_CR3_ACTIVATION) {
        status = failure_status(failure_point);
        goto cleanup;
    }
    linux_process_enter_user(runtime.image.entry,
        runtime.initial_stack_pointer);
    runtime.syscall_result = linux_syscall_get_result();
    if (table_failure_ordinal != 0U &&
        paging_process_table_failure_armed()) {
        size_t allocation_count = 0U;
        bool observed = false;

        if (!paging_process_table_failure_result(&allocation_count,
                &observed) ||
            (observed && allocation_count != table_failure_ordinal)) {
            status = LINUX_UNAME_ABI_STATUS_ROBUSTNESS;
            goto cleanup;
        }
        if (observed && runtime.syscall_result.status !=
                LINUX_SYSCALL_STATUS_OK) {
            *table_failure_observed = true;
            status = LINUX_UNAME_ABI_STATUS_MAPPING;
            goto cleanup;
        }
    }
    if (failure_in_range(failure_point, LINUX_FAILURE_BEFORE_SYSCALL_ONE,
            LINUX_FAILURE_AFTER_SYSCALL_SIX)) {
        if (runtime.syscall_result.status ==
                LINUX_SYSCALL_STATUS_CONTROLLED_FAILURE &&
            runtime.syscall_result.controlled_failure_observed &&
            !linux_process_boundary_active()) {
            status = failure_status(failure_point);
            goto cleanup;
        }
        status = LINUX_UNAME_ABI_STATUS_ROBUSTNESS;
        goto cleanup;
    }
    if (linux_process_boundary_active() ||
        runtime.address_space.state != PAGING_PROCESS_SPACE_INSTALLED ||
        (cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U)) !=
            before.paging.root_physical_address ||
        runtime.state != LINUX_UNAME_PROCESS_EXITING ||
        runtime.syscall_result.status != LINUX_SYSCALL_STATUS_OK ||
        runtime.syscall_result.cpu_state != LINUX_SYSCALL_CPU_RETURNED ||
        runtime.syscall_result.syscall_count !=
            LINUX_UNAME_SYSCALL_EXPECTED_CALLS ||
        runtime.syscall_result.distinct_syscalls !=
            LINUX_UNAME_SYSCALL_ALLOWLIST_COUNT ||
        runtime.syscall_result.stdout_bytes !=
            LINUX_UNAME_SYSCALL_STDOUT_BYTES ||
        !runtime.syscall_result.uts_copy_valid ||
        !runtime.syscall_result.stdout_valid ||
        !runtime.syscall_result.exit_zero ||
        !runtime.syscall_result.real_syscall_instruction ||
        !runtime.syscall_result.process_authenticated ||
        !runtime.syscall_result.cr3_authenticated) {
        status = LINUX_UNAME_ABI_STATUS_EXIT;
    }

cleanup:
    if (table_failure_ordinal != 0U &&
        paging_process_table_failure_armed()) {
        size_t allocation_count = 0U;
        bool observed = false;

        if (!paging_process_table_failure_result(&allocation_count,
                &observed)) {
            status = LINUX_UNAME_ABI_STATUS_ROBUSTNESS;
        } else {
            *table_failure_observed = observed;
        }
    }
    status = release_runtime(status);
    if (status == LINUX_UNAME_ABI_STATUS_TEARDOWN ||
        !linux_uname_abi_resources_released() || paging_verify() != PAGING_STATUS_OK) {
        zero_bytes(result, sizeof(*result));
        return LINUX_UNAME_ABI_STATUS_TEARDOWN;
    }
    capture_census(&after);
    if (!census_equal(&before, &after)) {
        zero_bytes(result, sizeof(*result));
        return LINUX_UNAME_ABI_STATUS_RESOURCE_CENSUS;
    }
    if (table_failure_ordinal != 0U && !*table_failure_observed) {
        zero_bytes(result, sizeof(*result));
        return status == LINUX_UNAME_ABI_STATUS_OK ?
            LINUX_UNAME_ABI_STATUS_PREREQUISITE : LINUX_UNAME_ABI_STATUS_ROBUSTNESS;
    }
    if (status != LINUX_UNAME_ABI_STATUS_OK) {
        zero_bytes(result, sizeof(*result));
        return status;
    }
    if (failure_point != LINUX_FAILURE_NONE) {
        zero_bytes(result, sizeof(*result));
        return LINUX_UNAME_ABI_STATUS_ROBUSTNESS;
    }
    runtime.syscall_result = linux_syscall_get_result();
    if (!runtime.syscall_result.cpu_disarmed) {
        return LINUX_UNAME_ABI_STATUS_TEARDOWN;
    }
    result->file_bytes = file_bytes;
    result->program_headers = LINUX_UNAME_ELF64_PROGRAM_HEADERS;
    result->load_segments = LINUX_UNAME_ELF64_LOAD_SEGMENTS;
    result->file_clusters = file_clusters;
    result->stdout_bytes = runtime.syscall_result.stdout_bytes;
    result->syscall_count = runtime.syscall_result.syscall_count;
    result->distinct_syscalls = runtime.syscall_result.distinct_syscalls;
    result->exit_status = runtime.syscall_result.exit_status;
    result->robustness_tests = LINUX_UNAME_ABI_CONTROLLED_ROBUSTNESS_TESTS;
    result->ring_three = true;
    result->private_address_space = true;
    result->real_syscall_instruction =
        runtime.syscall_result.real_syscall_instruction;
    result->uts_copy_valid = runtime.syscall_result.uts_copy_valid;
    result->stdout_valid = runtime.syscall_result.stdout_valid;
    result->exit_zero = runtime.syscall_result.exit_zero;
    result->unknown_enosys = linux_syscall_enosys_self_test();
    result->write_xor_execute = true;
    result->kernel_cr3_restored = after.cr3 == before.cr3;
    result->teardown_complete = true;
    result->resource_census_equal = true;
    installed_result = *result;
    return LINUX_UNAME_ABI_STATUS_OK;
}

enum linux_uname_abi_status linux_uname_abi_installed_prove(
    struct linux_uname_abi_proof_result *result
)
{
    struct linux_uname_abi_proof_result controlled_result;
    bool table_failure_exhausted = false;

    if (result == NULL) {
        return LINUX_UNAME_ABI_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(result, sizeof(*result));
    zero_bytes(&installed_result, sizeof(installed_result));
    for (enum linux_failure_point point = LINUX_FAILURE_AFTER_FILESYSTEM;
         point < LINUX_FAILURE_POINT_COUNT;
         point = (enum linux_failure_point)(point + 1)) {
        enum linux_uname_abi_status status;
        bool table_failure_observed = false;

        zero_bytes(&controlled_result, sizeof(controlled_result));
        status = linux_attempt(&controlled_result, point, 0U,
            &table_failure_observed);
        if (point == LINUX_FAILURE_AFTER_FILESYSTEM &&
            status == LINUX_UNAME_ABI_STATUS_ABSENT) {
            return status;
        }
        if (status != failure_status(point) ||
            !proof_result_zero(&controlled_result) ||
            !linux_uname_abi_resources_released() ||
            !proof_result_zero(&installed_result)) {
            report_control_failure("boundary", (uint64_t)point, status,
                failure_status(point), table_failure_observed);
            zero_bytes(result, sizeof(*result));
            return LINUX_UNAME_ABI_STATUS_ROBUSTNESS;
        }
    }
    for (size_t ordinal = 1U; ordinal <= LINUX_PAGING_FAILURE_CEILING;
         ++ordinal) {
        enum linux_uname_abi_status status;
        bool table_failure_observed = false;

        zero_bytes(&controlled_result, sizeof(controlled_result));
        status = linux_attempt(&controlled_result, LINUX_FAILURE_NONE,
            ordinal, &table_failure_observed);
        if (status == LINUX_UNAME_ABI_STATUS_PREREQUISITE &&
            !table_failure_observed) {
            table_failure_exhausted = true;
            break;
        }
        if (!table_failure_observed ||
            (status != LINUX_UNAME_ABI_STATUS_ADDRESS_SPACE &&
                status != LINUX_UNAME_ABI_STATUS_ALIAS &&
                status != LINUX_UNAME_ABI_STATUS_MAPPING) ||
            !proof_result_zero(&controlled_result) ||
            !linux_uname_abi_resources_released() ||
            !proof_result_zero(&installed_result)) {
            report_control_failure("page-table", ordinal, status,
                LINUX_UNAME_ABI_STATUS_MAPPING, table_failure_observed);
            zero_bytes(result, sizeof(*result));
            return LINUX_UNAME_ABI_STATUS_ROBUSTNESS;
        }
    }
    if (!table_failure_exhausted) {
        report_control_failure("page-table ceiling",
            LINUX_PAGING_FAILURE_CEILING, LINUX_UNAME_ABI_STATUS_ROBUSTNESS,
            LINUX_UNAME_ABI_STATUS_PREREQUISITE, false);
        zero_bytes(result, sizeof(*result));
        return LINUX_UNAME_ABI_STATUS_ROBUSTNESS;
    }
    {
        bool table_failure_observed = false;

        return linux_attempt(result, LINUX_FAILURE_NONE, 0U,
            &table_failure_observed);
    }
}

enum linux_uname_abi_status linux_uname_abi_launch(
    struct linux_uname_abi_proof_result *result
)
{
    bool table_failure_observed = false;
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    enum linux_uname_abi_status status;

    if (interrupts_were_enabled) {
        cpu_interrupt_disable();
    }
    status = linux_attempt(result, LINUX_FAILURE_NONE, 0U,
        &table_failure_observed);
    if (interrupts_were_enabled) {
        cpu_interrupt_enable();
    }
    return status;
}

struct linux_uname_abi_proof_result linux_uname_abi_get_proof_result(void)
{
    return installed_result;
}

bool linux_uname_abi_resources_released(void)
{
    const struct paging_state paging = paging_get_state();

    return !proof_active && !runtime.active &&
        paging_process_resources_released() &&
        filesystem_resources_released() && linux_syscall_resources_released() &&
        !linux_process_boundary_active() &&
        (!paging.active ||
            (cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U)) ==
                paging.root_physical_address);
}

const char *linux_uname_abi_status_string(enum linux_uname_abi_status status)
{
    static const char *const messages[LINUX_UNAME_ABI_STATUS_COUNT] = {
        "ok",
        "Linux ABI BusyBox fixture is absent",
        "null Linux ABI argument",
        "Linux ABI proof is already active",
        "Linux ABI prerequisite is incomplete",
        "BusyBox filesystem read failed",
        "Rust BusyBox ELF parser refused the image",
        "BusyBox ELF installation failed",
        "Linux initial-stack construction failed",
        "Linux private-frame allocation failed",
        "Linux private address-space construction failed",
        "Linux executable alias narrowing failed",
        "Linux user mapping installation failed",
        "Linux effective-permission audit failed",
        "Linux SYSCALL CPU contract failed",
        "BusyBox Ring 3 entry failed",
        "BusyBox exit was not authenticated",
        "controlled Linux syscall failure observed",
        "Linux process transition repeated",
        "Linux process transition reversed",
        "Linux process transition invalid",
        "Linux process teardown failed",
        "Linux process resource census differs",
        "Linux ABI controlled robustness failed"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        LINUX_UNAME_ABI_STATUS_COUNT,
        "Linux ABI status table cardinality changed");

    if (status >= LINUX_UNAME_ABI_STATUS_COUNT) {
        return "unknown Linux ABI status";
    }
    return messages[status];
}
