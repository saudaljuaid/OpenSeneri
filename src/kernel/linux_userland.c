/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/boot_ledger.h>
#include <phipia/console.h>
#include <phipia/fat32_fs.h>
#include <phipia/linux_abi.h>
#include <phipia/linux_cat.h>
#include <phipia/linux_uname.h>
#include <phipia/linux_userland.h>

static uint64_t next_generation = UINT64_C(1);
static uint64_t active_generation;
static uint32_t completed[LINUX_USERLAND_PROFILE_COUNT];

static void zero_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool ledger_authorizes(enum linux_userland_profile profile)
{
    static const enum boot_capability required[] = {
        BOOT_CAPABILITY_PAGE_TABLES_INSTALLED,
        BOOT_CAPABILITY_WRITE_XOR_EXECUTE_PROVED,
        BOOT_CAPABILITY_PHYSICAL_FRAME_ALLOCATOR_AVAILABLE,
        BOOT_CAPABILITY_HEAP_AVAILABLE,
        BOOT_CAPABILITY_NVME_FOUNDATION_AVAILABLE,
        BOOT_CAPABILITY_FAT16_FOUNDATION_AVAILABLE,
        BOOT_CAPABILITY_PRIVATE_ONE_FILE_READ_AVAILABLE,
        BOOT_CAPABILITY_PROCESS_ADDRESS_SPACE_FOUNDATION_AVAILABLE,
        BOOT_CAPABILITY_ELF64_LOADER_FOUNDATION_AVAILABLE,
        BOOT_CAPABILITY_LINUX_SYSCALL_CPU_FOUNDATION_AVAILABLE,
        BOOT_CAPABILITY_LINUX_IMAGE_STACK_FOUNDATION_AVAILABLE,
        BOOT_CAPABILITY_PHIPIA_INSTALLED_PROOF_COMPLETE
    };
    const struct boot_ledger *ledger = boot_ledger_installed();

    if (ledger == NULL || !ledger->validated || !ledger->executed ||
        ledger->status != BOOT_LEDGER_STATUS_OK || ledger->degraded ||
        !boot_ledger_fingerprint_valid(ledger)) {
        return false;
    }
    for (size_t index = 0U; index < sizeof(required) / sizeof(required[0]);
         ++index) {
        if (!boot_ledger_has_capability(ledger, required[index])) {
            return false;
        }
    }
    if (profile == LINUX_USERLAND_PROFILE_UNAME) {
        return boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_LINUX_UNAME_IMAGE_UTS_FOUNDATION_AVAILABLE);
    }
    return profile != LINUX_USERLAND_PROFILE_CAT ||
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_LINUX_CAT_IMAGE_STDIN_FOUNDATION_AVAILABLE);
}

static void evidence_selected(enum linux_userland_profile profile)
{
    console_serial_write("RW USERLAND deterministic read-only NVMe/");
    console_serial_write(phipfs_drive(PHIPFS_VOLUME_SYSTEM).mounted ?
        "FAT32" : "FAT16");
    console_serial_write(" profile selected ");
    console_serial_write(linux_userland_profile_name(profile));
    if (profile == LINUX_USERLAND_PROFILE_ECHO) {
        console_serial_write(" BUSYBOX\n");
    } else if (profile == LINUX_USERLAND_PROFILE_UNAME) {
        console_serial_write(" UNAMEBOX\n");
    } else {
        console_serial_write(" CATBOX\n");
    }
}

