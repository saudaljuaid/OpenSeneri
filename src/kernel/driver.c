/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Bounded PCI drivers and their match table. Configuration-only drivers are
 * read-only. MMIO drivers claim a function, map one BAR uncached, perform a
 * bounded operation, then unmap and release it. These drivers do not enable
 * bus mastering.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/clock.h>
#include <phipia/cpu.h>
#include <phipia/dma.h>
#include <phipia/driver.h>
#include <phipia/interrupt_vector.h>
#include <phipia/memory.h>
#include <phipia/msix.h>
#include <phipia/paging.h>
#include <phipia/pci.h>
#include <phipia/pci_resource.h>

/* PCI Code and ID Assignment Specification 1.19 section 1. */
#define DRIVER_CLASS_MASS_STORAGE UINT8_C(0x01)
#define DRIVER_CLASS_NETWORK UINT8_C(0x02)
#define DRIVER_CLASS_MULTIMEDIA UINT8_C(0x04)
#define DRIVER_CLASS_BRIDGE UINT8_C(0x06)
#define DRIVER_CLASS_DISPLAY UINT8_C(0x03)
#define DRIVER_CLASS_SERIAL_BUS UINT8_C(0x0C)
#define DRIVER_SUBCLASS_VGA UINT8_C(0x00)
#define DRIVER_SUBCLASS_OTHER_DISPLAY UINT8_C(0x80)
#define DRIVER_SUBCLASS_USB UINT8_C(0x03)
#define DRIVER_SUBCLASS_SCSI UINT8_C(0x00)
#define DRIVER_SUBCLASS_IDE UINT8_C(0x01)
#define DRIVER_SUBCLASS_SATA UINT8_C(0x06)
#define DRIVER_SUBCLASS_ETHERNET UINT8_C(0x00)
#define DRIVER_SUBCLASS_HD_AUDIO UINT8_C(0x03)
#define DRIVER_SUBCLASS_HOST_BRIDGE UINT8_C(0x00)
#define DRIVER_SUBCLASS_ISA_BRIDGE UINT8_C(0x01)
#define DRIVER_SUBCLASS_OTHER_BRIDGE UINT8_C(0x80)

/*
 * Intel 8254x Gigabit Ethernet Controller Software Developer's Manual,
 * sections 13.4.1, 13.4.2, 13.4.4 and 13.4.28. CTRL.RST is self-clearing:
 * the device clears it when the reset completes, which is the only completion
 * signal the device offers and therefore the only one worth waiting on.
 */
#define E1000_CTRL UINT64_C(0x0000)
#define E1000_STATUS UINT64_C(0x0008)
#define E1000_EERD UINT64_C(0x0014)
#define E1000_RAL0 UINT64_C(0x5400)
#define E1000_RAH0 UINT64_C(0x5404)
#define E1000_CTRL_RST UINT32_C(0x04000000)
#define E1000_RAH_ADDRESS_VALID UINT32_C(0x80000000)

/*
 * The same manual, section 5.6.1: every word of the EEPROM's first sixty-four,
 * added together modulo sixteen bits, is 0xBABA. That is a property of the
 * part's own non-volatile contents rather than of anything the driver did, so
 * a device whose EEPROM does not sum to it is not an Intel Gigabit part
 * however convincing its identifiers are.
 */
#define E1000_EEPROM_CHECKSUM_WORDS 64U
#define E1000_EEPROM_CHECKSUM UINT32_C(0xBABA)

/* IEEE 802-2014 clause 8.2: bit 0 of the first octet marks a group address. */
#define ETHERNET_GROUP_ADDRESS_BIT UINT64_C(0x01)
#define ETHERNET_BROADCAST_ADDRESS UINT64_C(0xFFFFFFFFFFFF)

/* 82540EM EEPROM read register: start bit 0, done bit 4, address from bit 8. */
#define E1000_EERD_START UINT32_C(0x00000001)
#define E1000_EERD_DONE UINT32_C(0x00000010)
#define E1000_EERD_ADDRESS_SHIFT 8U

/*
 * Intel 82574 GbE Controller Family Datasheet section 8.4: the same register
 * moved its done bit to 1 and its address field to bit 2, which is exactly the
 * kind of difference two drivers for two devices from one vendor exist for.
 */
#define E1000E_EERD_START UINT32_C(0x00000001)
#define E1000E_EERD_DONE UINT32_C(0x00000002)
#define E1000E_EERD_ADDRESS_SHIFT 2U
#define E1000_EERD_DATA_SHIFT 16U
#define E1000_EEPROM_MAC_WORDS 3U

/* Serial ATA AHCI 1.3.1 specification section 3.1 and 3.1.2. */
#define AHCI_CAP UINT64_C(0x00)
#define AHCI_GHC UINT64_C(0x04)
#define AHCI_PI UINT64_C(0x0C)
#define AHCI_VS UINT64_C(0x10)
#define AHCI_GHC_HBA_RESET UINT32_C(0x00000001)
#define AHCI_GHC_AHCI_ENABLE UINT32_C(0x80000000)
#define AHCI_CAP_PORT_MASK UINT32_C(0x0000001F)

/* High Definition Audio Specification 1.0a sections 3.3.1 through 3.3.9. */
#define HDA_GCAP UINT64_C(0x00)
#define HDA_VMIN UINT64_C(0x02)
#define HDA_VMAJ UINT64_C(0x03)
#define HDA_GCTL UINT64_C(0x08)
#define HDA_STATESTS UINT64_C(0x0E)
#define HDA_GCTL_CONTROLLER_RESET UINT32_C(0x00000001)

/* Realtek RTL8139D Registers, revision 1.4, sections 5.1, 5.9 and 5.11. */
#define RTL8139_IDR0 UINT64_C(0x00)
#define RTL8139_COMMAND UINT64_C(0x37)
#define RTL8139_TRANSMIT_CONFIGURATION UINT64_C(0x40)
#define RTL8139_CONFIG1 UINT64_C(0x52)
#define RTL8139_COMMAND_RESET UINT8_C(0x10)
#define RTL8139_TCR_HARDWARE_HIGH UINT32_C(0x7C000000)
#define RTL8139_TCR_HARDWARE_LOW UINT32_C(0x00C00000)

/*
 * AMD Am79C970A PCnet-PCI II datasheet, sections 4.2 and 5.2. In 32-bit
 * ("DWIO") mode the register file is at a fixed stride, and the chip is put in
 * that mode by a 32-bit write to the register data port while it is still in
 * 16-bit mode - the one documented way to change the access width.
 */
#define PCNET_APROM UINT64_C(0x00)
/* 16-bit ("WIO") register file, which is the mode the part powers up in. */
#define PCNET_WIO_RDP UINT64_C(0x10)
#define PCNET_WIO_RAP UINT64_C(0x12)
#define PCNET_WIO_RESET UINT64_C(0x14)
/* 32-bit ("DWIO") register file, at the wider stride. */
#define PCNET_RDP UINT64_C(0x10)
#define PCNET_RAP UINT64_C(0x14)
#define PCNET_CSR_CHIP_ID_LOW 88U
#define PCNET_CSR_CHIP_ID_HIGH 89U
#define PCNET_CHIP_ID_FIXED_BIT UINT32_C(0x00000001)
#define PCNET_CHIP_ID_MANUFACTURER_SHIFT 1U
#define PCNET_CHIP_ID_MANUFACTURER_MASK UINT32_C(0x000007FF)
#define PCNET_CHIP_ID_PART_SHIFT 12U
#define PCNET_CHIP_ID_PART_MASK UINT32_C(0x0000FFFF)
#define PCNET_MANUFACTURER_AMD UINT32_C(0x001)

/*
 * Enhanced Host Controller Interface for USB 2.0, revision 1.0, sections 2.2
 * and 2.3. The capability registers say where the operational registers start,
 * so a driver that assumes a fixed offset is a driver that works by accident.
 * HCRESET is self-clearing and clears the operational registers with it.
 */
#define EHCI_CAPLENGTH UINT64_C(0x00)
#define EHCI_HCIVERSION UINT64_C(0x02)
#define EHCI_HCSPARAMS UINT64_C(0x04)
#define EHCI_HCCPARAMS UINT64_C(0x08)
#define EHCI_USBCMD UINT64_C(0x00)
#define EHCI_USBSTS UINT64_C(0x04)
#define EHCI_USBCMD_HOST_CONTROLLER_RESET UINT32_C(0x00000002)
#define EHCI_USBSTS_HALTED UINT32_C(0x00001000)
#define EHCI_HCSPARAMS_PORT_MASK UINT32_C(0x0000000F)
#define EHCI_MINIMUM_CAPLENGTH UINT8_C(0x08)
#define EHCI_INTERFACE_VERSION UINT16_C(0x0100)

