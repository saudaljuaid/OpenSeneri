/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Cooperative round-robin scheduling for bounded user processes. Each process
 * owns a private page hierarchy, image, stack, generation, and saved register
 * set. CPL3 interrupts stay masked. A process fault ends that process without
 * ending the kernel or its peers.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/cpu.h>
#include <phipia/dma.h>
#include <phipia/elf64.h>
#include <phipia/interrupt_vector.h>
#include <phipia/interrupts.h>
#include <phipia/memory.h>
#include <phipia/msix.h>
#include <phipia/multiprocess.h>
#include <phipia/paging.h>
#include <phipia/pci_resource.h>
#include <phipia/process.h>

#define MULTIPROCESS_SENTINEL_BYTES 32U
#define MULTIPROCESS_USER_RFLAGS UINT64_C(2)

/*
 * Intel SDM volume 1 section 3.4.3: CF, PF, AF, ZF, SF and OF are the status
 * flags an ordinary instruction is entitled to change. A program that counts
 * and compares changes all of them, so requiring RFLAGS to still equal the
 * value it was entered with would reject every real yield. Everything outside
 * this mask is still required to be exactly what the kernel set: bit 1 stays
 * set, and IF, TF, DF, IOPL, NT, RF, VM and AC stay clear, which is what keeps
 * a user process from re-enabling interrupts, single-stepping, or running the
 * kernel's string operations backwards on the way back in.
 */
#define MULTIPROCESS_ARITHMETIC_FLAGS UINT64_C(0x08D5)

/*
 * What the program actually chose. RF is the processor's own note about the
 * trap rather than anything the program did (see cpu.h), so it is removed
 * before a register set is authenticated and before one is saved: a context
 * this kernel resumes carries exactly the flags it decided on, never a bit the
 * hardware happened to leave behind.
 */
static uint64_t authenticated_user_rflags(uint64_t rflags)
{
    return rflags & ~CPU_RFLAGS_PROCESSOR_BOOKKEEPING;
}

/*
 * Intel SDM volume 3A section 4.7: a user write to an absent page reports
 * P=0 W=1 U=1. That is the only error code the deliberate guard-page store may
 * produce, and it is distinct from every supervisor fault the kernel proves
 * elsewhere.
 */
#define MULTIPROCESS_FAULT_ERROR_CODE UINT64_C(0x06)
#define MULTIPROCESS_PAGE_FAULT_VECTOR UINT64_C(14)

/* Which round the contained-fault schedule tells one process to fault on. */
#define MULTIPROCESS_CONTAINMENT_FAULT_ROUND 3U
#define MULTIPROCESS_CONTAINMENT_FAULT_INDEX 1U

_Static_assert(MULTIPROCESS_MAX_PROCESSES >= 2U,
    "a round robin needs at least two processes to interleave");
_Static_assert(MULTIPROCESS_CONTAINMENT_FAULT_INDEX <
    MULTIPROCESS_MAX_PROCESSES,
    "the contained fault names a process that does not exist");
_Static_assert(MULTIPROCESS_CONTAINMENT_FAULT_ROUND <= MULTIPROCESS_ROUNDS,
    "the contained fault names a round the program never reaches");
_Static_assert(ELF64_LOAD_ADDRESS == PAGING_PROCESS_IMAGE_ADDRESS,
    "ELF and paging image placements disagree");
_Static_assert(ELF64_PROOF_VECTOR == INTERRUPT_PROCESS_PROOF_VECTOR,
    "ELF and interrupt proof vectors disagree");
_Static_assert(MULTIPROCESS_ELF_CODE_OFFSET + MULTIPROCESS_ELF_CODE_BYTES ==
    MULTIPROCESS_ELF_FILE_BYTES,
    "the multiprocess instructions do not fill the executable");
_Static_assert(MULTIPROCESS_ELF_FILE_BYTES <= PAGING_PAGE_SIZE,
    "the multiprocess executable does not fit one image page");
/*
 * Both published words are read back through the identity map of the last
 * stack frame, so both have to be inside that one page - not merely inside the
 * stack. read_user_word's offset arithmetic depends on it.
 */
_Static_assert(MULTIPROCESS_IDENTITY_ADDRESS >= PAGING_PROCESS_STACK_BASE +
    (uint64_t)(PAGING_PROCESS_STACK_PAGES - 1U) * PAGING_PAGE_SIZE,
    "the published identity word is outside the last private stack page");
_Static_assert(MULTIPROCESS_PROGRESS_ADDRESS >= MULTIPROCESS_IDENTITY_ADDRESS,
    "the published words are in an unexpected order");
_Static_assert(MULTIPROCESS_PROGRESS_ADDRESS + 8U <= PAGING_PROCESS_STACK_END,
    "the published progress word runs off the end of the private stack");
_Static_assert(sizeof(struct elf64_validated_image) == 88U,
    "Rust/C ELF64 validated-image ABI changed");

/*
 * The exact executable, carried whole rather than assembled here. Two other
 * records hold the same bytes: src/rust/elf64.rs pins the instruction stream
 * and the structural subset it must sit in, and tools/multiprocess_image.py
 * rebuilds the file from its instruction table. make verify compares this
 * table against that reconstruction, so all three have to agree.
 */
static const uint8_t multiprocess_image[MULTIPROCESS_ELF_FILE_BYTES] = {
    0x7F, 0x45, 0x4C, 0x46, 0x02, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x3E, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x78, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
    0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x38, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x31, 0xC9, 0x48, 0x89, 0xE5, 0x48, 0xFF, 0xC1,
    0x48, 0x89, 0x4D, 0xF8, 0x48, 0x89, 0x7D, 0xF0,
    0x48, 0x39, 0xD1, 0x74, 0x1B, 0xB8, 0x4D, 0x50,
    0x41, 0x53, 0x48, 0x89, 0xCB, 0xCD, 0x81, 0x48,
    0x39, 0xF1, 0x72, 0xE1, 0xB8, 0x58, 0x50, 0x41,
    0x53, 0x48, 0x89, 0xCB, 0xCD, 0x81, 0x0F, 0x0B,
    0x48, 0xB8, 0x00, 0x00, 0x20, 0x00, 0x00, 0x40,
    0x00, 0x00, 0x48, 0xC7, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x0F, 0x0B, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4,
    0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4,
    0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4,
    0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4,
    0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4,
    0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4,
    0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4,
    0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4,
    0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4
};

