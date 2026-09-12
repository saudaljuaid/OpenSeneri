/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_NATIVE_SYSCALL_H
#define PHIPIA_NATIVE_SYSCALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct native_syscall_frame {
    uint64_t rax;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t r10;
    uint64_t r8;
    uint64_t r9;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

bool native_syscall_arm(void);
bool native_syscall_disarm(void);
bool native_syscall_is_active(void);
uintptr_t native_syscall_dispatch(struct native_syscall_frame *frame);

extern const uint8_t native_syscall_entry[];
extern uint64_t native_syscall_kernel_stack;

#endif