/*
 * Cirrus Logic GD5446 technical reference, sections 4 and 6. The MMIO window
 * carries the VGA register file at its base, so a driver reads the miscellaneous
 * output register to learn which CRTC address pair is live before it touches
 * one - the same decision a VGA driver has made since 1987. The extension lock
 * exists so that software which does not know the part cannot reach its
 * extended registers, and it reports whether it is open, which makes it the
 * one register that proves a Cirrus part is answering.
 */
#define CIRRUS_VGA_MISCELLANEOUS_WRITE UINT64_C(0x02)
#define CIRRUS_VGA_MISCELLANEOUS_READ UINT64_C(0x0C)
#define CIRRUS_VGA_SEQUENCER_INDEX UINT64_C(0x04)
#define CIRRUS_VGA_SEQUENCER_DATA UINT64_C(0x05)
#define CIRRUS_VGA_COLOUR_CRTC_INDEX UINT64_C(0x14)
#define CIRRUS_MISCELLANEOUS_COLOUR_SELECT UINT8_C(0x01)
#define CIRRUS_SEQUENCER_UNLOCK_INDEX UINT8_C(0x06)
#define CIRRUS_SEQUENCER_UNLOCK_KEY UINT8_C(0x12)
#define CIRRUS_SEQUENCER_LOCKED UINT8_C(0x0F)
#define CIRRUS_CRTC_CHIP_IDENTIFIER UINT8_C(0x27)
#define CIRRUS_CHIP_IDENTIFIER_SHIFT 2U
#define CIRRUS_CHIP_GD5446 UINT8_C(0x2E)

/*
 * Bochs Display Interface, as implemented by Bochs, QEMU and VirtualBox and
 * driven in Linux by bochs-drm. The index registers live at a fixed offset
 * inside the memory-mapped window; the first of them is an interface
 * identifier naming the interface version, and a memory-size register that
 * has to agree with the prefetchable BAR the framebuffer actually lives in.
 */
#define BOCHS_DISPI_REGISTERS UINT64_C(0x500)
#define BOCHS_DISPI_ID 0U
#define BOCHS_DISPI_XRES 1U
#define BOCHS_DISPI_YRES 2U
#define BOCHS_DISPI_BPP 3U
#define BOCHS_DISPI_ENABLE 4U
#define BOCHS_DISPI_VIDEO_MEMORY_64K 10U
#define BOCHS_FRAMEBUFFER_BAR 0U
#define BOCHS_DISPI_ID0 UINT16_C(0xB0C0)
#define BOCHS_DISPI_ID5 UINT16_C(0xB0C5)

/* Intel 440FX PCIset 82441FX datasheet section 3.2.14 and 3.2.20. */
#define I440FX_PAM_BASE UINT16_C(0x59)
#define I440FX_PAM_REGISTERS 7U
#define I440FX_SMRAM UINT16_C(0x72)

/* Intel 82371AB PIIX4 datasheet sections 4.1.13, 4.1.16 and 4.2.x. */
#define PIIX_PIRQ_ROUTE UINT16_C(0x60)
#define PIIX_XBCS UINT16_C(0x4E)
#define PIIX_PIRQ_DISABLE UINT8_C(0x80)
#define PIIX_IDE_TIMING UINT16_C(0x40)
#define PIIX_IDE_TIMING_DECODE_ENABLE UINT16_C(0x8000)
#define PIIX_POWER_MANAGEMENT_BASE UINT16_C(0x40)
#define PIIX_POWER_MANAGEMENT_MISC UINT16_C(0x80)
#define PIIX_SMBUS_BASE UINT16_C(0x90)
#define PIIX_POWER_IO_ENABLE UINT32_C(0x00000001)
#define PIIX_IO_BASE_MASK UINT32_C(0x0000FFC0)

struct driver_record;

typedef enum driver_status (*driver_probe_t)(
    const struct driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct driver_probe *probe
);

struct driver_record {
    const char *name;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t bar_index;
    uint32_t minimum_register_bytes;
    enum driver_access access;
    /*
     * Whether this device's specification defines a reset the driver is
     * expected to perform at bind time. Stating it here rather than assuming
     * every mapped device has one keeps the contract with the matrix instead
     * of with whichever devices happen to be declared today.
     */
    bool defines_reset;
    driver_probe_t probe;
};

struct driver_census {
    struct frame_allocator_stats frames;
    struct paging_state paging;
    struct dma_state dma;
    struct pci_resource_state pci;
    struct interrupt_vector_state vectors;
    struct msix_state msix;
    bool interrupts_enabled;
};

static struct driver_matrix_result installed_result;
static bool matrix_active;
static uint32_t register_reads;
static uint32_t register_writes;

static void zero_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static uint8_t mmio_read8(volatile uint8_t *base, uint64_t offset)
{
    ++register_reads;
    return *(volatile uint8_t *)(void *)(base + offset);
}

static uint16_t mmio_read16(volatile uint8_t *base, uint64_t offset)
{
    ++register_reads;
    return *(volatile uint16_t *)(void *)(base + offset);
}

static uint32_t mmio_read32(volatile uint8_t *base, uint64_t offset)
{
    ++register_reads;
    return *(volatile uint32_t *)(void *)(base + offset);
}

static void mmio_write8(volatile uint8_t *base, uint64_t offset, uint8_t value)
{
    ++register_writes;
    *(volatile uint8_t *)(void *)(base + offset) = value;
}

static void mmio_write16(
    volatile uint8_t *base,
    uint64_t offset,
    uint16_t value
)
{
    ++register_writes;
    *(volatile uint16_t *)(void *)(base + offset) = value;
}

static void mmio_write32(
    volatile uint8_t *base,
    uint64_t offset,
    uint32_t value
)
{
    ++register_writes;
    *(volatile uint32_t *)(void *)(base + offset) = value;
}

static bool deadline_reached(uint64_t now, uint64_t deadline)
{
    return now >= deadline;
}

/*
 * Wait for a self-clearing or self-setting bit, bounded by the monotonic
 * clock. A device that never answers is a failed bind, never a hang: this is
 * the only loop in this file that does not run a fixed number of times.
 */
static bool wait_mask32(
    volatile uint8_t *base,
    uint64_t offset,
    uint32_t mask,
    uint32_t expected
)
{
    const uint64_t start = clock_monotonic_ns();
    const uint64_t deadline = start + DRIVER_MATRIX_RESET_TIMEOUT_NS;

    if (deadline < start) {
        return false;
    }
    while ((mmio_read32(base, offset) & mask) != expected) {
        if (deadline_reached(clock_monotonic_ns(), deadline)) {
            return false;
        }
        __asm__ volatile ("" : : : "memory");
    }
    return true;
}

static bool wait_mask8(
    volatile uint8_t *base,
    uint64_t offset,
    uint8_t mask,
    uint8_t expected
)
{
    const uint64_t start = clock_monotonic_ns();
    const uint64_t deadline = start + DRIVER_MATRIX_RESET_TIMEOUT_NS;

    if (deadline < start) {
        return false;
    }
    while ((mmio_read8(base, offset) & mask) != expected) {
        if (deadline_reached(clock_monotonic_ns(), deadline)) {
            return false;
        }
        __asm__ volatile ("" : : : "memory");
    }
    return true;
}

static bool configuration_dword(
    const struct pci_function *function,
    uint16_t offset,
    uint32_t *value
)
{
    ++register_reads;
    return pci_config_read_port(function->address, offset, value) ==
        PCI_STATUS_OK;
}

static bool configuration_byte(
    const struct pci_function *function,
    uint16_t offset,
    uint8_t *value
)
{
    uint32_t dword = 0U;

    if (!configuration_dword(function, (uint16_t)(offset & ~UINT16_C(3)),
            &dword)) {
        return false;
    }
    *value = (uint8_t)(dword >> ((offset & UINT16_C(3)) * 8U));
    return true;
}