static void evidence_complete(const struct linux_userland_result *result)
{
    const char *profile = linux_userland_profile_name(result->profile);

    console_serial_write("RW USERLAND Rust ");
    console_serial_write(phipfs_drive(PHIPFS_VOLUME_SYSTEM).mounted ?
        "FAT32" : "FAT16");
    console_serial_write(" SHA-256 ELF64 validation passed ");
    console_serial_write(profile);
    console_serial_write(" bytes ");
    console_serial_write_u64(result->file_bytes);
    console_serial_write("\nRW USERLAND private CPL3 address space entered ");
    console_serial_write(profile);
    console_serial_write("\nRW USERLAND real SYSCALL entry observed ");
    console_serial_write(profile);
    console_serial_write("\nRW USERLAND stdout bytes accepted ");
    console_serial_write(profile);
    console_serial_write(" bytes ");
    console_serial_write_u64(result->stdout_bytes);
    console_serial_write("\nRW USERLAND exit status zero ");
    console_serial_write(profile);
    console_serial_write("\nRW USERLAND teardown complete ");
    console_serial_write(profile);
    console_serial_write(" generation ");
    console_serial_write_u64(result->generation);
    console_serial_write("\nRW USERLAND launch completed successfully ");
    console_serial_write(profile);
    console_serial_write(" ordinal ");
    console_serial_write_u64(completed[result->profile]);
    console_serial_write("\n");
}

static enum linux_userland_status finish_failure(
    enum linux_userland_status status
)
{
    active_generation = 0U;
    (void)linux_cat_abi_abort();
    if (!linux_abi_resources_released() ||
        !linux_uname_abi_resources_released() ||
        !linux_cat_abi_resources_released()) {
        return LINUX_USERLAND_STATUS_TEARDOWN;
    }
    console_serial_write("RW USERLAND launch refused and teardown complete\n");
    return status;
}

