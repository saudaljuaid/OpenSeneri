/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_TEST_H
#define PHIPIA_TEST_H

#include <stdbool.h>
#include <stdint.h>

#include <phipia/acpi.h>
#include <phipia/boot.h>
#include <phipia/interrupts.h>
#include <phipia/paging.h>

enum kernel_test_scenario {
    KERNEL_TEST_NONE = 0,
    KERNEL_TEST_NORMAL,
    KERNEL_TEST_BREAKPOINT,
    KERNEL_TEST_INVALID_OPCODE,
    KERNEL_TEST_PAGE_FAULT,
    KERNEL_TEST_IST,
    KERNEL_TEST_PIT,
    KERNEL_TEST_UNEXPECTED,
    KERNEL_TEST_DOUBLE_FAULT,
    KERNEL_TEST_APIC,
    KERNEL_TEST_IOAPIC,
    KERNEL_TEST_IOAPIC_LEVEL,
    KERNEL_TEST_RETIRED,
    KERNEL_TEST_APIC_TIMER,
    KERNEL_TEST_TSC,
    KERNEL_TEST_PM_TIMER,
    KERNEL_TEST_PIT_RETIRED,
    KERNEL_TEST_TIMERS,
    KERNEL_TEST_PAGING,
    KERNEL_TEST_HEAP,
    KERNEL_TEST_PCI,
    KERNEL_TEST_PCI_ECAM,
    KERNEL_TEST_THREADS,
    KERNEL_TEST_THREAD_GUARD,
    KERNEL_TEST_FRAMEBUFFER,
    KERNEL_TEST_SCREEN,
    KERNEL_TEST_KEYBOARD,
    KERNEL_TEST_SHELL,
    KERNEL_TEST_SURFACE,
    KERNEL_TEST_WRITE_COMBINING,
    KERNEL_TEST_DEVICE_WINDOWS,
    KERNEL_TEST_BOOT_LEDGER,
    KERNEL_TEST_PHIPIA_PROOF,
    KERNEL_TEST_DEVICE_SUBSTRATE,
    KERNEL_TEST_XHCI,
    KERNEL_TEST_NVME,
    KERNEL_TEST_FILESYSTEM,
    KERNEL_TEST_PROCESS,
    KERNEL_TEST_LINUX_ABI,
    KERNEL_TEST_LINUX_ABI_UNAME,
    KERNEL_TEST_PHIPIA_PROOF_USERLAND,
    KERNEL_TEST_PHIPIA_PROOF_USERLAND_ABSENT,
    KERNEL_TEST_PHIPIA_PROOF_USERLAND_INTERACTIVE,
    KERNEL_TEST_PHIPIA_PROOF_USERLAND_INTERACTIVE_ABSENT,
    KERNEL_TEST_FAT32_SYSTEM,
    KERNEL_TEST_FAT32_DATA,
    KERNEL_TEST_FAT32_NESTED,
    KERNEL_TEST_FAT32_GROWTH,
    KERNEL_TEST_FAT32_RANDOM,
    KERNEL_TEST_FAT32_TRUNCATE,
    KERNEL_TEST_FAT32_RENAME,
    KERNEL_TEST_FAT32_DELETE,
    KERNEL_TEST_FAT32_FULL,
    KERNEL_TEST_FAT32_CORRUPT,
    KERNEL_TEST_FAT32_MISSING,
    KERNEL_TEST_FAT32_PERSISTENCE,
    KERNEL_TEST_FAT32_CACHE,
    KERNEL_TEST_FAT32_IMMUTABLE,
    KERNEL_TEST_FAT32_HANDLES,
    KERNEL_TEST_NETWORK_NIC_DISCOVERY,
    KERNEL_TEST_NETWORK_NIC_INITIALIZATION,
    KERNEL_TEST_NETWORK_NIC_ABSENT,
    KERNEL_TEST_NETWORK_LINK_DOWN,
    KERNEL_TEST_NETWORK_DHCP,
    KERNEL_TEST_NETWORK_DHCP_TIMEOUT,
    KERNEL_TEST_NETWORK_STATIC,
    KERNEL_TEST_NETWORK_ARP,
    KERNEL_TEST_NETWORK_ICMP,
    KERNEL_TEST_NETWORK_ICMP_TIMEOUT,
    KERNEL_TEST_NETWORK_UDP,
    KERNEL_TEST_NETWORK_DNS_A,
    KERNEL_TEST_NETWORK_DNS_CNAME,
    KERNEL_TEST_NETWORK_DNS_MALFORMED,
    KERNEL_TEST_NETWORK_TCP,
    KERNEL_TEST_NETWORK_TCP_RETRANSMIT,
    KERNEL_TEST_NETWORK_TCP_RESET,
    KERNEL_TEST_NETWORK_HTTP_LENGTH,
    KERNEL_TEST_NETWORK_HTTP_CHUNKED,
    KERNEL_TEST_NETWORK_HTTP_REDIRECT,
    KERNEL_TEST_NETWORK_HTTP_MALFORMED,
    KERNEL_TEST_NETWORK_HTTP_NESTED,
    KERNEL_TEST_NETWORK_HTTP_REPLACE,
    KERNEL_TEST_NETWORK_HTTP_DISK_FULL,
    KERNEL_TEST_NETWORK_NIC_RESET,
    KERNEL_TEST_NETWORK_SYSTEM_IMMUTABLE,
    KERNEL_TEST_NETWORK_MISSING_LINUX_ECHO,
    KERNEL_TEST_NETWORK_MISSING_LINUX_UNAME,
    KERNEL_TEST_NETWORK_MISSING_LINUX_CAT,
    KERNEL_TEST_NETWORK_FILES,
    KERNEL_TEST_NETWORK_NOTES,
    KERNEL_TEST_NETWORK_MEDIA_EDITOR,
    KERNEL_TEST_NETWORK_PERSISTENCE,
    KERNEL_TEST_NETWORK_SOCKET_ISOLATION,
    KERNEL_TEST_NETWORK_TCP_LISTEN,
    KERNEL_TEST_NETWORK_TCP_REFUSED,
    KERNEL_TEST_MULTIPROCESS,
    KERNEL_TEST_MULTIPROCESS_SLOTS,
    KERNEL_TEST_DRIVER_MATRIX,
    KERNEL_TEST_DRIVER_MATRIX_BUILTIN,
    KERNEL_TEST_AUDIO,
    KERNEL_TEST_NVIDIA,
    KERNEL_TEST_NVIDIA_BUILTIN,
    KERNEL_TEST_NATIVE,
    KERNEL_TEST_NATIVE_LUA,
    KERNEL_TEST_NATIVE_SQLITE,
    KERNEL_TEST_NATIVE_CANVAS,
    KERNEL_TEST_NATIVE_NETWORK,
    KERNEL_TEST_NATIVE_RUST,
    KERNEL_TEST_NATIVE_CRASH,
    KERNEL_TEST_NATIVE_ELF_REFUSAL,
    KERNEL_TEST_NATIVE_DIGEST_REFUSAL,
    KERNEL_TEST_NATIVE_ABI_REFUSAL,
    KERNEL_TEST_NATIVE_RELAUNCH,
    KERNEL_TEST_NATIVE_AUDIO,
    KERNEL_TEST_NATIVE_SDL,
    KERNEL_TEST_NATIVE_DYNAMIC,
    KERNEL_TEST_NATIVE_HTTPS,
    KERNEL_TEST_NATIVE_PHIP,
    KERNEL_TEST_EXT4_RECOVERY,
    KERNEL_TEST_INVALID
};