static bool configuration_word(
    const struct pci_function *function,
    uint16_t offset,
    uint16_t *value
)
{
    uint32_t dword = 0U;

    if ((offset & UINT16_C(1)) != 0U ||
        !configuration_dword(function, (uint16_t)(offset & ~UINT16_C(3)),
            &dword)) {
        return false;
    }
    *value = (uint16_t)(dword >> ((offset & UINT16_C(2)) * 8U));
    return true;
}

/*
 * Intel 82540EM, one of the most widely deployed Gigabit parts ever made.
 * Reset the device the way its manual says, then read the station address from
 * the receive-address registers and the same address again out of the EEPROM,
 * and require the two to agree. One source could be anything; two sources that
 * agree are the device telling the truth about itself twice.
 */
static enum driver_status probe_e1000_family(
    const struct driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct driver_probe *probe,
    uint32_t done_bit,
    unsigned int address_shift
)
{
    uint32_t control;
    uint32_t status;
    uint32_t low;
    uint32_t high;
    uint32_t checksum = 0U;
    uint64_t station = 0U;
    uint64_t eeprom = 0U;

    (void)function;
    (void)claim;
    if (register_bytes < record->minimum_register_bytes) {
        return DRIVER_STATUS_REGISTER_WINDOW;
    }
    control = mmio_read32(registers, E1000_CTRL);
    mmio_write32(registers, E1000_CTRL, control | E1000_CTRL_RST);
    if (!wait_mask32(registers, E1000_CTRL, E1000_CTRL_RST, 0U)) {
        return DRIVER_STATUS_RESET_TIMEOUT;
    }
    probe->reset_observed = true;
    status = mmio_read32(registers, E1000_STATUS);
    low = mmio_read32(registers, E1000_RAL0);
    high = mmio_read32(registers, E1000_RAH0);
    if ((high & E1000_RAH_ADDRESS_VALID) == 0U) {
        return DRIVER_STATUS_IDENTITY;
    }
    station = (uint64_t)low | ((uint64_t)(high & UINT32_C(0xFFFF)) << 32U);
    for (uint32_t word = 0U; word < E1000_EEPROM_CHECKSUM_WORDS; ++word) {
        const uint32_t request = (word << address_shift) | E1000_EERD_START;
        uint32_t value;

        mmio_write32(registers, E1000_EERD, request);
        if (!wait_mask32(registers, E1000_EERD, done_bit, done_bit)) {
            return DRIVER_STATUS_RESET_TIMEOUT;
        }
        value = (mmio_read32(registers, E1000_EERD) >>
            E1000_EERD_DATA_SHIFT) & UINT32_C(0xFFFF);
        checksum = (checksum + value) & UINT32_C(0xFFFF);
        if (word < E1000_EEPROM_MAC_WORDS) {
            eeprom |= (uint64_t)value << (word * 16U);
        }
    }
    /*
     * Two independent sources have to name the same station address, the
     * address has to be a unicast one, and the non-volatile memory it came out
     * of has to check out. Any one of those alone could be a coincidence.
     */
    if (checksum != E1000_EEPROM_CHECKSUM || eeprom != station ||
        station == 0U || station == ETHERNET_BROADCAST_ADDRESS ||
        (station & ETHERNET_GROUP_ADDRESS_BIT) != 0U) {
        return DRIVER_STATUS_IDENTITY;
    }
    probe->identity = station;
    probe->detail = ((uint64_t)checksum << 32U) | (uint64_t)status;
    return DRIVER_STATUS_OK;
}

static enum driver_status probe_intel_82540em(
    const struct driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct driver_probe *probe
)
{
    return probe_e1000_family(record, function, claim, registers,
        register_bytes, probe, E1000_EERD_DONE, E1000_EERD_ADDRESS_SHIFT);
}

static enum driver_status probe_intel_82574l(
    const struct driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct driver_probe *probe
)
{
    return probe_e1000_family(record, function, claim, registers,
        register_bytes, probe, E1000E_EERD_DONE, E1000E_EERD_ADDRESS_SHIFT);
}

/*
 * Intel ICH9 SATA in AHCI mode. The host-bus adapter reset is the one
 * operation every AHCI driver performs before it looks at anything, and the
 * ports-implemented mask is what tells it how much hardware it just reset.
 */
static enum driver_status probe_intel_ich9_ahci(
    const struct driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct driver_probe *probe
)
{
    uint32_t capability;
    uint32_t version;
    uint32_t ports;
    uint32_t implemented = 0U;

    (void)function;
    (void)claim;
    if (register_bytes < record->minimum_register_bytes) {
        return DRIVER_STATUS_REGISTER_WINDOW;
    }
    mmio_write32(registers, AHCI_GHC,
        mmio_read32(registers, AHCI_GHC) | AHCI_GHC_HBA_RESET);
    if (!wait_mask32(registers, AHCI_GHC, AHCI_GHC_HBA_RESET, 0U)) {
        return DRIVER_STATUS_RESET_TIMEOUT;
    }
    probe->reset_observed = true;
    mmio_write32(registers, AHCI_GHC, AHCI_GHC_AHCI_ENABLE);
    if ((mmio_read32(registers, AHCI_GHC) & AHCI_GHC_AHCI_ENABLE) == 0U) {
        return DRIVER_STATUS_IDENTITY;
    }
    capability = mmio_read32(registers, AHCI_CAP);
    version = mmio_read32(registers, AHCI_VS);
    ports = mmio_read32(registers, AHCI_PI);
    for (unsigned int bit = 0U; bit < 32U; ++bit) {
        if ((ports & (UINT32_C(1) << bit)) != 0U) {
            ++implemented;
        }
    }
    /*
     * AHCI 1.3.1 section 3.1.1: CAP.NP is the highest port number, so the
     * number of implemented ports can never exceed it by more than the
     * zero-based encoding, and a controller with no ports is not a controller.
     */
    if ((version >> 16U) != 1U || implemented == 0U ||
        implemented > (capability & AHCI_CAP_PORT_MASK) + 1U) {
        return DRIVER_STATUS_IDENTITY;
    }
    probe->identity = (uint64_t)version;
    probe->detail = ((uint64_t)implemented << 32U) | (uint64_t)capability;
    return DRIVER_STATUS_OK;
}

/*
 * Intel ICH9 High Definition Audio. The controller comes out of reset only
 * when software takes it out: CRST is written low, observed low, written high
 * and observed high, which is the exact handshake the specification defines
 * and the only way to know the link is running.
 */
static enum driver_status probe_intel_ich9_hda(
    const struct driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct driver_probe *probe
)
{
    uint32_t control;
    uint16_t capability;
    uint8_t major;
    uint8_t minor;

    (void)function;
    (void)claim;
    if (register_bytes < record->minimum_register_bytes) {
        return DRIVER_STATUS_REGISTER_WINDOW;
    }
    control = mmio_read32(registers, HDA_GCTL);
    mmio_write32(registers, HDA_GCTL, control & ~HDA_GCTL_CONTROLLER_RESET);
    if (!wait_mask32(registers, HDA_GCTL, HDA_GCTL_CONTROLLER_RESET, 0U)) {
        return DRIVER_STATUS_RESET_TIMEOUT;
    }
    mmio_write32(registers, HDA_GCTL, HDA_GCTL_CONTROLLER_RESET);
    if (!wait_mask32(registers, HDA_GCTL, HDA_GCTL_CONTROLLER_RESET,
            HDA_GCTL_CONTROLLER_RESET)) {
        return DRIVER_STATUS_RESET_TIMEOUT;
    }
    probe->reset_observed = true;
    capability = mmio_read16(registers, HDA_GCAP);
    minor = mmio_read8(registers, HDA_VMIN);
    major = mmio_read8(registers, HDA_VMAJ);
    /*
     * High Definition Audio 1.0a section 3.3.3: the only published major
     * version is 1, and a controller with no streams at all is not one.
     */
    if (major != 1U || capability == 0U) {
        return DRIVER_STATUS_IDENTITY;
    }
    probe->identity = ((uint64_t)major << 8U) | (uint64_t)minor;
    probe->detail = ((uint64_t)mmio_read16(registers, HDA_STATESTS) << 32U) |
        (uint64_t)capability;
    return DRIVER_STATUS_OK;
}

/*
 * Realtek RTL8139. Its software reset is one bit in an eight-bit command
 * register that clears itself, and its station address is six bytes at the
 * very start of the register file - the simplest complete bind sequence of any
 * device here, which is exactly why the part was so widely cloned.
 */
