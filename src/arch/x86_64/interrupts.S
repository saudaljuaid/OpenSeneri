/* SPDX-License-Identifier: GPL-3.0-only */
/* Uniform AMD64 interrupt entry, recovery probes, and the last-resort #DF path. */

.set COM1_DATA, 0x03F8
.set COM1_STATUS, 0x03FD
.set COM1_READY, 0x20
.set QEMU_EXIT_PORT, 0x00F4
.set QEMU_DOUBLE_FAULT_EXIT, 0x17
.set SERIAL_WAIT_LIMIT, 100000

.section .text, "ax"
.code64
.extern interrupt_dispatch
.extern kernel_test_double_fault_armed
.extern process_user_resume_from_interrupt

.macro DEFINE_INTERRUPT_VECTOR number
    .global interrupt_vector_\number
    .type interrupt_vector_\number, @function
interrupt_vector_\number:
    .if \number == 8
        jmp interrupt_double_fault_entry
    .elseif (\number == 10) || (\number == 11) || (\number == 12) || \
            (\number == 13) || (\number == 14) || (\number == 17) || \
            (\number == 21) || (\number == 29) || (\number == 30)
        pushq $\number
        jmp interrupt_common_entry
    .else
        pushq $0
        pushq $\number
        jmp interrupt_common_entry
    .endif
    .size interrupt_vector_\number, . - interrupt_vector_\number
.endm

.altmacro
.set vector_number, 0
.rept 256
    DEFINE_INTERRUPT_VECTOR %vector_number
    .set vector_number, vector_number + 1
.endr
.noaltmacro

.type interrupt_common_entry, @function
interrupt_common_entry:
    cld
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rdi
    pushq %rsi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %cr2, %rax
    pushq %rax

    /* RBX is already saved and is callee-saved by the SysV C ABI. */
    movq %rsp, %rbx
    andq $-16, %rsp
    movq %rbx, %rdi
    call interrupt_dispatch
    testq %rax, %rax
    jnz .process_kernel_resume
    movq %rbx, %rsp

    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rsi
    popq %rdi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    iretq
.process_kernel_resume:
    /*
     * C supplied a nonzero canonical RSP with RSP % 16 == 8, restored the
     * kernel CR3, and authenticated this private return. The target abandons
     * the user frame and pops exactly the six registers process_enter_user
     * saved on this stack.
     */
    movq %rax, %rsp
    jmp process_user_resume_from_interrupt
.size interrupt_common_entry, . - interrupt_common_entry

/*
 * A double fault must not call ordinary C or the stateful console.  It writes a
 * bounded fixed diagnostic directly to VGA and COM1, optionally terminates the
 * dedicated QEMU scenario, then halts forever on the emergency IST stack.
 */
.type interrupt_double_fault_entry, @function
interrupt_double_fault_entry:
    cli
    cld
    cmpb $0, kernel_test_double_fault_armed(%rip)
    jne .double_fault_test
    leaq double_fault_message(%rip), %rsi
    jmp .double_fault_emit
.double_fault_test:
    leaq double_fault_test_message(%rip), %rsi
.double_fault_emit:
    movq $0xB8000, %rdi
.double_fault_next:
    movb (%rsi), %bl
    testb %bl, %bl
    jz .double_fault_done
    movb %bl, (%rdi)
    movb $0x4F, 1(%rdi)
    addq $2, %rdi

    movl $SERIAL_WAIT_LIMIT, %ecx
.double_fault_serial_wait:
    movw $COM1_STATUS, %dx
    inb %dx, %al
    testb $COM1_READY, %al
    jnz .double_fault_serial_ready
    loop .double_fault_serial_wait
    jmp .double_fault_advance
.double_fault_serial_ready:
    movw $COM1_DATA, %dx
    movb %bl, %al
    outb %al, %dx
.double_fault_advance:
    incq %rsi
    jmp .double_fault_next
.double_fault_done:
    cmpb $0, kernel_test_double_fault_armed(%rip)
    je .double_fault_halt
    movw $QEMU_EXIT_PORT, %dx
    movl $QEMU_DOUBLE_FAULT_EXIT, %eax
    outl %eax, %dx
.double_fault_halt:
    hlt
    jmp .double_fault_halt
.size interrupt_double_fault_entry, . - interrupt_double_fault_entry

.global interrupt_trigger_register_probe
.type interrupt_trigger_register_probe, @function
interrupt_trigger_register_probe:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, interrupt_probe_expected_rsp(%rip)

    movl $0x01010101, %eax
    movl $0x02020202, %ebx
    movl $0x03030303, %ecx
    movl $0x04040404, %edx
    movl $0x05050505, %ebp
    movl $0x06060606, %edi
    movl $0x07070707, %esi
    movl $0x08080808, %r8d
    movl $0x09090909, %r9d
    movl $0x10101010, %r10d
    movl $0x11111111, %r11d
    movl $0x12121212, %r12d
    movl $0x13131313, %r13d
    movl $0x14141414, %r14d
    movl $0x15151515, %r15d
    std
    int3
