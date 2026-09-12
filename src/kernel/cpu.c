/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/cpu.h>

#define GDT_ENTRY_COUNT 7U
#define TSS_DESCRIPTOR_INDEX 3U
#define USER_DATA_DESCRIPTOR_INDEX 5U
#define USER_CODE_DESCRIPTOR_INDEX 6U
#define TSS_ACCESS_AVAILABLE UINT8_C(0x89)
#define TSS_ACCESS_BUSY UINT8_C(0x8B)
#define STACK_CANARY_BYTES 64U
#define STACK_ALIGNMENT 4096U
#define RSP0_CANARY UINT8_C(0xA1)
#define DOUBLE_FAULT_CANARY UINT8_C(0xD1)
#define NMI_CANARY UINT8_C(0xA2)
#define MACHINE_CHECK_CANARY UINT8_C(0xA3)

struct x86_64_tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

_Static_assert(sizeof(struct descriptor_table_pointer) == 10U,
               "descriptor pointer must be ten bytes");
_Static_assert(sizeof(struct x86_64_tss) == 104U,
               "x86_64 TSS layout changed");
_Static_assert(offsetof(struct x86_64_tss, rsp0) == 4U,
               "x86_64 TSS RSP0 offset changed");
_Static_assert(offsetof(struct x86_64_tss, ist1) == 36U,
               "x86_64 TSS IST1 offset changed");
_Static_assert(offsetof(struct x86_64_tss, iomap_base) == 102U,
               "x86_64 TSS I/O map offset changed");
_Static_assert(CPU_GDT_USER_DATA_SELECTOR == UINT16_C(0x2B),
               "user data selector must name GDT index 5 at RPL3");
_Static_assert(CPU_GDT_USER_CODE_SELECTOR == UINT16_C(0x33),
               "user code selector must name GDT index 6 at RPL3");

static uint64_t gdt[GDT_ENTRY_COUNT] __attribute__((aligned(16)));
static struct x86_64_tss tss __attribute__((aligned(16)));
static uint8_t rsp0_stack[CPU_IST_STACK_SIZE] __attribute__((aligned(STACK_ALIGNMENT)));
static uint8_t double_fault_stack[CPU_IST_STACK_SIZE]
    __attribute__((aligned(STACK_ALIGNMENT)));
static uint8_t nmi_stack[CPU_IST_STACK_SIZE] __attribute__((aligned(STACK_ALIGNMENT)));
static uint8_t machine_check_stack[CPU_IST_STACK_SIZE]
    __attribute__((aligned(STACK_ALIGNMENT)));
static struct descriptor_table_pointer gdt_pointer;
static bool tables_prepared;
static bool tables_active;

static void bytes_zero(void *destination, size_t size)
{
    uint8_t *bytes = (uint8_t *)destination;

    for (size_t index = 0; index < size; ++index) {
        bytes[index] = 0U;
    }
}

static void canary_fill(uint8_t *stack, uint8_t value)
{
    for (size_t index = 0; index < STACK_CANARY_BYTES; ++index) {
        stack[index] = value;
    }
}

static bool canary_matches(const uint8_t *stack, uint8_t value)
{
    for (size_t index = 0; index < STACK_CANARY_BYTES; ++index) {
        if (stack[index] != value) {
            return false;
        }
    }

    return true;
}

static uintptr_t stack_top(uint8_t *stack)
{
    return (uintptr_t)(void *)(stack + CPU_IST_STACK_SIZE);
}

static uint64_t tss_descriptor_low(uintptr_t base, uint32_t limit, uint8_t access)
{
    uint64_t descriptor = 0;

    descriptor |= (uint64_t)(limit & UINT32_C(0xFFFF));
    descriptor |= ((uint64_t)base & UINT64_C(0xFFFFFF)) << 16U;
    descriptor |= (uint64_t)access << 40U;
    descriptor |= (uint64_t)((limit >> 16U) & UINT32_C(0xF)) << 48U;
    descriptor |= ((uint64_t)base & UINT64_C(0xFF000000)) << 32U;
    return descriptor;
}

static uint8_t tss_descriptor_access(void)
{
    return (uint8_t)((gdt[TSS_DESCRIPTOR_INDEX] >> 40U) & UINT64_C(0xFF));
}

static uint32_t tss_descriptor_limit(void)
{
    return (uint32_t)(gdt[TSS_DESCRIPTOR_INDEX] & UINT64_C(0xFFFF)) |
        (uint32_t)((gdt[TSS_DESCRIPTOR_INDEX] >> 32U) & UINT64_C(0xF0000));
}

