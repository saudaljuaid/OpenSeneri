/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/acpi.h>
#include <phipia/acpi_util.h>
#include <phipia/abi/base.h>
#include <phipia/apic.h>
#include <phipia/apic_timer.h>
#include <phipia/boot_ledger.h>
#include <phipia/boot_plan.h>
#include <phipia/clock.h>
#include <phipia/console.h>
#include <phipia/cpu.h>
#include <phipia/device_substrate.h>
#include <phipia/dma.h>
#include <phipia/ext4_fs.h>
#include <phipia/framebuffer.h>
#include <phipia/filesystem.h>
#include <phipia/fat32_fs.h>
#include <phipia/font.h>
#include <phipia/heap.h>
#include <phipia/interrupts.h>
#include <phipia/ioapic.h>
#include <phipia/memory.h>
#include <phipia/network.h>
#include <phipia/native_process.h>
#include <phipia/network_syscall.h>
#include <phipia/audio.h>
#include <phipia/nvidia.h>
#include <phipia/driver.h>
#include <phipia/multiprocess.h>
#include <phipia/nvme.h>
#include <phipia/paging.h>
#include <phipia/package_service.h>
#include <phipia/package_state.h>
#include <phipia/pci.h>
#include <phipia/pci_resource.h>
#include <phipia/pic.h>
#include <phipia/pit.h>
#include <phipia/pointer.h>
#include <phipia/process.h>
#include <phipia/random.h>
#include <phipia/keyboard.h>
#include <phipia/linux_abi.h>
#include <phipia/linux_cat.h>
#include <phipia/linux_uname.h>
#include <phipia/linux_userland.h>
#include <phipia/screen.h>
#include <phipia/shell.h>
#include <phipia/pm_timer.h>
#include <phipia/surface.h>
#include <phipia/store.h>
#include <phipia/taskbar.h>
#include <phipia/test.h>
#include <phipia/thread.h>
#include <phipia/timer.h>
#include <phipia/tsc.h>
#include <phipia/ui.h>
#include <phipia/ui_anim.h>
#include <phipia/ui_font.h>
#include <phipia/xhci.h>

#define QEMU_EXIT_PORT UINT16_C(0x00F4)
#define QEMU_FAILURE_VALUE UINT8_C(0x7F)
#define PAGE_FAULT_TEST_ADDRESS UINT64_C(0x0000000100000000)
#define PIT_TEST_FREQUENCY UINT32_C(100)
#define PIT_TEST_TICKS UINT64_C(8)
#define APIC_TIMER_TEST_FREQUENCY UINT32_C(100)
#define APIC_TIMER_TEST_TICKS UINT64_C(20)
#define TSC_MONOTONIC_READS 64U

/*
 * Eight level-triggered deliveries at 100 Hz. The failure this scenario exists
 * to catch is a pin that delivers once and stops, so one delivery would prove
 * nothing; eight of them cannot happen by accident. The bound is 25 times the
 * 80 ms they should take, so a line that dies is a named status rather than a
 * hang, and it stays well inside one wrap of the reference counter.
 */
#define IOAPIC_LEVEL_TEST_TICKS UINT64_C(8)
#define IOAPIC_LEVEL_TEST_BOUND_NS UINT64_C(2000000000)

/*
 * Intel SDM volume 3A section 4.7 defines the page-fault error code: bit 0 is
 * P, bit 1 is W/R and bit 2 is U/S. A supervisor write to a present read-only
 * page is therefore P=1 W=1 U=0. That is what distinguishes this scenario's
 * fault from the page-fault scenario's absent page, which is P=0 W=0 U=0.
 */
#define PAGING_TEST_FAULT_ERROR_CODE UINT64_C(0x03)
#define PAGING_TEST_PATTERN UINT8_C(0x5A)

/* Enough repetitions that one leaked table per cycle is unmistakable. */
#define PAGING_TEST_CYCLES 64U

/*
 * The first 2 MiB identity region beyond the linker's 48 MiB kernel ceiling.
 * Keeping this tied to the ceiling, rather than to the old small image size,
 * guarantees the refusal probe always lands on a huge leaf as UI assets grow.
 */
#define PAGING_TEST_HUGE_ADDRESS UINT64_C(0x0000000003000000)

_Static_assert(
    PAGING_TEST_HUGE_ADDRESS % PAGING_HUGE_PAGE_SIZE == 0U &&
        PAGING_TEST_HUGE_ADDRESS < PHIPIA_EARLY_PHYSICAL_LIMIT,
    "the paging huge-leaf probe must stay aligned inside the identity map"
);

/*
 * A supervisor write to an absent page is P=0 W=1 U=0. That is a third distinct
 * error code: the page-fault scenario reads an absent page at P=0 W=0 U=0, and
 * the paging scenario writes a present read-only page at P=1 W=1 U=0. No two of
 * the three scenarios can pass on each other's fault.
 */
#define HEAP_TEST_FAULT_ERROR_CODE UINT64_C(0x02)
#define HEAP_TEST_PATTERN UINT8_C(0xC3)

/*
 * Ten milliseconds of the ACPI timer, which counts at 3.579545 MHz, and the
 * 200 ms interval the local APIC timer defines by counting twenty of its own
 * ticks at 100 Hz. Two hundred milliseconds is 4.3% of the narrowest counter's
 * period, so the measurement stays far inside a single wrap.
 */
#define PM_TIMER_TEST_TICKS UINT32_C(35795)
#define PM_TIMER_TEST_FREQUENCY UINT32_C(100)
#define PM_TIMER_TEST_APIC_TICKS UINT64_C(20)

/*
 * Three deadlines, 20 ms apart. Far enough apart that the fixed cost of
 * reprogramming between them cannot reorder them, and short enough that the
 * whole scenario stays well inside its QEMU timeout.
 */
#define TIMERS_TEST_COUNT 3U
#define TIMERS_TEST_STEP_NS UINT64_C(20000000)

volatile uint8_t kernel_test_double_fault_armed;
static enum kernel_test_scenario active_scenario;

static size_t literal_length(const char *text)
{
    size_t length = 0;

    while (text[length] != '\0') {
        ++length;
    }

    return length;
}

static bool package_text_equals(
    const struct package_state_text *text,
    const char *literal
)
{
    size_t length = literal_length(literal);

    if (text == NULL || text->bytes == NULL || text->length != length) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        if (text->bytes[index] != (uint8_t)literal[index]) {
            return false;
        }
    }
    return true;
}

static bool token_equals(const char *token, size_t token_length, const char *literal)
{
    const size_t expected_length = literal_length(literal);

    if (token_length != expected_length) {
        return false;
    }

    for (size_t index = 0; index < token_length; ++index) {
        if (token[index] != literal[index]) {
            return false;
        }
    }

    return true;
}

static bool token_has_prefix(
    const char *token,
    size_t token_length,
    const char *prefix,
    size_t *value_offset
)
{
    const size_t prefix_length = literal_length(prefix);

    if (token_length < prefix_length) {
        return false;
    }

    for (size_t index = 0; index < prefix_length; ++index) {
        if (token[index] != prefix[index]) {
            return false;
        }
    }

    *value_offset = prefix_length;
    return true;
}

static enum kernel_test_scenario scenario_from_value(
    const char *value,
    size_t length
)
{
    if (token_equals(value, length, "normal")) {
        return KERNEL_TEST_NORMAL;
    }

    if (token_equals(value, length, "breakpoint")) {
        return KERNEL_TEST_BREAKPOINT;
    }

    if (token_equals(value, length, "invalid-opcode")) {
        return KERNEL_TEST_INVALID_OPCODE;
    }

    if (token_equals(value, length, "page-fault")) {
        return KERNEL_TEST_PAGE_FAULT;
    }

    if (token_equals(value, length, "ist")) {
        return KERNEL_TEST_IST;
    }

    if (token_equals(value, length, "pit")) {
        return KERNEL_TEST_PIT;
    }

    if (token_equals(value, length, "unexpected")) {
        return KERNEL_TEST_UNEXPECTED;
    }

    if (token_equals(value, length, "double-fault")) {
        return KERNEL_TEST_DOUBLE_FAULT;
    }

    if (token_equals(value, length, "apic")) {
        return KERNEL_TEST_APIC;
    }

    if (token_equals(value, length, "ioapic")) {
        return KERNEL_TEST_IOAPIC;
    }

    if (token_equals(value, length, "ioapic-level")) {
        return KERNEL_TEST_IOAPIC_LEVEL;
    }

    if (token_equals(value, length, "retired")) {
        return KERNEL_TEST_RETIRED;
    }

    if (token_equals(value, length, "apic-timer")) {
        return KERNEL_TEST_APIC_TIMER;
    }

    if (token_equals(value, length, "tsc")) {
        return KERNEL_TEST_TSC;
    }

    if (token_equals(value, length, "pm-timer")) {
        return KERNEL_TEST_PM_TIMER;
    }

    if (token_equals(value, length, "pit-retired")) {
        return KERNEL_TEST_PIT_RETIRED;
    }

    if (token_equals(value, length, "timers")) {
        return KERNEL_TEST_TIMERS;
    }

    if (token_equals(value, length, "paging")) {
        return KERNEL_TEST_PAGING;
    }

    if (token_equals(value, length, "heap")) {
        return KERNEL_TEST_HEAP;
    }

    if (token_equals(value, length, "pci")) {
        return KERNEL_TEST_PCI;
    }

    if (token_equals(value, length, "pci-ecam")) {
        return KERNEL_TEST_PCI_ECAM;
    }

    if (token_equals(value, length, "threads")) {
        return KERNEL_TEST_THREADS;
    }

    if (token_equals(value, length, "thread-guard")) {
        return KERNEL_TEST_THREAD_GUARD;
    }

    if (token_equals(value, length, "shell")) {
        return KERNEL_TEST_SHELL;
    }

    if (token_equals(value, length, "keyboard")) {
        return KERNEL_TEST_KEYBOARD;
    }

    if (token_equals(value, length, "screen")) {
        return KERNEL_TEST_SCREEN;
    }

    if (token_equals(value, length, "framebuffer")) {
        return KERNEL_TEST_FRAMEBUFFER;
    }

    if (token_equals(value, length, "surface")) {
        return KERNEL_TEST_SURFACE;
    }

    if (token_equals(value, length, "write-combining")) {
        return KERNEL_TEST_WRITE_COMBINING;
    }

    if (token_equals(value, length, "device-windows")) {
        return KERNEL_TEST_DEVICE_WINDOWS;
    }

    if (token_equals(value, length, "boot-ledger")) {
        return KERNEL_TEST_BOOT_LEDGER;
    }

    if (token_equals(value, length, "phipia-proof")) {
        return KERNEL_TEST_PHIPIA_PROOF;
    }

    if (token_equals(value, length, "device-substrate")) {
        return KERNEL_TEST_DEVICE_SUBSTRATE;
    }

    if (token_equals(value, length, "xhci")) {
        return KERNEL_TEST_XHCI;
    }

    if (token_equals(value, length, "nvme")) {
        return KERNEL_TEST_NVME;
    }

    if (token_equals(value, length, "filesystem")) {
        return KERNEL_TEST_FILESYSTEM;
    }

    if (token_equals(value, length, "process")) {
        return KERNEL_TEST_PROCESS;
    }

    if (token_equals(value, length, "linux-abi")) {
        return KERNEL_TEST_LINUX_ABI;
    }

    if (token_equals(value, length, "linux-abi-uname")) {
        return KERNEL_TEST_LINUX_ABI_UNAME;
    }

    if (token_equals(value, length, "phipia-proof-userland")) {
        return KERNEL_TEST_PHIPIA_PROOF_USERLAND;
    }

    if (token_equals(value, length, "phipia-proof-userland-absent")) {
        return KERNEL_TEST_PHIPIA_PROOF_USERLAND_ABSENT;
    }

    if (token_equals(value, length, "phipia-proof-userland-interactive")) {
        return KERNEL_TEST_PHIPIA_PROOF_USERLAND_INTERACTIVE;
    }

    if (token_equals(
            value, length, "phipia-proof-userland-interactive-absent")) {
        return KERNEL_TEST_PHIPIA_PROOF_USERLAND_INTERACTIVE_ABSENT;
    }

    if (token_equals(value, length, "fat32-system")) {
        return KERNEL_TEST_FAT32_SYSTEM;
    }
    if (token_equals(value, length, "fat32-data")) {
        return KERNEL_TEST_FAT32_DATA;
    }
    if (token_equals(value, length, "fat32-nested")) {
        return KERNEL_TEST_FAT32_NESTED;
    }
    if (token_equals(value, length, "fat32-growth")) {
        return KERNEL_TEST_FAT32_GROWTH;
    }
    if (token_equals(value, length, "fat32-random")) {
        return KERNEL_TEST_FAT32_RANDOM;
    }
    if (token_equals(value, length, "fat32-truncate")) {
        return KERNEL_TEST_FAT32_TRUNCATE;
    }
    if (token_equals(value, length, "fat32-rename")) {
        return KERNEL_TEST_FAT32_RENAME;
    }
    if (token_equals(value, length, "fat32-delete")) {
        return KERNEL_TEST_FAT32_DELETE;
    }
    if (token_equals(value, length, "fat32-full")) {
        return KERNEL_TEST_FAT32_FULL;
    }
    if (token_equals(value, length, "fat32-corrupt")) {
        return KERNEL_TEST_FAT32_CORRUPT;
    }
    if (token_equals(value, length, "fat32-missing")) {
        return KERNEL_TEST_FAT32_MISSING;
    }
    if (token_equals(value, length, "fat32-persistence")) {
        return KERNEL_TEST_FAT32_PERSISTENCE;
    }
    if (token_equals(value, length, "fat32-cache")) {
        return KERNEL_TEST_FAT32_CACHE;
    }
    if (token_equals(value, length, "fat32-immutable")) {
        return KERNEL_TEST_FAT32_IMMUTABLE;
    }
    if (token_equals(value, length, "fat32-handles")) {
        return KERNEL_TEST_FAT32_HANDLES;
    }
    if (token_equals(value, length, "network-nic-discovery")) {
        return KERNEL_TEST_NETWORK_NIC_DISCOVERY;
    }
    if (token_equals(value, length, "network-nic-initialization")) {
        return KERNEL_TEST_NETWORK_NIC_INITIALIZATION;
    }
    if (token_equals(value, length, "network-nic-absent")) {
        return KERNEL_TEST_NETWORK_NIC_ABSENT;
    }
    if (token_equals(value, length, "network-link-down")) {
        return KERNEL_TEST_NETWORK_LINK_DOWN;
    }
    if (token_equals(value, length, "network-dhcp")) {
        return KERNEL_TEST_NETWORK_DHCP;
    }
    if (token_equals(value, length, "network-dhcp-timeout")) {
        return KERNEL_TEST_NETWORK_DHCP_TIMEOUT;
    }
    if (token_equals(value, length, "network-static")) {
        return KERNEL_TEST_NETWORK_STATIC;
    }
    if (token_equals(value, length, "network-arp")) {
        return KERNEL_TEST_NETWORK_ARP;
    }
    if (token_equals(value, length, "network-icmp")) {
        return KERNEL_TEST_NETWORK_ICMP;
    }
    if (token_equals(value, length, "network-icmp-timeout")) {
        return KERNEL_TEST_NETWORK_ICMP_TIMEOUT;
    }
    if (token_equals(value, length, "network-udp")) {
        return KERNEL_TEST_NETWORK_UDP;
    }
    if (token_equals(value, length, "network-dns-a")) {
        return KERNEL_TEST_NETWORK_DNS_A;
    }
    if (token_equals(value, length, "network-dns-cname")) {
        return KERNEL_TEST_NETWORK_DNS_CNAME;
    }
    if (token_equals(value, length, "network-dns-malformed")) {
        return KERNEL_TEST_NETWORK_DNS_MALFORMED;
    }
    if (token_equals(value, length, "network-tcp")) {
        return KERNEL_TEST_NETWORK_TCP;
    }
    if (token_equals(value, length, "network-tcp-retransmit")) {
        return KERNEL_TEST_NETWORK_TCP_RETRANSMIT;
    }
    if (token_equals(value, length, "network-tcp-reset")) {
        return KERNEL_TEST_NETWORK_TCP_RESET;
    }
    if (token_equals(value, length, "network-http-length")) {
        return KERNEL_TEST_NETWORK_HTTP_LENGTH;
    }
    if (token_equals(value, length, "network-http-chunked")) {
        return KERNEL_TEST_NETWORK_HTTP_CHUNKED;
    }
    if (token_equals(value, length, "network-http-redirect")) {
        return KERNEL_TEST_NETWORK_HTTP_REDIRECT;
    }
    if (token_equals(value, length, "network-http-malformed")) {
        return KERNEL_TEST_NETWORK_HTTP_MALFORMED;
    }
    if (token_equals(value, length, "network-http-nested")) {
        return KERNEL_TEST_NETWORK_HTTP_NESTED;
    }
    if (token_equals(value, length, "network-http-replace")) {
        return KERNEL_TEST_NETWORK_HTTP_REPLACE;
    }
    if (token_equals(value, length, "network-http-disk-full")) {
        return KERNEL_TEST_NETWORK_HTTP_DISK_FULL;
    }
    if (token_equals(value, length, "network-nic-reset")) {
        return KERNEL_TEST_NETWORK_NIC_RESET;
    }
    if (token_equals(value, length, "network-system-immutable")) {
        return KERNEL_TEST_NETWORK_SYSTEM_IMMUTABLE;
    }
    if (token_equals(value, length, "network-missing-linux-echo")) {
        return KERNEL_TEST_NETWORK_MISSING_LINUX_ECHO;
    }
    if (token_equals(value, length, "network-missing-linux-uname")) {
        return KERNEL_TEST_NETWORK_MISSING_LINUX_UNAME;
    }
    if (token_equals(value, length, "network-missing-linux-cat")) {
        return KERNEL_TEST_NETWORK_MISSING_LINUX_CAT;
    }
    if (token_equals(value, length, "network-files")) {
        return KERNEL_TEST_NETWORK_FILES;
    }
    if (token_equals(value, length, "network-notes")) {
        return KERNEL_TEST_NETWORK_NOTES;
    }
    if (token_equals(value, length, "network-media-editor")) {
        return KERNEL_TEST_NETWORK_MEDIA_EDITOR;
    }
    if (token_equals(value, length, "network-persistence")) {
        return KERNEL_TEST_NETWORK_PERSISTENCE;
    }
    if (token_equals(value, length, "network-socket-isolation")) {
        return KERNEL_TEST_NETWORK_SOCKET_ISOLATION;
    }
    if (token_equals(value, length, "network-tcp-listen")) {
        return KERNEL_TEST_NETWORK_TCP_LISTEN;
    }
    if (token_equals(value, length, "network-tcp-refused")) {
        return KERNEL_TEST_NETWORK_TCP_REFUSED;
    }
    if (token_equals(value, length, "multiprocess")) {
        return KERNEL_TEST_MULTIPROCESS;
    }
    if (token_equals(value, length, "multiprocess-slots")) {
        return KERNEL_TEST_MULTIPROCESS_SLOTS;
    }
    if (token_equals(value, length, "driver-matrix")) {
        return KERNEL_TEST_DRIVER_MATRIX;
    }
    if (token_equals(value, length, "driver-matrix-builtin")) {
        return KERNEL_TEST_DRIVER_MATRIX_BUILTIN;
    }
    if (token_equals(value, length, "audio")) {
        return KERNEL_TEST_AUDIO;
    }
    if (token_equals(value, length, "nvidia")) {
        return KERNEL_TEST_NVIDIA;
    }
    if (token_equals(value, length, "nvidia-builtin")) {
        return KERNEL_TEST_NVIDIA_BUILTIN;
    }
    if (token_equals(value, length, "native")) {
        return KERNEL_TEST_NATIVE;
    }
    if (token_equals(value, length, "native-lua")) {
        return KERNEL_TEST_NATIVE_LUA;
    }
    if (token_equals(value, length, "native-sqlite")) {
        return KERNEL_TEST_NATIVE_SQLITE;
    }
    if (token_equals(value, length, "native-canvas")) {
        return KERNEL_TEST_NATIVE_CANVAS;
    }
    if (token_equals(value, length, "network-native")) {
        return KERNEL_TEST_NATIVE_NETWORK;
    }
    if (token_equals(value, length, "native-rust")) {
        return KERNEL_TEST_NATIVE_RUST;
    }
    if (token_equals(value, length, "native-crash")) {
        return KERNEL_TEST_NATIVE_CRASH;
    }
    if (token_equals(value, length, "native-elf-refusal")) {
        return KERNEL_TEST_NATIVE_ELF_REFUSAL;
    }
    if (token_equals(value, length, "native-digest-refusal")) {
        return KERNEL_TEST_NATIVE_DIGEST_REFUSAL;
    }
    if (token_equals(value, length, "native-abi-refusal")) {
        return KERNEL_TEST_NATIVE_ABI_REFUSAL;
    }
    if (token_equals(value, length, "native-relaunch")) {
        return KERNEL_TEST_NATIVE_RELAUNCH;
    }
    if (token_equals(value, length, "native-audio")) {
        return KERNEL_TEST_NATIVE_AUDIO;
    }
    if (token_equals(value, length, "native-sdl")) {
        return KERNEL_TEST_NATIVE_SDL;
    }
    if (token_equals(value, length, "native-dynamic")) {
        return KERNEL_TEST_NATIVE_DYNAMIC;
    }
    if (token_equals(value, length, "native-https")) {
        return KERNEL_TEST_NATIVE_HTTPS;
    }
    if (token_equals(value, length, "native-phip")) {
        return KERNEL_TEST_NATIVE_PHIP;
    }
    if (token_equals(value, length, "ext4-recovery")) {
        return KERNEL_TEST_EXT4_RECOVERY;
    }

    return KERNEL_TEST_INVALID;
}

/*
 * The value each scenario hands to QEMU's debug exit device, which the Makefile
 * turns into the process status it requires. They are deliberately dense and
 * deliberately stable: a scenario that took another's value would pass as that
 * one.
 *
 * 0x22 belongs to ioapic-level. The scenarios added after it start at 0x23 so
 * every exit value remains stable across this integration.
 */
static uint8_t scenario_exit_value(enum kernel_test_scenario scenario)
{
    switch (scenario) {
    case KERNEL_TEST_NORMAL:
        return UINT8_C(0x10);
    case KERNEL_TEST_BREAKPOINT:
        return UINT8_C(0x11);
    case KERNEL_TEST_INVALID_OPCODE:
        return UINT8_C(0x12);
    case KERNEL_TEST_PAGE_FAULT:
        return UINT8_C(0x13);
    case KERNEL_TEST_IST:
        return UINT8_C(0x14);
    case KERNEL_TEST_PIT:
        return UINT8_C(0x15);
    case KERNEL_TEST_UNEXPECTED:
        return UINT8_C(0x16);
    case KERNEL_TEST_DOUBLE_FAULT:
        return UINT8_C(0x17);
    case KERNEL_TEST_APIC:
        return UINT8_C(0x18);
    case KERNEL_TEST_IOAPIC:
        return UINT8_C(0x19);
    case KERNEL_TEST_RETIRED:
        return UINT8_C(0x1A);
    case KERNEL_TEST_APIC_TIMER:
        return UINT8_C(0x1B);
    case KERNEL_TEST_TSC:
        return UINT8_C(0x1C);
    case KERNEL_TEST_PM_TIMER:
        return UINT8_C(0x1D);
    case KERNEL_TEST_PIT_RETIRED:
        return UINT8_C(0x1E);
    case KERNEL_TEST_TIMERS:
        return UINT8_C(0x1F);
    case KERNEL_TEST_PAGING:
        return UINT8_C(0x20);
    case KERNEL_TEST_HEAP:
        return UINT8_C(0x21);
    case KERNEL_TEST_IOAPIC_LEVEL:
        return UINT8_C(0x22);
    case KERNEL_TEST_PCI:
        return UINT8_C(0x23);
    case KERNEL_TEST_PCI_ECAM:
        return UINT8_C(0x24);
    case KERNEL_TEST_THREADS:
        return UINT8_C(0x25);
    case KERNEL_TEST_THREAD_GUARD:
        return UINT8_C(0x26);
    case KERNEL_TEST_FRAMEBUFFER:
        return UINT8_C(0x27);
    case KERNEL_TEST_SCREEN:
        return UINT8_C(0x28);
    case KERNEL_TEST_KEYBOARD:
        return UINT8_C(0x29);
    case KERNEL_TEST_SHELL:
        return UINT8_C(0x2A);
    case KERNEL_TEST_SURFACE:
        return UINT8_C(0x2B);
    case KERNEL_TEST_WRITE_COMBINING:
        return UINT8_C(0x2C);
    case KERNEL_TEST_DEVICE_WINDOWS:
        return UINT8_C(0x2D);
    case KERNEL_TEST_BOOT_LEDGER:
        return UINT8_C(0x2E);
    case KERNEL_TEST_PHIPIA_PROOF:
        return UINT8_C(0x2F);
    case KERNEL_TEST_DEVICE_SUBSTRATE:
        return UINT8_C(0x30);
    case KERNEL_TEST_XHCI:
        return UINT8_C(0x31);
    case KERNEL_TEST_NVME:
        return UINT8_C(0x32);
    case KERNEL_TEST_FILESYSTEM:
        return UINT8_C(0x33);
    case KERNEL_TEST_PROCESS:
        return UINT8_C(0x34);
    case KERNEL_TEST_LINUX_ABI:
        return UINT8_C(0x36);
    case KERNEL_TEST_LINUX_ABI_UNAME:
        return UINT8_C(0x37);
    case KERNEL_TEST_PHIPIA_PROOF_USERLAND:
        return UINT8_C(0x38);
    case KERNEL_TEST_PHIPIA_PROOF_USERLAND_ABSENT:
        return UINT8_C(0x39);
    case KERNEL_TEST_PHIPIA_PROOF_USERLAND_INTERACTIVE:
        return UINT8_C(0x3A);
    case KERNEL_TEST_PHIPIA_PROOF_USERLAND_INTERACTIVE_ABSENT:
        return UINT8_C(0x3B);
    case KERNEL_TEST_FAT32_SYSTEM:
        return UINT8_C(0x3C);
    case KERNEL_TEST_FAT32_DATA:
        return UINT8_C(0x3D);
    case KERNEL_TEST_FAT32_NESTED:
        return UINT8_C(0x3E);
    case KERNEL_TEST_FAT32_GROWTH:
        return UINT8_C(0x3F);
    case KERNEL_TEST_FAT32_RANDOM:
        return UINT8_C(0x40);
    case KERNEL_TEST_FAT32_TRUNCATE:
        return UINT8_C(0x41);
    case KERNEL_TEST_FAT32_RENAME:
        return UINT8_C(0x42);
    case KERNEL_TEST_FAT32_DELETE:
        return UINT8_C(0x43);
    case KERNEL_TEST_FAT32_FULL:
        return UINT8_C(0x44);
    case KERNEL_TEST_FAT32_CORRUPT:
        return UINT8_C(0x45);
    case KERNEL_TEST_FAT32_MISSING:
        return UINT8_C(0x46);
    case KERNEL_TEST_FAT32_PERSISTENCE:
        return UINT8_C(0x47);
    case KERNEL_TEST_FAT32_CACHE:
        return UINT8_C(0x48);
    case KERNEL_TEST_FAT32_IMMUTABLE:
        return UINT8_C(0x49);
    case KERNEL_TEST_FAT32_HANDLES:
        return UINT8_C(0x4A);
    case KERNEL_TEST_NETWORK_NIC_DISCOVERY: return UINT8_C(0x4B);
    case KERNEL_TEST_NETWORK_NIC_INITIALIZATION: return UINT8_C(0x4C);
    case KERNEL_TEST_NETWORK_NIC_ABSENT: return UINT8_C(0x4D);
    case KERNEL_TEST_NETWORK_LINK_DOWN: return UINT8_C(0x4E);
    case KERNEL_TEST_NETWORK_DHCP: return UINT8_C(0x4F);
    case KERNEL_TEST_NETWORK_DHCP_TIMEOUT: return UINT8_C(0x50);
    case KERNEL_TEST_NETWORK_STATIC: return UINT8_C(0x51);
    case KERNEL_TEST_NETWORK_ARP: return UINT8_C(0x52);
    case KERNEL_TEST_NETWORK_ICMP: return UINT8_C(0x53);
    case KERNEL_TEST_NETWORK_ICMP_TIMEOUT: return UINT8_C(0x54);
    case KERNEL_TEST_NETWORK_UDP: return UINT8_C(0x55);
    case KERNEL_TEST_NETWORK_DNS_A: return UINT8_C(0x56);
    case KERNEL_TEST_NETWORK_DNS_CNAME: return UINT8_C(0x57);
    case KERNEL_TEST_NETWORK_DNS_MALFORMED: return UINT8_C(0x58);
    case KERNEL_TEST_NETWORK_TCP: return UINT8_C(0x59);
    case KERNEL_TEST_NETWORK_TCP_RETRANSMIT: return UINT8_C(0x5A);
    case KERNEL_TEST_NETWORK_TCP_RESET: return UINT8_C(0x5B);
    case KERNEL_TEST_NETWORK_HTTP_LENGTH: return UINT8_C(0x5C);
    case KERNEL_TEST_NETWORK_HTTP_CHUNKED: return UINT8_C(0x5D);
    case KERNEL_TEST_NETWORK_HTTP_REDIRECT: return UINT8_C(0x5E);
    case KERNEL_TEST_NETWORK_HTTP_MALFORMED: return UINT8_C(0x5F);
    case KERNEL_TEST_NETWORK_HTTP_NESTED: return UINT8_C(0x60);
    case KERNEL_TEST_NETWORK_HTTP_REPLACE: return UINT8_C(0x61);
    case KERNEL_TEST_NETWORK_HTTP_DISK_FULL: return UINT8_C(0x62);
    case KERNEL_TEST_NETWORK_NIC_RESET: return UINT8_C(0x63);
    case KERNEL_TEST_NETWORK_SYSTEM_IMMUTABLE: return UINT8_C(0x64);
    case KERNEL_TEST_NETWORK_MISSING_LINUX_ECHO: return UINT8_C(0x65);
    case KERNEL_TEST_NETWORK_MISSING_LINUX_UNAME: return UINT8_C(0x66);
    case KERNEL_TEST_NETWORK_MISSING_LINUX_CAT: return UINT8_C(0x67);
    case KERNEL_TEST_NETWORK_FILES: return UINT8_C(0x68);
    case KERNEL_TEST_NETWORK_NOTES: return UINT8_C(0x69);
    case KERNEL_TEST_NETWORK_MEDIA_EDITOR: return UINT8_C(0x6A);
    case KERNEL_TEST_NETWORK_PERSISTENCE: return UINT8_C(0x6B);
    case KERNEL_TEST_NETWORK_SOCKET_ISOLATION: return UINT8_C(0x6C);
    case KERNEL_TEST_NETWORK_TCP_LISTEN: return UINT8_C(0x6D);
    case KERNEL_TEST_NETWORK_TCP_REFUSED: return UINT8_C(0x6E);
    case KERNEL_TEST_MULTIPROCESS: return UINT8_C(0x6F);
    case KERNEL_TEST_MULTIPROCESS_SLOTS: return UINT8_C(0x70);
    case KERNEL_TEST_DRIVER_MATRIX: return UINT8_C(0x71);
    case KERNEL_TEST_DRIVER_MATRIX_BUILTIN: return UINT8_C(0x72);
    case KERNEL_TEST_AUDIO: return UINT8_C(0x73);
    case KERNEL_TEST_NVIDIA: return UINT8_C(0x74);
    case KERNEL_TEST_NVIDIA_BUILTIN: return UINT8_C(0x75);
    case KERNEL_TEST_NATIVE: return UINT8_C(0x76);
    case KERNEL_TEST_NATIVE_LUA: return UINT8_C(0x77);
    case KERNEL_TEST_NATIVE_SQLITE: return UINT8_C(0x78);
    case KERNEL_TEST_NATIVE_CANVAS: return UINT8_C(0x79);
    case KERNEL_TEST_NATIVE_NETWORK: return UINT8_C(0x7A);
    case KERNEL_TEST_NATIVE_RUST: return UINT8_C(0x7B);
    case KERNEL_TEST_NATIVE_CRASH: return UINT8_C(0x7C);
    case KERNEL_TEST_NATIVE_ELF_REFUSAL: return UINT8_C(0x7D);
    case KERNEL_TEST_NATIVE_DIGEST_REFUSAL: return UINT8_C(0x7E);
    /* 0x7F is the invariant QEMU failure value. */
    case KERNEL_TEST_NATIVE_ABI_REFUSAL: return UINT8_C(0x80);
    case KERNEL_TEST_NATIVE_RELAUNCH: return UINT8_C(0x81);
    case KERNEL_TEST_NATIVE_AUDIO: return UINT8_C(0x82);
    case KERNEL_TEST_NATIVE_SDL: return UINT8_C(0x83);
    case KERNEL_TEST_NATIVE_DYNAMIC: return UINT8_C(0x84);
    case KERNEL_TEST_NATIVE_HTTPS: return UINT8_C(0x85);
    case KERNEL_TEST_EXT4_RECOVERY: return UINT8_C(0x86);
    case KERNEL_TEST_NATIVE_PHIP: return UINT8_C(0x87);
    default:
        return QEMU_FAILURE_VALUE;
    }
}

static bool device_substrate_exit_contract(uint8_t value)
{
    return value == UINT8_C(0x30);
}

bool kernel_test_device_substrate_exit_self_test(void)
{
    return device_substrate_exit_contract(
            scenario_exit_value(KERNEL_TEST_DEVICE_SUBSTRATE)) &&
        !device_substrate_exit_contract(UINT8_C(0x32));
}

static bool xhci_exit_contract(uint8_t value)
{
    return value == UINT8_C(0x31);
}

bool kernel_test_xhci_exit_self_test(void)
{
    return xhci_exit_contract(scenario_exit_value(KERNEL_TEST_XHCI)) &&
        !xhci_exit_contract(UINT8_C(0x32));
}

static bool nvme_exit_contract(uint8_t value)
{
    return value == UINT8_C(0x32);
}

bool kernel_test_nvme_exit_self_test(void)
{
    return nvme_exit_contract(scenario_exit_value(KERNEL_TEST_NVME)) &&
        !nvme_exit_contract(UINT8_C(0x33));
}

static bool filesystem_exit_contract(uint8_t value)
{
    return value == UINT8_C(0x33);
}

bool kernel_test_filesystem_exit_self_test(void)
{
    return filesystem_exit_contract(
            scenario_exit_value(KERNEL_TEST_FILESYSTEM)) &&
        !filesystem_exit_contract(UINT8_C(0x34));
}

static bool process_exit_contract(uint8_t value)
{
    return value == UINT8_C(0x34);
}

bool kernel_test_process_exit_self_test(void)
{
    return process_exit_contract(scenario_exit_value(KERNEL_TEST_PROCESS)) &&
        !process_exit_contract(UINT8_C(0x33));
}

static bool linux_abi_exit_contract(uint8_t value)
{
    return value == UINT8_C(0x36);
}

bool kernel_test_linux_abi_exit_self_test(void)
{
    return linux_abi_exit_contract(
            scenario_exit_value(KERNEL_TEST_LINUX_ABI)) &&
        !linux_abi_exit_contract(UINT8_C(0x35)) &&
        !linux_abi_exit_contract(UINT8_C(0x34));
}

static bool linux_uname_exit_contract(uint8_t value)
{
    return value == UINT8_C(0x37);
}

bool kernel_test_linux_uname_exit_self_test(void)
{
    return linux_uname_exit_contract(
            scenario_exit_value(KERNEL_TEST_LINUX_ABI_UNAME)) &&
        !linux_uname_exit_contract(UINT8_C(0x36)) &&
        !linux_uname_exit_contract(UINT8_C(0x38));
}

/*
 * The passive-open pair sits inside the networking block, so inserting a
 * scenario shifts every later exit value. This contract is what makes that a
 * refusal rather than a silent renumbering.
 */
static bool tcp_listen_exit_contract(uint8_t value)
{
    return value == UINT8_C(0x6D);
}

bool kernel_test_tcp_listen_exit_self_test(void)
{
    return tcp_listen_exit_contract(
            scenario_exit_value(KERNEL_TEST_NETWORK_TCP_LISTEN)) &&
        !tcp_listen_exit_contract(
            scenario_exit_value(KERNEL_TEST_NETWORK_TCP_REFUSED)) &&
        !tcp_listen_exit_contract(
            scenario_exit_value(KERNEL_TEST_NETWORK_SOCKET_ISOLATION)) &&
        scenario_exit_value(KERNEL_TEST_NETWORK_TCP_REFUSED) ==
            UINT8_C(0x6E);
}

static bool multiprocess_exit_contract(uint8_t value)
{
    return value == UINT8_C(0x6F);
}

bool kernel_test_multiprocess_exit_self_test(void)
{
    return multiprocess_exit_contract(
            scenario_exit_value(KERNEL_TEST_MULTIPROCESS)) &&
        !multiprocess_exit_contract(
            scenario_exit_value(KERNEL_TEST_MULTIPROCESS_SLOTS)) &&
        !multiprocess_exit_contract(UINT8_C(0x34));
}

static bool driver_matrix_exit_contract(uint8_t value)
{
    return value == UINT8_C(0x71);
}

bool kernel_test_driver_matrix_exit_self_test(void)
{
    return driver_matrix_exit_contract(
            scenario_exit_value(KERNEL_TEST_DRIVER_MATRIX)) &&
        !driver_matrix_exit_contract(
            scenario_exit_value(KERNEL_TEST_DRIVER_MATRIX_BUILTIN)) &&
        !driver_matrix_exit_contract(
            scenario_exit_value(KERNEL_TEST_MULTIPROCESS));
}

static bool nvidia_exit_contract(uint8_t value)
{
    return value == UINT8_C(0x74);
}

bool kernel_test_nvidia_exit_self_test(void)
{
    return nvidia_exit_contract(scenario_exit_value(KERNEL_TEST_NVIDIA)) &&
        !nvidia_exit_contract(
            scenario_exit_value(KERNEL_TEST_NVIDIA_BUILTIN)) &&
        !nvidia_exit_contract(scenario_exit_value(KERNEL_TEST_AUDIO));
}

static bool audio_exit_contract(uint8_t value)
{
    return value == UINT8_C(0x73);
}

bool kernel_test_audio_exit_self_test(void)
{
    return audio_exit_contract(scenario_exit_value(KERNEL_TEST_AUDIO)) &&
        !audio_exit_contract(scenario_exit_value(KERNEL_TEST_DRIVER_MATRIX)) &&
        !audio_exit_contract(
            scenario_exit_value(KERNEL_TEST_DRIVER_MATRIX_BUILTIN));
}

static void test_marker(const char *kind, enum kernel_test_scenario scenario)
{
    console_write("ST ");
    console_write(kind);
    console_putc(' ');
    console_write(kernel_test_scenario_name(scenario));
    console_putc('\n');
}

static _Noreturn void kernel_test_pass(void)
{
    const uint8_t exit_value = scenario_exit_value(active_scenario);

    test_marker("PASS", active_scenario);
    cpu_out32(QEMU_EXIT_PORT, exit_value);
    console_halt();
}

enum kernel_test_scenario kernel_test_select(
    const struct boot_information *context
)
{
    static const char prefix[] = "phipia.test=";
    enum kernel_test_scenario selected = KERNEL_TEST_NONE;
    size_t offset = 0;

    kernel_test_double_fault_armed = 0U;
    active_scenario = KERNEL_TEST_NONE;

    if (context == NULL || context->command_line == NULL) {
        return KERNEL_TEST_NONE;
    }

    while (offset < context->command_line_length) {
        size_t token_start;
        size_t token_length;
        size_t value_offset;

        while (offset < context->command_line_length &&
               context->command_line[offset] == ' ') {
            ++offset;
        }

        token_start = offset;

        while (offset < context->command_line_length &&
               context->command_line[offset] != ' ') {
            ++offset;
        }

        token_length = offset - token_start;

        if (token_length == 0U || !token_has_prefix(
                context->command_line + token_start,
                token_length,
                prefix,
                &value_offset
            )) {
            continue;
        }

        if (selected != KERNEL_TEST_NONE) {
            return KERNEL_TEST_INVALID;
        }

        selected = scenario_from_value(
            context->command_line + token_start + value_offset,
            token_length - value_offset
        );

        if (selected == KERNEL_TEST_INVALID) {
            return selected;
        }
    }

    active_scenario = selected;
    return selected;
}

/*
 * Enabling the local APIC takes the 8259 pair off the processor's direct
 * interrupt path. This scenario proves the replacement path: the APIC is online
 * and agrees with firmware, and the PIT still delivers through LINT0.
 */
static void apic_scenario(void)
{
    const struct apic_state apic = apic_get_state();
    enum pit_status pit_status;

    if (!apic_is_online() || !apic.online) {
        kernel_test_fail("local APIC is not online");
    }

    if (apic.base_address == 0U || apic.max_lvt_entry < 4U) {
        kernel_test_fail("local APIC reported an unusable register window");
    }

    if (!apic.legacy_interrupts_routed) {
        kernel_test_fail("local APIC did not route legacy interrupts");
    }

    pit_status = pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_LEGACY_PIC);

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    pit_status = pit_wait_for_ticks(PIT_TEST_TICKS);

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    pit_status = pit_stop();

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    if (pit_ticks() < PIT_TEST_TICKS) {
        kernel_test_fail("PIT stopped delivering once the local APIC was on");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

/*
 * Prove the timer arrives through the I/O APIC rather than the 8259 pair: the
 * legacy line stays masked, the redirection entry carries the ACPI override,
 * and the interrupt is acknowledged at the local APIC.
 */
static void ioapic_scenario(void)
{
    const struct ioapic_state ioapic = ioapic_get_state();
    enum pit_status pit_status;

    if (!ioapic_is_initialized() || ioapic.count == 0U) {
        kernel_test_fail("I/O APIC is not initialized");
    }

    if (ioapic.units[0].entry_count < 16U) {
        kernel_test_fail("I/O APIC cannot redirect the ISA interrupts");
    }

    if (pic_mask_snapshot() != UINT16_C(0xFFFF)) {
        kernel_test_fail("a legacy PIC line was left unmasked");
    }

    pit_status = pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_IO_APIC);

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    if (pit_active_route() != PIT_ROUTE_IO_APIC) {
        kernel_test_fail("timer did not take the I/O APIC route");
    }

    if (pic_mask_snapshot() != UINT16_C(0xFFFF)) {
        kernel_test_fail("I/O APIC routing unmasked a legacy PIC line");
    }

    pit_status = pit_wait_for_ticks(PIT_TEST_TICKS);

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    pit_status = pit_stop();

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    if (pit_ticks() < PIT_TEST_TICKS) {
        kernel_test_fail("I/O APIC delivered too few timer interrupts");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

/*
 * Prove a level-triggered redirection entry delivers more than once.
 *
 * Every other route in this kernel is edge triggered, and an edge needs no
 * acknowledgement at the I/O APIC: the pin is sampled on a transition and
 * nothing is latched. A level-triggered entry latches remote IRR when it
 * delivers and cannot deliver again until an end of interrupt directed at the
 * I/O APIC clears it, so the failure worth hunting is a line that fires exactly
 * once and then goes quiet. One delivery cannot tell that apart from success,
 * which is why this counts eight.
 *
 * The opposite failure is just as silent. Acknowledging a pin whose source is
 * still asserting re-delivers immediately, so a route that never quiets its
 * device counts its eight interrupts in microseconds and looks perfect. The
 * scenario therefore measures how long the eight took as well as that they
 * arrived, and holds it to the interval eight ticks of a 100 Hz timer take.
 */
static void ioapic_level_scenario(void)
{
    struct ioapic_redirection entry;
    struct ioapic_state before;
    struct ioapic_state after;
    uint64_t elapsed_ns = 0U;
    const uint64_t expected_ns = IOAPIC_LEVEL_TEST_TICKS * UINT64_C(1000000000) /
        PIT_TEST_FREQUENCY;

    if (!ioapic_is_initialized() || ioapic_get_state().count == 0U) {
        kernel_test_fail("I/O APIC is not initialized");
    }

    if (pic_mask_snapshot() != UINT16_C(0xFFFF)) {
        kernel_test_fail("a legacy PIC line was left unmasked");
    }

    /* Nothing is routed yet, so every acknowledgement is refused by name. */
    if (ioapic_send_eoi(INTERRUPT_IOAPIC_BASE) !=
            IOAPIC_STATUS_VECTOR_NOT_ROUTED ||
        ioapic_send_eoi(INTERRUPT_IOAPIC_BASE - 1U) !=
            IOAPIC_STATUS_BAD_VECTOR ||
        ioapic_send_eoi(INTERRUPT_LOCAL_APIC_BASE) !=
            IOAPIC_STATUS_BAD_VECTOR ||
        ioapic_read_redirection(INTERRUPT_IOAPIC_BASE, NULL) !=
            IOAPIC_STATUS_NULL_ARGUMENT ||
        ioapic_read_redirection(INTERRUPT_IOAPIC_BASE, &entry) !=
            IOAPIC_STATUS_VECTOR_NOT_ROUTED) {
        kernel_test_fail("an unrouted vector was acknowledged");
    }

    /* And every malformed routing request, through the public interface. */
    if (ioapic_route_isa_irq_as(0U, INTERRUPT_IOAPIC_BASE, 0U,
            (enum ioapic_trigger)7) != IOAPIC_STATUS_BAD_TRIGGER ||
        ioapic_route_isa_irq_as(0U, INTERRUPT_IOAPIC_BASE, UINT8_MAX + 1U,
            IOAPIC_TRIGGER_FORCE_LEVEL) != IOAPIC_STATUS_BAD_DESTINATION ||
        ioapic_route_isa_irq_as(UINT8_C(16), INTERRUPT_IOAPIC_BASE, 0U,
            IOAPIC_TRIGGER_FORCE_LEVEL) != IOAPIC_STATUS_BAD_IRQ) {
        kernel_test_fail("a malformed routing request was accepted");
    }

    /* A wait needs a running timer, a target and a bound the clock can hold. */
    if (pit_wait_for_ticks_bounded(1U, IOAPIC_LEVEL_TEST_BOUND_NS, NULL) !=
            PIT_STATUS_NULL_ARGUMENT ||
        pit_wait_for_ticks_bounded(1U, IOAPIC_LEVEL_TEST_BOUND_NS,
            &elapsed_ns) != PIT_STATUS_NOT_RUNNING ||
        elapsed_ns != 0U) {
        kernel_test_fail("a bounded wait ran without a running timer");
    }

    if (pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_IO_APIC_LEVEL) !=
        PIT_STATUS_OK) {
        kernel_test_fail("the timer would not take the level-triggered route");
    }

    if (pit_active_route() != PIT_ROUTE_IO_APIC_LEVEL ||
        pit_wait_for_ticks_bounded(0U, IOAPIC_LEVEL_TEST_BOUND_NS,
            &elapsed_ns) != PIT_STATUS_BAD_INTERVAL ||
        pit_wait_for_ticks_bounded(1U, PIT_MAX_WAIT_NS + 1U, &elapsed_ns) !=
            PIT_STATUS_BAD_INTERVAL) {
        kernel_test_fail("a bounded wait accepted an interval it cannot hold");
    }

    /*
     * Read the entry off the hardware. An entry programmed edge triggered while
     * Phipia's records called it level triggered would deliver every interrupt
     * below and latch nothing, so this is the check that catches it.
     */
    if (ioapic_read_redirection(pit_active_vector(), &entry) !=
            IOAPIC_STATUS_OK ||
        !entry.level_triggered || entry.masked || entry.active_low ||
        entry.vector != pit_active_vector() ||
        entry.global_interrupt != 2U) {
        kernel_test_fail("the level route did not read back level triggered");
    }

    if (!ioapic_vector_is_level_triggered(pit_active_vector()) ||
        ioapic_get_state().level_routes != 1U) {
        kernel_test_fail("the level route was not recorded as level triggered");
    }

    if (pic_mask_snapshot() != UINT16_C(0xFFFF)) {
        kernel_test_fail("level routing unmasked a legacy PIC line");
    }

    /*
     * A vector names one pin. Pointing this one at IRQ4's pin as well would
     * leave the timer's entry unmasked and delivering a vector the dispatcher
     * would acknowledge on the wrong unit, so it is refused by name.
     */
    if (ioapic_route_isa_irq(4U, pit_active_vector(), 0U) !=
        IOAPIC_STATUS_VECTOR_IN_USE) {
        kernel_test_fail("one vector was pointed at two redirection entries");
    }

    before = ioapic_get_state();

    if (pit_wait_for_ticks_bounded(
            IOAPIC_LEVEL_TEST_TICKS,
            IOAPIC_LEVEL_TEST_BOUND_NS,
            &elapsed_ns
        ) != PIT_STATUS_OK) {
        kernel_test_fail("the level-triggered line stopped delivering");
    }

    after = ioapic_get_state();

    /*
     * Stop before printing: framebuffer-backed console output can exceed the
     * next one-shot period and queue a vector after the handler is removed.
     */
    if (pit_stop() != PIT_STATUS_OK) {
        kernel_test_fail("the level route would not stop");
    }

    console_write("ST INFO ioapic-level: ");
    console_write_u64(pit_ticks());
    console_write(" deliveries, remote IRR ");
    console_write_u64(after.remote_irr_observed);
    console_write(", directed EOI ");
    console_write_u64(after.directed_eoi_count);
    console_write(", mode ");
    console_write(after.directed_eoi_mode ? "directed" : "broadcast");
    console_write(", in ");
    console_write_u64(elapsed_ns);
    console_write(" ns\n");

    if (pit_ticks() < IOAPIC_LEVEL_TEST_TICKS) {
        kernel_test_fail("a level-triggered line delivered too few interrupts");
    }

    /*
     * And not far more than it was asked for. A pin acknowledged while its
     * source is still asserting re-delivers inside the acknowledgement, so it
     * counts thousands of interrupts in the time eight should take. The
     * interval check below would fail on that too, but only after describing
     * it as a timing problem; this names it for what it is.
     */
    if (pit_ticks() > IOAPIC_LEVEL_TEST_TICKS * 2U) {
        kernel_test_fail("a level-triggered line delivered without stopping");
    }

    /*
     * Every delivery latched remote IRR and every one was acknowledged at the
     * I/O APIC. The counts are what say the entry behaved as a level-triggered
     * entry rather than merely that interrupts arrived.
     */
    if (after.remote_irr_observed - before.remote_irr_observed <
            IOAPIC_LEVEL_TEST_TICKS ||
        after.remote_irr_missing != 0U ||
        (after.directed_eoi_mode &&
         (after.directed_eoi_count - before.directed_eoi_count <
              IOAPIC_LEVEL_TEST_TICKS ||
          !apic_get_state().eoi_broadcasts_suppressed)) ||
        (!after.directed_eoi_mode &&
         after.directed_eoi_count != before.directed_eoi_count)) {
        kernel_test_fail("a level-triggered delivery did not latch remote IRR");
    }

    /*
     * Refuse a source that re-delivers too quickly. The bounded wait supplies
     * the upper limit; a host pause may legitimately stretch emulated time.
     */
    if (elapsed_ns <
        expected_ns - expected_ns / PM_TIMER_TOLERANCE_QUARTER) {
        kernel_test_fail("level-triggered deliveries did not take a period");
    }

    /* Stopping unroutes the entry, so its vector is nothing's again. */
    if (ioapic_vector_is_level_triggered(pit_active_vector()) ||
        ioapic_get_state().level_routes != 0U ||
        ioapic_read_redirection(pit_active_vector(), &entry) !=
            IOAPIC_STATUS_VECTOR_NOT_ROUTED) {
        kernel_test_fail("a stopped level route is still routed");
    }

    /*
     * The same pin, sampled as an edge again. An edge-triggered entry has no
     * remote IRR to latch and nothing to acknowledge, so both counters must
     * stand still across eight more deliveries. Without this, an
     * implementation that treated every route as level triggered would pass
     * everything above.
     */
    before = ioapic_get_state();

    if (pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_IO_APIC) != PIT_STATUS_OK ||
        ioapic_vector_is_level_triggered(pit_active_vector()) ||
        ioapic_send_eoi(pit_active_vector()) !=
            IOAPIC_STATUS_NOT_LEVEL_TRIGGERED) {
        kernel_test_fail("an edge route was treated as level triggered");
    }

    if (pit_wait_for_ticks_bounded(
            IOAPIC_LEVEL_TEST_TICKS,
            IOAPIC_LEVEL_TEST_BOUND_NS,
            &elapsed_ns
        ) != PIT_STATUS_OK) {
        kernel_test_fail("the edge-triggered line stopped delivering");
    }

    after = ioapic_get_state();

    if (after.remote_irr_observed != before.remote_irr_observed ||
        after.directed_eoi_count != before.directed_eoi_count ||
        after.level_routes != 0U) {
        kernel_test_fail("an edge-triggered delivery latched remote IRR");
    }

    if (pit_stop() != PIT_STATUS_OK) {
        kernel_test_fail("the edge route would not stop");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

/*
 * Retire the 8259 pair and prove the machine keeps its timer. This is the
 * scenario that would catch a retirement which silently took interrupt
 * delivery with it.
 */
static void retired_scenario(void)
{
    enum pit_status pit_status;
    enum pic_status pic_status;

    if (!pic_is_initialized() || pic_is_retired()) {
        kernel_test_fail("legacy PIC was not in its expected initial state");
    }

    pic_status = pic_retire();

    if (pic_status != PIC_STATUS_OK) {
        kernel_test_fail(pic_status_string(pic_status));
    }

    if (apic_retire_legacy_routing() != APIC_STATUS_OK) {
        kernel_test_fail("local APIC kept carrying legacy interrupts");
    }

    if (!pic_is_retired() || pic_mask_snapshot() != UINT16_C(0xFFFF)) {
        kernel_test_fail("legacy PIC is not fully masked after retirement");
    }

    if (pic_set_mask(0U, false) != PIC_STATUS_RETIRED ||
        pic_retire() != PIC_STATUS_RETIRED) {
        kernel_test_fail("retired PIC accepted a further mutation");
    }

    if (apic_get_state().legacy_interrupts_routed) {
        kernel_test_fail("local APIC still reports legacy routing");
    }

    pit_status = pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_IO_APIC);

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    pit_status = pit_wait_for_ticks(PIT_TEST_TICKS);

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    pit_status = pit_stop();

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    if (pit_ticks() < PIT_TEST_TICKS) {
        kernel_test_fail("timer stopped once the legacy PIC was retired");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

/*
 * Calibrate and run the local APIC timer, then check the rate it measured is
 * consistent with the reference it measured against. A timer that ticks but
 * counts at the wrong rate is the failure this scenario exists to find.
 */
static void apic_timer_scenario(void)
{
    enum apic_timer_status status;
    uint64_t elapsed_ticks;
    uint64_t measured_ns;
    uint64_t expected_ns;
    uint32_t start;
    uint32_t span = 0U;

    if (!apic_is_online()) {
        kernel_test_fail("local APIC is not online");
    }

    if (apic_timer_is_calibrated() || apic_timer_is_running()) {
        kernel_test_fail("local APIC timer was already in use");
    }

    if (apic_timer_start(APIC_TIMER_TEST_FREQUENCY) !=
        APIC_TIMER_STATUS_NOT_CALIBRATED) {
        kernel_test_fail("uncalibrated local APIC timer agreed to run");
    }

    status = apic_timer_calibrate();

    if (status != APIC_TIMER_STATUS_OK) {
        kernel_test_fail(apic_timer_status_string(status));
    }

    if (apic_timer_counts_per_second() == 0U) {
        kernel_test_fail("local APIC timer calibrated to a zero rate");
    }

    if (apic_timer_calibrate() != APIC_TIMER_STATUS_ALREADY_CALIBRATED) {
        kernel_test_fail("local APIC timer accepted a second calibration");
    }

    status = apic_timer_start(APIC_TIMER_TEST_FREQUENCY);

    if (status != APIC_TIMER_STATUS_OK) {
        kernel_test_fail(apic_timer_status_string(status));
    }

    if (apic_timer_start(APIC_TIMER_TEST_FREQUENCY) !=
        APIC_TIMER_STATUS_ALREADY_RUNNING) {
        kernel_test_fail("local APIC timer started twice");
    }

    /*
     * Let the timer count out a known number of its own ticks and measure how
     * long that took on the ACPI timer it was calibrated from. Checking the
     * duration rather than a tick count catches the failure a tick count cannot:
     * a timer whose rate is wrong still delivers every tick it is asked for, it
     * just takes the wrong amount of time doing it.
     */
    start = pm_timer_read();

    if (apic_timer_wait_for_ticks(APIC_TIMER_TEST_TICKS) !=
        APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer stopped delivering");
    }

    elapsed_ticks = apic_timer_ticks();
    status = apic_timer_stop();

    if (status != APIC_TIMER_STATUS_OK) {
        kernel_test_fail(apic_timer_status_string(status));
    }

    if (pm_timer_span(start, pm_timer_read(), &span) != PM_TIMER_STATUS_OK) {
        kernel_test_fail("reference clock reported no duration");
    }

    if (elapsed_ticks < APIC_TIMER_TEST_TICKS) {
        kernel_test_fail("local APIC timer delivered too few interrupts");
    }

    measured_ns = pm_timer_ticks_to_nanoseconds(span);
    expected_ns = APIC_TIMER_TEST_TICKS * UINT64_C(1000000000) /
        APIC_TIMER_TEST_FREQUENCY;

    if (!pm_timer_durations_agree(
            measured_ns,
            expected_ns,
            PM_TIMER_TOLERANCE_QUARTER
        )) {
        kernel_test_fail("local APIC timer rate disagrees with its reference");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

/*
 * Establish the time-stamp counter as a second reference and check it against
 * the first. Two clocks calibrated from the same ruler must agree about the
 * same interval; a clock that only agrees with itself proves nothing.
 */
static void tsc_scenario(void)
{
    struct tsc_state tsc;
    uint64_t previous;
    uint64_t start;
    uint64_t measured_ns;
    uint64_t expected_ns;
    enum tsc_status status;

    if (tsc_is_calibrated()) {
        kernel_test_fail("TSC was already calibrated");
    }

    if (tsc_span_nanoseconds(0U, UINT64_C(1000)) != 0U) {
        kernel_test_fail("uncalibrated TSC reported a duration");
    }

    status = tsc_calibrate();

    if (status != TSC_STATUS_OK) {
        kernel_test_fail(tsc_status_string(status));
    }

    tsc = tsc_get_state();

    if (!tsc.present || tsc.frequency_hz == 0U) {
        kernel_test_fail("TSC calibrated to an unusable rate");
    }

    if (tsc_calibrate() != TSC_STATUS_ALREADY_CALIBRATED) {
        kernel_test_fail("TSC accepted a second calibration");
    }

    /* A counter that steps backwards cannot order anything. */
    previous = tsc_read();

    for (size_t index = 0; index < TSC_MONOTONIC_READS; ++index) {
        const uint64_t current = tsc_read();

        if (current < previous) {
            kernel_test_fail("TSC ran backwards between reads");
        }

        previous = current;
    }

    if (apic_timer_calibrate() != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("second clock would not calibrate");
    }

    if (apic_timer_start(APIC_TIMER_TEST_FREQUENCY) != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("second clock would not start");
    }

    start = tsc_read();

    if (apic_timer_wait_for_ticks(APIC_TIMER_TEST_TICKS) !=
        APIC_TIMER_STATUS_OK) {
        kernel_test_fail("second clock stopped delivering");
    }

    measured_ns = tsc_span_nanoseconds(start, tsc_read());

    if (apic_timer_stop() != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("second clock would not stop");
    }

    expected_ns = APIC_TIMER_TEST_TICKS * UINT64_C(1000000000) /
        APIC_TIMER_TEST_FREQUENCY;

    if (measured_ns < expected_ns / 2U || measured_ns > expected_ns * 2U) {
        kernel_test_fail("TSC and local APIC timer disagree about an interval");
    }
}

/*
 * Establish the ACPI power management timer as an independent reference, then
 * check the two calibrated clocks against it.
 *
 * The local APIC timer and the TSC were both measured against the PIT, so they
 * agree with each other even if that shared measurement was wrong. This timer's
 * rate is fixed by the ACPI specification and is measured against nothing, so
 * one interval described by all three is the first evidence that the PIT
 * measurement itself was right. Retiring the PIT is the increment after this
 * one, and only on the strength of this agreement.
 */
static void pm_timer_scenario(void)
{
    struct pm_timer_state pm;
    struct acpi_fadt probe;
    uint64_t waited_ticks = 0U;
    uint64_t tsc_start;
    uint64_t measured_ns;
    uint64_t reference_ns;
    uint64_t expected_ns;
    uint32_t start;
    uint32_t end;
    uint32_t span = 0U;

    if (!pm_timer_is_present()) {
        kernel_test_fail("ACPI PM timer was not discovered during boot");
    }

    pm = pm_timer_get_state();

    if (pm.port == 0U ||
        (pm.counter_bits != ACPI_PM_TIMER_BASE_BITS &&
         pm.counter_bits != ACPI_PM_TIMER_EXTENDED_BITS)) {
        kernel_test_fail("ACPI PM timer reported an unusable description");
    }

    /*
     * The timer is discovered once. A second description is refused before any
     * of its fields are read, so a zeroed one is enough to prove the refusal.
     */
    acpi_bytes_zero(&probe, sizeof(probe));

    if (pm_timer_initialize(&probe) != PM_TIMER_STATUS_ALREADY_INITIALIZED) {
        kernel_test_fail("ACPI PM timer accepted a second description");
    }

    /* The counter has to advance on its own before it can time anything. */
    if (pm_timer_wait(PM_TIMER_TEST_TICKS, &waited_ticks) !=
        PM_TIMER_STATUS_OK) {
        kernel_test_fail("ACPI PM timer did not advance within its bound");
    }

    if (waited_ticks < PM_TIMER_TEST_TICKS) {
        kernel_test_fail("ACPI PM timer wait returned early");
    }

    if (tsc_calibrate() != TSC_STATUS_OK) {
        kernel_test_fail("time-stamp counter would not calibrate");
    }

    if (apic_timer_calibrate() != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer would not calibrate");
    }

    if (apic_timer_start(PM_TIMER_TEST_FREQUENCY) != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer would not start");
    }

    /*
     * One interval, three opinions. The APIC timer defines it by counting its
     * own ticks; the PM timer and the TSC each measure it without being told.
     */
    start = pm_timer_read();
    tsc_start = tsc_read();

    if (apic_timer_wait_for_ticks(PM_TIMER_TEST_APIC_TICKS) !=
        APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer stopped delivering");
    }

    end = pm_timer_read();
    reference_ns = tsc_span_nanoseconds(tsc_start, tsc_read());

    if (apic_timer_stop() != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer would not stop");
    }

    if (pm_timer_span(start, end, &span) != PM_TIMER_STATUS_OK) {
        kernel_test_fail("ACPI PM timer span is not a duration");
    }

    measured_ns = pm_timer_ticks_to_nanoseconds(span);
    expected_ns = PM_TIMER_TEST_APIC_TICKS * UINT64_C(1000000000) /
        PM_TIMER_TEST_FREQUENCY;

    console_write("ST INFO pm-timer: PM ");
    console_write_u64(measured_ns);
    console_write(" ns, APIC timer ");
    console_write_u64(expected_ns);
    console_write(" ns, TSC ");
    console_write_u64(reference_ns);
    console_write(" ns\n");

    /*
     * The local APIC timer is held to the tight bound: it and the PIT it was
     * calibrated against are driven from the same source under emulation, so
     * this comparison is the one that must catch a rate wrong by a factor.
     */
    if (!pm_timer_durations_agree(
            measured_ns,
            expected_ns,
            PM_TIMER_TOLERANCE_QUARTER
        )) {
        kernel_test_fail("PM timer and local APIC timer disagree on interval");
    }

    if (!pm_timer_durations_agree(
            measured_ns,
            reference_ns,
            PM_TIMER_TOLERANCE_HALF
        )) {
        kernel_test_fail("PM timer and TSC disagree about an interval");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

/*
 * Retire the 8254, reject further PIT changes, then calibrate and cross-check
 * both derived clocks without it.
 */
static void pit_retired_scenario(void)
{
    struct pm_timer_state pm;
    uint64_t tsc_start;
    uint64_t measured_ns;
    uint64_t reference_ns;
    uint64_t expected_ns;
    uint32_t start;
    uint32_t span = 0U;

    if (!pm_timer_is_present()) {
        kernel_test_fail("ACPI PM timer was not discovered during boot");
    }

    pm = pm_timer_get_state();

    if (pm.port == 0U) {
        kernel_test_fail("ACPI PM timer reported an unusable description");
    }

    /* The PIT still works at this point, and is proved so before it goes. */
    if (pit_is_retired()) {
        kernel_test_fail("PIT was already retired");
    }

    if (pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_IO_APIC) != PIT_STATUS_OK) {
        kernel_test_fail("PIT would not start before its retirement");
    }

    if (pit_wait_for_ticks(PIT_TEST_TICKS) != PIT_STATUS_OK) {
        kernel_test_fail("PIT would not deliver before its retirement");
    }

    if (pit_retire() != PIT_STATUS_OK) {
        kernel_test_fail("PIT refused to retire");
    }

    /* A retired PIT is latched: running, restarting and re-retiring all fail. */
    if (!pit_is_retired() || pit_is_running()) {
        kernel_test_fail("PIT is not fully retired");
    }

    if (pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_IO_APIC) !=
            PIT_STATUS_RETIRED ||
        pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_LEGACY_PIC) !=
            PIT_STATUS_RETIRED ||
        pit_retire() != PIT_STATUS_RETIRED) {
        kernel_test_fail("retired PIT accepted a further mutation");
    }

    /* Both derived clocks must calibrate with no PIT to lean on. */
    if (tsc_calibrate() != TSC_STATUS_OK) {
        kernel_test_fail("TSC would not calibrate without the PIT");
    }

    if (apic_timer_calibrate() != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer would not calibrate without the PIT");
    }

    if (apic_timer_counts_per_second() == 0U || tsc_frequency() == 0U) {
        kernel_test_fail("a clock calibrated to an unusable rate");
    }

    if (apic_timer_start(PM_TIMER_TEST_FREQUENCY) != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer would not start");
    }

    start = pm_timer_read();
    tsc_start = tsc_read();

    if (apic_timer_wait_for_ticks(PM_TIMER_TEST_APIC_TICKS) !=
        APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer stopped delivering");
    }

    reference_ns = tsc_span_nanoseconds(tsc_start, tsc_read());

    if (apic_timer_stop() != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer would not stop");
    }

    if (pm_timer_span(start, pm_timer_read(), &span) != PM_TIMER_STATUS_OK) {
        kernel_test_fail("ACPI PM timer span is not a duration");
    }

    measured_ns = pm_timer_ticks_to_nanoseconds(span);
    expected_ns = PM_TIMER_TEST_APIC_TICKS * UINT64_C(1000000000) /
        PM_TIMER_TEST_FREQUENCY;

    console_write("ST INFO pit-retired: PM ");
    console_write_u64(measured_ns);
    console_write(" ns, APIC timer ");
    console_write_u64(expected_ns);
    console_write(" ns, TSC ");
    console_write_u64(reference_ns);
    console_write(" ns\n");

    if (!pm_timer_durations_agree(
            measured_ns,
            expected_ns,
            PM_TIMER_TOLERANCE_QUARTER
        )) {
        kernel_test_fail("clocks disagree on an interval without the PIT");
    }

    if (!pm_timer_durations_agree(
            measured_ns,
            reference_ns,
            PM_TIMER_TOLERANCE_HALF
        )) {
        kernel_test_fail("PM timer and TSC disagree without the PIT");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

/*
 * Written by timer callbacks inside the timer interrupt and read by the scenario
 * outside it, so the compiler must not keep either in a register across the halt
 * inside a sleep.
 */
static volatile uint32_t timers_fired[TIMERS_TEST_COUNT];
static volatile size_t timers_fired_count;

static void timers_record(uint64_t deadline_ns, void *context)
{
    (void)deadline_ns;

    if (timers_fired_count < TIMERS_TEST_COUNT) {
        timers_fired[timers_fired_count] = *(const uint32_t *)context;
        ++timers_fired_count;
    }
}

/*
 * Establish the monotonic clock and deadline timers, and prove the two things
 * that make them usable: the clock never steps backwards, and a deadline arrives
 * after the instant it named rather than before it.
 *
 * A sleep that returns early is the failure worth hunting. It would not look
 * like a failure - the call returns, the callback ran - and every wait built on
 * it would be silently short. So the scenario checks the elapsed time against
 * the clock rather than trusting that the callback fired.
 */
static void timers_scenario(void)
{
    static const uint32_t labels[TIMERS_TEST_COUNT] = {1U, 2U, 3U};
    size_t heap_live_before;
    uint64_t identifiers[TIMERS_TEST_COUNT] = {0U, 0U, 0U};
    uint64_t previous;
    uint64_t start;
    uint64_t slept_ns;
    uint64_t spare = 0U;
    uint64_t now;

    /* Before the clock has an origin it reports nothing rather than garbage. */
    if (clock_is_started() || clock_monotonic_ns() != 0U) {
        kernel_test_fail("monotonic clock was already started");
    }

    if (clock_start() != CLOCK_STATUS_NO_SOURCE) {
        kernel_test_fail("clock started without a calibrated counter");
    }

    if (tsc_calibrate() != TSC_STATUS_OK) {
        kernel_test_fail("TSC would not calibrate");
    }

    if (apic_timer_calibrate() != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer would not calibrate");
    }

    if (clock_start() != CLOCK_STATUS_OK) {
        kernel_test_fail("monotonic clock would not start");
    }

    if (clock_start() != CLOCK_STATUS_ALREADY_STARTED) {
        kernel_test_fail("monotonic clock started twice");
    }

    /* A clock that steps backwards cannot order anything. */
    previous = clock_monotonic_ns();

    for (size_t index = 0; index < TSC_MONOTONIC_READS; ++index) {
        now = clock_monotonic_ns();

        if (now < previous) {
            kernel_test_fail("monotonic clock stepped backwards");
        }

        previous = now;
    }

    if (clock_get_state().backward_steps != 0U) {
        kernel_test_fail("monotonic clock had to repair a reading");
    }

    /* Deadlines need the clock, and refuse to run without it. */
    if (timer_arm(previous + TIMERS_TEST_STEP_NS, timers_record, NULL, &spare) !=
        TIMER_STATUS_NOT_STARTED) {
        kernel_test_fail("deadline armed before the timers were started");
    }

    /*
     * The deadline table is a heap allocation now, not a static array, so
     * starting must take exactly one block and report the capacity it got.
     */
    heap_live_before = heap_get_state().live_allocations;

    if (timer_capacity() != 0U) {
        kernel_test_fail("deadline timers held a table before starting");
    }

    if (timer_start() != TIMER_STATUS_OK) {
        kernel_test_fail("deadline timers would not start");
    }

    if (timer_capacity() != TIMER_MAX_PENDING ||
        heap_get_state().live_allocations != heap_live_before + 1U) {
        kernel_test_fail("starting did not take one table from the heap");
    }

    if (timer_start() != TIMER_STATUS_ALREADY_STARTED) {
        kernel_test_fail("deadline timers started twice");
    }

    /* A deadline already gone, and one too near to program, are both refused. */
    now = clock_monotonic_ns();

    if (timer_arm(0U, timers_record, NULL, &spare) !=
            TIMER_STATUS_BAD_INTERVAL ||
        spare != 0U ||
        timer_arm(now + 1U, timers_record, NULL, &spare) !=
            TIMER_STATUS_BAD_INTERVAL) {
        kernel_test_fail("a deadline in the past was accepted");
    }

    if (timer_cancel(0U) != TIMER_STATUS_UNKNOWN_TIMER ||
        timer_cancel(UINT64_MAX) != TIMER_STATUS_UNKNOWN_TIMER) {
        kernel_test_fail("cancelling an unknown deadline was accepted");
    }

    /* Three deadlines, armed out of order, must fire in time order. */
    timers_fired_count = 0U;
    start = clock_monotonic_ns();

    for (size_t index = 0; index < TIMERS_TEST_COUNT; ++index) {
        const size_t reversed = TIMERS_TEST_COUNT - 1U - index;

        if (timer_arm(
                start + TIMERS_TEST_STEP_NS * (uint64_t)(reversed + 1U),
                timers_record,
                (void *)&labels[reversed],
                &identifiers[reversed]
            ) != TIMER_STATUS_OK ||
            identifiers[reversed] == 0U) {
            kernel_test_fail("a deadline would not arm");
        }
    }

    if (timer_pending_count() != TIMERS_TEST_COUNT) {
        kernel_test_fail("deadline timer table lost an entry");
    }

    /* Sleeping past all three collects them; the sleep is itself a deadline. */
    slept_ns = clock_monotonic_ns();

    if (timer_sleep_ns(TIMERS_TEST_STEP_NS * (TIMERS_TEST_COUNT + 1U)) !=
        TIMER_STATUS_OK) {
        kernel_test_fail("sleep did not complete");
    }

    slept_ns = clock_monotonic_ns() - slept_ns;

    if (timers_fired_count != TIMERS_TEST_COUNT) {
        kernel_test_fail("not every deadline fired");
    }

    for (size_t index = 0; index < TIMERS_TEST_COUNT; ++index) {
        if (timers_fired[index] != labels[index]) {
            kernel_test_fail("deadlines fired out of order");
        }
    }

    if (slept_ns < TIMERS_TEST_STEP_NS * (TIMERS_TEST_COUNT + 1U)) {
        kernel_test_fail("sleep returned before its deadline");
    }

    if (timer_pending_count() != 0U || timer_expiry_count() == 0U) {
        kernel_test_fail("deadline timer table did not settle");
    }

    /* A cancelled deadline must not fire, and its identifier must go stale. */
    timers_fired_count = 0U;
    now = clock_monotonic_ns();

    if (timer_arm(
            now + TIMERS_TEST_STEP_NS,
            timers_record,
            (void *)&labels[0],
            &spare
        ) != TIMER_STATUS_OK) {
        kernel_test_fail("a deadline would not arm for cancellation");
    }

    if (timer_cancel(spare) != TIMER_STATUS_OK ||
        timer_cancel(spare) != TIMER_STATUS_UNKNOWN_TIMER ||
        timer_pending_count() != 0U) {
        kernel_test_fail("cancelling a deadline did not release it");
    }

    if (timer_sleep_ns(TIMERS_TEST_STEP_NS * 2U) != TIMER_STATUS_OK) {
        kernel_test_fail("sleep after a cancellation did not complete");
    }

    if (timers_fired_count != 0U) {
        kernel_test_fail("a cancelled deadline fired anyway");
    }

    if (timer_stop() != TIMER_STATUS_OK ||
        timer_is_started() ||
        timer_stop() != TIMER_STATUS_NOT_STARTED) {
        kernel_test_fail("deadline timers would not stop");
    }

    /*
     * And stopping gives it back. A subsystem that took heap memory once per
     * start and never returned it would look perfectly correct in every other
     * check here and exhaust the heap over a long-running kernel.
     */
    if (timer_capacity() != 0U ||
        heap_get_state().live_allocations != heap_live_before) {
        kernel_test_fail("stopping did not return the table to the heap");
    }

    if (heap_verify() != HEAP_STATUS_OK) {
        kernel_test_fail("the heap did not survive the deadline table");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

/*
 * Written through a volatile pointer inside the scenario and read back after a
 * permission change, so the compiler cannot cache either side of it or assume
 * it knows what a page it never mapped contains.
 */
static volatile uint8_t paging_scratch;

/*
 * Prove the permissions are enforced by the processor rather than merely
 * recorded in a table.
 *
 * `make verify` has always refused an RWX load segment, and until this
 * increment that assertion was the only thing standing behind Phipia's W^X
 * claim - and it inspects the ELF file, not the machine the kernel runs on.
 * Everything below the rejections is the part a file check can never do: a
 * fresh frame is mapped writable, written, narrowed to read-only, and written
 * again, and the scenario passes only if the processor refuses the second write
 * with the exact fault a supervisor write to a present read-only page produces.
 *
 * The probe returns if the store succeeds, so a permission that quietly failed
 * to take shows up as a scenario failure rather than as a timeout.
 */
static void paging_scenario(const struct paging_device_windows *device_windows)
{
    volatile uint8_t *probe =
        (volatile uint8_t *)(uintptr_t)PAGING_PROBE_ADDRESS;
    const uint64_t text = (uint64_t)(uintptr_t)(const void *)
        paging_probe_write_site;
    const uint64_t data = (uint64_t)(uintptr_t)(const void *)&paging_scratch;
    struct paging_translation translation;
    struct paging_state paging;
    struct paging_audit audit;
    size_t frames_before;
    size_t tables_before;
    uintptr_t frame;

    if (!paging_is_active()) {
        kernel_test_fail("kernel page tables are not installed");
    }

    paging = paging_get_state();

    if (!paging.no_execute_active || !paging.write_protect_active) {
        kernel_test_fail("W^X is not enforceable on this processor");
    }

    if (paging.root_physical_address == 0U || paging.table_frames == 0U ||
        paging_verify() != PAGING_STATUS_OK) {
        kernel_test_fail("installed page tables do not match their intent");
    }

    /* The whole point of the increment, read back off the live hierarchy. */
    if (paging_audit_hierarchy(&audit) != PAGING_STATUS_OK ||
        audit.leaf_count == 0U || audit.executable_leaves == 0U ||
        audit.write_execute_leaves != 0U || audit.user_leaves != 0U) {
        kernel_test_fail("a live mapping is writable and executable");
    }

    if (paging_translate(text, &translation) != PAGING_STATUS_OK ||
        translation.permissions != PAGING_EXECUTE ||
        translation.physical_address != text) {
        kernel_test_fail("kernel text is not read-only and executable");
    }

    if (paging_translate(data, &translation) != PAGING_STATUS_OK ||
        translation.permissions != PAGING_WRITE ||
        translation.physical_address != data) {
        kernel_test_fail("kernel data is not writable and non-executable");
    }

    /* The null page is absent, so a null dereference cannot read low memory. */
    if (paging_translate(0U, &translation) != PAGING_STATUS_NOT_MAPPED ||
        translation.level != 0U) {
        kernel_test_fail("the null page is mapped");
    }

    /* Every refusal, through the public interface, against the live tables. */
    if (paging_map(PAGING_PROBE_ADDRESS + 1U, 0U, PHIPIA_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_UNALIGNED_ADDRESS ||
        paging_map(PAGING_PROBE_ADDRESS, 0U, 0U, PAGING_WRITE) !=
            PAGING_STATUS_ZERO_LENGTH ||
        paging_map(UINT64_C(0x0000800000000000), 0U, PHIPIA_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_NONCANONICAL_ADDRESS ||
        paging_map(PAGING_PROBE_ADDRESS, 0U, PHIPIA_PAGE_SIZE,
            PAGING_WRITE | PAGING_EXECUTE) !=
            PAGING_STATUS_WRITABLE_AND_EXECUTABLE) {
        kernel_test_fail("a malformed mapping request was accepted");
    }

    if (paging_map(text & ~(PHIPIA_PAGE_SIZE - 1U), 0U, PHIPIA_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_ALREADY_MAPPED ||
        paging_unmap(PAGING_PROBE_ADDRESS, PHIPIA_PAGE_SIZE) !=
            PAGING_STATUS_NOT_MAPPED ||
        paging_protect(PAGING_PROBE_ADDRESS, PHIPIA_PAGE_SIZE, PAGING_READ) !=
            PAGING_STATUS_NOT_MAPPED) {
        kernel_test_fail("an impossible mapping change was accepted");
    }

    /*
     * The bulk identity map uses 2 MiB leaves, and splitting one is deferred,
     * so a 4 KiB change inside one is refused rather than silently applied to
     * the whole 2 MiB.
     */
    if (paging_protect(PAGING_TEST_HUGE_ADDRESS, PHIPIA_PAGE_SIZE,
            PAGING_READ) != PAGING_STATUS_HUGE_PAGE_PRESENT ||
        paging_unmap(PAGING_TEST_HUGE_ADDRESS, PHIPIA_PAGE_SIZE) !=
            PAGING_STATUS_HUGE_PAGE_PRESENT) {
        kernel_test_fail("a 2 MiB mapping accepted a 4 KiB change");
    }

    if (paging_initialize(device_windows) !=
        PAGING_STATUS_ALREADY_INITIALIZED) {
        kernel_test_fail("page tables accepted a second installation");
    }

    if (frame_allocate(&frame) != FRAME_STATUS_OK) {
        kernel_test_fail("no frame was available for the probe page");
    }

    if (paging_map(PAGING_PROBE_ADDRESS, frame, PHIPIA_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_OK ||
        paging_map(PAGING_PROBE_ADDRESS, frame, PHIPIA_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_ALREADY_MAPPED) {
        kernel_test_fail("the probe page would not map exactly once");
    }

    *probe = PAGING_TEST_PATTERN;

    if (*probe != PAGING_TEST_PATTERN) {
        kernel_test_fail("a writable mapping did not hold a write");
    }

    if (paging_translate(PAGING_PROBE_ADDRESS, &translation) !=
            PAGING_STATUS_OK ||
        translation.physical_address != (uint64_t)frame ||
        translation.permissions != PAGING_WRITE ||
        translation.level != 1U) {
        kernel_test_fail("the probe page does not translate to its frame");
    }

    if (paging_protect(PAGING_PROBE_ADDRESS, PHIPIA_PAGE_SIZE, PAGING_READ) !=
        PAGING_STATUS_OK) {
        kernel_test_fail("the probe page would not narrow to read-only");
    }

    if (paging_translate(PAGING_PROBE_ADDRESS, &translation) !=
            PAGING_STATUS_OK ||
        translation.permissions != PAGING_READ ||
        translation.physical_address != (uint64_t)frame) {
        kernel_test_fail("narrowing a mapping changed what it points at");
    }

    /* Reading is still permitted, and the contents survived the change. */
    if (*probe != PAGING_TEST_PATTERN) {
        kernel_test_fail("a read-only mapping lost the page contents");
    }

    /*
     * Map and undo the same page many times over. One leaked interior table
     * per cycle is invisible in a single pass and fatal over a long-running
     * kernel, so the check that matters is that the frame count is identical
     * after sixty-four cycles - and the paging state's own table count with it.
     */
    if (paging_unmap(PAGING_PROBE_ADDRESS, PHIPIA_PAGE_SIZE) !=
        PAGING_STATUS_OK) {
        kernel_test_fail("the probe page would not unmap before the cycle");
    }

    frames_before = frame_allocator_get_stats().free_frames;
    tables_before = paging_get_state().table_frames;

    for (size_t cycle = 0; cycle < PAGING_TEST_CYCLES; ++cycle) {
        uintptr_t cycle_frame;

        if (frame_allocate(&cycle_frame) != FRAME_STATUS_OK ||
            paging_map(PAGING_PROBE_ADDRESS, cycle_frame, PHIPIA_PAGE_SIZE,
                PAGING_WRITE) != PAGING_STATUS_OK ||
            paging_unmap(PAGING_PROBE_ADDRESS, PHIPIA_PAGE_SIZE) !=
                PAGING_STATUS_OK ||
            frame_release(cycle_frame) != FRAME_STATUS_OK) {
            kernel_test_fail("a map and unmap cycle did not complete");
        }
    }

    if (frame_allocator_get_stats().free_frames != frames_before) {
        kernel_test_fail("repeated mapping leaked a physical frame");
    }

    if (paging_get_state().table_frames != tables_before) {
        kernel_test_fail("repeated mapping leaked a page table");
    }

    if (paging_verify() != PAGING_STATUS_OK) {
        kernel_test_fail("the hierarchy did not survive repeated mapping");
    }

    /* Put the probe page back so the fault below has something to narrow. */
    if (frame_allocate(&frame) != FRAME_STATUS_OK ||
        paging_map(PAGING_PROBE_ADDRESS, frame, PHIPIA_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_OK ||
        paging_protect(PAGING_PROBE_ADDRESS, PHIPIA_PAGE_SIZE, PAGING_READ) !=
            PAGING_STATUS_OK) {
        kernel_test_fail("the probe page would not come back read-only");
    }

    console_write("ST INFO paging: read-only write to ");
    console_write_hex(PAGING_PROBE_ADDRESS);
    console_write(" expecting P=1 W=1 U=0\n");

    /*
     * The store that must fault. If the processor takes it, control returns
     * here and kernel_test_run reports the failure; if it faults,
     * kernel_test_handle_fatal_interrupt matches the vector, the error code,
     * CR2 and the faulting instruction, and passes.
     */
    paging_probe_write(probe, (uint8_t)~PAGING_TEST_PATTERN);
}

/*
 * Prove the heap hands out memory that is actually distinct, that it refuses
 * everything it should, and that its guard pages are enforced by the processor.
 *
 * The refusals matter more here than anywhere else in the kernel. An allocator
 * that accepts a pointer it never returned will happily mark a live block free
 * and hand the same bytes to two callers, and nothing downstream can detect
 * that. So every wrong pointer this scenario can construct - interior, below
 * the window, above the window, unaligned, already freed - is checked by name.
 */
static void heap_scenario(void)
{
    volatile uint8_t *bytes;
    struct paging_translation translation;
    struct heap_state heap;
    uint64_t committed_before;
    size_t pages_before;
    enum heap_status status;
    void *first = NULL;
    void *second = NULL;
    void *third = NULL;

    if (!heap_is_active() || !paging_is_active()) {
        kernel_test_fail("kernel heap is not online");
    }

    /*
     * The heap's boundary proof must be able to fill its whole 16 MiB window.
     * The screen normally owns a long-lived 3 MiB client now, so this one
     * destructive scenario relinquishes it before testing the allocator in
     * isolation. The scenario ends by faulting on the upper guard and never
     * returns to code that needs the screen.
     */
    if (screen_is_active() && screen_release() != SCREEN_STATUS_OK) {
        kernel_test_fail("the screen did not release its heap surface");
    }

    if (heap_initialize() != HEAP_STATUS_ALREADY_INITIALIZED) {
        kernel_test_fail("heap accepted a second initialization");
    }

    status = heap_verify();

    if (status != HEAP_STATUS_OK) {
        kernel_test_fail(heap_status_string(status));
    }

    /* Malformed requests, each refused by its own name. */
    if (heap_allocate(0U, &first) != HEAP_STATUS_ZERO_SIZE || first != NULL ||
        heap_allocate(HEAP_SIZE + 1U, &first) != HEAP_STATUS_TOO_LARGE ||
        first != NULL ||
        heap_allocate(16U, NULL) != HEAP_STATUS_NULL_ARGUMENT) {
        kernel_test_fail("a malformed allocation request was accepted");
    }

    if (heap_allocate(64U, &first) != HEAP_STATUS_OK || first == NULL ||
        heap_allocate(64U, &second) != HEAP_STATUS_OK || second == NULL ||
        first == second) {
        kernel_test_fail("the heap would not produce two distinct blocks");
    }

    if (((uint64_t)(uintptr_t)first & (HEAP_ALIGNMENT - 1U)) != 0U ||
        ((uint64_t)(uintptr_t)second & (HEAP_ALIGNMENT - 1U)) != 0U) {
        kernel_test_fail("the heap returned a misaligned allocation");
    }

    /* Both blocks must lie inside the window, between the guards. */
    if ((uint64_t)(uintptr_t)first < HEAP_BASE ||
        (uint64_t)(uintptr_t)second >= HEAP_GUARD_ABOVE) {
        kernel_test_fail("the heap returned a block outside its window");
    }

    /* Every wrong pointer this scenario can construct. */
    if (heap_free(NULL) != HEAP_STATUS_NULL_ARGUMENT ||
        heap_free((void *)((uintptr_t)first + 1U)) !=
            HEAP_STATUS_BAD_POINTER ||
        heap_free((void *)((uintptr_t)first + HEAP_ALIGNMENT)) !=
            HEAP_STATUS_BAD_POINTER ||
        heap_free((void *)(uintptr_t)HEAP_GUARD_BELOW) !=
            HEAP_STATUS_BAD_POINTER ||
        heap_free((void *)(uintptr_t)HEAP_GUARD_ABOVE) !=
            HEAP_STATUS_BAD_POINTER) {
        kernel_test_fail("the heap accepted a pointer it never returned");
    }

    if (heap_free(first) != HEAP_STATUS_OK ||
        heap_free(first) != HEAP_STATUS_DOUBLE_FREE) {
        kernel_test_fail("the heap accepted a double free");
    }

    /*
     * The freed block is the best fit for the same size again, so a heap that
     * reuses its free space hands back the identical address. A heap that only
     * ever grew would return something new here and slowly exhaust the window.
     */
    if (heap_allocate(64U, &third) != HEAP_STATUS_OK || third != first) {
        kernel_test_fail("the heap did not reuse a freed block");
    }

    bytes = (volatile uint8_t *)third;

    for (uint64_t index = 0; index < 64U; ++index) {
        bytes[index] = HEAP_TEST_PATTERN;
    }

    for (uint64_t index = 0; index < 64U; ++index) {
        if (bytes[index] != HEAP_TEST_PATTERN) {
            kernel_test_fail("a heap block did not hold what was written");
        }
    }

    if (heap_free(third) != HEAP_STATUS_OK ||
        heap_free(second) != HEAP_STATUS_OK) {
        kernel_test_fail("the heap refused to release its own allocation");
    }

    heap = heap_get_state();

    if (heap.live_allocations != 0U || heap.allocated_bytes != 0U ||
        heap.block_count != 1U) {
        kernel_test_fail("the heap did not coalesce back to one free block");
    }

    status = heap_verify();

    if (status != HEAP_STATUS_OK) {
        kernel_test_fail(heap_status_string(status));
    }

    /*
     * Ask for the entire window in one allocation. Which way this goes depends
     * on the machine, and both ways are worth checking, so the scenario asks
     * what happened rather than assuming how much memory it has.
     *
     * With more free memory than the window, growth commits every page and the
     * next byte requested must be refused at the window bound. With less, the
     * growth runs out of frames part way through, which is the only path that
     * exercises rollback - and then what matters is that the heap is left
     * exactly as it was, with every page that had been mapped given back.
     */
    committed_before = heap.committed_bytes;
    pages_before = heap.mapped_pages;
    status = heap_allocate(HEAP_SIZE, &first);

    if (status == HEAP_STATUS_OUT_OF_MEMORY) {
        heap = heap_get_state();

        if (first != NULL || heap_verify() != HEAP_STATUS_OK ||
            heap.committed_bytes != committed_before ||
            heap.mapped_pages != pages_before ||
            heap.live_allocations != 0U) {
            kernel_test_fail("a failed heap growth did not roll back");
        }

        console_write("ST INFO heap: growth rolled back at the frame limit\n");
    } else if (status == HEAP_STATUS_OK) {
        heap = heap_get_state();

        if (first == NULL || heap.committed_bytes != HEAP_SIZE ||
            heap.mapped_pages != HEAP_SIZE / PAGING_PAGE_SIZE) {
            kernel_test_fail("committing the window did not map every page");
        }

        if (heap_allocate(HEAP_ALIGNMENT, &second) !=
                HEAP_STATUS_OUT_OF_MEMORY ||
            second != NULL) {
            kernel_test_fail("a full heap accepted another allocation");
        }

        /* The guards survive the window being fully committed against them. */
        if (heap_verify() != HEAP_STATUS_OK ||
            heap_free(first) != HEAP_STATUS_OK ||
            heap_verify() != HEAP_STATUS_OK) {
            kernel_test_fail("heap invariants do not hold at full commitment");
        }
    } else {
        kernel_test_fail(heap_status_string(status));
    }

    /*
     * The guard page above the window is absent and stays absent. Its address
     * is one past the last byte the heap can ever hand out, so this is exactly
     * the write a caller running off the end of its last allocation would make.
     */
    if (paging_translate(HEAP_GUARD_ABOVE, &translation) !=
        PAGING_STATUS_NOT_MAPPED) {
        kernel_test_fail("the upper heap guard page is mapped");
    }

    console_write("ST INFO heap: guard write to ");
    console_write_hex(HEAP_GUARD_ABOVE);
    console_write(" expecting P=0 W=1 U=0\n");

    paging_probe_write(
        (volatile uint8_t *)(uintptr_t)HEAP_GUARD_ABOVE,
        HEAP_TEST_PATTERN
    );
}


/*
 * Read every register of every recorded function twice through the ports and
 * require the two passes to agree. Configuration reads are the foundation
 * everything above them is decoded from, so a reader that returns a different
 * answer for the same question makes every claim above it meaningless. This is
 * the check that would catch an address port left latched by something else.
 */
static void pci_reads_repeat(void)
{
    for (size_t index = 0; index < pci_function_count(); ++index) {
        const struct pci_function *function = pci_function_at(index);

        if (function == NULL) {
            kernel_test_fail("PCI returned no function for a live index");
        }

        for (uint16_t offset = 0U;
             offset <= PCI_CONFIG_SPACE_SIZE - 4U;
             offset = (uint16_t)(offset + 4U)) {
            uint32_t first = 0U;
            uint32_t second = 0U;

            if (pci_config_read_port(function->address, offset, &first) !=
                    PCI_STATUS_OK ||
                pci_config_read_port(function->address, offset, &second) !=
                    PCI_STATUS_OK) {
                kernel_test_fail("a configuration read was refused");
            }

            if (first != second) {
                kernel_test_fail("a configuration register did not read twice");
            }
        }
    }
}

/* Every refusal both readers owe, driven against the live machine. */
static void pci_refusals_are_named(void)
{
    struct pci_address address;
    uint32_t value = 0U;

    address.segment = 0U;
    address.bus = 0U;
    address.device = 0U;
    address.function = 0U;

    if (pci_config_read_port(address, 2U, &value) != PCI_STATUS_BAD_OFFSET) {
        kernel_test_fail("an unaligned configuration read was accepted");
    }

    if (pci_config_read_port(address, PCI_CONFIG_SPACE_SIZE, &value) !=
        PCI_STATUS_BAD_OFFSET) {
        kernel_test_fail("a configuration read past the space was accepted");
    }

    address.device = PCI_DEVICES_PER_BUS;

    if (pci_config_read_port(address, 0U, &value) != PCI_STATUS_BAD_ADDRESS) {
        kernel_test_fail("a device number out of range was accepted");
    }

    address.device = 0U;
    address.segment = 1U;

    if (pci_config_read_port(address, 0U, &value) != PCI_STATUS_BAD_ADDRESS) {
        kernel_test_fail("the ports accepted a segment they cannot carry");
    }

    if (pci_initialize(NULL, false) != PCI_STATUS_ALREADY_INITIALIZED) {
        kernel_test_fail("PCI accepted a second initialization");
    }
}

/*
 * At least one address on bus zero must have nothing at it, and must read all
 * ones. Absence is how enumeration decides a device is not there, so a machine
 * where absence read as anything else would produce devices that do not exist -
 * and this is the only way to check that the floating bus behaves as the
 * specification says it does.
 */
static void pci_absence_reads_all_ones(void)
{
    struct pci_address address;
    bool found_absent = false;

    address.segment = 0U;
    address.bus = 0U;
    address.function = 0U;

    for (uint8_t device = 0; device < PCI_DEVICES_PER_BUS; ++device) {
        uint32_t identity = 0U;

        address.device = device;

        if (pci_config_read_port(address, PCI_REGISTER_VENDOR_ID, &identity) !=
            PCI_STATUS_OK) {
            kernel_test_fail("a configuration read was refused");
        }

        if ((uint16_t)identity != PCI_VENDOR_ABSENT) {
            continue;
        }

        found_absent = true;

        /*
         * Not just the vendor register: an absent function floats the whole
         * bus high, so every register of it must read all ones. A machine that
         * answered zero for the rest would let a decoder invent a device with
         * class zero at every empty slot.
         */
        for (uint16_t offset = 0U;
             offset <= PCI_CONFIG_SPACE_SIZE - 4U;
             offset = (uint16_t)(offset + 4U)) {
            uint32_t value = 0U;

            if (pci_config_read_port(address, offset, &value) !=
                PCI_STATUS_OK) {
                kernel_test_fail("a configuration read was refused");
            }

            if (value != UINT32_C(0xFFFFFFFF)) {
                kernel_test_fail("an absent function did not read all ones");
            }
        }
    }

    if (!found_absent) {
        kernel_test_fail("bus zero has no empty slot to prove absence with");
    }
}

/*
 * Enumeration through the I/O ports alone, on a machine that declares no
 * configuration window. This is the path every x86 machine has, so it is the
 * one that must work without any of the rest, and the scenario asserts the
 * window really is absent rather than merely unused.
 */
static void pci_scenario(const struct acpi_mcfg *mcfg, bool mcfg_present)
{
    const struct pci_function *host_bridge;
    const struct pci_function *network;
    struct pci_state pci;
    struct pci_address address;
    enum pci_status status;
    uint32_t value = 0U;

    if (!heap_is_active() || !paging_is_active()) {
        kernel_test_fail("PCI enumeration ran without its lower layers");
    }

    if (pci_is_initialized()) {
        kernel_test_fail("PCI was initialized before its scenario");
    }

    status = pci_initialize(mcfg, mcfg_present);

    if (status != PCI_STATUS_OK) {
        kernel_test_fail(pci_status_string(status));
    }

    pci = pci_get_state();

    if (pci.ecam_active) {
        kernel_test_fail("a configuration window was mapped on this machine");
    }

    /*
     * With no window the memory reader has nothing to answer with, and says so
     * by name rather than reading whatever is at address zero.
     */
    address.segment = 0U;
    address.bus = 0U;
    address.device = 0U;
    address.function = 0U;

    if (pci_config_read_ecam(address, 0U, &value) != PCI_STATUS_NO_ECAM) {
        kernel_test_fail("the window reader answered without a window");
    }

    if (pci.compared_dwords != 0U || pci.compared_functions != 0U) {
        kernel_test_fail("a comparison was reported without two mechanisms");
    }

    if (pci.bus_count == 0U || pci.function_count == 0U) {
        kernel_test_fail("enumeration through the ports found nothing");
    }

    host_bridge = pci_find_class(PCI_CLASS_BRIDGE, PCI_SUBCLASS_HOST_BRIDGE);

    if (host_bridge == NULL) {
        kernel_test_fail("enumeration found no host bridge");
    }

    if (host_bridge->address.bus != 0U || host_bridge->address.device != 0U ||
        host_bridge->address.function != 0U) {
        kernel_test_fail("the host bridge is not at 00:00.0");
    }

    if (host_bridge->vendor_id == PCI_VENDOR_ABSENT ||
        host_bridge->vendor_id == 0U) {
        kernel_test_fail("the host bridge has no vendor");
    }

    /*
     * The host bridge is the first function on this machine, so a lookup that
     * ignored its arguments entirely would still return it and still satisfy
     * every check above. That was a negative control that passed, so two more
     * are asked: a class that is present but is not the first function, and a
     * class that is not assigned at all and must therefore be found nowhere.
     */
    network = pci_find_class(PCI_CLASS_NETWORK, 0U);

    if (network == NULL || network->class_code != PCI_CLASS_NETWORK ||
        network->subclass != 0U || network == host_bridge) {
        kernel_test_fail("the class lookup did not find the network device");
    }

    if (pci_find_class(UINT8_C(0xFE), UINT8_C(0xFE)) != NULL) {
        kernel_test_fail("the class lookup found an unassigned class");
    }

    pci_absence_reads_all_ones();
    pci_reads_repeat();
    pci_refusals_are_named();

    if (pci_verify() != PCI_STATUS_OK) {
        kernel_test_fail("PCI enumeration no longer matches the machine");
    }

    console_write("ST PCI ports functions ");
    console_write_u64(pci.function_count);
    console_write(" buses ");
    console_write_u64(pci.bus_count);
    console_putc('\n');

    if (pci_shutdown() != PCI_STATUS_OK ||
        pci_shutdown() != PCI_STATUS_NOT_INITIALIZED) {
        kernel_test_fail("PCI would not release its function table");
    }
}

/*
 * The same machine read two completely different ways.
 *
 * The port pair and the mapped window share no code below this file: one is two
 * I/O instructions, the other a load from uncacheable memory whose address is
 * computed from a firmware table. They have no reason to agree about anything
 * unless both are addressing the function the enumeration believes they are, so
 * requiring them to agree register for register is a check on the bus number
 * arithmetic, the window's base, the mapping's cacheability and the firmware
 * description all at once.
 *
 * The scenario also requires them to *disagree* about different functions,
 * because a window reader whose device and function bits went nowhere would
 * agree with the ports about 00:00.0 and answer 00:00.0 for everything else.
 */
static const struct paging_device_window *find_device_window(
    const struct paging_device_windows *windows,
    enum paging_device_window_kind kind
)
{
    if (windows == NULL) {
        return NULL;
    }

    for (size_t index = 0U; index < windows->count; ++index) {
        if (windows->entries[index].kind == kind) {
            return &windows->entries[index];
        }
    }

    return NULL;
}

static void pci_ecam_scenario(
    const struct acpi_mcfg *mcfg,
    bool mcfg_present,
    const struct paging_device_windows *device_windows
)
{
    const struct paging_device_window *ecam =
        find_device_window(device_windows, PAGING_DEVICE_WINDOW_PCI_ECAM);
    struct pci_state pci;
    struct pci_address address;
    enum pci_status status;
    size_t bridges = 0U;
    size_t message_signalled = 0U;
    size_t behind_a_bridge = 0U;
    size_t distinct_identities = 0U;
    uint32_t first_identity = 0U;
    uint32_t value = 0U;

    if (!mcfg_present) {
        kernel_test_fail("this machine declares no configuration window");
    }

    status = pci_initialize(mcfg, mcfg_present);

    if (status != PCI_STATUS_OK) {
        kernel_test_fail(pci_status_string(status));
    }

    pci = pci_get_state();

    if (!pci.ecam_active) {
        kernel_test_fail("the declared configuration window was not mapped");
    }

    if (ecam == NULL || pci.ecam_size != PAGING_ECAM_WINDOW_SIZE ||
        pci.ecam_base != ecam->physical_base ||
        pci.ecam_size != ecam->length) {
        kernel_test_fail("the window read is not the window that was mapped");
    }

    /*
     * Configuration space read through a cached mapping would be answered from
     * whatever the line held when it was last filled. QEMU under TCG models no
     * cache, so a cached window behaves identically here and no behavioural
     * test can tell the difference - the negative control for it passed with
     * the mapping made write-back. What can be checked is the mapping itself,
     * so it is: every page of the window must translate as uncacheable, at 4 KiB
     * granularity, to the physical address it claims. That is the same move
     * paging.c makes when it walks its own tables rather than trusting them.
     */
    for (uint64_t offset = 0U; offset < pci.ecam_size;
         offset += PAGING_PAGE_SIZE) {
        struct paging_translation window;

        if (paging_translate(pci.ecam_base + offset, &window) !=
            PAGING_STATUS_OK) {
            kernel_test_fail("a configuration window page is not mapped");
        }

        if (window.physical_address != pci.ecam_base + offset ||
            window.level != 1U ||
            window.permissions != (PAGING_WRITE | PAGING_UNCACHED)) {
            kernel_test_fail("the configuration window is not device memory");
        }
    }

    /*
     * Every function on this machine sits inside the mapped window, so every
     * one of them must have been compared. A comparison that quietly skipped
     * functions would still report agreement.
     */
    if (pci.compared_functions != pci.function_count ||
        pci.compared_dwords !=
            pci.function_count * (PCI_CONFIG_SPACE_SIZE / 4U) -
                pci.volatile_dwords) {
        kernel_test_fail("the two mechanisms were not compared everywhere");
    }

    for (size_t index = 0; index < pci.function_count; ++index) {
        const struct pci_function *function = pci_function_at(index);
        uint32_t identity = 0U;

        if (function == NULL) {
            kernel_test_fail("PCI returned no function for a live index");
        }

        if (function->header_type == PCI_HEADER_TYPE_BRIDGE) {
            ++bridges;
        }

        if (function->address.bus != 0U) {
            ++behind_a_bridge;
        }

        if (function->msi_x_offset != 0U) {
            ++message_signalled;

            /*
             * The capability the offset names must actually be there when the
             * window is asked, not only when the ports were. This is what turns
             * "a capability was recorded" into "the record points at it".
             */
            if (pci_config_read_ecam(
                    function->address,
                    function->msi_x_offset,
                    &value
                ) != PCI_STATUS_OK ||
                (uint8_t)value != PCI_CAPABILITY_MSI_X) {
                kernel_test_fail("a recorded MSI-X capability is not there");
            }
        }

        if (pci_config_read_ecam(
                function->address,
                PCI_REGISTER_VENDOR_ID,
                &identity
            ) != PCI_STATUS_OK) {
            kernel_test_fail("the window would not read a live function");
        }

        if (index == 0U) {
            first_identity = identity;
        } else if (identity != first_identity) {
            ++distinct_identities;
        }
    }

    /*
     * A window reader that ignored the device and function bits would answer
     * the same identity for every function and still agree with the ports about
     * the first one. Requiring the answers to differ is what closes that.
     */
    if (distinct_identities == 0U) {
        kernel_test_fail("the window answered one identity for every function");
    }

    if (bridges == 0U || behind_a_bridge == 0U || pci.bus_count < 2U) {
        kernel_test_fail("enumeration did not cross a bridge");
    }

    if (message_signalled == 0U) {
        kernel_test_fail("no function offered message-signalled interrupts");
    }

    /*
     * A bus past what Phipia mapped is refused rather than folded back into the
     * window, which is the failure that would read one bus as another.
     */
    address.segment = 0U;
    address.bus = (uint8_t)(pci.ecam_end_bus + 1U);
    address.device = 0U;
    address.function = 0U;

    if (pci_config_read_ecam(address, 0U, &value) !=
        PCI_STATUS_OUTSIDE_ECAM_WINDOW) {
        kernel_test_fail("the window answered about a bus it does not map");
    }

    pci_reads_repeat();

    if (pci_verify() != PCI_STATUS_OK) {
        kernel_test_fail("PCI enumeration no longer matches the machine");
    }

    console_write("ST PCI window agreed on ");
    console_write_u64(pci.compared_dwords);
    console_write(" registers of ");
    console_write_u64(pci.compared_functions);
    console_write(" functions across ");
    console_write_u64(pci.bus_count);
    console_write(" buses, ");
    console_write_u64(message_signalled);
    console_write(" with MSI-X\n");

    if (pci_shutdown() != PCI_STATUS_OK) {
        kernel_test_fail("PCI would not release its function table");
    }
}


/*
 * The guard page of the first thread this scenario creates. Slot zero belongs
 * to the boot thread, whose stack region is never mapped, so a created thread
 * is slot one and its guard is one stride above the region base. Written out
 * rather than computed so the Makefile can require this exact address in the
 * fault diagnostic - a guard that moved would otherwise still look like a pass.
 */
#define THREAD_GUARD_TEST_ADDRESS \
    (THREAD_STACK_REGION + THREAD_STACK_STRIDE)

/*
 * A supervisor write to an absent page is P=0 W=1 U=0. The heap scenario takes
 * the same error code at its own guard, and the two are told apart by CR2.
 */
#define THREAD_GUARD_TEST_ERROR_CODE UINT64_C(0x02)

_Static_assert(
    THREAD_GUARD_TEST_ADDRESS == UINT64_C(0x0000000800005000),
    "the thread guard page moved and the fault diagnostic no longer matches"
);

/* Written by every scenario worker, read by the boot thread after they exit. */
static volatile uint32_t thread_scenario_ran;
static volatile uint64_t thread_scenario_seen[THREAD_MAX];

static void scenario_worker(void *context)
{
    const uint64_t label = (uint64_t)(uintptr_t)context;

    for (unsigned int round = 0; round < 2U; ++round) {
        /*
         * Recorded from inside the thread, so this is also a check that
         * thread_current answers about the thread that is actually running
         * rather than about whoever asked last.
         */
        if (label < THREAD_MAX) {
            thread_scenario_seen[label] = thread_current();
        }

        thread_yield();
    }

    thread_scenario_ran += 1U;
}

/*
 * The parts of the thread layer normal boot does not reach: the capacity bound,
 * every refusal, and the teardown ordering. Normal boot proves three threads
 * rotate; this proves the layer says no in every way it has to.
 */
static void threads_scenario(void)
{
    struct frame_allocator_stats before;
    struct frame_allocator_stats after;
    struct thread_system_state threads;
    struct thread_state_report report;
    uint64_t identifiers[THREAD_MAX];
    uint64_t overflow = THREAD_ID_NONE;
    uint64_t boot_identifier;
    size_t created = 0U;
    enum thread_status status;

    if (!heap_is_active() || !paging_is_active()) {
        kernel_test_fail("threads ran without their lower layers");
    }

    if (thread_is_started()) {
        kernel_test_fail("threads were started before their scenario");
    }

    /*
     * The heap grows but deliberately never shrinks. With the surface already
     * occupying its first 3 MiB, the first thread table can commit one more
     * heap page that remains mapped after the table is freed. Warm that path
     * before taking the frame baseline so the comparison below measures only
     * the stack frames this scenario owns.
     */
    if (thread_start() != THREAD_STATUS_OK ||
        thread_stop() != THREAD_STATUS_OK) {
        kernel_test_fail("threads did not survive an empty start and stop");
    }

    before = frame_allocator_get_stats();
    status = thread_start();

    if (status != THREAD_STATUS_OK) {
        kernel_test_fail(thread_status_string(status));
    }

    boot_identifier = thread_current();

    if (boot_identifier == THREAD_ID_NONE) {
        kernel_test_fail("the boot thread was adopted without an identifier");
    }

    if (thread_report(boot_identifier, &report) != THREAD_STATUS_OK ||
        !report.boot_thread || report.state != THREAD_STATE_RUNNING ||
        report.stack_top != 0U) {
        kernel_test_fail("the boot thread was not adopted as itself");
    }

    /*
     * Fill every slot the table has. Slot zero is the boot thread's and is
     * never handed out, so the capacity for created threads is one less than
     * the table's - and the refusal must arrive exactly there rather than one
     * either side of it.
     */
    for (size_t index = 0; index + 1U < THREAD_MAX; ++index) {
        status = thread_create(
            scenario_worker,
            (void *)(uintptr_t)(index + 1U),
            &identifiers[index]
        );

        if (status != THREAD_STATUS_OK) {
            kernel_test_fail(thread_status_string(status));
        }

        ++created;
    }

    if (created != THREAD_MAX - 1U) {
        kernel_test_fail("the thread table did not fill to its capacity");
    }

    if (thread_create(scenario_worker, NULL, &overflow) !=
            THREAD_STATUS_NO_CAPACITY ||
        overflow != THREAD_ID_NONE) {
        kernel_test_fail("a thread was created past the table's capacity");
    }

    /* Every identifier must be distinct, or joining names the wrong thread. */
    for (size_t index = 0; index < created; ++index) {
        for (size_t other = index + 1U; other < created; ++other) {
            if (identifiers[index] == identifiers[other]) {
                kernel_test_fail("two threads share an identifier");
            }
        }

        if (identifiers[index] == boot_identifier) {
            kernel_test_fail("a thread reused the boot thread's identifier");
        }
    }

    /* Refusals, each by its own name, with threads live. */
    if (thread_join(boot_identifier) != THREAD_STATUS_BAD_IDENTIFIER) {
        kernel_test_fail("a thread was allowed to wait for itself");
    }

    if (thread_join(UINT64_C(0xABCDEF)) != THREAD_STATUS_BAD_IDENTIFIER ||
        thread_join(THREAD_ID_NONE) != THREAD_STATUS_BAD_IDENTIFIER) {
        kernel_test_fail("a join accepted an identifier naming nothing");
    }

    /*
     * Tearing down while a thread is still runnable would unmap a stack that
     * still holds a suspended frame. It is refused, and refused before any of
     * it happens rather than part way through.
     */
    if (thread_stop() != THREAD_STATUS_THREADS_STILL_RUNNABLE) {
        kernel_test_fail("threads stopped with a thread still runnable");
    }

    threads = thread_get_state();

    if (threads.ready != created || threads.stack_frames !=
        created * THREAD_STACK_PAGES) {
        kernel_test_fail("the thread table does not account for its stacks");
    }

    if (thread_verify() != THREAD_STATUS_OK) {
        kernel_test_fail("thread table does not match the address space");
    }

    for (size_t index = 0; index < created; ++index) {
        status = thread_join(identifiers[index]);

        if (status != THREAD_STATUS_OK) {
            kernel_test_fail(thread_status_string(status));
        }

        if (thread_report(identifiers[index], &report) != THREAD_STATUS_OK ||
            report.state != THREAD_STATE_EXITED) {
            kernel_test_fail("a joined thread had not exited");
        }

        /* Joining something that has already exited returns at once. */
        if (thread_join(identifiers[index]) != THREAD_STATUS_OK) {
            kernel_test_fail("joining an exited thread was refused");
        }
    }

    if (thread_scenario_ran != created) {
        kernel_test_fail("not every thread reached the end of its function");
    }

    /*
     * Each worker recorded what thread_current answered while it was running.
     * A scheduler that switched stacks but not its idea of who is current would
     * pass every other check here.
     */
    for (size_t index = 0; index < created; ++index) {
        if (thread_scenario_seen[index + 1U] != identifiers[index]) {
            kernel_test_fail("a thread did not know which thread it was");
        }
    }

    threads = thread_get_state();

    if (threads.exited != created || threads.ready != 0U ||
        threads.switches == 0U) {
        kernel_test_fail("the thread table does not account for its exits");
    }

    status = thread_stop();

    if (status != THREAD_STATUS_OK) {
        kernel_test_fail(thread_status_string(status));
    }

    if (thread_is_started() ||
        thread_stop() != THREAD_STATUS_NOT_STARTED ||
        thread_create(scenario_worker, NULL, &overflow) !=
            THREAD_STATUS_NOT_STARTED) {
        kernel_test_fail("threads accepted work after stopping");
    }

    after = frame_allocator_get_stats();

    if (after.free_frames != before.free_frames) {
        kernel_test_fail("stopping threads did not return every frame");
    }

    /*
     * The stacks are gone, so every guard and every stack page of every slot
     * must now be absent. Checked after teardown because a stop that unmapped
     * the guards but kept the stacks would leak silently.
     */
    for (size_t slot = 0; slot < THREAD_MAX; ++slot) {
        const uint64_t guard =
            THREAD_STACK_REGION + (uint64_t)slot * THREAD_STACK_STRIDE;
        struct paging_translation translation;

        for (uint64_t offset = 0U; offset < THREAD_STACK_STRIDE;
             offset += PAGING_PAGE_SIZE) {
            if (paging_translate(guard + offset, &translation) !=
                PAGING_STATUS_NOT_MAPPED) {
                kernel_test_fail("a thread stack outlived the scheduler");
            }
        }
    }

    console_write("ST THREADS created ");
    console_write_u64(created);
    console_write(" switches ");
    console_write_u64(threads.switches);
    console_write(" exited ");
    console_write_u64(threads.exited);
    console_putc('\n');
}

static void guard_worker(void *context)
{
    struct thread_state_report report;

    (void)context;

    if (thread_report(thread_current(), &report) != THREAD_STATUS_OK) {
        kernel_test_fail("a running thread cannot report itself");
    }

    if (report.guard_page != THREAD_GUARD_TEST_ADDRESS) {
        kernel_test_fail("the thread guard page is not where it should be");
    }

    if (report.stack_base != report.guard_page + PAGING_PAGE_SIZE) {
        kernel_test_fail("the guard page does not precede the stack");
    }

    /*
     * Written through the assembly probe for the same reason the paging and
     * heap scenarios use it: the scenario matches the faulting instruction
     * address exactly, and a compiler is free to move or delete an equivalent
     * C store to memory it can prove nothing reads.
     */
    paging_probe_write(
        (volatile uint8_t *)(uintptr_t)THREAD_GUARD_TEST_ADDRESS,
        UINT8_C(0x5E)
    );

    kernel_test_fail("a thread stack guard page accepted a write");
}

/*
 * Prove the guard by walking off the stack, exactly as the heap scenario proves
 * its window. The claim is that the page below a thread's stack is never
 * mapped, so a stack that runs past its own end meets a fault naming the guard
 * rather than the next thread's frame.
 *
 * What this deliberately does *not* prove is a true stack overflow, where RSP
 * itself has reached the guard. There the fault handler would need to push its
 * own frame onto a stack that has just run out, so the page fault escalates.
 * Proving that case would require a dedicated emergency fault stack.
 */
static void thread_guard_scenario(void)
{
    uint64_t identifier = THREAD_ID_NONE;
    struct paging_translation translation;
    enum thread_status status;

    status = thread_start();

    if (status != THREAD_STATUS_OK) {
        kernel_test_fail(thread_status_string(status));
    }

    status = thread_create(guard_worker, NULL, &identifier);

    if (status != THREAD_STATUS_OK) {
        kernel_test_fail(thread_status_string(status));
    }

    /*
     * The guard must be absent before the thread runs, or the fault the
     * scenario is about to take would prove nothing about the guard.
     */
    if (paging_translate(THREAD_GUARD_TEST_ADDRESS, &translation) !=
        PAGING_STATUS_NOT_MAPPED) {
        kernel_test_fail("the thread guard page is mapped");
    }

    if (paging_translate(THREAD_GUARD_TEST_ADDRESS + PAGING_PAGE_SIZE,
            &translation) != PAGING_STATUS_OK ||
        translation.permissions != PAGING_WRITE) {
        kernel_test_fail("the thread stack above the guard is not writable");
    }

    console_write("ST THREAD guard ");
    console_write_hex(THREAD_GUARD_TEST_ADDRESS);
    console_putc('\n');

    /* Hands the processor to the worker, which does not come back. */
    thread_yield();
    kernel_test_fail("the guard thread returned from its fault");
}


/*
 * Sixteen coordinates chosen to hit every edge and a few interior points,
 * rather than a sweep. Normal boot already reads every pixel back through the
 * framebuffer's own addressing; what this scenario adds is a second, completely
 * independent addressing to compare it against, and that only needs to
 * disagree once.
 */
#define FRAMEBUFFER_TEST_PROBES 16U

/*
 * The same picture addressed two ways.
 *
 * Normal boot proves that what framebuffer_write_pixel writes,
 * framebuffer_read_pixel reads - which is true even if both agree on the wrong
 * address. This computes the physical address of a coordinate from the loader's
 * own numbers, reads it through a raw volatile pointer that shares no code with
 * framebuffer.c, and requires the two to agree. It is the same argument the two
 * PCI configuration mechanisms make, one layer up.
 */
/*
 * The screen console, checked where boot cannot check it.
 *
 * prove_screen_console in src/kernel/boot_proofs.c verifies that what was drawn
 * is on the glass. What it cannot do is take the console apart: boot needs the
 * console it is printing through, so it can never leave it in a broken state to
 * see what happens. This scenario can, because nothing after it needs a screen.
 *
 * Every character in the font is drawn and read back here, not a sample. The
 * boot proof draws nine letters; a glyph table with one bad row in the middle
 * of it would pass that and fail this.
 */
/*
 * The keyboard, taken apart where boot cannot.
 *
 * prove_keyboard injects five scancodes and checks three characters come out.
 * What it cannot do is fill the queue, because boot needs the keyboard working
 * afterwards, and it cannot toggle caps lock, because that would leave the
 * machine in a state the rest of boot did not ask for. Nothing after this
 * scenario needs a keyboard, so this can do both.
 */
/*
 * The shell, taken apart where boot cannot.
 *
 * prove_shell types one command and reads the answer off the glass. What it
 * cannot do is run every command, because several of them clear the screen or
 * print pages, and boot has to keep its transcript. Nothing after this scenario
 * needs a console, so this can run all of them.
 */
static void shell_scenario(void)
{
    static const char *const commands[] = {
        "help", "echo hello", "uptime", "mem", "pci", "keys", "threads",
        "ledger", "version", "clear"
    };

    struct shell_state before;
    struct shell_state after;

    if (!shell_is_active()) {
        kernel_test_fail("the shell scenario has no shell");
    }

    before = shell_get_state();

    /*
     * Every command this shell has, run for real. A command that faults or
     * hangs takes the scenario with it, which is the point: these read live
     * kernel state and any of them could be pointing at something that has
     * since moved.
     */
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]);
         ++index) {
        if (shell_execute(commands[index]) != SHELL_STATUS_OK) {
            kernel_test_fail("a built-in command refused to run");
        }
    }

    after = shell_get_state();

    if (after.commands - before.commands !=
        sizeof(commands) / sizeof(commands[0])) {
        kernel_test_fail("the shell did not count every command it ran");
    }

    if (after.unknown != before.unknown) {
        kernel_test_fail("the shell did not recognise one of its own commands");
    }

    /*
     * A prefix must not run a longer command, and a longer word must not run a
     * shorter one. Both directions, because a dispatcher that compares only as
     * far as its own name gets one of them wrong.
     */
    if (shell_execute("hel") != SHELL_STATUS_UNKNOWN_COMMAND) {
        kernel_test_fail("a prefix of a command ran that command");
    }

    if (shell_execute("helpful") != SHELL_STATUS_UNKNOWN_COMMAND) {
        kernel_test_fail("a longer word ran a shorter command");
    }

    if (shell_execute("echoes") != SHELL_STATUS_UNKNOWN_COMMAND) {
        kernel_test_fail("a longer word ran echo");
    }

    /*
     * The line editor, driven the way a person drives it. Typed, corrected with
     * backspace, and submitted - and the correction has to have taken, which
     * only the command that runs can show.
     */
    before = shell_get_state();

    {
        static const char typed[] = "echa\bo corrected";

        for (size_t index = 0; typed[index] != '\0'; ++index) {
            if (shell_feed(typed[index]) != SHELL_STATUS_OK) {
                kernel_test_fail("the shell refused a character while typing");
            }
        }

        if (shell_feed('\n') != SHELL_STATUS_OK) {
            kernel_test_fail("the corrected line did not run");
        }
    }

    after = shell_get_state();

    if (after.unknown != before.unknown) {
        kernel_test_fail("backspace did not correct the command");
    }

    if (after.length != 0U) {
        kernel_test_fail("the shell kept the line after running it");
    }

    /*
     * An erase has to reach the glass, not only the buffer.
     *
     * This exists because a control found it missing. Deleting the space and
     * the second backspace from the shell's erase sequence - so the cursor
     * moves but the character stays on screen - passed every check here, and
     * chasing that found a real bug underneath: the screen console did not
     * handle backspace at all, so a correction drew the font's replacement
     * character instead of stepping back.
     */
    if (screen_is_active()) {
        if (screen_clear() != SCREEN_STATUS_OK) {
            kernel_test_fail("the shell scenario could not clear the screen");
        }

        if (shell_feed('a') != SHELL_STATUS_OK ||
            shell_feed('b') != SHELL_STATUS_OK) {
            kernel_test_fail("the shell refused a character before an erase");
        }

        if (screen_verify_cell(1U, 0U, 'b') != SCREEN_STATUS_OK) {
            kernel_test_fail("a typed character did not reach the screen");
        }

        if (shell_feed('\b') != SHELL_STATUS_OK) {
            kernel_test_fail("the shell refused a backspace");
        }

        /* The erased cell is blank, and the one before it is untouched. */
        if (screen_verify_cell(1U, 0U, ' ') != SCREEN_STATUS_OK) {
            kernel_test_fail("backspace did not erase the character on screen");
        }

        if (screen_verify_cell(0U, 0U, 'a') != SCREEN_STATUS_OK) {
            kernel_test_fail("backspace erased more than one character");
        }

        /* And the next character lands where the erased one was. */
        if (shell_feed('c') != SHELL_STATUS_OK) {
            kernel_test_fail("the shell refused a character after an erase");
        }

        if (screen_verify_cell(1U, 0U, 'c') != SCREEN_STATUS_OK) {
            kernel_test_fail("the cursor did not return to the erased cell");
        }

        while (shell_get_state().length > 0U) {
            if (shell_feed('\b') != SHELL_STATUS_OK) {
                kernel_test_fail("the shell would not clear its line");
            }
        }
    }

    /* And an unknown command is reported without stopping anything. */
    before = shell_get_state();

    if (shell_execute("definitelynotacommand") != SHELL_STATUS_UNKNOWN_COMMAND) {
        kernel_test_fail("an unknown command was not reported");
    }

    if (shell_get_state().unknown != before.unknown + 1U) {
        kernel_test_fail("an unknown command was not counted");
    }

    if (shell_execute("help") != SHELL_STATUS_OK) {
        kernel_test_fail("the shell stopped working after an unknown command");
    }

    after = shell_get_state();
    console_write("Phipia: shell scenario ran ");
    console_write_u64(after.commands);
    console_write(" commands and refused ");
    console_write_u64(after.unknown);
    console_write(" unknown\n");
}

static void keyboard_scenario(void)
{
    struct keyboard_state before;
    struct keyboard_state after;
    struct keyboard_event event;
    size_t drained = 0U;

    if (!keyboard_is_initialized()) {
        kernel_test_fail("the keyboard scenario has no keyboard");
    }

    /* Bringing it up twice is refused. Boot only ever does it once. */
    if (keyboard_initialize() != KEYBOARD_STATUS_ALREADY_INITIALIZED) {
        kernel_test_fail("the keyboard was brought up twice");
    }

    /* Drain whatever boot's proof left, so the counts below start clean. */
    while (keyboard_read(&event) == KEYBOARD_STATUS_OK) {
        drained += 1U;

        if (drained > 4096U) {
            kernel_test_fail("the keyboard queue would not drain");
        }
    }

    if (keyboard_read(&event) != KEYBOARD_STATUS_EMPTY) {
        kernel_test_fail("an empty keyboard queue did not say so");
    }

    if (keyboard_read(NULL) == KEYBOARD_STATUS_OK) {
        kernel_test_fail("the keyboard wrote an event through a null pointer");
    }

    /*
     * Caps lock, which boot deliberately does not touch. It is a toggle on
     * press only, it applies to letters and not to digits, and a second press
     * undoes it.
     */
    before = keyboard_get_state();

    if (before.caps_lock) {
        kernel_test_fail("caps lock was already latched");
    }

    cpu_interrupt_enable();

    if (keyboard_inject_scancode(0x3AU) != KEYBOARD_STATUS_OK ||
        keyboard_inject_scancode(0x1EU) != KEYBOARD_STATUS_OK) {
        cpu_interrupt_disable();
        kernel_test_fail("the controller refused an injected scancode");
    }

    for (uint64_t spins = 0; spins < UINT64_C(200000000); ++spins) {
        if (keyboard_get_state().events >= before.events + 2U) {
            break;
        }
    }

    cpu_interrupt_disable();

    if (!keyboard_get_state().caps_lock) {
        kernel_test_fail("caps lock did not latch on press");
    }

    /* The 'a' after it must have arrived capitalised. */
    while (keyboard_read(&event) == KEYBOARD_STATUS_OK) {
        if (event.character == '\0') {
            continue;
        }

        if (event.character != 'A') {
            kernel_test_fail("caps lock did not capitalise the next letter");
        }
    }

    if (keyboard_character_for(0x02U, false, true) != '1') {
        kernel_test_fail("caps lock changed a digit");
    }

    cpu_interrupt_enable();

    if (keyboard_inject_scancode(0x3AU) != KEYBOARD_STATUS_OK) {
        cpu_interrupt_disable();
        kernel_test_fail("the controller refused the second caps lock");
    }

    for (uint64_t spins = 0; spins < UINT64_C(200000000); ++spins) {
        if (!keyboard_get_state().caps_lock) {
            break;
        }
    }

    cpu_interrupt_disable();

    if (keyboard_get_state().caps_lock) {
        kernel_test_fail("caps lock did not release on a second press");
    }

    while (keyboard_read(&event) == KEYBOARD_STATUS_OK) {
        /* discard */
    }

    /*
     * Overflow. The queue holds sixty-four minus one; more than that must be
     * counted as dropped rather than silently overwriting what is waiting.
     * Interrupts stay off so nothing is consumed while it fills.
     */
    before = keyboard_get_state();

    if (before.dropped != 0U) {
        kernel_test_fail("the keyboard had already dropped an event");
    }

    /*
     * Interrupts stay on for the whole flood. An earlier version toggled them
     * around each injection, which delivered nothing at all: sti does not take
     * effect until after the instruction following it, so sti immediately
     * followed by cli leaves a window of exactly zero instructions. The
     * controller holds one byte, so each injection has to be taken by the
     * handler before the next will land, and the bounded wait inside
     * keyboard_inject_scancode is what gives it the chance.
     */
    cpu_interrupt_enable();

    for (uint32_t index = 0; index < 200U; ++index) {
        if (keyboard_inject_scancode(0x1EU) != KEYBOARD_STATUS_OK) {
            cpu_interrupt_disable();
            kernel_test_fail("the controller refused a flood of scancodes");
        }
    }

    for (uint64_t spins = 0; spins < UINT64_C(200000000); ++spins) {
        const struct keyboard_state now = keyboard_get_state();

        if (now.events + now.dropped >= before.events + 200U) {
            break;
        }
    }

    cpu_interrupt_disable();
    after = keyboard_get_state();

    if (after.queued >= KEYBOARD_QUEUE_SIZE) {
        kernel_test_fail("the keyboard queue grew past its bound");
    }

    if (after.dropped == 0U) {
        kernel_test_fail("a flooded keyboard queue dropped nothing");
    }

    if (after.events + after.dropped < before.events + 200U) {
        kernel_test_fail("the keyboard lost events it never accounted for");
    }

    console_write("Phipia: keyboard scenario queued ");
    console_write_u64((uint64_t)after.queued);
    console_write(" and dropped ");
    console_write_u64(after.dropped - before.dropped);
    console_write(" of a 200 event flood\n");
}

static void screen_scenario(void)
{
    struct screen_state before;
    struct screen_state after;
    uint32_t width = 0U;
    uint32_t height = 0U;
    uint32_t first = 0U;
    uint32_t count = 0U;

    if (!screen_is_active()) {
        kernel_test_fail("the screen scenario has no console");
    }

    if (phipia_font_geometry(&width, &height, &first, &count) !=
        FONT_STATUS_OK) {
        kernel_test_fail("the font table would not describe itself");
    }

    before = screen_get_state();

    if (before.cell_width != width || before.cell_height != height) {
        kernel_test_fail("the console and the font disagree about the cell");
    }

    /*
     * Bringing the console up twice must be refused. Boot cannot test this,
     * because boot only ever does it once.
     */
    if (screen_initialize() != SCREEN_STATUS_ALREADY_INITIALIZED) {
        kernel_test_fail("the console adopted the screen twice");
    }

    /*
     * Every glyph the table covers, drawn and read back. A cell is checked
     * immediately after it is written so a later character cannot repair an
     * earlier one by overlapping it.
     */
    if (screen_clear() != SCREEN_STATUS_OK) {
        kernel_test_fail("the console would not clear");
    }

    for (uint32_t code = first; code < first + count; ++code) {
        const char character = (char)(unsigned char)code;
        const struct screen_state cursor = screen_get_state();

        if (screen_putc(character) != SCREEN_STATUS_OK) {
            kernel_test_fail("the console refused a character its font covers");
        }

        if (screen_verify_cell(cursor.column, cursor.row, character) !=
            SCREEN_STATUS_OK) {
            kernel_test_fail("a glyph did not reach the screen intact");
        }
    }

    after = screen_get_state();

    if (after.characters - before.characters != (uint64_t)count) {
        kernel_test_fail("the console lost a character it said it drew");
    }

    /*
     * A cell outside the grid is refused rather than clamped, and the refusal
     * is the console's own rather than the framebuffer's bounds check catching
     * it afterwards.
     */
    if (screen_verify_cell(after.columns, 0U, 'x') != SCREEN_STATUS_NO_ROOM) {
        kernel_test_fail("the console read a cell past its last column");
    }

    if (screen_verify_cell(0U, after.rows, 'x') != SCREEN_STATUS_NO_ROOM) {
        kernel_test_fail("the console read a cell past its last row");
    }

    /*
     * A scroll that actually moves rows, checked by content.
     *
     * This exists because a control found it missing. Scrolling by more than
     * the screen and scrolling by zero both take early exits - one is a fill,
     * one is a no-op - so neither reaches the copy loop, and reversing that
     * loop's direction left every check above still passing. A copy whose
     * destination is above its source must walk forwards or it reads rows it
     * has already overwritten, and only content one cell tall can tell.
     */
    if (screen_clear() != SCREEN_STATUS_OK) {
        kernel_test_fail("the console would not clear before the scroll check");
    }

    if (screen_write("top\nsecond") != SCREEN_STATUS_OK) {
        kernel_test_fail("the console refused the scroll fixture");
    }

    while (screen_get_state().row + 1U < before.rows) {
        if (screen_putc('\n') != SCREEN_STATUS_OK) {
            kernel_test_fail("the console refused to reach its last row");
        }
    }

    if (screen_putc('\n') != SCREEN_STATUS_OK) {
        kernel_test_fail("the console refused to scroll one line");
    }

    /* The second line must now be the first. */
    for (uint32_t column = 0U; column < 6U; ++column) {
        if (screen_verify_cell(column, 0U, "second"[column]) !=
            SCREEN_STATUS_OK) {
            kernel_test_fail("a scroll did not move the rows it copied");
        }
    }

    console_write("Phipia: screen scenario drew ");
    console_write_u64((uint64_t)count);
    console_write(" glyphs and read every one back\n");
}

static uint32_t surface_test_colour(uint32_t value)
{
    return framebuffer_pack(
        (uint8_t)(value * 3U + 1U),
        (uint8_t)(value * 5U + 2U),
        (uint8_t)(value * 7U + 3U)
    );
}

static void require_framebuffer_pixel(
    uint32_t x,
    uint32_t y,
    uint32_t expected,
    const char *reason
)
{
    uint32_t pixel = 0U;
    const uint32_t mask = framebuffer_visible_mask();

    if (framebuffer_read_pixel(x, y, &pixel) != FRAMEBUFFER_STATUS_OK ||
        (pixel & mask) != (expected & mask)) {
        kernel_test_fail(reason);
    }
}

/*
 * The self-test proves the primitives over guarded synthetic rows. This proves
 * the other half: heap allocation, damage presented to device memory, and the
 * framebuffer pitch all agree on where the same pixels live.
 */
static void surface_scenario(void)
{
    uint32_t source[4U * 5U];
    const struct framebuffer_state framebuffer = framebuffer_get_state();
    const uint32_t base = surface_test_colour(1U);
    const uint32_t changed = surface_test_colour(2U);
    const uint32_t clipped_colour = surface_test_colour(3U);
    struct surface surface = { 0 };
    struct surface_rect rectangle;
    uint32_t origin_x;
    uint32_t origin_y;

    if (!framebuffer_is_active()) {
        kernel_test_fail("the surface scenario has no framebuffer");
    }

    if (framebuffer.width < 16U || framebuffer.height < 32U) {
        kernel_test_fail("the framebuffer is too small for surface fixtures");
    }

    if (surface_initialize(&surface, framebuffer.width, framebuffer.height) !=
        SURFACE_STATUS_OK) {
        kernel_test_fail("the surface scenario could not allocate its buffer");
    }

    rectangle.x = 0U;
    rectangle.y = 0U;
    rectangle.width = surface.width;
    rectangle.height = surface.height;

    if (surface_fill_rect(&surface, rectangle, base) != SURFACE_STATUS_OK ||
        surface_present(&surface) != SURFACE_STATUS_OK ||
        surface.last_present_pixels !=
            (uint64_t)surface.width * surface.height ||
        surface.damage.pending) {
        kernel_test_fail("a full surface present copied the wrong damage");
    }

    require_framebuffer_pixel(surface.width - 1U, surface.height - 1U, base,
        "a full surface present missed its last pixel");

    if (surface_pixel(&surface, surface.width - 2U, surface.height - 2U,
            changed) != SURFACE_STATUS_OK ||
        surface_present(&surface) != SURFACE_STATUS_OK ||
        surface.last_present_pixels != 1U) {
        kernel_test_fail("one damaged surface pixel copied more than itself");
    }

    require_framebuffer_pixel(surface.width - 2U, surface.height - 2U,
        changed, "a damaged surface pixel was presented on the wrong row");

    rectangle.x = 0U;
    rectangle.y = surface.height / 2U;
    rectangle.width = surface.width;
    rectangle.height = 16U;

    if (surface_fill_rect(&surface, rectangle, changed) != SURFACE_STATUS_OK ||
        surface_present(&surface) != SURFACE_STATUS_OK ||
        surface.last_present_pixels != (uint64_t)surface.width * 16U) {
        kernel_test_fail("one text line copied more than one line");
    }

    rectangle.x = surface.width - 2U;
    rectangle.y = surface.height - 2U;
    rectangle.width = 4U;
    rectangle.height = 4U;

    if (surface_fill_rect(&surface, rectangle, clipped_colour) !=
            SURFACE_STATUS_OK ||
        surface_present(&surface) != SURFACE_STATUS_OK ||
        surface.last_present_pixels != 4U) {
        kernel_test_fail("a clipped fill crossed the surface edge");
    }

    require_framebuffer_pixel(surface.width - 1U, surface.height - 1U,
        clipped_colour, "a clipped fill missed its visible corner");

    for (uint32_t y = 0U; y < 4U; ++y) {
        for (uint32_t x = 0U; x < 4U; ++x) {
            source[y * 5U + x] = surface_test_colour(y * 16U + x);
        }

        source[y * 5U + 4U] = UINT32_C(0xDEADBEEF);
    }

    origin_x = 8U;
    origin_y = 8U;

    if (surface_blit(&surface, origin_x, origin_y, source, 4U, 4U,
            5U * SURFACE_BYTES_PER_PIXEL) != SURFACE_STATUS_OK ||
        surface_present(&surface) != SURFACE_STATUS_OK ||
        surface.last_present_pixels != 16U) {
        kernel_test_fail("a padded source did not blit as four rows");
    }

    for (uint32_t y = 0U; y < 4U; ++y) {
        for (uint32_t x = 0U; x < 4U; ++x) {
            require_framebuffer_pixel(origin_x + x, origin_y + y,
                surface_test_colour(y * 16U + x),
                "surface blit used the destination pitch for its source");
        }
    }

    rectangle.x = origin_x;
    rectangle.y = origin_y;
    rectangle.width = 4U;
    rectangle.height = 4U;

    if (surface_copy_rect(&surface, rectangle, origin_x + 1U,
            origin_y + 1U) != SURFACE_STATUS_OK ||
        surface_present(&surface) != SURFACE_STATUS_OK) {
        kernel_test_fail("a downward overlapping copy was refused");
    }

    for (uint32_t y = 0U; y < 4U; ++y) {
        for (uint32_t x = 0U; x < 4U; ++x) {
            require_framebuffer_pixel(origin_x + x + 1U,
                origin_y + y + 1U, surface_test_colour(y * 16U + x),
                "a downward overlapping copy read overwritten pixels");
        }
    }

    if (surface_blit(&surface, origin_x, origin_y, source, 4U, 4U,
            5U * SURFACE_BYTES_PER_PIXEL) != SURFACE_STATUS_OK ||
        surface_present(&surface) != SURFACE_STATUS_OK) {
        kernel_test_fail("the overlap fixture could not be restored");
    }

    rectangle.x = origin_x + 1U;
    rectangle.y = origin_y + 1U;
    rectangle.width = 3U;
    rectangle.height = 3U;

    if (surface_copy_rect(&surface, rectangle, origin_x, origin_y) !=
            SURFACE_STATUS_OK ||
        surface_present(&surface) != SURFACE_STATUS_OK) {
        kernel_test_fail("an upward overlapping copy was refused");
    }

    for (uint32_t y = 0U; y < 3U; ++y) {
        for (uint32_t x = 0U; x < 3U; ++x) {
            require_framebuffer_pixel(origin_x + x, origin_y + y,
                surface_test_colour((y + 1U) * 16U + x + 1U),
                "an upward overlapping copy read overwritten pixels");
        }
    }

    if (surface_pixel(&surface, 1U, 2U, changed) != SURFACE_STATUS_OK ||
        surface_pixel(&surface, 4U, 6U, changed) != SURFACE_STATUS_OK ||
        !surface.damage.pending || surface.damage.rectangle.x != 1U ||
        surface.damage.rectangle.y != 2U ||
        surface.damage.rectangle.width != 4U ||
        surface.damage.rectangle.height != 5U ||
        surface_present(&surface) != SURFACE_STATUS_OK ||
        surface.last_present_pixels != 20U) {
        kernel_test_fail("surface damage did not form one bounding rectangle");
    }

    if (surface_release(&surface) != SURFACE_STATUS_OK) {
        kernel_test_fail("the surface scenario leaked its buffer");
    }

    console_write("ST SURFACE full ");
    console_write_u64((uint64_t)framebuffer.width * framebuffer.height);
    console_write(" line ");
    console_write_u64((uint64_t)framebuffer.width * 16U);
    console_write(" clipped 4 overlap both damage 20\n");
}

static void write_combining_scenario(
    const struct boot_framebuffer *framebuffer,
    const struct paging_device_windows *device_windows
)
{
    const struct paging_state paging = paging_get_state();
    const struct paging_device_window *framebuffer_window =
        find_device_window(device_windows, PAGING_DEVICE_WINDOW_FRAMEBUFFER);
    const struct paging_device_window *ecam =
        find_device_window(device_windows, PAGING_DEVICE_WINDOW_PCI_ECAM);
    struct paging_translation translation;
    struct surface cached = {0};

    if (framebuffer == NULL || !framebuffer->present ||
        framebuffer_window == NULL) {
        kernel_test_fail("the write-combining scenario has no framebuffer");
    }

    if (paging_verify() != PAGING_STATUS_OK ||
        paging.write_combining_pat_entry != 1U ||
        ((paging.pat_after >> 8U) & UINT64_C(0xFF)) != 1U) {
        kernel_test_fail("IA32_PAT does not select write-combining entry 1");
    }

    for (unsigned int index = 0U; index < 8U; ++index) {
        if (index != paging.write_combining_pat_entry &&
            ((paging.pat_before >> (index * 8U)) & UINT64_C(0xFF)) !=
                ((paging.pat_after >> (index * 8U)) & UINT64_C(0xFF))) {
            kernel_test_fail("IA32_PAT changed an entry it did not own");
        }
    }

    for (uint64_t offset = 0U; offset < framebuffer_window->length;
         offset += PAGING_PAGE_SIZE) {
        const uint64_t address = framebuffer_window->physical_base + offset;

        if (paging_translate(address, &translation) != PAGING_STATUS_OK ||
            translation.physical_address != address ||
            translation.level != 1U ||
            translation.permissions !=
                (PAGING_WRITE | PAGING_WRITE_COMBINING) ||
            translation.memory_type != PAGING_MEMORY_WRITE_COMBINING) {
            kernel_test_fail("a framebuffer page is not write-combining");
        }
    }

    for (uint64_t offset = 0U; ecam != NULL && offset < ecam->length;
         offset += PAGING_PAGE_SIZE) {
        const uint64_t address = ecam->physical_base + offset;

        if (paging_translate(address, &translation) != PAGING_STATUS_OK ||
            translation.permissions != (PAGING_WRITE | PAGING_UNCACHED) ||
            translation.memory_type != PAGING_MEMORY_UNCACHEABLE) {
            kernel_test_fail("a PCI ECAM page is not uncacheable");
        }
    }

    if (paging_translate((uint64_t)(uintptr_t)&active_scenario, &translation) !=
            PAGING_STATUS_OK ||
        translation.permissions != PAGING_WRITE ||
        translation.memory_type != PAGING_MEMORY_WRITE_BACK) {
        kernel_test_fail("ordinary kernel RAM is not write-back");
    }

    if (paging_map(PAGING_PROBE_ADDRESS, 0U, PAGING_PAGE_SIZE,
            PAGING_UNCACHED | PAGING_WRITE_COMBINING) !=
        PAGING_STATUS_CONFLICTING_MEMORY_TYPES) {
        kernel_test_fail("paging accepted incompatible memory types");
    }

    if (surface_initialize(&cached, framebuffer->width, framebuffer->height) !=
            SURFACE_STATUS_OK ||
        surface_verify(&cached) != SURFACE_STATUS_OK) {
        kernel_test_fail("the cached surface is not write-back");
    }

    if (surface_release(&cached) != SURFACE_STATUS_OK ||
        framebuffer_verify() != FRAMEBUFFER_STATUS_OK) {
        kernel_test_fail("write-combining state did not survive verification");
    }

    console_write("ST WRITE-COMBINING PAT ");
    console_write_hex(paging.pat_after);
    console_write(" ENTRY ");
    console_write_u64(paging.write_combining_pat_entry);
    console_write(" FRAMEBUFFER ");
    console_write_u64(framebuffer_window->length / PAGING_PAGE_SIZE);
    console_write(" PAGES\n");
}

/*
 * Fill every address-space slot, verify isolation and overflow refusal, then
 * check that narrowed aliases can only be released newest-first.
 */
static void multiprocess_slots_scenario(void)
{
    struct paging_process_space spaces[MULTIPROCESS_MAX_PROCESSES];
    struct paging_process_image_alias aliases[MULTIPROCESS_MAX_PROCESSES];
    uintptr_t image_frames[MULTIPROCESS_MAX_PROCESSES];
    uintptr_t stack_frames[MULTIPROCESS_MAX_PROCESSES]
        [PAGING_PROCESS_STACK_PAGES];
    struct paging_process_space overflow;
    struct paging_translation translation;
    const struct frame_allocator_stats before = frame_allocator_get_stats();
    const struct paging_state paging_before = paging_get_state();
    const bool restore_interrupts = cpu_interrupts_enabled();
    struct frame_allocator_stats after;

    if (!multiprocess_resources_released() ||
        !paging_process_resources_released()) {
        kernel_test_fail("multiprocess slots began with resources held");
    }
    cpu_interrupt_disable();
    for (size_t index = 0U; index < MULTIPROCESS_MAX_PROCESSES; ++index) {
        image_frames[index] = 0U;
        if (frame_allocate(&image_frames[index]) != FRAME_STATUS_OK) {
            kernel_test_fail("multiprocess slot image frame allocation failed");
        }
        for (size_t page = 0U; page < PAGING_PROCESS_STACK_PAGES; ++page) {
            stack_frames[index][page] = 0U;
            if (frame_allocate(&stack_frames[index][page]) !=
                    FRAME_STATUS_OK) {
                kernel_test_fail(
                    "multiprocess slot stack frame allocation failed");
            }
            for (size_t offset = 0U; offset < PAGING_PAGE_SIZE; ++offset) {
                ((volatile uint8_t *)(void *)stack_frames[index][page])
                    [offset] = 0U;
            }
        }
        for (size_t offset = 0U; offset < PAGING_PAGE_SIZE; ++offset) {
            ((volatile uint8_t *)(void *)image_frames[index])[offset] = 0U;
        }
        if (paging_process_space_build(&spaces[index]) != PAGING_STATUS_OK ||
            paging_process_image_alias_narrow(&spaces[index],
                image_frames[index], &aliases[index]) != PAGING_STATUS_OK ||
            paging_process_map_user_page(&spaces[index],
                PAGING_PROCESS_MAPPING_IMAGE, PAGING_PROCESS_IMAGE_ADDRESS,
                image_frames[index], PAGING_EXECUTE) != PAGING_STATUS_OK) {
            kernel_test_fail("a concurrent private address space was refused");
        }
        for (size_t page = 0U; page < PAGING_PROCESS_STACK_PAGES; ++page) {
            if (paging_process_map_user_page(&spaces[index],
                    PAGING_PROCESS_MAPPING_STACK,
                    PAGING_PROCESS_STACK_BASE +
                        (uint64_t)page * PAGING_PAGE_SIZE,
                    stack_frames[index][page], PAGING_WRITE) !=
                    PAGING_STATUS_OK) {
                kernel_test_fail("a concurrent private stack was refused");
            }
        }
        if (paging_process_validate(&spaces[index], image_frames[index],
                stack_frames[index]) != PAGING_STATUS_OK) {
            kernel_test_fail("a concurrent address space failed its walk");
        }
    }

    for (size_t index = 0U; index < MULTIPROCESS_MAX_PROCESSES; ++index) {
        for (size_t other = 0U; other < index; ++other) {
            if (spaces[index].root_physical_address ==
                    spaces[other].root_physical_address ||
                spaces[index].generation == spaces[other].generation ||
                image_frames[index] == image_frames[other]) {
                kernel_test_fail("two concurrent address spaces are the same");
            }
        }
        if (paging_process_translate(&spaces[index],
                PAGING_PROCESS_IMAGE_ADDRESS, &translation) !=
                PAGING_STATUS_OK ||
            !translation.user ||
            translation.permissions != PAGING_EXECUTE ||
            translation.physical_address != image_frames[index]) {
            kernel_test_fail("a private image mapping is not its own");
        }
    }
    if (paging_process_space_build(&overflow) != PAGING_STATUS_PROCESS_BUSY ||
        overflow.state != PAGING_PROCESS_SPACE_INVALID) {
        kernel_test_fail("the address-space slot bound was not enforced");
    }

    for (size_t index = 0U; index < MULTIPROCESS_MAX_PROCESSES; ++index) {
        for (size_t page = PAGING_PROCESS_STACK_PAGES; page > 0U; --page) {
            if (paging_process_unmap_user_page(&spaces[index],
                    PAGING_PROCESS_MAPPING_STACK,
                    PAGING_PROCESS_STACK_BASE +
                        (uint64_t)(page - 1U) * PAGING_PAGE_SIZE) !=
                    PAGING_STATUS_OK) {
                kernel_test_fail("a private stack mapping refused removal");
            }
        }
        if (paging_process_unmap_user_page(&spaces[index],
                PAGING_PROCESS_MAPPING_IMAGE, PAGING_PROCESS_IMAGE_ADDRESS) !=
                PAGING_STATUS_OK) {
            kernel_test_fail("a private image mapping refused removal");
        }
    }
    if (paging_process_image_alias_restore(&spaces[0], &aliases[0]) !=
            PAGING_STATUS_PROCESS_ALIAS_STATE) {
        kernel_test_fail("an out-of-order alias restore was accepted");
    }
    for (size_t count = MULTIPROCESS_MAX_PROCESSES; count > 0U; --count) {
        const size_t index = count - 1U;

        if (paging_process_image_alias_restore(&spaces[index],
                &aliases[index]) != PAGING_STATUS_OK ||
            paging_process_space_release(&spaces[index]) !=
                PAGING_STATUS_OK) {
            kernel_test_fail("a concurrent address space refused teardown");
        }
        for (size_t page = PAGING_PROCESS_STACK_PAGES; page > 0U; --page) {
            if (frame_release(stack_frames[index][page - 1U]) !=
                    FRAME_STATUS_OK) {
                kernel_test_fail("a private stack frame refused release");
            }
        }
        if (frame_release(image_frames[index]) != FRAME_STATUS_OK) {
            kernel_test_fail("a private image frame refused release");
        }
    }
    if (restore_interrupts) {
        cpu_interrupt_enable();
    }

    after = frame_allocator_get_stats();
    if (after.free_frames != before.free_frames ||
        after.allocated_frames != before.allocated_frames ||
        paging_get_state().table_frames != paging_before.table_frames ||
        paging_verify() != PAGING_STATUS_OK ||
        !paging_process_resources_released() ||
        !multiprocess_resources_released()) {
        kernel_test_fail("concurrent address spaces leaked on teardown");
    }
    console_write("ST MULTIPROCESS-SLOTS concurrent address spaces ");
    console_write_u64(MULTIPROCESS_MAX_PROCESSES);
    console_write(" bound enforced alias order enforced teardown clean\n");
}

static void device_windows_scenario(
    const struct paging_device_windows *expected
)
{
    const struct paging_device_windows *installed =
        paging_get_device_windows();
    struct paging_translation translation;
    struct paging_audit audit;
    size_t failed_window = PAGING_DEVICE_WINDOW_NONE;
    size_t page_count = 0U;
    size_t io_apic_count = 0U;
    bool found_vga = false;
    bool found_local_apic = false;
    bool found_ecam = false;
    bool found_framebuffer = false;

    if (paging_verify_device_windows(expected, &failed_window) !=
            PAGING_STATUS_OK ||
        paging_audit_hierarchy(&audit) != PAGING_STATUS_OK ||
        audit.write_execute_leaves != 0U || audit.user_leaves != 0U) {
        kernel_test_fail("the installed device-window registry is invalid");
    }

    for (size_t index = 0U; index < installed->count; ++index) {
        const struct paging_device_window *window = &installed->entries[index];
        enum paging_memory_type required_type = PAGING_MEMORY_UNCACHEABLE;

        switch (window->kind) {
        case PAGING_DEVICE_WINDOW_VGA_TEXT:
            found_vga = true;
            break;
        case PAGING_DEVICE_WINDOW_LOCAL_APIC:
            found_local_apic = true;
            break;
        case PAGING_DEVICE_WINDOW_IO_APIC:
            ++io_apic_count;
            break;
        case PAGING_DEVICE_WINDOW_PCI_ECAM:
            found_ecam = true;
            break;
        case PAGING_DEVICE_WINDOW_FRAMEBUFFER:
            found_framebuffer = true;
            required_type = PAGING_MEMORY_WRITE_COMBINING;
            break;
        case PAGING_DEVICE_WINDOW_KIND_COUNT:
        default:
            kernel_test_fail("the installed registry has an unknown kind");
        }

        if (window->memory_type != required_type ||
            window->permissions != PAGING_DEVICE_WINDOW_WRITE) {
            kernel_test_fail("a device window has the wrong policy");
        }

        for (uint64_t offset = 0U; offset < window->length;
             offset += PAGING_PAGE_SIZE) {
            const uint64_t address = window->physical_base + offset;
            const uint32_t permissions = PAGING_WRITE |
                (required_type == PAGING_MEMORY_WRITE_COMBINING
                    ? PAGING_WRITE_COMBINING
                    : PAGING_UNCACHED);

            if (paging_translate(address, &translation) != PAGING_STATUS_OK ||
                translation.physical_address != address ||
                translation.permissions != permissions ||
                translation.memory_type != required_type ||
                translation.level != 1U) {
                kernel_test_fail("a complete device window did not translate");
            }

            ++page_count;
        }
    }

    if (!found_vga || !found_local_apic || io_apic_count == 0U) {
        kernel_test_fail("the installed registry lacks a required window");
    }

    if (paging_translate((uint64_t)(uintptr_t)&active_scenario, &translation) !=
            PAGING_STATUS_OK ||
        translation.permissions != PAGING_WRITE ||
        translation.memory_type != PAGING_MEMORY_WRITE_BACK) {
        kernel_test_fail("ordinary RAM is not write-back");
    }

    console_write("ST DEVICE-WINDOWS WINDOWS ");
    console_write_u64(installed->count);
    console_write(" PAGES ");
    console_write_u64(page_count);
    console_write(" VGA 1 LOCAL-APIC 1 IO-APICS ");
    console_write_u64(io_apic_count);
    console_write(" ECAM ");
    console_write_u64(found_ecam ? 1U : 0U);
    console_write(" FRAMEBUFFER ");
    console_write_u64(found_framebuffer ? 1U : 0U);
    console_putc('\n');
}

static void framebuffer_scenario(const struct boot_framebuffer *framebuffer)
{
    struct framebuffer_state screen;
    uint32_t coordinates[FRAMEBUFFER_TEST_PROBES][2];
    uint32_t mask;
    size_t probes = 0U;
    enum framebuffer_status status;

    if (framebuffer == NULL || !framebuffer->present) {
        kernel_test_fail("the boot loader set no framebuffer");
    }

    if (!paging_is_active()) {
        kernel_test_fail("the framebuffer scenario ran before paging");
    }

    /*
     * Boot adopts the framebuffer before the scenarios run, because the screen
     * console has to come up early enough to show the rest of the boot. So the
     * framebuffer being already initialized is the expected state here and not
     * a failure; what this scenario needs is a framebuffer that is up, not one
     * that it personally brought up.
     *
     * Adopting it a second time must still be refused, and that refusal is the
     * one this branch is asserting.
     */
    status = framebuffer_initialize(framebuffer);

    if (status != FRAMEBUFFER_STATUS_OK &&
        status != FRAMEBUFFER_STATUS_ALREADY_INITIALIZED) {
        kernel_test_fail(framebuffer_status_string(status));
    }

    if (!framebuffer_is_active()) {
        kernel_test_fail("the framebuffer scenario has no framebuffer");
    }

    screen = framebuffer_get_state();
    mask = framebuffer_visible_mask();

    if (screen.width < 4U || screen.height < 4U) {
        kernel_test_fail("the framebuffer is too small to probe");
    }

    /* Four corners, four edge midpoints, and the rest spread through it. */
    coordinates[probes][0] = 0U;
    coordinates[probes++][1] = 0U;
    coordinates[probes][0] = screen.width - 1U;
    coordinates[probes++][1] = 0U;
    coordinates[probes][0] = 0U;
    coordinates[probes++][1] = screen.height - 1U;
    coordinates[probes][0] = screen.width - 1U;
    coordinates[probes++][1] = screen.height - 1U;
    coordinates[probes][0] = screen.width / 2U;
    coordinates[probes++][1] = 0U;
    coordinates[probes][0] = screen.width / 2U;
    coordinates[probes++][1] = screen.height - 1U;
    coordinates[probes][0] = 0U;
    coordinates[probes++][1] = screen.height / 2U;
    coordinates[probes][0] = screen.width - 1U;
    coordinates[probes++][1] = screen.height / 2U;

    while (probes < FRAMEBUFFER_TEST_PROBES) {
        coordinates[probes][0] =
            (uint32_t)(probes * 37U) % screen.width;
        coordinates[probes][1] =
            (uint32_t)(probes * 53U) % screen.height;
        ++probes;
    }

    /*
     * A distinct colour per probe, so a coordinate that aliases another shows
     * up as the wrong colour rather than as a coincidence.
     */
    for (size_t index = 0; index < probes; ++index) {
        const uint32_t colour = framebuffer_pack(
            (uint8_t)(index * 7U + 1U),
            (uint8_t)(index * 11U + 2U),
            (uint8_t)(index * 13U + 3U)
        );

        if (framebuffer_write_pixel(coordinates[index][0],
                coordinates[index][1], colour) != FRAMEBUFFER_STATUS_OK) {
            kernel_test_fail("the framebuffer refused a visible pixel");
        }
    }

    cpu_store_fence();

    for (size_t index = 0; index < probes; ++index) {
        const uint32_t x = coordinates[index][0];
        const uint32_t y = coordinates[index][1];
        const uint32_t colour = framebuffer_pack(
            (uint8_t)(index * 7U + 1U),
            (uint8_t)(index * 11U + 2U),
            (uint8_t)(index * 13U + 3U)
        );
        /*
         * Computed here from the loader's own pitch, not from framebuffer.c.
         * If this file and that one disagree about where a pixel lives, this is
         * where it surfaces.
         */
        const volatile uint32_t *raw = (const volatile uint32_t *)(uintptr_t)(
            framebuffer->address +
            (uint64_t)y * framebuffer->pitch +
            (uint64_t)x * FRAMEBUFFER_BYTES_PER_PIXEL);
        uint32_t through_api = 0U;

        if (framebuffer_read_pixel(x, y, &through_api) !=
            FRAMEBUFFER_STATUS_OK) {
            kernel_test_fail("the framebuffer refused a visible pixel");
        }

        if ((through_api & mask) != (colour & mask)) {
            kernel_test_fail("a framebuffer pixel did not hold its colour");
        }

        if ((*raw & mask) != (colour & mask)) {
            kernel_test_fail("the framebuffer wrote a pixel somewhere else");
        }
    }

    /*
     * No two probes may share an address. A pitch read as a width collapses
     * rows onto each other, which every single-pixel check above would survive.
     */
    for (size_t left = 0; left < probes; ++left) {
        for (size_t right = left + 1U; right < probes; ++right) {
            const uint64_t first =
                (uint64_t)coordinates[left][1] * framebuffer->pitch +
                (uint64_t)coordinates[left][0] * FRAMEBUFFER_BYTES_PER_PIXEL;
            const uint64_t second =
                (uint64_t)coordinates[right][1] * framebuffer->pitch +
                (uint64_t)coordinates[right][0] * FRAMEBUFFER_BYTES_PER_PIXEL;

            if (coordinates[left][0] == coordinates[right][0] &&
                coordinates[left][1] == coordinates[right][1]) {
                continue;
            }

            if (first == second) {
                kernel_test_fail("two framebuffer coordinates share an address");
            }
        }
    }

    /* The last visible pixel must still be inside the mapped span. */
    if ((uint64_t)(screen.height - 1U) * screen.pitch +
            (uint64_t)(screen.width - 1U) * FRAMEBUFFER_BYTES_PER_PIXEL +
            FRAMEBUFFER_BYTES_PER_PIXEL > screen.size) {
        kernel_test_fail("the last pixel lies outside the framebuffer");
    }

    if (framebuffer_write_pixel(screen.width, 0U, 0U) !=
            FRAMEBUFFER_STATUS_OUT_OF_BOUNDS ||
        framebuffer_write_pixel(0U, screen.height, 0U) !=
            FRAMEBUFFER_STATUS_OUT_OF_BOUNDS ||
        framebuffer_write_pixel(UINT32_MAX, UINT32_MAX, 0U) !=
            FRAMEBUFFER_STATUS_OUT_OF_BOUNDS) {
        kernel_test_fail("the framebuffer accepted a pixel off the screen");
    }

    status = framebuffer_verify();

    if (status != FRAMEBUFFER_STATUS_OK) {
        kernel_test_fail(framebuffer_status_string(status));
    }

    console_write("ST FRAMEBUFFER ");
    console_write_u64(screen.width);
    console_putc('x');
    console_write_u64(screen.height);
    console_write(" probes ");
    console_write_u64(probes);
    console_write(" pitch ");
    console_write_u64(screen.pitch);
    console_putc('\n');
}

void kernel_test_run(
    enum kernel_test_scenario scenario,
    const struct kernel_test_context *context
)
{
    enum pit_status pit_status;

    if (scenario == KERNEL_TEST_NONE) {
        return;
    }

    active_scenario = scenario;
    test_marker("BEGIN", scenario);

    if (context == NULL || context->framebuffer == NULL ||
        context->device_windows == NULL ||
        (context->mcfg_present && context->mcfg == NULL)) {
        kernel_test_fail("the test context is incomplete");
    }

    if (!interrupt_frame_layout_self_test()) {
        kernel_test_fail("interrupt frame or descriptor validation failed");
    }

    switch (scenario) {
    case KERNEL_TEST_NORMAL:
        return;
    case KERNEL_TEST_BREAKPOINT:
        if (!interrupt_breakpoint_self_test()) {
            kernel_test_fail("breakpoint register restoration failed");
        }
        kernel_test_pass();
    case KERNEL_TEST_INVALID_OPCODE:
        interrupt_trigger_invalid_opcode();
    case KERNEL_TEST_PAGE_FAULT:
        interrupt_trigger_page_fault();
    case KERNEL_TEST_IST:
        if (!interrupt_ist_self_test()) {
            kernel_test_fail("IST routing proof failed");
        }
        kernel_test_pass();
    case KERNEL_TEST_PIT:
        pit_status = pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_LEGACY_PIC);

        if (pit_status != PIT_STATUS_OK) {
            kernel_test_fail(pit_status_string(pit_status));
        }

        pit_status = pit_wait_for_ticks(PIT_TEST_TICKS);

        if (pit_status != PIT_STATUS_OK) {
            kernel_test_fail(pit_status_string(pit_status));
        }

        if (pit_ticks() < PIT_TEST_TICKS) {
            kernel_test_fail("PIT delivered too few ticks");
        }

        pit_status = pit_stop();

        if (pit_status != PIT_STATUS_OK) {
            kernel_test_fail(pit_status_string(pit_status));
        }

        kernel_test_pass();
    case KERNEL_TEST_UNEXPECTED:
        interrupt_trigger_unexpected();
    case KERNEL_TEST_APIC:
        apic_scenario();
        kernel_test_pass();
    case KERNEL_TEST_IOAPIC:
        ioapic_scenario();
        kernel_test_pass();
    case KERNEL_TEST_IOAPIC_LEVEL:
        ioapic_level_scenario();
        kernel_test_pass();
    case KERNEL_TEST_RETIRED:
        retired_scenario();
        kernel_test_pass();
    case KERNEL_TEST_APIC_TIMER:
        apic_timer_scenario();
        kernel_test_pass();
    case KERNEL_TEST_TSC:
        tsc_scenario();
        kernel_test_pass();
    case KERNEL_TEST_PM_TIMER:
        pm_timer_scenario();
        kernel_test_pass();
    case KERNEL_TEST_PIT_RETIRED:
        pit_retired_scenario();
        kernel_test_pass();
    case KERNEL_TEST_TIMERS:
        timers_scenario();
        kernel_test_pass();
    case KERNEL_TEST_PAGING:
        paging_scenario(context->device_windows);
        kernel_test_fail("a read-only page accepted a supervisor write");
    case KERNEL_TEST_HEAP:
        heap_scenario();
        kernel_test_fail("a heap guard page accepted a supervisor write");
    case KERNEL_TEST_PCI:
        pci_scenario(context->mcfg, context->mcfg_present);
        kernel_test_pass();
    case KERNEL_TEST_PCI_ECAM:
        pci_ecam_scenario(context->mcfg, context->mcfg_present,
            context->device_windows);
        kernel_test_pass();
    case KERNEL_TEST_THREADS:
        threads_scenario();
        kernel_test_pass();
    case KERNEL_TEST_THREAD_GUARD:
        thread_guard_scenario();
        kernel_test_fail("a thread stack guard page accepted a write");
    case KERNEL_TEST_FRAMEBUFFER:
        framebuffer_scenario(context->framebuffer);
        kernel_test_pass();
    case KERNEL_TEST_SCREEN:
        screen_scenario();
        kernel_test_pass();
    case KERNEL_TEST_KEYBOARD:
        keyboard_scenario();
        kernel_test_pass();
    case KERNEL_TEST_SHELL:
        shell_scenario();
        kernel_test_pass();
    case KERNEL_TEST_SURFACE:
        surface_scenario();
        kernel_test_pass();
    case KERNEL_TEST_WRITE_COMBINING:
        write_combining_scenario(context->framebuffer,
            context->device_windows);
        kernel_test_pass();
    case KERNEL_TEST_DEVICE_WINDOWS:
        device_windows_scenario(context->device_windows);
        kernel_test_pass();
    case KERNEL_TEST_BOOT_LEDGER:
        /* Deferred until kernel_main publishes the fully verified receipts. */
        return;
    case KERNEL_TEST_PHIPIA_PROOF:
        /* Deferred until the ledger and UI are both installed and published. */
        return;
    case KERNEL_TEST_DEVICE_SUBSTRATE:
        /* Deferred until the proof receipt is installed and published. */
        return;
    case KERNEL_TEST_XHCI:
        /* Deferred until the proof receipt is installed and published. */
        return;
    case KERNEL_TEST_NVME:
        /* Deferred until the proof receipt is installed and published. */
        return;
    case KERNEL_TEST_FILESYSTEM:
        /* Deferred until the proof receipt is installed and published. */
        return;
    case KERNEL_TEST_PROCESS:
        /* Deferred until the proof receipt is installed and published. */
        return;
    case KERNEL_TEST_LINUX_ABI:
        /* Deferred until the proof receipt is installed and published. */
        return;
    case KERNEL_TEST_LINUX_ABI_UNAME:
        /* Deferred until the uname proof receipt is installed and published. */
        return;
    case KERNEL_TEST_PHIPIA_PROOF_USERLAND:
    case KERNEL_TEST_PHIPIA_PROOF_USERLAND_ABSENT:
    case KERNEL_TEST_PHIPIA_PROOF_USERLAND_INTERACTIVE:
    case KERNEL_TEST_PHIPIA_PROOF_USERLAND_INTERACTIVE_ABSENT:
    case KERNEL_TEST_FAT32_SYSTEM:
    case KERNEL_TEST_FAT32_DATA:
    case KERNEL_TEST_FAT32_NESTED:
    case KERNEL_TEST_FAT32_GROWTH:
    case KERNEL_TEST_FAT32_RANDOM:
    case KERNEL_TEST_FAT32_TRUNCATE:
    case KERNEL_TEST_FAT32_RENAME:
    case KERNEL_TEST_FAT32_DELETE:
    case KERNEL_TEST_FAT32_FULL:
    case KERNEL_TEST_FAT32_CORRUPT:
    case KERNEL_TEST_FAT32_MISSING:
    case KERNEL_TEST_FAT32_PERSISTENCE:
    case KERNEL_TEST_FAT32_CACHE:
    case KERNEL_TEST_FAT32_IMMUTABLE:
    case KERNEL_TEST_FAT32_HANDLES:
    case KERNEL_TEST_NETWORK_NIC_DISCOVERY:
    case KERNEL_TEST_NETWORK_NIC_INITIALIZATION:
    case KERNEL_TEST_NETWORK_NIC_ABSENT:
    case KERNEL_TEST_NETWORK_LINK_DOWN:
    case KERNEL_TEST_NETWORK_DHCP:
    case KERNEL_TEST_NETWORK_DHCP_TIMEOUT:
    case KERNEL_TEST_NETWORK_STATIC:
    case KERNEL_TEST_NETWORK_ARP:
    case KERNEL_TEST_NETWORK_ICMP:
    case KERNEL_TEST_NETWORK_ICMP_TIMEOUT:
    case KERNEL_TEST_NETWORK_UDP:
    case KERNEL_TEST_NETWORK_DNS_A:
    case KERNEL_TEST_NETWORK_DNS_CNAME:
    case KERNEL_TEST_NETWORK_DNS_MALFORMED:
    case KERNEL_TEST_NETWORK_TCP:
    case KERNEL_TEST_NETWORK_TCP_RETRANSMIT:
    case KERNEL_TEST_NETWORK_TCP_RESET:
    case KERNEL_TEST_NETWORK_HTTP_LENGTH:
    case KERNEL_TEST_NETWORK_HTTP_CHUNKED:
    case KERNEL_TEST_NETWORK_HTTP_REDIRECT:
    case KERNEL_TEST_NETWORK_HTTP_MALFORMED:
    case KERNEL_TEST_NETWORK_HTTP_NESTED:
    case KERNEL_TEST_NETWORK_HTTP_REPLACE:
    case KERNEL_TEST_NETWORK_HTTP_DISK_FULL:
    case KERNEL_TEST_NETWORK_NIC_RESET:
    case KERNEL_TEST_NETWORK_SYSTEM_IMMUTABLE:
    case KERNEL_TEST_NETWORK_MISSING_LINUX_ECHO:
    case KERNEL_TEST_NETWORK_MISSING_LINUX_UNAME:
    case KERNEL_TEST_NETWORK_MISSING_LINUX_CAT:
    case KERNEL_TEST_NETWORK_FILES:
    case KERNEL_TEST_NETWORK_NOTES:
    case KERNEL_TEST_NETWORK_MEDIA_EDITOR:
    case KERNEL_TEST_NETWORK_PERSISTENCE:
    case KERNEL_TEST_NETWORK_SOCKET_ISOLATION:
    case KERNEL_TEST_NETWORK_TCP_LISTEN:
    case KERNEL_TEST_NETWORK_TCP_REFUSED:
    case KERNEL_TEST_MULTIPROCESS:
    case KERNEL_TEST_DRIVER_MATRIX:
    case KERNEL_TEST_DRIVER_MATRIX_BUILTIN:
    case KERNEL_TEST_AUDIO:
    case KERNEL_TEST_NVIDIA:
    case KERNEL_TEST_NVIDIA_BUILTIN:
    case KERNEL_TEST_NATIVE:
    case KERNEL_TEST_NATIVE_LUA:
    case KERNEL_TEST_NATIVE_SQLITE:
    case KERNEL_TEST_NATIVE_CANVAS:
    case KERNEL_TEST_NATIVE_NETWORK:
    case KERNEL_TEST_NATIVE_RUST:
    case KERNEL_TEST_NATIVE_CRASH:
    case KERNEL_TEST_NATIVE_ELF_REFUSAL:
    case KERNEL_TEST_NATIVE_DIGEST_REFUSAL:
    case KERNEL_TEST_NATIVE_ABI_REFUSAL:
    case KERNEL_TEST_NATIVE_RELAUNCH:
    case KERNEL_TEST_NATIVE_AUDIO:
    case KERNEL_TEST_NATIVE_SDL:
    case KERNEL_TEST_NATIVE_DYNAMIC:
    case KERNEL_TEST_NATIVE_HTTPS:
    case KERNEL_TEST_NATIVE_PHIP:
    case KERNEL_TEST_EXT4_RECOVERY:
        /* Deferred until Phipia and the Boot Ledger are published. */
        return;
    case KERNEL_TEST_MULTIPROCESS_SLOTS:
        multiprocess_slots_scenario();
        kernel_test_pass();
    case KERNEL_TEST_DOUBLE_FAULT:
        kernel_test_double_fault_armed = 1U;
        interrupt_test_set_gate_present(14U, false);
        interrupt_trigger_page_fault();
    case KERNEL_TEST_INVALID:
        kernel_test_fail("invalid or duplicate phipia.test argument");
    case KERNEL_TEST_NONE:
    default:
        kernel_test_fail("unreachable test scenario");
    }
}

_Noreturn void kernel_test_complete_normal(void)
{
    if (active_scenario != KERNEL_TEST_NORMAL) {
        kernel_test_fail("normal completion used outside the normal scenario");
    }

    kernel_test_pass();
}

_Noreturn void kernel_test_complete_ext4_recovery(void)
{
    static const uint8_t expected[] =
        "Phipia deterministic ext4 fixture\n";
    static const uint8_t transaction_byte = 'X';
    const struct boot_ledger *ledger = boot_ledger_installed();
    const struct boot_stage_receipt *nvme_proof;
    const struct boot_stage_receipt *fat16_proof;
    struct phipia_ext4_mount_diagnostic mount_diagnostic = {0};
    struct phipia_ext4_recovery_report clean_remount = {0};
    struct phipia_ext4_recovery_report recovery = {0};
    const struct phipfs_drive_info drive = phipfs_drive(PHIPFS_VOLUME_SYSTEM);
    struct phipfs_stat stat = {0};
    phipfs_handle handle = 0U;
    uint8_t bytes[sizeof(expected)] = {0};
    uint8_t appended = 0U;
    size_t read_bytes = 0U;
    size_t written_bytes = 0U;
    bool contents_match = true;
    bool transaction_already_visible = false;
    const bool power_cut = ext4_backend_test_power_cut_configured();
    const uint64_t before = phipfs_completion_count(PHIPFS_VOLUME_SYSTEM);

    if (active_scenario != KERNEL_TEST_EXT4_RECOVERY) {
        kernel_test_fail("ext4 recovery completion used outside its scenario");
    }
    nvme_proof = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_NVME_READ_PROOF);
    fat16_proof = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_FILESYSTEM_FILE_PROOF);
    if (ledger == NULL || nvme_proof == NULL || fat16_proof == NULL ||
        nvme_proof->result != BOOT_RECEIPT_SKIPPED ||
        fat16_proof->result != BOOT_RECEIPT_SKIPPED ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_NVME_FIXTURE_ABSENT) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_FILESYSTEM_FIXTURE_ABSENT) ||
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_NVME_READ_PROOF_COMPLETE) ||
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_FILESYSTEM_FILE_PROOF_COMPLETE)) {
        kernel_test_fail("ext4 namespace proof skips are invalid");
    }
    if (!drive.present || !drive.mounted || drive.read_only || !drive.healthy) {
        if (!ext4_backend_mount_diagnostic(PHIPFS_VOLUME_SYSTEM,
                &mount_diagnostic)) {
            kernel_test_fail("ext4 mount diagnostic is unavailable");
        }
        console_write("ST EXT4 RECOVERY mount status ");
        console_write_u64((uint64_t)ext4_backend_last_mount_status(
            PHIPFS_VOLUME_SYSTEM));
        console_write(" begin ");
        console_write_u64((uint64_t)mount_diagnostic.begin_status);
        console_write(" rust ");
        console_write_u64((uint64_t)(uint32_t)mount_diagnostic.rust_status);
        console_write(" close ");
        console_write_u64((uint64_t)mount_diagnostic.close_status);
        console_write(" nvme ");
        console_write_u64((uint64_t)(uint32_t)
            mount_diagnostic.nvme_close_status);
        console_write(" teardown ");
        console_write_u64((uint64_t)(uint32_t)
            mount_diagnostic.nvme_teardown_status);
        console_write(" resources ");
        console_write_u64((uint64_t)
            mount_diagnostic.nvme_resource_mismatches);
        console_putc('\n');
        kernel_test_fail("ext4 recovered drive state is invalid");
    }
    if (drive.free_bytes == 0U || drive.free_bytes >= drive.total_bytes) {
        kernel_test_fail("ext4 allocator capacity was not exported");
    }
    if (!ext4_backend_recovery_report(PHIPFS_VOLUME_SYSTEM, &recovery)) {
        kernel_test_fail("ext4 recovery report is unavailable");
    }
    if (recovery.performed) {
        if ((recovery.transactions == 0U &&
                (recovery.replayed_blocks != 0U ||
                 recovery.consumed_slots != 0U)) ||
            (recovery.transactions != 0U &&
                (recovery.transactions != 1U ||
                 recovery.replayed_blocks == 0U ||
                 recovery.consumed_slots != recovery.replayed_blocks + 2U))) {
            kernel_test_fail("ext4 recovery report is inconsistent");
        }
    } else if (recovery.transactions != 0U || recovery.replayed_blocks != 0U ||
        recovery.consumed_slots != 0U) {
        kernel_test_fail("clean ext4 mount reported journal recovery");
    }
    if (phipfs_stat_path(PHIPFS_VOLUME_SYSTEM, "system/README.TXT", &stat) !=
            PHIPFS_STATUS_OK || stat.directory || stat.read_only ||
        (stat.size != sizeof(expected) - 1U && stat.size != UINT64_C(4097)) ||
        phipfs_open(PHIPFS_VOLUME_SYSTEM, "system/README.TXT",
            PHIPFS_ACCESS_READ, &handle) != PHIPFS_STATUS_OK ||
        phipfs_pread(handle, bytes, sizeof(expected) - 1U, 0U, &read_bytes) !=
            PHIPFS_STATUS_OK || read_bytes != sizeof(expected) - 1U) {
        kernel_test_fail("ext4 recovered namespace could not be read");
    }
    transaction_already_visible = stat.size == UINT64_C(4097);
    for (size_t index = 0U; index < read_bytes; ++index) {
        if (bytes[index] != expected[index]) {
            contents_match = false;
        }
    }
    if (!contents_match ||
        (transaction_already_visible &&
            (phipfs_pread(handle, &appended, sizeof(appended), UINT64_C(4096),
                &read_bytes) != PHIPFS_STATUS_OK || read_bytes != 1U ||
             appended != transaction_byte)) ||
        phipfs_close(handle) != PHIPFS_STATUS_OK ||
        phipfs_close(handle) != PHIPFS_STATUS_STALE_HANDLE ||
        phipfs_completion_count(PHIPFS_VOLUME_SYSTEM) <= before) {
        kernel_test_fail("ext4 recovery read leaked or changed data");
    }
    if (phipfs_open(PHIPFS_VOLUME_SYSTEM, "system/README.TXT",
            PHIPFS_ACCESS_WRITE, &handle) != PHIPFS_STATUS_OK ||
        phipfs_pread(handle, &appended, sizeof(appended), 0U, &read_bytes) !=
            PHIPFS_STATUS_ACCESS ||
        phipfs_close(handle) != PHIPFS_STATUS_OK) {
        kernel_test_fail("ext4 writable handle access enforcement failed");
    }
    if (phipfs_sync(PHIPFS_VOLUME_SYSTEM) != PHIPFS_STATUS_OK) {
        kernel_test_fail("clean ext4 sync failed");
    }
    if (!power_cut && !transaction_already_visible) {
        if (!ext4_backend_test_fail_storage_once(3U) ||
            ext4_backend_transaction_probe(PHIPFS_VOLUME_SYSTEM,
                "system/README.TXT", UINT64_C(4096), &transaction_byte,
                sizeof(transaction_byte), &written_bytes) != PHIPFS_STATUS_IO ||
            !ext4_backend_test_storage_failure_observed(
                PHIPIA_EXT4_TEST_STORAGE_WRITE) ||
            !ext4_backend_test_fail_storage_once(3U) ||
            phipfs_sync(PHIPFS_VOLUME_SYSTEM) != PHIPFS_STATUS_IO ||
            !ext4_backend_test_storage_failure_observed(
                PHIPIA_EXT4_TEST_STORAGE_FLUSH)) {
            kernel_test_fail("ext4 pending allocation failure retry is invalid");
        }
    }
    if (!transaction_already_visible &&
        (ext4_backend_transaction_probe(PHIPFS_VOLUME_SYSTEM,
            "system/README.TXT", UINT64_C(4096), &transaction_byte,
            sizeof(transaction_byte), &written_bytes) != PHIPFS_STATUS_OK ||
         written_bytes != sizeof(transaction_byte))) {
        kernel_test_fail("ext4 private journal transaction probe failed");
    }
    if (!power_cut && !transaction_already_visible) {
        if (!ext4_backend_test_fail_storage_once(1U) ||
            phipfs_sync(PHIPFS_VOLUME_SYSTEM) != PHIPFS_STATUS_IO ||
            !ext4_backend_test_storage_failure_observed(
                PHIPIA_EXT4_TEST_STORAGE_WRITE) ||
            !ext4_backend_test_fail_storage_once(2U) ||
            phipfs_sync(PHIPFS_VOLUME_SYSTEM) != PHIPFS_STATUS_IO ||
            !ext4_backend_test_storage_failure_observed(
                PHIPIA_EXT4_TEST_STORAGE_FLUSH)) {
            kernel_test_fail("ext4 sync retry is invalid");
        }
    }
    if (phipfs_sync(PHIPFS_VOLUME_SYSTEM) != PHIPFS_STATUS_OK ||
        phipfs_stat_path(PHIPFS_VOLUME_SYSTEM, "system/README.TXT", &stat) !=
            PHIPFS_STATUS_OK || stat.size != UINT64_C(4097) ||
        phipfs_open(PHIPFS_VOLUME_SYSTEM, "system/README.TXT",
            PHIPFS_ACCESS_READ, &handle) != PHIPFS_STATUS_OK ||
        phipfs_pread(handle, &appended, sizeof(appended), UINT64_C(4096),
            &read_bytes) != PHIPFS_STATUS_OK || read_bytes != 1U ||
        appended != transaction_byte ||
        phipfs_close(handle) != PHIPFS_STATUS_OK) {
        kernel_test_fail("ext4 private journal transaction probe failed");
    }
    if (!power_cut && !transaction_already_visible) {
        if (phipfs_truncate(PHIPFS_VOLUME_SYSTEM,
                "system/README.TXT", sizeof(expected) - 1U) !=
                PHIPFS_STATUS_OK ||
            phipfs_sync(PHIPFS_VOLUME_SYSTEM) != PHIPFS_STATUS_OK ||
            phipfs_stat_path(PHIPFS_VOLUME_SYSTEM, "system/README.TXT", &stat) !=
                PHIPFS_STATUS_OK || stat.size != sizeof(expected) - 1U ||
            phipfs_open(PHIPFS_VOLUME_SYSTEM, "system/README.TXT",
                PHIPFS_ACCESS_READ, &handle) != PHIPFS_STATUS_OK ||
            phipfs_pread(handle, &appended, sizeof(appended), UINT64_C(4096),
                &read_bytes) != PHIPFS_STATUS_OK || read_bytes != 0U ||
            phipfs_close(handle) != PHIPFS_STATUS_OK) {
            kernel_test_fail("ext4 private truncate revocation probe failed");
        }
        written_bytes = 0U;
        if (phipfs_open(PHIPFS_VOLUME_SYSTEM, "system/README.TXT",
                PHIPFS_ACCESS_WRITE, &handle) != PHIPFS_STATUS_OK ||
            phipfs_seek(handle, INT64_C(4096), PHIPFS_SEEK_START, NULL) !=
                PHIPFS_STATUS_INVALID_ARGUMENT ||
            phipfs_seek(handle, INT64_C(4096), PHIPFS_SEEK_START,
                &stat.size) != PHIPFS_STATUS_OK || stat.size != UINT64_C(4096) ||
            phipfs_write(handle, &transaction_byte, sizeof(transaction_byte),
                &written_bytes) != PHIPFS_STATUS_OK ||
            written_bytes != sizeof(transaction_byte) ||
            phipfs_close(handle) != PHIPFS_STATUS_OK ||
            phipfs_sync(PHIPFS_VOLUME_SYSTEM) != PHIPFS_STATUS_OK ||
            phipfs_stat_path(PHIPFS_VOLUME_SYSTEM, "system/README.TXT", &stat) !=
                PHIPFS_STATUS_OK || stat.size != UINT64_C(4097)) {
            kernel_test_fail("ext4 post-truncate marker re-arm failed");
        }
        if (phipfs_create_mode(PHIPFS_VOLUME_SYSTEM,
                "data/user/JRNLPROBE.TMP", UINT16_C(0555)) !=
                PHIPFS_STATUS_OK ||
            phipfs_stat_path(PHIPFS_VOLUME_SYSTEM, "data/user/JRNLPROBE.TMP",
                &stat) != PHIPFS_STATUS_OK || stat.directory || stat.size != 0U ||
            stat.read_only || (stat.mode & UINT16_C(0777)) != UINT16_C(0555) ||
            phipfs_open(PHIPFS_VOLUME_SYSTEM, "data/user/JRNLPROBE.TMP",
                PHIPFS_ACCESS_READ_WRITE, &handle) != PHIPFS_STATUS_OK ||
            phipfs_write(handle, &transaction_byte, sizeof(transaction_byte),
                &written_bytes) != PHIPFS_STATUS_OK || written_bytes != 1U ||
            phipfs_seek(handle, 0, PHIPFS_SEEK_START, &stat.size) !=
                PHIPFS_STATUS_OK || stat.size != 0U ||
            phipfs_read(handle, &appended, sizeof(appended), &read_bytes) !=
                PHIPFS_STATUS_OK || read_bytes != 1U ||
            appended != transaction_byte ||
            phipfs_unlink(PHIPFS_VOLUME_SYSTEM,
                "data/user/JRNLPROBE.TMP") != PHIPFS_STATUS_BUSY ||
            phipfs_rename(PHIPFS_VOLUME_SYSTEM,
                "data/user/JRNLPROBE.TMP", "data/user/JRNLPROBE.BUSY") !=
                    PHIPFS_STATUS_BUSY ||
            phipfs_close(handle) != PHIPFS_STATUS_OK ||
            phipfs_sync(PHIPFS_VOLUME_SYSTEM) != PHIPFS_STATUS_OK ||
            phipfs_link(PHIPFS_VOLUME_SYSTEM,
                "data/user/JRNLPROBE.TMP", "data/user/JRNLPROBE.LNK") !=
                    PHIPFS_STATUS_OK ||
            phipfs_sync(PHIPFS_VOLUME_SYSTEM) != PHIPFS_STATUS_OK ||
            phipfs_unlink(PHIPFS_VOLUME_SYSTEM,
                "data/user/JRNLPROBE.TMP") != PHIPFS_STATUS_OK ||
            phipfs_sync(PHIPFS_VOLUME_SYSTEM) != PHIPFS_STATUS_OK ||
            phipfs_stat_path(PHIPFS_VOLUME_SYSTEM, "data/user/JRNLPROBE.TMP",
                &stat) != PHIPFS_STATUS_NOT_FOUND ||
            phipfs_stat_path(PHIPFS_VOLUME_SYSTEM, "data/user/JRNLPROBE.LNK",
                &stat) != PHIPFS_STATUS_OK || stat.directory || stat.size != 1U ||
            phipfs_rename(PHIPFS_VOLUME_SYSTEM,
                "data/user/JRNLPROBE.LNK", "data/user/JRNLPROBE.REN") !=
                    PHIPFS_STATUS_OK ||
            phipfs_sync(PHIPFS_VOLUME_SYSTEM) != PHIPFS_STATUS_OK ||
            phipfs_stat_path(PHIPFS_VOLUME_SYSTEM, "data/user/JRNLPROBE.LNK",
                &stat) != PHIPFS_STATUS_NOT_FOUND ||
            phipfs_stat_path(PHIPFS_VOLUME_SYSTEM, "data/user/JRNLPROBE.REN",
                &stat) != PHIPFS_STATUS_OK || stat.directory || stat.size != 1U ||
            phipfs_unlink(PHIPFS_VOLUME_SYSTEM,
                "data/user/JRNLPROBE.REN") != PHIPFS_STATUS_OK ||
            phipfs_sync(PHIPFS_VOLUME_SYSTEM) != PHIPFS_STATUS_OK ||
            phipfs_stat_path(PHIPFS_VOLUME_SYSTEM, "data/user/JRNLPROBE.REN",
                &stat) != PHIPFS_STATUS_NOT_FOUND ||
            phipfs_mkdir(PHIPFS_VOLUME_SYSTEM,
                "data/user/JRNLPROBE.DIR") != PHIPFS_STATUS_OK ||
            phipfs_sync(PHIPFS_VOLUME_SYSTEM) != PHIPFS_STATUS_OK ||
            phipfs_stat_path(PHIPFS_VOLUME_SYSTEM, "data/user/JRNLPROBE.DIR",
                &stat) != PHIPFS_STATUS_OK || !stat.directory ||
            phipfs_rename(PHIPFS_VOLUME_SYSTEM,
                "data/user/JRNLPROBE.DIR", "data/user/JRNLPROBE.RDR") !=
                    PHIPFS_STATUS_OK ||
            phipfs_sync(PHIPFS_VOLUME_SYSTEM) != PHIPFS_STATUS_OK ||
            phipfs_stat_path(PHIPFS_VOLUME_SYSTEM, "data/user/JRNLPROBE.DIR",
                &stat) != PHIPFS_STATUS_NOT_FOUND ||
            phipfs_stat_path(PHIPFS_VOLUME_SYSTEM, "data/user/JRNLPROBE.RDR",
                &stat) != PHIPFS_STATUS_OK || !stat.directory ||
            phipfs_create(PHIPFS_VOLUME_SYSTEM,
                "data/user/JRNLPROBE.RDR/CHILD.TMP") != PHIPFS_STATUS_OK ||
            phipfs_sync(PHIPFS_VOLUME_SYSTEM) != PHIPFS_STATUS_OK ||
            phipfs_rmdir(PHIPFS_VOLUME_SYSTEM,
                "data/user/JRNLPROBE.RDR") != PHIPFS_STATUS_NOT_EMPTY ||
            phipfs_stat_path(PHIPFS_VOLUME_SYSTEM,
                "data/user/JRNLPROBE.RDR/CHILD.TMP", &stat) !=
                    PHIPFS_STATUS_OK || stat.directory ||
            phipfs_unlink(PHIPFS_VOLUME_SYSTEM,
                "data/user/JRNLPROBE.RDR/CHILD.TMP") != PHIPFS_STATUS_OK ||
            phipfs_sync(PHIPFS_VOLUME_SYSTEM) != PHIPFS_STATUS_OK ||
            phipfs_rmdir(PHIPFS_VOLUME_SYSTEM,
                "data/user/JRNLPROBE.RDR") != PHIPFS_STATUS_OK ||
            phipfs_sync(PHIPFS_VOLUME_SYSTEM) != PHIPFS_STATUS_OK ||
            phipfs_stat_path(PHIPFS_VOLUME_SYSTEM, "data/user/JRNLPROBE.RDR",
                &stat) != PHIPFS_STATUS_NOT_FOUND) {
            kernel_test_fail("ext4 VFS namespace journal proof failed");
        }
    }
    if (phipfs_unmount(PHIPFS_VOLUME_SYSTEM) != PHIPFS_STATUS_OK ||
        phipfs_drive(PHIPFS_VOLUME_SYSTEM).mounted ||
        !nvme_filesystem_session_resources_released() ||
        heap_verify() != HEAP_STATUS_OK) {
        kernel_test_fail("ext4 recovered mount did not release cleanly");
    }
    const struct heap_state heap_before_remount = heap_get_state();
    const struct frame_allocator_stats frames_before_remount =
        frame_allocator_get_stats();
    if (phipfs_mount(PHIPFS_VOLUME_SYSTEM) != PHIPFS_STATUS_OK ||
        !ext4_backend_recovery_report(PHIPFS_VOLUME_SYSTEM, &clean_remount) ||
        clean_remount.performed || clean_remount.transactions != 0U ||
        clean_remount.replayed_blocks != 0U ||
        clean_remount.consumed_slots != 0U ||
        phipfs_stat_path(PHIPFS_VOLUME_SYSTEM, "system/README.TXT", &stat) !=
            PHIPFS_STATUS_OK || stat.directory ||
        stat.size != UINT64_C(4097) ||
        phipfs_open(PHIPFS_VOLUME_SYSTEM, "system/README.TXT",
            PHIPFS_ACCESS_READ, &handle) != PHIPFS_STATUS_OK ||
        phipfs_pread(handle, &appended, sizeof(appended), UINT64_C(4096),
            &read_bytes) != PHIPFS_STATUS_OK || read_bytes != 1U ||
        appended != transaction_byte || phipfs_close(handle) != PHIPFS_STATUS_OK ||
        phipfs_unmount(PHIPFS_VOLUME_SYSTEM) != PHIPFS_STATUS_OK ||
        phipfs_drive(PHIPFS_VOLUME_SYSTEM).mounted ||
        !nvme_filesystem_session_resources_released() ||
        heap_verify() != HEAP_STATUS_OK ||
        paging_verify() != PAGING_STATUS_OK) {
        kernel_test_fail("clean ext4 remount or release failed");
    }
    const struct heap_state heap_after_remount = heap_get_state();
    const struct frame_allocator_stats frames_after_remount =
        frame_allocator_get_stats();
    if (heap_after_remount.base_address != heap_before_remount.base_address ||
        heap_after_remount.size != heap_before_remount.size ||
        heap_after_remount.committed_bytes !=
            heap_before_remount.committed_bytes ||
        heap_after_remount.allocated_bytes !=
            heap_before_remount.allocated_bytes ||
        heap_after_remount.block_count != heap_before_remount.block_count ||
        heap_after_remount.live_allocations !=
            heap_before_remount.live_allocations ||
        heap_after_remount.mapped_pages != heap_before_remount.mapped_pages ||
        heap_after_remount.active != heap_before_remount.active ||
        frames_after_remount.addressable_frames !=
            frames_before_remount.addressable_frames ||
        frames_after_remount.allocatable_frames !=
            frames_before_remount.allocatable_frames ||
        frames_after_remount.free_frames != frames_before_remount.free_frames ||
        frames_after_remount.allocated_frames !=
            frames_before_remount.allocated_frames ||
        frames_after_remount.reserved_frames !=
            frames_before_remount.reserved_frames ||
        frames_after_remount.highest_allocatable_address !=
            frames_before_remount.highest_allocatable_address) {
        kernel_test_fail("clean ext4 remount changed the resource census");
    }
    console_write("ST EXT4 RECOVERY marker cleared transaction committed ");
    console_write("appended exact truncate revoke rearm create mode hardlink unlink journal clean ");
    console_write("transactions 0 replay 0 slots 0 ");
    console_write("VFS writable remount clean resources exact\n");
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_native(void)
{
    static const uint8_t expected[] = "native ABI v1\n";
    struct native_process_result result = { 0 };
    struct phipfs_stat output;
    phipfs_handle file;
    uint8_t bytes[sizeof(expected) - 1U];
    size_t read_bytes = 0U;
    bool content_matches = true;
    enum native_process_status launch_status;

    if (active_scenario != KERNEL_TEST_NATIVE) {
        kernel_test_fail("native completion used outside its scenario");
    }
    launch_status = native_process_launch("NATIVET.MAN", &result);
    if (launch_status != NATIVE_PROCESS_OK) {
        console_write("Phipia: native launch refusal: ");
        console_write(native_process_status_string(launch_status));
        console_putc('\n');
        kernel_test_fail("native application admission failed");
    }
    if (!result.exited || result.faulted ||
        result.exit_status != 0 || !result.resources_released ||
        result.syscall_count < 20U || result.thread_switches < 4U ||
        result.context_transition_samples == 0U ||
        result.context_cycles_with_fpu < result.context_cycles_without_fpu ||
        !native_process_resources_released()) {
        console_write("Phipia: native result exit ");
        if (result.exit_status < 0) {
            console_putc('-');
            console_write_u64((uint64_t)(-(int64_t)result.exit_status));
        } else {
            console_write_u64((uint64_t)result.exit_status);
        }
        console_write(" syscalls ");
        console_write_u64(result.syscall_count);
        console_write(" switches ");
        console_write_u64(result.thread_switches);
        console_write(" peak pages ");
        console_write_u64(result.peak_pages);
        console_write(" peak handles ");
        console_write_u64(result.peak_handles);
        console_write(" exited ");
        console_write(result.exited ? "yes" : "no");
        console_write(" faulted ");
        console_write(result.faulted ? "yes" : "no");
        console_write(" released ");
        console_write(result.resources_released ? "yes" : "no");
        console_putc('\n');
        kernel_test_fail("native application did not exit with a clean census");
    }
    console_write("PHIPIA PERF context-switch transitions=");
    console_write_u64(result.context_transition_samples);
    console_write(" without_fpu_cycles=");
    console_write_u64(result.context_cycles_without_fpu /
        result.context_transition_samples);
    console_write(" with_fpu_cycles=");
    console_write_u64(result.context_cycles_with_fpu /
        result.context_transition_samples);
    console_putc('\n');
    if (phipfs_stat_path(PHIPFS_VOLUME_DATA, "NATIVET/FOUND.TXT", &output) !=
            PHIPFS_STATUS_OK || output.directory || output.size != sizeof(bytes) ||
        phipfs_open(PHIPFS_VOLUME_DATA, "NATIVET/FOUND.TXT", PHIPFS_ACCESS_READ,
            &file) != PHIPFS_STATUS_OK ||
        phipfs_read(file, bytes, sizeof(bytes), &read_bytes) != PHIPFS_STATUS_OK ||
        read_bytes != sizeof(bytes)) {
        kernel_test_fail("native Ring 3 file result is missing");
    }
    for (size_t index = 0U; index < sizeof(bytes); ++index) {
        content_matches = content_matches && bytes[index] == expected[index];
    }
    if (phipfs_close(file) != PHIPFS_STATUS_OK || !content_matches) {
        kernel_test_fail("native Ring 3 file result is wrong");
    }
    console_write("Phipia: native general loader, SDK, TLS, threads and FPU passed\n");
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_native_lua(void)
{
    static const uint8_t expected[] =
        "input=phipia\nsum=5050\nmath=ok\n";
    struct native_process_result result;
    struct phipfs_stat output;
    phipfs_handle file;
    uint8_t bytes[sizeof(expected) - 1U];
    size_t read_bytes = 0U;
    bool content_matches = true;

    if (active_scenario != KERNEL_TEST_NATIVE_LUA) {
        kernel_test_fail("Lua completion used outside its scenario");
    }
    if (native_process_launch("LUA.MAN", &result) != NATIVE_PROCESS_OK ||
        !result.exited || result.faulted || result.exit_status != 0 ||
        !result.resources_released || result.syscall_count < 10U ||
        !native_process_resources_released()) {
        kernel_test_fail("Lua did not exit with a clean resource census");
    }
    if (phipfs_stat_path(PHIPFS_VOLUME_DATA, "LUA/RESULT.TXT", &output) !=
            PHIPFS_STATUS_OK || output.directory || output.size != sizeof(bytes) ||
        phipfs_open(PHIPFS_VOLUME_DATA, "LUA/RESULT.TXT", PHIPFS_ACCESS_READ,
            &file) != PHIPFS_STATUS_OK ||
        phipfs_read(file, bytes, sizeof(bytes), &read_bytes) != PHIPFS_STATUS_OK ||
        read_bytes != sizeof(bytes)) {
        kernel_test_fail("Lua result file is missing");
    }
    for (size_t index = 0U; index < sizeof(bytes); ++index) {
        content_matches = content_matches && bytes[index] == expected[index];
    }
    if (phipfs_close(file) != PHIPFS_STATUS_OK || !content_matches ||
        phipfs_sync(PHIPFS_VOLUME_DATA) != PHIPFS_STATUS_OK) {
        kernel_test_fail("Lua result file is wrong or could not be synchronized");
    }
    console_write("Phipia: upstream Lua used stdin, Data, math and stdout\n");
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_native_sqlite(void)
{
    static const uint8_t expected[] =
        "rows=3\nsum=66\nintegrity=ok\n";
    struct native_process_result result;
    struct phipfs_stat database;
    struct phipfs_stat journal;
    struct phipfs_stat output;
    phipfs_handle file;
    uint8_t bytes[sizeof(expected) - 1U];
    size_t read_bytes = 0U;
    bool content_matches = true;
    const enum phipfs_status before = phipfs_stat_path(PHIPFS_VOLUME_DATA,
        "SQLITE/PORT.DB", &database);

    if (active_scenario != KERNEL_TEST_NATIVE_SQLITE) {
        kernel_test_fail("SQLite completion used outside its scenario");
    }
    if (before != PHIPFS_STATUS_OK && before != PHIPFS_STATUS_NOT_FOUND) {
        kernel_test_fail("SQLite database census failed before launch");
    }
    if (native_process_launch("SQLITE.MAN", &result) != NATIVE_PROCESS_OK ||
        !result.exited || result.faulted || result.exit_status != 0 ||
        !result.resources_released || result.syscall_count < 20U ||
        !native_process_resources_released()) {
        kernel_test_fail("SQLite did not exit with a clean resource census");
    }
    if (phipfs_stat_path(PHIPFS_VOLUME_DATA, "SQLITE/PORT.JRN", &journal) !=
            PHIPFS_STATUS_NOT_FOUND) {
        kernel_test_fail("SQLite left a rollback journal after clean close");
    }
    if (before == PHIPFS_STATUS_NOT_FOUND) {
        if (phipfs_stat_path(PHIPFS_VOLUME_DATA, "SQLITE/PORT.DB", &database) !=
                PHIPFS_STATUS_OK || database.directory || database.size == 0U ||
            phipfs_unmount(PHIPFS_VOLUME_DATA) != PHIPFS_STATUS_OK) {
            kernel_test_fail("SQLite first phase did not synchronize its database");
        }
        console_write("Phipia: upstream SQLite synchronized reboot phase\n");
        cpu_out8(UINT16_C(0x0064), UINT8_C(0xFE));
        kernel_test_fail("platform reset did not restart SQLite scenario");
    }
    if (phipfs_stat_path(PHIPFS_VOLUME_DATA, "SQLITE/RESULT.TXT", &output) !=
            PHIPFS_STATUS_OK || output.directory || output.size != sizeof(bytes) ||
        phipfs_open(PHIPFS_VOLUME_DATA, "SQLITE/RESULT.TXT", PHIPFS_ACCESS_READ,
            &file) != PHIPFS_STATUS_OK ||
        phipfs_read(file, bytes, sizeof(bytes), &read_bytes) != PHIPFS_STATUS_OK ||
        read_bytes != sizeof(bytes)) {
        kernel_test_fail("SQLite reboot result is missing");
    }
    for (size_t index = 0U; index < sizeof(bytes); ++index) {
        content_matches = content_matches && bytes[index] == expected[index];
    }
    if (phipfs_close(file) != PHIPFS_STATUS_OK || !content_matches ||
        phipfs_sync(PHIPFS_VOLUME_DATA) != PHIPFS_STATUS_OK) {
        kernel_test_fail("SQLite reboot result is wrong or could not be synchronized");
    }
    console_write("Phipia: upstream SQLite retained and verified three rows after reboot\n");
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_native_canvas(void)
{
    struct native_process_result result;
    uint64_t first_generation;
    uint64_t second_generation;
    enum native_process_status run_status;

    if (active_scenario != KERNEL_TEST_NATIVE_CANVAS) {
        kernel_test_fail("Canvas completion used outside its scenario");
    }
    if (native_process_spawn("CANVAS.MAN", &first_generation) !=
            NATIVE_PROCESS_OK ||
        native_process_spawn("CANVAS.MAN", &second_generation) !=
            NATIVE_PROCESS_OK ||
        first_generation == 0U || second_generation <= first_generation) {
        kernel_test_fail("Canvas applications were not admitted together");
    }
    run_status = native_process_run(&result);
    if (run_status != NATIVE_PROCESS_OK || !result.exited ||
        result.faulted || result.exit_status != 0 ||
        result.generation != second_generation || !result.resources_released ||
        result.syscall_count < 20U || result.thread_switches < 10U ||
        !native_process_resources_released() ||
        ui_native_window_is_open(0U) || ui_native_window_is_open(1U)) {
        console_write("Phipia: native Canvas run ");
        console_write(native_process_status_string(run_status));
        console_write(" generation ");
        console_write_u64(result.generation);
        console_write(" expected ");
        console_write_u64(second_generation);
        console_write(" exit ");
        if (result.exit_status < 0) {
            console_putc('-');
            console_write_u64((uint64_t)(-(int64_t)result.exit_status));
        } else {
            console_write_u64((uint64_t)result.exit_status);
        }
        console_write(" syscalls ");
        console_write_u64(result.syscall_count);
        console_write(" switches ");
        console_write_u64(result.thread_switches);
        console_write(" faulted ");
        console_write(result.faulted ? "yes" : "no");
        console_write(" released ");
        console_write(result.resources_released ? "yes" : "no");
        console_write(" windows ");
        console_write(ui_native_window_is_open(0U) ? "1" : "0");
        console_write(ui_native_window_is_open(1U) ? "1" : "0");
        console_putc('\n');
        kernel_test_fail("Canvas windows did not exit with a clean census");
    }
    console_write("Phipia: two native Canvas windows handled focus, input and partial damage\n");
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_native_network(void)
{
    static const uint8_t expected[] = "hello from the Phipia network\n";
    struct native_process_result result = { 0 };
    struct phipfs_stat output;
    struct network_state network;
    phipfs_handle file;
    uint8_t bytes[sizeof(expected) - 1U];
    size_t read_bytes = 0U;
    bool matches = true;
    enum native_process_status launch_status;

    if (active_scenario != KERNEL_TEST_NATIVE_NETWORK) {
        kernel_test_fail("native network completion used outside its scenario");
    }
    launch_status = native_process_launch("NETAPP.MAN", &result);
    if (launch_status != NATIVE_PROCESS_OK ||
        !result.exited || result.faulted || result.exit_status != 0 ||
        !result.resources_released || result.syscall_count < 25U ||
        !native_process_resources_released()) {
        console_write("Phipia: native network launch ");
        console_write(native_process_status_string(launch_status));
        console_write(" result exit ");
        if (result.exit_status < 0) {
            console_putc('-');
            console_write_u64((uint64_t)(-(int64_t)result.exit_status));
        } else {
            console_write_u64((uint64_t)result.exit_status);
        }
        console_write(" syscalls ");
        console_write_u64(result.syscall_count);
        console_write(" peak handles ");
        console_write_u64(result.peak_handles);
        console_write(" faulted ");
        console_write(result.faulted ? "yes" : "no");
        console_write(" released ");
        console_write(result.resources_released ? "yes" : "no");
        console_putc('\n');
        kernel_test_fail("native network app did not exit with a clean census");
    }
    network = network_get_state();
    if (network.udp_sockets != 0U || network.tcp_connections != 0U ||
        network.timers != 0U) {
        kernel_test_fail("native network handles survived process teardown");
    }
    if (phipfs_stat_path(PHIPFS_VOLUME_DATA, "NETAPP/HTTP.TXT", &output) !=
            PHIPFS_STATUS_OK || output.directory || output.size != sizeof(bytes) ||
        phipfs_open(PHIPFS_VOLUME_DATA, "NETAPP/HTTP.TXT", PHIPFS_ACCESS_READ,
            &file) != PHIPFS_STATUS_OK ||
        phipfs_read(file, bytes, sizeof(bytes), &read_bytes) != PHIPFS_STATUS_OK ||
        read_bytes != sizeof(bytes)) {
        kernel_test_fail("native HTTP body is missing from Data");
    }
    for (size_t index = 0U; index < sizeof(bytes); ++index) {
        matches = matches && bytes[index] == expected[index];
    }
    if (phipfs_close(file) != PHIPFS_STATUS_OK || !matches) {
        kernel_test_fail("native HTTP body framing or contents are wrong");
    }
    console_write("Phipia: native DNS, TCP, UDP, timeout, reset and cancellation passed\n");
    console_write("ST NETWORK production path bounded and recoverable\n");
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_native_rust(void)
{
    static const uint8_t expected[] = "native Rust no_std ABI v1\n";
    struct native_process_result result;
    struct phipfs_stat output;
    phipfs_handle file;
    uint8_t bytes[sizeof(expected) - 1U];
    size_t read_bytes = 0U;
    bool matches = true;

    if (active_scenario != KERNEL_TEST_NATIVE_RUST) {
        kernel_test_fail("Rust completion used outside its scenario");
    }
    if (native_process_launch("RUSTAPP.MAN", &result) != NATIVE_PROCESS_OK ||
        !result.exited || result.faulted || result.exit_status != 0 ||
        !result.resources_released || result.syscall_count < 12U ||
        result.thread_switches == 0U || !native_process_resources_released()) {
        kernel_test_fail("Rust application did not exit with a clean census");
    }
    if (phipfs_stat_path(PHIPFS_VOLUME_DATA, "RUSTAPP/RUST.TXT", &output) !=
            PHIPFS_STATUS_OK || output.directory || output.size != sizeof(bytes) ||
        phipfs_open(PHIPFS_VOLUME_DATA, "RUSTAPP/RUST.TXT", PHIPFS_ACCESS_READ,
            &file) != PHIPFS_STATUS_OK ||
        phipfs_read(file, bytes, sizeof(bytes), &read_bytes) != PHIPFS_STATUS_OK ||
        read_bytes != sizeof(bytes)) {
        kernel_test_fail("Rust application output is missing");
    }
    for (size_t index = 0U; index < sizeof(bytes); ++index) {
        matches = matches && bytes[index] == expected[index];
    }
    if (phipfs_close(file) != PHIPFS_STATUS_OK || !matches) {
        kernel_test_fail("Rust application output is wrong");
    }
    console_write("Phipia: no_std Rust application used native ABI v1 services\n");
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_native_crash(void)
{
    struct native_process_result crash;
    struct native_process_result survivor;

    if (active_scenario != KERNEL_TEST_NATIVE_CRASH) {
        kernel_test_fail("native crash completion used outside its scenario");
    }
    if (native_process_launch("CRASH.MAN", &crash) != NATIVE_PROCESS_OK ||
        !crash.exited || !crash.faulted || crash.exit_status != -PHIPIA_EFAULT ||
        !crash.resources_released || crash.peak_handles < 6U ||
        crash.peak_pages < 20U || !native_process_resources_released()) {
        kernel_test_fail("faulted process did not release its live resources");
    }
    if (native_process_launch("NATIVET.MAN", &survivor) != NATIVE_PROCESS_OK ||
        !survivor.exited || survivor.faulted || survivor.exit_status != 0 ||
        !survivor.resources_released || !native_process_resources_released()) {
        kernel_test_fail("process after fault observed damaged or leaked state");
    }
    console_write("Phipia: native crash contained; mappings handles threads windows FS x87 SSE reclaimed\n");
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_native_admission_refusal(void)
{
    struct native_process_result result;
    const char *manifest;
    const char *diagnostic;

    switch (active_scenario) {
    case KERNEL_TEST_NATIVE_ELF_REFUSAL:
        manifest = "BADELF.MAN";
        diagnostic = "Phipia: native malformed ELF refused; resource census unchanged\n";
        break;
    case KERNEL_TEST_NATIVE_DIGEST_REFUSAL:
        manifest = "BADDGST.MAN";
        diagnostic = "Phipia: native manifest digest mismatch refused; resource census unchanged\n";
        break;
    case KERNEL_TEST_NATIVE_ABI_REFUSAL:
        manifest = "BADABI.MAN";
        diagnostic = "Phipia: native unsupported ABI version refused; resource census unchanged\n";
        break;
    default:
        kernel_test_fail("native admission completion used outside its scenario");
    }
    if (!native_process_resources_released() ||
        native_process_launch(manifest, &result) !=
            NATIVE_PROCESS_IMAGE_REFUSED ||
        !native_process_resources_released()) {
        kernel_test_fail("native admission refusal changed the resource census");
    }
    console_write(diagnostic);
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_native_relaunch(void)
{
    struct native_process_result first;
    struct native_process_result second;

    if (active_scenario != KERNEL_TEST_NATIVE_RELAUNCH) {
        kernel_test_fail("native relaunch completion used outside its scenario");
    }
    if (native_process_launch("NATIVET.MAN", &first) != NATIVE_PROCESS_OK ||
        !first.exited || first.faulted || first.exit_status != 0 ||
        !first.resources_released || !native_process_resources_released() ||
        native_process_launch("NATIVET.MAN", &second) != NATIVE_PROCESS_OK ||
        !second.exited || second.faulted || second.exit_status != 0 ||
        !second.resources_released || second.generation <= first.generation ||
        !native_process_resources_released()) {
        kernel_test_fail("native relaunch did not reset generations and resources");
    }
    console_write("Phipia: native relaunch advanced generation; both resource censuses clean\n");
    kernel_test_pass();
}

static bool native_audio_census_equal(
    const struct pci_resource_state *pci_before,
    const struct dma_state *dma_before
)
{
    const struct pci_resource_state pci_after = pci_resource_get_state();
    const struct dma_state dma_after = dma_get_state();

    return pci_before->active_claims == pci_after.active_claims &&
        pci_before->active_mappings == pci_after.active_mappings &&
        pci_before->arena_pages == pci_after.arena_pages &&
        pci_before->mapped_pages == pci_after.mapped_pages &&
        pci_before->bus_masters == pci_after.bus_masters &&
        pci_before->active == pci_after.active &&
        dma_before->active_allocations == dma_after.active_allocations &&
        dma_before->cpu_owned_allocations == dma_after.cpu_owned_allocations &&
        dma_before->device_owned_allocations ==
            dma_after.device_owned_allocations &&
        dma_before->active == dma_after.active;
}

_Noreturn void kernel_test_complete_native_audio(void)
{
    struct native_process_result refusal = { 0 };
    struct native_process_result proof = { 0 };
    struct pci_resource_state pci_before;
    struct dma_state dma_before;

    if (active_scenario != KERNEL_TEST_NATIVE_AUDIO) {
        kernel_test_fail("native audio completion used outside its scenario");
    }
    pci_before = pci_resource_get_state();
    dma_before = dma_get_state();
    if (native_process_launch("AUDIONO.MAN", &refusal) != NATIVE_PROCESS_OK ||
        !refusal.exited || refusal.faulted || refusal.exit_status != 0 ||
        !refusal.resources_released || refusal.peak_handles != 0U ||
        !native_process_resources_released() ||
        !audio_native_resources_released() ||
        !native_audio_census_equal(&pci_before, &dma_before)) {
        kernel_test_fail(
            "audio capability refusal changed the resource census");
    }
    pci_before = pci_resource_get_state();
    dma_before = dma_get_state();
    if (native_process_launch("AUDIO.MAN", &proof) != NATIVE_PROCESS_OK ||
        !proof.exited || proof.faulted || proof.exit_status != 0 ||
        !proof.resources_released || proof.peak_handles != 2U ||
        proof.syscall_count < 20U || !native_process_resources_released() ||
        !audio_native_resources_released() || !audio_resources_released() ||
        !native_audio_census_equal(&pci_before, &dma_before)) {
        kernel_test_fail("native audio proof did not leave a clean census");
    }
    console_write(
        "Phipia: native audio ABI capability, mixing, cancellation and teardown passed\n");
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_native_sdl(void)
{
    static const char state_path[] = "SDLPROOF/SDL/D81F0C7A/STATE.BIN";
    struct native_process_result first = { 0 };
    struct native_process_result second = { 0 };
    struct phipfs_stat state;
    phipfs_handle file;
    uint8_t bytes[4];
    size_t read_bytes = 0U;

    if (active_scenario != KERNEL_TEST_NATIVE_SDL) {
        kernel_test_fail("native SDL completion used outside its scenario");
    }
    if (native_process_launch("SDLPROOF.MAN", &first) != NATIVE_PROCESS_OK ||
        !first.exited || first.faulted || first.exit_status != 0 ||
        !first.resources_released || first.peak_handles < 4U ||
        first.syscall_count < 20U || first.thread_switches == 0U ||
        !native_process_resources_released() ||
        !audio_native_resources_released() ||
        ui_native_window_is_open(0U) || ui_native_window_is_open(1U)) {
        kernel_test_fail("first SDL process did not leave a clean census");
    }
    if (native_process_launch("SDLPROOF.MAN", &second) != NATIVE_PROCESS_OK ||
        !second.exited || second.faulted || second.exit_status != 0 ||
        !second.resources_released || second.generation <= first.generation ||
        second.peak_handles < 4U || second.syscall_count < 20U ||
        second.thread_switches == 0U ||
        !native_process_resources_released() ||
        !audio_native_resources_released() || !audio_resources_released() ||
        ui_native_window_is_open(0U) || ui_native_window_is_open(1U)) {
        kernel_test_fail("second SDL process did not leave a clean census");
    }
    if (phipfs_stat_path(PHIPFS_VOLUME_DATA, state_path, &state) !=
            PHIPFS_STATUS_OK || state.directory || state.size != sizeof(bytes) ||
        phipfs_open(PHIPFS_VOLUME_DATA, state_path, PHIPFS_ACCESS_READ, &file) !=
            PHIPFS_STATUS_OK ||
        phipfs_read(file, bytes, sizeof(bytes), &read_bytes) != PHIPFS_STATUS_OK ||
        read_bytes != sizeof(bytes) || phipfs_close(file) != PHIPFS_STATUS_OK ||
        bytes[0] != 2U || bytes[1] != 0U || bytes[2] != 0U || bytes[3] != 0U) {
        kernel_test_fail("SDL preference state did not survive process relaunch");
    }
    console_write(
        "Phipia: SDL 2 window, input, partial damage, PCM and persistence passed\n");
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_native_dynamic(void)
{
    struct native_process_result proof = { 0 };
    uint64_t first_generation;
    uint64_t second_generation;

    if (active_scenario != KERNEL_TEST_NATIVE_DYNAMIC) {
        kernel_test_fail("native dynamic completion used outside its scenario");
    }
    if (native_process_spawn("DYNROOT.MAN", &first_generation) !=
            NATIVE_PROCESS_OK ||
        native_process_spawn("DYNROOT.MAN", &second_generation) !=
            NATIVE_PROCESS_OK ||
        first_generation == 0U || second_generation <= first_generation ||
        native_process_run(&proof) != NATIVE_PROCESS_OK ||
        !proof.exited || proof.faulted || proof.exit_status != 0 ||
        proof.generation != second_generation ||
        !proof.resources_released || proof.peak_handles != 0U ||
        proof.syscall_count < 7U || proof.thread_switches == 0U ||
        !native_process_resources_released()) {
        kernel_test_fail(
            "concurrent dynamic ELF proofs did not leave a clean census");
    }
    console_write(
        "Phipia: dynamic ELF shared RX, private TLS and lifecycle passed\n");
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_native_https(void)
{
    static const uint8_t expected[] =
        "hello from the Phipia HTTPS peer\n";
    struct native_process_result proof = { 0 };
    struct phipfs_stat output;
    struct network_state network;
    phipfs_handle file;
    uint8_t bytes[sizeof(expected) - 1U];
    size_t read_bytes = 0U;
    bool matches = true;

    if (active_scenario != KERNEL_TEST_NATIVE_HTTPS) {
        kernel_test_fail("native HTTPS completion used outside its scenario");
    }
    if (random_get_state().capability != RANDOM_CAPABILITY_INITIALIZED) {
        kernel_test_fail("native HTTPS did not retain strong hardware entropy");
    }
    if (native_process_launch("HTTPSAPP.MAN", &proof) != NATIVE_PROCESS_OK ||
        !proof.exited || proof.faulted || proof.exit_status != 0 ||
        !proof.resources_released || proof.syscall_count < 15U ||
        proof.thread_switches == 0U || !native_process_resources_released()) {
        kernel_test_fail("native HTTPS client did not leave a clean census");
    }
    network = network_get_state();
    if (network.udp_sockets != 0U || network.tcp_connections != 0U ||
        network.timers != 0U) {
        kernel_test_fail("native HTTPS network resources survived teardown");
    }
    if (phipfs_stat_path(PHIPFS_VOLUME_DATA, "HTTPSAPP/HTTPS.TXT", &output) !=
            PHIPFS_STATUS_OK || output.directory || output.size != sizeof(bytes) ||
        phipfs_open(PHIPFS_VOLUME_DATA, "HTTPSAPP/HTTPS.TXT", PHIPFS_ACCESS_READ,
            &file) != PHIPFS_STATUS_OK ||
        phipfs_read(file, bytes, sizeof(bytes), &read_bytes) != PHIPFS_STATUS_OK ||
        read_bytes != sizeof(bytes)) {
        kernel_test_fail("authenticated HTTPS body is missing from Data");
    }
    for (size_t index = 0U; index < sizeof(bytes); ++index) {
        matches = matches && bytes[index] == expected[index];
    }
    if (phipfs_close(file) != PHIPFS_STATUS_OK || !matches) {
        kernel_test_fail("authenticated HTTPS body contents are wrong");
    }
    console_write("Phipia: HTTPS strong hardware entropy passed\n");
    console_write(
        "Phipia: HTTPS TLS 1.2 hostname time trust framing close and teardown passed\n");
    console_write("ST NETWORK production path bounded and recoverable\n");
    kernel_test_pass();
}

static bool native_phip_authority_is_canonical(
    uint8_t *database,
    size_t database_capacity,
    struct package_service_report *service
)
{
    struct package_state_database_view view;
    struct package_state_package_view package;
    struct package_state_file_view executable;
    struct package_state_file_view manifest;
    size_t database_bytes = 0U;

    return package_service_snapshot(database, database_capacity,
            &database_bytes, service) == PACKAGE_SERVICE_STATUS_OK &&
        (service->generation == 1U || service->generation == 2U ||
            service->generation == 3U) &&
        service->live_file_handles == 0U &&
        service->live_allocations == 0U && database_bytes != 0U &&
        package_state_database_parse(database, database_bytes, &view) ==
            PACKAGE_STATE_STATUS_OK &&
        view.generation == service->generation && view.package_count == 1U &&
        view.edge_count == 0U && view.file_count == 2U &&
        package_state_database_package(&view, 0U, &package) ==
            PACKAGE_STATE_STATUS_OK &&
        package_state_database_file(&view, 0U, &executable) ==
            PACKAGE_STATE_STATUS_OK &&
        package_state_database_file(&view, 1U, &manifest) ==
            PACKAGE_STATE_STATUS_OK &&
        package_text_equals(&package.identifier, "org.libsdl.chess") &&
        ((service->generation == 1U &&
            package_text_equals(&package.version, "1.0.0")) ||
         (service->generation == 2U &&
            package_text_equals(&package.version, "2.0.0")) ||
         (service->generation == 3U &&
            package_text_equals(&package.version, "2.0.0"))) &&
        package_text_equals(&executable.path, "bin/CHESS.APP") &&
        package_text_equals(&manifest.path, "bin/CHESS.MAN") &&
        executable.owner_index == 0U && executable.length != 0U &&
        manifest.owner_index == 0U && manifest.length == UINT64_C(1024);
}

_Noreturn void kernel_test_complete_native_phip(void)
{
    static const uint8_t expected[] = "SDL chess release-2.32.10\n";
    static const char damaged_manifest[] =
        "pkgstate/gen/00000000/00000002/root/bin/CHESS.MAN";
    static const char repaired_manifest[] =
        "pkgstate/gen/00000000/00000003/root/bin/CHESS.MAN";
    static const char state_path[] =
        "SDLCHESS/SDL/8F0B0BEC/STATE.TXT";
    static uint8_t database[4096U];
    struct native_process_result proof = { 0 };
    struct package_service_report service;
    const struct phipfs_drive_info data = phipfs_drive(PHIPFS_VOLUME_DATA);
    struct phipfs_stat authority;
    struct phipfs_stat output;
    phipfs_handle file;
    uint8_t bytes[sizeof(expected) - 1U];
    size_t read_bytes = 0U;
    size_t database_bytes = 0U;
    bool matches = true;
    struct network_state network;
    enum phipfs_status authority_status;
    enum native_process_status launch_status;

    if (active_scenario != KERNEL_TEST_NATIVE_PHIP) {
        kernel_test_fail("native phip completion used outside its scenario");
    }
    if (random_get_state().capability != RANDOM_CAPABILITY_INITIALIZED) {
        kernel_test_fail("native phip did not retain strong hardware entropy");
    }
    if (!data.present || !data.mounted || data.read_only || !data.healthy ||
        data.total_bytes != UINT64_C(128) * UINT64_C(1024) * UINT64_C(1024) ||
        data.free_bytes == 0U || data.free_bytes >= data.total_bytes) {
        kernel_test_fail("native phip writable ext4 volume is unavailable");
    }
    authority_status = phipfs_stat_path(PHIPFS_VOLUME_DATA,
        PACKAGE_SERVICE_AUTHORITY_PATH, &authority);
    if (authority_status == PHIPFS_STATUS_NOT_FOUND) {
        launch_status = native_process_launch("PHIP.MAN", &proof);
        if (launch_status != NATIVE_PROCESS_OK ||
            !proof.exited || proof.faulted || proof.exit_status != 0 ||
            !proof.resources_released || proof.peak_handles < 3U ||
            proof.syscall_count < 20U || proof.thread_switches == 0U ||
            !native_process_resources_released()) {
            console_write("ST PHIP DIAGNOSTIC launch ");
            console_write_u64((uint64_t)launch_status);
            console_write(" exited ");
            console_write(proof.exited ? "yes" : "no");
            console_write(" faulted ");
            console_write(proof.faulted ? "yes" : "no");
            console_write(" status ");
            if (proof.exit_status < 0) {
                console_putc('-');
                console_write_u64((uint64_t)(-(int64_t)proof.exit_status));
            } else {
                console_write_u64((uint64_t)proof.exit_status);
            }
            console_write(" released ");
            console_write(proof.resources_released ? "yes" : "no");
            console_write(" handles ");
            console_write_u64(proof.peak_handles);
            console_write(" syscalls ");
            console_write_u64(proof.syscall_count);
            console_write(" last-syscall ");
            console_write_hex(proof.last_syscall);
            console_write(" failure-stage ");
            console_write_u64(proof.failure_stage);
            console_write(" switches ");
            console_write_u64(proof.thread_switches);
            console_putc('\n');
            kernel_test_fail("native phip client did not leave a clean census");
        }
        if (!native_phip_authority_is_canonical(database, sizeof(database),
                &service) || service.generation != 1U) {
            kernel_test_fail("native phip installed authority is not canonical");
        }
        if (phipfs_sync(PHIPFS_VOLUME_DATA) != PHIPFS_STATUS_OK ||
            phipfs_unmount(PHIPFS_VOLUME_DATA) != PHIPFS_STATUS_OK ||
            !nvme_filesystem_session_resources_released() ||
            !native_process_resources_released()) {
            kernel_test_fail("native phip ext4 reboot barrier leaked resources");
        }
        console_write(
            "Phipia: signed HTTPS package install synchronized reboot phase\n");
        cpu_out8(UINT16_C(0x0064), UINT8_C(0xFE));
        kernel_test_fail("platform reset did not restart QEMU");
    }
    if (authority_status != PHIPFS_STATUS_OK || authority.directory ||
        authority.size == 0U) {
        kernel_test_fail("native phip reboot authority is unavailable");
    }
    network = network_get_state();
    if (network.udp_sockets != 0U || network.tcp_connections != 0U ||
        network.timers != 0U) {
        kernel_test_fail("native phip network resources survived teardown");
    }
    if (!native_phip_authority_is_canonical(database, sizeof(database),
            &service)) {
        kernel_test_fail("native phip reboot authority is not canonical");
    }
    if (service.generation == 1U) {
        if (native_process_launch("PHIP.MAN", &proof) != NATIVE_PROCESS_OK ||
            !proof.exited || proof.faulted || proof.exit_status != 0 ||
            !proof.resources_released || proof.peak_handles < 3U ||
            proof.syscall_count < 20U || proof.thread_switches == 0U ||
            !native_process_resources_released() ||
            !native_phip_authority_is_canonical(database, sizeof(database),
                &service) || service.generation != 2U ||
            phipfs_sync(PHIPFS_VOLUME_DATA) != PHIPFS_STATUS_OK ||
            phipfs_unmount(PHIPFS_VOLUME_DATA) != PHIPFS_STATUS_OK ||
            !nvme_filesystem_session_resources_released()) {
            kernel_test_fail("native phip ext4 update did not commit cleanly");
        }
        console_write(
            "Phipia: signed HTTPS package update synchronized reboot phase\n");
        cpu_out8(UINT16_C(0x0064), UINT8_C(0xFE));
        kernel_test_fail("platform reset did not restart QEMU");
    }
    if (service.generation != 2U ||
        native_process_launch("PHIP.MAN", &proof) != NATIVE_PROCESS_OK ||
        !proof.exited || proof.faulted || proof.exit_status != 21 ||
        !proof.resources_released || !native_process_resources_released() ||
        !native_phip_authority_is_canonical(database, sizeof(database),
            &service) || service.generation != 2U) {
        kernel_test_fail("native phip signed rollback was not refused cleanly");
    }
    if (phipfs_truncate(PHIPFS_VOLUME_DATA, damaged_manifest, UINT64_C(1)) !=
            PHIPFS_STATUS_OK ||
        phipfs_sync(PHIPFS_VOLUME_DATA) != PHIPFS_STATUS_OK ||
        package_service_snapshot(database, sizeof(database), &database_bytes,
            &service) != PACKAGE_SERVICE_STATUS_INCOMPLETE ||
        database_bytes != 0U || service.journal_present ||
        service.live_file_handles != 0U || service.live_allocations != 0U) {
        kernel_test_fail("native phip damaged generation was not quarantined");
    }
    console_write(
        "Phipia: damaged package generation quarantined before repair passed\n");
    if (native_process_launch("PHIPREP.MAN", &proof) != NATIVE_PROCESS_OK ||
        !proof.exited || proof.faulted || proof.exit_status != 0 ||
        !proof.resources_released || proof.peak_handles < 3U ||
        proof.syscall_count < 20U || proof.thread_switches == 0U ||
        !native_process_resources_released() ||
        !native_phip_authority_is_canonical(database, sizeof(database),
            &service) || service.generation != 3U ||
        native_process_launch_installed(repaired_manifest, &proof) !=
            NATIVE_PROCESS_OK ||
        !proof.exited || proof.faulted || proof.exit_status != 0 ||
        !proof.resources_released || proof.syscall_count < 12U ||
        proof.thread_switches == 0U || !native_process_resources_released() ||
        phipfs_stat_path(PHIPFS_VOLUME_DATA, state_path, &output) !=
            PHIPFS_STATUS_OK || output.directory ||
        output.size != sizeof(bytes) ||
        phipfs_open(PHIPFS_VOLUME_DATA, state_path,
            PHIPFS_ACCESS_READ, &file) != PHIPFS_STATUS_OK ||
        phipfs_read(file, bytes, sizeof(bytes), &read_bytes) !=
            PHIPFS_STATUS_OK || read_bytes != sizeof(bytes)) {
        kernel_test_fail("native phip authenticated repair did not launch");
    }
    for (size_t index = 0U; index < sizeof(bytes); ++index) {
        matches = matches && bytes[index] == expected[index];
    }
    if (phipfs_close(file) != PHIPFS_STATUS_OK || !matches ||
        phipfs_sync(PHIPFS_VOLUME_DATA) != PHIPFS_STATUS_OK ||
        phipfs_unmount(PHIPFS_VOLUME_DATA) != PHIPFS_STATUS_OK ||
        !nvme_filesystem_session_resources_released()) {
        kernel_test_fail("native phip upstream SDL launch did not cleanly sync");
    }
    console_write(
        "Phipia: damaged SDL package repaired authenticated and launched from writable ext4 passed\n");
    console_write("ST NETWORK production path bounded and recoverable\n");
    kernel_test_pass();
}

static uint32_t boot_ledger_stage_sequence(
    const struct boot_ledger *ledger,
    enum boot_stage_id stage
)
{
    const struct boot_stage_receipt *receipt =
        boot_ledger_receipt_for(ledger, stage);

    return receipt == NULL ? 0U : receipt->sequence;
}

static uint32_t boot_ledger_capability_sequence(
    const struct boot_ledger *ledger,
    enum boot_capability capability
)
{
    for (size_t receipt_index = 0U;
         receipt_index < ledger->receipt_count;
         ++receipt_index) {
        const struct boot_stage_receipt *receipt =
            boot_ledger_receipt_at(ledger, receipt_index);

        if (receipt == NULL) {
            kernel_test_fail("Boot Ledger receipt lookup inconsistent");
        }

        for (size_t capability_index = 0U;
             capability_index < receipt->provided_capability_count;
             ++capability_index) {
            if (receipt->provided_capabilities[capability_index] ==
                capability) {
                return receipt->sequence;
            }
        }
    }

    return 0U;
}

_Noreturn void kernel_test_complete_boot_ledger(
    const struct boot_context *context
)
{
    const struct boot_ledger *ledger = boot_ledger_installed();
    const struct boot_stage_receipt *framebuffer_wc;
    const struct boot_stage_receipt *framebuffer_output;
    uint32_t device_windows;
    uint32_t paging_install;
    uint32_t paging_proofs;
    uint32_t interrupts;

    if (active_scenario != KERNEL_TEST_BOOT_LEDGER) {
        kernel_test_fail("Boot Ledger completion used outside its scenario");
    }

    if (context == NULL || ledger == NULL || !ledger->validated ||
        !ledger->executed || ledger->status != BOOT_LEDGER_STATUS_OK) {
        kernel_test_fail("the installed Boot Ledger is incomplete");
    }

    device_windows = boot_ledger_stage_sequence(ledger,
        BOOT_STAGE_DEVICE_WINDOWS);
    paging_install = boot_ledger_stage_sequence(ledger,
        BOOT_STAGE_PAGING_INSTALL);
    paging_proofs = boot_ledger_stage_sequence(ledger,
        BOOT_STAGE_PAGING_PROOFS);
    interrupts = boot_ledger_capability_sequence(ledger,
        BOOT_CAPABILITY_INTERRUPTS_ENABLED);

    for (size_t index = 0U; index < ledger->planned_count; ++index) {
        const struct boot_stage_descriptor *descriptor =
            boot_ledger_planned_stage_at(ledger, index);
        const struct boot_stage_receipt *receipt =
            boot_ledger_receipt_at(ledger, index);

        if (descriptor == NULL || receipt == NULL ||
            descriptor->id != receipt->stage_id ||
            receipt->sequence != index + 1U) {
            kernel_test_fail("validated plan and receipt order differ");
        }

        if (descriptor->required &&
            (receipt->result != BOOT_RECEIPT_RAN ||
             receipt->status != BOOT_LEDGER_STATUS_OK)) {
            kernel_test_fail("a mandatory Boot Ledger stage has no receipt");
        }

        if (receipt->result == BOOT_RECEIPT_RAN) {
            for (size_t requirement = 0U;
                 requirement < descriptor->required_capability_count;
                 ++requirement) {
                const uint32_t provider = boot_ledger_capability_sequence(
                    ledger, descriptor->required_capabilities[requirement]);

                if (provider == 0U || provider >= receipt->sequence) {
                    kernel_test_fail(
                        "a Boot Ledger dependency was ordered after its consumer"
                    );
                }
            }
        }
    }

    if (device_windows == 0U || paging_install <= device_windows ||
        paging_proofs <= paging_install ||
        boot_ledger_capability_sequence(ledger,
            BOOT_CAPABILITY_WRITE_XOR_EXECUTE_PROVED) != paging_proofs ||
        boot_ledger_capability_sequence(ledger,
            BOOT_CAPABILITY_INSTALLED_DEVICE_WINDOWS_PROVED) !=
                paging_proofs) {
        kernel_test_fail("paging receipts violate device-window proof order");
    }

    if (interrupts <= boot_ledger_stage_sequence(ledger,
            BOOT_STAGE_INTERRUPT_FOUNDATION) ||
        interrupts <= boot_ledger_stage_sequence(ledger,
            BOOT_STAGE_INTERRUPT_CONTROLLERS)) {
        kernel_test_fail("interrupt enable preceded its foundation");
    }

    framebuffer_wc = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_FRAMEBUFFER_WC);
    framebuffer_output = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_FRAMEBUFFER_OUTPUT);

    if (context->information.framebuffer.present) {
        if (framebuffer_wc == NULL || framebuffer_output == NULL ||
            framebuffer_wc->result != BOOT_RECEIPT_RAN ||
            framebuffer_output->result != BOOT_RECEIPT_RAN ||
            framebuffer_output->sequence <= framebuffer_wc->sequence) {
            kernel_test_fail("framebuffer output preceded independent WC proof");
        }
    } else if (boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_FRAMEBUFFER_WC_INDEPENDENTLY_PROVED) ||
        framebuffer_output == NULL ||
        framebuffer_output->result == BOOT_RECEIPT_RAN) {
        kernel_test_fail("absent framebuffer leaked a success capability");
    }

    if (!boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_BOOT_PROOFS_COMPLETE) ||
        !boot_ledger_fingerprint_valid(ledger)) {
        kernel_test_fail("installed Boot Ledger fingerprint is invalid");
    }

    console_write("ST LEDGER stages ");
    console_write_u64(ledger->planned_count);
    console_write(" receipts ");
    console_write_u64(ledger->receipt_count);
    console_write(" capabilities ");
    console_write_u64(ledger->established_capability_count);
    console_write(" skips ");
    console_write_u64(ledger->optional_skip_count);
    console_write(" fingerprint ");
    console_write_hex(ledger->fingerprint);
    console_putc('\n');
    kernel_test_pass();
}

static uint32_t phipia_proof_pixel(uint32_t x, uint32_t y)
{
    uint32_t pixel = 0U;
    struct surface *surface = screen_surface();

    if (surface == NULL ||
        surface_read_pixel(surface, x, y, &pixel) != SURFACE_STATUS_OK) {
        kernel_test_fail("Phipia cached-surface pixel read failed");
    }
    return pixel;
}

static void phipia_proof_process_ui(const char *failure)
{
    enum ui_status status = ui_process_events();

    if (status == UI_STATUS_OK) {
        status = ui_flush();
    }
    if (status != UI_STATUS_OK) {
        kernel_test_fail(failure);
    }
}

static void phipia_proof_settle_ui(const char *failure)
{
    if (!ui_animation_active()) {
        return;
    }
    if (timer_sleep_ns(UI_ANIM_DEFAULT_OPEN_NS + UI_ANIM_FRAME_NS) !=
            TIMER_STATUS_OK) {
        kernel_test_fail(failure);
    }
    phipia_proof_process_ui(failure);
    if (ui_animation_active()) {
        kernel_test_fail(failure);
    }
}

static void phipia_proof_inject_pointer(
    uint8_t flags,
    int32_t delta_x,
    int32_t delta_y,
    const char *failure
)
{
    const int8_t device_x = (int8_t)delta_x;
    const int8_t device_y = (int8_t)-delta_y;
    uint8_t packet_flags = flags;

    if (device_x < 0) {
        packet_flags |= UINT8_C(0x10);
    }
    if (device_y < 0) {
        packet_flags |= UINT8_C(0x20);
    }
    cpu_interrupt_enable();
    const enum pointer_status status = pointer_inject_packet(packet_flags,
        (uint8_t)device_x, (uint8_t)device_y);
    cpu_interrupt_disable();
    if (status != POINTER_STATUS_OK) {
        kernel_test_fail(failure);
    }
    phipia_proof_process_ui(failure);
}

static void phipia_proof_move_pointer(
    uint32_t target_x,
    uint32_t target_y,
    const char *failure
)
{
    for (uint32_t packets = 0U; packets < 64U; ++packets) {
        const struct pointer_state pointer = pointer_get_state();
        int32_t delta_x;
        int32_t delta_y;

        if (pointer.x == target_x && pointer.y == target_y) {
            return;
        }
        delta_x = (int32_t)target_x - (int32_t)pointer.x;
        delta_y = (int32_t)target_y - (int32_t)pointer.y;
        if (delta_x > 127) {
            delta_x = 127;
        } else if (delta_x < -127) {
            delta_x = -127;
        }
        if (delta_y > 127) {
            delta_y = 127;
        } else if (delta_y < -127) {
            delta_y = -127;
        }
        phipia_proof_inject_pointer(0U, delta_x, delta_y, failure);
    }
    kernel_test_fail("Phipia cursor did not reach its UI target");
}

static void phipia_proof_click_taskbar_item(
    size_t item_index,
    enum ui_panel_id expected_panel
)
{
    struct ui_rect bounds;
    struct taskbar_counters counters;
    const struct ui_state *ui;
    uint32_t title_width;

    if (taskbar_app_bounds(item_index, &bounds) != TASKBAR_STATUS_OK ||
            bounds.width == 0U || bounds.height == 0U) {
        kernel_test_fail("Phipia taskbar application bounds are unavailable");
    }
    counters = taskbar_get_counters();
    const uint32_t target_x = bounds.x + bounds.width / 2U;
    const uint32_t target_y = bounds.y + bounds.height / 2U;
    phipia_proof_move_pointer(target_x, target_y,
        "Phipia real pointer movement failed");
    if (taskbar_get_counters().hover_changes <= counters.hover_changes) {
        kernel_test_fail("Phipia taskbar hover state is incorrect");
    }

    phipia_proof_inject_pointer(UINT8_C(0x01), 0, 0,
        "Phipia pointer press failed");
    phipia_proof_inject_pointer(0U, 0, 0,
        "Phipia pointer release failed");
    ui = ui_get_state();
    if (ui->active_panel != expected_panel ||
            phipia_proof_pixel(target_x, target_y) == 0U) {
        kernel_test_fail("Phipia taskbar activation is incorrect");
    }
    if (ui_font_text_width(ui_panel_name(expected_panel), &title_width) !=
            UI_FONT_STATUS_OK || title_width == 0U) {
        kernel_test_fail("Phipia panel title width is unavailable");
    }
}

static void phipia_proof_click_point(
    uint32_t target_x,
    uint32_t target_y,
    const char *failure
)
{
    phipia_proof_move_pointer(target_x, target_y, failure);
    phipia_proof_inject_pointer(UINT8_C(0x01), 0, 0, failure);
    phipia_proof_inject_pointer(0U, 0, 0, failure);
}

_Noreturn void kernel_test_complete_phipia_proof(void)
{
    static const enum ui_element_id ids[UI_DOCK_ITEM_COUNT] = {
        UI_ELEMENT_DOCK_FILES, UI_ELEMENT_DOCK_TERMINAL,
        UI_ELEMENT_DOCK_NOTES, UI_ELEMENT_DOCK_MEDIA_EDITOR,
        UI_ELEMENT_DOCK_CAMERA, UI_ELEMENT_DOCK_CANVAS,
        UI_ELEMENT_DOCK_STORE,
        UI_ELEMENT_DOCK_SETTINGS
    };
    static const enum ui_action actions[UI_DOCK_ITEM_COUNT] = {
        UI_ACTION_OPEN_FILES, UI_ACTION_OPEN_TERMINAL,
        UI_ACTION_OPEN_NOTES, UI_ACTION_OPEN_MEDIA_EDITOR,
        UI_ACTION_OPEN_CAMERA, UI_ACTION_OPEN_CANVAS,
        UI_ACTION_OPEN_STORE,
        UI_ACTION_OPEN_SETTINGS
    };
    static const enum ui_panel_id panels[UI_DOCK_ITEM_COUNT] = {
        UI_PANEL_FILES, UI_PANEL_TERMINAL, UI_PANEL_NOTES, UI_PANEL_MEDIA_EDITOR,
        UI_PANEL_CAMERA, UI_PANEL_PAINT, UI_PANEL_STORE, UI_PANEL_SETTINGS
    };
    const struct boot_ledger *ledger = boot_ledger_installed();
    const struct boot_stage_receipt *font;
    const struct boot_stage_receipt *layout;
    const struct boot_stage_receipt *construction;
    const struct boot_stage_receipt *activation;
    const struct boot_stage_receipt *proof_receipt;
    const struct boot_stage_receipt *wc;
    const struct ui_state *ui = ui_get_state();
    const struct ui_render_counters initial_renders = ui->renders;
    struct ui_point trail_probe;
    uint32_t dock_rim;
    uint32_t dock_rim_x;
    uint32_t dock_rim_y;
    uint32_t trail_under;
    struct ui_proof proof;
    enum ui_status proof_status;

    if (active_scenario != KERNEL_TEST_PHIPIA_PROOF) {
        kernel_test_fail("Phipia completion used outside its scenario");
    }
    if (ledger == NULL || !ledger->validated || !ledger->executed ||
        ledger->status != BOOT_LEDGER_STATUS_OK || ledger->degraded ||
        !boot_ledger_fingerprint_valid(ledger)) {
        kernel_test_fail("Phipia installed ledger is invalid");
    }
    font = boot_ledger_receipt_for(ledger, BOOT_STAGE_UI_FONT);
    layout = boot_ledger_receipt_for(ledger, BOOT_STAGE_UI_LAYOUT);
    construction = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_DESKTOP_CONSTRUCTION);
    activation = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_DESKTOP_ACTIVATION);
    proof_receipt = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_PHIPIA_INSTALLED_PROOF);
    wc = boot_ledger_receipt_for(ledger, BOOT_STAGE_FRAMEBUFFER_WC);
    if (font == NULL || layout == NULL || construction == NULL ||
        activation == NULL || proof_receipt == NULL || wc == NULL ||
        font->result != BOOT_RECEIPT_RAN ||
        layout->result != BOOT_RECEIPT_RAN ||
        construction->result != BOOT_RECEIPT_RAN ||
        activation->result != BOOT_RECEIPT_RAN ||
        proof_receipt->result != BOOT_RECEIPT_RAN ||
        wc->result != BOOT_RECEIPT_RAN) {
        kernel_test_fail("Phipia required stage receipt is missing");
    }
    if (wc->sequence >= construction->sequence ||
        wc->sequence >= activation->sequence ||
        construction->sequence >= activation->sequence ||
        activation->sequence >= proof_receipt->sequence) {
        kernel_test_fail("Phipia desktop present preceded its WC proof");
    }
    if (!boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_UI_FONT_VERIFIED) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_UI_LAYOUT_VALIDATED) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_DESKTOP_SHELL_ACTIVATED) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_PHIPIA_INSTALLED_PROOF_COMPLETE)) {
        kernel_test_fail("Phipia installed capability is missing");
    }
    if (!ui->active || !ui->pointer_present || !ui->ledger_pass ||
        !pointer_is_present() || ui->layout.surface.width != 1024U ||
        ui->layout.surface.height != 768U) {
        kernel_test_fail("Phipia installed UI state is incomplete");
    }
    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT; ++index) {
        const struct ui_dock_item *item = &ui->layout.dock_items[index];

        if (item->id != ids[index] || item->action != actions[index] ||
            item->panel != panels[index]) {
            kernel_test_fail("Phipia dock typed action is incorrect");
        }
    }

    const uint32_t wallpaper_probe = phipia_proof_pixel(512U, 250U);
    if (wallpaper_probe == 0U) {
        kernel_test_fail("Phipia wallpaper probe is empty");
    }
    if (phipia_proof_pixel(ui->layout.menu_bar.x,
            ui->layout.menu_bar.y) == wallpaper_probe) {
        kernel_test_fail("Phipia menu bar is not integrated");
    }
    /*
     * The native 3D shelf has a centre-hot blended specular line rather than
     * the old flat white rectangle.  Derive its back edge from the resting
     * icon baseline and prove the line is visibly distinct on both sides.
     */
    dock_rim_x = ui->layout.surface.width / 2U;
    dock_rim_y = ui->layout.dock_items[0U].icon_bounds.y +
        ui->layout.dock_items[0U].icon_bounds.height;
    if (dock_rim_y == 0U || dock_rim_y + 1U >=
            ui->layout.surface.height) {
        kernel_test_fail("Phipia dock rim geometry is invalid");
    }
    dock_rim = phipia_proof_pixel(dock_rim_x, dock_rim_y);
    if (dock_rim == 0U ||
        dock_rim == phipia_proof_pixel(dock_rim_x, dock_rim_y - 1U) ||
        dock_rim == phipia_proof_pixel(dock_rim_x, dock_rim_y + 1U)) {
        kernel_test_fail("Phipia dock rim is not integrated");
    }

    trail_under = phipia_proof_pixel(20U, 100U);
    phipia_proof_move_pointer(20U, 100U,
        "Phipia cursor trail probe movement failed");
    ui = ui_get_state();
    trail_probe = ui->pointer;
    if (phipia_proof_pixel((uint32_t)trail_probe.x,
            (uint32_t)trail_probe.y) == trail_under) {
        kernel_test_fail("Phipia cursor trail probe is not visible");
    }

    /* Exercise the launcher a person can actually see: open the taskbar's
     * search box, launch Store as the best match, reopen it, then filter and
     * launch Paint. */
    {
        struct ui_rect first_app;
        const struct ui_rect bar = taskbar_bounds();
        struct keyboard_event search_key = {
            .scancode = 0x1CU, .pressed = true, .shift = false,
            .control = false, .character = '\0'
        };
        uint32_t search_x;
        uint32_t search_y;

        if (taskbar_app_bounds(0U, &first_app) != TASKBAR_STATUS_OK ||
                first_app.x <= bar.x + 48U || bar.height == 0U) {
            kernel_test_fail("Phipia taskbar search geometry is unavailable");
        }
        search_x = bar.x + 48U + (first_app.x - (bar.x + 48U)) / 2U;
        search_y = bar.y + bar.height / 2U;
        phipia_proof_click_point(search_x, search_y,
            "Phipia taskbar search did not open");
        if (!taskbar_search_panel_open()) {
            kernel_test_fail("Phipia taskbar search is not open");
        }
        static const char store_query[] = "store";
        for (size_t index = 0U; index < sizeof(store_query) - 1U; ++index) {
            search_key.scancode = 0U;
            search_key.character = store_query[index];
            if (ui_handle_keyboard(&search_key) != UI_STATUS_OK) {
                kernel_test_fail("Phipia Store search failed");
            }
        }
        phipia_proof_process_ui("Phipia Store search draw failed");
        search_key.scancode = 0x1CU;
        search_key.character = '\0';
        if (ui_handle_keyboard(&search_key) != UI_STATUS_OK) {
            kernel_test_fail("Phipia Store search activation failed");
        }
        phipia_proof_process_ui("Phipia Store search activation draw failed");
        if (ui_get_state()->active_panel != UI_PANEL_STORE) {
            kernel_test_fail("Phipia search chose the wrong Store app");
        }
        phipia_proof_settle_ui(
            "Phipia Store search animation did not settle");
        {
            struct ui_rect action;
            char manifest[13U];

            if (store_primary_action_bounds(&action) != STORE_STATUS_OK ||
                    action.width == 0U || action.height == 0U) {
                kernel_test_fail("Phipia Store package action is unavailable");
            }
            phipia_proof_click_point(action.x + action.width / 2U,
                action.y + action.height / 2U,
                "Phipia Store package action did not activate");
            if (!ui_application_launch_dequeue(manifest, sizeof(manifest)) ||
                    manifest[0] != 'P' || manifest[1] != 'H' ||
                    manifest[2] != 'I' || manifest[3] != 'P' ||
                    manifest[4] != '.' || manifest[5] != 'M' ||
                    manifest[6] != 'A' || manifest[7] != 'N' ||
                    manifest[8] != '\0') {
                kernel_test_fail(
                    "Phipia Store did not queue its signed package client");
            }
            console_serial_write(
                "ST PHIPIA STORE signed package action passed\n");
        }
        phipia_proof_click_point(search_x, search_y,
            "Phipia taskbar search did not reopen");
        if (!taskbar_search_panel_open()) {
            kernel_test_fail("Phipia taskbar search did not reopen");
        }
        static const char paint_query[] = "paint";
        for (size_t index = 0U; index < sizeof(paint_query) - 1U; ++index) {
            search_key.scancode = 0U;
            search_key.character = paint_query[index];
            if (ui_handle_keyboard(&search_key) != UI_STATUS_OK) {
                kernel_test_fail("Phipia Paint search failed");
            }
        }
        phipia_proof_process_ui("Phipia Paint search draw failed");
        search_key.scancode = 0x1CU;
        search_key.character = '\0';
        if (ui_handle_keyboard(&search_key) != UI_STATUS_OK) {
            kernel_test_fail("Phipia Paint search activation failed");
        }
        phipia_proof_process_ui("Phipia Paint search activation draw failed");
        char manifest[13U];
        if (ui_get_state()->active_panel != UI_PANEL_PAINT ||
                ui_application_launch_dequeue(manifest, sizeof(manifest))) {
            kernel_test_fail("Phipia search chose the wrong Paint app");
        }
        phipia_proof_settle_ui(
            "Phipia Paint search animation did not settle");
    }

    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT; ++index) {
        const enum ui_panel_id expected_panel = panels[index];

        phipia_proof_click_taskbar_item(index, expected_panel);
        console_serial_write("ST PHIPIA TASKBAR app ");
        console_serial_write_u64(index);
        console_serial_write(" active\n");
        phipia_proof_settle_ui(
            "Phipia taskbar application animation did not settle");
        console_serial_write("ST PHIPIA TASKBAR app ");
        console_serial_write_u64(index);
        console_serial_write(" settled\n");
        ui = ui_get_state();
    }
    console_serial_write("ST PHIPIA TASKBAR applications passed\n");
    if (trail_probe.x < 0 || trail_probe.y < 0 ||
        phipia_proof_pixel((uint32_t)trail_probe.x,
            (uint32_t)trail_probe.y) != trail_under ||
        ui->renders.cursor_moves <= initial_renders.cursor_moves ||
        ui->renders.damage_rectangles <= initial_renders.damage_rectangles) {
        kernel_test_fail("Phipia cursor damage left a trail");
    }

    struct keyboard_event keyboard = {
        .scancode = 0x01U, .pressed = true, .shift = false, .character = '\0'
    };
    if (ui_handle_keyboard(&keyboard) != UI_STATUS_OK) {
        kernel_test_fail("Phipia keyboard panel close failed");
    }
    phipia_proof_process_ui("Phipia keyboard panel close draw failed");
    phipia_proof_settle_ui(
        "Phipia keyboard panel close animation did not settle");
    console_serial_write("ST PHIPIA TASKBAR close passed\n");
    keyboard.scancode = 0x0FU;
    if (ui_handle_keyboard(&keyboard) != UI_STATUS_OK) {
        kernel_test_fail("Phipia keyboard focus-next failed");
    }
    phipia_proof_process_ui("Phipia keyboard focus-next draw failed");
    if (ui_get_state()->focus != UI_ELEMENT_DOCK_TERMINAL) {
        kernel_test_fail("Phipia keyboard focus-next chose wrong item");
    }
    keyboard.shift = true;
    if (ui_handle_keyboard(&keyboard) != UI_STATUS_OK) {
        kernel_test_fail("Phipia keyboard focus-previous failed");
    }
    phipia_proof_process_ui("Phipia keyboard focus-previous draw failed");
    if (ui_get_state()->focus != UI_ELEMENT_DOCK_FILES) {
        kernel_test_fail("Phipia keyboard focus-previous chose wrong item");
    }
    keyboard.scancode = 0x1CU;
    keyboard.shift = false;
    if (ui_handle_keyboard(&keyboard) != UI_STATUS_OK) {
        kernel_test_fail("Phipia keyboard activation failed");
    }
    phipia_proof_process_ui("Phipia keyboard activation draw failed");
    phipia_proof_settle_ui(
        "Phipia keyboard activation animation did not settle");
    if (ui_get_state()->active_panel != UI_PANEL_FILES) {
        kernel_test_fail("Phipia keyboard activation chose wrong panel");
    }
    console_serial_write("ST PHIPIA TASKBAR keyboard passed\n");

    if (!boot_plan_pointer_absence_self_test()) {
        kernel_test_fail("Phipia pointer-absence synthetic plan failed");
    }
    proof_status = ui_verify_installed(&proof);
    if (proof_status != UI_STATUS_OK) {
        kernel_test_fail(ui_installed_proof_failure());
    }
    if (proof.width != 1024U || proof.height != 768U ||
        proof.dock_items != UI_DOCK_ITEM_COUNT ||
        proof.ledger_fingerprint != ledger->fingerprint ||
        proof.render_hash == 0U) {
        kernel_test_fail("Phipia final installed shape is inconsistent");
    }
    if (proof.events == 0U || proof.panels < 3U ||
        proof.cursor_moves == 0U || proof.damage_rectangles == 0U ||
        proof.glyphs == 0U) {
        kernel_test_fail("Phipia final interaction counters are incomplete");
    }

    console_write("ST PHIPIA_PROOF geometry ");
    console_write_u64(proof.width);
    console_putc('x');
    console_write_u64(proof.height);
    console_write(" dock ");
    console_write_u64(proof.dock_items);
    console_write(" events ");
    console_write_u64(proof.events);
    console_write(" panels ");
    console_write_u64(proof.panels);
    console_write(" cursor ");
    console_write_u64(proof.cursor_moves);
    console_write(" damage ");
    console_write_u64(proof.damage_rectangles);
    console_write(" glyphs ");
    console_write_u64(proof.glyphs);
    console_write(" fingerprint ");
    console_write_hex(proof.ledger_fingerprint);
    console_putc('\n');
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_device_substrate(void)
{
    const struct boot_ledger *ledger = boot_ledger_installed();
    const struct boot_stage_receipt *receipt;
    const struct device_substrate_proof proof = device_substrate_get_proof();
    const size_t negative_controls = 4U + 4U + 2U + 2U +
        proof.negative_controls;

    if (active_scenario != KERNEL_TEST_DEVICE_SUBSTRATE) {
        kernel_test_fail("device-substrate completion used outside its scenario");
    }
    receipt = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_DEVICE_SUBSTRATE_PROOF);
    if (ledger == NULL || receipt == NULL ||
        receipt->result != BOOT_RECEIPT_RAN ||
        receipt->proof_counter_count != 2U ||
        receipt->proof_counters[0] != 1U ||
        receipt->proof_counters[1] != DEVICE_SUBSTRATE_DMA_BYTES ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_DEVICE_SUBSTRATE_INSTALLED_PROOF_COMPLETE) ||
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_DEVICE_SUBSTRATE_FIXTURE_ABSENT)) {
        kernel_test_fail("device-substrate installed receipt is invalid");
    }
    if (proof.queue_size == 0U || proof.used_before != 0U ||
        proof.used_after != 1U ||
        proof.used_length != DEVICE_SUBSTRATE_DMA_BYTES ||
        proof.interrupt_count != 1U ||
        proof.random_bytes != DEVICE_SUBSTRATE_DMA_BYTES ||
        proof.nonzero_bytes == 0U ||
        !proof.dma_device_written || !proof.msix_delivered ||
        !proof.ownership_round_trip || !proof.teardown_complete ||
        proof.negative_controls != 2U || negative_controls != 14U) {
        kernel_test_fail("device-substrate installed proof is inconsistent");
    }

    console_write("ST DEVICE_SUBSTRATE dma ");
    console_write_u64(proof.random_bytes);
    console_write(" msix ");
    console_write_u64(proof.interrupt_count);
    console_write(" used 0->1 ownership CPU-DEVICE-CPU teardown clean ");
    console_write("negatives ");
    console_write_u64(negative_controls);
    console_putc('\n');
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_xhci(void)
{
    const struct boot_ledger *ledger = boot_ledger_installed();
    const struct boot_stage_receipt *foundation;
    const struct boot_stage_receipt *receipt;
    const struct xhci_descriptor_proof proof = xhci_get_descriptor_proof();

    if (active_scenario != KERNEL_TEST_XHCI) {
        kernel_test_fail("xHCI completion used outside its scenario");
    }
    foundation = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_XHCI_FOUNDATION);
    receipt = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_XHCI_DESCRIPTOR_PROOF);
    if (ledger == NULL || foundation == NULL || receipt == NULL ||
        foundation->result != BOOT_RECEIPT_RAN ||
        receipt->result != BOOT_RECEIPT_RAN ||
        receipt->proof_counter_count != 2U ||
        receipt->proof_counters[0] != XHCI_DEVICE_DESCRIPTOR_BYTES ||
        receipt->proof_counters[1] != 1U ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_XHCI_FOUNDATION_AVAILABLE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_XHCI_DESCRIPTOR_PROOF_COMPLETE) ||
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_XHCI_FIXTURE_ABSENT)) {
        kernel_test_fail("xHCI installed receipt is invalid");
    }
    if (proof.descriptor_bytes != XHCI_DEVICE_DESCRIPTOR_BYTES ||
        proof.msix_completion_count != 1U ||
        proof.robustness_tests != XHCI_CONTROLLED_ROBUSTNESS_TESTS ||
        !proof.controller_ready || !proof.descriptor_valid ||
        !proof.sentinel_changed_while_controller_owned ||
        !proof.ownership_complete || !proof.teardown_complete) {
        kernel_test_fail("xHCI installed proof is inconsistent");
    }

    console_write("ST XHCI descriptor ");
    console_write_u64(proof.descriptor_bytes);
    console_write(" msix ");
    console_write_u64(proof.msix_completion_count);
    console_write(" ownership CPU-CONTROLLER-CPU teardown clean robustness ");
    console_write_u64(proof.robustness_tests);
    console_putc('\n');
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_nvme(void)
{
    const struct boot_ledger *ledger = boot_ledger_installed();
    const struct boot_stage_receipt *foundation;
    const struct boot_stage_receipt *receipt;
    const struct nvme_read_proof proof = nvme_get_read_proof();

    if (active_scenario != KERNEL_TEST_NVME) {
        kernel_test_fail("NVMe completion used outside its scenario");
    }
    foundation = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_NVME_FOUNDATION);
    receipt = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_NVME_READ_PROOF);
    if (ledger == NULL || foundation == NULL || receipt == NULL ||
        foundation->result != BOOT_RECEIPT_RAN ||
        receipt->result != BOOT_RECEIPT_RAN ||
        receipt->proof_counter_count != 2U ||
        receipt->proof_counters[0] != NVME_BLOCK_BYTES ||
        receipt->proof_counters[1] != 1U ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_NVME_FOUNDATION_AVAILABLE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_NVME_READ_PROOF_COMPLETE) ||
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_NVME_FIXTURE_ABSENT)) {
        kernel_test_fail("NVMe installed receipt is invalid");
    }
    if (proof.block_bytes != NVME_BLOCK_BYTES ||
        proof.msix_completion_count != 1U ||
        proof.ignored_completions != 0U ||
        proof.robustness_tests != NVME_CONTROLLED_ROBUSTNESS_TESTS ||
        !proof.controller_ready || !proof.namespace_ready ||
        !proof.contents_valid || !proof.sentinel_valid ||
        !proof.changed_while_controller_owned ||
        !proof.ownership_complete || !proof.teardown_complete) {
        kernel_test_fail("NVMe installed proof is inconsistent");
    }

    console_write("ST NVME read ");
    console_write_u64(proof.block_bytes);
    console_write(" msix ");
    console_write_u64(proof.msix_completion_count);
    console_write(" ownership CPU-CONTROLLER-CPU teardown clean robustness ");
    console_write_u64(proof.robustness_tests);
    console_putc('\n');
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_filesystem(void)
{
    static const uint8_t expected_name[FAT16_CANONICAL_NAME_BYTES] =
        {'P', 'H', 'I', 'P', 'I', 'A', ' ', ' ', 'B', 'I', 'N'};
    const struct boot_ledger *ledger = boot_ledger_installed();
    const struct boot_stage_receipt *foundation;
    const struct boot_stage_receipt *receipt;
    const struct filesystem_file_proof proof = filesystem_get_file_proof();

    if (active_scenario != KERNEL_TEST_FILESYSTEM) {
        kernel_test_fail("filesystem completion used outside its scenario");
    }
    foundation = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_FAT16_FOUNDATION);
    receipt = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_FILESYSTEM_FILE_PROOF);
    if (ledger == NULL || foundation == NULL || receipt == NULL ||
        foundation->result != BOOT_RECEIPT_RAN ||
        receipt->result != BOOT_RECEIPT_RAN ||
        receipt->proof_counter_count != 2U ||
        receipt->proof_counters[0] != FAT16_FILE_BYTES ||
        receipt->proof_counters[1] != 4U ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_FAT16_FOUNDATION_AVAILABLE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_FILESYSTEM_FILE_PROOF_COMPLETE) ||
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_FILESYSTEM_FIXTURE_ABSENT)) {
        kernel_test_fail("filesystem installed receipt is invalid");
    }
    for (size_t index = 0U; index < sizeof(expected_name); ++index) {
        if (proof.canonical_name[index] != expected_name[index]) {
            kernel_test_fail("filesystem canonical name is invalid");
        }
    }
    if (proof.file_bytes != FAT16_FILE_BYTES || proof.read_count != 4U ||
        proof.msix_completion_count != 4U ||
        proof.ignored_completions != 0U ||
        proof.robustness_tests != FILESYSTEM_CONTROLLED_ROBUSTNESS_TESTS ||
        !proof.fat16_ready || !proof.file_located || !proof.contents_valid ||
        !proof.sentinel_valid || !proof.changed_while_controller_owned ||
        !proof.ownership_complete || !proof.teardown_complete) {
        kernel_test_fail("filesystem installed proof is inconsistent");
    }

    kernel_test_pass();
}

_Noreturn void kernel_test_complete_process(void)
{
    const struct boot_ledger *ledger = boot_ledger_installed();
    const struct boot_stage_receipt *address_space;
    const struct boot_stage_receipt *elf64;
    const struct boot_stage_receipt *receipt;
    const struct process_proof_result proof = process_get_proof_result();

    if (active_scenario != KERNEL_TEST_PROCESS) {
        kernel_test_fail("process completion used outside its scenario");
    }
    address_space = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_PROCESS_ADDRESS_SPACE_FOUNDATION);
    elf64 = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_ELF64_LOADER_FOUNDATION);
    receipt = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_PROCESS_INSTALLED_PROOF);
    if (ledger == NULL || address_space == NULL || elf64 == NULL ||
        receipt == NULL || address_space->result != BOOT_RECEIPT_RAN ||
        elf64->result != BOOT_RECEIPT_RAN ||
        receipt->result != BOOT_RECEIPT_RAN ||
        receipt->proof_counter_count != 2U ||
        receipt->proof_counters[0] != 128U ||
        receipt->proof_counters[1] != 1U ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_PROCESS_ADDRESS_SPACE_FOUNDATION_AVAILABLE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_ELF64_LOADER_FOUNDATION_AVAILABLE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_PROCESS_INSTALLED_PROOF_COMPLETE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_PROCESS_OUTCOME_DECIDED) ||
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_PROCESS_FIXTURE_ABSENT)) {
        kernel_test_fail("process installed receipt is invalid");
    }
    if (proof.file_bytes != 128U || proof.segment_count != 1U ||
        proof.result != UINT32_C(0x53415037) ||
        proof.robustness_tests != PROCESS_CONTROLLED_ROBUSTNESS_TESTS ||
        !proof.ring_three || !proof.private_address_space ||
        !proof.image_read_execute || !proof.stack_read_write_no_execute ||
        !proof.guard_unmapped || !proof.interrupt_authenticated ||
        !proof.normal_exit || !proof.teardown_complete ||
        !proof.resource_census_equal || !process_resources_released()) {
        kernel_test_fail("process installed proof is inconsistent");
    }
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_linux_abi(void)
{
    const struct boot_ledger *ledger = boot_ledger_installed();
    const struct boot_stage_receipt *syscall_cpu;
    const struct boot_stage_receipt *image_stack;
    const struct boot_stage_receipt *receipt;
    const struct linux_abi_proof_result proof =
        linux_abi_get_proof_result();

    if (active_scenario != KERNEL_TEST_LINUX_ABI) {
        kernel_test_fail("Linux ABI completion used outside its scenario");
    }
    syscall_cpu = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_LINUX_SYSCALL_CPU_FOUNDATION);
    image_stack = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_LINUX_IMAGE_STACK_FOUNDATION);
    receipt = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_LINUX_INSTALLED_PROOF);
    if (ledger == NULL || syscall_cpu == NULL || image_stack == NULL ||
        receipt == NULL || syscall_cpu->result != BOOT_RECEIPT_RAN ||
        image_stack->result != BOOT_RECEIPT_RAN ||
        receipt->result != BOOT_RECEIPT_RAN ||
        receipt->proof_counter_count != 2U ||
        receipt->proof_counters[0] != LINUX_ABI_IMAGE_BYTES ||
        receipt->proof_counters[1] != 9U ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_LINUX_SYSCALL_CPU_FOUNDATION_AVAILABLE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_LINUX_IMAGE_STACK_FOUNDATION_AVAILABLE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_LINUX_INSTALLED_PROOF_COMPLETE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_LINUX_OUTCOME_DECIDED) ||
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_LINUX_FIXTURE_ABSENT)) {
        kernel_test_fail("Linux ABI installed receipt is invalid");
    }
    if (proof.file_bytes != LINUX_ABI_IMAGE_BYTES ||
        proof.program_headers != 5U ||
        proof.load_segments != 4U || proof.file_clusters != 9U ||
        proof.stdout_bytes != 7U || proof.syscall_count != 9U ||
        proof.distinct_syscalls != 7U || proof.exit_status != 0U ||
        proof.robustness_tests != LINUX_ABI_CONTROLLED_ROBUSTNESS_TESTS ||
        !proof.ring_three || !proof.private_address_space ||
        !proof.real_syscall_instruction || !proof.stdout_valid ||
        !proof.exit_zero || !proof.unknown_enosys ||
        !proof.write_xor_execute || !proof.kernel_cr3_restored ||
        !proof.teardown_complete || !proof.resource_census_equal ||
        !linux_abi_resources_released()) {
        kernel_test_fail("Linux ABI installed proof is inconsistent");
    }
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_linux_uname(void)
{
    const struct boot_ledger *ledger = boot_ledger_installed();
    const struct boot_stage_receipt *syscall_cpu;
    const struct boot_stage_receipt *uname_foundation;
    const struct boot_stage_receipt *receipt;
    const struct linux_uname_abi_proof_result proof =
        linux_uname_abi_get_proof_result();

    if (active_scenario != KERNEL_TEST_LINUX_ABI_UNAME) {
        kernel_test_fail("Linux uname completion used outside its scenario");
    }
    syscall_cpu = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_LINUX_SYSCALL_CPU_FOUNDATION);
    uname_foundation = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_LINUX_UNAME_IMAGE_UTS_FOUNDATION);
    receipt = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_LINUX_UNAME_INSTALLED_PROOF);
    if (ledger == NULL || syscall_cpu == NULL || uname_foundation == NULL ||
        receipt == NULL || syscall_cpu->result != BOOT_RECEIPT_RAN ||
        uname_foundation->result != BOOT_RECEIPT_RAN ||
        receipt->result != BOOT_RECEIPT_RAN ||
        receipt->proof_counter_count != 2U ||
        receipt->proof_counters[0] != LINUX_UNAME_ABI_IMAGE_BYTES ||
        receipt->proof_counters[1] != 6U ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_LINUX_SYSCALL_CPU_FOUNDATION_AVAILABLE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_LINUX_UNAME_IMAGE_UTS_FOUNDATION_AVAILABLE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_LINUX_UNAME_INSTALLED_PROOF_COMPLETE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_LINUX_UNAME_OUTCOME_DECIDED) ||
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_LINUX_UNAME_FIXTURE_ABSENT)) {
        kernel_test_fail("Linux uname installed receipt is invalid");
    }
    if (proof.file_bytes != LINUX_UNAME_ABI_IMAGE_BYTES ||
        proof.program_headers != 5U || proof.load_segments != 4U ||
        proof.file_clusters != 10U || proof.stdout_bytes != 6U ||
        proof.syscall_count != 6U || proof.distinct_syscalls != 6U ||
        proof.exit_status != 0U ||
        proof.robustness_tests !=
            LINUX_UNAME_ABI_CONTROLLED_ROBUSTNESS_TESTS ||
        !proof.ring_three || !proof.private_address_space ||
        !proof.real_syscall_instruction || !proof.uts_copy_valid ||
        !proof.stdout_valid || !proof.exit_zero || !proof.unknown_enosys ||
        !proof.write_xor_execute || !proof.kernel_cr3_restored ||
        !proof.teardown_complete || !proof.resource_census_equal ||
        !linux_uname_abi_resources_released()) {
        kernel_test_fail("Linux uname installed proof is inconsistent");
    }
    kernel_test_pass();
}

static bool feed_shell_line(const char *text)
{
    size_t index = 0U;

    if (text == NULL) {
        return false;
    }
    while (text[index] != '\0') {
        if (shell_feed(text[index]) != SHELL_STATUS_OK) {
            return false;
        }
        ++index;
    }
    return shell_feed('\n') == SHELL_STATUS_OK;
}

static uint8_t keyboard_scancode_for_ascii(char character)
{
    static const uint8_t letters[26] = {
        0x1EU, 0x30U, 0x2EU, 0x20U, 0x12U, 0x21U, 0x22U,
        0x23U, 0x17U, 0x24U, 0x25U, 0x26U, 0x32U, 0x31U,
        0x18U, 0x19U, 0x10U, 0x13U, 0x1FU, 0x14U, 0x16U,
        0x2FU, 0x11U, 0x2DU, 0x15U, 0x2CU
    };

    if (character >= 'a' && character <= 'z') {
        return letters[(size_t)(character - 'a')];
    }
    if (character == ' ') {
        return UINT8_C(0x39);
    }
    if (character == '\n') {
        return UINT8_C(0x1C);
    }
    return 0U;
}

static bool inject_keyboard_byte(uint8_t byte)
{
    const struct keyboard_state before = keyboard_get_state();
    enum keyboard_status status;

    if (!cpu_interrupts_enabled()) {
        console_serial_write(
            "ST interactive keyboard injection refused: interrupts clear\n");
        return false;
    }
    status = keyboard_inject_scancode(byte);
    if (status != KEYBOARD_STATUS_OK) {
        console_serial_write(
            "ST interactive keyboard injection controller refusal: ");
        console_serial_write(keyboard_status_string(status));
        console_serial_write("\n");
        return false;
    }
    for (uint64_t spins = 0U; spins < UINT64_C(200000000); ++spins) {
        const struct keyboard_state now = keyboard_get_state();

        if (now.events > before.events || now.dropped > before.dropped) {
            return now.events == before.events + 1U &&
                now.dropped == before.dropped;
        }
    }
    const struct keyboard_state after = keyboard_get_state();

    console_serial_write(
        "ST interactive keyboard injection delivery timeout byte ");
    console_serial_write_u64(byte);
    console_serial_write(" interrupts ");
    console_serial_write_u64(after.interrupts - before.interrupts);
    console_serial_write(" events ");
    console_serial_write_u64(after.events - before.events);
    console_serial_write(" dropped ");
    console_serial_write_u64(after.dropped - before.dropped);
    console_serial_write(" queued ");
    console_serial_write_u64(after.queued);
    console_serial_write("\n");
    return false;
}

static bool inject_keyboard_text(const char *text)
{
    size_t index = 0U;

    if (text == NULL) {
        return false;
    }
    while (text[index] != '\0') {
        const uint8_t scancode = keyboard_scancode_for_ascii(text[index]);

        if (scancode == 0U || !inject_keyboard_byte(scancode)) {
            return false;
        }
        shell_process_keyboard_events();
        if (!inject_keyboard_byte(
                (uint8_t)(scancode | UINT8_C(0x80)))) {
            return false;
        }
        shell_process_keyboard_events();
        ++index;
    }
    return true;
}

static bool inject_keyboard_ctrl_d(void)
{
    static const uint8_t sequence[] = {
        UINT8_C(0x1D), UINT8_C(0x20), UINT8_C(0xA0), UINT8_C(0x9D)
    };

    for (size_t index = 0U;
         index < sizeof(sequence) / sizeof(sequence[0]); ++index) {
        if (!inject_keyboard_byte(sequence[index])) {
            return false;
        }
        shell_process_keyboard_events();
    }
    if (keyboard_get_state().control) {
        return false;
    }
    return true;
}

static bool focus_phipia_proof_terminal(void)
{
    if (ui_get_state()->active_panel == UI_PANEL_TERMINAL) {
        return true;
    }
    for (size_t attempts = 0U;
         attempts < UI_DOCK_ITEM_COUNT &&
            ui_get_state()->focus != UI_ELEMENT_DOCK_TERMINAL;
         ++attempts) {
        if (!inject_keyboard_byte(UINT8_C(0x0F))) {
            return false;
        }
        shell_process_keyboard_events();
        phipia_proof_process_ui(
            "interactive terminal focus-next processing failed");
        if (!inject_keyboard_byte(UINT8_C(0x8F))) {
            return false;
        }
        shell_process_keyboard_events();
        phipia_proof_process_ui(
            "interactive terminal focus release processing failed");
    }
    if (ui_get_state()->focus != UI_ELEMENT_DOCK_TERMINAL) {
        return false;
    }
    if (!inject_keyboard_byte(UINT8_C(0x1C))) {
        return false;
    }
    shell_process_keyboard_events();
    phipia_proof_process_ui(
        "interactive terminal activation processing failed");
    if (!inject_keyboard_byte(UINT8_C(0x9C))) {
        return false;
    }
    shell_process_keyboard_events();
    phipia_proof_process_ui(
        "interactive terminal activation release processing failed");
    return ui_get_state()->active_panel == UI_PANEL_TERMINAL;
}

static bool installed_phipia_proof_ready(void)
{
    const struct boot_ledger *ledger = boot_ledger_installed();

    return ledger != NULL && ledger->validated && ledger->executed &&
        ledger->status == BOOT_LEDGER_STATUS_OK && !ledger->degraded &&
        boot_ledger_fingerprint_valid(ledger) &&
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_PHIPIA_INSTALLED_PROOF_COMPLETE) &&
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_LINUX_SYSCALL_CPU_FOUNDATION_AVAILABLE) &&
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_LINUX_IMAGE_STACK_FOUNDATION_AVAILABLE) &&
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_LINUX_UNAME_IMAGE_UTS_FOUNDATION_AVAILABLE) &&
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_LINUX_CAT_IMAGE_STDIN_FOUNDATION_AVAILABLE);
}

_Noreturn void kernel_test_complete_phipia_proof_userland(void)
{
    const struct shell_state before = shell_get_state();
    const uint32_t echo_before =
        linux_userland_completed(LINUX_USERLAND_PROFILE_ECHO);
    const uint32_t uname_before =
        linux_userland_completed(LINUX_USERLAND_PROFILE_UNAME);
    struct linux_abi_proof_result echo;
    struct linux_uname_abi_proof_result uname;

    if (active_scenario != KERNEL_TEST_PHIPIA_PROOF_USERLAND ||
        !installed_phipia_proof_ready() || !shell_is_active()) {
        kernel_test_fail("Phipia userspace prerequisites are incomplete");
    }
    cpu_interrupt_enable();
    console_write("\n");
    console_write("phip> ");
    if (!feed_shell_line("linux unsupported") ||
        !feed_shell_line("echo native") ||
        !feed_shell_line("linux echo") ||
        !feed_shell_line("linux uname") ||
        !feed_shell_line("linux echo") ||
        !feed_shell_line("linux uname")) {
        kernel_test_fail("Phipia shell input injection was refused");
    }
    echo = linux_abi_get_proof_result();
    uname = linux_uname_abi_get_proof_result();
    if (shell_get_state().commands != before.commands + 6U ||
        shell_get_state().lines != before.lines + 6U ||
        shell_get_state().unknown != before.unknown ||
        linux_userland_completed(LINUX_USERLAND_PROFILE_ECHO) !=
            echo_before + 2U ||
        linux_userland_completed(LINUX_USERLAND_PROFILE_UNAME) !=
            uname_before + 2U ||
        echo.file_bytes != LINUX_ABI_IMAGE_BYTES || echo.stdout_bytes != 7U ||
        echo.syscall_count != 9U || !echo.ring_three ||
        !echo.private_address_space || !echo.real_syscall_instruction ||
        !echo.stdout_valid || !echo.exit_zero || !echo.teardown_complete ||
        uname.file_bytes != LINUX_UNAME_ABI_IMAGE_BYTES ||
        uname.stdout_bytes != 6U || uname.syscall_count != 6U ||
        !uname.ring_three || !uname.private_address_space ||
        !uname.real_syscall_instruction || !uname.uts_copy_valid ||
        !uname.stdout_valid || !uname.exit_zero || !uname.teardown_complete ||
        !linux_userland_resources_released() || !cpu_interrupts_enabled()) {
        kernel_test_fail("Phipia userspace relaunch contract failed");
    }
    console_write("\nST PHIPIA_PROOF_USERLAND shell production echo 2 uname 2 ");
    console_write("invalid-profile recovered CPL3 SYSCALL stdout exact exit 0 ");
    console_write("teardown clean prompt restored\n");
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_phipia_proof_userland_absent(void)
{
    const struct shell_state before = shell_get_state();
    const uint32_t echo_before =
        linux_userland_completed(LINUX_USERLAND_PROFILE_ECHO);

    if (active_scenario != KERNEL_TEST_PHIPIA_PROOF_USERLAND_ABSENT ||
        !installed_phipia_proof_ready() || !shell_is_active()) {
        kernel_test_fail("absent-volume userspace prerequisites are incomplete");
    }
    cpu_interrupt_enable();
    console_write("\n");
    console_write("phip> ");
    if (!feed_shell_line("linux echo") ||
        !feed_shell_line("echo still usable") ||
        shell_get_state().commands != before.commands + 2U ||
        shell_get_state().lines != before.lines + 2U ||
        shell_get_state().unknown != before.unknown ||
        linux_userland_completed(LINUX_USERLAND_PROFILE_ECHO) != echo_before ||
        !linux_userland_resources_released() || !cpu_interrupts_enabled()) {
        kernel_test_fail("absent userspace volume did not recover cleanly");
    }
    console_write("\nST PHIPIA_PROOF_USERLAND_ABSENT concise refusal prompt usable ");
    console_write("teardown clean\n");
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_phipia_proof_userland_interactive(void)
{
    const struct shell_state before = shell_get_state();
    const uint32_t cat_before =
        linux_userland_completed(LINUX_USERLAND_PROFILE_CAT);
    uint64_t first_generation;
    uint64_t second_generation;
    struct linux_cat_abi_proof_result proof;

    if (active_scenario != KERNEL_TEST_PHIPIA_PROOF_USERLAND_INTERACTIVE ||
        !installed_phipia_proof_ready() || !shell_is_active() ||
        !keyboard_is_initialized()) {
        kernel_test_fail("interactive userspace prerequisites are incomplete");
    }
    cpu_interrupt_enable();
    shell_process_keyboard_events();
    if (!focus_phipia_proof_terminal()) {
        kernel_test_fail("interactive scenario could not focus Terminal");
    }
    console_write("\n");
    console_write("phip> ");

    if (!inject_keyboard_text("linux cat\n") ||
        !linux_userland_foreground_waiting()) {
        kernel_test_fail("first cat launch did not wait for keyboard input");
    }
    first_generation = linux_userland_active_generation();
    if (first_generation == 0U || !inject_keyboard_text("pebble\n") ||
        !linux_userland_foreground_waiting() ||
        !inject_keyboard_ctrl_d() || linux_userland_foreground_waiting() ||
        !linux_userland_resources_released()) {
        kernel_test_fail("first interactive cat launch did not complete");
    }

    if (!inject_keyboard_text("linux cat\n") ||
        !linux_userland_foreground_waiting()) {
        kernel_test_fail("second cat launch did not wait for keyboard input");
    }
    second_generation = linux_userland_active_generation();
    if (second_generation == 0U || second_generation == first_generation ||
        !inject_keyboard_text("again\n") ||
        !linux_userland_foreground_waiting() ||
        !inject_keyboard_ctrl_d() || linux_userland_foreground_waiting()) {
        kernel_test_fail("second interactive cat launch did not complete");
    }

    proof = linux_cat_abi_get_proof_result();
    if (shell_get_state().commands != before.commands + 2U ||
        shell_get_state().lines != before.lines + 2U ||
        shell_get_state().unknown != before.unknown ||
        linux_userland_completed(LINUX_USERLAND_PROFILE_CAT) !=
            cat_before + 2U ||
        proof.file_bytes != LINUX_CAT_ABI_IMAGE_BYTES ||
        proof.stdout_bytes != 6U || proof.input_bytes != 6U ||
        proof.input_lines != 1U || proof.resume_count != 2U ||
        proof.syscall_count != 6U || proof.distinct_syscalls != 5U ||
        proof.robustness_tests != LINUX_CAT_ABI_RUNTIME_NEGATIVE_CONTROLS ||
        proof.generation != second_generation || !proof.ring_three ||
        !proof.private_address_space || !proof.real_syscall_instruction ||
        !proof.stdout_valid || !proof.exit_zero || !proof.eof_delivered ||
        !proof.kernel_cr3_restored || !proof.teardown_complete ||
        !proof.resource_census_equal || !linux_userland_resources_released() ||
        !cpu_interrupts_enabled()) {
        kernel_test_fail("interactive cat proof is inconsistent");
    }
    console_write("\nST PHIPIA_PROOF_USERLAND_INTERACTIVE cat 2 keyboard IRQ ");
    console_write("read SYSCALL copy-out resume write SYSCALL stdout exact ");
    console_write("EOF exit 0 teardown clean fresh generation prompt restored\n");
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_phipia_proof_userland_interactive_absent(
    void
)
{
    const struct shell_state before = shell_get_state();
    const uint32_t echo_before =
        linux_userland_completed(LINUX_USERLAND_PROFILE_ECHO);
    const uint32_t cat_before =
        linux_userland_completed(LINUX_USERLAND_PROFILE_CAT);
    struct linux_abi_proof_result echo;

    if (active_scenario !=
            KERNEL_TEST_PHIPIA_PROOF_USERLAND_INTERACTIVE_ABSENT ||
        !installed_phipia_proof_ready() || !shell_is_active() ||
        !keyboard_is_initialized()) {
        kernel_test_fail("interactive absent-profile prerequisites incomplete");
    }
    cpu_interrupt_enable();
    shell_process_keyboard_events();
    if (!focus_phipia_proof_terminal()) {
        kernel_test_fail("absent scenario could not focus Terminal");
    }
    console_write("\n");
    console_write("phip> ");
    if (!inject_keyboard_text("linux cat\n") ||
        linux_userland_foreground_waiting() ||
        !linux_userland_resources_released() ||
        !inject_keyboard_text("linux echo\n")) {
        kernel_test_fail("missing cat profile did not recover through keyboard");
    }
    echo = linux_abi_get_proof_result();
    if (shell_get_state().commands != before.commands + 2U ||
        shell_get_state().lines != before.lines + 2U ||
        shell_get_state().unknown != before.unknown ||
        linux_userland_completed(LINUX_USERLAND_PROFILE_CAT) != cat_before ||
        linux_userland_completed(LINUX_USERLAND_PROFILE_ECHO) !=
            echo_before + 1U ||
        echo.file_bytes != LINUX_ABI_IMAGE_BYTES || echo.stdout_bytes != 7U ||
        !echo.ring_three || !echo.real_syscall_instruction ||
        !echo.stdout_valid || !echo.exit_zero || !echo.teardown_complete ||
        !linux_userland_resources_released() || !cpu_interrupts_enabled()) {
        kernel_test_fail("missing cat profile recovery proof is inconsistent");
    }
    console_write("\nST PHIPIA_PROOF_USERLAND_INTERACTIVE_ABSENT cat missing ");
    console_write("echo valid keyboard IRQ refusal recoverable teardown clean ");
    console_write("prompt usable\n");
    kernel_test_pass();
}

static bool fat32_read_file(
    const char *path,
    uint8_t *buffer,
    size_t capacity,
    size_t *file_bytes
)
{
    phipfs_handle handle;
    struct phipfs_stat stat;
    size_t read_bytes = 0U;
    enum phipfs_status status;

    if (path == NULL || buffer == NULL || file_bytes == NULL ||
        phipfs_stat_path(PHIPFS_VOLUME_DATA, path, &stat) != PHIPFS_STATUS_OK ||
        stat.directory || stat.size > capacity ||
        phipfs_open(PHIPFS_VOLUME_DATA, path, PHIPFS_ACCESS_READ, &handle) !=
            PHIPFS_STATUS_OK) {
        return false;
    }
    status = phipfs_read(handle, buffer, capacity, &read_bytes);
    if (phipfs_close(handle) != PHIPFS_STATUS_OK) {
        return false;
    }
    *file_bytes = read_bytes;
    return status == PHIPFS_STATUS_OK && read_bytes == stat.size;
}

static bool fat32_file_equals(
    const char *path,
    const uint8_t *expected,
    size_t expected_bytes
)
{
    /* HTTP already uses a bounded 7 KiB parser frame.  Keep this serial,
     * test-only comparison buffer off the 16 KiB bootstrap stack so compiler
     * inlining decisions cannot turn a test assertion into a double fault.
     */
    static uint8_t buffer[4096];
    size_t file_bytes = 0U;

    if (expected == NULL || expected_bytes > sizeof(buffer) ||
        !fat32_read_file(path, buffer, sizeof(buffer), &file_bytes) ||
        file_bytes != expected_bytes) {
        return false;
    }
    for (size_t index = 0U; index < expected_bytes; ++index) {
        if (buffer[index] != expected[index]) {
            return false;
        }
    }
    return true;
}

static void fat32_feed(const char *line)
{
    if (!feed_shell_line(line)) {
        kernel_test_fail("Phipia refused a FAT32 command line");
    }
}

static void fat32_require_base(bool data_required)
{
    struct phipfs_drive_info system = phipfs_drive(PHIPFS_VOLUME_SYSTEM);
    struct phipfs_drive_info data = phipfs_drive(PHIPFS_VOLUME_DATA);

    if (!installed_phipia_proof_ready() || !shell_is_active() ||
        !system.present || !system.healthy || !system.mounted ||
        !system.read_only || system.volume_id != FAT32_SYSTEM_VOLUME_ID ||
        (data_required && (!data.present || !data.healthy || !data.mounted ||
            data.read_only || data.volume_id != FAT32_DATA_VOLUME_ID))) {
        kernel_test_fail("FAT32 scenario mount prerequisites are incomplete");
    }
}

static void fat32_system_scenario(void)
{
    const uint32_t echo_before =
        linux_userland_completed(LINUX_USERLAND_PROFILE_ECHO);
    const uint32_t uname_before =
        linux_userland_completed(LINUX_USERLAND_PROFILE_UNAME);

    fat32_require_base(true);
    fat32_feed("drives");
    fat32_feed("linux echo");
    fat32_feed("linux uname");
    if (linux_userland_completed(LINUX_USERLAND_PROFILE_ECHO) !=
            echo_before + 1U ||
        linux_userland_completed(LINUX_USERLAND_PROFILE_UNAME) !=
            uname_before + 1U || !linux_userland_resources_released()) {
        kernel_test_fail("FAT32 system executables did not complete");
    }
    console_write("\nST FAT32 SYSTEM authenticated echo uname FAT32 immutable\n");
}

static void fat32_data_scenario(void)
{
    static const uint8_t expected[] = "first cut\nsecond line\n";

    fat32_require_base(true);
    fat32_feed("write notes.txt \"first cut\"");
    fat32_feed("append notes.txt \"second line\"");
    fat32_feed("read notes.txt");
    fat32_feed("stat notes.txt");
    fat32_feed("sync");
    if (!fat32_file_equals("notes.txt", expected, sizeof(expected) - 1U)) {
        kernel_test_fail("FAT32 create/read/write contents changed");
    }
    console_write("\nST FAT32 DATA create read write append sync exact\n");
}

static void fat32_nested_scenario(void)
{
    static const uint8_t expected[] = "nested\n";
    struct phipfs_stat stat;

    fat32_require_base(true);
    fat32_feed("mkdir projects");
    fat32_feed("cd projects");
    fat32_feed("mkdir cuts");
    fat32_feed("write cuts/notes.txt \"nested\"");
    fat32_feed("ls cuts");
    fat32_feed("pwd");
    fat32_feed("cd ..");
    if (!fat32_file_equals("projects/cuts/notes.txt", expected,
            sizeof(expected) - 1U) ||
        phipfs_stat_path(PHIPFS_VOLUME_DATA, "projects/cuts", &stat) !=
            PHIPFS_STATUS_OK || !stat.directory) {
        kernel_test_fail("nested FAT32 traversal changed");
    }
    console_write("\nST FAT32 NESTED dot dotdot traversal enumeration exact\n");
}

static void fat32_growth_scenario(void)
{
    struct phipfs_stat stat;
    uint8_t buffer[1024];
    size_t bytes = 0U;

    fat32_require_base(true);
    fat32_feed("write growth.bin \"0123456789012345678901234567890123456789012345678901234567890123456789\"");
    for (size_t index = 1U; index < 8U; ++index) {
        fat32_feed("append growth.bin \"0123456789012345678901234567890123456789012345678901234567890123456789\"");
    }
    fat32_feed("stat growth.bin");
    if (phipfs_stat_path(PHIPFS_VOLUME_DATA, "growth.bin", &stat) !=
            PHIPFS_STATUS_OK || stat.size != 568U || stat.cluster_count != 2U ||
        !fat32_read_file("growth.bin", buffer, sizeof(buffer), &bytes) ||
        bytes != stat.size) {
        kernel_test_fail("multi-cluster FAT32 growth changed");
    }
    console_write("\nST FAT32 GROWTH bytes 568 clusters 2 contents readable\n");
}

static void fat32_random_scenario(void)
{
    static const uint8_t expected[] = "abcXYZ\nhi\n";

    fat32_require_base(true);
    fat32_feed("write random.txt \"abcdefghi\"");
    fat32_feed("writeat random.txt 3 \"XYZ\"");
    fat32_feed("read random.txt");
    if (!fat32_file_equals("random.txt", expected, sizeof(expected) - 1U)) {
        kernel_test_fail("random-access FAT32 overwrite changed");
    }
    console_write("\nST FAT32 RANDOM seek overwrite preserved surrounding bytes\n");
}

static void fat32_truncate_scenario(void)
{
    struct phipfs_stat stat;
    uint8_t buffer[800];
    size_t bytes = 0U;

    fat32_require_base(true);
    fat32_feed("touch trim.bin");
    fat32_feed("truncate trim.bin 648");
    fat32_feed("writeat trim.bin 0 \"prefix\"");
    fat32_feed("truncate trim.bin 100");
    if (phipfs_stat_path(PHIPFS_VOLUME_DATA, "trim.bin", &stat) !=
            PHIPFS_STATUS_OK || stat.size != 100U || stat.cluster_count != 1U) {
        kernel_test_fail("FAT32 truncation did not release its tail");
    }
    fat32_feed("truncate trim.bin 700");
    if (!fat32_read_file("trim.bin", buffer, sizeof(buffer), &bytes) ||
        bytes != 700U || buffer[0] != 'p' || buffer[6] != '\n') {
        kernel_test_fail("FAT32 truncation regrowth changed live data");
    }
    for (size_t index = 100U; index < bytes; ++index) {
        if (buffer[index] != 0U) {
            kernel_test_fail("truncated FAT32 bytes reappeared after growth");
        }
    }
    console_write("\nST FAT32 TRUNCATE release regrow zero tail exact\n");
}

static void fat32_rename_scenario(void)
{
    static const uint8_t expected[] = "move me\n";
    struct phipfs_stat stat;

    fat32_require_base(true);
    fat32_feed("mkdir a");
    fat32_feed("mkdir b");
    fat32_feed("mkdir a/child");
    if (phipfs_rename(PHIPFS_VOLUME_DATA, "a", "a/child/a") !=
            PHIPFS_STATUS_PATH) {
        kernel_test_fail("FAT32 accepted a directory move into itself");
    }
    fat32_feed("write a/note.txt \"move me\"");
    fat32_feed("mv a/note.txt b/moved.txt");
    fat32_feed("mv b archive");
    fat32_feed("touch conflict.txt");
    if (phipfs_rename(PHIPFS_VOLUME_DATA, "conflict.txt",
            "archive/moved.txt") != PHIPFS_STATUS_EXISTS) {
        kernel_test_fail("FAT32 rename conflict was not rejected");
    }
    fat32_feed("ls archive");
    if (phipfs_stat_path(PHIPFS_VOLUME_DATA, "a/note.txt", &stat) !=
            PHIPFS_STATUS_NOT_FOUND ||
        !fat32_file_equals("archive/moved.txt", expected,
            sizeof(expected) - 1U)) {
        kernel_test_fail("FAT32 rename or move changed ownership");
    }
    console_write("\nST FAT32 RENAME file move directory parent updated\n");
}

static void fat32_delete_scenario(void)
{
    struct phipfs_stat first;
    struct phipfs_stat second;

    fat32_require_base(true);
    fat32_feed("write first.bin \"one\"");
    if (phipfs_stat_path(PHIPFS_VOLUME_DATA, "first.bin", &first) !=
            PHIPFS_STATUS_OK) {
        kernel_test_fail("FAT32 deletion setup failed");
    }
    fat32_feed("rm first.bin");
    fat32_feed("write second.bin \"two\"");
    fat32_feed("mkdir kept");
    fat32_feed("write kept/live.txt \"live\"");
    if (phipfs_stat_path(PHIPFS_VOLUME_DATA, "second.bin", &second) !=
            PHIPFS_STATUS_OK || first.first_cluster != second.first_cluster ||
        phipfs_rmdir(PHIPFS_VOLUME_DATA, "kept") != PHIPFS_STATUS_NOT_EMPTY) {
        kernel_test_fail("FAT32 deletion did not reuse or protect ownership");
    }
    fat32_feed("rm kept/live.txt");
    fat32_feed("rm kept");
    console_write("\nST FAT32 DELETE cluster reused nonempty directory refused\n");
}

static void fat32_full_scenario(void)
{
    struct phipfs_stat stat;

    fat32_require_base(true);
    if (phipfs_drive(PHIPFS_VOLUME_DATA).free_bytes != 0U) {
        kernel_test_fail("full FAT32 fixture retained free clusters");
    }
    fat32_feed("write recovery.txt \"blocked\"");
    if (phipfs_stat_path(PHIPFS_VOLUME_DATA, "recovery.txt", &stat) !=
            PHIPFS_STATUS_OK || stat.size != 0U) {
        kernel_test_fail("full-volume refusal exposed partial contents");
    }
    fat32_feed("rm tiny.bin");
    fat32_feed("write recovery.txt \"recovered\"");
    fat32_feed("sync");
    if (phipfs_stat_path(PHIPFS_VOLUME_DATA, "recovery.txt", &stat) !=
            PHIPFS_STATUS_OK || stat.size != 10U) {
        kernel_test_fail("full FAT32 volume did not recover after deletion");
    }
    console_write("\nST FAT32 FULL refusal no leak deletion recovered\n");
}

static void fat32_unavailable_scenario(bool corrupt)
{
    struct phipfs_drive_info data = phipfs_drive(PHIPFS_VOLUME_DATA);
    const uint32_t before =
        linux_userland_completed(LINUX_USERLAND_PROFILE_ECHO);

    fat32_require_base(false);
    if (data.mounted || (corrupt ? (!data.present || data.healthy) :
            data.present)) {
        kernel_test_fail("unavailable FAT32 data volume state changed");
    }
    fat32_feed("drives");
    fat32_feed("linux echo");
    if (linux_userland_completed(LINUX_USERLAND_PROFILE_ECHO) != before + 1U) {
        kernel_test_fail("unavailable data volume blocked authenticated echo");
    }
    console_write(corrupt ?
        "\nST FAT32 CORRUPT refused session usable system executable valid\n" :
        "\nST FAT32 MISSING session usable system executable valid\n");
}

static void fat32_persistence_scenario(void)
{
    static const uint8_t expected[] = "first cut\nsecond line\n";
    struct phipfs_stat stat;
    enum phipfs_status status;

    fat32_require_base(true);
    status = phipfs_stat_path(PHIPFS_VOLUME_DATA, "projects/notes.txt", &stat);
    if (status == PHIPFS_STATUS_NOT_FOUND) {
        fat32_feed("mkdir projects");
        fat32_feed("write projects/notes.txt \"first cut\"");
        fat32_feed("append projects/notes.txt \"second line\"");
        fat32_feed("sync");
        if (!fat32_file_equals("projects/notes.txt", expected,
                sizeof(expected) - 1U) ||
            phipfs_unmount(PHIPFS_VOLUME_DATA) != PHIPFS_STATUS_OK) {
            kernel_test_fail("clean persistence write phase failed");
        }
        console_write("\nST FAT32 PERSISTENCE synchronized reboot phase\n");
        cpu_out8(UINT16_C(0x0064), UINT8_C(0xFE));
        kernel_test_fail("platform reset did not restart QEMU");
    }
    if (status != PHIPFS_STATUS_OK ||
        !fat32_file_equals("projects/notes.txt", expected,
            sizeof(expected) - 1U)) {
        kernel_test_fail("clean reboot did not retain FAT32 contents");
    }
    fat32_feed("read projects/notes.txt");
    console_write("\nST FAT32 PERSISTENCE clean reboot retained exact contents\n");
}

static void fat32_cache_scenario(void)
{
    uint8_t buffer[4096];
    size_t bytes = 0U;
    static const uint32_t offsets[] = {0U, 600U, 1200U, 1800U, 2400U, 3000U};

    fat32_require_base(true);
    fat32_feed("touch cache.bin");
    fat32_feed("truncate cache.bin 3072");
    fat32_feed("writeat cache.bin 0 \"A\"");
    fat32_feed("writeat cache.bin 600 \"B\"");
    fat32_feed("writeat cache.bin 1200 \"C\"");
    fat32_feed("writeat cache.bin 1800 \"D\"");
    fat32_feed("writeat cache.bin 2400 \"E\"");
    fat32_feed("writeat cache.bin 3000 \"F\"");
    fat32_feed("sync");
    if (!fat32_read_file("cache.bin", buffer, sizeof(buffer), &bytes) ||
        bytes != 3072U) {
        kernel_test_fail("FAT32 cache readback length changed");
    }
    for (size_t index = 0U; index < sizeof(offsets) / sizeof(offsets[0]);
         ++index) {
        if (buffer[offsets[index]] != (uint8_t)('A' + index) ||
            buffer[offsets[index] + 1U] != '\n') {
            kernel_test_fail("FAT32 cache eviction lost a write");
        }
    }
    console_write("\nST FAT32 CACHE six clusters eviction sync readback exact\n");
}

static void fat32_immutable_scenario(void)
{
    phipfs_handle handle;
    const uint32_t before =
        linux_userland_completed(LINUX_USERLAND_PROFILE_ECHO);

    fat32_require_base(true);
    fat32_feed("linux echo");
    if (phipfs_create(PHIPFS_VOLUME_SYSTEM, "ATTACK.TXT") !=
            PHIPFS_STATUS_READ_ONLY ||
        phipfs_open(PHIPFS_VOLUME_SYSTEM, "BUSYBOX", PHIPFS_ACCESS_WRITE,
            &handle) != PHIPFS_STATUS_READ_ONLY ||
        linux_userland_completed(LINUX_USERLAND_PROFILE_ECHO) != before + 1U) {
        kernel_test_fail("immutable FAT32 system volume accepted a write");
    }
    console_write("\nST FAT32 IMMUTABLE write refused below shell executable valid\n");
}

static void fat32_handles_scenario(void)
{
    phipfs_handle handles[PHIPFS_MAX_HANDLES];
    phipfs_handle extra;
    size_t bytes = 0U;
    uint8_t byte = 0U;

    fat32_require_base(true);
    fat32_feed("write handle.txt \"generation\"");
    if (phipfs_open(PHIPFS_VOLUME_DATA, "handle.txt", PHIPFS_ACCESS_READ,
            &handles[0]) != PHIPFS_STATUS_OK ||
        phipfs_write(handles[0], &byte, 1U, &bytes) != PHIPFS_STATUS_ACCESS ||
        phipfs_read(handles[0], NULL, 1U, &bytes) !=
            PHIPFS_STATUS_INVALID_ARGUMENT ||
        phipfs_unlink(PHIPFS_VOLUME_DATA, "handle.txt") != PHIPFS_STATUS_BUSY ||
        phipfs_close(handles[0]) != PHIPFS_STATUS_OK ||
        phipfs_close(handles[0]) != PHIPFS_STATUS_STALE_HANDLE ||
        phipfs_read(handles[0], &byte, 1U, &bytes) !=
            PHIPFS_STATUS_STALE_HANDLE) {
        kernel_test_fail("FAT32 handle generation controls changed");
    }
    for (size_t index = 0U; index < PHIPFS_MAX_HANDLES; ++index) {
        if (phipfs_open(PHIPFS_VOLUME_DATA, "handle.txt", PHIPFS_ACCESS_READ,
                &handles[index]) != PHIPFS_STATUS_OK) {
            kernel_test_fail("FAT32 handle table filled early");
        }
    }
    if (phipfs_open(PHIPFS_VOLUME_DATA, "handle.txt", PHIPFS_ACCESS_READ,
            &extra) != PHIPFS_STATUS_NO_HANDLES) {
        kernel_test_fail("FAT32 handle table exceeded its fixed bound");
    }
    for (size_t index = 0U; index < PHIPFS_MAX_HANDLES; ++index) {
        if (phipfs_close(handles[index]) != PHIPFS_STATUS_OK) {
            kernel_test_fail("FAT32 handle teardown leaked ownership");
        }
    }
    console_write("\nST FAT32 HANDLES generation stale double-close access bound clean\n");
}

_Noreturn void kernel_test_complete_fat32(void)
{
    if (active_scenario < KERNEL_TEST_FAT32_SYSTEM ||
        active_scenario > KERNEL_TEST_FAT32_HANDLES) {
        kernel_test_fail("FAT32 completion used outside its scenario");
    }
    cpu_interrupt_enable();
    console_write("\nphip> ");
    switch (active_scenario) {
    case KERNEL_TEST_FAT32_SYSTEM: fat32_system_scenario(); break;
    case KERNEL_TEST_FAT32_DATA: fat32_data_scenario(); break;
    case KERNEL_TEST_FAT32_NESTED: fat32_nested_scenario(); break;
    case KERNEL_TEST_FAT32_GROWTH: fat32_growth_scenario(); break;
    case KERNEL_TEST_FAT32_RANDOM: fat32_random_scenario(); break;
    case KERNEL_TEST_FAT32_TRUNCATE: fat32_truncate_scenario(); break;
    case KERNEL_TEST_FAT32_RENAME: fat32_rename_scenario(); break;
    case KERNEL_TEST_FAT32_DELETE: fat32_delete_scenario(); break;
    case KERNEL_TEST_FAT32_FULL: fat32_full_scenario(); break;
    case KERNEL_TEST_FAT32_CORRUPT: fat32_unavailable_scenario(true); break;
    case KERNEL_TEST_FAT32_MISSING: fat32_unavailable_scenario(false); break;
    case KERNEL_TEST_FAT32_PERSISTENCE: fat32_persistence_scenario(); break;
    case KERNEL_TEST_FAT32_CACHE: fat32_cache_scenario(); break;
    case KERNEL_TEST_FAT32_IMMUTABLE: fat32_immutable_scenario(); break;
    case KERNEL_TEST_FAT32_HANDLES: fat32_handles_scenario(); break;
    default: kernel_test_fail("unreachable FAT32 scenario");
    }
    kernel_test_pass();
}

#define NETWORK_TEST_OWNER UINT64_C(0x54455354)
#define NETWORK_TEST_GUEST UINT32_C(0x0A00020F)
#define NETWORK_TEST_MASK UINT32_C(0xFFFFFF00)
#define NETWORK_TEST_GATEWAY UINT32_C(0x0A000202)
#define NETWORK_TEST_DNS UINT32_C(0x0A000203)
#define NETWORK_TEST_HTTP UINT32_C(0x0A000214)

static const uint8_t network_welcome[] =
    "hello from the Phipia network\n";

static void network_require_device(void)
{
    const struct network_state state = network_get_state();

    if (!state.active || !state.device.present || !state.device.active ||
        !state.device.link_up || state.device.rx_queue_size == 0U ||
        state.device.tx_queue_size == 0U) {
        kernel_test_fail("production virtio-net device is not ready");
    }
}

static void network_require_dhcp(void)
{
    enum network_status status;

    network_require_device();
    status = network_start_dhcp(NETWORK_DEFAULT_OPERATION_TIMEOUT_NS);
    if (status != NETWORK_STATUS_OK) {
        kernel_test_fail(network_status_string(status));
    }
    const struct network_ipv4_configuration configuration =
        network_get_state().configuration;

    if (!configuration.configured ||
        configuration.source != NETWORK_CONFIGURATION_DHCP ||
        configuration.address != NETWORK_TEST_GUEST ||
        configuration.gateway != NETWORK_TEST_GATEWAY ||
        configuration.dns_server != NETWORK_TEST_DNS ||
        configuration.lease_expires_ns == 0U) {
        kernel_test_fail("DHCP configuration is not the fixture lease");
    }
}

static void network_require_static(void)
{
    network_require_device();
    if (network_configure_static(NETWORK_TEST_GUEST, NETWORK_TEST_MASK,
            NETWORK_TEST_GATEWAY, NETWORK_TEST_DNS) != NETWORK_STATUS_OK) {
        kernel_test_fail("static IPv4 configuration was refused");
    }
}

static bool network_bytes_equal(
    const uint8_t *left,
    const uint8_t *right,
    size_t length
)
{
    for (size_t index = 0U; index < length; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

static void network_syscall_http_download_scenario(void)
{
    const uint64_t request_address = PAGING_LINUX_STACK_BASE;
    const uint64_t response_address = request_address + UINT64_C(256);
    const uint64_t url_address = request_address + UINT64_C(512);
    const uint64_t path_address = request_address + UINT64_C(640);
    static const char url[] = "http://phipia.test/welcome.txt";
    static const char destination[] = "HTTPLEN.TXT";
    uintptr_t executable_frame = 0U;
    uintptr_t data_frame = 0U;
    struct paging_process_space space = {0};
    struct paging_process_alias_set alias = {0};
    struct network_syscall_authenticator authenticator = {0};
    struct network_syscall_request *request;
    struct network_syscall_response *response;
    struct paging_process_expected_page pages[2];
    uint64_t executable_pages[1];
    const bool restore_interrupts = cpu_interrupts_enabled();

    fat32_require_base(true);
    if (frame_allocate(&executable_frame) != FRAME_STATUS_OK ||
        frame_allocate(&data_frame) != FRAME_STATUS_OK) {
        kernel_test_fail("network syscall fixture frames were unavailable");
    }
    for (size_t offset = 0U; offset < (size_t)PAGING_PAGE_SIZE; ++offset) {
        ((uint8_t *)(void *)executable_frame)[offset] = 0U;
        ((uint8_t *)(void *)data_frame)[offset] = 0U;
    }
    request = (struct network_syscall_request *)(void *)data_frame;
    response = (struct network_syscall_response *)(void *)(data_frame + 256U);
    for (size_t index = 0U; index < sizeof(url) - 1U; ++index) {
        ((char *)(void *)(data_frame + 512U))[index] = url[index];
    }
    for (size_t index = 0U; index < sizeof(destination) - 1U; ++index) {
        ((char *)(void *)(data_frame + 640U))[index] = destination[index];
    }
    *request = (struct network_syscall_request){
        .version = NETWORK_SYSCALL_ABI_VERSION,
        .size = sizeof(*request),
        .operation = NETWORK_SYSCALL_HTTP_TO_FILE,
        .primary_address = url_address,
        .secondary_address = path_address,
        .timeout_ns = UINT64_C(15000000000),
        .primary_length = sizeof(url) - 1U,
        .secondary_length = sizeof(destination) - 1U
    };
    if (restore_interrupts) {
        cpu_interrupt_disable();
    }
    executable_pages[0] = executable_frame;
    pages[0] = (struct paging_process_expected_page){
        .virtual_address = PAGING_LINUX_IMAGE_BASE + PAGING_PAGE_SIZE,
        .physical_address = executable_frame,
        .permissions = PAGING_EXECUTE
    };
    pages[1] = (struct paging_process_expected_page){
        .virtual_address = PAGING_LINUX_STACK_BASE,
        .physical_address = data_frame,
        .permissions = PAGING_WRITE
    };
    if (paging_process_space_build(&space) != PAGING_STATUS_OK ||
        paging_process_alias_set_narrow(&space, executable_pages, 1U,
            &alias) != PAGING_STATUS_OK ||
        paging_process_map_user_page(&space,
            PAGING_PROCESS_MAPPING_LINUX_IMAGE,
            pages[0].virtual_address, executable_frame, PAGING_EXECUTE) !=
                PAGING_STATUS_OK ||
        paging_process_map_user_page(&space,
            PAGING_PROCESS_MAPPING_LINUX_STACK,
            pages[1].virtual_address, data_frame, PAGING_WRITE) !=
                PAGING_STATUS_OK ||
        paging_process_validate_linux(&space, pages, 2U) !=
                PAGING_STATUS_OK) {
        kernel_test_fail("network syscall process boundary was not installed");
    }
    if (restore_interrupts) {
        cpu_interrupt_enable();
    }
    if (network_syscall_register(&space, UINT64_C(0x210),
            &authenticator) != NETWORK_SYSCALL_STATUS_OK ||
        network_syscall_dispatch(&authenticator, request_address,
            response_address) != NETWORK_SYSCALL_STATUS_OK ||
        response->boundary_status != NETWORK_SYSCALL_STATUS_OK ||
        response->network_status != NETWORK_STATUS_OK ||
        response->http_status != 200U ||
        response->value != sizeof(network_welcome) - 1U) {
        kernel_test_fail("network syscall HTTP operation failed");
    }
    network_syscall_process_terminated(&authenticator);
    if (network_syscall_dispatch(&authenticator, request_address,
            response_address) != NETWORK_SYSCALL_STATUS_BAD_TOKEN) {
        kernel_test_fail("terminated network syscall token remained valid");
    }
    if (restore_interrupts) {
        cpu_interrupt_disable();
    }
    if (paging_process_unmap_user_page(&space,
            PAGING_PROCESS_MAPPING_LINUX_STACK,
            PAGING_LINUX_STACK_BASE) != PAGING_STATUS_OK ||
        paging_process_unmap_user_page(&space,
            PAGING_PROCESS_MAPPING_LINUX_IMAGE,
            PAGING_LINUX_IMAGE_BASE + PAGING_PAGE_SIZE) != PAGING_STATUS_OK ||
        paging_process_alias_set_restore(&space, &alias) != PAGING_STATUS_OK ||
        paging_process_space_release(&space) != PAGING_STATUS_OK ||
        frame_release(data_frame) != FRAME_STATUS_OK ||
        frame_release(executable_frame) != FRAME_STATUS_OK) {
        kernel_test_fail("network syscall proof did not release its resources");
    }
    if (restore_interrupts) {
        cpu_interrupt_enable();
    }
    if (!fat32_file_equals(destination, network_welcome,
            sizeof(network_welcome) - 1U)) {
        kernel_test_fail("network syscall HTTP file changed after sync");
    }
    console_write("ST NETWORK syscall v1 HTTP-to-FAT32 boundary passed\n");
}

static void network_http_download_scenario(
    const char *url,
    const char *destination,
    bool expect_chunked,
    uint32_t redirects
)
{
    struct network_http_result result;
    enum network_status status;

    fat32_require_base(true);
    if (!network_get_state().configuration.configured) {
        network_require_dhcp();
    }
    status = network_http_download(NETWORK_TEST_OWNER, url, destination,
        false, UINT64_C(15000000000), &result);
    if (status != NETWORK_STATUS_OK ||
        result.status_code != 200U ||
        result.body_bytes != sizeof(network_welcome) - 1U ||
        result.chunked != expect_chunked || result.redirects != redirects ||
        !result.synchronized ||
        !fat32_file_equals(destination, network_welcome,
            sizeof(network_welcome) - 1U)) {
        console_write("ST NETWORK HTTP failure status ");
        console_write(network_status_string(status));
        console_write(" code ");
        console_write_u64(result.status_code);
        console_write(" bytes ");
        console_write_u64(result.body_bytes);
        console_write(" redirects ");
        console_write_u64(result.redirects);
        console_write(" synchronized ");
        console_write(result.synchronized ? "yes\n" : "no\n");
        kernel_test_fail("bounded HTTP FAT32 response changed");
    }
    console_write("ST NETWORK HTTP bytes ");
    console_write_u64(result.body_bytes);
    console_write(" elapsed-ns ");
    console_write_u64(result.elapsed_ns);
    console_write(" sync-ns ");
    console_write_u64(result.synchronize_ns);
    console_write(" throughput-Bps ");
    console_write_u64(result.elapsed_ns == 0U ? 0U :
        (uint64_t)result.body_bytes * UINT64_C(1000000000) /
            result.elapsed_ns);
    console_putc('\n');
    const struct virtio_net_statistics statistics =
        network_get_state().device.statistics;

    console_write("ST NETWORK CPU poll-ns ");
    console_write_u64(statistics.polling_processing_ns);
    console_write(" interrupt-ns ");
    console_write_u64(statistics.interrupt_processing_ns);
    console_write(" drops ");
    console_write_u64(statistics.dropped_frames);
    console_putc('\n');
}

static void network_tcp_connect_close(bool expect_reset)
{
    network_handle handle;
    enum network_status status;

    network_require_dhcp();
    if (network_tcp_open(NETWORK_TEST_OWNER, &handle) != NETWORK_STATUS_OK) {
        kernel_test_fail("TCP connection allocation failed");
    }
    status = network_tcp_connect(NETWORK_TEST_OWNER, handle,
        NETWORK_TEST_HTTP, 80U, NETWORK_DEFAULT_OPERATION_TIMEOUT_NS);
    if ((!expect_reset && status != NETWORK_STATUS_OK) ||
        (expect_reset && status != NETWORK_STATUS_CONNECTION_RESET) ||
        network_close(NETWORK_TEST_OWNER, handle) != NETWORK_STATUS_OK) {
        kernel_test_fail("TCP connection terminal state changed");
    }
}

/*
 * The peer cannot know a port is open until this side says so, and it has no
 * clock of its own: it answers frames. So a passive-open scenario announces
 * its port over UDP and the peer opens a TCP connection back. The announcement
 * also decides what the scenario proves. Sent to the gateway, it leaves the
 * peer's hardware address unknown, so the acknowledgement to its SYN has to be
 * deferred out of the receive path and retransmitted -- which is exactly the
 * hazard the service guard exists for. Sent to the peer itself, the hardware
 * address is known and a refusal can leave immediately.
 */
#define NETWORK_TEST_KNOCK_PORT UINT16_C(4243)
#define NETWORK_TEST_KNOCK_SOURCE UINT16_C(50003)
#define NETWORK_TEST_SECOND_KNOCK_SOURCE UINT16_C(50004)
#define NETWORK_TEST_LISTEN_PORT UINT16_C(7777)
#define NETWORK_TEST_CLOSED_PORT UINT16_C(7778)

static const uint8_t network_listen_request[] = "PHIPIA LISTEN\n";
static const uint8_t network_refusal_notice[] = "REFUSED";

static network_handle network_announce_port(
    uint32_t destination,
    uint16_t announced,
    uint16_t from_port
)
{
    network_handle knock;
    uint8_t message[6];

    message[0] = (uint8_t)'P';
    message[1] = (uint8_t)'H';
    message[2] = (uint8_t)'I';
    message[3] = (uint8_t)'P';
    message[4] = (uint8_t)(announced >> 8U);
    message[5] = (uint8_t)announced;
    if (network_udp_open(NETWORK_TEST_OWNER, &knock) != NETWORK_STATUS_OK ||
        network_udp_bind(NETWORK_TEST_OWNER, knock, from_port) !=
            NETWORK_STATUS_OK ||
        network_udp_send(NETWORK_TEST_OWNER, knock, destination,
            NETWORK_TEST_KNOCK_PORT, message, sizeof(message),
            NETWORK_DEFAULT_OPERATION_TIMEOUT_NS) != NETWORK_STATUS_OK) {
        kernel_test_fail("the listening port could not be announced");
    }
    return knock;
}

static void network_tcp_listen_controls(network_handle listener)
{
    network_handle stranger;
    network_handle accepted = 0U;
    uint8_t buffer[8];
    uint32_t source = 0U;
    uint16_t port = 0U;
    size_t length = 0U;
    size_t written = 0U;

    if (network_tcp_listen(NETWORK_TEST_OWNER, listener, 0U, 1U) !=
            NETWORK_STATUS_INVALID_ARGUMENT ||
        network_tcp_listen(NETWORK_TEST_OWNER, listener,
            NETWORK_TEST_LISTEN_PORT, 0U) !=
            NETWORK_STATUS_INVALID_ARGUMENT ||
        network_tcp_listen(NETWORK_TEST_OWNER, listener,
            NETWORK_TEST_LISTEN_PORT, NETWORK_TCP_MAX_BACKLOG + 1U) !=
            NETWORK_STATUS_INVALID_ARGUMENT ||
        network_tcp_listen(NETWORK_TEST_OWNER + 1U, listener,
            NETWORK_TEST_LISTEN_PORT, 1U) != NETWORK_STATUS_WRONG_OWNER) {
        kernel_test_fail("a listen outside its declared bounds was admitted");
    }
    if (network_tcp_accept(NETWORK_TEST_OWNER, listener, &accepted, &source,
            &port, UINT64_C(1000000)) != NETWORK_STATUS_WRONG_MODE ||
        accepted != 0U) {
        kernel_test_fail("accept was admitted before listen");
    }
    if (network_tcp_listen(NETWORK_TEST_OWNER, listener,
            NETWORK_TEST_LISTEN_PORT, 2U) != NETWORK_STATUS_OK ||
        network_get_state().tcp_listeners != 1U) {
        kernel_test_fail("the listening socket was refused its port");
    }
    if (network_tcp_open(NETWORK_TEST_OWNER, &stranger) != NETWORK_STATUS_OK ||
        network_tcp_listen(NETWORK_TEST_OWNER, stranger,
            NETWORK_TEST_LISTEN_PORT, 1U) != NETWORK_STATUS_PORT_IN_USE ||
        network_close(NETWORK_TEST_OWNER, stranger) != NETWORK_STATUS_OK) {
        kernel_test_fail("two sockets were allowed to listen on one port");
    }
    if (network_tcp_listen(NETWORK_TEST_OWNER, listener,
            NETWORK_TEST_CLOSED_PORT, 1U) != NETWORK_STATUS_WRONG_MODE ||
        network_tcp_connect(NETWORK_TEST_OWNER, listener, NETWORK_TEST_HTTP,
            80U, UINT64_C(1000000)) != NETWORK_STATUS_INVALID_ARGUMENT ||
        network_tcp_read(NETWORK_TEST_OWNER, listener, buffer,
            sizeof(buffer), &length, UINT64_C(1000000)) !=
            NETWORK_STATUS_WRONG_MODE ||
        network_tcp_write(NETWORK_TEST_OWNER, listener, network_welcome, 1U,
            &written, UINT64_C(1000000)) != NETWORK_STATUS_WRONG_MODE ||
        network_tcp_shutdown(NETWORK_TEST_OWNER, listener,
            UINT64_C(1000000)) != NETWORK_STATUS_WRONG_MODE ||
        network_tcp_accept(NETWORK_TEST_OWNER + 1U, listener, &accepted,
            &source, &port, UINT64_C(1000000)) !=
            NETWORK_STATUS_WRONG_OWNER) {
        kernel_test_fail("a listening socket answered a client operation");
    }
}

/*
 * A second peer, deliberately never accepted. It proves the two halves of the
 * listener's ownership: a completed connection waiting to be accepted is what
 * `network_poll` calls acceptable, and closing the listener refuses that peer
 * rather than orphaning its slot. The refusal is confirmed by the peer, which
 * reports the reset back over UDP.
 */
static network_handle network_tcp_listen_unaccepted(network_handle listener)
{
    struct network_poll_request request;
    struct network_poll_result result;
    struct network_state before = network_get_state();
    struct network_state after;
    network_handle knock;
    uint8_t received[16];
    uint32_t source = 0U;
    uint16_t port = 0U;
    size_t length = 0U;
    size_t ready = 0U;

    knock = network_announce_port(NETWORK_TEST_HTTP, NETWORK_TEST_LISTEN_PORT,
        NETWORK_TEST_SECOND_KNOCK_SOURCE);
    request.handle = listener;
    request.interests = NETWORK_READY_ACCEPTABLE;
    if (network_poll(NETWORK_TEST_OWNER, &request, 1U, &result, 1U, &ready,
            UINT64_C(10000000000)) != NETWORK_STATUS_OK || ready != 1U ||
        (result.ready & NETWORK_READY_ACCEPTABLE) == 0U ||
        (result.ready & NETWORK_READY_CONNECTED) != 0U ||
        result.error != NETWORK_STATUS_OK) {
        kernel_test_fail("a waiting connection was not reported acceptable");
    }
    after = network_get_state();
    if (after.tcp_connections != 2U || after.tcp_listeners != 1U ||
        after.statistics.tcp_passive_opens !=
            before.statistics.tcp_passive_opens + 1U) {
        kernel_test_fail("the second passive open was not counted once");
    }
    if (network_close(NETWORK_TEST_OWNER, listener) != NETWORK_STATUS_OK) {
        kernel_test_fail("the listener refused to close");
    }
    after = network_get_state();
    if (after.tcp_connections != 0U || after.tcp_listeners != 0U) {
        kernel_test_fail("closing a listener orphaned the peer it produced");
    }
    if (after.statistics.tcp_refusals !=
            before.statistics.tcp_refusals + 1U) {
        kernel_test_fail("the unaccepted peer was dropped rather than refused");
    }
    if (network_udp_receive(NETWORK_TEST_OWNER, knock, &source, &port,
            received, sizeof(received), &length,
            UINT64_C(10000000000)) != NETWORK_STATUS_OK ||
        source != NETWORK_TEST_HTTP || port != NETWORK_TEST_KNOCK_PORT ||
        length != sizeof(network_refusal_notice) - 1U ||
        !network_bytes_equal(received, network_refusal_notice, length)) {
        kernel_test_fail("the refused peer never saw the reset");
    }
    return knock;
}

static void network_tcp_listen_scenario(void)
{
    network_handle listener;
    network_handle knock;
    network_handle accepted = 0U;
    struct network_state before;
    struct network_state after;
    uint8_t received[64];
    uint32_t source = 0U;
    uint16_t port = 0U;
    size_t length = 0U;
    size_t written = 0U;
    enum network_status status;

    if (!kernel_test_tcp_listen_exit_self_test()) {
        kernel_test_fail("the passive-open exit contract drifted");
    }
    network_require_dhcp();
    if (network_tcp_open(NETWORK_TEST_OWNER, &listener) !=
            NETWORK_STATUS_OK) {
        kernel_test_fail("listening socket allocation failed");
    }
    network_tcp_listen_controls(listener);
    before = network_get_state();
    knock = network_announce_port(NETWORK_TEST_GATEWAY,
        NETWORK_TEST_LISTEN_PORT, NETWORK_TEST_KNOCK_SOURCE);
    status = network_tcp_accept(NETWORK_TEST_OWNER, listener, &accepted,
        &source, &port, UINT64_C(10000000000));
    if (status != NETWORK_STATUS_OK || accepted == 0U ||
        source != NETWORK_TEST_HTTP || port == 0U) {
        kernel_test_fail("the peer's connection was not accepted");
    }
    after = network_get_state();
    if (after.statistics.tcp_passive_opens !=
            before.statistics.tcp_passive_opens + 1U) {
        kernel_test_fail("the accepted connection was not a passive open");
    }
    if (after.statistics.arp_deferred <= before.statistics.arp_deferred) {
        kernel_test_fail("the receive path did not defer its unresolved send");
    }
    if (after.statistics.tcp_retransmissions <=
            before.statistics.tcp_retransmissions) {
        kernel_test_fail("the deferred acknowledgement was never retransmitted");
    }
    if (after.tcp_connections != 2U || after.tcp_listeners != 1U) {
        kernel_test_fail("the passive open did not cost exactly one slot");
    }
    if (network_tcp_read(NETWORK_TEST_OWNER, accepted, received,
            sizeof(received), &length,
            NETWORK_DEFAULT_OPERATION_TIMEOUT_NS) != NETWORK_STATUS_OK ||
        length != sizeof(network_listen_request) - 1U ||
        !network_bytes_equal(received, network_listen_request, length)) {
        kernel_test_fail("the accepted connection lost the peer's request");
    }
    if (network_tcp_write(NETWORK_TEST_OWNER, accepted, network_welcome,
            sizeof(network_welcome) - 1U, &written,
            NETWORK_DEFAULT_OPERATION_TIMEOUT_NS) != NETWORK_STATUS_OK ||
        written != sizeof(network_welcome) - 1U) {
        kernel_test_fail("the accepted connection could not answer its peer");
    }
    status = network_tcp_read(NETWORK_TEST_OWNER, accepted, received,
        sizeof(received), &length, NETWORK_DEFAULT_OPERATION_TIMEOUT_NS);
    if (status != NETWORK_STATUS_CONNECTION_CLOSED || length != 0U) {
        kernel_test_fail("the peer's close was not reported to the reader");
    }
    if (network_tcp_shutdown(NETWORK_TEST_OWNER, accepted,
            NETWORK_DEFAULT_OPERATION_TIMEOUT_NS) != NETWORK_STATUS_OK) {
        kernel_test_fail("the accepted connection did not close cleanly");
    }
    if (network_close(NETWORK_TEST_OWNER, accepted) != NETWORK_STATUS_OK ||
        network_close(NETWORK_TEST_OWNER, knock) != NETWORK_STATUS_OK) {
        kernel_test_fail("the accepted connection did not release cleanly");
    }
    knock = network_tcp_listen_unaccepted(listener);
    if (network_close(NETWORK_TEST_OWNER, knock) != NETWORK_STATUS_OK) {
        kernel_test_fail("passive-open teardown failed");
    }
    after = network_get_state();
    if (after.tcp_connections != 0U || after.tcp_listeners != 0U ||
        after.udp_sockets != 0U || after.timers != 0U) {
        kernel_test_fail("a passive open left endpoints behind");
    }
    if (network_tcp_accept(NETWORK_TEST_OWNER, listener, &accepted, &source,
            &port, UINT64_C(1000000)) != NETWORK_STATUS_STALE_HANDLE) {
        kernel_test_fail("a closed listener still accepted connections");
    }
}

static void network_tcp_refused_scenario(void)
{
    network_handle knock;
    struct network_state before;
    struct network_state after;
    uint8_t received[16];
    uint32_t source = 0U;
    uint16_t port = 0U;
    size_t length = 0U;

    if (!kernel_test_tcp_listen_exit_self_test()) {
        kernel_test_fail("the passive-open exit contract drifted");
    }
    network_require_dhcp();
    before = network_get_state();
    /*
     * Announced to the peer itself, so the peer's hardware address is resolved
     * before its SYN arrives and the refusal can leave the receive path at
     * once. Nothing is listening on the announced port.
     */
    knock = network_announce_port(NETWORK_TEST_HTTP, NETWORK_TEST_CLOSED_PORT,
        NETWORK_TEST_KNOCK_SOURCE);
    if (network_udp_receive(NETWORK_TEST_OWNER, knock, &source, &port,
            received, sizeof(received), &length,
            UINT64_C(10000000000)) != NETWORK_STATUS_OK ||
        source != NETWORK_TEST_HTTP || port != NETWORK_TEST_KNOCK_PORT ||
        length != sizeof(network_refusal_notice) - 1U ||
        !network_bytes_equal(received, network_refusal_notice, length)) {
        kernel_test_fail("the peer did not report a reset from a closed port");
    }
    after = network_get_state();
    if (after.statistics.tcp_refusals != before.statistics.tcp_refusals + 1U) {
        kernel_test_fail("a SYN to a closed port was not refused exactly once");
    }
    if (after.statistics.tcp_accepted != before.statistics.tcp_accepted) {
        kernel_test_fail("a refused SYN was counted as an accepted segment");
    }
    if (after.tcp_connections != 0U || after.tcp_listeners != 0U) {
        kernel_test_fail("a refused SYN consumed a connection slot");
    }
    if (network_close(NETWORK_TEST_OWNER, knock) != NETWORK_STATUS_OK) {
        kernel_test_fail("refusal teardown failed");
    }
    after = network_get_state();
    if (after.udp_sockets != 0U || after.timers != 0U) {
        kernel_test_fail("a refusal left endpoints behind");
    }
}

static void network_udp_scenario(bool isolate)
{
    network_handle first;
    network_handle second = 0U;
    uint8_t first_message[] = "one";
    uint8_t second_message[] = "two";
    uint8_t received[8];
    uint32_t source;
    uint16_t port;
    size_t length;

    network_require_dhcp();
    if (network_udp_open(NETWORK_TEST_OWNER, &first) != NETWORK_STATUS_OK ||
        network_udp_bind(NETWORK_TEST_OWNER, first, 50001U) !=
            NETWORK_STATUS_OK ||
        (isolate &&
            (network_udp_open(NETWORK_TEST_OWNER, &second) !=
                NETWORK_STATUS_OK ||
             network_udp_bind(NETWORK_TEST_OWNER, second, 50002U) !=
                NETWORK_STATUS_OK))) {
        kernel_test_fail("UDP endpoint allocation failed");
    }
    if (network_udp_send(NETWORK_TEST_OWNER, first, NETWORK_TEST_HTTP, 4242U,
            first_message, sizeof(first_message) - 1U,
            NETWORK_DEFAULT_OPERATION_TIMEOUT_NS) != NETWORK_STATUS_OK ||
        (isolate && network_udp_send(NETWORK_TEST_OWNER, second,
            NETWORK_TEST_HTTP, 4242U, second_message,
            sizeof(second_message) - 1U,
            NETWORK_DEFAULT_OPERATION_TIMEOUT_NS) != NETWORK_STATUS_OK) ||
        network_udp_receive(NETWORK_TEST_OWNER, first, &source, &port,
            received, sizeof(received), &length,
            NETWORK_DEFAULT_OPERATION_TIMEOUT_NS) != NETWORK_STATUS_OK ||
        source != NETWORK_TEST_HTTP || port != 4242U ||
        length != sizeof(first_message) - 1U ||
        !network_bytes_equal(received, first_message, length)) {
        kernel_test_fail("UDP echo used the wrong endpoint");
    }
    if (isolate &&
        (network_udp_receive(NETWORK_TEST_OWNER, second, &source, &port,
            received, sizeof(received), &length,
            NETWORK_DEFAULT_OPERATION_TIMEOUT_NS) != NETWORK_STATUS_OK ||
         length != sizeof(second_message) - 1U ||
         !network_bytes_equal(received, second_message, length))) {
        kernel_test_fail("one socket received another socket's datagram");
    }
    if (network_close(NETWORK_TEST_OWNER, first) != NETWORK_STATUS_OK ||
        (isolate && network_close(NETWORK_TEST_OWNER, second) !=
            NETWORK_STATUS_OK)) {
        kernel_test_fail("UDP endpoint teardown failed");
    }
}

static void network_download(const char *destination)
{
    struct network_http_result result;

    network_require_dhcp();
    if (network_http_download(NETWORK_TEST_OWNER,
            "http://phipia.test/welcome.txt", destination, false,
            UINT64_C(15000000000), &result) != NETWORK_STATUS_OK ||
        !result.synchronized || result.status_code != 200U ||
        !fat32_file_equals(destination, network_welcome,
            sizeof(network_welcome) - 1U)) {
        kernel_test_fail("HTTP download did not reach synchronized FAT32");
    }
}

static void network_linux_cat_twice(void)
{
    static const uint8_t input[] = "fixture\n";

    for (size_t run = 0U; run < 2U; ++run) {
        struct linux_userland_result result;

        if (linux_userland_launch(LINUX_USERLAND_PROFILE_CAT, &result) !=
                LINUX_USERLAND_STATUS_WAITING ||
            linux_userland_deliver_cat_input(input, sizeof(input) - 1U,
                false, &result) != LINUX_USERLAND_STATUS_WAITING ||
            linux_userland_deliver_cat_input(NULL, 0U, true, &result) !=
                LINUX_USERLAND_STATUS_OK || !result.teardown_complete ||
            !linux_userland_resources_released()) {
            kernel_test_fail("authenticated cat launch did not recover");
        }
    }
}

_Noreturn void kernel_test_complete_network(void)
{
    if (active_scenario < KERNEL_TEST_NETWORK_NIC_DISCOVERY ||
        active_scenario > KERNEL_TEST_NETWORK_TCP_REFUSED) {
        kernel_test_fail("network completion used outside its scenario");
    }
    cpu_interrupt_enable();
    switch (active_scenario) {
    case KERNEL_TEST_NETWORK_NIC_DISCOVERY:
    case KERNEL_TEST_NETWORK_NIC_INITIALIZATION:
        network_require_device();
        break;
    case KERNEL_TEST_NETWORK_NIC_ABSENT:
        if (network_get_state().device.present || network_get_state().active) {
            kernel_test_fail("absent NIC was invented");
        }
        break;
    case KERNEL_TEST_NETWORK_LINK_DOWN:
        if (!network_get_state().device.present ||
            network_get_state().device.link_up) {
            kernel_test_fail("link-down state was not retained");
        }
        break;
    case KERNEL_TEST_NETWORK_DHCP:
        if (shell_execute("dhcp") != SHELL_STATUS_OK ||
            !network_get_state().configuration.configured) {
            (void)shell_execute("netstat");
            (void)shell_execute("network");
            kernel_test_fail("Terminal DHCP command did not configure IPv4");
        }
        break;
    case KERNEL_TEST_NETWORK_DHCP_TIMEOUT: {
        enum network_status status = network_start_dhcp(UINT64_C(1000000000));

        if (status != NETWORK_STATUS_TIMEOUT) {
            console_serial_write("ST DHCP silent-peer status ");
            console_serial_write(network_status_string(status));
            console_serial_write("\n");
            kernel_test_fail("DHCP silent peer did not time out");
        }
        if (network_get_state().configuration.configured) {
            kernel_test_fail("DHCP timeout invented a lease");
        }
        break;
    }
    case KERNEL_TEST_NETWORK_STATIC:
        network_require_static();
        if (network_get_state().configuration.source !=
                NETWORK_CONFIGURATION_STATIC) {
            kernel_test_fail("static IPv4 source was not recorded");
        }
        break;
    case KERNEL_TEST_NETWORK_ARP:
    case KERNEL_TEST_NETWORK_ICMP: {
        struct network_ping_result result;

        network_require_static();
        if (network_ping(NETWORK_TEST_GATEWAY, 3U, UINT64_C(1000000000),
                &result) != NETWORK_STATUS_OK || result.received != 3U ||
            network_get_state().arp_entries == 0U) {
            kernel_test_fail("ARP and ICMP production exchange failed");
        }
        break;
    }
    case KERNEL_TEST_NETWORK_ICMP_TIMEOUT: {
        struct network_ping_result result;

        network_require_static();
        if (network_ping(NETWORK_TEST_GATEWAY, 1U, UINT64_C(500000000),
                &result) != NETWORK_STATUS_TIMEOUT || result.received != 0U) {
            kernel_test_fail("ICMP timeout was not recoverable");
        }
        break;
    }
    case KERNEL_TEST_NETWORK_UDP:
        network_udp_scenario(false);
        break;
    case KERNEL_TEST_NETWORK_DNS_A:
    case KERNEL_TEST_NETWORK_DNS_CNAME: {
        uint32_t address;

        network_require_dhcp();
        if (network_resolve("phipia.test", &address,
                NETWORK_DEFAULT_OPERATION_TIMEOUT_NS) != NETWORK_STATUS_OK ||
            address != NETWORK_TEST_HTTP) {
            kernel_test_fail("DNS resolution did not return fixture address");
        }
        break;
    }
    case KERNEL_TEST_NETWORK_DNS_MALFORMED: {
        uint32_t address;
        enum network_status status;

        network_require_dhcp();
        status = network_resolve("phipia.test", &address,
            UINT64_C(1000000000));
        if (status != NETWORK_STATUS_TIMEOUT &&
            status != NETWORK_STATUS_DNS_FAILURE &&
            status != NETWORK_STATUS_MALFORMED) {
            kernel_test_fail("malformed DNS response was accepted");
        }
        break;
    }
    case KERNEL_TEST_NETWORK_TCP:
        network_tcp_connect_close(false);
        break;
    case KERNEL_TEST_NETWORK_TCP_RETRANSMIT: {
        uint64_t before = network_get_state().statistics.tcp_retransmissions;

        network_tcp_connect_close(false);
        if (network_get_state().statistics.tcp_retransmissions <= before) {
            kernel_test_fail("TCP SYN was not retransmitted");
        }
        break;
    }
    case KERNEL_TEST_NETWORK_TCP_RESET:
        network_tcp_connect_close(true);
        break;
    case KERNEL_TEST_NETWORK_HTTP_LENGTH: {
        struct network_ping_result proof_ping;

        network_require_dhcp();
        if (network_ping(NETWORK_TEST_GATEWAY, 1U, UINT64_C(1000000000),
                &proof_ping) != NETWORK_STATUS_OK ||
            proof_ping.received != 1U) {
            kernel_test_fail("HTTP production proof could not ping gateway");
        }
        network_syscall_http_download_scenario();
        break;
    }
    case KERNEL_TEST_NETWORK_HTTP_CHUNKED:
        network_http_download_scenario(
            "http://phipia.test/welcome.txt", "HTTPCHNK.TXT", true, 0U);
        break;
    case KERNEL_TEST_NETWORK_HTTP_REDIRECT:
        network_http_download_scenario(
            "http://phipia.test/start", "HTTPREDR.TXT", false, 1U);
        break;
    case KERNEL_TEST_NETWORK_HTTP_MALFORMED: {
        struct network_http_result result;
        struct phipfs_stat stat;

        fat32_require_base(true);
        network_require_dhcp();
        if (network_http_download(NETWORK_TEST_OWNER,
                "http://phipia.test/welcome.txt", "BADHTTP.TXT", false,
                UINT64_C(5000000000), &result) == NETWORK_STATUS_OK ||
            phipfs_stat_path(PHIPFS_VOLUME_DATA, "BADHTTP.TXT", &stat) !=
                PHIPFS_STATUS_NOT_FOUND) {
            kernel_test_fail("malformed HTTP response was accepted");
        }
        break;
    }
    case KERNEL_TEST_NETWORK_HTTP_NESTED:
        fat32_require_base(true);
        if (phipfs_mkdir(PHIPFS_VOLUME_DATA, "DOWNLDS") != PHIPFS_STATUS_OK &&
            phipfs_stat_path(PHIPFS_VOLUME_DATA, "DOWNLDS",
                &(struct phipfs_stat){0}) != PHIPFS_STATUS_OK) {
            kernel_test_fail("download directory was not available");
        }
        network_download("DOWNLDS/WELCOME.TXT");
        break;
    case KERNEL_TEST_NETWORK_HTTP_REPLACE:
        fat32_require_base(true);
        fat32_feed("write welcome.txt old");
        network_download("welcome.txt");
        break;
    case KERNEL_TEST_NETWORK_HTTP_DISK_FULL: {
        struct network_http_result result;
        enum network_status status;

        fat32_require_base(true);
        network_require_dhcp();
        status = network_http_download(NETWORK_TEST_OWNER,
            "http://phipia.test/welcome.txt", "full.txt", false,
            UINT64_C(5000000000), &result);
        if (status != NETWORK_STATUS_TOO_LARGE &&
            status != NETWORK_STATUS_FILESYSTEM) {
            kernel_test_fail("disk-full HTTP failure was not recoverable");
        }
        break;
    }
    case KERNEL_TEST_NETWORK_NIC_RESET: {
        network_handle stale;
        enum network_status open_status;
        enum network_status shutdown_status;
        enum network_status initialize_status;
        enum network_status stale_status;

        network_require_dhcp();
        open_status = network_udp_open(NETWORK_TEST_OWNER, &stale);
        shutdown_status = network_shutdown();
        initialize_status = network_initialize();
        stale_status = network_close(NETWORK_TEST_OWNER, stale);
        if (open_status != NETWORK_STATUS_OK ||
            shutdown_status != NETWORK_STATUS_OK ||
            initialize_status != NETWORK_STATUS_OK ||
            stale_status != NETWORK_STATUS_STALE_HANDLE) {
            console_write("ST NETWORK RESET open ");
            console_write(network_status_string(open_status));
            console_write(" shutdown ");
            console_write(network_status_string(shutdown_status));
            console_write(" initialize ");
            console_write(network_status_string(initialize_status));
            console_write(" stale ");
            console_write(network_status_string(stale_status));
            console_putc('\n');
            kernel_test_fail("NIC reset retained an open handle");
        }
        break;
    }
    case KERNEL_TEST_NETWORK_SYSTEM_IMMUTABLE: {
        phipfs_handle handle;

        fat32_require_base(true);
        network_http_download_scenario(
            "http://phipia.test/welcome.txt", "IMMUTABL.TXT",
            false, 0U);
        if (phipfs_open(PHIPFS_VOLUME_SYSTEM, "BUSYBOX", PHIPFS_ACCESS_WRITE,
                &handle) != PHIPFS_STATUS_READ_ONLY) {
            kernel_test_fail("networking weakened the immutable system volume");
        }
        break;
    }
    case KERNEL_TEST_NETWORK_MISSING_LINUX_ECHO:
    case KERNEL_TEST_NETWORK_MISSING_LINUX_UNAME: {
        struct linux_userland_result result;
        enum linux_userland_profile profile = active_scenario ==
            KERNEL_TEST_NETWORK_MISSING_LINUX_ECHO ?
            LINUX_USERLAND_PROFILE_ECHO : LINUX_USERLAND_PROFILE_UNAME;

        fat32_require_base(false);
        if (network_get_state().device.present ||
            linux_userland_launch(profile, &result) !=
                LINUX_USERLAND_STATUS_OK || !result.teardown_complete) {
            kernel_test_fail("missing NIC blocked authenticated userspace");
        }
        break;
    }
    case KERNEL_TEST_NETWORK_MISSING_LINUX_CAT:
        fat32_require_base(false);
        if (network_get_state().device.present) {
            kernel_test_fail("missing NIC scenario discovered a device");
        }
        network_linux_cat_twice();
        break;
    case KERNEL_TEST_NETWORK_FILES: {
        struct phipfs_list_entry entries[4];
        size_t count = 0U;

        fat32_require_base(true);
        network_require_dhcp();
        if (phipfs_list(PHIPFS_VOLUME_DATA, ".", entries, 4U, &count) !=
                PHIPFS_STATUS_OK || ui_flush() != UI_STATUS_OK) {
            kernel_test_fail("networking regressed Files");
        }
        break;
    }
    case KERNEL_TEST_NETWORK_NOTES:
        fat32_require_base(true);
        network_require_dhcp();
        fat32_feed("write NOTES.TXT network-note");
        if (ui_flush() != UI_STATUS_OK) {
            kernel_test_fail("networking regressed Notes");
        }
        break;
    case KERNEL_TEST_NETWORK_MEDIA_EDITOR:
        fat32_require_base(true);
        network_require_dhcp();
        if (!ui_is_active() || ui_flush() != UI_STATUS_OK) {
            kernel_test_fail("networking regressed Media Editor");
        }
        break;
    case KERNEL_TEST_NETWORK_PERSISTENCE: {
        struct phipfs_stat stat;
        enum phipfs_status status;

        fat32_require_base(true);
        status = phipfs_stat_path(PHIPFS_VOLUME_DATA, "network.txt", &stat);
        if (status == PHIPFS_STATUS_NOT_FOUND) {
            network_download("network.txt");
            if (phipfs_unmount(PHIPFS_VOLUME_DATA) != PHIPFS_STATUS_OK) {
                kernel_test_fail("network download did not unmount cleanly");
            }
            console_write("\nST NETWORK PERSISTENCE synchronized reboot phase\n");
            cpu_out8(UINT16_C(0x0064), UINT8_C(0xFE));
            kernel_test_fail("platform reset did not restart QEMU");
        }
        if (status != PHIPFS_STATUS_OK ||
            !fat32_file_equals("network.txt", network_welcome,
                sizeof(network_welcome) - 1U)) {
            kernel_test_fail("network download did not persist after reboot");
        }
        break;
    }
    case KERNEL_TEST_NETWORK_SOCKET_ISOLATION:
        network_udp_scenario(true);
        break;
    case KERNEL_TEST_NETWORK_TCP_LISTEN:
        network_tcp_listen_scenario();
        break;
    case KERNEL_TEST_NETWORK_TCP_REFUSED:
        network_tcp_refused_scenario();
        break;
    default:
        kernel_test_fail("unreachable network scenario");
    }
    console_write("\nST NETWORK production path bounded and recoverable\n");
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_multiprocess(void)
{
    const struct boot_ledger *ledger = boot_ledger_installed();
    const struct boot_stage_receipt *foundation;
    const struct boot_stage_receipt *receipt;
    const struct multiprocess_proof_result proof =
        multiprocess_get_proof_result();

    if (active_scenario != KERNEL_TEST_MULTIPROCESS) {
        kernel_test_fail("multiprocess completion used outside its scenario");
    }
    foundation = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_MULTIPROCESS_FOUNDATION);
    receipt = boot_ledger_receipt_for(ledger, BOOT_STAGE_MULTIPROCESS_PROOF);
    if (ledger == NULL || foundation == NULL || receipt == NULL ||
        foundation->result != BOOT_RECEIPT_RAN ||
        receipt->result != BOOT_RECEIPT_RAN ||
        receipt->proof_counter_count != 2U ||
        receipt->proof_counters[0] != MULTIPROCESS_EXPECTED_SWITCHES ||
        receipt->proof_counters[1] != MULTIPROCESS_MAX_PROCESSES ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_MULTIPROCESS_FOUNDATION_AVAILABLE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_MULTIPROCESS_PROOF_COMPLETE) ||
        !kernel_test_multiprocess_exit_self_test()) {
        kernel_test_fail("multiprocess installed receipt is invalid");
    }
    if (proof.process_count != MULTIPROCESS_MAX_PROCESSES ||
        proof.rounds != MULTIPROCESS_ROUNDS ||
        proof.switches != MULTIPROCESS_EXPECTED_SWITCHES ||
        proof.completed != MULTIPROCESS_MAX_PROCESSES ||
        proof.terminated != 0U ||
        proof.address_space_table_frames == 0U ||
        proof.robustness_tests !=
            MULTIPROCESS_CONTROLLED_ROBUSTNESS_TESTS ||
        !proof.concurrent_address_spaces ||
        !proof.round_robin_interleaved || !proof.contexts_preserved ||
        !proof.isolation_confirmed || !proof.fault_contained ||
        !proof.teardown_complete || !proof.resource_census_equal ||
        !multiprocess_resources_released()) {
        kernel_test_fail("multiprocess installed proof is inconsistent");
    }
    kernel_test_pass();
}

/*
 * The station addresses the host hands QEMU on its command line, in the order
 * the driver matrix declares the devices that carry them. Nothing inside the
 * kernel could produce these values: they travel from the Makefile, through
 * QEMU's device models, into each part's own EEPROM or address registers, and
 * back out through the driver that read them. A driver that reported a
 * plausible-looking address it had not actually fetched would have to invent
 * all four of these exactly.
 */
#define DRIVER_PINNED_STATION_ADDRESSES 4U

static const struct {
    size_t driver;
    uint64_t station;
} driver_pinned_stations[DRIVER_PINNED_STATION_ADDRESSES] = {
    { 4U, UINT64_C(0x01BBAA005452) },
    { 5U, UINT64_C(0x02BBAA005452) },
    { 8U, UINT64_C(0x03BBAA005452) },
    { 12U, UINT64_C(0x04BBAA005452) }
};

_Noreturn void kernel_test_complete_driver_matrix(void)
{
    const struct boot_ledger *ledger = boot_ledger_installed();
    const struct boot_stage_receipt *foundation;
    const struct boot_stage_receipt *receipt;
    const struct driver_matrix_result matrix = driver_matrix_get_result();
    const bool every_device = active_scenario == KERNEL_TEST_DRIVER_MATRIX;
    const uint32_t expected_present = every_device ?
        (uint32_t)driver_matrix_count() : 5U;
    uint32_t reset_capable = 0U;

    if (active_scenario != KERNEL_TEST_DRIVER_MATRIX &&
        active_scenario != KERNEL_TEST_DRIVER_MATRIX_BUILTIN) {
        kernel_test_fail("driver completion used outside its scenario");
    }
    foundation = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_DRIVER_MATRIX_FOUNDATION);
    receipt = boot_ledger_receipt_for(ledger, BOOT_STAGE_DRIVER_MATRIX_PROBE);
    if (ledger == NULL || foundation == NULL || receipt == NULL ||
        foundation->result != BOOT_RECEIPT_RAN ||
        receipt->result != BOOT_RECEIPT_RAN ||
        receipt->proof_counter_count != 2U ||
        receipt->proof_counters[0] != matrix.bound ||
        receipt->proof_counters[1] != matrix.present ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_DRIVER_MATRIX_FOUNDATION_AVAILABLE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_DRIVER_MATRIX_PROBE_COMPLETE) ||
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_DRIVER_MATRIX_DEVICES_ABSENT) ||
        !kernel_test_driver_matrix_exit_self_test()) {
        kernel_test_fail("driver matrix receipt is invalid");
    }
    if (matrix.declared != driver_matrix_count() ||
        matrix.controls != DRIVER_MATRIX_CONTROLLED_CONTROLS ||
        matrix.present != expected_present ||
        matrix.bound != matrix.present ||
        !matrix.every_present_device_bound || !matrix.teardown_complete ||
        !matrix.resource_census_equal || matrix.register_reads == 0U ||
        matrix.register_writes == 0U ||
        !driver_matrix_resources_released()) {
        kernel_test_fail("driver matrix result is inconsistent");
    }
    for (size_t index = 0U; index < driver_matrix_count(); ++index) {
        const struct driver_probe *probe = &matrix.probes[index];
        const bool memory_driver =
            driver_matrix_access(index) == DRIVER_ACCESS_MEMORY;

        if (!every_device && !probe->present) {
            if (probe->bound || probe->identity != 0U) {
                kernel_test_fail("an absent device reported a bound driver");
            }
            continue;
        }
        if (!probe->present || !probe->bound ||
            probe->vendor_id != driver_matrix_vendor(index) ||
            probe->device_id != driver_matrix_device(index) ||
            probe->identity == 0U) {
            kernel_test_fail("a declared driver did not bind its device");
        }
        if (memory_driver) {
            if (probe->register_bytes == 0U ||
                probe->reset_observed != driver_matrix_defines_reset(index)) {
                kernel_test_fail("a driver did not honour its reset contract");
            }
            if (driver_matrix_defines_reset(index)) {
                ++reset_capable;
            }
        } else if (probe->register_bytes != 0U || probe->reset_observed ||
            driver_matrix_defines_reset(index)) {
            kernel_test_fail("a configuration driver mapped a window");
        }
    }
    if (matrix.resets != reset_capable) {
        kernel_test_fail("the recorded reset count is inconsistent");
    }
    if (every_device) {
        for (size_t index = 0U; index < DRIVER_PINNED_STATION_ADDRESSES;
             ++index) {
            const size_t driver = driver_pinned_stations[index].driver;

            if (driver >= driver_matrix_count() ||
                matrix.probes[driver].identity !=
                    driver_pinned_stations[index].station) {
                kernel_test_fail(
                    "a driver did not read its device's real station address");
            }
        }
    }
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_audio(void)
{
    const struct boot_ledger *ledger = boot_ledger_installed();
    const struct boot_stage_receipt *foundation;
    const struct boot_stage_receipt *receipt;
    const struct audio_proof_result proof = audio_get_proof_result();
    uint32_t identified = 0U;

    if (active_scenario != KERNEL_TEST_AUDIO) {
        kernel_test_fail("audio completion used outside its scenario");
    }
    foundation = boot_ledger_receipt_for(ledger, BOOT_STAGE_AUDIO_FOUNDATION);
    receipt = boot_ledger_receipt_for(ledger, BOOT_STAGE_AUDIO_CODEC_PROOF);
    if (ledger == NULL || foundation == NULL || receipt == NULL ||
        foundation->result != BOOT_RECEIPT_RAN ||
        receipt->result != BOOT_RECEIPT_RAN ||
        receipt->proof_counter_count != 2U ||
        receipt->proof_counters[0] != proof.responses_received ||
        receipt->proof_counters[1] != proof.codecs_identified ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_AUDIO_FOUNDATION_AVAILABLE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_AUDIO_CODEC_PROOF_COMPLETE) ||
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_AUDIO_CONTROLLER_ABSENT) ||
        !kernel_test_audio_exit_self_test()) {
        kernel_test_fail("HD Audio receipt is invalid");
    }
    if (proof.controls != AUDIO_CONTROLLED_CONTROLS ||
        (proof.version >> 8U) != 1U || proof.capability == 0U ||
        proof.output_streams == 0U || proof.corb_entries == 0U ||
        proof.rirb_entries == 0U || proof.codecs_present == 0U ||
        proof.codecs_identified != proof.codecs_present ||
        proof.verbs_issued == 0U ||
        proof.responses_received != proof.verbs_issued ||
        !proof.controller_reset || !proof.rings_running ||
        !proof.audio_function_group_found ||
        !proof.device_wrote_response_ring ||
        proof.sample_rate != AUDIO_PCM_SAMPLE_RATE ||
        proof.channels != AUDIO_PCM_CHANNELS ||
        proof.bits_per_sample != AUDIO_PCM_BITS_PER_SAMPLE ||
        proof.pcm_frames != AUDIO_PCM_FRAMES ||
        proof.pcm_bytes != AUDIO_PCM_BYTES ||
        proof.bdl_entries != AUDIO_PCM_BDL_ENTRIES ||
        proof.stream_format != UINT16_C(0x0011) || proof.pcm_hash == 0U ||
        proof.playback_converter == 0U || proof.playback_pin == 0U ||
        proof.playback_stream_tag != AUDIO_PCM_STREAM_TAG ||
        !proof.output_route_found || !proof.pcm_profile_supported ||
        !proof.pcm_device_owned_during_run ||
        !proof.bdl_device_owned_during_run || !proof.stream_reset ||
        !proof.stream_started || !proof.link_position_advanced ||
        !proof.stream_status_observed || proof.service_iterations == 0U ||
        proof.period_completions == 0U ||
        !proof.stream_stopped_before_reset ||
        !proof.bus_master_withdrawn_before_release ||
        !proof.teardown_complete || !proof.resource_census_equal ||
        !audio_resources_released()) {
        kernel_test_fail("HD Audio proof is inconsistent");
    }
    for (size_t index = 0U; index < AUDIO_MAX_CODECS; ++index) {
        const struct audio_codec *codec = &proof.codecs[index];

        if (!codec->identified) {
            continue;
        }
        ++identified;
        if (codec->address != index || codec->vendor_device == 0U ||
            (codec->vendor_device >> 16U) == 0U ||
            codec->vendor_device == UINT32_C(0xFFFFFFFF) ||
            codec->first_group_node == 0U || codec->group_node_count == 0U) {
            kernel_test_fail("a codec did not identify itself");
        }
    }
    if (identified != proof.codecs_identified) {
        kernel_test_fail("the recorded codec count is inconsistent");
    }
    /*
     * Nothing this side of the link produces a codec identity, and no other
     * scenario reaches this device. The bounded proof leaves no allocation,
     * no claim and no bus master behind.
     */
    if (dma_get_state().active_allocations != 0U ||
        pci_resource_get_state().bus_masters != 0U ||
        pci_resource_get_state().active_claims != 0U) {
        kernel_test_fail("the HD Audio proof left the machine holding memory");
    }
    kernel_test_pass();
}

/*
 * The published encodings this scenario re-derives independently of the
 * driver's own table. Each is a master control register value built from the
 * documented field layout -- part number in bits 20 through 28, revision in
 * the low byte -- and the family Nouveau's device table puts it in. They are
 * constructed from that encoding rather than captured from a board, and this
 * scenario exists to make that construction check the same decode the driver
 * would use on real silicon.
 */
static const struct {
    uint32_t boot0;
    uint32_t chipset;
    enum nvidia_architecture architecture;
} nvidia_published_encodings[] = {
    { UINT32_C(0x050000A2), UINT32_C(0x050), NVIDIA_ARCHITECTURE_TESLA },
    { UINT32_C(0x0A0000A3), UINT32_C(0x0A0), NVIDIA_ARCHITECTURE_TESLA },
    { UINT32_C(0x0C0000A3), UINT32_C(0x0C0), NVIDIA_ARCHITECTURE_FERMI },
    { UINT32_C(0x0D9000A1), UINT32_C(0x0D9), NVIDIA_ARCHITECTURE_FERMI },
    { UINT32_C(0x0E4000A1), UINT32_C(0x0E4), NVIDIA_ARCHITECTURE_KEPLER },
    { UINT32_C(0x108000A1), UINT32_C(0x108), NVIDIA_ARCHITECTURE_KEPLER },
    { UINT32_C(0x117000A1), UINT32_C(0x117), NVIDIA_ARCHITECTURE_MAXWELL },
    { UINT32_C(0x124000A1), UINT32_C(0x124), NVIDIA_ARCHITECTURE_MAXWELL },
    { UINT32_C(0x134000A1), UINT32_C(0x134), NVIDIA_ARCHITECTURE_PASCAL },
    { UINT32_C(0x140000A1), UINT32_C(0x140), NVIDIA_ARCHITECTURE_VOLTA },
    { UINT32_C(0x164000A1), UINT32_C(0x164), NVIDIA_ARCHITECTURE_TURING },
    { UINT32_C(0x172000A1), UINT32_C(0x172), NVIDIA_ARCHITECTURE_AMPERE },
    { UINT32_C(0x192000A1), UINT32_C(0x192), NVIDIA_ARCHITECTURE_ADA }
};

#define NVIDIA_PUBLISHED_ENCODINGS \
    (sizeof(nvidia_published_encodings) / \
        sizeof(nvidia_published_encodings[0]))

/*
 * The declared shape of the fifteen drivers, restated outside the driver so a
 * table that changed quietly has to change in two places. Eight map one
 * register window each, one reads the aperture descriptions a claim produces,
 * six read configuration space and take nothing, and exactly one writes.
 */
static const struct {
    uint8_t class_code;
    uint8_t subclass;
    enum nvidia_access access;
    bool writes_registers;
} nvidia_declared_drivers[NVIDIA_DRIVER_COUNT] = {
    { UINT8_C(0x03), UINT8_C(0xFF), NVIDIA_ACCESS_MEMORY, false },
    { UINT8_C(0x03), UINT8_C(0xFF), NVIDIA_ACCESS_MEMORY, false },
    { UINT8_C(0x03), UINT8_C(0xFF), NVIDIA_ACCESS_MEMORY, false },
    { UINT8_C(0x03), UINT8_C(0xFF), NVIDIA_ACCESS_MEMORY, true },
    { UINT8_C(0x04), UINT8_C(0x03), NVIDIA_ACCESS_MEMORY, false },
    { UINT8_C(0x03), UINT8_C(0xFF), NVIDIA_ACCESS_MEMORY, false },
    { UINT8_C(0x03), UINT8_C(0xFF), NVIDIA_ACCESS_MEMORY, false },
    { UINT8_C(0x03), UINT8_C(0xFF), NVIDIA_ACCESS_APERTURE, false },
    { UINT8_C(0x03), UINT8_C(0xFF), NVIDIA_ACCESS_CONFIGURATION, false },
    { UINT8_C(0x03), UINT8_C(0xFF), NVIDIA_ACCESS_CONFIGURATION, false },
    { UINT8_C(0x03), UINT8_C(0xFF), NVIDIA_ACCESS_CONFIGURATION, false },
    { UINT8_C(0x03), UINT8_C(0xFF), NVIDIA_ACCESS_CONFIGURATION, false },
    { UINT8_C(0x03), UINT8_C(0xFF), NVIDIA_ACCESS_CONFIGURATION, false },
    { UINT8_C(0x03), UINT8_C(0xFF), NVIDIA_ACCESS_CONFIGURATION, false },
    { UINT8_C(0x03), UINT8_C(0xFF), NVIDIA_ACCESS_MEMORY, false }
};

static void nvidia_require_pure_layer(void)
{
    size_t controls = 0U;
    size_t reference_length = 0U;
    const uint8_t *reference;
    uint32_t writers = 0U;

    if (!kernel_test_nvidia_exit_self_test()) {
        kernel_test_fail("the NVIDIA exit contract drifted");
    }
    if (!nvidia_foundation_self_test(&controls) ||
        controls != NVIDIA_CONTROLLED_CONTROLS) {
        kernel_test_fail("the NVIDIA foundation controls did not all pass");
    }
    if (nvidia_driver_count() != NVIDIA_DRIVER_COUNT) {
        kernel_test_fail("the NVIDIA driver table changed size");
    }
    for (size_t index = 0U; index < NVIDIA_DRIVER_COUNT; ++index) {
        if (nvidia_driver_name(index) == NULL ||
            nvidia_driver_class(index) !=
                nvidia_declared_drivers[index].class_code ||
            nvidia_driver_subclass(index) !=
                nvidia_declared_drivers[index].subclass ||
            nvidia_driver_access(index) !=
                nvidia_declared_drivers[index].access ||
            nvidia_driver_interface(index) != UINT8_C(0xFF) ||
            nvidia_driver_writes_registers(index) !=
                nvidia_declared_drivers[index].writes_registers) {
            kernel_test_fail("an NVIDIA driver is not the one declared");
        }
        if (nvidia_driver_writes_registers(index)) {
            ++writers;
        }
    }
    if (writers != 1U) {
        kernel_test_fail("the NVIDIA drivers gained a second writer");
    }
    /*
     * Every published encoding, decoded again here rather than trusted from
     * the driver's own self-test.
     */
    for (size_t index = 0U; index < NVIDIA_PUBLISHED_ENCODINGS; ++index) {
        const struct nvidia_identity identity =
            nvidia_decode_identity(nvidia_published_encodings[index].boot0);

        if (!identity.recognized ||
            identity.chipset != nvidia_published_encodings[index].chipset ||
            identity.architecture !=
                nvidia_published_encodings[index].architecture ||
            identity.revision !=
                (nvidia_published_encodings[index].boot0 & UINT32_C(0xFF)) ||
            identity.family != (identity.chipset & UINT32_C(0x1F0))) {
            kernel_test_fail("a published NVIDIA encoding decoded wrongly");
        }
    }
    /* An absent aperture and a dead bus are both refused, never guessed. */
    if (nvidia_decode_identity(0U).recognized ||
        nvidia_decode_identity(UINT32_MAX).recognized ||
        nvidia_decode_identity(UINT32_C(0x180000A1)).recognized) {
        kernel_test_fail("the NVIDIA decode invented a part");
    }
    reference = nvidia_reference_vbios(&reference_length);
    if (reference == NULL || reference_length != 1024U ||
        reference[0] != UINT8_C(0x55) || reference[1] != UINT8_C(0xAA) ||
        reference[0x40] != (uint8_t)'P' || reference[0x41] != (uint8_t)'C' ||
        reference[0x42] != (uint8_t)'I' || reference[0x43] != (uint8_t)'R' ||
        reference[0x44] != UINT8_C(0xDE) || reference[0x45] != UINT8_C(0x10) ||
        reference[0x100] != UINT8_C(0xFF) ||
        reference[0x101] != UINT8_C(0xB8) ||
        reference[0x102] != (uint8_t)'B') {
        kernel_test_fail("the reference VBIOS image is not the pinned one");
    }
    if (!nvidia_resources_released()) {
        kernel_test_fail("the NVIDIA drivers are holding a claim");
    }
}

_Noreturn void kernel_test_complete_nvidia(void)
{
    const struct boot_ledger *ledger = boot_ledger_installed();
    const struct boot_stage_receipt *foundation;
    const struct boot_stage_receipt *receipt;

    if (active_scenario != KERNEL_TEST_NVIDIA &&
        active_scenario != KERNEL_TEST_NVIDIA_BUILTIN) {
        kernel_test_fail("NVIDIA completion used outside its scenario");
    }
    foundation = boot_ledger_receipt_for(ledger, BOOT_STAGE_NVIDIA_FOUNDATION);
    receipt = boot_ledger_receipt_for(ledger, BOOT_STAGE_NVIDIA_PROBE);
    if (ledger == NULL || foundation == NULL || receipt == NULL ||
        foundation->result != BOOT_RECEIPT_RAN ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_NVIDIA_FOUNDATION_AVAILABLE)) {
        kernel_test_fail("the NVIDIA foundation receipt is invalid");
    }
    nvidia_require_pure_layer();

    if (active_scenario == KERNEL_TEST_NVIDIA_BUILTIN) {
        /*
         * The probe was never asked for, so the ledger must say it was
         * skipped and say why, and nothing may have been claimed.
         */
        if (receipt->result != BOOT_RECEIPT_SKIPPED ||
            !boot_ledger_has_capability(ledger,
                BOOT_CAPABILITY_NVIDIA_FUNCTIONS_ABSENT) ||
            boot_ledger_has_capability(ledger,
                BOOT_CAPABILITY_NVIDIA_PROBE_COMPLETE)) {
            kernel_test_fail("a skipped NVIDIA probe was not recorded as one");
        }
        if (pci_resource_get_state().active_claims != 0U ||
            pci_resource_get_state().active_mappings != 0U ||
            dma_get_state().active_allocations != 0U) {
            kernel_test_fail("the skipped NVIDIA probe still took resources");
        }
        kernel_test_pass();
    }

    {
        const struct nvidia_result probe = nvidia_get_result();

        if (receipt->result != BOOT_RECEIPT_RAN ||
            receipt->proof_counter_count != 2U ||
            receipt->proof_counters[0] != probe.bound ||
            receipt->proof_counters[1] != probe.controls ||
            !boot_ledger_has_capability(ledger,
                BOOT_CAPABILITY_NVIDIA_PROBE_COMPLETE) ||
            boot_ledger_has_capability(ledger,
                BOOT_CAPABILITY_NVIDIA_FUNCTIONS_ABSENT)) {
            kernel_test_fail("the NVIDIA probe receipt is invalid");
        }
        if (probe.declared != NVIDIA_DRIVER_COUNT ||
            probe.controls != NVIDIA_CONTROLLED_CONTROLS ||
            probe.bound != probe.present ||
            !probe.every_present_function_bound ||
            !probe.teardown_complete || !probe.resource_census_equal) {
            kernel_test_fail("the NVIDIA probe is inconsistent");
        }
        /* With no NVIDIA function present, no register or resource is touched. */
        if (!probe.any_function_present) {
            /*
             * Absence has to be a refusal rather than an empty machine. This
             * scenario attaches display and HD Audio functions of exactly the
             * classes these drivers match on, from vendors that are not
             * NVIDIA, so "no function present" means every one of them was
             * turned down on the one field that decides it.
             */
            size_t candidates = 0U;

            for (size_t index = 0U; index < pci_function_count(); ++index) {
                const struct pci_function *function = pci_function_at(index);

                if (function == NULL) {
                    continue;
                }
                if (function->vendor_id == NVIDIA_VENDOR_ID) {
                    kernel_test_fail(
                        "an NVIDIA function was present and reported absent");
                }
                for (size_t driver = 0U; driver < NVIDIA_DRIVER_COUNT;
                     ++driver) {
                    const uint8_t subclass = nvidia_driver_subclass(driver);

                    if (function->class_code != nvidia_driver_class(driver)) {
                        continue;
                    }
                    if (subclass == UINT8_C(0xFF) ?
                            (function->subclass == 0U ||
                                function->subclass == 2U) :
                            function->subclass == subclass) {
                        ++candidates;
                        break;
                    }
                }
            }
            if (candidates < 3U) {
                kernel_test_fail("the NVIDIA refusal had nothing to refuse");
            }
            if (probe.present != 0U || probe.bound != 0U ||
                probe.register_reads != 0U || probe.register_writes != 0U ||
                probe.identity.recognized || probe.vbios_valid ||
                probe.identity.boot0 != 0U) {
                kernel_test_fail("an absent NVIDIA board reported readings");
            }
            for (size_t index = 0U; index < NVIDIA_DRIVER_COUNT; ++index) {
                if (probe.probes[index].present ||
                    probe.probes[index].bound ||
                    probe.probes[index].identity != 0U) {
                    kernel_test_fail("an absent NVIDIA driver reported a bind");
                }
            }
        } else {
            /*
             * Never yet observed on any machine this has run on. If it ever
             * is, every bound driver has to have read something, and only the
             * video BIOS driver may have written.
             */
            for (size_t index = 0U; index < NVIDIA_DRIVER_COUNT; ++index) {
                const struct nvidia_driver_probe *entry = &probe.probes[index];

                if (!entry->bound) {
                    continue;
                }
                if (entry->vendor_id != NVIDIA_VENDOR_ID ||
                    (entry->register_writes != 0U &&
                        !nvidia_driver_writes_registers(index))) {
                    kernel_test_fail("a bound NVIDIA driver broke its bounds");
                }
                /*
                 * The aperture driver reads the BAR descriptions a claim
                 * produced and touches no register at all, which is a
                 * stronger statement than "it read something" and is worth
                 * asserting as exactly zero. Everything else has to have
                 * actually asked the device a question.
                 */
                if (nvidia_driver_access(index) ==
                        NVIDIA_ACCESS_APERTURE) {
                    if (entry->register_reads != 0U ||
                        entry->register_writes != 0U) {
                        kernel_test_fail(
                            "the NVIDIA aperture driver touched a register");
                    }
                } else if (entry->register_reads == 0U) {
                    kernel_test_fail("a bound NVIDIA driver read nothing");
                }
            }
            if (!probe.identity.recognized) {
                kernel_test_fail("a present NVIDIA board was not identified");
            }
        }
        if (pci_resource_get_state().active_claims != 0U ||
            pci_resource_get_state().bus_masters != 0U ||
            dma_get_state().active_allocations != 0U ||
            !nvidia_resources_released()) {
            kernel_test_fail("the NVIDIA probe left the machine holding a claim");
        }
    }
    kernel_test_pass();
}

bool kernel_test_handle_fatal_interrupt(const struct interrupt_frame *frame)
{
    bool matches = false;

    if (frame == NULL) {
        return false;
    }

    switch (active_scenario) {
    case KERNEL_TEST_INVALID_OPCODE:
        matches = frame->vector == 6U &&
            frame->error_code == 0U &&
            frame->rip == (uintptr_t)(const void *)interrupt_invalid_opcode_site;
        break;
    case KERNEL_TEST_PAGE_FAULT:
        matches = frame->vector == 14U &&
            frame->error_code == 0U &&
            frame->cr2 == PAGE_FAULT_TEST_ADDRESS &&
            frame->rip == (uintptr_t)(const void *)interrupt_page_fault_site;
        break;
    case KERNEL_TEST_UNEXPECTED:
        matches = frame->vector == UINT64_C(0x80) && frame->error_code == 0U;
        break;
    case KERNEL_TEST_PAGING:
        matches = frame->vector == 14U &&
            frame->error_code == PAGING_TEST_FAULT_ERROR_CODE &&
            frame->cr2 == PAGING_PROBE_ADDRESS &&
            frame->rip == (uintptr_t)(const void *)paging_probe_write_site;
        break;
    case KERNEL_TEST_HEAP:
        matches = frame->vector == 14U &&
            frame->error_code == HEAP_TEST_FAULT_ERROR_CODE &&
            frame->cr2 == HEAP_GUARD_ABOVE &&
            frame->rip == (uintptr_t)(const void *)paging_probe_write_site;
        break;
    case KERNEL_TEST_THREAD_GUARD:
        /*
         * The fault is taken on the created thread's own stack, so this also
         * proves the fault path works at all on a stack this layer allocated
         * rather than only on the one boot.S set up.
         */
        matches = frame->vector == 14U &&
            frame->error_code == THREAD_GUARD_TEST_ERROR_CODE &&
            frame->cr2 == THREAD_GUARD_TEST_ADDRESS &&
            frame->rip == (uintptr_t)(const void *)paging_probe_write_site;
        break;
    default:
        return false;
    }

    if (!matches) {
        kernel_test_fail("fatal interrupt did not match its expectation");
    }

    kernel_test_pass();
}

const char *kernel_test_scenario_name(enum kernel_test_scenario scenario)
{
    switch (scenario) {
    case KERNEL_TEST_NONE:
        return "none";
    case KERNEL_TEST_NORMAL:
        return "normal";
    case KERNEL_TEST_BREAKPOINT:
        return "breakpoint";
    case KERNEL_TEST_INVALID_OPCODE:
        return "invalid-opcode";
    case KERNEL_TEST_PAGE_FAULT:
        return "page-fault";
    case KERNEL_TEST_IST:
        return "ist";
    case KERNEL_TEST_PIT:
        return "pit";
    case KERNEL_TEST_UNEXPECTED:
        return "unexpected";
    case KERNEL_TEST_DOUBLE_FAULT:
        return "double-fault";
    case KERNEL_TEST_APIC:
        return "apic";
    case KERNEL_TEST_IOAPIC:
        return "ioapic";
    case KERNEL_TEST_IOAPIC_LEVEL:
        return "ioapic-level";
    case KERNEL_TEST_RETIRED:
        return "retired";
    case KERNEL_TEST_APIC_TIMER:
        return "apic-timer";
    case KERNEL_TEST_TSC:
        return "tsc";
    case KERNEL_TEST_PM_TIMER:
        return "pm-timer";
    case KERNEL_TEST_PIT_RETIRED:
        return "pit-retired";
    case KERNEL_TEST_TIMERS:
        return "timers";
    case KERNEL_TEST_PAGING:
        return "paging";
    case KERNEL_TEST_HEAP:
        return "heap";
    case KERNEL_TEST_PCI:
        return "pci";
    case KERNEL_TEST_PCI_ECAM:
        return "pci-ecam";
    case KERNEL_TEST_THREADS:
        return "threads";
    case KERNEL_TEST_THREAD_GUARD:
        return "thread-guard";
    case KERNEL_TEST_FRAMEBUFFER:
        return "framebuffer";
    case KERNEL_TEST_SCREEN:
        return "screen";
    case KERNEL_TEST_KEYBOARD:
        return "keyboard";
    case KERNEL_TEST_SHELL:
        return "shell";
    case KERNEL_TEST_SURFACE:
        return "surface";
    case KERNEL_TEST_WRITE_COMBINING:
        return "write-combining";
    case KERNEL_TEST_DEVICE_WINDOWS:
        return "device-windows";
    case KERNEL_TEST_BOOT_LEDGER:
        return "boot-ledger";
    case KERNEL_TEST_PHIPIA_PROOF:
        return "phipia-proof";
    case KERNEL_TEST_DEVICE_SUBSTRATE:
        return "device-substrate";
    case KERNEL_TEST_XHCI:
        return "xhci";
    case KERNEL_TEST_NVME:
        return "nvme";
    case KERNEL_TEST_FILESYSTEM:
        return "filesystem";
    case KERNEL_TEST_PROCESS:
        return "process";
    case KERNEL_TEST_LINUX_ABI:
        return "linux-abi";
    case KERNEL_TEST_LINUX_ABI_UNAME:
        return "linux-abi-uname";
    case KERNEL_TEST_PHIPIA_PROOF_USERLAND:
        return "phipia-proof-userland";
    case KERNEL_TEST_PHIPIA_PROOF_USERLAND_ABSENT:
        return "phipia-proof-userland-absent";
    case KERNEL_TEST_PHIPIA_PROOF_USERLAND_INTERACTIVE:
        return "phipia-proof-userland-interactive";
    case KERNEL_TEST_PHIPIA_PROOF_USERLAND_INTERACTIVE_ABSENT:
        return "phipia-proof-userland-interactive-absent";
    case KERNEL_TEST_FAT32_SYSTEM:
        return "fat32-system";
    case KERNEL_TEST_FAT32_DATA:
        return "fat32-data";
    case KERNEL_TEST_FAT32_NESTED:
        return "fat32-nested";
    case KERNEL_TEST_FAT32_GROWTH:
        return "fat32-growth";
    case KERNEL_TEST_FAT32_RANDOM:
        return "fat32-random";
    case KERNEL_TEST_FAT32_TRUNCATE:
        return "fat32-truncate";
    case KERNEL_TEST_FAT32_RENAME:
        return "fat32-rename";
    case KERNEL_TEST_FAT32_DELETE:
        return "fat32-delete";
    case KERNEL_TEST_FAT32_FULL:
        return "fat32-full";
    case KERNEL_TEST_FAT32_CORRUPT:
        return "fat32-corrupt";
    case KERNEL_TEST_FAT32_MISSING:
        return "fat32-missing";
    case KERNEL_TEST_FAT32_PERSISTENCE:
        return "fat32-persistence";
    case KERNEL_TEST_FAT32_CACHE:
        return "fat32-cache";
    case KERNEL_TEST_FAT32_IMMUTABLE:
        return "fat32-immutable";
    case KERNEL_TEST_FAT32_HANDLES:
        return "fat32-handles";
    case KERNEL_TEST_NETWORK_NIC_DISCOVERY:
        return "network-nic-discovery";
    case KERNEL_TEST_NETWORK_NIC_INITIALIZATION:
        return "network-nic-initialization";
    case KERNEL_TEST_NETWORK_NIC_ABSENT:
        return "network-nic-absent";
    case KERNEL_TEST_NETWORK_LINK_DOWN:
        return "network-link-down";
    case KERNEL_TEST_NETWORK_DHCP:
        return "network-dhcp";
    case KERNEL_TEST_NETWORK_DHCP_TIMEOUT:
        return "network-dhcp-timeout";
    case KERNEL_TEST_NETWORK_STATIC:
        return "network-static";
    case KERNEL_TEST_NETWORK_ARP:
        return "network-arp";
    case KERNEL_TEST_NETWORK_ICMP:
        return "network-icmp";
    case KERNEL_TEST_NETWORK_ICMP_TIMEOUT:
        return "network-icmp-timeout";
    case KERNEL_TEST_NETWORK_UDP:
        return "network-udp";
    case KERNEL_TEST_NETWORK_DNS_A:
        return "network-dns-a";
    case KERNEL_TEST_NETWORK_DNS_CNAME:
        return "network-dns-cname";
    case KERNEL_TEST_NETWORK_DNS_MALFORMED:
        return "network-dns-malformed";
    case KERNEL_TEST_NETWORK_TCP:
        return "network-tcp";
    case KERNEL_TEST_NETWORK_TCP_RETRANSMIT:
        return "network-tcp-retransmit";
    case KERNEL_TEST_NETWORK_TCP_RESET:
        return "network-tcp-reset";
    case KERNEL_TEST_NETWORK_HTTP_LENGTH:
        return "network-http-length";
    case KERNEL_TEST_NETWORK_HTTP_CHUNKED:
        return "network-http-chunked";
    case KERNEL_TEST_NETWORK_HTTP_REDIRECT:
        return "network-http-redirect";
    case KERNEL_TEST_NETWORK_HTTP_MALFORMED:
        return "network-http-malformed";
    case KERNEL_TEST_NETWORK_HTTP_NESTED:
        return "network-http-nested";
    case KERNEL_TEST_NETWORK_HTTP_REPLACE:
        return "network-http-replace";
    case KERNEL_TEST_NETWORK_HTTP_DISK_FULL:
        return "network-http-disk-full";
    case KERNEL_TEST_NETWORK_NIC_RESET:
        return "network-nic-reset";
    case KERNEL_TEST_NETWORK_SYSTEM_IMMUTABLE:
        return "network-system-immutable";
    case KERNEL_TEST_NETWORK_MISSING_LINUX_ECHO:
        return "network-missing-linux-echo";
    case KERNEL_TEST_NETWORK_MISSING_LINUX_UNAME:
        return "network-missing-linux-uname";
    case KERNEL_TEST_NETWORK_MISSING_LINUX_CAT:
        return "network-missing-linux-cat";
    case KERNEL_TEST_NETWORK_FILES:
        return "network-files";
    case KERNEL_TEST_NETWORK_NOTES:
        return "network-notes";
    case KERNEL_TEST_NETWORK_MEDIA_EDITOR:
        return "network-media-editor";
    case KERNEL_TEST_NETWORK_PERSISTENCE:
        return "network-persistence";
    case KERNEL_TEST_NETWORK_SOCKET_ISOLATION:
        return "network-socket-isolation";
    case KERNEL_TEST_NETWORK_TCP_LISTEN:
        return "network-tcp-listen";
    case KERNEL_TEST_NETWORK_TCP_REFUSED:
        return "network-tcp-refused";
    case KERNEL_TEST_MULTIPROCESS:
        return "multiprocess";
    case KERNEL_TEST_MULTIPROCESS_SLOTS:
        return "multiprocess-slots";
    case KERNEL_TEST_DRIVER_MATRIX:
        return "driver-matrix";
    case KERNEL_TEST_DRIVER_MATRIX_BUILTIN:
        return "driver-matrix-builtin";
    case KERNEL_TEST_AUDIO:
        return "audio";
    case KERNEL_TEST_NVIDIA:
        return "nvidia";
    case KERNEL_TEST_NVIDIA_BUILTIN:
        return "nvidia-builtin";
    case KERNEL_TEST_NATIVE:
        return "native";
    case KERNEL_TEST_NATIVE_LUA:
        return "native-lua";
    case KERNEL_TEST_NATIVE_SQLITE:
        return "native-sqlite";
    case KERNEL_TEST_NATIVE_CANVAS:
        return "native-canvas";
    case KERNEL_TEST_NATIVE_NETWORK:
        return "network-native";
    case KERNEL_TEST_NATIVE_RUST:
        return "native-rust";
    case KERNEL_TEST_NATIVE_CRASH:
        return "native-crash";
    case KERNEL_TEST_NATIVE_ELF_REFUSAL:
        return "native-elf-refusal";
    case KERNEL_TEST_NATIVE_DIGEST_REFUSAL:
        return "native-digest-refusal";
    case KERNEL_TEST_NATIVE_ABI_REFUSAL:
        return "native-abi-refusal";
    case KERNEL_TEST_NATIVE_RELAUNCH:
        return "native-relaunch";
    case KERNEL_TEST_NATIVE_AUDIO:
        return "native-audio";
    case KERNEL_TEST_NATIVE_SDL:
        return "native-sdl";
    case KERNEL_TEST_NATIVE_DYNAMIC:
        return "native-dynamic";
    case KERNEL_TEST_NATIVE_HTTPS:
        return "native-https";
    case KERNEL_TEST_NATIVE_PHIP:
        return "native-phip";
    case KERNEL_TEST_EXT4_RECOVERY:
        return "ext4-recovery";
    case KERNEL_TEST_INVALID:
        return "invalid";
    default:
        return "unknown";
    }
}

_Noreturn void kernel_test_fail(const char *reason)
{
    console_write("ST FAIL ");
    console_write(kernel_test_scenario_name(active_scenario));
    console_write(": ");
    console_write(reason);
    console_putc('\n');
    cpu_out32(QEMU_EXIT_PORT, QEMU_FAILURE_VALUE);
    console_halt();
}