static enum driver_status probe_realtek_rtl8139(
    const struct driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct driver_probe *probe
)
{
    uint64_t station = 0U;
    uint32_t transmit;

    (void)function;
    (void)claim;
    if (register_bytes < record->minimum_register_bytes) {
        return DRIVER_STATUS_REGISTER_WINDOW;
    }
    mmio_write8(registers, RTL8139_COMMAND, RTL8139_COMMAND_RESET);
    if (!wait_mask8(registers, RTL8139_COMMAND, RTL8139_COMMAND_RESET, 0U)) {
        return DRIVER_STATUS_RESET_TIMEOUT;
    }
    probe->reset_observed = true;
    for (unsigned int index = 0U; index < 6U; ++index) {
        station |= (uint64_t)mmio_read8(registers, RTL8139_IDR0 + index) <<
            (index * 8U);
    }
    transmit = mmio_read32(registers, RTL8139_TRANSMIT_CONFIGURATION);
    /*
     * RTL8139D registers 1.4 section 5.9: the hardware version identifier is
     * split across two fields of the transmit configuration register, and a
     * genuine part always names itself in at least one of them.
     */
    if (station == 0U || station == UINT64_C(0xFFFFFFFFFFFF) ||
        (transmit & (RTL8139_TCR_HARDWARE_HIGH | RTL8139_TCR_HARDWARE_LOW)) ==
            0U) {
        return DRIVER_STATUS_IDENTITY;
    }
    probe->identity = station;
    probe->detail = ((uint64_t)mmio_read8(registers, RTL8139_CONFIG1) << 32U) |
        (uint64_t)transmit;
    return DRIVER_STATUS_OK;
}

/*
 * AMD Am79C970A PCnet-PCI II. The address PROM sits at the base of the window
 * and needs nothing to read; the chip identity needs the device moved into
 * 32-bit register mode first, which is a real mode change with a real
 * observable consequence - the register address port starts reading back what
 * was written to it at the 32-bit stride.
 */
static enum driver_status probe_amd_pcnet(
    const struct driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct driver_probe *probe
)
{
    uint64_t station = 0U;
    uint32_t narrow_low;
    uint32_t narrow_high;
    uint32_t low;
    uint32_t high;
    uint32_t identifier;

    (void)function;
    (void)claim;
    if (register_bytes < record->minimum_register_bytes) {
        return DRIVER_STATUS_REGISTER_WINDOW;
    }
    for (unsigned int index = 0U; index < 6U; ++index) {
        station |= (uint64_t)mmio_read8(registers, PCNET_APROM + index) <<
            (index * 8U);
    }
    /*
     * S_RESET is a read, not a write: the datasheet gives the reset register
     * no data, and reading it is the whole operation. It also returns the part
     * to 16-bit mode, which is what makes the next check meaningful - the
     * address port answering at the narrow stride is the observable evidence
     * that the reset happened.
     */
    (void)mmio_read16(registers, PCNET_WIO_RESET);
    mmio_write16(registers, PCNET_WIO_RAP, (uint16_t)PCNET_CSR_CHIP_ID_LOW);
    if ((mmio_read16(registers, PCNET_WIO_RAP) & UINT16_C(0xFF)) !=
            PCNET_CSR_CHIP_ID_LOW) {
        return DRIVER_STATUS_RESET_TIMEOUT;
    }
    probe->reset_observed = true;
    narrow_low = mmio_read16(registers, PCNET_WIO_RDP);
    mmio_write16(registers, PCNET_WIO_RAP, (uint16_t)PCNET_CSR_CHIP_ID_HIGH);
    narrow_high = mmio_read16(registers, PCNET_WIO_RDP);

    /* One 32-bit write to the data port is the documented mode change. */
    mmio_write32(registers, PCNET_RDP, 0U);
    mmio_write32(registers, PCNET_RAP, PCNET_CSR_CHIP_ID_LOW);
    if ((mmio_read32(registers, PCNET_RAP) & UINT32_C(0xFF)) !=
            PCNET_CSR_CHIP_ID_LOW) {
        return DRIVER_STATUS_IDENTITY;
    }
    low = mmio_read32(registers, PCNET_RDP) & UINT32_C(0xFFFF);
    mmio_write32(registers, PCNET_RAP, PCNET_CSR_CHIP_ID_HIGH);
    high = mmio_read32(registers, PCNET_RDP) & UINT32_C(0xFFFF);
    if (low != narrow_low || high != narrow_high) {
        return DRIVER_STATUS_IDENTITY;
    }
    identifier = (high << 16U) | low;
    /*
     * Am79C970A datasheet section 5.2: bit 0 of the chip identity is always
     * one and bits 11:1 carry the JEDEC manufacturer number, which for AMD is
     * 0x001. A device that answers anything else is not this part.
     */
    if ((identifier & PCNET_CHIP_ID_FIXED_BIT) == 0U ||
        ((identifier >> PCNET_CHIP_ID_MANUFACTURER_SHIFT) &
            PCNET_CHIP_ID_MANUFACTURER_MASK) != PCNET_MANUFACTURER_AMD ||
        station == 0U || station == UINT64_C(0xFFFFFFFFFFFF)) {
        return DRIVER_STATUS_IDENTITY;
    }
    probe->identity = station;
    probe->detail = ((uint64_t)((identifier >> PCNET_CHIP_ID_PART_SHIFT) &
        PCNET_CHIP_ID_PART_MASK) << 32U) | (uint64_t)identifier;
    return DRIVER_STATUS_OK;
}

/*
 * Intel 82801DB USB 2.0 EHCI. Everything about an EHCI controller is behind
 * the capability length: the operational registers do not start at a fixed
 * offset, they start wherever the first byte of the register file says they
 * do. The reset is then the operational HCRESET bit, which clears itself and
 * leaves the controller halted, which is exactly the state a driver wants
 * before it builds a schedule.
 */
static enum driver_status probe_intel_ich4_ehci(
    const struct driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct driver_probe *probe
)
{
    uint8_t capability_length;
    uint16_t interface_version;
    uint32_t structural;
    uint32_t capabilities;
    uint32_t ports;
    uint64_t operational;

    (void)function;
    (void)claim;
    if (register_bytes < record->minimum_register_bytes) {
        return DRIVER_STATUS_REGISTER_WINDOW;
    }
    capability_length = mmio_read8(registers, EHCI_CAPLENGTH);
    interface_version = mmio_read16(registers, EHCI_HCIVERSION);
    structural = mmio_read32(registers, EHCI_HCSPARAMS);
    capabilities = mmio_read32(registers, EHCI_HCCPARAMS);
    ports = structural & EHCI_HCSPARAMS_PORT_MASK;
    operational = (uint64_t)capability_length;
    /*
     * EHCI 1.0 section 2.2.1 and 2.2.2: the capability registers are at least
     * eight bytes long, the operational registers have to fit inside the
     * window they point into, the interface version is 1.0, and a host
     * controller with no root ports is not a host controller.
     */
    if (capability_length < EHCI_MINIMUM_CAPLENGTH ||
        interface_version != EHCI_INTERFACE_VERSION || ports == 0U ||
        operational + EHCI_USBSTS + 4U > register_bytes) {
        return DRIVER_STATUS_IDENTITY;
    }
    mmio_write32(registers, operational + EHCI_USBCMD,
        EHCI_USBCMD_HOST_CONTROLLER_RESET);
    if (!wait_mask32(registers, operational + EHCI_USBCMD,
            EHCI_USBCMD_HOST_CONTROLLER_RESET, 0U)) {
        return DRIVER_STATUS_RESET_TIMEOUT;
    }
    probe->reset_observed = true;
    /* EHCI 1.0 section 2.3.2: a reset controller reports itself halted. */
    if ((mmio_read32(registers, operational + EHCI_USBSTS) &
            EHCI_USBSTS_HALTED) == 0U) {
        return DRIVER_STATUS_IDENTITY;
    }
    probe->identity = ((uint64_t)interface_version << 16U) |
        (uint64_t)capability_length;
    probe->detail = ((uint64_t)ports << 32U) | (uint64_t)capabilities;
    return DRIVER_STATUS_OK;
}

/*
 * Cirrus Logic GD5446. Its extended registers are behind a lock whose whole
 * purpose is to answer whether it is open, so opening it, reading the chip
 * identifier through the CRTC pair the miscellaneous output register selects,
 * and closing it again is a complete conversation with the part that leaves
 * nothing changed.
 */
