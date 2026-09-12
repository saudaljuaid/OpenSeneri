/* SPDX-License-Identifier: GPL-3.0-only */
.section .text.start, "ax", @progbits
.global _start
.type _start, @function
.extern main
.extern phipia_runtime_initialize
.weak phipia_application_entry_probe
_start:
    /* Preserve the loader contract across an optional instrumented probe. */
    xorq %rbp, %rbp
    andq $-16, %rsp
    pushq %rdi
    pushq %rsi
    pushq %rdx
    subq $8, %rsp
    movabsq $phipia_application_entry_probe, %rax
    testq %rax, %rax
    jz .Lruntime_start
    call *%rax
.Lruntime_start:
    movq 24(%rsp), %rdi
    movq 16(%rsp), %rsi
    movq 8(%rsp), %rdx
    call phipia_runtime_initialize
    movq 24(%rsp), %rdi
    movq 16(%rsp), %rsi
    movq 8(%rsp), %rdx
    call main
    movslq %eax, %rdi
    movq $1, %rax
    syscall
    ud2
.size _start, . - _start
.section .note.GNU-stack, "", @progbits
