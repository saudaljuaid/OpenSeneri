/* SPDX-License-Identifier: GPL-3.0-only */
/* Three measured Linux x86-64 SYSCALL profiles for bounded BusyBox proofs. */

#include <phipia/linux_syscall.h>

#include <stddef.h>

#include <phipia/console.h>
#include <phipia/cpu.h>
#include <phipia/linux_abi.h>
#include <phipia/linux_cat.h>
#include <phipia/linux_uname.h>
#include <phipia/memory.h>

#define CPUID_EXTENDED_ROOT UINT32_C(0x80000000)
#define CPUID_EXTENDED_FEATURES UINT32_C(0x80000001)
#define CPUID_SYSCALL_SYSRET (UINT32_C(1) << 11U)
#define IA32_EFER UINT32_C(0xC0000080)
#define IA32_STAR UINT32_C(0xC0000081)
#define IA32_LSTAR UINT32_C(0xC0000082)
#define IA32_FMASK UINT32_C(0xC0000084)
#define IA32_FS_BASE UINT32_C(0xC0000100)
#define EFER_SCE UINT64_C(1)
#define ARCH_SET_FS UINT64_C(0x1002)
#define LINUX_STAR_VALUE \
    ((UINT64_C(0x23) << 48U) | (UINT64_C(0x08) << 32U))
#define LINUX_FMASK_VALUE \
    ((UINT64_C(1) << 8U) | (UINT64_C(1) << 9U) | \
        (UINT64_C(1) << 10U) | (UINT64_C(1) << 14U) | \
        (UINT64_C(1) << 18U))
#define LINUX_USER_RFLAGS_ALLOWED UINT64_C(0x8D7)
#define LINUX_ERRNO_EINVAL 22
#define LINUX_ERRNO_EFAULT 14
#define LINUX_ERRNO_EBADF 9
#define LINUX_ERRNO_ENOTTY 25
#define LINUX_ERRNO_ENOSYS 38
#define LINUX_TIOCGWINSZ UINT64_C(0x5413)
#define LINUX_KERNEL_STACK_BYTES (16U * 1024U)
_Static_assert(sizeof(struct linux_syscall_frame) == 144U,
    "Linux syscall assembly frame size changed");
_Static_assert(offsetof(struct linux_syscall_frame, rax) == 0U &&
    offsetof(struct linux_syscall_frame, rdi) == 8U &&
    offsetof(struct linux_syscall_frame, r10) == 32U &&
    offsetof(struct linux_syscall_frame, r9) == 48U &&
    offsetof(struct linux_syscall_frame, rip) == 104U &&
    offsetof(struct linux_syscall_frame, rflags) == 120U &&
    offsetof(struct linux_syscall_frame, rsp) == 128U &&
    offsetof(struct linux_syscall_frame, ss) == 136U,
    "Linux syscall assembly frame offsets changed");
_Static_assert(LINUX_SYSCALL_ALLOWLIST_COUNT <= LINUX_SYSCALL_ALLOWLIST_MAX,
    "Linux syscall allowlist exceeds the fixed ceiling");
_Static_assert(LINUX_UNAME_SYSCALL_ALLOWLIST_COUNT <=
    LINUX_SYSCALL_ALLOWLIST_MAX,
    "Linux uname syscall allowlist exceeds the fixed ceiling");
_Static_assert(LINUX_CAT_SYSCALL_ALLOWLIST_COUNT <=
    LINUX_SYSCALL_ALLOWLIST_MAX,
    "Linux cat syscall allowlist exceeds the fixed ceiling");

struct linux_utsname_record {
    char sysname[LINUX_UTS_FIELD_BYTES];
    char nodename[LINUX_UTS_FIELD_BYTES];
    char release[LINUX_UTS_FIELD_BYTES];
    char version[LINUX_UTS_FIELD_BYTES];
    char machine[LINUX_UTS_FIELD_BYTES];
    char domainname[LINUX_UTS_FIELD_BYTES];
};

struct linux_iovec {
    uint64_t base;
    uint64_t length;
};

_Static_assert(sizeof(struct linux_utsname_record) == LINUX_UTS_BYTES &&
    _Alignof(struct linux_utsname_record) == 1U &&
    offsetof(struct linux_utsname_record, sysname) == 0U &&
    offsetof(struct linux_utsname_record, nodename) == 65U &&
    offsetof(struct linux_utsname_record, release) == 130U &&
    offsetof(struct linux_utsname_record, version) == 195U &&
    offsetof(struct linux_utsname_record, machine) == 260U &&
    offsetof(struct linux_utsname_record, domainname) == 325U,
    "Linux x86-64 utsname layout changed");
_Static_assert(sizeof(struct linux_iovec) == 16U &&
    _Alignof(struct linux_iovec) == 8U,
    "Linux x86-64 iovec layout changed");

enum stdout_sink_state {
    STDOUT_SINK_CANDIDATE = 0,
    STDOUT_SINK_ARMED,
    STDOUT_SINK_WRITTEN,
    STDOUT_SINK_RELEASED
};

enum provenance_state {
    PROVENANCE_CANDIDATE = 0,
    PROVENANCE_ENTERED,
    PROVENANCE_COMPLETED,
    PROVENANCE_RELEASED
};

enum cat_syscall_phase {
    CAT_SYSCALL_INIT_FS = 0,
    CAT_SYSCALL_INIT_TID,
    CAT_SYSCALL_READ,
    CAT_SYSCALL_WRITE,
    CAT_SYSCALL_EXIT,
    CAT_SYSCALL_RELEASED
};

struct linux_syscall_runtime {
    struct linux_syscall_context context;
    struct linux_syscall_result result;
    uint64_t saved_efer;
    uint64_t saved_star;
    uint64_t saved_lstar;
    uint64_t saved_fmask;
    uint64_t saved_fs_base;
    uint64_t request_generation;
    uint32_t call_index;
    uint32_t request_ordinal;
    enum linux_syscall_cpu_state state;
    enum stdout_sink_state stdout_state;
    enum linux_uts_copy_state uts_state;
    enum linux_uname_stdout_state uname_stdout_state;
    enum provenance_state provenance_state;
    struct linux_syscall_frame cat_saved_frame;
    struct linux_syscall_frame cat_authenticated_frame;
    uint8_t cat_output[LINUX_CAT_INPUT_LINE_BYTES + 1U];
    uint64_t cat_saved_generation;
    uint64_t cat_saved_cr3;
    uint32_t cat_saved_ordinal;
    uint32_t cat_output_bytes;
    enum linux_cat_read_state cat_read_state;
    enum cat_syscall_phase cat_phase;
    bool cat_saved_valid;
    bool cat_eof_pending;
    bool seen[LINUX_SYSCALL_ALLOWLIST_MAX];
    bool heap_mapped[PAGING_LINUX_HEAP_PAGES];
    bool anonymous_mapped;
    bool active;
};

static const uint64_t allowlist[LINUX_SYSCALL_ALLOWLIST_COUNT] = {
    UINT64_C(1), UINT64_C(9), UINT64_C(11), UINT64_C(12),
    UINT64_C(158), UINT64_C(218), UINT64_C(231)
};

static const uint64_t expected_calls[LINUX_SYSCALL_EXPECTED_CALLS] = {
    UINT64_C(158), UINT64_C(218), UINT64_C(12), UINT64_C(12),
    UINT64_C(9), UINT64_C(9), UINT64_C(1), UINT64_C(11), UINT64_C(231)
};

static const uint8_t expected_stdout[LINUX_SYSCALL_STDOUT_BYTES] = {
    'P', 'H', 'I', 'P', 'I', 'A', '\n'
};

static const uint64_t uname_allowlist[LINUX_UNAME_SYSCALL_ALLOWLIST_COUNT] = {
    UINT64_C(16), UINT64_C(20), UINT64_C(63), UINT64_C(158),
    UINT64_C(218), UINT64_C(231)
};

static const uint64_t uname_expected_calls[
    LINUX_UNAME_SYSCALL_EXPECTED_CALLS] = {
    UINT64_C(158), UINT64_C(218), UINT64_C(63), UINT64_C(16),
    UINT64_C(20), UINT64_C(231)
};

static const uint8_t uname_expected_stdout[
    LINUX_UNAME_SYSCALL_STDOUT_BYTES] = {
    'L', 'i', 'n', 'u', 'x', '\n'
};

static const uint64_t cat_allowlist[LINUX_CAT_SYSCALL_ALLOWLIST_COUNT] = {
    UINT64_C(0), UINT64_C(1), UINT64_C(158), UINT64_C(218), UINT64_C(231)
};

static const struct linux_utsname_record phipia_uts_record = {
    .sysname = "Linux",
    .nodename = "phipia",
    .release = "2.2.0-phipia",
    .version = "Phipia",
    .machine = "x86_64",
    .domainname = "(none)"
};

static struct linux_syscall_runtime runtime;

/* Loaded by the second instruction in linux_syscall_entry. */
uint64_t linux_syscall_kernel_stack;

static void zero_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool bytes_equal(const void *left, const void *right, size_t length)
{
    const uint8_t *left_bytes = left;
    const uint8_t *right_bytes = right;

    for (size_t index = 0U; index < length; ++index) {
        if (left_bytes[index] != right_bytes[index]) {
            return false;
        }
    }
    return true;
}

static bool canonical_user(uint64_t address)
{
    return address <= UINT64_C(0x00007FFFFFFFFFFF);
}

static bool user_range_shape_valid(uint64_t address, size_t length)
{
    return address != 0U && length != 0U && canonical_user(address) &&
        address <= UINT64_MAX - (uint64_t)(length - 1U) &&
        canonical_user(address + (uint64_t)(length - 1U));
}

static bool syscall_supported(void)
{
    struct cpuid_result root;
    struct cpuid_result features;

    cpu_cpuid(CPUID_EXTENDED_ROOT, 0U, &root);
    if (root.eax < CPUID_EXTENDED_FEATURES) {
        return false;
    }
    cpu_cpuid(CPUID_EXTENDED_FEATURES, 0U, &features);
    return (features.edx & CPUID_SYSCALL_SYSRET) != 0U;
}

static enum linux_syscall_status transition_cpu(
    enum linux_syscall_cpu_state next
)
{
    bool allowed = false;

    if (next >= LINUX_SYSCALL_CPU_STATE_COUNT || runtime.state == next) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    switch (runtime.state) {
    case LINUX_SYSCALL_CPU_CANDIDATE:
        allowed = next == LINUX_SYSCALL_CPU_ARMED;
        break;
    case LINUX_SYSCALL_CPU_ARMED:
        allowed = next == LINUX_SYSCALL_CPU_ENTERED ||
            next == LINUX_SYSCALL_CPU_DISARMED;
        break;
    case LINUX_SYSCALL_CPU_ENTERED:
        allowed = next == LINUX_SYSCALL_CPU_RETURNED;
        break;
    case LINUX_SYSCALL_CPU_RETURNED:
        allowed = next == LINUX_SYSCALL_CPU_ENTERED ||
            next == LINUX_SYSCALL_CPU_DISARMED;
        break;
    case LINUX_SYSCALL_CPU_DISARMED:
    case LINUX_SYSCALL_CPU_STATE_COUNT:
        break;
    }
    if (!allowed) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    runtime.state = next;
    runtime.result.cpu_state = next;
    return LINUX_SYSCALL_STATUS_OK;
}

static enum linux_syscall_status transition_stdout(enum stdout_sink_state next)
{
    bool allowed = false;

    if (next > STDOUT_SINK_RELEASED || runtime.stdout_state == next) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    switch (runtime.stdout_state) {
    case STDOUT_SINK_CANDIDATE:
        allowed = next == STDOUT_SINK_ARMED ||
            next == STDOUT_SINK_RELEASED;
        break;
    case STDOUT_SINK_ARMED:
        allowed = next == STDOUT_SINK_WRITTEN ||
            next == STDOUT_SINK_RELEASED;
        break;
    case STDOUT_SINK_WRITTEN:
        allowed = next == STDOUT_SINK_RELEASED;
        break;
    case STDOUT_SINK_RELEASED:
        break;
    }
    if (!allowed) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    runtime.stdout_state = next;
    return LINUX_SYSCALL_STATUS_OK;
}