static enum driver_status probe_cirrus_gd5446(
    const struct driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct driver_probe *probe
)
{
    uint8_t miscellaneous;
    uint8_t addressing;
    uint8_t unlocked;
    uint8_t relocked;
    uint8_t identifier;

    (void)function;
    (void)claim;
    if (register_bytes < record->minimum_register_bytes) {
        return DRIVER_STATUS_REGISTER_WINDOW;
    }
    /*
     * The CRTC answers at one of two address pairs and the miscellaneous
     * output register chooses which. Only the colour pair is inside this
     * memory window, so a driver that wants the CRTC through it has to select
     * colour addressing first - and put back whatever it found afterwards.
     */
    miscellaneous = mmio_read8(registers, CIRRUS_VGA_MISCELLANEOUS_READ);
    mmio_write8(registers, CIRRUS_VGA_MISCELLANEOUS_WRITE,
        miscellaneous | CIRRUS_MISCELLANEOUS_COLOUR_SELECT);
    addressing = mmio_read8(registers, CIRRUS_VGA_MISCELLANEOUS_READ);
    if ((addressing & CIRRUS_MISCELLANEOUS_COLOUR_SELECT) == 0U) {
        return DRIVER_STATUS_IDENTITY;
    }

    mmio_write8(registers, CIRRUS_VGA_SEQUENCER_INDEX,
        CIRRUS_SEQUENCER_UNLOCK_INDEX);
    mmio_write8(registers, CIRRUS_VGA_SEQUENCER_DATA,
        CIRRUS_SEQUENCER_UNLOCK_KEY);
    unlocked = mmio_read8(registers, CIRRUS_VGA_SEQUENCER_DATA);
    if (unlocked != CIRRUS_SEQUENCER_UNLOCK_KEY) {
        mmio_write8(registers, CIRRUS_VGA_MISCELLANEOUS_WRITE, miscellaneous);
        return DRIVER_STATUS_IDENTITY;
    }
    mmio_write8(registers, CIRRUS_VGA_COLOUR_CRTC_INDEX,
        CIRRUS_CRTC_CHIP_IDENTIFIER);
    identifier = mmio_read8(registers, CIRRUS_VGA_COLOUR_CRTC_INDEX + 1U);
    /* Put the lock and the addressing back before judging anything. */
    mmio_write8(registers, CIRRUS_VGA_SEQUENCER_INDEX,
        CIRRUS_SEQUENCER_UNLOCK_INDEX);
    mmio_write8(registers, CIRRUS_VGA_SEQUENCER_DATA, 0U);
    relocked = mmio_read8(registers, CIRRUS_VGA_SEQUENCER_DATA);
    mmio_write8(registers, CIRRUS_VGA_MISCELLANEOUS_WRITE, miscellaneous);
    /*
     * GD5446 technical reference section 6.1: the lock reads back the key when
     * open and 0x0F when closed, and the chip identifier's upper six bits name
     * the part while the lower two carry its revision.
     */
    if (relocked != CIRRUS_SEQUENCER_LOCKED ||
        (identifier >> CIRRUS_CHIP_IDENTIFIER_SHIFT) != CIRRUS_CHIP_GD5446) {
        return DRIVER_STATUS_IDENTITY;
    }
    probe->identity = (uint64_t)identifier;
    probe->detail = ((uint64_t)unlocked << 32U) | (uint64_t)miscellaneous;
    return DRIVER_STATUS_OK;
}

/*
 * The Bochs Display Interface. Software writes the interface version it wants
 * into the identifier register and reads back the version it actually got,
 * then asks the device for the largest mode it can set. Both are real
 * negotiations with real answers, and the second one is only meaningful
 * because the first one succeeded.
 */
static enum driver_status probe_bochs_display(
    const struct driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct driver_probe *probe
)
{
    volatile uint8_t *dispi = registers + BOCHS_DISPI_REGISTERS;
    const struct pci_bar_description *framebuffer;
    uint16_t identifier;
    uint16_t enable;
    uint16_t width;
    uint16_t height;
    uint16_t depth;
    uint16_t memory;

    (void)function;
    if (register_bytes < record->minimum_register_bytes || claim == NULL) {
        return DRIVER_STATUS_REGISTER_WINDOW;
    }
    framebuffer = pci_claim_bar(claim, BOCHS_FRAMEBUFFER_BAR);
    if (framebuffer == NULL || !framebuffer->implemented ||
        !framebuffer->prefetchable) {
        return DRIVER_STATUS_REGISTER_WINDOW;
    }
    identifier = mmio_read16(dispi, BOCHS_DISPI_ID * 2U);
    memory = mmio_read16(dispi, BOCHS_DISPI_VIDEO_MEMORY_64K * 2U);
    enable = mmio_read16(dispi, BOCHS_DISPI_ENABLE * 2U);
    width = mmio_read16(dispi, BOCHS_DISPI_XRES * 2U);
    height = mmio_read16(dispi, BOCHS_DISPI_YRES * 2U);
    depth = mmio_read16(dispi, BOCHS_DISPI_BPP * 2U);
    /*
     * The interface version and the memory behind it are what identify this
     * device; the mode registers describe whatever mode is currently set,
     * which on a display nothing has programmed yet is legitimately nothing.
     */
    probe->identity = ((uint64_t)memory << 16U) | (uint64_t)identifier;
    probe->detail = ((uint64_t)enable << 48U) | ((uint64_t)width << 32U) |
        ((uint64_t)height << 16U) | (uint64_t)depth;
    /*
     * Two windows of the same device have to agree. The interface register
     * reports how much video memory exists, in units of 64 KiB; the
     * prefetchable BAR is that memory. A driver that reads one and sizes its
     * framebuffer from the other is reading a device that is describing
     * itself consistently - and no amount of guessing produces two numbers
     * that match to the byte. This is a read-only bind: the display in front
     * of the machine is not somewhere to negotiate experimentally.
     */
    if (identifier < BOCHS_DISPI_ID0 || identifier > BOCHS_DISPI_ID5 ||
        memory == 0U ||
        (uint64_t)memory * UINT64_C(65536) != framebuffer->size) {
        return DRIVER_STATUS_IDENTITY;
    }
    return DRIVER_STATUS_OK;
}

/*
 * Intel 82441FX, the host bridge of the machine Phipia is tested on. Its
 * programmable attribute map is what decides whether the legacy BIOS regions
 * read from ROM or from DRAM, and that is the first thing a memory
 * initialisation driver has to know. Nothing here writes: changing the
 * attribute map would change what the processor is currently executing from.
 */
static enum driver_status probe_intel_82441fx(
    const struct driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct driver_probe *probe
)
{
    uint64_t attributes = 0U;
    uint8_t smram = 0U;

    (void)record;
    (void)claim;
    (void)registers;
    (void)register_bytes;
    for (unsigned int index = 0U; index < I440FX_PAM_REGISTERS; ++index) {
        uint8_t value = 0U;

        if (!configuration_byte(function,
                (uint16_t)(I440FX_PAM_BASE + index), &value)) {
            return DRIVER_STATUS_CONFIGURATION_READ;
        }
        attributes |= (uint64_t)value << (index * 8U);
    }
    if (!configuration_byte(function, I440FX_SMRAM, &smram)) {
        return DRIVER_STATUS_CONFIGURATION_READ;
    }
    /*
     * 82441FX datasheet section 3.2.14: the upper and lower attribute fields
     * of PAM0 are reserved and read as zero, which is the one bit pattern this
     * register cannot legally break.
     */
    if ((attributes & UINT64_C(0x0F)) != 0U) {
        return DRIVER_STATUS_IDENTITY;
    }
    probe->identity = attributes;
    probe->detail = (uint64_t)smram;
    return DRIVER_STATUS_OK;
}

/*
 * Intel 82371SB PIIX3, the ISA bridge. Its four route-control registers say
 * which ISA interrupt each PCI interrupt pin lands on, or that the pin is not
 * routed at all. Phipia retired the 8259 pair and routes through the I/O APIC,
 * so this driver reports the legacy routing rather than using it - and reports
 * it without touching it, because a bridge that is decoding the machine's
 * legacy I/O is not somewhere to write experimentally.
 */