enum linux_userland_status linux_userland_launch(
    enum linux_userland_profile profile,
    struct linux_userland_result *result
)
{
    if (result == NULL) {
        return LINUX_USERLAND_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(result, sizeof(*result));
    if (profile >= LINUX_USERLAND_PROFILE_COUNT) {
        return LINUX_USERLAND_STATUS_PROFILE;
    }
    result->profile = profile;
    if (!ledger_authorizes(profile)) {
        return LINUX_USERLAND_STATUS_BOOT_LEDGER;
    }
    if (active_generation != 0U || !linux_userland_resources_released()) {
        return LINUX_USERLAND_STATUS_LAUNCH_REFUSED;
    }
    active_generation = next_generation++;
    if (next_generation == 0U) {
        next_generation = 1U;
    }
    result->generation = active_generation;
    evidence_selected(profile);

    if (profile == LINUX_USERLAND_PROFILE_ECHO) {
        struct linux_abi_proof_result proof;
        const enum linux_abi_status status = linux_abi_launch(&proof);

        if (status == LINUX_ABI_STATUS_ABSENT) {
            return finish_failure(LINUX_USERLAND_STATUS_VOLUME_ABSENT);
        }
        if (status == LINUX_ABI_STATUS_FILESYSTEM ||
            status == LINUX_ABI_STATUS_ELF) {
            return finish_failure(LINUX_USERLAND_STATUS_PROFILE_REFUSED);
        }
        if (status != LINUX_ABI_STATUS_OK || !proof.ring_three ||
            !proof.private_address_space || !proof.real_syscall_instruction ||
            !proof.stdout_valid || !proof.exit_zero ||
            !proof.write_xor_execute || !proof.kernel_cr3_restored ||
            !proof.teardown_complete || !proof.resource_census_equal) {
            return finish_failure(LINUX_USERLAND_STATUS_LAUNCH_REFUSED);
        }
        result->file_bytes = proof.file_bytes;
        result->stdout_bytes = proof.stdout_bytes;
        result->syscall_count = proof.syscall_count;
        result->exit_status = proof.exit_status;
        result->rust_validated = true;
        result->ring_three = proof.ring_three;
        result->real_syscall_entry = proof.real_syscall_instruction;
        result->stdout_valid = proof.stdout_valid;
        result->teardown_complete = proof.teardown_complete;
    } else if (profile == LINUX_USERLAND_PROFILE_UNAME) {
        struct linux_uname_abi_proof_result proof;
        const enum linux_uname_abi_status status =
            linux_uname_abi_launch(&proof);

        if (status == LINUX_UNAME_ABI_STATUS_ABSENT) {
            return finish_failure(LINUX_USERLAND_STATUS_VOLUME_ABSENT);
        }
        if (status == LINUX_UNAME_ABI_STATUS_FILESYSTEM ||
            status == LINUX_UNAME_ABI_STATUS_ELF) {
            return finish_failure(LINUX_USERLAND_STATUS_PROFILE_REFUSED);
        }
        if (status != LINUX_UNAME_ABI_STATUS_OK || !proof.ring_three ||
            !proof.private_address_space || !proof.real_syscall_instruction ||
            !proof.uts_copy_valid || !proof.stdout_valid || !proof.exit_zero ||
            !proof.write_xor_execute || !proof.kernel_cr3_restored ||
            !proof.teardown_complete || !proof.resource_census_equal) {
            return finish_failure(LINUX_USERLAND_STATUS_LAUNCH_REFUSED);
        }
        result->file_bytes = proof.file_bytes;
        result->stdout_bytes = proof.stdout_bytes;
        result->syscall_count = proof.syscall_count;
        result->exit_status = proof.exit_status;
        result->rust_validated = true;
        result->ring_three = proof.ring_three;
        result->real_syscall_entry = proof.real_syscall_instruction;
        result->stdout_valid = proof.stdout_valid;
        result->teardown_complete = proof.teardown_complete;
    } else {
        struct linux_cat_abi_proof_result proof;
        const enum linux_cat_abi_status status = linux_cat_abi_launch(&proof);

        if (status == LINUX_CAT_ABI_STATUS_ABSENT) {
            return finish_failure(LINUX_USERLAND_STATUS_VOLUME_ABSENT);
        }
        if (status == LINUX_CAT_ABI_STATUS_FILESYSTEM ||
            status == LINUX_CAT_ABI_STATUS_ELF) {
            return finish_failure(LINUX_USERLAND_STATUS_PROFILE_REFUSED);
        }
        if (status != LINUX_CAT_ABI_STATUS_WAITING || !proof.ring_three ||
            !proof.private_address_space ||
            !proof.real_syscall_instruction || !proof.write_xor_execute ||
            !proof.kernel_cr3_restored || !proof.waiting_for_input ||
            proof.generation == 0U) {
            return finish_failure(LINUX_USERLAND_STATUS_LAUNCH_REFUSED);
        }
        active_generation = proof.generation;
        result->generation = proof.generation;
        result->file_bytes = proof.file_bytes;
        result->stdout_bytes = proof.stdout_bytes;
        result->syscall_count = proof.syscall_count;
        result->exit_status = proof.exit_status;
        result->rust_validated = true;
        result->ring_three = proof.ring_three;
        result->real_syscall_entry = proof.real_syscall_instruction;
        result->stdout_valid = proof.stdout_valid;
        result->input_bytes = proof.input_bytes;
        result->input_lines = proof.input_lines;
        result->resume_count = proof.resume_count;
        result->waiting_for_input = true;
        console_serial_write(
            "RW USERLAND cat foreground launch yielded to Phipia\n");
        return LINUX_USERLAND_STATUS_WAITING;
    }

    active_generation = 0U;
    ++completed[profile];
    evidence_complete(result);
    return LINUX_USERLAND_STATUS_OK;
}

enum linux_userland_status linux_userland_deliver_cat_input(
    const uint8_t *bytes,
    size_t byte_count,
    bool eof,
    struct linux_userland_result *result
)
{
    struct linux_cat_abi_proof_result proof;
    enum linux_cat_abi_status status;

    if (result == NULL) {
        return LINUX_USERLAND_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(result, sizeof(*result));
    result->profile = LINUX_USERLAND_PROFILE_CAT;
    result->generation = active_generation;
    if (active_generation == 0U || !linux_cat_abi_waiting() ||
        active_generation != linux_cat_abi_generation()) {
        return LINUX_USERLAND_STATUS_INPUT_REFUSED;
    }
    status = linux_cat_abi_deliver_input(bytes, byte_count, eof, &proof);
    if (status == LINUX_CAT_ABI_STATUS_INPUT) {
        return LINUX_USERLAND_STATUS_INPUT_REFUSED;
    }
    if (status != LINUX_CAT_ABI_STATUS_WAITING &&
        status != LINUX_CAT_ABI_STATUS_OK) {
        return finish_failure(status == LINUX_CAT_ABI_STATUS_TEARDOWN ?
            LINUX_USERLAND_STATUS_TEARDOWN :
            LINUX_USERLAND_STATUS_LAUNCH_REFUSED);
    }
    result->generation = active_generation;
    result->file_bytes = proof.file_bytes;
    result->stdout_bytes = proof.stdout_bytes;
    result->syscall_count = proof.syscall_count;
    result->exit_status = proof.exit_status;
    result->rust_validated = true;
    result->ring_three = proof.ring_three;
    result->real_syscall_entry = proof.real_syscall_instruction;
    result->stdout_valid = proof.stdout_valid;
    result->teardown_complete = proof.teardown_complete;
    result->input_bytes = proof.input_bytes;
    result->input_lines = proof.input_lines;
    result->resume_count = proof.resume_count;
    result->waiting_for_input = proof.waiting_for_input;
    result->eof_delivered = proof.eof_delivered;
    if (status == LINUX_CAT_ABI_STATUS_WAITING) {
        return LINUX_USERLAND_STATUS_WAITING;
    }
    active_generation = 0U;
    ++completed[LINUX_USERLAND_PROFILE_CAT];
    evidence_complete(result);
    return LINUX_USERLAND_STATUS_OK;
}

bool linux_userland_foreground_waiting(void)
{
    return active_generation != 0U && linux_cat_abi_waiting() &&
        active_generation == linux_cat_abi_generation();
}

uint64_t linux_userland_active_generation(void)
{
    return active_generation;
}

enum linux_userland_status linux_userland_abort_foreground(void)
{
    const enum linux_cat_abi_status status = linux_cat_abi_abort();

    active_generation = 0U;
    return status == LINUX_CAT_ABI_STATUS_OK ?
        LINUX_USERLAND_STATUS_OK : LINUX_USERLAND_STATUS_TEARDOWN;
}

bool linux_userland_resources_released(void)
{
    return active_generation == 0U && linux_abi_resources_released() &&
        linux_uname_abi_resources_released() &&
        linux_cat_abi_resources_released();
}

uint32_t linux_userland_completed(enum linux_userland_profile profile)
{
    return profile < LINUX_USERLAND_PROFILE_COUNT ? completed[profile] : 0U;
}

const char *linux_userland_profile_name(enum linux_userland_profile profile)
{
    static const char *const names[LINUX_USERLAND_PROFILE_COUNT] = {
        "echo", "uname", "cat"
    };

    if (profile >= LINUX_USERLAND_PROFILE_COUNT) {
        return "unknown";
    }
    return names[profile];
}

const char *linux_userland_status_string(enum linux_userland_status status)
{
    static const char *const messages[LINUX_USERLAND_STATUS_COUNT] = {
        "ok",
        "foreground userspace is waiting for input",
        "bad launch result",
        "unsupported measured profile",
        "launch boundary unavailable",
        "userspace volume unavailable",
        "measured profile refused",
        "userspace launch refused",
        "foreground input refused",
        "userspace teardown failed"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        LINUX_USERLAND_STATUS_COUNT,
        "Linux userland status table cardinality changed");
    if (status >= LINUX_USERLAND_STATUS_COUNT) {
        return "unknown userspace launch status";
    }
    return messages[status];
}