static enum linux_syscall_status transition_uts(
    enum linux_uts_copy_state next
)
{
    bool allowed = false;

    if (next >= LINUX_UTS_COPY_STATE_COUNT || runtime.uts_state == next) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    switch (runtime.uts_state) {
    case LINUX_UTS_COPY_CANDIDATE:
        allowed = next == LINUX_UTS_COPY_ACTIVE ||
            next == LINUX_UTS_COPY_RELEASED;
        break;
    case LINUX_UTS_COPY_ACTIVE:
        allowed = next == LINUX_UTS_COPY_COMPLETED ||
            next == LINUX_UTS_COPY_FAILED;
        break;
    case LINUX_UTS_COPY_COMPLETED:
    case LINUX_UTS_COPY_FAILED:
        allowed = next == LINUX_UTS_COPY_RELEASED;
        break;
    case LINUX_UTS_COPY_RELEASED:
    case LINUX_UTS_COPY_STATE_COUNT:
        break;
    }
    if (!allowed) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    runtime.uts_state = next;
    return LINUX_SYSCALL_STATUS_OK;
}

static enum linux_syscall_status transition_uname_stdout(
    enum linux_uname_stdout_state next
)
{
    bool allowed = false;

    if (next >= LINUX_UNAME_STDOUT_STATE_COUNT ||
        runtime.uname_stdout_state == next) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    switch (runtime.uname_stdout_state) {
    case LINUX_UNAME_STDOUT_CANDIDATE:
        allowed = next == LINUX_UNAME_STDOUT_EMPTY ||
            next == LINUX_UNAME_STDOUT_RELEASED;
        break;
    case LINUX_UNAME_STDOUT_EMPTY:
        allowed = next == LINUX_UNAME_STDOUT_RECEIVING ||
            next == LINUX_UNAME_STDOUT_INVALID ||
            next == LINUX_UNAME_STDOUT_RELEASED;
        break;
    case LINUX_UNAME_STDOUT_RECEIVING:
        allowed = next == LINUX_UNAME_STDOUT_VALID ||
            next == LINUX_UNAME_STDOUT_INVALID;
        break;
    case LINUX_UNAME_STDOUT_VALID:
    case LINUX_UNAME_STDOUT_INVALID:
        allowed = next == LINUX_UNAME_STDOUT_RELEASED;
        break;
    case LINUX_UNAME_STDOUT_RELEASED:
    case LINUX_UNAME_STDOUT_STATE_COUNT:
        break;
    }
    if (!allowed) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    runtime.uname_stdout_state = next;
    return LINUX_SYSCALL_STATUS_OK;
}

static enum linux_syscall_status transition_provenance(
    enum provenance_state next
)
{
    bool allowed = false;

    if (next > PROVENANCE_RELEASED || runtime.provenance_state == next) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    switch (runtime.provenance_state) {
    case PROVENANCE_CANDIDATE:
        allowed = next == PROVENANCE_ENTERED ||
            next == PROVENANCE_RELEASED;
        break;
    case PROVENANCE_ENTERED:
        allowed = next == PROVENANCE_COMPLETED ||
            next == PROVENANCE_RELEASED;
        break;
    case PROVENANCE_COMPLETED:
        allowed = next == PROVENANCE_CANDIDATE ||
            next == PROVENANCE_RELEASED;
        break;
    case PROVENANCE_RELEASED:
        break;
    }
    if (!allowed) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    runtime.provenance_state = next;
    return LINUX_SYSCALL_STATUS_OK;
}

static size_t profile_allowlist_count(enum linux_syscall_profile profile)
{
    switch (profile) {
    case LINUX_SYSCALL_PROFILE_UNAME:
        return LINUX_UNAME_SYSCALL_ALLOWLIST_COUNT;
    case LINUX_SYSCALL_PROFILE_CAT:
        return LINUX_CAT_SYSCALL_ALLOWLIST_COUNT;
    case LINUX_SYSCALL_PROFILE_ECHO:
    case LINUX_SYSCALL_PROFILE_COUNT:
    default:
        return LINUX_SYSCALL_ALLOWLIST_COUNT;
    }
}

static size_t profile_expected_calls(enum linux_syscall_profile profile)
{
    switch (profile) {
    case LINUX_SYSCALL_PROFILE_UNAME:
        return LINUX_UNAME_SYSCALL_EXPECTED_CALLS;
    case LINUX_SYSCALL_PROFILE_CAT:
        return LINUX_CAT_SYSCALL_MAX_CALLS;
    case LINUX_SYSCALL_PROFILE_ECHO:
    case LINUX_SYSCALL_PROFILE_COUNT:
    default:
        return LINUX_SYSCALL_EXPECTED_CALLS;
    }
}

static uint64_t profile_allowed(
    enum linux_syscall_profile profile,
    size_t index
)
{
    switch (profile) {
    case LINUX_SYSCALL_PROFILE_UNAME:
        return uname_allowlist[index];
    case LINUX_SYSCALL_PROFILE_CAT:
        return cat_allowlist[index];
    case LINUX_SYSCALL_PROFILE_ECHO:
    case LINUX_SYSCALL_PROFILE_COUNT:
    default:
        return allowlist[index];
    }
}

static uint64_t profile_expected(
    enum linux_syscall_profile profile,
    size_t index
)
{
    return profile == LINUX_SYSCALL_PROFILE_UNAME ?
        uname_expected_calls[index] : expected_calls[index];
}

static uint64_t cat_expected_number(enum cat_syscall_phase phase)
{
    switch (phase) {
    case CAT_SYSCALL_INIT_FS:
        return UINT64_C(158);
    case CAT_SYSCALL_INIT_TID:
        return UINT64_C(218);
    case CAT_SYSCALL_READ:
        return UINT64_C(0);
    case CAT_SYSCALL_WRITE:
        return UINT64_C(1);
    case CAT_SYSCALL_EXIT:
        return UINT64_C(231);
    case CAT_SYSCALL_RELEASED:
    default:
        return UINT64_MAX;
    }
}

static size_t allowlist_index(
    enum linux_syscall_profile profile,
    uint64_t number
)
{
    for (size_t index = 0U; index < profile_allowlist_count(profile); ++index) {
        if (profile_allowed(profile, index) == number) {
            return index;
        }
    }
    return SIZE_MAX;
}

static bool enosys_result(
    enum linux_syscall_profile profile,
    uint64_t number,
    uint64_t *result
)
{
    if (result == NULL || allowlist_index(profile, number) != SIZE_MAX) {
        return false;
    }
    *result = (uint64_t)(int64_t)-LINUX_ERRNO_ENOSYS;
    return true;
}

static bool context_equal(
    const struct linux_syscall_context *left,
    const struct linux_syscall_context *right
)
{
    if (left->profile != right->profile ||
        left->address_space != right->address_space ||
        left->process_generation != right->process_generation ||
        left->executable_start != right->executable_start ||
        left->executable_end != right->executable_end ||
        left->stack_start != right->stack_start ||
        left->stack_end != right->stack_end ||
        left->fs_address != right->fs_address ||
        left->tid_address != right->tid_address ||
        left->anonymous_frame != right->anonymous_frame ||
        left->exit_observed != right->exit_observed ||
        left->failure_before_ordinal != right->failure_before_ordinal ||
        left->failure_after_ordinal != right->failure_after_ordinal ||
        left->controlled_run != right->controlled_run ||
        left->publish_stdout != right->publish_stdout) {
        return false;
    }
    for (size_t index = 0U; index < PAGING_LINUX_HEAP_PAGES; ++index) {
        if (left->heap_frames[index] != right->heap_frames[index]) {
            return false;
        }
    }
    for (size_t index = 0U; index < PAGING_LINUX_STACK_PAGES; ++index) {
        if (left->stack_frames[index] != right->stack_frames[index]) {
            return false;
        }
    }
    return true;
}

static bool result_equal(
    const struct linux_syscall_result *left,
    const struct linux_syscall_result *right
)
{
    return left->syscall_count == right->syscall_count &&
        left->distinct_syscalls == right->distinct_syscalls &&
        left->stdout_bytes == right->stdout_bytes &&
        left->exit_status == right->exit_status &&
        left->status == right->status &&
        left->cpu_state == right->cpu_state &&
        left->stdout_valid == right->stdout_valid &&
        left->exit_zero == right->exit_zero &&
        left->real_syscall_instruction == right->real_syscall_instruction &&
        left->process_authenticated == right->process_authenticated &&
        left->cr3_authenticated == right->cr3_authenticated &&
        left->cpu_disarmed == right->cpu_disarmed &&
        left->controlled_failure_observed ==
            right->controlled_failure_observed &&
        left->uts_copy_valid == right->uts_copy_valid &&
        left->cat_input_bytes == right->cat_input_bytes &&
        left->cat_input_lines == right->cat_input_lines &&
        left->cat_resume_count == right->cat_resume_count &&
        left->cat_wait_observed == right->cat_wait_observed &&
        left->cat_eof_delivered == right->cat_eof_delivered;
}

static bool runtime_equal(
    const struct linux_syscall_runtime *left,
    const struct linux_syscall_runtime *right
)
{
    if (!context_equal(&left->context, &right->context) ||
        !result_equal(&left->result, &right->result) ||
        left->saved_efer != right->saved_efer ||
        left->saved_star != right->saved_star ||
        left->saved_lstar != right->saved_lstar ||
        left->saved_fmask != right->saved_fmask ||
        left->saved_fs_base != right->saved_fs_base ||
        left->request_generation != right->request_generation ||
        left->call_index != right->call_index ||
        left->request_ordinal != right->request_ordinal ||
        left->state != right->state ||
        left->stdout_state != right->stdout_state ||
        left->uts_state != right->uts_state ||
        left->uname_stdout_state != right->uname_stdout_state ||
        left->provenance_state != right->provenance_state ||
        !bytes_equal(&left->cat_saved_frame, &right->cat_saved_frame,
            sizeof(left->cat_saved_frame)) ||
        !bytes_equal(&left->cat_authenticated_frame,
            &right->cat_authenticated_frame,
            sizeof(left->cat_authenticated_frame)) ||
        !bytes_equal(left->cat_output, right->cat_output,
            sizeof(left->cat_output)) ||
        left->cat_saved_generation != right->cat_saved_generation ||
        left->cat_saved_cr3 != right->cat_saved_cr3 ||
        left->cat_saved_ordinal != right->cat_saved_ordinal ||
        left->cat_output_bytes != right->cat_output_bytes ||
        left->cat_read_state != right->cat_read_state ||
        left->cat_phase != right->cat_phase ||
        left->cat_saved_valid != right->cat_saved_valid ||
        left->cat_eof_pending != right->cat_eof_pending ||
        left->anonymous_mapped != right->anonymous_mapped ||
        left->active != right->active) {
        return false;
    }
    for (size_t index = 0U; index < LINUX_SYSCALL_ALLOWLIST_MAX; ++index) {
        if (left->seen[index] != right->seen[index]) {
            return false;
        }
    }
    for (size_t index = 0U; index < PAGING_LINUX_HEAP_PAGES; ++index) {
        if (left->heap_mapped[index] != right->heap_mapped[index]) {
            return false;
        }
    }
    return true;
}

static bool msr_values_valid(
    uint64_t efer,
    uint64_t star,
    uint64_t lstar,
    uint64_t fmask
)
{
    return (efer & EFER_SCE) != 0U && star == LINUX_STAR_VALUE &&
        lstar == (uint64_t)(uintptr_t)linux_syscall_entry &&
        fmask == LINUX_FMASK_VALUE;
}

static bool msr_contract_valid(void)
{
    return msr_values_valid(cpu_read_msr(IA32_EFER),
        cpu_read_msr(IA32_STAR), cpu_read_msr(IA32_LSTAR),
        cpu_read_msr(IA32_FMASK));
}

