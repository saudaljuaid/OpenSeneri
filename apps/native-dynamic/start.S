/* SPDX-License-Identifier: GPL-3.0-only */
.section .text.start, "ax", @progbits
.global _start
.type _start, @function
.extern dynamic_root_main
_start:
    xorq %rbp, %rbp
    andq $-16, %rsp
    call dynamic_root_main
    movslq %eax, %rdi
    call dynamic_exit
    ud2
.size _start, . - _start

/* Match the public SDK wrapper: SYS_EXIT executes with an 8-mod-16 RSP. */
.type dynamic_exit, @function
dynamic_exit:
    movq $1, %rax
    syscall
    ret
.size dynamic_exit, . - dynamic_exit
.section .note.GNU-stack, "", @progbits
