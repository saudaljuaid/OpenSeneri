/* SPDX-License-Identifier: GPL-3.0-only */
/* Native Phipia ABI v1 SYSCALL entry; every call returns through the scheduler. */

.set USER_DATA_SELECTOR, 0x2B
.set USER_CODE_SELECTOR, 0x33

.section .text, "ax"
.code64

.global native_syscall_entry
.type native_syscall_entry, @function
native_syscall_entry:
    movq %rsp, native_syscall_user_rsp(%rip)
    movq native_syscall_kernel_stack(%rip), %rsp
    cld

    pushq $USER_DATA_SELECTOR
    pushq native_syscall_user_rsp(%rip)
    pushq %r11
    pushq $USER_CODE_SELECTOR
    pushq %rcx

    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %rbp
    pushq %rbx
    pushq %r9
    pushq %r8
    pushq %r10
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rax

    movq %rsp, %rdi
    call native_syscall_dispatch
    testq %rax, %rax
    jz .invalid_resume
    movq %rax, %rsp
    movq $0, native_syscall_user_rsp(%rip)
    jmp process_user_resume_from_interrupt

.invalid_resume:
    ud2
.size native_syscall_entry, . - native_syscall_entry

.section .bss, "aw", @nobits
.balign 8
native_syscall_user_rsp:
    .skip 8

.section .note.GNU-stack, "", @progbits
