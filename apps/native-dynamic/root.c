/* SPDX-License-Identifier: GPL-3.0-only */
#include "proof.h"

#include <phipia/abi.h>
#include <stdint.h>

static int root_initialized;
static _Thread_local int root_tls = 7;

void proof_write(const char *text, size_t length)
{
    register uint64_t number __asm__("rax") = PHIPIA_SYS_CONSOLE_WRITE;
    register uint64_t address __asm__("rdi") = (uint64_t)(uintptr_t)text;
    register uint64_t bytes __asm__("rsi") = length;

    __asm__ volatile("syscall"
        : "+a"(number)
        : "D"(address), "S"(bytes)
        : "rcx", "r11", "memory");
}

__attribute__((constructor)) static void root_initialize(void)
{
    static const char marker[] = "PHIPIA DYNAMIC ROOT INIT\n";

    ++root_tls;
    if (root_tls == 8 && dynamic_tls_value() == 42 &&
        dynamic_add(8) == 50) {
        root_initialized = 1;
    }
    proof_write(marker, sizeof(marker) - 1U);
}

__attribute__((destructor)) static void root_finalize(void)
{
    static const char marker[] = "PHIPIA DYNAMIC ROOT FINI\n";

    proof_write(marker, sizeof(marker) - 1U);
}

int dynamic_root_main(void)
{
    static const char pass[] = "PHIPIA DYNAMIC RING3 PASS\n";
    static const char fail[] = "PHIPIA DYNAMIC RING3 FAIL\n";

    if (root_initialized != 1 || root_tls != 8 ||
        dynamic_tls_value() != 42 ||
        dynamic_add(1) != 43) {
        proof_write(fail, sizeof(fail) - 1U);
        return 91;
    }
    proof_write(pass, sizeof(pass) - 1U);
    return 0;
}