static bool user_writable(uint64_t address, size_t length)
{
    uint64_t cursor = address;
    size_t remaining = length;

    if (!user_range_shape_valid(address, length)) {
        return false;
    }
    while (remaining > 0U) {
        struct paging_translation translation;
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (cursor & (PAGING_PAGE_SIZE - 1U)));

        if (chunk > remaining) {
            chunk = remaining;
        }
        if (paging_process_translate(runtime.context.address_space, cursor,
                &translation) != PAGING_STATUS_OK || !translation.user ||
            translation.permissions != PAGING_WRITE ||
            translation.level != 1U ||
            !frame_range_overlaps_allocatable_memory(
                translation.physical_address, chunk)) {
            return false;
        }
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool user_stack_writable(uint64_t address, size_t length)
{
    uint64_t cursor = address;
    size_t remaining = length;

    if (!user_range_shape_valid(address, length) ||
        address < runtime.context.stack_start ||
        address + (uint64_t)(length - 1U) >= runtime.context.stack_end) {
        return false;
    }
    while (remaining > 0U) {
        struct paging_translation translation;
        const size_t page = (size_t)((cursor -
            runtime.context.stack_start) / PAGING_PAGE_SIZE);
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (cursor & (PAGING_PAGE_SIZE - 1U)));

        if (chunk > remaining) {
            chunk = remaining;
        }
        if (page >= PAGING_LINUX_STACK_PAGES ||
            runtime.context.stack_frames[page] == 0U ||
            paging_process_translate(runtime.context.address_space, cursor,
                &translation) != PAGING_STATUS_OK || !translation.user ||
            translation.permissions != PAGING_WRITE ||
            translation.level != 1U ||
            (translation.physical_address & ~(PAGING_PAGE_SIZE - 1U)) !=
                runtime.context.stack_frames[page] ||
            !frame_range_overlaps_allocatable_memory(
                translation.physical_address, chunk)) {
            return false;
        }
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool copy_to_user_stack(
    uint64_t address,
    const void *source,
    size_t length
)
{
    const uint8_t *input = source;
    uint64_t cursor = address;
    size_t remaining = length;

    /* Full ownership and permission validation precedes the first store. */
    if (source == NULL || !user_stack_writable(address, length)) {
        return false;
    }
    while (remaining > 0U) {
        struct paging_translation translation;
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (cursor & (PAGING_PAGE_SIZE - 1U)));

        if (chunk > remaining) {
            chunk = remaining;
        }
        if (paging_process_translate(runtime.context.address_space, cursor,
                &translation) != PAGING_STATUS_OK) {
            return false;
        }
        uint8_t *output =
            (uint8_t *)(uintptr_t)translation.physical_address;

        for (size_t index = 0U; index < chunk; ++index) {
            output[index] = input[index];
        }
        input += chunk;
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool copy_from_user(void *destination, uint64_t address, size_t length)
{
    uint8_t *output = destination;
    uint64_t cursor = address;
    size_t remaining = length;

    if (destination == NULL || !user_range_shape_valid(address, length)) {
        return false;
    }
    while (remaining > 0U) {
        struct paging_translation translation;
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (cursor & (PAGING_PAGE_SIZE - 1U)));

        if (chunk > remaining) {
            chunk = remaining;
        }
        if (paging_process_translate(runtime.context.address_space, cursor,
                &translation) != PAGING_STATUS_OK || !translation.user ||
            translation.level != 1U ||
            !frame_range_overlaps_allocatable_memory(
                translation.physical_address, chunk)) {
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

static bool frame_return_shape_valid(
    const struct linux_syscall_context *context,
    const struct linux_syscall_frame *frame
)
{
    return context != NULL && frame != NULL &&
        context->executable_start <= UINT64_MAX - 2U &&
        context->executable_start + 2U < context->executable_end &&
        context->stack_start < context->stack_end &&
        frame->cs == CPU_GDT_USER_CODE_SELECTOR &&
        frame->ss == CPU_GDT_USER_DATA_SELECTOR &&
        (frame->rflags & UINT64_C(2)) != 0U &&
        (frame->rflags &
            ~(LINUX_USER_RFLAGS_ALLOWED |
                CPU_RFLAGS_PROCESSOR_BOOKKEEPING)) == 0U &&
        canonical_user(frame->rip) &&
        frame->rip >= context->executable_start + 2U &&
        frame->rip < context->executable_end &&
        canonical_user(frame->rsp) && frame->rsp >= context->stack_start &&
        frame->rsp < context->stack_end;
}

static bool cat_frame_authenticated(void)
{
    uint8_t instruction[2];

    return runtime.cat_saved_valid &&
        bytes_equal(&runtime.cat_saved_frame,
            &runtime.cat_authenticated_frame,
            sizeof(runtime.cat_saved_frame)) &&
        runtime.cat_saved_generation == runtime.request_generation &&
        runtime.cat_saved_generation ==
            runtime.context.process_generation &&
        runtime.cat_saved_cr3 ==
            runtime.context.address_space->root_physical_address &&
        runtime.cat_saved_ordinal == runtime.request_ordinal &&
        runtime.cat_saved_frame.rax == UINT64_C(0) &&
        runtime.cat_saved_frame.rdi == UINT64_C(0) &&
        runtime.cat_saved_frame.rsi == LINUX_CAT_READ_BUFFER &&
        runtime.cat_saved_frame.rdx == LINUX_CAT_READ_COUNT &&
        frame_return_shape_valid(&runtime.context,
            &runtime.cat_saved_frame) &&
        copy_from_user(instruction, runtime.cat_saved_frame.rip - 2U,
            sizeof(instruction)) && instruction[0] == UINT8_C(0x0F) &&
        instruction[1] == UINT8_C(0x05);
}

static bool cat_line_valid(const uint8_t *bytes, size_t byte_count)
{
    if (bytes == NULL || byte_count == 0U ||
        byte_count > LINUX_CAT_INPUT_LINE_BYTES + 1U ||
        bytes[byte_count - 1U] != '\n') {
        return false;
    }
    for (size_t index = 0U; index + 1U < byte_count; ++index) {
        if (bytes[index] < UINT8_C(0x20) || bytes[index] > UINT8_C(0x7E)) {
            return false;
        }
    }
    return true;
}

static int64_t cat_read_validation(const struct linux_syscall_frame *frame)
{
    if (frame->rdi != 0U) {
        return -LINUX_ERRNO_EBADF;
    }
    if (frame->rsi != LINUX_CAT_READ_BUFFER ||
        frame->rdx != LINUX_CAT_READ_COUNT ||
        !user_stack_writable(frame->rsi, (size_t)frame->rdx)) {
        return -LINUX_ERRNO_EFAULT;
    }
    return 0;
}

static bool cat_delivery_valid(
    uint32_t delivered_lines,
    uint32_t delivered_bytes,
    const uint8_t *bytes,
    size_t byte_count,
    bool eof
)
{
    if (eof) {
        return bytes == NULL && byte_count == 0U;
    }
    return cat_line_valid(bytes, byte_count) &&
        delivered_lines < LINUX_CAT_INPUT_LINES &&
        delivered_bytes <= LINUX_CAT_INPUT_TOTAL_BYTES &&
        byte_count <= LINUX_CAT_INPUT_TOTAL_BYTES - delivered_bytes;
}

static bool cat_wait_invariants(uint64_t process_generation)
{
    const uint64_t kernel_cr3 =
        paging_get_state().root_physical_address;

    return runtime.active &&
        runtime.context.profile == LINUX_SYSCALL_PROFILE_CAT &&
        runtime.context.address_space != NULL &&
        runtime.context.address_space->state ==
            PAGING_PROCESS_SPACE_INSTALLED &&
        runtime.state == LINUX_SYSCALL_CPU_RETURNED &&
        runtime.provenance_state == PROVENANCE_CANDIDATE &&
        runtime.cat_phase == CAT_SYSCALL_READ &&
        runtime.cat_read_state == LINUX_CAT_READ_WAITING &&
        process_generation != 0U &&
        process_generation == runtime.context.process_generation &&
        !linux_process_boundary_active() && !cpu_interrupts_enabled() &&
        (cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U)) == kernel_cr3 &&
        cat_frame_authenticated() &&
        user_stack_writable(LINUX_CAT_READ_BUFFER,
            LINUX_CAT_READ_COUNT);
}

bool linux_syscall_cat_waiting(uint64_t process_generation)
{
    return runtime.active &&
        runtime.context.profile == LINUX_SYSCALL_PROFILE_CAT &&
        runtime.context.process_generation == process_generation &&
        runtime.cat_phase == CAT_SYSCALL_READ &&
        runtime.cat_read_state == LINUX_CAT_READ_WAITING &&
        runtime.cat_saved_valid;
}

enum linux_syscall_status linux_syscall_cat_complete_read(
    uint64_t process_generation,
    const uint8_t *bytes,
    size_t byte_count,
    bool eof
)
{
    if (!cat_wait_invariants(process_generation)) {
        return process_generation != runtime.context.process_generation ?
            LINUX_SYSCALL_STATUS_BAD_GENERATION :
            LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    if (!cat_delivery_valid(runtime.result.cat_input_lines,
            runtime.result.cat_input_bytes, bytes, byte_count, eof)) {
        return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
    }
    if (!eof && !copy_to_user_stack(LINUX_CAT_READ_BUFFER, bytes,
            byte_count)) {
        return LINUX_SYSCALL_STATUS_COPYOUT;
    }
    zero_bytes(runtime.cat_output, sizeof(runtime.cat_output));
    if (!eof) {
        for (size_t index = 0U; index < byte_count; ++index) {
            runtime.cat_output[index] = bytes[index];
        }
        runtime.cat_output_bytes = (uint32_t)byte_count;
        runtime.result.cat_input_bytes += (uint32_t)byte_count;
        ++runtime.result.cat_input_lines;
        runtime.cat_phase = CAT_SYSCALL_WRITE;
    } else {
        runtime.cat_output_bytes = 0U;
        runtime.cat_eof_pending = true;
        runtime.result.cat_eof_delivered = true;
        runtime.cat_phase = CAT_SYSCALL_EXIT;
    }
    runtime.cat_saved_frame.rax = (uint64_t)byte_count;
    runtime.cat_authenticated_frame.rax = (uint64_t)byte_count;
    runtime.cat_read_state = LINUX_CAT_READ_READY;
    console_serial_write(eof ?
        "RW CAT EOF converted to zero-length read result\n" :
        "RW CAT destination validated and all-or-nothing copy-out complete\n");
    return LINUX_SYSCALL_STATUS_OK;
}

static bool cat_resume_invariants(uint64_t process_generation)
{
    const uint64_t kernel_cr3 =
        paging_get_state().root_physical_address;

    return runtime.active &&
        runtime.context.profile == LINUX_SYSCALL_PROFILE_CAT &&
        runtime.context.address_space != NULL &&
        runtime.context.address_space->state ==
            PAGING_PROCESS_SPACE_INSTALLED &&
        runtime.state == LINUX_SYSCALL_CPU_RETURNED &&
        runtime.provenance_state == PROVENANCE_CANDIDATE &&
        runtime.cat_read_state == LINUX_CAT_READ_READY &&
        process_generation == runtime.context.process_generation &&
        runtime.cat_saved_valid &&
        runtime.cat_saved_generation == process_generation &&
        runtime.cat_saved_cr3 ==
            runtime.context.address_space->root_physical_address &&
        runtime.cat_saved_ordinal == runtime.request_ordinal &&
        runtime.cat_saved_frame.rax == runtime.cat_output_bytes &&
        (runtime.cat_eof_pending || runtime.cat_output_bytes != 0U) &&
        bytes_equal(&runtime.cat_saved_frame,
            &runtime.cat_authenticated_frame,
            sizeof(runtime.cat_saved_frame)) &&
        frame_return_shape_valid(&runtime.context,
            &runtime.cat_saved_frame) &&
        user_stack_writable(LINUX_CAT_READ_BUFFER,
            LINUX_CAT_READ_COUNT) &&
        !linux_process_boundary_active() && !cpu_interrupts_enabled() &&
        (cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U)) == kernel_cr3;
}

const struct linux_syscall_frame *linux_syscall_cat_resume_frame(
    uint64_t process_generation
)
{
    if (!cat_resume_invariants(process_generation)) {
        return NULL;
    }
    runtime.cat_read_state = LINUX_CAT_READ_RESUMED;
    ++runtime.result.cat_resume_count;
    console_serial_write(
        "RW CAT authenticated process generation ready to resume\n");
    return &runtime.cat_saved_frame;
}

static enum linux_syscall_status validate_entry(
    const struct linux_syscall_frame *frame
)
{
    const uintptr_t stack_pointer = cpu_read_stack_pointer();
    uint8_t instruction[2];

    if (frame == NULL) {
        return LINUX_SYSCALL_STATUS_NULL_ARGUMENT;
    }
    if (!runtime.active || runtime.context.address_space == NULL ||
        runtime.provenance_state != PROVENANCE_CANDIDATE ||
        (runtime.state != LINUX_SYSCALL_CPU_ARMED &&
            runtime.state != LINUX_SYSCALL_CPU_RETURNED)) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    if (runtime.context.process_generation == 0U ||
        runtime.request_generation != runtime.context.process_generation) {
        return LINUX_SYSCALL_STATUS_BAD_GENERATION;
    }
    if (runtime.context.address_space->state != PAGING_PROCESS_SPACE_ACTIVE ||
        (cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U)) !=
            runtime.context.address_space->root_physical_address) {
        return LINUX_SYSCALL_STATUS_BAD_CR3;
    }
    if (cpu_read_cs() != CPU_GDT_CODE_SELECTOR ||
        linux_syscall_kernel_stack == 0U ||
        stack_pointer > linux_syscall_kernel_stack ||
        stack_pointer < linux_syscall_kernel_stack -
            LINUX_KERNEL_STACK_BYTES ||
        (uintptr_t)frame > linux_syscall_kernel_stack ||
        (uintptr_t)frame < linux_syscall_kernel_stack -
            LINUX_KERNEL_STACK_BYTES) {
        return LINUX_SYSCALL_STATUS_BAD_STACK;
    }
    if (!frame_return_shape_valid(&runtime.context, frame)) {
        return LINUX_SYSCALL_STATUS_BAD_RETURN;
    }
    if (!copy_from_user(instruction, frame->rip - 2U,
            sizeof(instruction)) || instruction[0] != UINT8_C(0x0F) ||
        instruction[1] != UINT8_C(0x05)) {
        return LINUX_SYSCALL_STATUS_BAD_ENTRY;
    }
    return LINUX_SYSCALL_STATUS_OK;
}

static uintptr_t return_to_kernel(enum linux_syscall_status status)
{
    uintptr_t resume_stack;

    runtime.result.status = status;
    if (runtime.state == LINUX_SYSCALL_CPU_ENTERED) {
        (void)transition_cpu(LINUX_SYSCALL_CPU_RETURNED);
    }
    if (runtime.context.address_space != NULL &&
        runtime.context.address_space->state == PAGING_PROCESS_SPACE_ACTIVE &&
        paging_process_restore_kernel(runtime.context.address_space) !=
            PAGING_STATUS_OK) {
        runtime.result.status = LINUX_SYSCALL_STATUS_RESTORE;
    }
    resume_stack = linux_process_resume_stack();
    return resume_stack;
}

bool linux_syscall_cpu_foundation_self_test(size_t *completed_tests)
{
    struct linux_syscall_runtime saved_runtime;

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;
    if (!syscall_supported() || !cpu_user_transition_contract_valid() ||
        cpu_tss_rsp0() == 0U ||
        CPU_GDT_CODE_SELECTOR != UINT16_C(0x08) ||
        CPU_GDT_DATA_SELECTOR != UINT16_C(0x10) ||
        CPU_GDT_USER_DATA_SELECTOR != UINT16_C(0x2B) ||
        CPU_GDT_USER_CODE_SELECTOR != UINT16_C(0x33) ||
        LINUX_STAR_VALUE != ((UINT64_C(0x23) << 48U) |
            (UINT64_C(0x08) << 32U)) ||
        LINUX_FMASK_VALUE != UINT64_C(0x44700) ||
        LINUX_SYSCALL_ALLOWLIST_COUNT > LINUX_SYSCALL_ALLOWLIST_MAX ||
        sizeof(struct linux_syscall_frame) != 144U ||
        !msr_values_valid(EFER_SCE, LINUX_STAR_VALUE,
            (uint64_t)(uintptr_t)linux_syscall_entry,
            LINUX_FMASK_VALUE) ||
        msr_values_valid(0U, LINUX_STAR_VALUE,
            (uint64_t)(uintptr_t)linux_syscall_entry,
            LINUX_FMASK_VALUE) ||
        msr_values_valid(EFER_SCE, LINUX_STAR_VALUE ^ UINT64_C(1),
            (uint64_t)(uintptr_t)linux_syscall_entry,
            LINUX_FMASK_VALUE) ||
        msr_values_valid(EFER_SCE, LINUX_STAR_VALUE,
            UINT64_C(0x0000800000000000), LINUX_FMASK_VALUE) ||
        msr_values_valid(EFER_SCE, LINUX_STAR_VALUE,
            (uint64_t)(uintptr_t)linux_syscall_entry,
            LINUX_FMASK_VALUE ^ UINT64_C(1))) {
        return false;
    }
    if (runtime.active) {
        return false;
    }
    saved_runtime = runtime;
    zero_bytes(&runtime, sizeof(runtime));
    runtime.state = LINUX_SYSCALL_CPU_CANDIDATE;
    runtime.result.cpu_state = LINUX_SYSCALL_CPU_CANDIDATE;
    if (transition_cpu(LINUX_SYSCALL_CPU_ARMED) != LINUX_SYSCALL_STATUS_OK ||
        transition_cpu(LINUX_SYSCALL_CPU_ARMED) !=
            LINUX_SYSCALL_STATUS_BAD_STATE ||
        transition_cpu(LINUX_SYSCALL_CPU_ENTERED) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_cpu(LINUX_SYSCALL_CPU_DISARMED) !=
            LINUX_SYSCALL_STATUS_BAD_STATE ||
        transition_cpu(LINUX_SYSCALL_CPU_RETURNED) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_cpu(LINUX_SYSCALL_CPU_ENTERED) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_cpu(LINUX_SYSCALL_CPU_RETURNED) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_cpu(LINUX_SYSCALL_CPU_DISARMED) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_cpu(LINUX_SYSCALL_CPU_ENTERED) !=
            LINUX_SYSCALL_STATUS_BAD_STATE ||
        transition_stdout(STDOUT_SINK_ARMED) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_stdout(STDOUT_SINK_ARMED) !=
            LINUX_SYSCALL_STATUS_BAD_STATE ||
        transition_stdout(STDOUT_SINK_WRITTEN) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_stdout(STDOUT_SINK_ARMED) !=
            LINUX_SYSCALL_STATUS_BAD_STATE ||
        transition_stdout(STDOUT_SINK_RELEASED) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_stdout(STDOUT_SINK_WRITTEN) !=
            LINUX_SYSCALL_STATUS_BAD_STATE ||
        transition_provenance(PROVENANCE_ENTERED) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_provenance(PROVENANCE_ENTERED) !=
            LINUX_SYSCALL_STATUS_BAD_STATE ||
        transition_provenance(PROVENANCE_COMPLETED) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_provenance(PROVENANCE_CANDIDATE) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_provenance(PROVENANCE_RELEASED) !=
            LINUX_SYSCALL_STATUS_OK ||
        transition_provenance(PROVENANCE_ENTERED) !=
            LINUX_SYSCALL_STATUS_BAD_STATE) {
        runtime = saved_runtime;
        return false;
    }
    runtime = saved_runtime;
    *completed_tests = LINUX_SYSCALL_CPU_FOUNDATION_CONTROLS;
    return true;
}

bool linux_syscall_enosys_self_test(void)
{
    static const uint64_t refused[] = {
        UINT64_C(2), UINT64_C(60), UINT64_C(999), UINT64_MAX
    };

    const struct linux_syscall_runtime before = runtime;

    for (size_t index = 0U; index < sizeof(refused) / sizeof(refused[0]);
         ++index) {
        uint64_t result = 0U;

        if (!enosys_result(runtime.context.profile, refused[index], &result) ||
            (int64_t)result != -LINUX_ERRNO_ENOSYS) {
            return false;
        }
    }
    for (size_t index = 0U;
         index < profile_allowlist_count(runtime.context.profile); ++index) {
        uint64_t result = 0U;

        if (enosys_result(runtime.context.profile,
                profile_allowed(runtime.context.profile, index), &result)) {
            return false;
        }
    }
    return runtime_equal(&before, &runtime);
}

enum linux_syscall_status linux_syscall_arm(
    const struct linux_syscall_context *context
)
{
    const uint64_t kernel_stack = (uint64_t)cpu_tss_rsp0();

    if (context == NULL || context->address_space == NULL) {
        return LINUX_SYSCALL_STATUS_NULL_ARGUMENT;
    }
    if (runtime.active || linux_process_boundary_active()) {
        return LINUX_SYSCALL_STATUS_BUSY;
    }
    if (!syscall_supported()) {
        return LINUX_SYSCALL_STATUS_UNSUPPORTED_CPU;
    }
    if (context->profile >= LINUX_SYSCALL_PROFILE_COUNT ||
        !cpu_user_transition_contract_valid() || kernel_stack == 0U ||
        cpu_interrupts_enabled() ||
        context->address_space->state != PAGING_PROCESS_SPACE_INSTALLED ||
        context->process_generation == 0U ||
        context->stack_start != PAGING_LINUX_STACK_BASE ||
        context->stack_end != PAGING_LINUX_STACK_END ||
        (context->profile == LINUX_SYSCALL_PROFILE_ECHO &&
            (context->executable_start != LINUX_ABI_EXECUTABLE_START ||
                context->executable_end != LINUX_ABI_EXECUTABLE_END ||
                context->fs_address != LINUX_ABI_FS_ADDRESS ||
                context->tid_address != LINUX_ABI_TID_ADDRESS)) ||
        (context->profile == LINUX_SYSCALL_PROFILE_UNAME &&
            (context->executable_start !=
                    LINUX_UNAME_ABI_EXECUTABLE_START ||
                context->executable_end != LINUX_UNAME_ABI_EXECUTABLE_END ||
                context->fs_address != LINUX_UNAME_ABI_FS_ADDRESS ||
                context->tid_address != LINUX_UNAME_ABI_TID_ADDRESS)) ||
        (context->profile == LINUX_SYSCALL_PROFILE_CAT &&
            (context->executable_start != LINUX_CAT_ABI_EXECUTABLE_START ||
                context->executable_end != LINUX_CAT_ABI_EXECUTABLE_END ||
                context->fs_address != LINUX_CAT_ABI_FS_ADDRESS ||
                context->tid_address != LINUX_CAT_ABI_TID_ADDRESS))) {
        return LINUX_SYSCALL_STATUS_BAD_PROCESS;
    }
    for (size_t index = 0U; index < PAGING_LINUX_HEAP_PAGES; ++index) {
        if (context->profile == LINUX_SYSCALL_PROFILE_ECHO &&
            context->heap_frames[index] == 0U) {
            return LINUX_SYSCALL_STATUS_BAD_PROCESS;
        }
    }
    for (size_t index = 0U; index < PAGING_LINUX_STACK_PAGES; ++index) {
        if (context->stack_frames[index] == 0U) {
            return LINUX_SYSCALL_STATUS_BAD_PROCESS;
        }
    }
    if ((context->profile == LINUX_SYSCALL_PROFILE_ECHO &&
            context->anonymous_frame == 0U) ||
        context->exit_observed == NULL) {
        return LINUX_SYSCALL_STATUS_BAD_PROCESS;
    }
    if ((context->failure_before_ordinal != 0U &&
            context->failure_after_ordinal != 0U) ||
        context->failure_before_ordinal >
            profile_expected_calls(context->profile) ||
        context->failure_after_ordinal >
            profile_expected_calls(context->profile) ||
        context->controlled_run == context->publish_stdout ||
        ((context->failure_before_ordinal != 0U ||
            context->failure_after_ordinal != 0U) &&
                !context->controlled_run)) {
        return LINUX_SYSCALL_STATUS_BAD_PROCESS;
    }
    zero_bytes(&runtime, sizeof(runtime));
    runtime.context = *context;
    runtime.state = LINUX_SYSCALL_CPU_CANDIDATE;
    runtime.result.cpu_state = runtime.state;
    runtime.stdout_state = STDOUT_SINK_CANDIDATE;
    runtime.uts_state = LINUX_UTS_COPY_CANDIDATE;
    runtime.uname_stdout_state = LINUX_UNAME_STDOUT_CANDIDATE;
    runtime.provenance_state = PROVENANCE_CANDIDATE;
    runtime.cat_read_state = LINUX_CAT_READ_IDLE;
    runtime.cat_phase = CAT_SYSCALL_INIT_FS;
    runtime.request_generation = context->process_generation;
    runtime.saved_efer = cpu_read_msr(IA32_EFER);
    runtime.saved_star = cpu_read_msr(IA32_STAR);
    runtime.saved_lstar = cpu_read_msr(IA32_LSTAR);
    runtime.saved_fmask = cpu_read_msr(IA32_FMASK);
    runtime.saved_fs_base = cpu_read_msr(IA32_FS_BASE);
    linux_syscall_kernel_stack = kernel_stack;
    cpu_write_msr(IA32_STAR, LINUX_STAR_VALUE);
    cpu_write_msr(IA32_LSTAR, (uint64_t)(uintptr_t)linux_syscall_entry);
    cpu_write_msr(IA32_FMASK, LINUX_FMASK_VALUE);
    cpu_write_msr(IA32_EFER, runtime.saved_efer | EFER_SCE);
    runtime.active = true;
    if (transition_cpu(LINUX_SYSCALL_CPU_ARMED) != LINUX_SYSCALL_STATUS_OK ||
        (context->profile == LINUX_SYSCALL_PROFILE_ECHO &&
            (transition_stdout(STDOUT_SINK_ARMED) !=
                    LINUX_SYSCALL_STATUS_OK ||
                transition_uts(LINUX_UTS_COPY_RELEASED) !=
                    LINUX_SYSCALL_STATUS_OK ||
                transition_uname_stdout(LINUX_UNAME_STDOUT_RELEASED) !=
                    LINUX_SYSCALL_STATUS_OK)) ||
        (context->profile == LINUX_SYSCALL_PROFILE_UNAME &&
            (transition_stdout(STDOUT_SINK_RELEASED) !=
                    LINUX_SYSCALL_STATUS_OK ||
                transition_uname_stdout(LINUX_UNAME_STDOUT_EMPTY) !=
                    LINUX_SYSCALL_STATUS_OK)) ||
        (context->profile == LINUX_SYSCALL_PROFILE_CAT &&
            (transition_stdout(STDOUT_SINK_RELEASED) !=
                    LINUX_SYSCALL_STATUS_OK ||
                transition_uts(LINUX_UTS_COPY_RELEASED) !=
                    LINUX_SYSCALL_STATUS_OK ||
                transition_uname_stdout(LINUX_UNAME_STDOUT_RELEASED) !=
                    LINUX_SYSCALL_STATUS_OK)) ||
        !msr_contract_valid()) {
        (void)linux_syscall_disarm();
        return LINUX_SYSCALL_STATUS_MSR_CONTRACT;
    }
    runtime.result.status = LINUX_SYSCALL_STATUS_OK;
    return LINUX_SYSCALL_STATUS_OK;
}

enum linux_syscall_status linux_syscall_validate_armed(void)
{
    if (!runtime.active || runtime.state != LINUX_SYSCALL_CPU_ARMED ||
        runtime.context.address_space == NULL) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    if (!msr_contract_valid() || linux_syscall_kernel_stack != cpu_tss_rsp0()) {
        return LINUX_SYSCALL_STATUS_MSR_CONTRACT;
    }
    return LINUX_SYSCALL_STATUS_OK;
}

static enum linux_syscall_status map_heap(void)
{
    for (size_t index = 0U; index < PAGING_LINUX_HEAP_PAGES; ++index) {
        if (paging_process_map_user_page(runtime.context.address_space,
                PAGING_PROCESS_MAPPING_LINUX_HEAP,
                PAGING_LINUX_HEAP_BASE + index * PAGING_PAGE_SIZE,
                runtime.context.heap_frames[index], PAGING_WRITE) !=
                PAGING_STATUS_OK) {
            for (size_t rollback = 0U; rollback < index; ++rollback) {
                (void)paging_process_unmap_user_page(
                    runtime.context.address_space,
                    PAGING_PROCESS_MAPPING_LINUX_HEAP,
                    PAGING_LINUX_HEAP_BASE + rollback * PAGING_PAGE_SIZE);
                runtime.heap_mapped[rollback] = false;
            }
            return LINUX_SYSCALL_STATUS_MAPPING;
        }
        runtime.heap_mapped[index] = true;
    }
    return LINUX_SYSCALL_STATUS_OK;
}

static bool echo_arguments_match(
    const struct linux_syscall_runtime *candidate,
    size_t call_index,
    const struct linux_syscall_frame *frame
)
{
    if (candidate == NULL || frame == NULL) {
        return false;
    }
    switch (call_index) {
    case 0U:
        return frame->rdi == ARCH_SET_FS &&
            frame->rsi == candidate->context.fs_address;
    case 1U:
        return frame->rdi == candidate->context.tid_address;
    case 2U:
        return frame->rdi == 0U;
    case 3U:
        return frame->rdi == PAGING_LINUX_HEAP_BASE +
            PAGING_LINUX_HEAP_PAGES * PAGING_PAGE_SIZE;
    case 4U:
        return frame->rdi == PAGING_LINUX_HEAP_BASE &&
            frame->rsi == PAGING_PAGE_SIZE && frame->rdx == 0U &&
            frame->r10 == UINT64_C(0x32) && frame->r8 == UINT64_MAX &&
            frame->r9 == 0U && candidate->heap_mapped[0];
    case 5U:
        return frame->rdi == 0U && frame->rsi == PAGING_PAGE_SIZE &&
            frame->rdx == UINT64_C(3) &&
            frame->r10 == UINT64_C(0x22) && frame->r8 == UINT64_MAX &&
            frame->r9 == 0U && !candidate->anonymous_mapped;
    case 6U:
        return frame->rdi == 1U &&
            frame->rdx == LINUX_SYSCALL_STDOUT_BYTES &&
            candidate->stdout_state == STDOUT_SINK_ARMED;
    case 7U:
        return frame->rdi == PAGING_LINUX_ANON_ADDRESS &&
            frame->rsi == PAGING_PAGE_SIZE && candidate->anonymous_mapped;
    case 8U:
        return frame->rdi == 0U &&
            candidate->stdout_state == STDOUT_SINK_WRITTEN &&
            !candidate->anonymous_mapped;
    default:
        return false;
    }
}

static bool uname_arguments_match(
    const struct linux_syscall_runtime *candidate,
    size_t call_index,
    const struct linux_syscall_frame *frame
)
{
    if (candidate == NULL || frame == NULL) {
        return false;
    }
    switch (call_index) {
    case 0U:
        return frame->rdi == ARCH_SET_FS &&
            frame->rsi == candidate->context.fs_address;
    case 1U:
        return frame->rdi == candidate->context.tid_address;
    case 2U:
        return user_range_shape_valid(frame->rdi, LINUX_UTS_BYTES) &&
            candidate->uts_state == LINUX_UTS_COPY_CANDIDATE;
    case 3U:
        return frame->rdi == 1U && frame->rsi == LINUX_TIOCGWINSZ &&
            user_range_shape_valid(frame->rdx, 8U) &&
            candidate->uts_state == LINUX_UTS_COPY_COMPLETED;
    case 4U:
        return frame->rdi == 1U && frame->rdx == 2U &&
            user_range_shape_valid(frame->rsi,
                2U * sizeof(struct linux_iovec)) &&
            candidate->uts_state == LINUX_UTS_COPY_COMPLETED &&
            candidate->uname_stdout_state == LINUX_UNAME_STDOUT_EMPTY;
    case 5U:
        return frame->rdi == 0U &&
            candidate->uts_state == LINUX_UTS_COPY_COMPLETED &&
            candidate->uname_stdout_state == LINUX_UNAME_STDOUT_VALID;
    default:
        return false;
    }
}

static bool arguments_match(
    const struct linux_syscall_runtime *candidate,
    size_t call_index,
    const struct linux_syscall_frame *frame
)
{
    if (candidate == NULL) {
        return false;
    }
    return candidate->context.profile == LINUX_SYSCALL_PROFILE_UNAME ?
        uname_arguments_match(candidate, call_index, frame) :
        echo_arguments_match(candidate, call_index, frame);
}

static void expected_arguments(
    struct linux_syscall_runtime *candidate,
    size_t call_index,
    struct linux_syscall_frame *frame
)
{
    zero_bytes(candidate, sizeof(*candidate));
    zero_bytes(frame, sizeof(*frame));
    candidate->context.fs_address = LINUX_ABI_FS_ADDRESS;
    candidate->context.tid_address = LINUX_ABI_TID_ADDRESS;
    candidate->context.profile = LINUX_SYSCALL_PROFILE_ECHO;
    switch (call_index) {
    case 0U:
        frame->rdi = ARCH_SET_FS;
        frame->rsi = candidate->context.fs_address;
        break;
    case 1U:
        frame->rdi = candidate->context.tid_address;
        break;
    case 2U:
        break;
    case 3U:
        frame->rdi = PAGING_LINUX_HEAP_BASE +
            PAGING_LINUX_HEAP_PAGES * PAGING_PAGE_SIZE;
        break;
    case 4U:
        frame->rdi = PAGING_LINUX_HEAP_BASE;
        frame->rsi = PAGING_PAGE_SIZE;
        frame->r10 = UINT64_C(0x32);
        frame->r8 = UINT64_MAX;
        candidate->heap_mapped[0] = true;
        break;
    case 5U:
        frame->rsi = PAGING_PAGE_SIZE;
        frame->rdx = UINT64_C(3);
        frame->r10 = UINT64_C(0x22);
        frame->r8 = UINT64_MAX;
        break;
    case 6U:
        frame->rdi = 1U;
        frame->rdx = LINUX_SYSCALL_STDOUT_BYTES;
        candidate->stdout_state = STDOUT_SINK_ARMED;
        break;
    case 7U:
        frame->rdi = PAGING_LINUX_ANON_ADDRESS;
        frame->rsi = PAGING_PAGE_SIZE;
        candidate->anonymous_mapped = true;
        break;
    case 8U:
        candidate->stdout_state = STDOUT_SINK_WRITTEN;
        break;
    default:
        break;
    }
}

bool linux_syscall_semantic_self_test(void)
{
    struct linux_syscall_runtime candidate;
    struct linux_syscall_frame frame;

    for (size_t call = 0U; call < LINUX_SYSCALL_EXPECTED_CALLS; ++call) {
        struct linux_syscall_frame changed;

        expected_arguments(&candidate, call, &frame);
        if (!arguments_match(&candidate, call, &frame)) {
            return false;
        }
        changed = frame;
        changed.rdi ^= UINT64_C(1);
        if (arguments_match(&candidate, call, &changed)) {
            return false;
        }
        if (call == 0U || call == 4U || call == 5U || call == 7U) {
            changed = frame;
            changed.rsi ^= UINT64_C(1);
            if (arguments_match(&candidate, call, &changed)) {
                return false;
            }
        }
        if (call == 4U || call == 5U || call == 6U) {
            changed = frame;
            changed.rdx ^= UINT64_C(1);
            if (arguments_match(&candidate, call, &changed)) {
                return false;
            }
        }
        if (call == 4U || call == 5U) {
            changed = frame;
            changed.r10 ^= UINT64_C(1);
            if (arguments_match(&candidate, call, &changed)) {
                return false;
            }
            changed = frame;
            changed.r8 ^= UINT64_C(1);
            if (arguments_match(&candidate, call, &changed)) {
                return false;
            }
            changed = frame;
            changed.r9 ^= UINT64_C(1);
            if (arguments_match(&candidate, call, &changed)) {
                return false;
            }
        }
    }
    if (arguments_match(&candidate, LINUX_SYSCALL_EXPECTED_CALLS, &frame)) {
        return false;
    }
    for (size_t allowed = 0U; allowed < LINUX_SYSCALL_ALLOWLIST_COUNT;
         ++allowed) {
        bool seen = false;

        for (size_t call = 0U; call < LINUX_SYSCALL_EXPECTED_CALLS; ++call) {
            if (expected_calls[call] == allowlist[allowed]) {
                seen = true;
            }
        }
        if (!seen) {
            return false;
        }
    }
    zero_bytes(&candidate, sizeof(candidate));
    zero_bytes(&frame, sizeof(frame));
    candidate.context.executable_start = UINT64_C(0x400000);
    candidate.context.executable_end = UINT64_C(0x401000);
    candidate.context.stack_start = UINT64_C(0x500000);
    candidate.context.stack_end = UINT64_C(0x504000);
    frame.cs = CPU_GDT_USER_CODE_SELECTOR;
    frame.ss = CPU_GDT_USER_DATA_SELECTOR;
    frame.rflags = UINT64_C(2);
    frame.rip = candidate.context.executable_start + 2U;
    frame.rsp = candidate.context.stack_end - 8U;
    if (!frame_return_shape_valid(&candidate.context, &frame)) {
        return false;
    }
    frame.cs = CPU_GDT_CODE_SELECTOR;
    if (frame_return_shape_valid(&candidate.context, &frame)) {
        return false;
    }
    frame.cs = CPU_GDT_USER_CODE_SELECTOR;
    frame.ss = CPU_GDT_DATA_SELECTOR;
    if (frame_return_shape_valid(&candidate.context, &frame)) {
        return false;
    }
    frame.ss = CPU_GDT_USER_DATA_SELECTOR;
    frame.rflags = UINT64_C(0x1002);
    if (frame_return_shape_valid(&candidate.context, &frame)) {
        return false;
    }
    frame.rflags = UINT64_C(2);
    frame.rip = candidate.context.executable_end;
    if (frame_return_shape_valid(&candidate.context, &frame)) {
        return false;
    }
    frame.rip = candidate.context.executable_start + 2U;
    frame.rsp = candidate.context.stack_start - 1U;
    if (frame_return_shape_valid(&candidate.context, &frame)) {
        return false;
    }
    return LINUX_SYSCALL_ALLOWLIST_COUNT <= LINUX_SYSCALL_ALLOWLIST_MAX &&
        user_range_shape_valid(UINT64_C(0x1000), PAGING_PAGE_SIZE) &&
        !user_range_shape_valid(UINT64_C(0x1000), 0U) &&
        !user_range_shape_valid(UINT64_MAX, 2U) &&
        !user_range_shape_valid(UINT64_C(0x0000800000000000), 1U) &&
        !user_range_shape_valid(UINT64_C(0x00007FFFFFFFFFFF), 2U);
}

static bool uts_field_valid(
    const char field[LINUX_UTS_FIELD_BYTES],
    const char *value,
    size_t value_bytes
)
{
    if (value == NULL || value_bytes == 0U ||
        value_bytes >= LINUX_UTS_FIELD_BYTES ||
        !bytes_equal(field, value, value_bytes) || field[value_bytes] != '\0') {
        return false;
    }
    for (size_t index = value_bytes + 1U;
         index < LINUX_UTS_FIELD_BYTES; ++index) {
        if (field[index] != '\0') {
            return false;
        }
    }
    return true;
}

bool linux_syscall_uname_semantic_self_test(void)
{
    struct linux_syscall_runtime candidate;
    struct linux_syscall_frame frame;

    if (!uts_field_valid(phipia_uts_record.sysname, "Linux", 5U) ||
        !uts_field_valid(phipia_uts_record.nodename, "phipia", 6U) ||
        !uts_field_valid(phipia_uts_record.release, "2.2.0-phipia", 12U) ||
        !uts_field_valid(phipia_uts_record.version, "Phipia", 6U) ||
        !uts_field_valid(phipia_uts_record.machine, "x86_64", 6U) ||
        !uts_field_valid(phipia_uts_record.domainname, "(none)", 6U) ||
        sizeof(phipia_uts_record) != 390U ||
        LINUX_UNAME_SYSCALL_ALLOWLIST_COUNT >
            LINUX_SYSCALL_ALLOWLIST_MAX) {
        return false;
    }
    zero_bytes(&candidate, sizeof(candidate));
    zero_bytes(&frame, sizeof(frame));
    candidate.context.profile = LINUX_SYSCALL_PROFILE_UNAME;
    candidate.context.fs_address = LINUX_UNAME_ABI_FS_ADDRESS;
    candidate.context.tid_address = LINUX_UNAME_ABI_TID_ADDRESS;
    candidate.uts_state = LINUX_UTS_COPY_CANDIDATE;
    candidate.uname_stdout_state = LINUX_UNAME_STDOUT_EMPTY;
    for (size_t call = 0U; call < LINUX_UNAME_SYSCALL_EXPECTED_CALLS;
         ++call) {
        zero_bytes(&frame, sizeof(frame));
        switch (call) {
        case 0U:
            frame.rdi = ARCH_SET_FS;
            frame.rsi = candidate.context.fs_address;
            break;
        case 1U:
            frame.rdi = candidate.context.tid_address;
            break;
        case 2U:
            frame.rdi = UINT64_C(0x500000);
            break;
        case 3U:
            candidate.uts_state = LINUX_UTS_COPY_COMPLETED;
            frame.rdi = 1U;
            frame.rsi = LINUX_TIOCGWINSZ;
            frame.rdx = UINT64_C(0x500000);
            break;
        case 4U:
            frame.rdi = 1U;
            frame.rsi = UINT64_C(0x500000);
            frame.rdx = 2U;
            break;
        case 5U:
            candidate.uname_stdout_state = LINUX_UNAME_STDOUT_VALID;
            break;
        default:
            return false;
        }
        if (!uname_arguments_match(&candidate, call, &frame)) {
            return false;
        }
        if (call == 2U) {
            frame.rdi = 0U;
        } else {
            frame.rdi ^= UINT64_C(1);
        }
        if (uname_arguments_match(&candidate, call, &frame)) {
            return false;
        }
    }
    for (size_t allowed = 0U;
         allowed < LINUX_UNAME_SYSCALL_ALLOWLIST_COUNT; ++allowed) {
        bool seen = false;

        for (size_t call = 0U; call < LINUX_UNAME_SYSCALL_EXPECTED_CALLS;
             ++call) {
            if (uname_expected_calls[call] == uname_allowlist[allowed]) {
                seen = true;
            }
        }
        if (!seen) {
            return false;
        }
    }
    return true;
}

bool linux_syscall_cat_semantic_self_test(void)
{
    struct linux_syscall_frame frame;
    struct linux_syscall_frame authenticated;
    uint8_t line[LINUX_CAT_INPUT_LINE_BYTES + 1U];

    zero_bytes(&frame, sizeof(frame));
    frame.rax = 0U;
    frame.rdi = 0U;
    frame.rsi = LINUX_CAT_READ_BUFFER;
    frame.rdx = LINUX_CAT_READ_COUNT;
    frame.rip = LINUX_CAT_ABI_EXECUTABLE_START + 2U;
    frame.cs = CPU_GDT_USER_CODE_SELECTOR;
    frame.rflags = 2U;
    frame.rsp = PAGING_LINUX_STACK_END - 16U;
    frame.ss = CPU_GDT_USER_DATA_SELECTOR;
    authenticated = frame;
    if (!bytes_equal(&frame, &authenticated, sizeof(frame))) {
        return false;
    }
#define CAT_FRAME_MUTATION(field) \
    do { \
        struct linux_syscall_frame changed = frame; \
        changed.field ^= UINT64_C(1); \
        if (bytes_equal(&changed, &authenticated, sizeof(changed))) { \
            return false; \
        } \
    } while (false)
    CAT_FRAME_MUTATION(rip);
    CAT_FRAME_MUTATION(rsp);
    CAT_FRAME_MUTATION(cs);
    CAT_FRAME_MUTATION(ss);
    CAT_FRAME_MUTATION(rflags);
    CAT_FRAME_MUTATION(rdi);
    CAT_FRAME_MUTATION(rsi);
    CAT_FRAME_MUTATION(rdx);
#undef CAT_FRAME_MUTATION

    for (size_t index = 0U; index < LINUX_CAT_INPUT_LINE_BYTES; ++index) {
        line[index] = (uint8_t)'x';
    }
    line[LINUX_CAT_INPUT_LINE_BYTES] = (uint8_t)'\n';
    if (!cat_line_valid(line, sizeof(line)) ||
        cat_line_valid(NULL, sizeof(line)) || cat_line_valid(line, 0U) ||
        cat_line_valid(line, sizeof(line) + 1U)) {
        return false;
    }
    line[0] = UINT8_C(0x1F);
    if (cat_line_valid(line, sizeof(line))) {
        return false;
    }
    line[0] = (uint8_t)'x';
    line[sizeof(line) - 1U] = (uint8_t)'x';
    if (cat_line_valid(line, sizeof(line))) {
        return false;
    }
    return cat_expected_number(CAT_SYSCALL_INIT_FS) == UINT64_C(158) &&
        cat_expected_number(CAT_SYSCALL_INIT_TID) == UINT64_C(218) &&
        cat_expected_number(CAT_SYSCALL_READ) == UINT64_C(0) &&
        cat_expected_number(CAT_SYSCALL_WRITE) == UINT64_C(1) &&
        cat_expected_number(CAT_SYSCALL_EXIT) == UINT64_C(231) &&
        cat_expected_number(CAT_SYSCALL_RELEASED) == UINT64_MAX &&
        LINUX_CAT_INPUT_LINES == 4U &&
        LINUX_CAT_INPUT_TOTAL_BYTES == 256U &&
        (int64_t)(uint64_t)(int64_t)-LINUX_ERRNO_EBADF ==
            -LINUX_ERRNO_EBADF &&
        (int64_t)(uint64_t)(int64_t)-LINUX_ERRNO_EFAULT ==
            -LINUX_ERRNO_EFAULT;
}

bool linux_syscall_cat_read_negative_self_test(size_t *completed_tests)
{
    struct linux_syscall_frame frame;
    uint8_t before[32];
    uint8_t after[32];
    uint8_t long_line[LINUX_CAT_INPUT_LINE_BYTES + 2U];
    static const uint8_t line[] = {'x', '\n'};
    const uint64_t invalid_pointers[] = {
        LINUX_CAT_ABI_EXECUTABLE_START,
        PAGING_LINUX_HEAP_BASE,
        PAGING_LINUX_STACK_GUARD,
        UINT64_MAX - LINUX_CAT_READ_COUNT / 2U,
        PAGING_LINUX_STACK_END - LINUX_CAT_READ_COUNT / 2U
    };

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;
    zero_bytes(&frame, sizeof(frame));
    frame.rdi = 0U;
    frame.rsi = LINUX_CAT_READ_BUFFER;
    frame.rdx = LINUX_CAT_READ_COUNT;
    if (!runtime.active ||
        runtime.context.profile != LINUX_SYSCALL_PROFILE_CAT ||
        runtime.state != LINUX_SYSCALL_CPU_ARMED ||
        runtime.context.address_space == NULL ||
        runtime.context.address_space->state !=
            PAGING_PROCESS_SPACE_INSTALLED ||
        cat_read_validation(&frame) != 0 ||
        !copy_from_user(before, LINUX_CAT_READ_BUFFER, sizeof(before))) {
        return false;
    }
    frame.rdi = 1U;
    if (cat_read_validation(&frame) != -LINUX_ERRNO_EBADF) {
        return false;
    }
    frame.rdi = 0U;
    for (size_t index = 0U;
         index < sizeof(invalid_pointers) / sizeof(invalid_pointers[0]);
         ++index) {
        frame.rsi = invalid_pointers[index];
        if (cat_read_validation(&frame) != -LINUX_ERRNO_EFAULT ||
            user_stack_writable(frame.rsi, LINUX_CAT_READ_COUNT)) {
            return false;
        }
    }
    frame.rsi = LINUX_CAT_READ_BUFFER;
    if (linux_syscall_cat_complete_read(
            runtime.context.process_generation, line, sizeof(line), false) !=
            LINUX_SYSCALL_STATUS_BAD_STATE ||
        linux_syscall_cat_complete_read(
            runtime.context.process_generation ^ UINT64_C(1),
            line, sizeof(line), false) !=
            LINUX_SYSCALL_STATUS_BAD_GENERATION ||
        !copy_from_user(after, LINUX_CAT_READ_BUFFER, sizeof(after)) ||
        !bytes_equal(before, after, sizeof(before))) {
        return false;
    }
    for (size_t index = 0U; index < sizeof(long_line) - 1U; ++index) {
        long_line[index] = (uint8_t)'x';
    }
    long_line[sizeof(long_line) - 1U] = (uint8_t)'\n';
    if (!cat_delivery_valid(0U, 0U, line, sizeof(line), false) ||
        cat_delivery_valid(0U, 0U, long_line, sizeof(long_line), false) ||
        cat_delivery_valid(LINUX_CAT_INPUT_LINES, 0U,
            line, sizeof(line), false) ||
        cat_delivery_valid(0U, LINUX_CAT_INPUT_TOTAL_BYTES - 1U,
            line, sizeof(line), false) ||
        !cat_delivery_valid(0U, 0U, NULL, 0U, true) ||
        cat_delivery_valid(0U, 0U, line, sizeof(line), true)) {
        return false;
    }
    *completed_tests = LINUX_CAT_READ_NEGATIVE_CONTROLS;
    return true;
}

bool linux_syscall_cat_resume_negative_self_test(
    uint64_t process_generation,
    size_t *completed_tests
)
{
    size_t page;
    uint64_t saved_u64;
    uintptr_t saved_frame;
    uintptr_t saved_root;

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;
    if (!cat_resume_invariants(process_generation) ||
        cat_resume_invariants(process_generation ^ UINT64_C(1))) {
        return false;
    }
    page = (size_t)((LINUX_CAT_READ_BUFFER -
        runtime.context.stack_start) / PAGING_PAGE_SIZE);
#define CAT_RESUME_MUTATION(field) \
    do { \
        runtime.cat_saved_frame.field ^= UINT64_C(1); \
        if (cat_resume_invariants(process_generation)) { \
            runtime.cat_saved_frame.field ^= UINT64_C(1); \
            return false; \
        } \
        runtime.cat_saved_frame.field ^= UINT64_C(1); \
    } while (false)
    CAT_RESUME_MUTATION(rip);
    CAT_RESUME_MUTATION(rsp);
    CAT_RESUME_MUTATION(cs);
    CAT_RESUME_MUTATION(ss);
    CAT_RESUME_MUTATION(rflags);
#undef CAT_RESUME_MUTATION
    saved_u64 = runtime.cat_saved_cr3;
    runtime.cat_saved_cr3 ^= PAGING_PAGE_SIZE;
    if (cat_resume_invariants(process_generation)) {
        runtime.cat_saved_cr3 = saved_u64;
        return false;
    }
    runtime.cat_saved_cr3 = saved_u64;
    if (page >= PAGING_LINUX_STACK_PAGES) {
        return false;
    }
    saved_frame = runtime.context.stack_frames[page];
    runtime.context.stack_frames[page] = 0U;
    if (cat_resume_invariants(process_generation)) {
        runtime.context.stack_frames[page] = saved_frame;
        return false;
    }
    runtime.context.stack_frames[page] = saved_frame;
    saved_root = runtime.context.address_space->root_physical_address;
    runtime.context.address_space->root_physical_address ^=
        PAGING_PAGE_SIZE;
    if (cat_resume_invariants(process_generation)) {
        runtime.context.address_space->root_physical_address = saved_root;
        return false;
    }
    runtime.context.address_space->root_physical_address = saved_root;
    if (!cat_resume_invariants(process_generation)) {
        return false;
    }
    *completed_tests = LINUX_CAT_RESUME_NEGATIVE_CONTROLS;
    return true;
}

bool linux_syscall_uname_copyout_self_test(size_t *completed_tests)
{
    uint8_t before[LINUX_UTS_BYTES];
    uint8_t after[LINUX_UTS_BYTES];
    uint8_t prefix_before[LINUX_UTS_BYTES / 2U];
    uint8_t prefix_after[LINUX_UTS_BYTES / 2U];
    const uint64_t valid = runtime.context.stack_start + PAGING_PAGE_SIZE -
        LINUX_UTS_BYTES / 2U;
    const uint64_t invalid_cross = runtime.context.stack_end -
        LINUX_UTS_BYTES / 2U;

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;
    if (!runtime.active ||
        runtime.context.profile != LINUX_SYSCALL_PROFILE_UNAME ||
        runtime.state != LINUX_SYSCALL_CPU_ARMED ||
        runtime.uts_state != LINUX_UTS_COPY_CANDIDATE ||
        runtime.context.address_space == NULL ||
        runtime.context.address_space->state !=
            PAGING_PROCESS_SPACE_INSTALLED ||
        !copy_from_user(before, valid, sizeof(before)) ||
        copy_to_user_stack(0U, &phipia_uts_record,
            sizeof(phipia_uts_record)) ||
        copy_to_user_stack(UINT64_MAX - 1U, &phipia_uts_record,
            sizeof(phipia_uts_record)) ||
        copy_to_user_stack(UINT64_C(0x0000800000000000),
            &phipia_uts_record, sizeof(phipia_uts_record)) ||
        copy_to_user_stack(PAGING_LINUX_STACK_GUARD,
            &phipia_uts_record, sizeof(phipia_uts_record)) ||
        copy_to_user_stack(runtime.context.executable_start,
            &phipia_uts_record, sizeof(phipia_uts_record)) ||
        !copy_from_user(prefix_before, invalid_cross,
            sizeof(prefix_before)) ||
        copy_to_user_stack(invalid_cross, &phipia_uts_record,
            sizeof(phipia_uts_record)) ||
        !copy_from_user(prefix_after, invalid_cross,
            sizeof(prefix_after)) ||
        !bytes_equal(prefix_before, prefix_after, sizeof(prefix_before)) ||
        !copy_to_user_stack(valid, &phipia_uts_record,
            sizeof(phipia_uts_record)) ||
        !copy_from_user(after, valid, sizeof(after)) ||
        !bytes_equal(after, &phipia_uts_record,
            sizeof(phipia_uts_record)) ||
        !copy_to_user_stack(valid, before, sizeof(before))) {
        return false;
    }
    *completed_tests = LINUX_UNAME_COPYOUT_CONTROLS;
    return true;
}

static enum linux_syscall_status execute_echo_call(
    struct linux_syscall_frame *frame,
    uint64_t *return_value
)
{
    if (!arguments_match(&runtime, runtime.call_index, frame)) {
        return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
    }
    switch (runtime.call_index) {
    case 0U:
        if (!user_writable(frame->rsi, sizeof(uint64_t))) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        cpu_write_msr(IA32_FS_BASE, frame->rsi);
        if (cpu_read_msr(IA32_FS_BASE) != frame->rsi) {
            return LINUX_SYSCALL_STATUS_MSR_CONTRACT;
        }
        *return_value = 0U;
        return LINUX_SYSCALL_STATUS_OK;
    case 1U:
        if (!user_writable(frame->rdi, sizeof(uint32_t))) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        *return_value = 1U;
        return LINUX_SYSCALL_STATUS_OK;
    case 2U:
        *return_value = PAGING_LINUX_HEAP_BASE;
        return LINUX_SYSCALL_STATUS_OK;
    case 3U:
        if (map_heap() != LINUX_SYSCALL_STATUS_OK) {
            return LINUX_SYSCALL_STATUS_MAPPING;
        }
        *return_value = frame->rdi;
        return LINUX_SYSCALL_STATUS_OK;
    case 4U:
        if (paging_process_unmap_user_page(runtime.context.address_space,
                PAGING_PROCESS_MAPPING_LINUX_HEAP,
                PAGING_LINUX_HEAP_BASE) != PAGING_STATUS_OK) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        runtime.heap_mapped[0] = false;
        *return_value = PAGING_LINUX_HEAP_BASE;
        return LINUX_SYSCALL_STATUS_OK;
    case 5U:
        if (paging_process_map_user_page(runtime.context.address_space,
                PAGING_PROCESS_MAPPING_LINUX_ANON,
                PAGING_LINUX_ANON_ADDRESS, runtime.context.anonymous_frame,
                PAGING_WRITE) != PAGING_STATUS_OK) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        runtime.anonymous_mapped = true;
        *return_value = PAGING_LINUX_ANON_ADDRESS;
        return LINUX_SYSCALL_STATUS_OK;
    case 6U: {
        uint8_t output[LINUX_SYSCALL_STDOUT_BYTES];

        if (!copy_from_user(output, frame->rsi, sizeof(output))) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        for (size_t index = 0U; index < sizeof(output); ++index) {
            if (output[index] != expected_stdout[index]) {
                return LINUX_SYSCALL_STATUS_STDOUT;
            }
        }
        if (runtime.context.publish_stdout) {
            console_write_n((const char *)output, sizeof(output));
        }
        if (transition_stdout(STDOUT_SINK_WRITTEN) !=
                LINUX_SYSCALL_STATUS_OK) {
            return LINUX_SYSCALL_STATUS_BAD_STATE;
        }
        runtime.result.stdout_bytes = LINUX_SYSCALL_STDOUT_BYTES;
        runtime.result.stdout_valid = true;
        *return_value = LINUX_SYSCALL_STDOUT_BYTES;
        return LINUX_SYSCALL_STATUS_OK;
    }
    case 7U:
        if (paging_process_unmap_user_page(runtime.context.address_space,
                PAGING_PROCESS_MAPPING_LINUX_ANON,
                PAGING_LINUX_ANON_ADDRESS) != PAGING_STATUS_OK) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        runtime.anonymous_mapped = false;
        *return_value = 0U;
        return LINUX_SYSCALL_STATUS_OK;
    case 8U:
        if (!runtime.context.exit_observed(
                runtime.context.process_generation)) {
            return LINUX_SYSCALL_STATUS_EXIT;
        }
        runtime.result.exit_status = 0U;
        runtime.result.exit_zero = true;
        *return_value = 0U;
        return LINUX_SYSCALL_STATUS_OK;
    default:
        return LINUX_SYSCALL_STATUS_BAD_ORDER;
    }
}

static enum linux_syscall_status execute_uname_call(
    struct linux_syscall_frame *frame,
    uint64_t *return_value
)
{
    if (!uname_arguments_match(&runtime, runtime.call_index, frame)) {
        return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
    }
    switch (runtime.call_index) {
    case 0U:
        if (!user_writable(frame->rsi, sizeof(uint64_t))) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        cpu_write_msr(IA32_FS_BASE, frame->rsi);
        if (cpu_read_msr(IA32_FS_BASE) != frame->rsi) {
            return LINUX_SYSCALL_STATUS_MSR_CONTRACT;
        }
        *return_value = 0U;
        return LINUX_SYSCALL_STATUS_OK;
    case 1U:
        if (!user_writable(frame->rdi, sizeof(uint32_t))) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        *return_value = 1U;
        return LINUX_SYSCALL_STATUS_OK;
    case 2U:
        if (transition_uts(LINUX_UTS_COPY_ACTIVE) !=
                LINUX_SYSCALL_STATUS_OK) {
            return LINUX_SYSCALL_STATUS_BAD_STATE;
        }
        if (!copy_to_user_stack(frame->rdi, &phipia_uts_record,
                sizeof(phipia_uts_record))) {
            if (transition_uts(LINUX_UTS_COPY_FAILED) !=
                    LINUX_SYSCALL_STATUS_OK) {
                return LINUX_SYSCALL_STATUS_BAD_STATE;
            }
            *return_value = (uint64_t)(int64_t)-LINUX_ERRNO_EFAULT;
            return LINUX_SYSCALL_STATUS_OK;
        }
        if (transition_uts(LINUX_UTS_COPY_COMPLETED) !=
                LINUX_SYSCALL_STATUS_OK) {
            return LINUX_SYSCALL_STATUS_BAD_STATE;
        }
        runtime.result.uts_copy_valid = true;
        *return_value = 0U;
        return LINUX_SYSCALL_STATUS_OK;
    case 3U:
        *return_value = (uint64_t)(int64_t)-LINUX_ERRNO_ENOTTY;
        return LINUX_SYSCALL_STATUS_OK;
    case 4U: {
        struct linux_iovec vectors[2];
        uint8_t first[5];
        uint8_t second[1];

        if (transition_uname_stdout(LINUX_UNAME_STDOUT_RECEIVING) !=
                LINUX_SYSCALL_STATUS_OK) {
            return LINUX_SYSCALL_STATUS_BAD_STATE;
        }
        if (!copy_from_user(vectors, frame->rsi, sizeof(vectors)) ||
            vectors[0].length != sizeof(first) ||
            vectors[1].length != sizeof(second) ||
            !copy_from_user(first, vectors[0].base, sizeof(first)) ||
            !copy_from_user(second, vectors[1].base, sizeof(second)) ||
            !bytes_equal(first, uname_expected_stdout, sizeof(first)) ||
            second[0] != uname_expected_stdout[sizeof(first)]) {
            (void)transition_uname_stdout(LINUX_UNAME_STDOUT_INVALID);
            return LINUX_SYSCALL_STATUS_STDOUT;
        }
        if (transition_uname_stdout(LINUX_UNAME_STDOUT_VALID) !=
                LINUX_SYSCALL_STATUS_OK) {
            return LINUX_SYSCALL_STATUS_BAD_STATE;
        }
        if (runtime.context.publish_stdout) {
            console_write_n((const char *)uname_expected_stdout,
                sizeof(uname_expected_stdout));
        }
        runtime.result.stdout_bytes = LINUX_UNAME_SYSCALL_STDOUT_BYTES;
        runtime.result.stdout_valid = true;
        *return_value = LINUX_UNAME_SYSCALL_STDOUT_BYTES;
        return LINUX_SYSCALL_STATUS_OK;
    }
    case 5U:
        if (!runtime.context.exit_observed(
                runtime.context.process_generation)) {
            return LINUX_SYSCALL_STATUS_EXIT;
        }
        runtime.result.exit_status = 0U;
        runtime.result.exit_zero = true;
        *return_value = 0U;
        return LINUX_SYSCALL_STATUS_OK;
    default:
        return LINUX_SYSCALL_STATUS_BAD_ORDER;
    }
}

static void cat_release_saved_read(void)
{
    zero_bytes(&runtime.cat_saved_frame, sizeof(runtime.cat_saved_frame));
    zero_bytes(&runtime.cat_authenticated_frame,
        sizeof(runtime.cat_authenticated_frame));
    runtime.cat_saved_generation = 0U;
    runtime.cat_saved_cr3 = 0U;
    runtime.cat_saved_ordinal = 0U;
    runtime.cat_saved_valid = false;
}

static enum linux_syscall_status execute_cat_call(
    struct linux_syscall_frame *frame,
    uint64_t *return_value
)
{
    switch (runtime.cat_phase) {
    case CAT_SYSCALL_INIT_FS:
        if (frame->rdi != ARCH_SET_FS ||
            frame->rsi != runtime.context.fs_address ||
            !user_writable(frame->rsi, sizeof(uint64_t))) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        cpu_write_msr(IA32_FS_BASE, frame->rsi);
        if (cpu_read_msr(IA32_FS_BASE) != frame->rsi) {
            return LINUX_SYSCALL_STATUS_MSR_CONTRACT;
        }
        runtime.cat_phase = CAT_SYSCALL_INIT_TID;
        *return_value = 0U;
        return LINUX_SYSCALL_STATUS_OK;
    case CAT_SYSCALL_INIT_TID:
        if (frame->rdi != runtime.context.tid_address ||
            !user_writable(frame->rdi, sizeof(uint32_t))) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        runtime.cat_phase = CAT_SYSCALL_READ;
        *return_value = 1U;
        return LINUX_SYSCALL_STATUS_OK;
    case CAT_SYSCALL_READ: {
        const int64_t read_result = cat_read_validation(frame);

        if (read_result != 0) {
            *return_value = (uint64_t)read_result;
            return LINUX_SYSCALL_STATUS_OK;
        }
        if (runtime.cat_read_state != LINUX_CAT_READ_IDLE ||
            runtime.cat_saved_valid) {
            return LINUX_SYSCALL_STATUS_BAD_STATE;
        }
        runtime.cat_saved_frame = *frame;
        runtime.cat_authenticated_frame = *frame;
        runtime.cat_saved_generation = runtime.context.process_generation;
        runtime.cat_saved_cr3 =
            runtime.context.address_space->root_physical_address;
        runtime.cat_saved_ordinal = runtime.request_ordinal;
        runtime.cat_saved_valid = true;
        runtime.cat_read_state = LINUX_CAT_READ_WAITING;
        runtime.result.cat_wait_observed = true;
        console_serial_write("RW CAT authentic read SYSCALL observed\n");
        *return_value = 0U;
        return LINUX_SYSCALL_STATUS_OK;
    }
    case CAT_SYSCALL_WRITE: {
        uint8_t output[LINUX_CAT_INPUT_LINE_BYTES + 1U];

        if (runtime.cat_read_state != LINUX_CAT_READ_RESUMED ||
            runtime.cat_output_bytes == 0U || frame->rdi != 1U ||
            frame->rsi != LINUX_CAT_READ_BUFFER ||
            frame->rdx != runtime.cat_output_bytes ||
            !copy_from_user(output, frame->rsi,
                runtime.cat_output_bytes) ||
            !bytes_equal(output, runtime.cat_output,
                runtime.cat_output_bytes)) {
            return LINUX_SYSCALL_STATUS_STDOUT;
        }
        console_serial_write("RW CAT authentic write SYSCALL observed\n");
        if (runtime.context.publish_stdout) {
            console_serial_write("RW CAT userspace stdout begin\n");
            console_write_n((const char *)output,
                runtime.cat_output_bytes);
            console_serial_write("RW CAT userspace stdout accepted\n");
        }
        runtime.result.stdout_bytes += runtime.cat_output_bytes;
        runtime.result.stdout_valid = true;
        *return_value = runtime.cat_output_bytes;
        zero_bytes(runtime.cat_output, sizeof(runtime.cat_output));
        runtime.cat_output_bytes = 0U;
        runtime.cat_read_state = LINUX_CAT_READ_IDLE;
        runtime.cat_phase = CAT_SYSCALL_READ;
        cat_release_saved_read();
        return LINUX_SYSCALL_STATUS_OK;
    }
    case CAT_SYSCALL_EXIT:
        if (frame->rdi != 0U || !runtime.cat_eof_pending ||
            runtime.cat_read_state != LINUX_CAT_READ_RESUMED ||
            !runtime.context.exit_observed(
                runtime.context.process_generation)) {
            return LINUX_SYSCALL_STATUS_EXIT;
        }
        runtime.result.exit_status = 0U;
        runtime.result.exit_zero = true;
        runtime.result.stdout_valid = true;
        runtime.cat_eof_pending = false;
        runtime.cat_read_state = LINUX_CAT_READ_RELEASED;
        runtime.cat_phase = CAT_SYSCALL_RELEASED;
        cat_release_saved_read();
        console_serial_write("RW CAT exit status zero observed\n");
        *return_value = 0U;
        return LINUX_SYSCALL_STATUS_OK;
    case CAT_SYSCALL_RELEASED:
    default:
        return LINUX_SYSCALL_STATUS_BAD_ORDER;
    }
}

static enum linux_syscall_status execute_call(
    struct linux_syscall_frame *frame,
    uint64_t *return_value
)
{
    switch (runtime.context.profile) {
    case LINUX_SYSCALL_PROFILE_UNAME:
        return execute_uname_call(frame, return_value);
    case LINUX_SYSCALL_PROFILE_CAT:
        return execute_cat_call(frame, return_value);
    case LINUX_SYSCALL_PROFILE_ECHO:
    case LINUX_SYSCALL_PROFILE_COUNT:
    default:
        return execute_echo_call(frame, return_value);
    }
}

uintptr_t linux_syscall_dispatch(struct linux_syscall_frame *frame)
{
    enum linux_syscall_status status = validate_entry(frame);
    uint64_t return_value = (uint64_t)(int64_t)-LINUX_ERRNO_EINVAL;
    size_t allowed_index;

    if (status != LINUX_SYSCALL_STATUS_OK) {
        return return_to_kernel(status);
    }
    if (transition_cpu(LINUX_SYSCALL_CPU_ENTERED) !=
            LINUX_SYSCALL_STATUS_OK) {
        return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_STATE);
    }
    if (transition_provenance(PROVENANCE_ENTERED) !=
            LINUX_SYSCALL_STATUS_OK) {
        return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_PROVENANCE);
    }
    ++runtime.request_ordinal;
    if (runtime.context.failure_before_ordinal == runtime.request_ordinal) {
        runtime.result.controlled_failure_observed = true;
        return return_to_kernel(LINUX_SYSCALL_STATUS_CONTROLLED_FAILURE);
    }
    allowed_index = allowlist_index(runtime.context.profile, frame->rax);
    if (allowed_index == SIZE_MAX) {
        if (!enosys_result(runtime.context.profile, frame->rax, &frame->rax)) {
            return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_ARGUMENT);
        }
        if (transition_provenance(PROVENANCE_COMPLETED) !=
                LINUX_SYSCALL_STATUS_OK) {
            return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_PROVENANCE);
        }
        if (transition_cpu(LINUX_SYSCALL_CPU_RETURNED) !=
                LINUX_SYSCALL_STATUS_OK) {
            return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_STATE);
        }
        if (transition_provenance(PROVENANCE_CANDIDATE) !=
                LINUX_SYSCALL_STATUS_OK) {
            return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_PROVENANCE);
        }
        return 0U;
    }
    if ((runtime.context.profile == LINUX_SYSCALL_PROFILE_CAT &&
            (runtime.call_index >= LINUX_CAT_SYSCALL_MAX_CALLS ||
                frame->rax != cat_expected_number(runtime.cat_phase))) ||
        (runtime.context.profile != LINUX_SYSCALL_PROFILE_CAT &&
            (runtime.call_index >=
                    profile_expected_calls(runtime.context.profile) ||
                frame->rax != profile_expected(runtime.context.profile,
                    runtime.call_index)))) {
        return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_ORDER);
    }
    status = execute_call(frame, &return_value);
    if (status != LINUX_SYSCALL_STATUS_OK) {
        return return_to_kernel(status);
    }
    if (!runtime.seen[allowed_index]) {
        runtime.seen[allowed_index] = true;
        ++runtime.result.distinct_syscalls;
    }
    ++runtime.call_index;
    runtime.result.syscall_count = runtime.call_index;
    runtime.result.real_syscall_instruction = true;
    runtime.result.process_authenticated = true;
    runtime.result.cr3_authenticated = true;
    if (transition_provenance(PROVENANCE_COMPLETED) !=
            LINUX_SYSCALL_STATUS_OK) {
        return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_PROVENANCE);
    }
    frame->rax = return_value;
    if (runtime.context.failure_after_ordinal == runtime.request_ordinal) {
        runtime.result.controlled_failure_observed = true;
        return return_to_kernel(LINUX_SYSCALL_STATUS_CONTROLLED_FAILURE);
    }
    if (transition_cpu(LINUX_SYSCALL_CPU_RETURNED) !=
            LINUX_SYSCALL_STATUS_OK) {
        return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_STATE);
    }
    if (transition_provenance(PROVENANCE_CANDIDATE) !=
            LINUX_SYSCALL_STATUS_OK) {
        return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_PROVENANCE);
    }
    if (runtime.context.profile == LINUX_SYSCALL_PROFILE_CAT &&
        runtime.cat_read_state == LINUX_CAT_READ_WAITING) {
        if (paging_process_restore_kernel(runtime.context.address_space) !=
                PAGING_STATUS_OK) {
            runtime.result.status = LINUX_SYSCALL_STATUS_RESTORE;
        }
        console_serial_write(
            "RW CAT suspended with kernel CR3 and safe stack restored\n");
        return linux_process_resume_stack();
    }
    if (runtime.context.profile == LINUX_SYSCALL_PROFILE_CAT) {
        if (runtime.cat_phase != CAT_SYSCALL_RELEASED) {
            return 0U;
        }
        if (runtime.call_index < LINUX_CAT_SYSCALL_MIN_CALLS ||
            paging_process_restore_kernel(runtime.context.address_space) !=
                PAGING_STATUS_OK) {
            runtime.result.status = LINUX_SYSCALL_STATUS_RESTORE;
        }
        return linux_process_resume_stack();
    }
    if (runtime.call_index != profile_expected_calls(
            runtime.context.profile)) {
        return 0U;
    }
    if (paging_process_restore_kernel(runtime.context.address_space) !=
            PAGING_STATUS_OK) {
        runtime.result.status = LINUX_SYSCALL_STATUS_RESTORE;
    }
    return linux_process_resume_stack();
}