static uintptr_t tss_descriptor_base(void)
{
    uint64_t low = gdt[TSS_DESCRIPTOR_INDEX];
    uint64_t high = gdt[TSS_DESCRIPTOR_INDEX + 1U];
    uint64_t base;

    base = (low >> 16U) & UINT64_C(0xFFFFFF);
    base |= (low >> 32U) & UINT64_C(0xFF000000);
    base |= high << 32U;
    return (uintptr_t)base;
}

static bool stack_top_valid(uintptr_t address)
{
    return address != 0U && (address & (uintptr_t)0xFU) == 0U;
}

static bool gdt_is_valid(uint8_t expected_tss_access)
{
    if (gdt[0] != 0U ||
        gdt[1] != UINT64_C(0x00AF9B000000FFFF) ||
        gdt[2] != UINT64_C(0x00CF93000000FFFF) ||
        gdt[USER_DATA_DESCRIPTOR_INDEX] !=
            UINT64_C(0x00CFF3000000FFFF) ||
        gdt[USER_CODE_DESCRIPTOR_INDEX] !=
            UINT64_C(0x00AFFB000000FFFF)) {
        return false;
    }

    if (tss_descriptor_access() != expected_tss_access ||
        tss_descriptor_limit() != sizeof(tss) - 1U ||
        tss_descriptor_base() != (uintptr_t)(void *)&tss ||
        (gdt[TSS_DESCRIPTOR_INDEX + 1U] & UINT64_C(0xFFFFFFFF00000000)) != 0U) {
        return false;
    }

    return (gdt[TSS_DESCRIPTOR_INDEX] & UINT64_C(0x00F0000000000000)) == 0U;
}

static bool tss_is_valid(void)
{
    return tss.reserved0 == 0U &&
        tss.rsp0 == stack_top(rsp0_stack) &&
        tss.rsp1 == 0U &&
        tss.rsp2 == 0U &&
        tss.reserved1 == 0U &&
        tss.ist1 == stack_top(double_fault_stack) &&
        tss.ist2 == stack_top(nmi_stack) &&
        tss.ist3 == stack_top(machine_check_stack) &&
        tss.ist4 == 0U &&
        tss.ist5 == 0U &&
        tss.ist6 == 0U &&
        tss.ist7 == 0U &&
        tss.reserved2 == 0U &&
        tss.reserved3 == 0U &&
        tss.iomap_base == sizeof(tss);
}

enum cpu_status cpu_tables_prepare(void)
{
    const uintptr_t tss_base = (uintptr_t)(void *)&tss;
    const uint32_t tss_limit = (uint32_t)(sizeof(tss) - 1U);

    if (tables_prepared) {
        return CPU_STATUS_ALREADY_PREPARED;
    }

    bytes_zero(gdt, sizeof(gdt));
    bytes_zero(&tss, sizeof(tss));
    canary_fill(rsp0_stack, RSP0_CANARY);
    canary_fill(double_fault_stack, DOUBLE_FAULT_CANARY);
    canary_fill(nmi_stack, NMI_CANARY);
    canary_fill(machine_check_stack, MACHINE_CHECK_CANARY);

    if (!stack_top_valid(stack_top(rsp0_stack)) ||
        !stack_top_valid(stack_top(double_fault_stack)) ||
        !stack_top_valid(stack_top(nmi_stack)) ||
        !stack_top_valid(stack_top(machine_check_stack))) {
        return CPU_STATUS_BAD_STACK;
    }

    tss.rsp0 = stack_top(rsp0_stack);
    tss.ist1 = stack_top(double_fault_stack);
    tss.ist2 = stack_top(nmi_stack);
    tss.ist3 = stack_top(machine_check_stack);
    tss.iomap_base = sizeof(tss);

    gdt[0] = 0U;
    /* Pre-set accessed bits so loading descriptors never writes table memory. */
    gdt[1] = UINT64_C(0x00AF9B000000FFFF);
    gdt[2] = UINT64_C(0x00CF93000000FFFF);
    gdt[TSS_DESCRIPTOR_INDEX] = tss_descriptor_low(
        tss_base,
        tss_limit,
        TSS_ACCESS_AVAILABLE
    );
    gdt[TSS_DESCRIPTOR_INDEX + 1U] = (uint64_t)tss_base >> 32U;
    gdt[USER_DATA_DESCRIPTOR_INDEX] = UINT64_C(0x00CFF3000000FFFF);
    gdt[USER_CODE_DESCRIPTOR_INDEX] = UINT64_C(0x00AFFB000000FFFF);