/*
 * The bounded environment a scenario is allowed to inspect. A new discovered
 * window extends the registry, not kernel_test_run's signature, and every read
 * remains explicit rather than reaching into kernel.c's file scope.
 */
struct kernel_test_context {
    const struct acpi_mcfg *mcfg;
    const struct boot_framebuffer *framebuffer;
    const struct paging_device_windows *device_windows;
    bool mcfg_present;
};

enum kernel_test_scenario kernel_test_select(
    const struct boot_information *information
);
void kernel_test_run(
    enum kernel_test_scenario scenario,
    const struct kernel_test_context *context
);
_Noreturn void kernel_test_complete_normal(void);
struct boot_context;
_Noreturn void kernel_test_complete_boot_ledger(
    const struct boot_context *context
);
_Noreturn void kernel_test_complete_phipia_proof(void);
_Noreturn void kernel_test_complete_device_substrate(void);
bool kernel_test_device_substrate_exit_self_test(void);
_Noreturn void kernel_test_complete_xhci(void);
bool kernel_test_xhci_exit_self_test(void);
_Noreturn void kernel_test_complete_nvme(void);
bool kernel_test_nvme_exit_self_test(void);
_Noreturn void kernel_test_complete_filesystem(void);
bool kernel_test_filesystem_exit_self_test(void);
_Noreturn void kernel_test_complete_ext4_recovery(void);
_Noreturn void kernel_test_complete_process(void);
bool kernel_test_process_exit_self_test(void);
_Noreturn void kernel_test_complete_linux_abi(void);
bool kernel_test_linux_abi_exit_self_test(void);
_Noreturn void kernel_test_complete_linux_uname(void);
bool kernel_test_linux_uname_exit_self_test(void);
_Noreturn void kernel_test_complete_phipia_proof_userland(void);
_Noreturn void kernel_test_complete_phipia_proof_userland_absent(void);
_Noreturn void kernel_test_complete_phipia_proof_userland_interactive(void);
_Noreturn void kernel_test_complete_phipia_proof_userland_interactive_absent(
    void
);
_Noreturn void kernel_test_complete_fat32(void);
_Noreturn void kernel_test_complete_network(void);
_Noreturn void kernel_test_complete_multiprocess(void);
bool kernel_test_tcp_listen_exit_self_test(void);
bool kernel_test_multiprocess_exit_self_test(void);
_Noreturn void kernel_test_complete_driver_matrix(void);
bool kernel_test_driver_matrix_exit_self_test(void);
_Noreturn void kernel_test_complete_audio(void);
_Noreturn void kernel_test_complete_nvidia(void);
_Noreturn void kernel_test_complete_native(void);
_Noreturn void kernel_test_complete_native_lua(void);
_Noreturn void kernel_test_complete_native_sqlite(void);
_Noreturn void kernel_test_complete_native_canvas(void);
_Noreturn void kernel_test_complete_native_network(void);
_Noreturn void kernel_test_complete_native_rust(void);
_Noreturn void kernel_test_complete_native_crash(void);
_Noreturn void kernel_test_complete_native_admission_refusal(void);
_Noreturn void kernel_test_complete_native_relaunch(void);
_Noreturn void kernel_test_complete_native_audio(void);
_Noreturn void kernel_test_complete_native_sdl(void);
_Noreturn void kernel_test_complete_native_dynamic(void);
_Noreturn void kernel_test_complete_native_https(void);
_Noreturn void kernel_test_complete_native_phip(void);
bool kernel_test_audio_exit_self_test(void);
bool kernel_test_nvidia_exit_self_test(void);
bool kernel_test_handle_fatal_interrupt(const struct interrupt_frame *frame);
const char *kernel_test_scenario_name(enum kernel_test_scenario scenario);
_Noreturn void kernel_test_fail(const char *reason);

extern volatile uint8_t kernel_test_double_fault_armed;

#endif