struct multiprocess_census {
    struct frame_allocator_stats frames;
    struct paging_state paging;
    struct dma_state dma;
    struct pci_resource_state pci;
    struct interrupt_vector_state vectors;
    struct msix_state msix;
    uint64_t cr3;
    bool paging_process_released;
    bool proof_gate_released;
    bool user_boundary_inactive;
    bool interrupts_enabled;
};

struct multiprocess_process {
    uint64_t generation;
    enum multiprocess_process_state state;
    uint64_t identity;
    uint32_t rounds;
    uint32_t fault_round;
    uint32_t observed_rounds;
    enum multiprocess_trap last_trap;
    uintptr_t image_frame;
    uintptr_t stack_frames[PAGING_PROCESS_STACK_PAGES];
    size_t stack_frame_count;
    size_t stack_mapped_count;
    struct paging_process_space address_space;
    struct paging_process_image_alias alias;
    struct process_user_context context;
    uint64_t published_progress;
    uint64_t published_identity;
    bool image_mapped;
    bool built;
};

/*
 * Where a controlled run is told to stop. Every point leaves a different mix
 * of frames, hierarchies, aliases and mappings owned, and the sweep in
 * multiprocess_prove requires each of them to return the census to exactly
 * what it was - with one complete process already installed alongside, which
 * is the part the single-process teardown never had to survive.
 */
enum multiprocess_failure_point {
    MULTIPROCESS_FAILURE_NONE = 0,
    MULTIPROCESS_FAILURE_AFTER_IMAGE_ALLOCATION,
    MULTIPROCESS_FAILURE_AFTER_IMAGE_INITIALIZATION,
    MULTIPROCESS_FAILURE_AFTER_STACK_ALLOCATION,
    MULTIPROCESS_FAILURE_AFTER_ADDRESS_SPACE,
    MULTIPROCESS_FAILURE_AFTER_ALIAS_NARROW,
    MULTIPROCESS_FAILURE_AFTER_IMAGE_MAPPING,
    MULTIPROCESS_FAILURE_AFTER_STACK_MAPPING,
    MULTIPROCESS_FAILURE_AFTER_PERMISSION_WALK,
    MULTIPROCESS_FAILURE_POINT_COUNT
};

_Static_assert(MULTIPROCESS_FAILURE_POINT_COUNT - 1U ==
    MULTIPROCESS_CONTROLLED_ROBUSTNESS_TESTS,
    "controlled multiprocess robustness total changed");