    gdt_pointer.limit = (uint16_t)(sizeof(gdt) - 1U);
    gdt_pointer.base = (uintptr_t)(void *)gdt;

    if (!gdt_is_valid(TSS_ACCESS_AVAILABLE)) {
        return CPU_STATUS_BAD_GDT;
    }

    if (!tss_is_valid()) {
        return CPU_STATUS_BAD_TSS;
    }

    tables_prepared = true;
    return CPU_STATUS_OK;
}

enum cpu_status cpu_tables_activate(void)
{
    if (!tables_prepared) {
        return CPU_STATUS_NOT_PREPARED;
    }

    if (tables_active) {
        return CPU_STATUS_ALREADY_ACTIVE;
    }

    cpu_load_gdt_and_tss(&gdt_pointer);
    tables_active = true;
    return cpu_tables_validate();
}

enum cpu_status cpu_tables_validate(void)
{
    struct descriptor_table_pointer observed;

    if (!tables_prepared) {
        return CPU_STATUS_NOT_PREPARED;
    }

    if (!gdt_is_valid(tables_active ? TSS_ACCESS_BUSY : TSS_ACCESS_AVAILABLE)) {
        return CPU_STATUS_BAD_GDT;
    }

    if (!tss_is_valid()) {
        return CPU_STATUS_BAD_TSS;
    }

    if (!cpu_stack_canaries_valid()) {
        return CPU_STATUS_BAD_STACK;
    }

    if (!tables_active) {
        return CPU_STATUS_OK;
    }

    cpu_store_gdt(&observed);

    if (observed.limit != gdt_pointer.limit || observed.base != gdt_pointer.base) {
        return CPU_STATUS_GDTR_MISMATCH;
    }

    if (cpu_read_task_register() != CPU_GDT_TSS_SELECTOR) {
        return CPU_STATUS_TR_MISMATCH;
    }

    return CPU_STATUS_OK;
}

bool cpu_tables_active(void)
{
    return tables_active;
}

bool cpu_user_transition_contract_valid(void)
{
    return tables_active &&
        cpu_tables_validate() == CPU_STATUS_OK &&
        stack_top_valid((uintptr_t)tss.rsp0) &&
        tss.rsp0 == stack_top(rsp0_stack);
}

uintptr_t cpu_tss_rsp0(void)
{
    return cpu_user_transition_contract_valid() ? (uintptr_t)tss.rsp0 : 0U;
}

bool cpu_address_on_ist(uint8_t ist_index, uintptr_t address)
{
    const uint8_t *stack;
    uintptr_t low;
    uintptr_t high;

    switch (ist_index) {
    case CPU_IST_DOUBLE_FAULT:
        stack = double_fault_stack;
        break;
    case CPU_IST_NMI:
        stack = nmi_stack;
        break;
    case CPU_IST_MACHINE_CHECK:
        stack = machine_check_stack;
        break;
    default:
        return false;
    }

    low = (uintptr_t)(const void *)(stack + STACK_CANARY_BYTES);
    high = (uintptr_t)(const void *)(stack + CPU_IST_STACK_SIZE);
    return address >= low && address < high;
}

bool cpu_stack_canaries_valid(void)
{
    return canary_matches(rsp0_stack, RSP0_CANARY) &&
        canary_matches(double_fault_stack, DOUBLE_FAULT_CANARY) &&
        canary_matches(nmi_stack, NMI_CANARY) &&
        canary_matches(machine_check_stack, MACHINE_CHECK_CANARY);
}

const char *cpu_status_string(enum cpu_status status)
{
    switch (status) {
    case CPU_STATUS_OK:
        return "ok";
    case CPU_STATUS_ALREADY_PREPARED:
        return "CPU tables were prepared twice";
    case CPU_STATUS_NOT_PREPARED:
        return "CPU tables are not prepared";
    case CPU_STATUS_ALREADY_ACTIVE:
        return "CPU tables were activated twice";
    case CPU_STATUS_BAD_GDT:
        return "GDT validation failed";
    case CPU_STATUS_BAD_TSS:
        return "TSS validation failed";
    case CPU_STATUS_BAD_STACK:
        return "emergency stack validation failed";
    case CPU_STATUS_GDTR_MISMATCH:
        return "GDTR does not reference the Phipia GDT";
    case CPU_STATUS_TR_MISMATCH:
        return "task register does not reference the Phipia TSS";
    default:
        return "unknown CPU table status";
    }
}