static enum driver_status probe_intel_piix3_isa(
    const struct driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct driver_probe *probe
)
{
    uint32_t routes = 0U;
    uint16_t chip_selects = 0U;
    unsigned int routed = 0U;

    (void)record;
    (void)claim;
    (void)registers;
    (void)register_bytes;
    if (!configuration_dword(function, PIIX_PIRQ_ROUTE, &routes) ||
        !configuration_word(function, PIIX_XBCS, &chip_selects)) {
        return DRIVER_STATUS_CONFIGURATION_READ;
    }
    for (unsigned int pin = 0U; pin < 4U; ++pin) {
        const uint8_t route = (uint8_t)(routes >> (pin * 8U));

        if ((route & PIIX_PIRQ_DISABLE) == 0U) {
            /*
             * PIIX3 datasheet section 2.4: a routed pin may only name IRQ3
             * through IRQ15, excluding the ones the chipset owns itself.
             */
            const uint8_t interrupt = route & UINT8_C(0x0F);

            if (interrupt < 3U || interrupt == 8U || interrupt == 13U) {
                return DRIVER_STATUS_IDENTITY;
            }
            ++routed;
        }
    }
    probe->identity = (uint64_t)routes;
    probe->detail = ((uint64_t)routed << 32U) | (uint64_t)chip_selects;
    return DRIVER_STATUS_OK;
}

/*
 * Intel 82371SB PIIX3 IDE. The timing register carries one decode-enable bit
 * per channel, which is how a storage stack learns whether a channel is worth
 * probing at all before it touches a single task-file register.
 */
static enum driver_status probe_intel_piix3_ide(
    const struct driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct driver_probe *probe
)
{
    uint32_t timing = 0U;
    unsigned int enabled = 0U;

    (void)record;
    (void)claim;
    (void)registers;
    (void)register_bytes;
    if (!configuration_dword(function, PIIX_IDE_TIMING, &timing)) {
        return DRIVER_STATUS_CONFIGURATION_READ;
    }
    for (unsigned int channel = 0U; channel < 2U; ++channel) {
        const uint16_t value = (uint16_t)(timing >> (channel * 16U));

        if ((value & PIIX_IDE_TIMING_DECODE_ENABLE) != 0U) {
            ++enabled;
        }
    }
    if (function->prog_if == UINT8_C(0xFF)) {
        return DRIVER_STATUS_IDENTITY;
    }
    probe->identity = (uint64_t)timing;
    probe->detail = ((uint64_t)enabled << 32U) | (uint64_t)function->prog_if;
    return DRIVER_STATUS_OK;
}

/*
 * Intel 82371AB PIIX4 power management. Everything ACPI does on this machine
 * starts from the base address in this register and the enable bit that says
 * the base is live, so this is where an ACPI driver begins.
 */
static enum driver_status probe_intel_piix4_acpi(
    const struct driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct driver_probe *probe
)
{
    uint32_t power_base = 0U;
    uint32_t smbus_base = 0U;
    uint8_t miscellaneous = 0U;

    (void)record;
    (void)claim;
    (void)registers;
    (void)register_bytes;
    if (!configuration_dword(function, PIIX_POWER_MANAGEMENT_BASE,
            &power_base) ||
        !configuration_dword(function, PIIX_SMBUS_BASE, &smbus_base) ||
        !configuration_byte(function, PIIX_POWER_MANAGEMENT_MISC,
            &miscellaneous)) {
        return DRIVER_STATUS_CONFIGURATION_READ;
    }
    /*
     * PIIX4 datasheet section 4.2.1: the power-management base is an I/O base
     * with the low six bits reserved, and it is only meaningful while the
     * enable bit in PMREGMISC is set.
     */
    if ((miscellaneous & PIIX_POWER_IO_ENABLE) != 0U &&
        (power_base & PIIX_IO_BASE_MASK) == 0U) {
        return DRIVER_STATUS_IDENTITY;
    }
    probe->identity = (uint64_t)(power_base & PIIX_IO_BASE_MASK);
    probe->detail = ((uint64_t)(smbus_base & PIIX_IO_BASE_MASK) << 32U) |
        (uint64_t)miscellaneous;
    return DRIVER_STATUS_OK;
}

/*
 * The matrix. Order is the order the probes run in and the order the receipt
 * reports them; it is stable so a scenario can name a driver by position.
 */
static const struct driver_record driver_matrix[DRIVER_MATRIX_CAPACITY] = {
    {
        .name = "Intel 82441FX host bridge",
        .vendor_id = UINT16_C(0x8086),
        .device_id = UINT16_C(0x1237),
        .class_code = DRIVER_CLASS_BRIDGE,
        .subclass = DRIVER_SUBCLASS_HOST_BRIDGE,
        .bar_index = UINT8_MAX,
        .minimum_register_bytes = 0U,
        .access = DRIVER_ACCESS_CONFIGURATION,
        .defines_reset = false,
        .probe = probe_intel_82441fx
    },
    {
        .name = "Intel 82371SB PIIX3 ISA bridge",
        .vendor_id = UINT16_C(0x8086),
        .device_id = UINT16_C(0x7000),
        .class_code = DRIVER_CLASS_BRIDGE,
        .subclass = DRIVER_SUBCLASS_ISA_BRIDGE,
        .bar_index = UINT8_MAX,
        .minimum_register_bytes = 0U,
        .access = DRIVER_ACCESS_CONFIGURATION,
        .defines_reset = false,
        .probe = probe_intel_piix3_isa
    },
    {
        .name = "Intel 82371SB PIIX3 IDE",
        .vendor_id = UINT16_C(0x8086),
        .device_id = UINT16_C(0x7010),
        .class_code = DRIVER_CLASS_MASS_STORAGE,
        .subclass = DRIVER_SUBCLASS_IDE,
        .bar_index = UINT8_MAX,
        .minimum_register_bytes = 0U,
        .access = DRIVER_ACCESS_CONFIGURATION,
        .defines_reset = false,
        .probe = probe_intel_piix3_ide
    },
    {
        .name = "Intel 82371AB PIIX4 power management",
        .vendor_id = UINT16_C(0x8086),
        .device_id = UINT16_C(0x7113),
        .class_code = DRIVER_CLASS_BRIDGE,
        .subclass = DRIVER_SUBCLASS_OTHER_BRIDGE,
        .bar_index = UINT8_MAX,
        .minimum_register_bytes = 0U,
        .access = DRIVER_ACCESS_CONFIGURATION,
        .defines_reset = false,
        .probe = probe_intel_piix4_acpi
    },
    {
        .name = "Intel 82540EM Gigabit Ethernet",
        .vendor_id = UINT16_C(0x8086),
        .device_id = UINT16_C(0x100E),
        .class_code = DRIVER_CLASS_NETWORK,
        .subclass = DRIVER_SUBCLASS_ETHERNET,
        .bar_index = 0U,
        .minimum_register_bytes = UINT32_C(0x6000),
        .access = DRIVER_ACCESS_MEMORY,
        .defines_reset = true,
        .probe = probe_intel_82540em
    },
    {
        .name = "Intel 82574L Gigabit Ethernet",
        .vendor_id = UINT16_C(0x8086),
        .device_id = UINT16_C(0x10D3),
        .class_code = DRIVER_CLASS_NETWORK,
        .subclass = DRIVER_SUBCLASS_ETHERNET,
        .bar_index = 0U,
        .minimum_register_bytes = UINT32_C(0x6000),
        .access = DRIVER_ACCESS_MEMORY,
        .defines_reset = true,
        .probe = probe_intel_82574l
    },
    {
        .name = "Intel 82801IR ICH9 SATA AHCI",
        .vendor_id = UINT16_C(0x8086),
        .device_id = UINT16_C(0x2922),
        .class_code = DRIVER_CLASS_MASS_STORAGE,
        .subclass = DRIVER_SUBCLASS_SATA,
        .bar_index = 5U,
        .minimum_register_bytes = UINT32_C(0x100),
        .access = DRIVER_ACCESS_MEMORY,
        .defines_reset = true,
        .probe = probe_intel_ich9_ahci
    },
    {
        .name = "Intel 82801I ICH9 HD Audio",
        .vendor_id = UINT16_C(0x8086),
        .device_id = UINT16_C(0x293E),
        .class_code = DRIVER_CLASS_MULTIMEDIA,
        .subclass = DRIVER_SUBCLASS_HD_AUDIO,
        .bar_index = 0U,
        .minimum_register_bytes = UINT32_C(0x100),
        .access = DRIVER_ACCESS_MEMORY,
        .defines_reset = true,
        .probe = probe_intel_ich9_hda
    },
    {
        .name = "Realtek RTL8139 Fast Ethernet",
        .vendor_id = UINT16_C(0x10EC),
        .device_id = UINT16_C(0x8139),
        .class_code = DRIVER_CLASS_NETWORK,
        .subclass = DRIVER_SUBCLASS_ETHERNET,
        .bar_index = 1U,
        .minimum_register_bytes = UINT32_C(0x80),
        .access = DRIVER_ACCESS_MEMORY,
        .defines_reset = true,
        .probe = probe_realtek_rtl8139
    },
    {
        .name = "Intel 82801DB USB 2.0 EHCI",
        .vendor_id = UINT16_C(0x8086),
        .device_id = UINT16_C(0x24CD),
        .class_code = DRIVER_CLASS_SERIAL_BUS,
        .subclass = DRIVER_SUBCLASS_USB,
        .bar_index = 0U,
        .minimum_register_bytes = UINT32_C(0x40),
        .access = DRIVER_ACCESS_MEMORY,
        .defines_reset = true,
        .probe = probe_intel_ich4_ehci
    },
    {
        .name = "Cirrus Logic GD5446 display",
        .vendor_id = UINT16_C(0x1013),
        .device_id = UINT16_C(0x00B8),
        .class_code = DRIVER_CLASS_DISPLAY,
        .subclass = DRIVER_SUBCLASS_VGA,
        .bar_index = 1U,
        .minimum_register_bytes = UINT32_C(0x100),
        .access = DRIVER_ACCESS_MEMORY,
        .defines_reset = false,
        .probe = probe_cirrus_gd5446
    },
    {
        .name = "Bochs Display Interface",
        .vendor_id = UINT16_C(0x1234),
        .device_id = UINT16_C(0x1111),
        .class_code = DRIVER_CLASS_DISPLAY,
        .subclass = DRIVER_SUBCLASS_OTHER_DISPLAY,
        .bar_index = 2U,
        .minimum_register_bytes = UINT32_C(0x600),
        .access = DRIVER_ACCESS_MEMORY,
        .defines_reset = false,
        .probe = probe_bochs_display
    },
    {
        .name = "AMD Am79C970A PCnet-PCI II",
        .vendor_id = UINT16_C(0x1022),
        .device_id = UINT16_C(0x2000),
        .class_code = DRIVER_CLASS_NETWORK,
        .subclass = DRIVER_SUBCLASS_ETHERNET,
        .bar_index = 1U,
        .minimum_register_bytes = UINT32_C(0x20),
        .access = DRIVER_ACCESS_MEMORY,
        .defines_reset = true,
        .probe = probe_amd_pcnet
    }
};