static struct multiprocess_process processes[MULTIPROCESS_MAX_PROCESSES];
static struct multiprocess_proof_result installed_result;
static struct interrupt_process_gate gate;
static uint8_t schedule[MULTIPROCESS_SWITCH_CAPACITY];
static size_t schedule_length;
static size_t installed_count;
static size_t running_index;
static uint64_t next_generation = UINT64_C(1);
static enum multiprocess_status trap_status;
static bool running;
static bool gate_armed;
static bool proof_active;
static bool interrupts_were_enabled;
static uint8_t multiprocess_sentinel[MULTIPROCESS_SENTINEL_BYTES] = {
    0x50, 0x48, 0x49, 0x50, 0x49, 0x41, 0x2D, 0x4D,
    0x55, 0x4C, 0x54, 0x49, 0x50, 0x52, 0x4F, 0x43,
    0x45, 0x53, 0x53, 0x2D, 0x53, 0x45, 0x4E, 0x54,
    0x49, 0x4E, 0x45, 0x4C, 0x2D, 0x52, 0x33, 0x30
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

static bool result_zero(const struct multiprocess_proof_result *result)
{
    const uint8_t *bytes = (const uint8_t *)(const void *)result;

    for (size_t index = 0U; index < sizeof(*result); ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static enum multiprocess_status failure_status(
    enum multiprocess_failure_point point
)
{
    switch (point) {
    case MULTIPROCESS_FAILURE_AFTER_IMAGE_ALLOCATION:
    case MULTIPROCESS_FAILURE_AFTER_STACK_ALLOCATION:
        return MULTIPROCESS_STATUS_FRAME_ALLOCATION;
    case MULTIPROCESS_FAILURE_AFTER_IMAGE_INITIALIZATION:
        return MULTIPROCESS_STATUS_FRAME_INITIALIZATION;
    case MULTIPROCESS_FAILURE_AFTER_ADDRESS_SPACE:
        return MULTIPROCESS_STATUS_ADDRESS_SPACE;
    case MULTIPROCESS_FAILURE_AFTER_ALIAS_NARROW:
        return MULTIPROCESS_STATUS_IMAGE_ALIAS;
    case MULTIPROCESS_FAILURE_AFTER_IMAGE_MAPPING:
    case MULTIPROCESS_FAILURE_AFTER_STACK_MAPPING:
        return MULTIPROCESS_STATUS_USER_MAPPING;
    case MULTIPROCESS_FAILURE_AFTER_PERMISSION_WALK:
        return MULTIPROCESS_STATUS_USER_WALK;
    case MULTIPROCESS_FAILURE_NONE:
    case MULTIPROCESS_FAILURE_POINT_COUNT:
        return MULTIPROCESS_STATUS_OK;
    }
    return MULTIPROCESS_STATUS_ROBUSTNESS;
}

static void capture_census(struct multiprocess_census *census)
{
    census->frames = frame_allocator_get_stats();
    census->paging = paging_get_state();
    census->dma = dma_get_state();
    census->pci = pci_resource_get_state();
    census->vectors = interrupt_vector_get_state();
    census->msix = msix_get_state();
    census->cr3 = cpu_read_cr3();
    census->paging_process_released = paging_process_resources_released();
    census->proof_gate_released =
        interrupt_process_gate_resources_released();
    census->user_boundary_inactive = !process_user_boundary_active();
    census->interrupts_enabled = cpu_interrupts_enabled();
}

static bool census_equal(
    const struct multiprocess_census *left,
    const struct multiprocess_census *right
)
{
    return left->frames.addressable_frames ==
            right->frames.addressable_frames &&
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
        image->program_flags == 5U &&
        image->entry == MULTIPROCESS_ENTRY_ADDRESS &&
        image->file_offset == 0U &&
        image->virtual_address == PAGING_PROCESS_IMAGE_ADDRESS &&
        image->file_size == MULTIPROCESS_ELF_FILE_BYTES &&
        image->memory_size == MULTIPROCESS_ELF_FILE_BYTES &&
        image->alignment == ELF64_PAGE_BYTES &&
        image->mapping_start == PAGING_PROCESS_IMAGE_ADDRESS &&
        image->mapping_end ==
            PAGING_PROCESS_IMAGE_ADDRESS + ELF64_PAGE_BYTES &&
        canonical_user(image->entry) &&
        canonical_user(image->mapping_end - 1U);
}

static bool initialize_frame(uintptr_t frame)
{
    uint8_t *bytes = (uint8_t *)(void *)frame;

    for (size_t index = 0U; index < PAGING_PAGE_SIZE; ++index) {
        bytes[index] = 0U;
    }
    return true;
}

static uint64_t read_user_word(uintptr_t frame, uint64_t address)
{
    const uint64_t offset = address - PAGING_PROCESS_STACK_BASE -
        (uint64_t)(PAGING_PROCESS_STACK_PAGES - 1U) * PAGING_PAGE_SIZE;
    const volatile uint64_t *word =
        (const volatile uint64_t *)(void *)(frame + (uintptr_t)offset);

    return *word;
}

/*
 * What the process must look like when the kernel is about to give it the
 * processor. Only two register sets are ever admitted: the one the kernel
 * built for a first entry, and the one the program itself left at the single
 * instruction after its yield. Anything else means the saved context was
 * damaged between the trap and the resume.
 */
static bool context_authenticated(const struct multiprocess_process *process)
{
    const struct process_user_context *context = &process->context;
    const bool first_entry = process->observed_rounds == 0U;

    if (context->rsp != PAGING_PROCESS_STACK_END ||
        (authenticated_user_rflags(context->rflags) &
            ~MULTIPROCESS_ARITHMETIC_FLAGS) != MULTIPROCESS_USER_RFLAGS ||
        context->rdi != process->identity ||
        context->rsi != (uint64_t)process->rounds ||
        context->rdx != (uint64_t)process->fault_round ||
        context->rcx != (uint64_t)process->observed_rounds) {
        return false;
    }
    if (first_entry) {
        return context->rip == MULTIPROCESS_ENTRY_ADDRESS &&
            context->rax == 0U && context->rbx == 0U && context->rbp == 0U;
    }
    return context->rip == MULTIPROCESS_YIELD_RETURN_ADDRESS &&
        context->rax == MULTIPROCESS_YIELD_RESULT &&
        context->rbx == (uint64_t)process->observed_rounds &&
        context->rbp == PAGING_PROCESS_STACK_END;
}

static void save_context(
    struct multiprocess_process *process,
    const struct interrupt_frame *frame
)
{
    struct process_user_context *context = &process->context;

    context->rax = frame->rax;
    context->rbx = frame->rbx;
    context->rcx = frame->rcx;
    context->rdx = frame->rdx;
    context->rsi = frame->rsi;
    context->rdi = frame->rdi;
    context->rbp = frame->rbp;
    context->r8 = frame->r8;
    context->r9 = frame->r9;
    context->r10 = frame->r10;
    context->r11 = frame->r11;
    context->r12 = frame->r12;
    context->r13 = frame->r13;
    context->r14 = frame->r14;
    context->r15 = frame->r15;
    context->rip = frame->rip;
    context->rsp = (uint64_t)interrupt_frame_stack_pointer(frame);
    context->rflags = authenticated_user_rflags(frame->rflags);
}

static enum multiprocess_trap classify_trap(
    const struct multiprocess_process *process,
    const struct interrupt_frame *frame
)
{
    if (frame->vector == INTERRUPT_PROCESS_PROOF_VECTOR &&
        frame->error_code == 0U) {
        if (frame->rip == MULTIPROCESS_YIELD_RETURN_ADDRESS &&
            (uint32_t)frame->rax == MULTIPROCESS_YIELD_RESULT &&
            frame->rbx == (uint64_t)process->observed_rounds + 1U) {
            return MULTIPROCESS_TRAP_YIELD;
        }
        if (frame->rip == MULTIPROCESS_EXIT_RETURN_ADDRESS &&
            (uint32_t)frame->rax == MULTIPROCESS_EXIT_RESULT &&
            frame->rbx == (uint64_t)process->rounds) {
            return MULTIPROCESS_TRAP_EXIT;
        }
        return MULTIPROCESS_TRAP_UNEXPECTED;
    }
    if (frame->vector == MULTIPROCESS_PAGE_FAULT_VECTOR &&
        frame->error_code == MULTIPROCESS_FAULT_ERROR_CODE &&
        frame->rip == MULTIPROCESS_FAULT_ADDRESS &&
        frame->cr2 == PAGING_PROCESS_STACK_GUARD) {
        return MULTIPROCESS_TRAP_FAULT;
    }
    return MULTIPROCESS_TRAP_UNEXPECTED;
}

/*
 * The one CPL3 return path. It runs on the interrupt stack with the process
 * hierarchy still selected, saves what the process had, puts the kernel's own
 * tables back, and asks interrupts.S to abandon the user frame and return to
 * the scheduler instead of to CPL3.
 */
static struct multiprocess_process *process_for_active_hierarchy(void)
{
    const uint64_t root = cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U);

    for (size_t index = 0U; index < installed_count; ++index) {
        struct multiprocess_process *process = &processes[index];

        if (process->address_space.state == PAGING_PROCESS_SPACE_ACTIVE &&
            process->address_space.root_physical_address == root) {
            return process;
        }
    }
    return NULL;
}

static void multiprocess_trap_interrupt(
    struct interrupt_frame *frame,
    void *context
)
{
    /*
     * Which process this is comes from the hierarchy the processor is running
     * on, not from what the scheduler last recorded. The two are then required
     * to agree: a disagreement means the saved scheduler state and the machine
     * state parted company, and the kernel's own tables still have to be put
     * back before that can be reported.
     */
    struct multiprocess_process *process = process_for_active_hierarchy();
    uintptr_t resume_stack;

    if (process == NULL) {
        trap_status = MULTIPROCESS_STATUS_TRAP_AUTHENTICATION;
        resume_stack = process_user_resume_stack();
        if (interrupt_request_kernel_resume(frame, resume_stack) !=
                INTERRUPT_STATUS_OK) {
            trap_status = MULTIPROCESS_STATUS_GATE;
        }
        return;
    }
    if (context != &processes[0] || !running ||
        running_index >= installed_count ||
        process != &processes[running_index]) {
        trap_status = MULTIPROCESS_STATUS_TRAP_AUTHENTICATION;
        process->last_trap = MULTIPROCESS_TRAP_UNEXPECTED;
    } else if (frame == NULL || (frame->cs & UINT64_C(3)) != 3U ||
        frame->cs != CPU_GDT_USER_CODE_SELECTOR ||
        (authenticated_user_rflags(frame->rflags) &
            ~MULTIPROCESS_ARITHMETIC_FLAGS) != MULTIPROCESS_USER_RFLAGS ||
        !interrupt_frame_has_stack_tail(frame) ||
        interrupt_frame_stack_pointer(frame) != PAGING_PROCESS_STACK_END ||
        interrupt_frame_stack_selector(frame) != CPU_GDT_USER_DATA_SELECTOR ||
        (cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U)) !=
            process->address_space.root_physical_address) {
        trap_status = MULTIPROCESS_STATUS_TRAP_AUTHENTICATION;
        process->last_trap = MULTIPROCESS_TRAP_UNEXPECTED;
    } else {
        save_context(process, frame);
        process->last_trap = classify_trap(process, frame);
        if (process->last_trap == MULTIPROCESS_TRAP_UNEXPECTED) {
            trap_status = MULTIPROCESS_STATUS_TRAP_AUTHENTICATION;
        }
    }
    if (paging_process_restore_kernel(&process->address_space) !=
            PAGING_STATUS_OK) {
        trap_status = MULTIPROCESS_STATUS_KERNEL_CR3;
    }
    resume_stack = process_user_resume_stack();
    if (interrupt_request_kernel_resume(frame, resume_stack) !=
            INTERRUPT_STATUS_OK) {
        trap_status = MULTIPROCESS_STATUS_GATE;
    }
}

static enum multiprocess_status release_process(
    struct multiprocess_process *process
)
{
    bool failed = false;

    for (size_t mapped = process->stack_mapped_count; mapped > 0U; --mapped) {
        const size_t index = mapped - 1U;
        const uint64_t address = PAGING_PROCESS_STACK_BASE +
            (uint64_t)index * PAGING_PAGE_SIZE;

        if (paging_process_unmap_user_page(&process->address_space,
                PAGING_PROCESS_MAPPING_STACK, address) != PAGING_STATUS_OK) {
            failed = true;
        } else {
            --process->stack_mapped_count;
        }
    }
    if (process->image_mapped) {
        if (paging_process_unmap_user_page(&process->address_space,
                PAGING_PROCESS_MAPPING_IMAGE, PAGING_PROCESS_IMAGE_ADDRESS) !=
                PAGING_STATUS_OK) {
            failed = true;
        } else {
            process->image_mapped = false;
        }
    }
    if (process->alias.active &&
        paging_process_image_alias_restore(&process->address_space,
            &process->alias) != PAGING_STATUS_OK) {
        failed = true;
    }
    if (process->address_space.state != PAGING_PROCESS_SPACE_INVALID &&
        process->address_space.state != PAGING_PROCESS_SPACE_RELEASED &&
        paging_process_space_release(&process->address_space) !=
            PAGING_STATUS_OK) {
        failed = true;
    }
    for (size_t count = process->stack_frame_count; count > 0U; --count) {
        const size_t index = count - 1U;

        if (frame_release(process->stack_frames[index]) != FRAME_STATUS_OK) {
            failed = true;
        } else {
            process->stack_frames[index] = 0U;
            --process->stack_frame_count;
        }
    }
    if (process->image_frame != 0U) {
        if (frame_release(process->image_frame) != FRAME_STATUS_OK) {
            failed = true;
        } else {
            process->image_frame = 0U;
        }
    }
    process->state = MULTIPROCESS_PROCESS_RELEASED;
    process->built = false;
    return failed ? MULTIPROCESS_STATUS_TEARDOWN : MULTIPROCESS_STATUS_OK;
}

/*
 * Teardown runs newest first. A narrowing that split a 2 MiB kernel page so it
 * could make one frame read-only owns that split table, and a later narrowing
 * of a frame in the same region borrows it; restoring the owner first would
 * free a table the borrower still has a leaf in. paging.c refuses an
 * out-of-order restore, so this order is required rather than preferred.
 */
static enum multiprocess_status release_all(enum multiprocess_status result)
{
    bool failed = false;

    if (gate_armed) {
        if (interrupt_process_gate_disarm(&gate) != INTERRUPT_STATUS_OK) {
            failed = true;
        }
        gate_armed = false;
    }
    for (size_t count = installed_count; count > 0U; --count) {
        if (release_process(&processes[count - 1U]) !=
                MULTIPROCESS_STATUS_OK) {
            failed = true;
        }
        installed_count = count - 1U;
    }
    running = false;
    proof_active = false;
    if (interrupts_were_enabled) {
        cpu_interrupt_enable();
    }
    return failed ? MULTIPROCESS_STATUS_TEARDOWN : result;
}

static enum multiprocess_status build_process(
    size_t index,
    uint32_t rounds,
    uint32_t fault_round,
    enum multiprocess_failure_point failure_point,
    bool apply_failure
)
{
    struct multiprocess_process *process = &processes[index];
    struct elf64_validated_image image;

    zero_bytes(process, sizeof(*process));
    zero_bytes(&image, sizeof(image));
    process->state = MULTIPROCESS_PROCESS_BUILDING;
    process->identity = MULTIPROCESS_IDENTITY(index);
    process->rounds = rounds;
    process->fault_round = fault_round;
    process->generation = next_generation++;
    if (next_generation == 0U) {
        next_generation = 1U;
    }
    installed_count = index + 1U;

    if (phipia_multiprocess_elf64_parse(multiprocess_image,
            sizeof(multiprocess_image), &image) != ELF64_STATUS_OK) {
        return MULTIPROCESS_STATUS_ELF_PARSER;
    }
    if (!validated_placement(&image)) {
        return MULTIPROCESS_STATUS_ELF_PLACEMENT;
    }
    if (frame_allocate(&process->image_frame) != FRAME_STATUS_OK) {
        return MULTIPROCESS_STATUS_FRAME_ALLOCATION;
    }
    if (apply_failure &&
        failure_point == MULTIPROCESS_FAILURE_AFTER_IMAGE_ALLOCATION) {
        return failure_status(failure_point);
    }
    if (!initialize_frame(process->image_frame)) {
        return MULTIPROCESS_STATUS_FRAME_INITIALIZATION;
    }
    for (size_t offset = 0U; offset < sizeof(multiprocess_image); ++offset) {
        ((uint8_t *)(void *)process->image_frame)[offset] =
            multiprocess_image[offset];
    }
    if (!bytes_equal((const uint8_t *)(const void *)process->image_frame,
            multiprocess_image, sizeof(multiprocess_image))) {
        return MULTIPROCESS_STATUS_FRAME_INITIALIZATION;
    }
    if (apply_failure &&
        failure_point == MULTIPROCESS_FAILURE_AFTER_IMAGE_INITIALIZATION) {
        return failure_status(failure_point);
    }
    for (size_t page = 0U; page < PAGING_PROCESS_STACK_PAGES; ++page) {
        if (frame_allocate(&process->stack_frames[page]) != FRAME_STATUS_OK) {
            return MULTIPROCESS_STATUS_FRAME_ALLOCATION;
        }
        ++process->stack_frame_count;
        if (!initialize_frame(process->stack_frames[page])) {
            return MULTIPROCESS_STATUS_FRAME_INITIALIZATION;
        }
    }
    if (apply_failure &&
        failure_point == MULTIPROCESS_FAILURE_AFTER_STACK_ALLOCATION) {
        return failure_status(failure_point);
    }
    if (paging_process_space_build(&process->address_space) !=
            PAGING_STATUS_OK) {
        return MULTIPROCESS_STATUS_ADDRESS_SPACE;
    }
    if (apply_failure &&
        failure_point == MULTIPROCESS_FAILURE_AFTER_ADDRESS_SPACE) {
        return failure_status(failure_point);
    }
    if (paging_process_image_alias_narrow(&process->address_space,
            process->image_frame, &process->alias) != PAGING_STATUS_OK) {
        return MULTIPROCESS_STATUS_IMAGE_ALIAS;
    }
    if (apply_failure &&
        failure_point == MULTIPROCESS_FAILURE_AFTER_ALIAS_NARROW) {
        return failure_status(failure_point);
    }
    if (paging_process_map_user_page(&process->address_space,
            PAGING_PROCESS_MAPPING_IMAGE, PAGING_PROCESS_IMAGE_ADDRESS,
            process->image_frame, PAGING_EXECUTE) != PAGING_STATUS_OK) {
        return MULTIPROCESS_STATUS_USER_MAPPING;
    }
    process->image_mapped = true;
    if (apply_failure &&
        failure_point == MULTIPROCESS_FAILURE_AFTER_IMAGE_MAPPING) {
        return failure_status(failure_point);
    }
    for (size_t page = 0U; page < PAGING_PROCESS_STACK_PAGES; ++page) {
        if (paging_process_map_user_page(&process->address_space,
                PAGING_PROCESS_MAPPING_STACK,
                PAGING_PROCESS_STACK_BASE + (uint64_t)page * PAGING_PAGE_SIZE,
                process->stack_frames[page], PAGING_WRITE) !=
                PAGING_STATUS_OK) {
            return MULTIPROCESS_STATUS_USER_MAPPING;
        }
        ++process->stack_mapped_count;
    }
    if (apply_failure &&
        failure_point == MULTIPROCESS_FAILURE_AFTER_STACK_MAPPING) {
        return failure_status(failure_point);
    }
    if (paging_process_validate(&process->address_space, process->image_frame,
            process->stack_frames) != PAGING_STATUS_OK) {
        return MULTIPROCESS_STATUS_USER_WALK;
    }
    if (apply_failure &&
        failure_point == MULTIPROCESS_FAILURE_AFTER_PERMISSION_WALK) {
        return failure_status(failure_point);
    }
    if (!cpu_user_transition_contract_valid() || cpu_tss_rsp0() == 0U ||
        !canonical_user(image.entry) ||
        !canonical_user(PAGING_PROCESS_STACK_END - 1U)) {
        return MULTIPROCESS_STATUS_CPU_CONTRACT;
    }

    process->context.rip = MULTIPROCESS_ENTRY_ADDRESS;
    process->context.rsp = PAGING_PROCESS_STACK_END;
    process->context.rflags = MULTIPROCESS_USER_RFLAGS;
    process->context.rdi = process->identity;
    process->context.rsi = (uint64_t)rounds;
    process->context.rdx = (uint64_t)fault_round;
    process->state = MULTIPROCESS_PROCESS_RUNNABLE;
    process->built = true;
    return MULTIPROCESS_STATUS_OK;
}

static enum multiprocess_status give_processor(size_t index)
{
    struct multiprocess_process *process = &processes[index];

    if (!context_authenticated(process)) {
        return MULTIPROCESS_STATUS_CONTEXT_AUTHENTICATION;
    }
    if (schedule_length >= MULTIPROCESS_SWITCH_CAPACITY) {
        return MULTIPROCESS_STATUS_SCHEDULE;
    }
    if (interrupt_process_gate_validate(&gate) != INTERRUPT_STATUS_OK) {
        return MULTIPROCESS_STATUS_GATE;
    }
    if (paging_process_activate(&process->address_space) !=
            PAGING_STATUS_OK) {
        return MULTIPROCESS_STATUS_ENTRY;
    }
    process->last_trap = MULTIPROCESS_TRAP_NONE;
    running_index = index;
    running = true;
    process_enter_user_context(&process->context);
    running = false;
    schedule[schedule_length] = (uint8_t)index;
    ++schedule_length;
    if (trap_status != MULTIPROCESS_STATUS_OK) {
        return trap_status;
    }
    if (process_user_boundary_active() ||
        (cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U)) !=
            paging_get_state().root_physical_address) {
        return MULTIPROCESS_STATUS_KERNEL_CR3;
    }
    switch (process->last_trap) {
    case MULTIPROCESS_TRAP_YIELD:
        ++process->observed_rounds;
        break;
    case MULTIPROCESS_TRAP_EXIT:
        process->state = MULTIPROCESS_PROCESS_FINISHED;
        break;
    case MULTIPROCESS_TRAP_FAULT:
        process->state = MULTIPROCESS_PROCESS_TERMINATED;
        break;
    case MULTIPROCESS_TRAP_NONE:
    case MULTIPROCESS_TRAP_UNEXPECTED:
    case MULTIPROCESS_TRAP_COUNT:
        return MULTIPROCESS_STATUS_TRAP_AUTHENTICATION;
    }
    if (interrupt_process_gate_rearm(&gate) != INTERRUPT_STATUS_OK) {
        return MULTIPROCESS_STATUS_GATE;
    }
    return MULTIPROCESS_STATUS_OK;
}