.global interrupt_breakpoint_after
interrupt_breakpoint_after:
    cmpq interrupt_probe_expected_rsp(%rip), %rsp
    jne .register_probe_failed
    cmpq $0x01010101, %rax
    jne .register_probe_failed
    cmpq $0x02020202, %rbx
    jne .register_probe_failed
    cmpq $0x03030303, %rcx
    jne .register_probe_failed
    cmpq $0x04040404, %rdx
    jne .register_probe_failed
    cmpq $0x05050505, %rbp
    jne .register_probe_failed
    cmpq $0x06060606, %rdi
    jne .register_probe_failed
    cmpq $0x07070707, %rsi
    jne .register_probe_failed
    cmpq $0x08080808, %r8
    jne .register_probe_failed
    cmpq $0x09090909, %r9
    jne .register_probe_failed
    cmpq $0x10101010, %r10
    jne .register_probe_failed
    cmpq $0x11111111, %r11
    jne .register_probe_failed
    cmpq $0x12121212, %r12
    jne .register_probe_failed
    cmpq $0x13131313, %r13
    jne .register_probe_failed
    cmpq $0x14141414, %r14
    jne .register_probe_failed
    cmpq $0x15151515, %r15
    jne .register_probe_failed
    pushfq
    popq %rax
    testq $0x400, %rax
    jz .register_probe_failed
    cld
    movl $1, %eax
    jmp .register_probe_restore
.register_probe_failed:
    cld
    xorl %eax, %eax
.register_probe_restore:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
.size interrupt_trigger_register_probe, . - interrupt_trigger_register_probe

.global interrupt_trigger_ist_probe
.type interrupt_trigger_ist_probe, @function
interrupt_trigger_ist_probe:
    int $0xF0
    ret
.size interrupt_trigger_ist_probe, . - interrupt_trigger_ist_probe

.global interrupt_trigger_spurious_irq7
.type interrupt_trigger_spurious_irq7, @function
interrupt_trigger_spurious_irq7:
    int $0x27
    ret
.size interrupt_trigger_spurious_irq7, . - interrupt_trigger_spurious_irq7

.global interrupt_trigger_spurious_irq15
.type interrupt_trigger_spurious_irq15, @function
interrupt_trigger_spurious_irq15:
    int $0x2F
    ret
.size interrupt_trigger_spurious_irq15, . - interrupt_trigger_spurious_irq15

.global interrupt_trigger_invalid_opcode
.type interrupt_trigger_invalid_opcode, @function
interrupt_trigger_invalid_opcode:
.global interrupt_invalid_opcode_site
interrupt_invalid_opcode_site:
    ud2
    cli
.invalid_opcode_returned:
    hlt
    jmp .invalid_opcode_returned
.size interrupt_trigger_invalid_opcode, . - interrupt_trigger_invalid_opcode

.global interrupt_trigger_page_fault
.type interrupt_trigger_page_fault, @function
interrupt_trigger_page_fault:
    movabsq $0x0000000100000000, %rax
.global interrupt_page_fault_site
interrupt_page_fault_site:
    movq (%rax), %rax
    cli
.page_fault_returned:
    hlt
    jmp .page_fault_returned
.size interrupt_trigger_page_fault, . - interrupt_trigger_page_fault

/*
 * void paging_probe_write(volatile uint8_t *target, uint8_t value)
 * A supervisor byte store at an instruction address a test can name, so the
 * paging scenario can match the faulting RIP exactly the way the page-fault
 * scenario does. It lives with the other fault probes because it is one: a
 * compiler is free to move, duplicate, widen or delete an equivalent C store,
 * and none of those would still fault at a predictable address.
 *
 * Unlike the probes above it returns when the store succeeds, so a permission
 * that failed to take shows up as a scenario failure rather than as a hang.
 */
.global paging_probe_write
.type paging_probe_write, @function
paging_probe_write:
.global paging_probe_write_site
paging_probe_write_site:
    movb %sil, (%rdi)
    ret
.size paging_probe_write, . - paging_probe_write

.global interrupt_trigger_unexpected
.type interrupt_trigger_unexpected, @function
interrupt_trigger_unexpected:
    int $0x80
    cli
.unexpected_returned:
    hlt
    jmp .unexpected_returned
.size interrupt_trigger_unexpected, . - interrupt_trigger_unexpected

.section .rodata, "a"
double_fault_message:
    .asciz "Phipia DOUBLE FAULT - HALTED\n"
double_fault_test_message:
    .asciz "Phipia DOUBLE FAULT - HALTED\nST PASS double-fault\n"

.balign 8
.global interrupt_vector_table
interrupt_vector_table:
.macro INTERRUPT_TABLE_ENTRY number
    .quad interrupt_vector_\number
.endm
.altmacro
.set table_vector, 0
.rept 256
    INTERRUPT_TABLE_ENTRY %table_vector
    .set table_vector, table_vector + 1
.endr
.noaltmacro
.global interrupt_vector_table_end
interrupt_vector_table_end:

.section .bss, "aw", @nobits
.balign 8
interrupt_probe_expected_rsp:
    .skip 8

.section .note.GNU-stack, "", @progbits
