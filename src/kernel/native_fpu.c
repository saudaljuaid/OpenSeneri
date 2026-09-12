/* SPDX-License-Identifier: GPL-3.0-only */
/* Explicit x87/SSE ownership for native userspace threads. */

#include <phipia/native_fpu.h>

#include <phipia/cpu.h>

#define CPUID_FPU (UINT32_C(1) << 0U)
#define CPUID_FXSR (UINT32_C(1) << 24U)
#define CPUID_SSE (UINT32_C(1) << 25U)
#define CPUID_SSE2 (UINT32_C(1) << 26U)
#define CR0_MP (UINT64_C(1) << 1U)
#define CR0_EM (UINT64_C(1) << 2U)
#define CR0_TS (UINT64_C(1) << 3U)
#define CR0_NE (UINT64_C(1) << 5U)
#define CR4_OSFXSR (UINT64_C(1) << 9U)
#define CR4_OSXMMEXCPT (UINT64_C(1) << 10U)

_Static_assert(sizeof(struct native_fpu_state) == NATIVE_FPU_STATE_BYTES,
    "FXSAVE state size changed");
_Static_assert(_Alignof(struct native_fpu_state) ==
    NATIVE_FPU_STATE_ALIGNMENT, "FXSAVE alignment changed");

static bool ready;

static bool aligned_state(const void *state)
{
    return state != NULL &&
        ((uintptr_t)state & (NATIVE_FPU_STATE_ALIGNMENT - 1U)) == 0U;
}

bool native_fpu_initialize(void)
{
    struct cpuid_result features;
    uint64_t cr0;
    uint64_t cr4;

    if (ready) {
        return true;
    }
    cpu_cpuid(1U, 0U, &features);
    if ((features.edx & (CPUID_FPU | CPUID_FXSR | CPUID_SSE | CPUID_SSE2)) !=
            (CPUID_FPU | CPUID_FXSR | CPUID_SSE | CPUID_SSE2)) {
        return false;
    }
    cr0 = cpu_read_cr0();
    cr0 &= ~(CR0_EM | CR0_TS);
    cr0 |= CR0_MP | CR0_NE;
    cpu_write_cr0(cr0);
    cr4 = cpu_read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT;
    cpu_write_cr4(cr4);
    ready = (cpu_read_cr0() & (CR0_EM | CR0_TS)) == 0U &&
        (cpu_read_cr0() & (CR0_MP | CR0_NE)) == (CR0_MP | CR0_NE) &&
        (cpu_read_cr4() & (CR4_OSFXSR | CR4_OSXMMEXCPT)) ==
            (CR4_OSFXSR | CR4_OSXMMEXCPT);
    return ready;
}

bool native_fpu_state_initialize(struct native_fpu_state *state)
{
    if (!ready || !aligned_state(state)) {
        return false;
    }
    native_fxinit(state->bytes);
    return true;
}

bool native_fpu_save(struct native_fpu_state *state)
{
    if (!ready || !aligned_state(state)) {
        return false;
    }
    native_fxsave(state->bytes);
    return true;
}

bool native_fpu_restore(const struct native_fpu_state *state)
{
    if (!ready || !aligned_state(state)) {
        return false;
    }
    native_fxrstor(state->bytes);
    return true;
}

bool native_fpu_is_ready(void)
{
    return ready;
}

bool native_fpu_capability_self_test(size_t *completed_tests)
{
    struct cpuid_result features;

    if (completed_tests == NULL) return false;
    *completed_tests = 0U;
    cpu_cpuid(1U, 0U, &features);
    if ((features.edx & CPUID_FPU) == 0U) return false;
    ++*completed_tests;
    if ((features.edx & CPUID_FXSR) == 0U) return false;
    ++*completed_tests;
    if ((features.edx & CPUID_SSE) == 0U) return false;
    ++*completed_tests;
    if ((features.edx & CPUID_SSE2) == 0U) return false;
    ++*completed_tests;
    return true;
}

bool native_fpu_self_test(size_t *completed_tests)
{
    /* FXSAVE needs 512 aligned bytes; the boot stack need not carry it. */
    static struct native_fpu_state state;

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;
    if (!native_fpu_initialize()) {
        return false;
    }
    ++*completed_tests;
    if (!native_fpu_state_initialize(&state) ||
        state.bytes[0] != UINT8_C(0x7F) ||
        state.bytes[1] != UINT8_C(0x03) ||
        state.bytes[4] != 0U ||
        state.bytes[24] != UINT8_C(0x80) ||
        state.bytes[25] != UINT8_C(0x1F)) {
        return false;
    }
    for (size_t index = 160U; index < 416U; ++index) {
        if (state.bytes[index] != 0U) {
            return false;
        }
    }
    ++*completed_tests;
    if (!native_fpu_restore(&state) || !native_fpu_save(&state)) {
        return false;
    }
    ++*completed_tests;
    return true;
}