/*
 * The schedule a plain round robin over the installed processes would have
 * produced, recomputed here from nothing but each process's configuration.
 * Comparing the recorded schedule against it is what turns "several processes
 * ran" into "they were interleaved, in order, and each one left exactly when
 * its program says it should".
 */
static bool schedule_is_round_robin(size_t count)
{
    uint32_t traps[MULTIPROCESS_MAX_PROCESSES];
    bool done[MULTIPROCESS_MAX_PROCESSES];
    size_t position = 0U;
    size_t remaining = count;

    if (count == 0U || count > MULTIPROCESS_MAX_PROCESSES) {
        return false;
    }
    for (size_t index = 0U; index < count; ++index) {
        traps[index] = 0U;
        done[index] = false;
    }
    while (remaining > 0U) {
        for (size_t index = 0U; index < count; ++index) {
            const struct multiprocess_process *process = &processes[index];

            if (done[index]) {
                continue;
            }
            if (position >= schedule_length ||
                schedule[position] != (uint8_t)index) {
                return false;
            }
            ++position;
            ++traps[index];
            if (process->fault_round != 0U &&
                traps[index] == process->fault_round) {
                done[index] = true;
                --remaining;
            } else if (traps[index] > process->rounds) {
                done[index] = true;
                --remaining;
            }
        }
    }
    return position == schedule_length;
}

