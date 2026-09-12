/* SPDX-License-Identifier: GPL-3.0-only */
/* Pin integer, x87 and SSE state across one native blocking syscall. */

.section .text, "ax", @progbits
.code64

.global phipia_application_entry_probe
.type phipia_application_entry_probe, @function
phipia_application_entry_probe:
    fxsave64 native_initial_fpu_state(%rip)
    /* crt0 must preserve the loader's argc/argv/environment registers. */
    movq $0x11, %rdi
    movq $0x22, %rsi
    movq $0x33, %rdx
    ret
.size phipia_application_entry_probe, . - phipia_application_entry_probe

.global native_state_round_trip
.type native_state_round_trip, @function
native_state_round_trip:
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $64, %rsp

    movq %rdi, 0(%rsp)
    movq %rsi, 8(%rsp)
    movq %rsi, %rax
    notq %rax
    movq %rax, 16(%rsp)
    movdqu 8(%rsp), %xmm15
    fld1

    movq %rsi, %rbx
    leaq 1(%rsi), %r12
    leaq 2(%rsi), %r13
    leaq 3(%rsi), %r14
    leaq 4(%rsi), %r15

    movq $0x0301, %rdi
    movq 0(%rsp), %rsi
    call phipia_syscall1
    movdqu %xmm15, 24(%rsp)
    fstpt 40(%rsp)
    testq %rax, %rax
    jne .Lstate_failed
    cmpq 8(%rsp), %rbx
    jne .Lstate_failed
    movq 8(%rsp), %rdx
    leaq 1(%rdx), %rax
    cmpq %rax, %r12
    jne .Lstate_failed
    leaq 2(%rdx), %rax
    cmpq %rax, %r13
    jne .Lstate_failed
    leaq 3(%rdx), %rax
    cmpq %rax, %r14
    jne .Lstate_failed
    leaq 4(%rdx), %rax
    cmpq %rax, %r15
    jne .Lstate_failed
    cmpq %rdx, 24(%rsp)
    jne .Lstate_failed
    notq %rdx
    cmpq %rdx, 32(%rsp)
    jne .Lstate_failed
    movabsq $0x8000000000000000, %rdx
    cmpq %rdx, 40(%rsp)
    jne .Lstate_failed
    cmpw $0x3FFF, 48(%rsp)
    jne .Lstate_failed
    xorl %eax, %eax
    jmp .Lstate_done

.Lstate_failed:
    movl $1, %eax
.Lstate_done:
    addq $64, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    ret
.size native_state_round_trip, . - native_state_round_trip

.section .bss, "aw", @nobits
.balign 16
.global native_initial_fpu_state
.type native_initial_fpu_state, @object
native_initial_fpu_state:
    .skip 512
.size native_initial_fpu_state, . - native_initial_fpu_state

.section .note.GNU-stack, "", @progbits
