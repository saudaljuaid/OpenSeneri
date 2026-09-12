/* SPDX-License-Identifier: GPL-3.0-only */
/* CPU admission for the Phipia-owned native ABI, separate from Linux profiles. */

#include <phipia/native_syscall.h>

#include <phipia/cpu.h>
#include <phipia/native_process.h>
#include <phipia/process.h>

#define CPUID_EXTENDED_ROOT UINT32_C(0x80000000)
#define CPUID_EXTENDED_FEATURES UINT32_C(0x80000001)
#define CPUID_SYSCALL_SYSRET (UINT32_C(1) << 11U)
#define IA32_EFER UINT32_C(0xC0000080)
#define IA32_STAR UINT32_C(0xC0000081)
#define IA32_LSTAR UINT32_C(0xC0000082)
#define IA32_FMASK UINT32_C(0xC0000084)
#define IA32_FS_BASE UINT32_C(0xC0000100)
#define EFER_SCE UINT64_C(1)
#define NATIVE_STAR_VALUE \
    ((UINT64_C(0x23) << 48U) | (UINT64_C(0x08) << 32U))
#define NATIVE_FMASK_VALUE \
    ((UINT64_C(1) << 8U) | (UINT64_C(1) << 9U) | \
        (UINT64_C(1) << 10U) | (UINT64_C(1) << 14U) | \
        (UINT64_C(1) << 18U))
#define NATIVE_KERNEL_STACK_BYTES (64U * 1024U)
#define NATIVE_KERNEL_STACK_CANARY_BYTES 64U
#define NATIVE_KERNEL_STACK_CANARY UINT8_C(0x4E)

_Static_assert(NATIVE_KERNEL_STACK_CANARY_BYTES < NATIVE_KERNEL_STACK_BYTES,
    "native syscall stack canary consumes the stack");

_Static_assert(sizeof(struct native_syscall_frame) == 144U,
    "native syscall frame size changed");
_Static_assert(offsetof(struct native_syscall_frame, rax) == 0U &&
    offsetof(struct native_syscall_frame, r10) == 32U &&
    offsetof(struct native_syscall_frame, rip) == 104U &&
    offsetof(struct native_syscall_frame, rsp) == 128U,
    "native syscall assembly offsets changed");

struct native_syscall_runtime {
    uint64_t saved_efer;
    uint64_t saved_star;
    uint64_t saved_lstar;
    uint64_t saved_fmask;
    uint64_t saved_fs_base;
    bool active;
};

static struct native_syscall_runtime runtime;
static uint8_t native_kernel_stack[NATIVE_KERNEL_STACK_BYTES]
    __attribute__((aligned(4096)));
uint64_t native_syscall_kernel_stack;

static uintptr_t native_kernel_stack_top(void)
{
    return (uintptr_t)(void *)(native_kernel_stack +
        sizeof(native_kernel_stack));
}

static void native_kernel_stack_arm(void)
{
    for (size_t index = 0U; index < NATIVE_KERNEL_STACK_CANARY_BYTES;
         ++index) {
        native_kernel_stack[index] = NATIVE_KERNEL_STACK_CANARY;
    }
}

static bool native_kernel_stack_intact(void)
{
    for (size_t index = 0U; index < NATIVE_KERNEL_STACK_CANARY_BYTES;
         ++index) {
        if (native_kernel_stack[index] != NATIVE_KERNEL_STACK_CANARY) {
            return false;
        }
    }
    return true;
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

bool native_syscall_arm(void)
{
    if (runtime.active || !syscall_supported() || cpu_interrupts_enabled() ||
        !cpu_user_transition_contract_valid() || cpu_tss_rsp0() == 0U ||
        process_user_boundary_active()) {
        return false;
    }
    runtime.saved_efer = cpu_read_msr(IA32_EFER);
    runtime.saved_star = cpu_read_msr(IA32_STAR);
    runtime.saved_lstar = cpu_read_msr(IA32_LSTAR);
    runtime.saved_fmask = cpu_read_msr(IA32_FMASK);
    runtime.saved_fs_base = cpu_read_msr(IA32_FS_BASE);
    native_kernel_stack_arm();
    native_syscall_kernel_stack = (uint64_t)native_kernel_stack_top();
    cpu_write_msr(IA32_STAR, NATIVE_STAR_VALUE);
    cpu_write_msr(IA32_LSTAR, (uint64_t)(uintptr_t)native_syscall_entry);
    cpu_write_msr(IA32_FMASK, NATIVE_FMASK_VALUE);
    cpu_write_msr(IA32_EFER, runtime.saved_efer | EFER_SCE);
    runtime.active = cpu_read_msr(IA32_LSTAR) ==
            (uint64_t)(uintptr_t)native_syscall_entry &&
        cpu_read_msr(IA32_STAR) == NATIVE_STAR_VALUE &&
        cpu_read_msr(IA32_FMASK) == NATIVE_FMASK_VALUE;
    if (!runtime.active) {
        cpu_write_msr(IA32_EFER, runtime.saved_efer);
        cpu_write_msr(IA32_STAR, runtime.saved_star);
        cpu_write_msr(IA32_LSTAR, runtime.saved_lstar);
        cpu_write_msr(IA32_FMASK, runtime.saved_fmask);
        cpu_write_msr(IA32_FS_BASE, runtime.saved_fs_base);
        native_syscall_kernel_stack = 0U;
    }
    return runtime.active;
}

bool native_syscall_disarm(void)
{
    bool stack_intact;

    if (!runtime.active || cpu_interrupts_enabled() ||
        process_user_boundary_active()) {
        return false;
    }
    stack_intact = native_kernel_stack_intact();
    cpu_write_msr(IA32_EFER, runtime.saved_efer);
    cpu_write_msr(IA32_STAR, runtime.saved_star);
    cpu_write_msr(IA32_LSTAR, runtime.saved_lstar);
    cpu_write_msr(IA32_FMASK, runtime.saved_fmask);
    cpu_write_msr(IA32_FS_BASE, runtime.saved_fs_base);
    runtime.active = false;
    native_syscall_kernel_stack = 0U;
    return stack_intact;
}

bool native_syscall_is_active(void)
{
    return runtime.active;
}

uintptr_t native_syscall_dispatch(struct native_syscall_frame *frame)
{
    const uintptr_t stack_pointer = (uintptr_t)(void *)frame;

    if (!runtime.active || frame == NULL || !process_user_boundary_active() ||
        native_syscall_kernel_stack == 0U ||
        stack_pointer > native_syscall_kernel_stack ||
        stack_pointer < native_syscall_kernel_stack -
            NATIVE_KERNEL_STACK_BYTES + NATIVE_KERNEL_STACK_CANARY_BYTES ||
        frame->cs != CPU_GDT_USER_CODE_SELECTOR ||
        frame->ss != CPU_GDT_USER_DATA_SELECTOR ||
        frame->rip > UINT64_C(0x00007FFFFFFFFFFF) ||
        frame->rsp > UINT64_C(0x00007FFFFFFFFFFF)) {
        return 0U;
    }
    return native_process_on_syscall(frame);
}