enum linux_syscall_status linux_syscall_disarm(void)
{
    enum linux_syscall_status result = LINUX_SYSCALL_STATUS_OK;

    if (!runtime.active) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    if (runtime.context.address_space == NULL ||
        runtime.context.address_space->state == PAGING_PROCESS_SPACE_ACTIVE ||
        linux_process_boundary_active() || cpu_interrupts_enabled() ||
        (cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U)) !=
            paging_get_state().root_physical_address ||
        (runtime.state != LINUX_SYSCALL_CPU_ARMED &&
            runtime.state != LINUX_SYSCALL_CPU_RETURNED)) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    cpu_write_msr(IA32_FS_BASE, runtime.saved_fs_base);
    cpu_write_msr(IA32_FMASK, runtime.saved_fmask);
    cpu_write_msr(IA32_LSTAR, runtime.saved_lstar);
    cpu_write_msr(IA32_STAR, runtime.saved_star);
    cpu_write_msr(IA32_EFER, runtime.saved_efer);
    if (cpu_read_msr(IA32_FS_BASE) != runtime.saved_fs_base ||
        cpu_read_msr(IA32_FMASK) != runtime.saved_fmask ||
        cpu_read_msr(IA32_LSTAR) != runtime.saved_lstar ||
        cpu_read_msr(IA32_STAR) != runtime.saved_star ||
        cpu_read_msr(IA32_EFER) != runtime.saved_efer) {
        result = LINUX_SYSCALL_STATUS_MSR_CONTRACT;
    }
    if (runtime.stdout_state != STDOUT_SINK_RELEASED &&
        transition_stdout(STDOUT_SINK_RELEASED) !=
            LINUX_SYSCALL_STATUS_OK) {
        result = LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    if (runtime.uts_state != LINUX_UTS_COPY_RELEASED &&
        transition_uts(LINUX_UTS_COPY_RELEASED) !=
            LINUX_SYSCALL_STATUS_OK) {
        result = LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    if (runtime.uname_stdout_state != LINUX_UNAME_STDOUT_RELEASED &&
        transition_uname_stdout(LINUX_UNAME_STDOUT_RELEASED) !=
            LINUX_SYSCALL_STATUS_OK) {
        result = LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    if (runtime.provenance_state != PROVENANCE_RELEASED &&
        transition_provenance(PROVENANCE_RELEASED) !=
            LINUX_SYSCALL_STATUS_OK) {
        result = LINUX_SYSCALL_STATUS_BAD_PROVENANCE;
    }
    cat_release_saved_read();
    zero_bytes(runtime.cat_output, sizeof(runtime.cat_output));
    runtime.cat_output_bytes = 0U;
    runtime.cat_eof_pending = false;
    runtime.cat_read_state = LINUX_CAT_READ_RELEASED;
    runtime.cat_phase = CAT_SYSCALL_RELEASED;
    if (transition_cpu(LINUX_SYSCALL_CPU_DISARMED) !=
            LINUX_SYSCALL_STATUS_OK) {
        result = LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    runtime.result.cpu_disarmed = result == LINUX_SYSCALL_STATUS_OK;
    if (runtime.result.status == LINUX_SYSCALL_STATUS_OK) {
        runtime.result.status = result;
    }
    runtime.active = false;
    linux_syscall_kernel_stack = 0U;
    return result;
}

struct linux_syscall_result linux_syscall_get_result(void)
{
    return runtime.result;
}

bool linux_syscall_resources_released(void)
{
    return !runtime.active && linux_syscall_kernel_stack == 0U &&
        !linux_process_boundary_active() && !runtime.cat_saved_valid &&
        runtime.cat_saved_generation == 0U &&
        runtime.cat_saved_cr3 == 0U && runtime.cat_saved_ordinal == 0U &&
        runtime.cat_output_bytes == 0U && !runtime.cat_eof_pending;
}