static enum multiprocess_status run_schedule(size_t count)
{
    size_t remaining = count;
    size_t passes = 0U;

    while (remaining > 0U) {
        if (passes > MULTIPROCESS_SWITCH_CAPACITY) {
            return MULTIPROCESS_STATUS_SCHEDULE;
        }
        ++passes;
        for (size_t index = 0U; index < count; ++index) {
            struct multiprocess_process *process = &processes[index];
            enum multiprocess_status status;

            if (process->state != MULTIPROCESS_PROCESS_RUNNABLE) {
                continue;
            }
            status = give_processor(index);
            if (status != MULTIPROCESS_STATUS_OK) {
                return status;
            }
            if (process->state != MULTIPROCESS_PROCESS_RUNNABLE) {
                --remaining;
            }
        }
    }
    return MULTIPROCESS_STATUS_OK;
}

/*
 * Every process publishes its own identity and the round it reached on its own
 * stack. Reading all of them back at once is the isolation check: a process
 * that could reach another's pages would have overwritten a word that names a
 * different process.
 */
static enum multiprocess_status collect_published_state(size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        struct multiprocess_process *process = &processes[index];
        const uintptr_t top =
            process->stack_frames[PAGING_PROCESS_STACK_PAGES - 1U];
        uint32_t expected_progress;

        process->published_progress =
            read_user_word(top, MULTIPROCESS_PROGRESS_ADDRESS);
        process->published_identity =
            read_user_word(top, MULTIPROCESS_IDENTITY_ADDRESS);
        expected_progress = process->state ==
            MULTIPROCESS_PROCESS_TERMINATED ? process->fault_round :
            process->rounds;
        if (process->published_identity != process->identity ||
            process->published_progress != (uint64_t)expected_progress) {
            return MULTIPROCESS_STATUS_ISOLATION;
        }
        for (size_t other = 0U; other < count; ++other) {
            if (other != index &&
                process->published_identity == processes[other].identity) {
                return MULTIPROCESS_STATUS_ISOLATION;
            }
        }
    }
    return MULTIPROCESS_STATUS_OK;
}

