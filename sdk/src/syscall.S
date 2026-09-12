/* SPDX-License-Identifier: GPL-3.0-only */
.section .text, "ax", @progbits
.macro PHIPIA_STUB name, count
.global \name
.type \name, @function
\name:
    movq %rdi, %rax
.if \count > 0
    movq %rsi, %rdi
.endif
.if \count > 1
    movq %rdx, %rsi
.endif
.if \count > 2
    movq %rcx, %rdx
.endif
.if \count > 3
    movq %r8, %r10
.endif
.if \count > 4
    movq %r9, %r8
.endif
.if \count > 5
    movq 8(%rsp), %r9
.endif
    syscall
    ret
.size \name, . - \name
.endm
PHIPIA_STUB phipia_syscall0, 0
PHIPIA_STUB phipia_syscall1, 1
PHIPIA_STUB phipia_syscall2, 2
PHIPIA_STUB phipia_syscall3, 3
PHIPIA_STUB phipia_syscall4, 4
PHIPIA_STUB phipia_syscall5, 5
PHIPIA_STUB phipia_syscall6, 6
.section .note.GNU-stack, "", @progbits