static void capture_census(struct driver_census *census)
{
    census->frames = frame_allocator_get_stats();
    census->paging = paging_get_state();
    census->dma = dma_get_state();
    census->pci = pci_resource_get_state();
    census->vectors = interrupt_vector_get_state();
    census->msix = msix_get_state();
    census->interrupts_enabled = cpu_interrupts_enabled();
}

static bool census_equal(
    const struct driver_census *left,
    const struct driver_census *right
)
{
    return left->frames.free_frames == right->frames.free_frames &&
        left->frames.allocated_frames == right->frames.allocated_frames &&
        left->paging.table_frames == right->paging.table_frames &&
        left->paging.root_physical_address ==
            right->paging.root_physical_address &&
        left->dma.active_allocations == right->dma.active_allocations &&
        left->dma.device_owned_allocations ==
            right->dma.device_owned_allocations &&
        left->pci.active_claims == right->pci.active_claims &&
        left->pci.active_mappings == right->pci.active_mappings &&
        left->pci.mapped_pages == right->pci.mapped_pages &&
        left->pci.bus_masters == right->pci.bus_masters &&
        left->vectors.allocated == right->vectors.allocated &&
        left->vectors.free == right->vectors.free &&
        left->msix.active_bindings == right->msix.active_bindings &&
        left->interrupts_enabled == right->interrupts_enabled;
}

static const struct pci_function *find_device(
    const struct driver_record *record
)
{
    for (size_t index = 0U; index < pci_function_count(); ++index) {
        const struct pci_function *function = pci_function_at(index);

        if (function != NULL && function->vendor_id == record->vendor_id &&
            function->device_id == record->device_id &&
            function->class_code == record->class_code &&
            function->subclass == record->subclass) {
            return function;
        }
    }
    return NULL;
}

static enum driver_status bind_one(
    const struct driver_record *record,
    const struct pci_function *function,
    struct driver_probe *probe
)
{
    struct pci_device_claim claim;
    struct pci_mmio_region *region = NULL;
    volatile void *pointer = NULL;
    enum driver_status status;
    bool mapped = false;

    probe->address = function->address;
    probe->vendor_id = function->vendor_id;
    probe->device_id = function->device_id;
    probe->revision = function->revision;
    probe->class_code = function->class_code;
    probe->subclass = function->subclass;
    probe->present = true;

    if (record->access == DRIVER_ACCESS_CONFIGURATION) {
        status = record->probe(record, function, NULL, NULL, 0U, probe);
        probe->bound = status == DRIVER_STATUS_OK;
        return status;
    }

    zero_bytes(&claim, sizeof(claim));
    if (pci_claim_device(function, &claim) != PCI_RESOURCE_STATUS_OK) {
        return DRIVER_STATUS_CLAIM_FAILURE;
    }
    if (pci_claim_map_bar(&claim, record->bar_index, &region) !=
            PCI_RESOURCE_STATUS_OK || region == NULL) {
        status = DRIVER_STATUS_MAPPING_FAILURE;
        goto release;
    }
    mapped = true;
    if (region->size < record->minimum_register_bytes ||
        pci_mmio_subregion(region, 0U, region->size, &pointer) !=
            PCI_RESOURCE_STATUS_OK || pointer == NULL) {
        status = DRIVER_STATUS_REGISTER_WINDOW;
        goto release;
    }
    probe->register_bytes = (uint32_t)region->size;
    status = record->probe(record, function, &claim,
        (volatile uint8_t *)pointer, region->size, probe);

release:
    if (mapped && pci_claim_unmap_last_bar(&claim, record->bar_index) !=
            PCI_RESOURCE_STATUS_OK) {
        status = DRIVER_STATUS_RELEASE_FAILURE;
    }
    if (pci_release_device(&claim) != PCI_RESOURCE_STATUS_OK) {
        status = DRIVER_STATUS_RELEASE_FAILURE;
    }
    probe->bound = status == DRIVER_STATUS_OK;
    return status;
}

size_t driver_matrix_count(void)
{
    return DRIVER_MATRIX_CAPACITY;
}

const char *driver_matrix_name(size_t index)
{
    if (index >= DRIVER_MATRIX_CAPACITY) {
        return "unknown driver";
    }
    return driver_matrix[index].name;
}

uint16_t driver_matrix_vendor(size_t index)
{
    if (index >= DRIVER_MATRIX_CAPACITY) {
        return UINT16_C(0xFFFF);
    }
    return driver_matrix[index].vendor_id;
}

uint16_t driver_matrix_device(size_t index)
{
    if (index >= DRIVER_MATRIX_CAPACITY) {
        return UINT16_C(0xFFFF);
    }
    return driver_matrix[index].device_id;
}

enum driver_access driver_matrix_access(size_t index)
{
    if (index >= DRIVER_MATRIX_CAPACITY) {
        return DRIVER_ACCESS_COUNT;
    }
    return driver_matrix[index].access;
}

bool driver_matrix_defines_reset(size_t index)
{
    if (index >= DRIVER_MATRIX_CAPACITY) {
        return false;
    }
    return driver_matrix[index].defines_reset;
}