static enum multiprocess_status multiprocess_attempt(
    struct multiprocess_proof_result *result,
    uint32_t fault_index,
    uint32_t fault_round,
    enum multiprocess_failure_point failure_point
)
{
    struct multiprocess_census before;
    struct multiprocess_census after;
    uint8_t sentinel_before[MULTIPROCESS_SENTINEL_BYTES];
    uint32_t table_frames = 0U;
    uint32_t completed = 0U;
    uint32_t terminated = 0U;
    bool concurrent = false;
    enum multiprocess_status status = MULTIPROCESS_STATUS_OK;

    if (result == NULL || failure_point >= MULTIPROCESS_FAILURE_POINT_COUNT) {
        return MULTIPROCESS_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(result, sizeof(*result));
    if (proof_active || !multiprocess_resources_released()) {
        return MULTIPROCESS_STATUS_BUSY;
    }
    capture_census(&before);
    for (size_t index = 0U; index < sizeof(sentinel_before); ++index) {
        sentinel_before[index] = multiprocess_sentinel[index];
    }
    interrupts_were_enabled = before.interrupts_enabled;
    cpu_interrupt_disable();
    proof_active = true;
    schedule_length = 0U;
    installed_count = 0U;
    running_index = 0U;
    running = false;
    trap_status = MULTIPROCESS_STATUS_OK;
    zero_bytes(processes, sizeof(processes));
    zero_bytes(schedule, sizeof(schedule));

    for (size_t index = 0U; index < MULTIPROCESS_MAX_PROCESSES; ++index) {
        const bool apply_failure =
            failure_point != MULTIPROCESS_FAILURE_NONE &&
            index + 1U == MULTIPROCESS_MAX_PROCESSES;
        const uint32_t process_fault_round =
            (uint32_t)index == fault_index ? fault_round : 0U;

        status = build_process(index, MULTIPROCESS_ROUNDS,
            process_fault_round, failure_point, apply_failure);
        if (status != MULTIPROCESS_STATUS_OK) {
            goto cleanup;
        }
        table_frames += (uint32_t)processes[index].address_space.table_frames;
    }
    concurrent = installed_count == MULTIPROCESS_MAX_PROCESSES;
    for (size_t index = 1U; index < installed_count; ++index) {
        if (processes[index].address_space.root_physical_address ==
                processes[index - 1U].address_space.root_physical_address ||
            processes[index].address_space.generation ==
                processes[index - 1U].address_space.generation) {
            status = MULTIPROCESS_STATUS_ADDRESS_SPACE;
            goto cleanup;
        }
    }
    if (interrupt_process_gate_arm(multiprocess_trap_interrupt, &processes[0],
            &gate) != INTERRUPT_STATUS_OK ||
        interrupt_process_gate_validate(&gate) != INTERRUPT_STATUS_OK ||
        gate.state != INTERRUPT_PROCESS_GATE_ARMED) {
        status = MULTIPROCESS_STATUS_GATE;
        goto cleanup;
    }
    gate_armed = true;
    status = run_schedule(installed_count);
    if (status != MULTIPROCESS_STATUS_OK) {
        goto cleanup;
    }
    if (!schedule_is_round_robin(installed_count)) {
        status = MULTIPROCESS_STATUS_SCHEDULE;
        goto cleanup;
    }
    status = collect_published_state(installed_count);
    if (status != MULTIPROCESS_STATUS_OK) {
        goto cleanup;
    }
    for (size_t index = 0U; index < installed_count; ++index) {
        const struct multiprocess_process *process = &processes[index];

        if (process->state == MULTIPROCESS_PROCESS_FINISHED) {
            ++completed;
            if (process->observed_rounds != process->rounds) {
                status = MULTIPROCESS_STATUS_SCHEDULE;
                goto cleanup;
            }
        } else if (process->state == MULTIPROCESS_PROCESS_TERMINATED) {
            ++terminated;
            if (process->observed_rounds + 1U != process->fault_round) {
                status = MULTIPROCESS_STATUS_CONTAINMENT;
                goto cleanup;
            }
        } else {
            status = MULTIPROCESS_STATUS_SCHEDULE;
            goto cleanup;
        }
    }
    if (fault_round == 0U) {
        if (terminated != 0U || completed != installed_count) {
            status = MULTIPROCESS_STATUS_SCHEDULE;
            goto cleanup;
        }
    } else if (terminated != 1U ||
        completed + 1U != (uint32_t)installed_count) {
        status = MULTIPROCESS_STATUS_CONTAINMENT;
        goto cleanup;
    }

cleanup:
    status = release_all(status);
    if (status == MULTIPROCESS_STATUS_TEARDOWN) {
        zero_bytes(result, sizeof(*result));
        return status;
    }
    if (paging_verify() != PAGING_STATUS_OK ||
        !multiprocess_resources_released()) {
        return MULTIPROCESS_STATUS_TEARDOWN;
    }
    capture_census(&after);
    if (!census_equal(&before, &after)) {
        return MULTIPROCESS_STATUS_RESOURCE_CENSUS;
    }
    if (!bytes_equal(multiprocess_sentinel, sentinel_before,
            sizeof(sentinel_before))) {
        return MULTIPROCESS_STATUS_SENTINEL;
    }
    if (status != MULTIPROCESS_STATUS_OK) {
        zero_bytes(result, sizeof(*result));
        return status;
    }
    if (failure_point != MULTIPROCESS_FAILURE_NONE) {
        return MULTIPROCESS_STATUS_ROBUSTNESS;
    }
    result->process_count = (uint32_t)MULTIPROCESS_MAX_PROCESSES;
    result->rounds = MULTIPROCESS_ROUNDS;
    result->switches = (uint32_t)schedule_length;
    result->completed = completed;
    result->terminated = terminated;
    result->address_space_table_frames = table_frames;
    result->robustness_tests = MULTIPROCESS_CONTROLLED_ROBUSTNESS_TESTS;
    result->concurrent_address_spaces = concurrent;
    result->round_robin_interleaved = true;
    result->contexts_preserved = true;
    result->isolation_confirmed = true;
    result->fault_contained = terminated == 1U;
    result->teardown_complete = true;
    result->resource_census_equal = true;
    return MULTIPROCESS_STATUS_OK;
}

bool multiprocess_foundation_self_test(size_t *completed_tests)
{
    size_t completed = 0U;
    struct elf64_validated_image image;

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;
    if (!process_user_context_layout_self_test()) {
        return false;
    }
    ++completed;
    if (phipia_multiprocess_elf64_self_test() !=
            ELF64_PARSER_ROBUSTNESS_CONTROLS) {
        return false;
    }
    ++completed;
    zero_bytes(&image, sizeof(image));
    if (phipia_multiprocess_elf64_parse(multiprocess_image,
            sizeof(multiprocess_image), &image) != ELF64_STATUS_OK ||
        !validated_placement(&image)) {
        return false;
    }
    ++completed;
    /* The proof executable and the multiprocess one must refuse each other. */
    zero_bytes(&image, sizeof(image));
    if (phipia_multiprocess_elf64_parse(multiprocess_image,
            ELF64_FILE_BYTES, &image) == ELF64_STATUS_OK ||
        phipia_elf64_parse(multiprocess_image, sizeof(multiprocess_image),
            &image) == ELF64_STATUS_OK) {
        return false;
    }
    ++completed;
    if (MULTIPROCESS_YIELD_RETURN_ADDRESS <= MULTIPROCESS_ENTRY_ADDRESS ||
        MULTIPROCESS_EXIT_RETURN_ADDRESS <=
            MULTIPROCESS_YIELD_RETURN_ADDRESS ||
        MULTIPROCESS_FAULT_ADDRESS <= MULTIPROCESS_EXIT_RETURN_ADDRESS ||
        MULTIPROCESS_FAULT_ADDRESS >=
            MULTIPROCESS_ENTRY_ADDRESS + MULTIPROCESS_ELF_CODE_BYTES) {
        return false;
    }
    ++completed;
    if (MULTIPROCESS_YIELD_RESULT == MULTIPROCESS_EXIT_RESULT ||
        MULTIPROCESS_YIELD_RESULT == ELF64_PROOF_RESULT ||
        MULTIPROCESS_EXIT_RESULT == ELF64_PROOF_RESULT) {
        return false;
    }
    ++completed;
    for (size_t index = 0U; index < MULTIPROCESS_MAX_PROCESSES; ++index) {
        for (size_t other = 0U; other < index; ++other) {
            if (MULTIPROCESS_IDENTITY(index) == MULTIPROCESS_IDENTITY(other)) {
                return false;
            }
        }
        if (MULTIPROCESS_IDENTITY(index) == 0U) {
            return false;
        }
    }
    ++completed;
    /*
     * RF is discarded rather than authenticated, and discarded rather than
     * merely tolerated: a register set carrying it is accepted, and the value
     * that survives has the bit gone, so nothing hands it back to a process.
     * Every other flag outside the arithmetic set is still required to be
     * exact, which the interrupt-enable case here is the control for.
     */
    if (MULTIPROCESS_EXPECTED_SWITCHES >= MULTIPROCESS_SWITCH_CAPACITY ||
        !multiprocess_resources_released() ||
        authenticated_user_rflags(MULTIPROCESS_USER_RFLAGS |
            CPU_RFLAGS_PROCESSOR_BOOKKEEPING) != MULTIPROCESS_USER_RFLAGS ||
        (authenticated_user_rflags(UINT64_C(0x10046)) &
            ~MULTIPROCESS_ARITHMETIC_FLAGS) != MULTIPROCESS_USER_RFLAGS ||
        (authenticated_user_rflags(MULTIPROCESS_USER_RFLAGS |
            UINT64_C(0x200)) & ~MULTIPROCESS_ARITHMETIC_FLAGS) ==
            MULTIPROCESS_USER_RFLAGS) {
        return false;
    }
    ++completed;
    *completed_tests = completed;
    return completed == MULTIPROCESS_CONTROLLED_ROBUSTNESS_TESTS;
}

enum multiprocess_status multiprocess_prove(
    struct multiprocess_proof_result *result
)
{
    struct multiprocess_proof_result controlled;
    enum multiprocess_status status;

    if (result == NULL) {
        return MULTIPROCESS_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(result, sizeof(*result));
    zero_bytes(&installed_result, sizeof(installed_result));

    for (enum multiprocess_failure_point point =
            MULTIPROCESS_FAILURE_AFTER_IMAGE_ALLOCATION;
        point < MULTIPROCESS_FAILURE_POINT_COUNT;
        point = (enum multiprocess_failure_point)(point + 1)) {
        zero_bytes(&controlled, sizeof(controlled));
        status = multiprocess_attempt(&controlled, UINT32_MAX, 0U, point);
        if (status != failure_status(point) || !result_zero(&controlled) ||
            !multiprocess_resources_released()) {
            return MULTIPROCESS_STATUS_ROBUSTNESS;
        }
    }

    /* One process faults; the rest must still finish and tear down clean. */
    zero_bytes(&controlled, sizeof(controlled));
    status = multiprocess_attempt(&controlled,
        MULTIPROCESS_CONTAINMENT_FAULT_INDEX,
        MULTIPROCESS_CONTAINMENT_FAULT_ROUND, MULTIPROCESS_FAILURE_NONE);
    if (status != MULTIPROCESS_STATUS_OK || !controlled.fault_contained ||
        controlled.terminated != 1U ||
        controlled.completed + 1U != controlled.process_count ||
        !multiprocess_resources_released()) {
        return status == MULTIPROCESS_STATUS_OK ?
            MULTIPROCESS_STATUS_CONTAINMENT : status;
    }

    status = multiprocess_attempt(result, UINT32_MAX, 0U,
        MULTIPROCESS_FAILURE_NONE);
    if (status != MULTIPROCESS_STATUS_OK) {
        return status;
    }
    if (result->switches != MULTIPROCESS_EXPECTED_SWITCHES) {
        zero_bytes(result, sizeof(*result));
        return MULTIPROCESS_STATUS_SCHEDULE;
    }
    /*
     * Nothing faults in the clean run, so containment is carried over from the
     * run that did fault. The published receipt is about the whole proof, not
     * about its last third.
     */
    result->fault_contained = controlled.fault_contained;
    installed_result = *result;
    return MULTIPROCESS_STATUS_OK;
}

struct multiprocess_proof_result multiprocess_get_proof_result(void)
{
    return installed_result;
}

bool multiprocess_resources_released(void)
{
    const struct paging_state paging = paging_get_state();

    return !proof_active && !gate_armed && !running &&
        paging_process_resources_released() &&
        interrupt_process_gate_resources_released() &&
        !process_user_boundary_active() &&
        (!paging.active ||
            (cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U)) ==
                paging.root_physical_address);
}

const char *multiprocess_trap_string(enum multiprocess_trap trap)
{
    static const char *const messages[MULTIPROCESS_TRAP_COUNT] = {
        "none",
        "yield",
        "exit",
        "fault",
        "unexpected"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        MULTIPROCESS_TRAP_COUNT, "multiprocess trap names are out of sync");
    if (trap < MULTIPROCESS_TRAP_NONE || trap >= MULTIPROCESS_TRAP_COUNT) {
        return "unknown multiprocess trap";
    }
    return messages[trap];
}

const char *multiprocess_status_string(enum multiprocess_status status)
{
    static const char *const messages[MULTIPROCESS_STATUS_COUNT] = {
        "ok",
        "null multiprocess argument",
        "the bounded multiprocess proof is already active",
        "multiprocess prerequisites are incomplete",
        "Rust ELF64 parser refused the multiprocess executable",
        "validated multiprocess placement is outside the fixed policy",
        "multiprocess frame allocation failed",
        "multiprocess frame initialization failed",
        "private multiprocess address-space construction failed",
        "executable identity-alias narrowing failed",
        "fixed user mapping installation failed",
        "effective user permission walk failed",
        "CPL3 descriptor or TSS contract failed",
        "shared multiprocess gate operation failed",
        "IRETQ user entry failed",
        "a saved user context was not authenticated",
        "a CPL3 return was not authenticated",
        "kernel CR3 restoration failed",
        "the recorded schedule is not the round robin",
        "a process observed another process's memory",
        "a faulting process was not contained",
        "multiprocess teardown leaked or failed",
        "multiprocess pre/post resource census differs",
        "the multiprocess proof changed the kernel sentinel",
        "controlled multiprocess robustness cleanup failed"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        MULTIPROCESS_STATUS_COUNT,
        "multiprocess status messages are out of sync");
    if (status < MULTIPROCESS_STATUS_OK ||
        status >= MULTIPROCESS_STATUS_COUNT) {
        return "unknown multiprocess status";
    }
    return messages[status];
}
