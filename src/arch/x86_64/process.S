/* SPDX-License-Identifier: GPL-3.0-only */
/* IRETQ entry boundary for a fresh CPL3 process. */

/* Mirrors the selectors and user-frame contract in include/phipia/cpu.h. */
.set USER_DATA_SELECTOR, 0x2B
.set USER_CODE_SELECTOR, 0x33
.set USER_RFLAGS, 0x2

.section .text, "ax"
.code64

.global process_enter_user
.type process_enter_user, @function
process_enter_user:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, process_resume_stack(%rip)
    movb $1, process_boundary_active(%rip)

    movq %rdi, %r10
    movq %rsi, %r11
    pushq $USER_DATA_SELECTOR
    pushq %r11
    pushq $USER_RFLAGS
    pushq $USER_CODE_SELECTOR
    pushq %r10

    xorl %eax, %eax
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %ebp, %ebp
    xorl %edi, %edi
    xorl %esi, %esi
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    xorl %r10d, %r10d
    xorl %r11d, %r11d
    xorl %r12d, %r12d
    xorl %r13d, %r13d
    xorl %r14d, %r14d
    xorl %r15d, %r15d
    iretq
    ud2
.size process_enter_user, . - process_enter_user

/*
 * Enter CPL3 from a saved process_user_context. The function publishes the same
 * resume state as process_enter_user so interrupts return through interrupts.S.
 * The context layout mirrors include/phipia/process.h and is checked at boot.
 */
.set CONTEXT_RAX, 0
.set CONTEXT_RBX, 8
.set CONTEXT_RCX, 16
.set CONTEXT_RDX, 24
.set CONTEXT_RSI, 32
.set CONTEXT_RDI, 40
.set CONTEXT_RBP, 48
.set CONTEXT_R8, 56
.set CONTEXT_R9, 64
.set CONTEXT_R10, 72
.set CONTEXT_R11, 80
.set CONTEXT_R12, 88
.set CONTEXT_R13, 96
.set CONTEXT_R14, 104
.set CONTEXT_R15, 112
.set CONTEXT_RIP, 120
.set CONTEXT_RSP, 128
.set CONTEXT_RFLAGS, 136

.global process_enter_user_context
.type process_enter_user_context, @function
process_enter_user_context:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, process_resume_stack(%rip)
    movb $1, process_boundary_active(%rip)

    /*
     * The IRETQ frame is built first, while RDI still names the context, and
     * every general register is loaded afterwards. RDI itself is loaded last
     * for the same reason.
     */
    pushq $USER_DATA_SELECTOR
    pushq CONTEXT_RSP(%rdi)
    pushq CONTEXT_RFLAGS(%rdi)
    pushq $USER_CODE_SELECTOR
    pushq CONTEXT_RIP(%rdi)

    movq CONTEXT_RAX(%rdi), %rax
    movq CONTEXT_RBX(%rdi), %rbx
    movq CONTEXT_RCX(%rdi), %rcx
    movq CONTEXT_RDX(%rdi), %rdx
    movq CONTEXT_RSI(%rdi), %rsi
    movq CONTEXT_RBP(%rdi), %rbp
    movq CONTEXT_R8(%rdi), %r8
    movq CONTEXT_R9(%rdi), %r9
    movq CONTEXT_R10(%rdi), %r10
    movq CONTEXT_R11(%rdi), %r11
    movq CONTEXT_R12(%rdi), %r12
    movq CONTEXT_R13(%rdi), %r13
    movq CONTEXT_R14(%rdi), %r14
    movq CONTEXT_R15(%rdi), %r15
    movq CONTEXT_RDI(%rdi), %rdi
    iretq
    ud2
.size process_enter_user_context, . - process_enter_user_context

.global process_user_resume_from_interrupt
.type process_user_resume_from_interrupt, @function
process_user_resume_from_interrupt:
    movb $0, process_boundary_active(%rip)
    movq $0, process_resume_stack(%rip)
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
.size process_user_resume_from_interrupt, . - process_user_resume_from_interrupt

.global process_user_resume_stack
.type process_user_resume_stack, @function
process_user_resume_stack:
    cmpb $0, process_boundary_active(%rip)
    je .resume_stack_inactive
    movq process_resume_stack(%rip), %rax
    ret
.resume_stack_inactive:
    xorl %eax, %eax
    ret
.size process_user_resume_stack, . - process_user_resume_stack

.global process_user_boundary_active
.type process_user_boundary_active, @function
process_user_boundary_active:
    xorl %eax, %eax
    cmpb $0, process_boundary_active(%rip)
    setne %al
    ret
.size process_user_boundary_active, . - process_user_boundary_active

.section .bss, "aw", @nobits
.balign 8
process_resume_stack:
    .skip 8
process_boundary_active:
    .skip 1

.section .note.GNU-stack, "", @progbits