bool driver_matrix_self_test(size_t *completed_tests)
{
    size_t completed = 0U;

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;

    for (size_t index = 0U; index < DRIVER_MATRIX_CAPACITY; ++index) {
        if (driver_matrix[index].name == NULL ||
            driver_matrix[index].probe == NULL) {
            return false;
        }
    }
    ++completed;
    /* No two drivers may claim the same device identity. */
    for (size_t index = 0U; index < DRIVER_MATRIX_CAPACITY; ++index) {
        for (size_t other = 0U; other < index; ++other) {
            if (driver_matrix[index].vendor_id ==
                    driver_matrix[other].vendor_id &&
                driver_matrix[index].device_id ==
                    driver_matrix[other].device_id) {
                return false;
            }
        }
    }
    ++completed;
    /* An absent function answers 0xFFFF, so no driver may ask for it. */
    for (size_t index = 0U; index < DRIVER_MATRIX_CAPACITY; ++index) {
        if (driver_matrix[index].vendor_id == PCI_VENDOR_ABSENT ||
            driver_matrix[index].vendor_id == 0U) {
            return false;
        }
    }
    ++completed;
    /* A memory driver names a real BAR; a configuration driver names none. */
    for (size_t index = 0U; index < DRIVER_MATRIX_CAPACITY; ++index) {
        const struct driver_record *record = &driver_matrix[index];

        if (record->access == DRIVER_ACCESS_MEMORY) {
            if (record->bar_index >= PCI_BAR_COUNT ||
                record->minimum_register_bytes == 0U) {
                return false;
            }
        } else if (record->access == DRIVER_ACCESS_CONFIGURATION) {
            if (record->bar_index != UINT8_MAX ||
                record->minimum_register_bytes != 0U ||
                record->defines_reset) {
                return false;
            }
        } else {
            return false;
        }
    }
    ++completed;
    /* Every declared class is one the assignment specification defines. */
    for (size_t index = 0U; index < DRIVER_MATRIX_CAPACITY; ++index) {
        const uint8_t class_code = driver_matrix[index].class_code;

        if (class_code != DRIVER_CLASS_MASS_STORAGE &&
            class_code != DRIVER_CLASS_NETWORK &&
            class_code != DRIVER_CLASS_MULTIMEDIA &&
            class_code != DRIVER_CLASS_DISPLAY &&
            class_code != DRIVER_CLASS_BRIDGE &&
            class_code != DRIVER_CLASS_SERIAL_BUS) {
            return false;
        }
    }
    ++completed;
    /* Both accessor bounds refuse an index past the end of the matrix. */
    if (driver_matrix_name(DRIVER_MATRIX_CAPACITY)[0] != 'u' ||
        driver_matrix_vendor(DRIVER_MATRIX_CAPACITY) != UINT16_C(0xFFFF) ||
        driver_matrix_device(DRIVER_MATRIX_CAPACITY) != UINT16_C(0xFFFF) ||
        driver_matrix_access(DRIVER_MATRIX_CAPACITY) != DRIVER_ACCESS_COUNT) {
        return false;
    }
    ++completed;
    /* At least one vendor other than Intel, so the matrix is not one family. */
    {
        bool other_vendor = false;

        for (size_t index = 0U; index < DRIVER_MATRIX_CAPACITY; ++index) {
            if (driver_matrix[index].vendor_id != UINT16_C(0x8086)) {
                other_vendor = true;
            }
        }
        if (!other_vendor) {
            return false;
        }
    }
    ++completed;
    /* The reset bound is a real deadline rather than an unbounded wait. */
    if (DRIVER_MATRIX_RESET_TIMEOUT_NS == 0U ||
        DRIVER_MATRIX_RESET_TIMEOUT_NS > UINT64_C(5000000000)) {
        return false;
    }
    ++completed;
    /* Every status has a distinct message and the table is complete. */
    for (int status = 0; status < (int)DRIVER_STATUS_COUNT; ++status) {
        const char *message =
            driver_status_string((enum driver_status)status);

        if (message == NULL || message[0] == '\0') {
            return false;
        }
        for (int other = 0; other < status; ++other) {
            if (message == driver_status_string((enum driver_status)other)) {
                return false;
            }
        }
    }
    ++completed;
    if (driver_status_string(DRIVER_STATUS_COUNT)[0] != 'u') {
        return false;
    }
    ++completed;
    /* The two configuration accessors decode the byte and word they name. */
    {
        const struct pci_function *host = find_device(&driver_matrix[0]);
        uint32_t dword = 0U;
        uint8_t byte = 0U;
        uint16_t word = 0U;

        if (host != NULL) {
            if (!configuration_dword(host, 0U, &dword) ||
                !configuration_byte(host, 1U, &byte) ||
                !configuration_word(host, 2U, &word) ||
                byte != (uint8_t)(dword >> 8U) ||
                word != (uint16_t)(dword >> 16U) ||
                configuration_word(host, 1U, &word)) {
                return false;
            }
        }
    }
    ++completed;
    if (!driver_matrix_resources_released()) {
        return false;
    }
    ++completed;
    *completed_tests = completed;
    return completed == DRIVER_MATRIX_CONTROLLED_CONTROLS;
}

enum driver_status driver_matrix_bind(struct driver_matrix_result *result)
{
    struct driver_census before;
    struct driver_census after;
    enum driver_status first_failure = DRIVER_STATUS_OK;
    const bool restore_interrupts = cpu_interrupts_enabled();
    size_t completed = 0U;

    if (result == NULL) {
        return DRIVER_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(result, sizeof(*result));
    if (matrix_active) {
        return DRIVER_STATUS_BUSY;
    }
    if (!pci_is_initialized() || !pci_resource_get_state().active) {
        return DRIVER_STATUS_PREREQUISITE;
    }
    if (!driver_matrix_self_test(&completed) ||
        completed != DRIVER_MATRIX_CONTROLLED_CONTROLS) {
        return DRIVER_STATUS_MATRIX_INVALID;
    }
    matrix_active = true;
    register_reads = 0U;
    register_writes = 0U;
    cpu_interrupt_disable();
    capture_census(&before);
    result->declared = (uint32_t)DRIVER_MATRIX_CAPACITY;
    result->controls = (uint32_t)completed;
    result->failed_driver = (uint32_t)DRIVER_MATRIX_CAPACITY;
    result->failed_status = DRIVER_STATUS_OK;

    for (size_t index = 0U; index < DRIVER_MATRIX_CAPACITY; ++index) {
        const struct driver_record *record = &driver_matrix[index];
        const struct pci_function *function = find_device(record);
        struct driver_probe *probe = &result->probes[index];
        enum driver_status status;

        if (function == NULL) {
            continue;
        }
        ++result->present;
        status = bind_one(record, function, probe);
        if (status != DRIVER_STATUS_OK) {
            if (first_failure == DRIVER_STATUS_OK) {
                first_failure = status;
                result->failed_driver = (uint32_t)index;
                result->failed_status = status;
            }
            continue;
        }
        ++result->bound;
        if (probe->reset_observed) {
            ++result->resets;
        }
    }

    capture_census(&after);
    result->register_reads = register_reads;
    result->register_writes = register_writes;
    result->every_present_device_bound = result->bound == result->present;
    result->teardown_complete = pci_resource_verify() ==
        PCI_RESOURCE_STATUS_OK;
    result->resource_census_equal = census_equal(&before, &after);
    matrix_active = false;
    if (restore_interrupts) {
        cpu_interrupt_enable();
    }
    if (first_failure != DRIVER_STATUS_OK) {
        return first_failure;
    }
    if (!result->teardown_complete) {
        return DRIVER_STATUS_RELEASE_FAILURE;
    }
    if (!result->resource_census_equal) {
        return DRIVER_STATUS_RESOURCE_CENSUS;
    }
    if (result->present == 0U) {
        return DRIVER_STATUS_ABSENT;
    }
    installed_result = *result;
    return DRIVER_STATUS_OK;
}

struct driver_matrix_result driver_matrix_get_result(void)
{
    return installed_result;
}

bool driver_matrix_resources_released(void)
{
    return !matrix_active;
}

const char *driver_status_string(enum driver_status status)
{
    static const char *const messages[DRIVER_STATUS_COUNT] = {
        "ok",
        "null driver argument",
        "the bounded driver matrix is already active",
        "driver matrix prerequisites are incomplete",
        "the declared driver matrix is inconsistent",
        "no declared device is present",
        "device claim failed",
        "register window mapping failed",
        "the mapped register window is too small",
        "configuration-space read failed",
        "a device reset did not complete inside its bound",
        "the device did not identify itself as its specification requires",
        "device release failed",
        "driver pre/post resource census differs",
        "controlled driver matrix cleanup failed"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        DRIVER_STATUS_COUNT, "driver status messages are out of sync");
    if (status < DRIVER_STATUS_OK || status >= DRIVER_STATUS_COUNT) {
        return "unknown driver status";
    }
    return messages[status];
}
