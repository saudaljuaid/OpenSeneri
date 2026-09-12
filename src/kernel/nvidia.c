/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Bounded NVIDIA PCI, MMIO, timer, and video-BIOS identification probes.
 * Register definitions come from envytools, Nouveau, NVK, NVIDIA's open
 * modules, and the PCI specifications listed in docs/NVIDIA.md.
 *
 * Bus mastering stays disabled and no probe allocates DMA. The video-BIOS
 * probe temporarily changes the ROM shadow bit and restores it before return.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/clock.h>
#include <phipia/cpu.h>
#include <phipia/dma.h>
#include <phipia/interrupt_vector.h>
#include <phipia/memory.h>
#include <phipia/msix.h>
#include <phipia/nvidia.h>
#include <phipia/paging.h>
#include <phipia/pci.h>
#include <phipia/pci_resource.h>

/* PCI Code and ID Assignment Specification 1.19 section 1. */
#define NVIDIA_CLASS_DISPLAY UINT8_C(0x03)
#define NVIDIA_CLASS_MULTIMEDIA UINT8_C(0x04)
#define NVIDIA_SUBCLASS_VGA UINT8_C(0x00)
#define NVIDIA_SUBCLASS_3D UINT8_C(0x02)
#define NVIDIA_SUBCLASS_HD_AUDIO UINT8_C(0x03)
#define NVIDIA_MATCH_ANY UINT8_C(0xFF)

/*
 * envytools, hw/pmc.txt: the master control area occupies the first four
 * kilobytes of the register aperture on every part since NV3, and BOOT_0 at
 * offset zero is the first register any driver for this hardware reads.
 */
#define NV_PMC_BOOT_0 UINT64_C(0x000000)
#define NV_PMC_BOOT_1 UINT64_C(0x000004)

/*
 * Nouveau, nvkm_device_ctor: the part number and its revision are both fields
 * of BOOT_0, and the whole device table is keyed off them.
 */
#define NV_PMC_BOOT_0_CHIPSET_MASK UINT32_C(0x1FF00000)
#define NV_PMC_BOOT_0_CHIPSET_SHIFT 20U
#define NV_PMC_BOOT_0_REVISION_MASK UINT32_C(0x000000FF)
#define NVIDIA_CHIPSET_MASK UINT32_C(0x1FF)
#define NVIDIA_FAMILY_MASK UINT32_C(0x1F0)
#define NVIDIA_IMPLEMENTATION_MASK UINT32_C(0x00F)

/*
 * envytools, hw/pmc.txt: BOOT_1 is the endian switch. A part answering
 * big-endian through a little-endian mapping is not a part this kernel can
 * read, so it is refused rather than misread.
 */
#define NV_PMC_BOOT_1_ENDIAN_BIG UINT32_C(0x01000001)

/*
 * Nouveau, nvkm/subdev/pci: the PCI configuration space of the graphics
 * function is mirrored into the register aperture, at 0x001800 on parts before
 * NV50 and 0x088000 from NV50 onwards. That mirror is the strongest identity
 * check available here, because it makes the device state its vendor and part
 * a second time through a completely different path from the configuration
 * cycles that enumerated it.
 */
#define NV_PBUS_PCI_MIRROR_LEGACY UINT64_C(0x001800)
#define NV_PBUS_PCI_MIRROR_MODERN UINT64_C(0x088000)

/*
 * envytools, hw/ptimer.txt: PTIMER's count is two registers, and Nouveau's
 * nv04_timer_read reads the high half, then the low half, then the high half
 * again, because the count can carry between the two reads. A timer that never
 * moves is a device that is not running, which is worth knowing and is the one
 * thing in this file that observes the device over time.
 */
#define NV_PTIMER_TIME_0 UINT64_C(0x009400)
#define NV_PTIMER_TIME_1 UINT64_C(0x009410)

/*
 * Nouveau, nvkm/subdev/bios/shadowrom.c: the video BIOS is readable through a
 * window at 0x300000 once the ROM shadow is switched off, and the bit that
 * switches it off lives in the configuration mirror at offset 0x50. Clearing
 * it exposes the real ROM; the driver puts back exactly what it found.
 */
#define NV_PROM_BASE UINT64_C(0x300000)
#define NV_PBUS_PCI_NV_20_OFFSET UINT64_C(0x50)
#define NV_PBUS_PCI_NV_20_ROM_SHADOW UINT32_C(0x00000001)

/*
 * envytools, hw/pmc.txt: the engine enable mask and the two interrupt
 * registers beside it. What any particular bit means changes with the part, so
 * this kernel reports these rather than asserting on them; what it does assert
 * is that the aperture answers with distinct registers at distinct offsets
 * instead of mirroring one value, which is the difference between a decoded
 * window and a window that merely accepts reads.
 */
#define NV_PMC_INTR_0 UINT64_C(0x000100)
#define NV_PMC_INTR_EN_0 UINT64_C(0x000140)
#define NV_PMC_ENABLE UINT64_C(0x000200)

/*
 * Nouveau, nvkm/subdev/devinit and nvkm/subdev/clk: the board's strap register.
 * It carries how the board was wired -- among other things which crystal feeds
 * the clock tree -- and the bit assignments move between generations, so this
 * driver reports the register and decodes nothing it cannot stand behind.
 */
#define NV_PEXTDEV_BOOT_0 UINT64_C(0x101000)

/*
 * PCI Express Base Specification, the capability structure at the offset PCI
 * enumeration already found: link capabilities at +0x0C and link status at
 * +0x12. Every NVIDIA graphics part made this century is a PCI Express
 * endpoint, so a function claiming to be one that cannot describe its own link
 * is not one.
 */
#define PCI_EXPRESS_LINK_CAPABILITIES UINT16_C(0x0C)
#define PCI_EXPRESS_LINK_STATUS UINT16_C(0x12)
#define PCI_EXPRESS_LINK_WIDTH_SHIFT 4U
#define PCI_EXPRESS_LINK_WIDTH_MASK UINT32_C(0x3F)
#define PCI_EXPRESS_LINK_SPEED_MASK UINT32_C(0x0F)

/*
 * PCI Bus Power Management Interface Specification 1.2 section 3.2. The
 * capabilities live in the top half of the capability's own dword and the
 * control and status register is one dword further on. Only three interface
 * versions were ever defined, and the power state occupies the bottom two bits
 * of the control register with D0 encoded as zero.
 */
#define PCI_POWER_CONTROL_STATUS UINT16_C(0x04)
#define PCI_POWER_VERSION_MASK UINT32_C(0x0007)
#define PCI_POWER_VERSION_MAXIMUM UINT32_C(3)
#define PCI_POWER_STATE_MASK UINT32_C(0x0003)
#define PCI_POWER_STATE_D0 UINT32_C(0)

/*
 * PCI Local Bus Specification 3.0 section 6.8.1: the message control field in
 * the top half of the message-signalled interrupt capability's own dword. Bits
 * 3:1 say how many messages the function can send and bits 6:4 how many
 * software turned on, both as a power-of-two exponent, and only the encodings
 * zero through five are defined. Bit zero is the enable itself.
 */
#define PCI_MSI_CONTROL_ENABLE UINT32_C(0x0001)
#define PCI_MSI_CONTROL_CAPABLE_SHIFT 1U
#define PCI_MSI_CONTROL_ENABLED_SHIFT 4U
#define PCI_MSI_CONTROL_COUNT_MASK UINT32_C(0x0007)
#define PCI_MSI_COUNT_MAXIMUM UINT32_C(5)

/*
 * PCI Express Base Specification, the rest of the capability whose link half
 * driver eight reads. The capability register in the top of the first dword
 * carries the structure's version and what kind of port this is; device
 * capabilities at +0x04 says the largest payload the function can accept and
 * device control at +0x08 says what it was actually programmed to. Both
 * payload fields are a power-of-two exponent above 128 bytes, so only the
 * encodings zero through five are defined.
 */
#define PCI_EXPRESS_DEVICE_CAPABILITIES UINT16_C(0x04)
#define PCI_EXPRESS_DEVICE_CONTROL UINT16_C(0x08)
#define PCI_EXPRESS_VERSION_MASK UINT32_C(0x000F)
#define PCI_EXPRESS_TYPE_SHIFT 4U
#define PCI_EXPRESS_TYPE_MASK UINT32_C(0x000F)
#define PCI_EXPRESS_TYPE_ENDPOINT UINT32_C(0x0)
#define PCI_EXPRESS_TYPE_LEGACY_ENDPOINT UINT32_C(0x1)
#define PCI_EXPRESS_TYPE_INTEGRATED_ENDPOINT UINT32_C(0x9)
#define PCI_EXPRESS_PAYLOAD_MASK UINT32_C(0x0007)
#define PCI_EXPRESS_PAYLOAD_MAXIMUM UINT32_C(5)
#define PCI_EXPRESS_CONTROL_PAYLOAD_SHIFT 5U

/*
 * PCI Local Bus Specification 3.0 section 6.2.5.2: the expansion ROM base
 * address register in a type-0 header. Bit zero enables the decoder, bits 10:1
 * are reserved and read as zero, and what is left is a base address the
 * specification requires to be 2 KiB aligned. The three fields cover the
 * register exactly, which is a fact this file checks rather than assumes.
 */
#define PCI_EXPANSION_ROM_BASE UINT16_C(0x30)
#define PCI_EXPANSION_ROM_ENABLE UINT32_C(0x00000001)
#define PCI_EXPANSION_ROM_RESERVED_MASK UINT32_C(0x000007FE)
#define PCI_EXPANSION_ROM_ADDRESS_MASK UINT32_C(0xFFFFF800)

/*
 * Nouveau, nvkm/subdev/timer/nv04.c and nv40.c: the timer's rate is a ratio,
 * and these are the two registers it is written into. Nouveau reduces the
 * crystal frequency and a nanosecond into a numerator and a denominator and
 * writes them here; this kernel only reads them. What it asserts is that
 * neither half is degenerate and that neither offset is the free-running
 * counter beside them wearing another name -- not that the ratio explains the
 * rate, which needs a crystal frequency this kernel has no way to know.
 */
#define NV_PTIMER_NUMERATOR UINT64_C(0x009200)
#define NV_PTIMER_DENOMINATOR UINT64_C(0x009210)

/*
 * PCI Local Bus Specification 3.0 section 6.2.1: a type-0 header carries the
 * identity of the board the chip is soldered to, which for a graphics part is
 * not the same organisation as the chip's own vendor. NVIDIA sells the silicon
 * to add-in-board partners, so the subsystem vendor is theirs and the
 * subsystem device is the model of card.
 */
#define PCI_SUBSYSTEM_IDENTITY UINT16_C(0x2C)

/* The graphics aperture NVIDIA parts publish beside their register window. */
#define NVIDIA_REGISTER_APERTURE_MINIMUM UINT64_C(0x1000000)
#define NVIDIA_FRAMEBUFFER_BAR 1U

/*
 * How much of the PROM window is read. A legal image declares its own length,
 * and the structures this kernel reads -- the expansion ROM header, the PCIR
 * data structure and the BIT table -- are near the front of every image the
 * public tooling describes. Eight kilobytes is a bound, not a belief: an image
 * whose BIT table is beyond it is reported as malformed rather than searched
 * for further.
 */
#define NVIDIA_VBIOS_READ_BYTES 8192U

_Static_assert(NVIDIA_VBIOS_READ_BYTES <= NVIDIA_VBIOS_MAX_BYTES,
    "the video BIOS prefix is larger than the PROM aperture");
_Static_assert(NVIDIA_VBIOS_READ_BYTES % NVIDIA_VBIOS_BLOCK_BYTES == 0U,
    "the video BIOS prefix is not a whole number of ROM blocks");

/* High Definition Audio Specification 1.0a sections 3.3.1 through 3.3.3. */
#define HDA_GCAP UINT64_C(0x00)
#define HDA_VMIN UINT64_C(0x02)
#define HDA_VMAJ UINT64_C(0x03)
#define HDA_GCAP_OUTPUT_STREAM_SHIFT 12U
#define HDA_GCAP_OUTPUT_STREAM_MASK UINT16_C(0x000F)
#define HDA_VERSION_MAJOR UINT8_C(1)
#define HDA_VERSION_MINOR UINT8_C(0)

/*
 * The one status value that crosses back from the Rust validator by number.
 * `nvbios::Status` is `#[repr(i32)]` with explicit discriminants, and a window
 * with no ROM in it fails at the signature rather than anywhere later, which
 * is the difference between "there is no video BIOS here" and "there is one
 * and it is wrong".
 */
#define NVBIOS_STATUS_SIGNATURE 2

/* Where the C copy of the reference image and Rust's must agree. */
#define NVIDIA_REFERENCE_VBIOS_BYTES 1024U
#define NVIDIA_REFERENCE_VBIOS_DEVICE UINT16_C(0x5341)
#define NVIDIA_VBIOS_ROBUSTNESS_CONTROLS 16U

struct nvidia_driver_record;

typedef enum nvidia_status (*nvidia_probe_t)(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct nvidia_driver_probe *probe
);

struct nvidia_driver_record {
    const char *name;
    uint8_t class_code;
    /* NVIDIA_MATCH_ANY accepts either display subclass. */
    uint8_t subclass;
    /* NVIDIA_MATCH_ANY accepts any programming interface. */
    uint8_t programming_interface;
    uint8_t bar_index;
    uint32_t minimum_register_bytes;
    enum nvidia_access access;
    bool writes_registers;
    nvidia_probe_t probe;
};

struct nvidia_census {
    struct frame_allocator_stats frames;
    struct paging_state paging;
    struct dma_state dma;
    struct pci_resource_state pci;
    struct interrupt_vector_state vectors;
    struct msix_state msix;
    bool interrupts_enabled;
};

/* The freestanding Rust validator; C never parses a VBIOS byte itself. */
extern uint32_t phipia_nvbios_self_test(void);
extern uint32_t phipia_nvbios_controls(void);
extern size_t phipia_nvbios_reference(uint8_t *out, size_t capacity);
extern int phipia_nvbios_parse(
    const uint8_t *input,
    size_t input_len,
    struct nvidia_vbios_image *out
);

static struct nvidia_result installed_result;
static bool nvidia_active;
static uint32_t register_reads;
static uint32_t register_writes;
/*
 * Drivers one and three need to know which side of the NV50 boundary the part
 * is on to pick their register offsets, and driver zero is what establishes
 * that. The table is ordered so the answer exists before it is needed, and a
 * part whose identity was never established uses the modern offsets and says
 * so through its status rather than guessing silently.
 */
static struct nvidia_identity current_identity;
static uint8_t vbios_window[NVIDIA_VBIOS_READ_BYTES];

/*
 * The reference VBIOS image, stated here in C, in freestanding Rust, and in a
 * Python record the build compares against this table. It is synthesised
 * rather than dumped: no board's ROM is reproduced here, and its PCIR device
 * identifier is 0x5341 so it can never be mistaken for a real part.
 */
static const uint8_t reference_vbios[NVIDIA_REFERENCE_VBIOS_BYTES] = {
    0x55, 0xAA, 0x02, 0xEB, 0x0A, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x50, 0x43, 0x49, 0x52, 0xDE, 0x10, 0x41, 0x53,
    0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x03, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xFF, 0xB8, 0x42, 0x49, 0x54, 0x00, 0x01, 0x00,
    0x0C, 0x06, 0x03, 0x00, 0x69, 0x02, 0x40, 0x00, 0x00, 0x02, 0x42, 0x02,
    0x20, 0x00, 0x40, 0x02, 0x50, 0x02, 0x10, 0x00, 0x60, 0x02, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

_Static_assert(sizeof(reference_vbios) == NVIDIA_REFERENCE_VBIOS_BYTES,
    "the reference VBIOS table is not the declared length");

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

static void mmio_write32(
    volatile uint8_t *base,
    uint64_t offset,
    uint32_t value
)
{
    ++register_writes;
    *(volatile uint32_t *)(void *)(base + offset) = value;
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

/*
 * Where a capability sits, taken from the list PCI enumeration already walked.
 * Zero is not a legal capability offset -- the list may not start below 0x40 --
 * so it is an unambiguous "absent". Every driver that uses this reads the
 * capability's own identifier byte back from the device afterwards, so a stale
 * or wrong entry in that list is caught rather than trusted.
 */
static uint8_t capability_offset(
    const struct pci_function *function,
    uint8_t identifier
)
{
    for (size_t index = 0U; index < function->capability_count; ++index) {
        if (function->capabilities[index].identifier == identifier) {
            return function->capabilities[index].offset;
        }
    }
    return 0U;
}

/*
 * A capability whose last dword would fall outside configuration space is not
 * a capability, whatever the list said.
 */
static bool capability_fits(uint8_t offset, uint16_t last_dword)
{
    return offset >= PCI_CAPABILITY_FIRST_OFFSET &&
        (uint32_t)offset + last_dword + 4U <= PCI_CONFIG_SPACE_SIZE;
}

/*
 * The whole architecture table, and the only place a family boundary is
 * written down. Nouveau keys its device table off chipset & 0x1f0; these are
 * that table's own boundaries rather than marketing names, which is why a part
 * this kernel has never heard of still lands in the right family.
 */
struct nvidia_family_entry {
    uint32_t family;
    enum nvidia_architecture architecture;
};

static const struct nvidia_family_entry nvidia_families[] = {
    { UINT32_C(0x010), NVIDIA_ARCHITECTURE_CELSIUS },
    { UINT32_C(0x020), NVIDIA_ARCHITECTURE_KELVIN },
    { UINT32_C(0x030), NVIDIA_ARCHITECTURE_RANKINE },
    { UINT32_C(0x040), NVIDIA_ARCHITECTURE_CURIE },
    { UINT32_C(0x060), NVIDIA_ARCHITECTURE_CURIE },
    { UINT32_C(0x050), NVIDIA_ARCHITECTURE_TESLA },
    { UINT32_C(0x080), NVIDIA_ARCHITECTURE_TESLA },
    { UINT32_C(0x090), NVIDIA_ARCHITECTURE_TESLA },
    { UINT32_C(0x0A0), NVIDIA_ARCHITECTURE_TESLA },
    { UINT32_C(0x0C0), NVIDIA_ARCHITECTURE_FERMI },
    { UINT32_C(0x0D0), NVIDIA_ARCHITECTURE_FERMI },
    { UINT32_C(0x0E0), NVIDIA_ARCHITECTURE_KEPLER },
    { UINT32_C(0x0F0), NVIDIA_ARCHITECTURE_KEPLER },
    { UINT32_C(0x100), NVIDIA_ARCHITECTURE_KEPLER },
    { UINT32_C(0x110), NVIDIA_ARCHITECTURE_MAXWELL },
    { UINT32_C(0x120), NVIDIA_ARCHITECTURE_MAXWELL },
    { UINT32_C(0x130), NVIDIA_ARCHITECTURE_PASCAL },
    { UINT32_C(0x140), NVIDIA_ARCHITECTURE_VOLTA },
    { UINT32_C(0x160), NVIDIA_ARCHITECTURE_TURING },
    { UINT32_C(0x170), NVIDIA_ARCHITECTURE_AMPERE },
    { UINT32_C(0x190), NVIDIA_ARCHITECTURE_ADA }
};

#define NVIDIA_FAMILY_COUNT \
    (sizeof(nvidia_families) / sizeof(nvidia_families[0]))

struct nvidia_identity nvidia_decode_identity(uint32_t boot0)
{
    struct nvidia_identity identity;

    zero_bytes(&identity, sizeof(identity));
    identity.boot0 = boot0;
    identity.chipset = (boot0 & NV_PMC_BOOT_0_CHIPSET_MASK) >>
        NV_PMC_BOOT_0_CHIPSET_SHIFT;
    identity.revision = boot0 & NV_PMC_BOOT_0_REVISION_MASK;
    identity.family = identity.chipset & NVIDIA_FAMILY_MASK;
    identity.implementation = identity.chipset & NVIDIA_IMPLEMENTATION_MASK;
    identity.architecture = NVIDIA_ARCHITECTURE_UNKNOWN;
    for (size_t index = 0U; index < NVIDIA_FAMILY_COUNT; ++index) {
        if (nvidia_families[index].family == identity.family) {
            identity.architecture = nvidia_families[index].architecture;
            break;
        }
    }
    /*
     * A bus that answers with all ones decodes to chipset 0x1ff, and an
     * aperture that is not there at all decodes to zero. Neither is a family,
     * so neither is recognized, which is what keeps a missing device from
     * being reported as an ancient one.
     */
    identity.recognized =
        identity.architecture != NVIDIA_ARCHITECTURE_UNKNOWN &&
        boot0 != 0U && boot0 != UINT32_MAX;
    return identity;
}

const char *nvidia_architecture_name(enum nvidia_architecture architecture)
{
    static const char *const names[NVIDIA_ARCHITECTURE_COUNT] = {
        "unknown", "Celsius", "Kelvin", "Rankine", "Curie", "Tesla",
        "Fermi", "Kepler", "Maxwell", "Pascal", "Volta", "Turing",
        "Ampere", "Ada"
    };

    _Static_assert(sizeof(names) / sizeof(names[0]) ==
        NVIDIA_ARCHITECTURE_COUNT, "NVIDIA architecture names drifted");
    if (architecture < NVIDIA_ARCHITECTURE_UNKNOWN ||
        architecture >= NVIDIA_ARCHITECTURE_COUNT) {
        return "unknown";
    }
    return names[architecture];
}

static bool identity_is_modern(void)
{
    /*
     * The configuration mirror and the ROM shadow bit both moved at NV50.
     * Anything Tesla or newer uses the modern offsets; an unrecognized part
     * uses them too, because every part made this century is on that side.
     */
    return !current_identity.recognized ||
        current_identity.architecture >= NVIDIA_ARCHITECTURE_TESLA;
}

static uint64_t configuration_mirror_base(void)
{
    return identity_is_modern() ? NV_PBUS_PCI_MIRROR_MODERN :
        NV_PBUS_PCI_MIRROR_LEGACY;
}

const uint8_t *nvidia_reference_vbios(size_t *length)
{
    if (length == NULL) {
        return NULL;
    }
    *length = NVIDIA_REFERENCE_VBIOS_BYTES;
    return reference_vbios;
}

/*
 * Driver zero. The first register on the part, and the one every driver for
 * this hardware has read first since 1999. What it carries is the part number
 * and its revision; what its neighbour carries is the endianness the register
 * aperture is answering in. A part that answers big-endian is refused rather
 * than read backwards.
 */
static enum nvidia_status probe_master_control(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct nvidia_driver_probe *probe
)
{
    uint32_t boot0;
    uint32_t boot1;
    struct nvidia_identity identity;

    (void)record;
    (void)function;
    (void)claim;
    if (register_bytes < NV_PMC_BOOT_1 + 4U) {
        return NVIDIA_STATUS_REGISTER_WINDOW;
    }
    boot0 = mmio_read32(registers, NV_PMC_BOOT_0);
    boot1 = mmio_read32(registers, NV_PMC_BOOT_1);
    identity = nvidia_decode_identity(boot0);
    probe->identity = boot0;
    probe->detail = boot1;
    if (!identity.recognized) {
        return NVIDIA_STATUS_IDENTITY;
    }
    if ((boot1 & NV_PMC_BOOT_1_ENDIAN_BIG) != 0U) {
        return NVIDIA_STATUS_ENDIANNESS;
    }
    current_identity = identity;
    installed_result.identity = identity;
    return NVIDIA_STATUS_OK;
}

/*
 * Driver one. The graphics function mirrors its own PCI configuration space
 * into the register aperture, so the first dword of that mirror has to be the
 * same vendor and device the enumeration read through configuration cycles.
 * One source could be anything. Two sources that agree, reached through
 * completely different hardware paths, are the device telling the truth about
 * itself twice.
 */
static enum nvidia_status probe_configuration_mirror(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct nvidia_driver_probe *probe
)
{
    const uint64_t base = configuration_mirror_base();
    uint32_t mirrored;
    uint32_t enumerated = 0U;

    (void)record;
    (void)claim;
    if (register_bytes < base + 4U) {
        return NVIDIA_STATUS_REGISTER_WINDOW;
    }
    mirrored = mmio_read32(registers, base);
    if (!configuration_dword(function, 0U, &enumerated)) {
        return NVIDIA_STATUS_IDENTITY;
    }
    probe->identity = mirrored;
    probe->detail = enumerated;
    if ((mirrored & UINT32_C(0xFFFF)) != NVIDIA_VENDOR_ID ||
        mirrored != enumerated) {
        return NVIDIA_STATUS_IDENTITY;
    }
    return NVIDIA_STATUS_OK;
}

/*
 * Driver two. PTIMER's count is wider than a register, so it is read high,
 * low, high again and reassembled only if the high half did not move; that is
 * Nouveau's own sequence and it is the only correct way to read a counter that
 * can carry underneath you. A count that has not advanced after a bounded wait
 * on this kernel's own clock is a stopped timer, reported as one.
 */
static bool read_ptimer(volatile uint8_t *registers, uint64_t *value)
{
    for (unsigned attempt = 0U; attempt < 4U; ++attempt) {
        const uint32_t high = mmio_read32(registers, NV_PTIMER_TIME_1);
        const uint32_t low = mmio_read32(registers, NV_PTIMER_TIME_0);

        if (mmio_read32(registers, NV_PTIMER_TIME_1) == high) {
            *value = ((uint64_t)high << 32U) | low;
            return true;
        }
    }
    return false;
}

static enum nvidia_status probe_timer(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct nvidia_driver_probe *probe
)
{
    uint64_t first = 0U;
    uint64_t second = 0U;
    uint64_t deadline;

    (void)record;
    (void)function;
    (void)claim;
    if (register_bytes < NV_PTIMER_TIME_1 + 4U) {
        return NVIDIA_STATUS_REGISTER_WINDOW;
    }
    if (!read_ptimer(registers, &first)) {
        return NVIDIA_STATUS_TIMER_STOPPED;
    }
    deadline = clock_monotonic_ns() + NVIDIA_TIMER_OBSERVATION_NS;
    while (clock_monotonic_ns() < deadline) {
        __asm__ volatile ("" : : : "memory");
    }
    if (!read_ptimer(registers, &second)) {
        return NVIDIA_STATUS_TIMER_STOPPED;
    }
    probe->identity = second;
    probe->detail = second - first;
    if (second <= first) {
        return NVIDIA_STATUS_TIMER_STOPPED;
    }
    return NVIDIA_STATUS_OK;
}

/*
 * Driver three. The only driver here that writes anything, and the write is
 * the one the window requires: clearing the ROM shadow bit in the
 * configuration mirror is what makes the PROM aperture answer with the real
 * image instead of the shadow copy. The original value goes back afterwards
 * and is read again to prove it went back, because a driver that leaves a
 * device in a state it did not find it in has not finished.
 *
 * The bytes themselves are never parsed here. They go straight to the
 * freestanding Rust validator, which is the whole reason that crate exists.
 */
static enum nvidia_status probe_video_bios(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct nvidia_driver_probe *probe
)
{
    const uint64_t shadow = configuration_mirror_base() +
        NV_PBUS_PCI_NV_20_OFFSET;
    uint32_t saved;
    uint32_t restored;
    struct nvidia_vbios_image image;
    int status;

    (void)record;
    (void)function;
    (void)claim;
    if (register_bytes < NV_PROM_BASE + NVIDIA_VBIOS_READ_BYTES ||
        register_bytes < shadow + 4U) {
        return NVIDIA_STATUS_REGISTER_WINDOW;
    }
    saved = mmio_read32(registers, shadow);
    mmio_write32(registers, shadow, saved & ~NV_PBUS_PCI_NV_20_ROM_SHADOW);
    for (size_t offset = 0U; offset < NVIDIA_VBIOS_READ_BYTES; ++offset) {
        vbios_window[offset] = mmio_read8(registers, NV_PROM_BASE + offset);
    }
    mmio_write32(registers, shadow, saved);
    restored = mmio_read32(registers, shadow);
    if (restored != saved) {
        return NVIDIA_STATUS_ROM_NOT_RESTORED;
    }
    zero_bytes(&image, sizeof(image));
    status = phipia_nvbios_parse(vbios_window, sizeof(vbios_window), &image);
    probe->identity = ((uint64_t)image.vendor_id << 16U) | image.device_id;
    probe->detail = image.image_bytes;
    if (status != 0) {
        return status == NVBIOS_STATUS_SIGNATURE ? NVIDIA_STATUS_ROM_ABSENT :
            NVIDIA_STATUS_ROM_MALFORMED;
    }
    installed_result.vbios = image;
    installed_result.vbios_valid = true;
    return NVIDIA_STATUS_OK;
}

/*
 * Driver four, and the only one that binds a function other than the graphics
 * one. Every NVIDIA board since Fermi carries an HD Audio controller beside
 * the GPU for the audio a display link carries, and it answers the same
 * register contract Phipia's ICH9 driver already proves: a version, and a
 * count of the streams the controller has. This driver resets nothing: the
 * audio function of a board that may be driving a live display is not
 * something to reset blind.
 */
static enum nvidia_status probe_hd_audio(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct nvidia_driver_probe *probe
)
{
    uint16_t capability;
    uint8_t major;
    uint8_t minor;

    (void)record;
    (void)function;
    (void)claim;
    if (register_bytes < HDA_VMAJ + 1U) {
        return NVIDIA_STATUS_REGISTER_WINDOW;
    }
    capability = mmio_read16(registers, HDA_GCAP);
    minor = mmio_read8(registers, HDA_VMIN);
    major = mmio_read8(registers, HDA_VMAJ);
    probe->identity = ((uint64_t)major << 8U) | minor;
    probe->detail = capability;
    if (major != HDA_VERSION_MAJOR || minor != HDA_VERSION_MINOR) {
        return NVIDIA_STATUS_VERSION;
    }
    if (((capability >> HDA_GCAP_OUTPUT_STREAM_SHIFT) &
            HDA_GCAP_OUTPUT_STREAM_MASK) == 0U) {
        return NVIDIA_STATUS_IDENTITY;
    }
    return NVIDIA_STATUS_OK;
}

/*
 * Driver five. The board's strap register says how this particular card was
 * wired rather than what part is on it, and the bit assignments move between
 * generations, so this driver decodes nothing it cannot stand behind: it reads
 * the register, reports it, and requires only that the aperture answered with a
 * real register rather than an open bus or a mirror of offset zero.
 */
static enum nvidia_status probe_boot_straps(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct nvidia_driver_probe *probe
)
{
    uint32_t straps;
    uint32_t boot0;

    (void)record;
    (void)function;
    (void)claim;
    if (register_bytes < NV_PEXTDEV_BOOT_0 + 4U) {
        return NVIDIA_STATUS_REGISTER_WINDOW;
    }
    straps = mmio_read32(registers, NV_PEXTDEV_BOOT_0);
    boot0 = mmio_read32(registers, NV_PMC_BOOT_0);
    probe->identity = straps;
    probe->detail = boot0;
    if (straps == UINT32_MAX) {
        return NVIDIA_STATUS_STRAPS;
    }
    /*
     * An aperture that answers every offset with the same value is not
     * decoding, and would make every other driver here believe whatever the
     * first register happened to say.
     */
    if (straps == boot0) {
        return NVIDIA_STATUS_APERTURE_ALIASED;
    }
    if (current_identity.recognized && boot0 != current_identity.boot0) {
        return NVIDIA_STATUS_IDENTITY;
    }
    return NVIDIA_STATUS_OK;
}

/*
 * Driver six. The engine enable mask and the two interrupt registers beside
 * it. What any one bit means changes with the part, so none of them is asserted
 * on; what is asserted is that this is the same device driver zero identified,
 * answering the same way through a second claim and a second mapping, and that
 * its register window decodes distinct offsets.
 */
static enum nvidia_status probe_master_control_engines(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct nvidia_driver_probe *probe
)
{
    uint32_t enable;
    uint32_t interrupt_enable;
    uint32_t interrupts;
    uint32_t boot0;

    (void)record;
    (void)function;
    (void)claim;
    if (register_bytes < NV_PMC_ENABLE + 4U) {
        return NVIDIA_STATUS_REGISTER_WINDOW;
    }
    boot0 = mmio_read32(registers, NV_PMC_BOOT_0);
    enable = mmio_read32(registers, NV_PMC_ENABLE);
    interrupt_enable = mmio_read32(registers, NV_PMC_INTR_EN_0);
    interrupts = mmio_read32(registers, NV_PMC_INTR_0);
    probe->identity = ((uint64_t)enable << 32U) | interrupt_enable;
    probe->detail = interrupts;
    if (enable == UINT32_MAX) {
        return NVIDIA_STATUS_IDENTITY;
    }
    if (enable == boot0) {
        return NVIDIA_STATUS_APERTURE_ALIASED;
    }
    /*
     * The same part, claimed and mapped a second time, still says who it is.
     * A device that answers differently across two bindings is not one device.
     */
    if (current_identity.recognized && boot0 != current_identity.boot0) {
        return NVIDIA_STATUS_IDENTITY;
    }
    return NVIDIA_STATUS_OK;
}

/*
 * Driver seven. Not a register driver at all: it reads the apertures the
 * function publishes through the typed substrate and checks their shape. An
 * NVIDIA graphics part puts its register window in BAR0 and its framebuffer
 * aperture in BAR1, and the two differ in exactly the way the PCI
 * specification lets a device declare: the register window is not
 * prefetchable, because reads of it have side effects, and the framebuffer
 * aperture is, because they do not. This driver maps nothing.
 */
static enum nvidia_status probe_memory_apertures(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct nvidia_driver_probe *probe
)
{
    const struct pci_bar_description *window;
    const struct pci_bar_description *aperture;

    (void)record;
    (void)function;
    (void)registers;
    (void)register_bytes;
    if (claim == NULL) {
        return NVIDIA_STATUS_CLAIM_FAILURE;
    }
    window = pci_claim_bar(claim, 0U);
    aperture = pci_claim_bar(claim, NVIDIA_FRAMEBUFFER_BAR);
    if (window == NULL || aperture == NULL) {
        return NVIDIA_STATUS_APERTURE;
    }
    probe->identity = aperture->size;
    probe->detail = window->size;
    if (!window->implemented || !aperture->implemented) {
        return NVIDIA_STATUS_APERTURE;
    }
    if (window->kind != PCI_BAR_MEMORY_32 &&
        window->kind != PCI_BAR_MEMORY_64) {
        return NVIDIA_STATUS_APERTURE;
    }
    if (aperture->kind != PCI_BAR_MEMORY_32 &&
        aperture->kind != PCI_BAR_MEMORY_64) {
        return NVIDIA_STATUS_APERTURE;
    }
    if (window->size < NVIDIA_REGISTER_APERTURE_MINIMUM) {
        return NVIDIA_STATUS_APERTURE;
    }
    /*
     * Reading a register can change it, so the window may never be
     * prefetchable. The framebuffer is memory and is.
     */
    if (window->prefetchable || !aperture->prefetchable) {
        return NVIDIA_STATUS_APERTURE;
    }
    if (aperture->size < window->size) {
        return NVIDIA_STATUS_APERTURE;
    }
    return NVIDIA_STATUS_OK;
}

/*
 * Driver eight. Configuration space only, and the one driver here that claims
 * nothing at all. Every NVIDIA graphics part made this century is a PCI
 * Express endpoint, so it carries the Express capability and that capability
 * describes the link it is actually running on. A function that cannot say how
 * wide its own link is has not been understood.
 */
static enum nvidia_status probe_express_link(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct nvidia_driver_probe *probe
)
{
    uint32_t capabilities = 0U;
    uint32_t status = 0U;
    uint32_t negotiated_width;
    uint32_t maximum_width;

    (void)record;
    (void)claim;
    (void)registers;
    (void)register_bytes;
    if (function->express_offset == 0U) {
        return NVIDIA_STATUS_LINK;
    }
    if (!configuration_dword(function,
            (uint16_t)(function->express_offset +
                PCI_EXPRESS_LINK_CAPABILITIES), &capabilities) ||
        !configuration_dword(function,
            (uint16_t)((function->express_offset +
                PCI_EXPRESS_LINK_STATUS) & ~UINT16_C(3)), &status)) {
        return NVIDIA_STATUS_LINK;
    }
    status >>= 16U;
    probe->identity = status;
    probe->detail = capabilities;
    maximum_width = (capabilities >> PCI_EXPRESS_LINK_WIDTH_SHIFT) &
        PCI_EXPRESS_LINK_WIDTH_MASK;
    negotiated_width = (status >> PCI_EXPRESS_LINK_WIDTH_SHIFT) &
        PCI_EXPRESS_LINK_WIDTH_MASK;
    if (maximum_width == 0U || negotiated_width == 0U ||
        negotiated_width > maximum_width) {
        return NVIDIA_STATUS_LINK;
    }
    if ((capabilities & PCI_EXPRESS_LINK_SPEED_MASK) == 0U) {
        return NVIDIA_STATUS_LINK;
    }
    return NVIDIA_STATUS_OK;
}

/* Driver nine: validate the add-in board's subsystem identity. */
static enum nvidia_status probe_board_identity(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct nvidia_driver_probe *probe
)
{
    uint32_t subsystem = 0U;
    uint32_t identity = 0U;

    (void)record;
    (void)claim;
    (void)registers;
    (void)register_bytes;
    if (!configuration_dword(function, PCI_SUBSYSTEM_IDENTITY, &subsystem) ||
        !configuration_dword(function, 0U, &identity)) {
        return NVIDIA_STATUS_IDENTITY;
    }
    probe->identity = subsystem;
    probe->detail = identity;
    /* Reject absent and open-bus subsystem identifiers. */
    if (subsystem == 0U || subsystem == UINT32_MAX ||
        (subsystem & UINT32_C(0xFFFF)) == 0U ||
        (subsystem & UINT32_C(0xFFFF)) == UINT32_C(0xFFFF)) {
        return NVIDIA_STATUS_IDENTITY;
    }
    /* The chip under the board is still the one enumeration found. */
    if ((identity & UINT32_C(0xFFFF)) != NVIDIA_VENDOR_ID) {
        return NVIDIA_STATUS_IDENTITY;
    }
    return NVIDIA_STATUS_OK;
}

/* Driver ten: require a valid power-management capability in D0. */
static enum nvidia_status probe_power_management(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct nvidia_driver_probe *probe
)
{
    const uint8_t offset = capability_offset(function,
        PCI_CAPABILITY_POWER_MANAGEMENT);
    uint32_t header = 0U;
    uint32_t control = 0U;
    uint32_t version;

    (void)record;
    (void)claim;
    (void)registers;
    (void)register_bytes;
    if (offset == 0U || !capability_fits(offset, PCI_POWER_CONTROL_STATUS)) {
        return NVIDIA_STATUS_POWER_MANAGEMENT;
    }
    if (!configuration_dword(function, offset, &header) ||
        !configuration_dword(function,
            (uint16_t)(offset + PCI_POWER_CONTROL_STATUS), &control)) {
        return NVIDIA_STATUS_POWER_MANAGEMENT;
    }
    probe->identity = header;
    probe->detail = control;
    /* The enumeration said the capability was here; the device has to agree. */
    if ((header & UINT32_C(0xFF)) != PCI_CAPABILITY_POWER_MANAGEMENT) {
        return NVIDIA_STATUS_POWER_MANAGEMENT;
    }
    version = (header >> 16U) & PCI_POWER_VERSION_MASK;
    if (version == 0U || version > PCI_POWER_VERSION_MAXIMUM) {
        return NVIDIA_STATUS_POWER_MANAGEMENT;
    }
    if ((control & PCI_POWER_STATE_MASK) != PCI_POWER_STATE_D0) {
        return NVIDIA_STATUS_POWER_MANAGEMENT;
    }
    return NVIDIA_STATUS_OK;
}

/*
 * Driver eleven. Configuration space only. Message-signalled interrupts are
 * delivered as a memory write to the local APIC, so a function that arrives
 * with them already enabled can interrupt this kernel without this kernel ever
 * having agreed to it. Nothing in Phipia enabled them on this function and
 * nothing outside Phipia may have: this driver's whole job is to say so out
 * loud, and to check the count fields against the encodings the specification
 * actually defines rather than assuming a device fills them in sanely.
 */
static enum nvidia_status probe_message_interrupts(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct nvidia_driver_probe *probe
)
{
    const uint8_t offset = capability_offset(function, PCI_CAPABILITY_MSI);
    uint32_t header = 0U;
    uint32_t control;
    uint32_t capable;
    uint32_t enabled;

    (void)record;
    (void)claim;
    (void)registers;
    (void)register_bytes;
    if (offset == 0U || !capability_fits(offset, 0U)) {
        return NVIDIA_STATUS_MESSAGE_INTERRUPTS;
    }
    /*
     * PCI enumeration keeps a shortcut to this capability beside the list it
     * came from. The two are written at different times by different code, so
     * requiring them to agree is a real check on the enumerator and not a
     * tautology.
     */
    if (offset != function->msi_offset) {
        return NVIDIA_STATUS_MESSAGE_INTERRUPTS;
    }
    if (!configuration_dword(function, offset, &header)) {
        return NVIDIA_STATUS_MESSAGE_INTERRUPTS;
    }
    control = header >> 16U;
    probe->identity = control;
    probe->detail = offset;
    if ((header & UINT32_C(0xFF)) != PCI_CAPABILITY_MSI) {
        return NVIDIA_STATUS_MESSAGE_INTERRUPTS;
    }
    capable = (control >> PCI_MSI_CONTROL_CAPABLE_SHIFT) &
        PCI_MSI_CONTROL_COUNT_MASK;
    enabled = (control >> PCI_MSI_CONTROL_ENABLED_SHIFT) &
        PCI_MSI_CONTROL_COUNT_MASK;
    if (capable > PCI_MSI_COUNT_MAXIMUM || enabled > capable) {
        return NVIDIA_STATUS_MESSAGE_INTERRUPTS;
    }
    if ((control & PCI_MSI_CONTROL_ENABLE) != 0U || enabled != 0U) {
        return NVIDIA_STATUS_MESSAGE_INTERRUPTS;
    }
    return NVIDIA_STATUS_OK;
}

/*
 * Driver twelve: accept PCIe endpoint encodings and verify that the configured
 * payload size does not exceed the advertised maximum.
 */
static enum nvidia_status probe_express_device(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct nvidia_driver_probe *probe
)
{
    const uint8_t offset = function->express_offset;
    uint32_t header = 0U;
    uint32_t capabilities = 0U;
    uint32_t control = 0U;
    uint32_t type;
    uint32_t supported;
    uint32_t programmed;

    (void)record;
    (void)claim;
    (void)registers;
    (void)register_bytes;
    if (offset == 0U ||
        offset != capability_offset(function, PCI_CAPABILITY_EXPRESS) ||
        !capability_fits(offset, PCI_EXPRESS_DEVICE_CONTROL)) {
        return NVIDIA_STATUS_ENDPOINT;
    }
    if (!configuration_dword(function, offset, &header) ||
        !configuration_dword(function,
            (uint16_t)(offset + PCI_EXPRESS_DEVICE_CAPABILITIES),
            &capabilities) ||
        !configuration_dword(function,
            (uint16_t)(offset + PCI_EXPRESS_DEVICE_CONTROL), &control)) {
        return NVIDIA_STATUS_ENDPOINT;
    }
    probe->identity = capabilities;
    probe->detail = ((uint64_t)(header >> 16U) << 32U) |
        (control & UINT32_C(0xFFFF));
    /*
     * The capability's own identifier byte, read back from the device. PCI
     * requires capability pointers to be dword aligned and a conforming
     * enumeration masks the bottom two bits off, so a capability placed at an
     * odd offset is reachable only at the aligned offset below it -- where the
     * bytes belong to whatever came before. This check is what turns that into
     * a refusal with a name instead of a field decoded out of a neighbour.
     */
    if ((header & UINT32_C(0xFF)) != PCI_CAPABILITY_EXPRESS) {
        return NVIDIA_STATUS_ENDPOINT;
    }
    header >>= 16U;
    if ((header & PCI_EXPRESS_VERSION_MASK) == 0U) {
        return NVIDIA_STATUS_ENDPOINT;
    }
    type = (header >> PCI_EXPRESS_TYPE_SHIFT) & PCI_EXPRESS_TYPE_MASK;
    if (type != PCI_EXPRESS_TYPE_ENDPOINT &&
        type != PCI_EXPRESS_TYPE_LEGACY_ENDPOINT &&
        type != PCI_EXPRESS_TYPE_INTEGRATED_ENDPOINT) {
        return NVIDIA_STATUS_ENDPOINT;
    }
    supported = capabilities & PCI_EXPRESS_PAYLOAD_MASK;
    programmed = (control >> PCI_EXPRESS_CONTROL_PAYLOAD_SHIFT) &
        PCI_EXPRESS_PAYLOAD_MASK;
    if (supported > PCI_EXPRESS_PAYLOAD_MAXIMUM || programmed > supported) {
        return NVIDIA_STATUS_ENDPOINT;
    }
    return NVIDIA_STATUS_OK;
}

/*
 * Driver thirteen. Configuration space only, and the counterpart to driver
 * three: the expansion ROM this board declares to PCI, as opposed to the one
 * driver three actually read through the PROM aperture. Those are two decoders
 * over the same ROM and this kernel deliberately used only one of them, so the
 * other had better still be switched off -- which is the check. The image
 * driver three came back with is checked here too, against the shape the PCI
 * Firmware Specification requires of any expansion ROM: a whole number of
 * 512-byte blocks, inside the window.
 *
 * Nothing here writes. Sizing a base address register means writing all ones
 * to it and putting it back, and this driver is not going to do that to a
 * decoder that may be pointed at live firmware.
 */
static enum nvidia_status probe_expansion_rom(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct nvidia_driver_probe *probe
)
{
    uint32_t rom = 0U;

    (void)record;
    (void)claim;
    (void)registers;
    (void)register_bytes;
    if ((function->header_type & PCI_HEADER_TYPE_MASK) !=
        PCI_HEADER_TYPE_ENDPOINT) {
        return NVIDIA_STATUS_EXPANSION_ROM;
    }
    if (!configuration_dword(function, PCI_EXPANSION_ROM_BASE, &rom)) {
        return NVIDIA_STATUS_EXPANSION_ROM;
    }
    probe->identity = rom;
    probe->detail = installed_result.vbios_valid ?
        installed_result.vbios.image_bytes : 0U;
    if (rom == UINT32_MAX) {
        return NVIDIA_STATUS_EXPANSION_ROM;
    }
    if ((rom & PCI_EXPANSION_ROM_ENABLE) != 0U) {
        return NVIDIA_STATUS_EXPANSION_ROM;
    }
    if ((rom & PCI_EXPANSION_ROM_RESERVED_MASK) != 0U) {
        return NVIDIA_STATUS_EXPANSION_ROM;
    }
    /*
     * The image driver three came back with, re-asserted here against the
     * shape the PCI Firmware Specification requires of an expansion ROM. The
     * Rust validator already guarantees this -- it derives the length from the
     * image's own block count -- so no device can make this fire and it is not
     * claimed as a check on hardware. It is a check on the boundary: this is
     * where C first uses that number, and a number that crossed from another
     * language is worth confirming where it is used rather than where it was
     * made.
     */
    if (installed_result.vbios_valid &&
        (installed_result.vbios.image_bytes == 0U ||
        installed_result.vbios.image_bytes % NVIDIA_VBIOS_BLOCK_BYTES != 0U ||
        installed_result.vbios.image_bytes > NVIDIA_VBIOS_MAX_BYTES)) {
        return NVIDIA_STATUS_EXPANSION_ROM;
    }
    return NVIDIA_STATUS_OK;
}

/*
 * Driver fourteen. The last one with a register window, and the counterpart to
 * driver two: the pair of registers that set the timer's rate. Driver two
 * proved the counter moves; this one proves the two offsets beside it are
 * their own registers rather than more of the counter, by reading the
 * numerator, waiting for the counter to advance, and reading the numerator
 * again. A configuration register does not change while a counter does.
 *
 * What this does not do is claim the ratio explains the rate. That needs the
 * part's crystal frequency, which is not in any register this kernel reads, so
 * the numerator and the denominator are reported and only their degeneracy is
 * refused.
 */
static enum nvidia_status probe_timer_scale(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    const struct pci_device_claim *claim,
    volatile uint8_t *registers,
    uint64_t register_bytes,
    struct nvidia_driver_probe *probe
)
{
    uint32_t numerator;
    uint32_t denominator;
    uint32_t again;
    uint64_t first = 0U;
    uint64_t second = 0U;
    uint64_t deadline;

    (void)record;
    (void)function;
    (void)claim;
    if (register_bytes < NV_PTIMER_DENOMINATOR + 4U) {
        return NVIDIA_STATUS_REGISTER_WINDOW;
    }
    numerator = mmio_read32(registers, NV_PTIMER_NUMERATOR);
    denominator = mmio_read32(registers, NV_PTIMER_DENOMINATOR);
    probe->identity = ((uint64_t)numerator << 32U) | denominator;
    if (numerator == 0U || denominator == 0U ||
        numerator == UINT32_MAX || denominator == UINT32_MAX) {
        return NVIDIA_STATUS_TIMER_SCALE;
    }
    if (!read_ptimer(registers, &first)) {
        return NVIDIA_STATUS_TIMER_STOPPED;
    }
    deadline = clock_monotonic_ns() + NVIDIA_TIMER_OBSERVATION_NS;
    while (clock_monotonic_ns() < deadline) {
        __asm__ volatile ("" : : : "memory");
    }
    if (!read_ptimer(registers, &second) || second <= first) {
        return NVIDIA_STATUS_TIMER_STOPPED;
    }
    probe->detail = second - first;
    again = mmio_read32(registers, NV_PTIMER_NUMERATOR);
    if (again != numerator) {
        return NVIDIA_STATUS_TIMER_SCALE;
    }
    return NVIDIA_STATUS_OK;
}

/*
 * The order matters exactly once: driver zero establishes which side of the
 * NV50 boundary the part is on, and drivers one and three pick their register
 * offsets from that.
 */
static const struct nvidia_driver_record nvidia_drivers[NVIDIA_DRIVER_COUNT] = {
    {
        .name = "NVIDIA GPU master control",
        .class_code = NVIDIA_CLASS_DISPLAY,
        .subclass = NVIDIA_MATCH_ANY,
        .programming_interface = NVIDIA_MATCH_ANY,
        .bar_index = 0U,
        .minimum_register_bytes = UINT32_C(0x1000),
        .access = NVIDIA_ACCESS_MEMORY,
        .writes_registers = false,
        .probe = probe_master_control
    },
    {
        .name = "NVIDIA GPU configuration mirror",
        .class_code = NVIDIA_CLASS_DISPLAY,
        .subclass = NVIDIA_MATCH_ANY,
        .programming_interface = NVIDIA_MATCH_ANY,
        .bar_index = 0U,
        .minimum_register_bytes = UINT32_C(0x89000),
        .access = NVIDIA_ACCESS_MEMORY,
        .writes_registers = false,
        .probe = probe_configuration_mirror
    },
    {
        .name = "NVIDIA GPU timer",
        .class_code = NVIDIA_CLASS_DISPLAY,
        .subclass = NVIDIA_MATCH_ANY,
        .programming_interface = NVIDIA_MATCH_ANY,
        .bar_index = 0U,
        .minimum_register_bytes = UINT32_C(0xA000),
        .access = NVIDIA_ACCESS_MEMORY,
        .writes_registers = false,
        .probe = probe_timer
    },
    {
        .name = "NVIDIA GPU video BIOS",
        .class_code = NVIDIA_CLASS_DISPLAY,
        .subclass = NVIDIA_MATCH_ANY,
        .programming_interface = NVIDIA_MATCH_ANY,
        .bar_index = 0U,
        .minimum_register_bytes = UINT32_C(0x302000),
        .access = NVIDIA_ACCESS_MEMORY,
        .writes_registers = true,
        .probe = probe_video_bios
    },
    {
        .name = "NVIDIA HD Audio function",
        .class_code = NVIDIA_CLASS_MULTIMEDIA,
        .subclass = NVIDIA_SUBCLASS_HD_AUDIO,
        .programming_interface = NVIDIA_MATCH_ANY,
        .bar_index = 0U,
        .minimum_register_bytes = UINT32_C(0x100),
        .access = NVIDIA_ACCESS_MEMORY,
        .writes_registers = false,
        .probe = probe_hd_audio
    },
    {
        .name = "NVIDIA GPU boot straps",
        .class_code = NVIDIA_CLASS_DISPLAY,
        .subclass = NVIDIA_MATCH_ANY,
        .programming_interface = NVIDIA_MATCH_ANY,
        .bar_index = 0U,
        .minimum_register_bytes = UINT32_C(0x102000),
        .access = NVIDIA_ACCESS_MEMORY,
        .writes_registers = false,
        .probe = probe_boot_straps
    },
    {
        .name = "NVIDIA GPU master control engines",
        .class_code = NVIDIA_CLASS_DISPLAY,
        .subclass = NVIDIA_MATCH_ANY,
        .programming_interface = NVIDIA_MATCH_ANY,
        .bar_index = 0U,
        .minimum_register_bytes = UINT32_C(0x1000),
        .access = NVIDIA_ACCESS_MEMORY,
        .writes_registers = false,
        .probe = probe_master_control_engines
    },
    {
        .name = "NVIDIA GPU memory apertures",
        .class_code = NVIDIA_CLASS_DISPLAY,
        .subclass = NVIDIA_MATCH_ANY,
        .programming_interface = NVIDIA_MATCH_ANY,
        .bar_index = UINT8_MAX,
        .minimum_register_bytes = 0U,
        .access = NVIDIA_ACCESS_APERTURE,
        .writes_registers = false,
        .probe = probe_memory_apertures
    },
    {
        .name = "NVIDIA GPU PCI Express link",
        .class_code = NVIDIA_CLASS_DISPLAY,
        .subclass = NVIDIA_MATCH_ANY,
        .programming_interface = NVIDIA_MATCH_ANY,
        .bar_index = UINT8_MAX,
        .minimum_register_bytes = 0U,
        .access = NVIDIA_ACCESS_CONFIGURATION,
        .writes_registers = false,
        .probe = probe_express_link
    },
    {
        .name = "NVIDIA GPU board identity",
        .class_code = NVIDIA_CLASS_DISPLAY,
        .subclass = NVIDIA_MATCH_ANY,
        .programming_interface = NVIDIA_MATCH_ANY,
        .bar_index = UINT8_MAX,
        .minimum_register_bytes = 0U,
        .access = NVIDIA_ACCESS_CONFIGURATION,
        .writes_registers = false,
        .probe = probe_board_identity
    },
    {
        .name = "NVIDIA GPU power management",
        .class_code = NVIDIA_CLASS_DISPLAY,
        .subclass = NVIDIA_MATCH_ANY,
        .programming_interface = NVIDIA_MATCH_ANY,
        .bar_index = UINT8_MAX,
        .minimum_register_bytes = 0U,
        .access = NVIDIA_ACCESS_CONFIGURATION,
        .writes_registers = false,
        .probe = probe_power_management
    },
    {
        .name = "NVIDIA GPU message interrupts",
        .class_code = NVIDIA_CLASS_DISPLAY,
        .subclass = NVIDIA_MATCH_ANY,
        .programming_interface = NVIDIA_MATCH_ANY,
        .bar_index = UINT8_MAX,
        .minimum_register_bytes = 0U,
        .access = NVIDIA_ACCESS_CONFIGURATION,
        .writes_registers = false,
        .probe = probe_message_interrupts
    },
    {
        .name = "NVIDIA GPU PCI Express endpoint",
        .class_code = NVIDIA_CLASS_DISPLAY,
        .subclass = NVIDIA_MATCH_ANY,
        .programming_interface = NVIDIA_MATCH_ANY,
        .bar_index = UINT8_MAX,
        .minimum_register_bytes = 0U,
        .access = NVIDIA_ACCESS_CONFIGURATION,
        .writes_registers = false,
        .probe = probe_express_device
    },
    {
        .name = "NVIDIA GPU expansion ROM declaration",
        .class_code = NVIDIA_CLASS_DISPLAY,
        .subclass = NVIDIA_MATCH_ANY,
        .programming_interface = NVIDIA_MATCH_ANY,
        .bar_index = UINT8_MAX,
        .minimum_register_bytes = 0U,
        .access = NVIDIA_ACCESS_CONFIGURATION,
        .writes_registers = false,
        .probe = probe_expansion_rom
    },
    {
        .name = "NVIDIA GPU timer scale",
        .class_code = NVIDIA_CLASS_DISPLAY,
        .subclass = NVIDIA_MATCH_ANY,
        .programming_interface = NVIDIA_MATCH_ANY,
        .bar_index = 0U,
        .minimum_register_bytes = UINT32_C(0xA000),
        .access = NVIDIA_ACCESS_MEMORY,
        .writes_registers = false,
        .probe = probe_timer_scale
    }
};

static void capture_census(struct nvidia_census *census)
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
    const struct nvidia_census *left,
    const struct nvidia_census *right
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

static bool subclass_matches(
    const struct nvidia_driver_record *record,
    const struct pci_function *function
)
{
    if (record->programming_interface != NVIDIA_MATCH_ANY &&
        function->prog_if != record->programming_interface) {
        return false;
    }
    if (record->subclass != NVIDIA_MATCH_ANY) {
        return function->subclass == record->subclass;
    }
    /* Boards enumerate as a VGA controller or a bare 3D controller. */
    return function->subclass == NVIDIA_SUBCLASS_VGA ||
        function->subclass == NVIDIA_SUBCLASS_3D;
}

static const struct pci_function *find_function(
    const struct nvidia_driver_record *record
)
{
    for (size_t index = 0U; index < pci_function_count(); ++index) {
        const struct pci_function *function = pci_function_at(index);

        if (function != NULL && function->vendor_id == NVIDIA_VENDOR_ID &&
            function->class_code == record->class_code &&
            subclass_matches(record, function)) {
            return function;
        }
    }
    return NULL;
}

static enum nvidia_status bind_one(
    const struct nvidia_driver_record *record,
    const struct pci_function *function,
    struct nvidia_driver_probe *probe
)
{
    struct pci_device_claim claim;
    struct pci_mmio_region *region = NULL;
    volatile void *pointer = NULL;
    enum nvidia_status status;
    bool mapped = false;
    const uint32_t reads_before = register_reads;
    const uint32_t writes_before = register_writes;

    probe->address = function->address;
    probe->vendor_id = function->vendor_id;
    probe->device_id = function->device_id;
    probe->class_code = function->class_code;
    probe->subclass = function->subclass;
    probe->present = true;

    /*
     * A driver that reads only configuration space takes nothing at all: no
     * claim, no mapping, no command-register change. Phipia has no IOMMU, and
     * the cheapest way to be sure a driver cannot reach memory is for it never
     * to have been given a window.
     */
    if (record->access == NVIDIA_ACCESS_CONFIGURATION) {
        status = record->probe(record, function, NULL, NULL, 0U, probe);
        probe->register_reads = register_reads - reads_before;
        probe->register_writes = register_writes - writes_before;
        probe->bound = status == NVIDIA_STATUS_OK;
        return status;
    }

    zero_bytes(&claim, sizeof(claim));
    if (pci_claim_device(function, &claim) != PCI_RESOURCE_STATUS_OK) {
        return NVIDIA_STATUS_CLAIM_FAILURE;
    }
    /*
     * The aperture driver wants the BAR descriptions the claim produced and
     * nothing else. Claiming sizes them; mapping is a separate decision and it
     * declines to make it.
     */
    if (record->access == NVIDIA_ACCESS_APERTURE) {
        status = record->probe(record, function, &claim, NULL, 0U, probe);
        goto release;
    }
    if (pci_claim_map_bar(&claim, record->bar_index, &region) !=
            PCI_RESOURCE_STATUS_OK || region == NULL) {
        status = NVIDIA_STATUS_MAPPING_FAILURE;
        goto release;
    }
    mapped = true;
    if (region->size < record->minimum_register_bytes ||
        pci_mmio_subregion(region, 0U, region->size, &pointer) !=
            PCI_RESOURCE_STATUS_OK || pointer == NULL) {
        status = NVIDIA_STATUS_REGISTER_WINDOW;
        goto release;
    }
    probe->register_bytes = (uint32_t)region->size;
    status = record->probe(record, function, &claim,
        (volatile uint8_t *)pointer, region->size, probe);

release:
    if (mapped && pci_claim_unmap_last_bar(&claim, record->bar_index) !=
            PCI_RESOURCE_STATUS_OK) {
        status = NVIDIA_STATUS_RELEASE_FAILURE;
    }
    if (pci_release_device(&claim) != PCI_RESOURCE_STATUS_OK) {
        status = NVIDIA_STATUS_RELEASE_FAILURE;
    }
    probe->register_reads = register_reads - reads_before;
    probe->register_writes = register_writes - writes_before;
    probe->bound = status == NVIDIA_STATUS_OK;
    return status;
}

size_t nvidia_driver_count(void)
{
    return NVIDIA_DRIVER_COUNT;
}

const char *nvidia_driver_name(size_t index)
{
    if (index >= NVIDIA_DRIVER_COUNT) {
        return "unknown NVIDIA driver";
    }
    return nvidia_drivers[index].name;
}

uint8_t nvidia_driver_class(size_t index)
{
    if (index >= NVIDIA_DRIVER_COUNT) {
        return UINT8_MAX;
    }
    return nvidia_drivers[index].class_code;
}

uint8_t nvidia_driver_subclass(size_t index)
{
    if (index >= NVIDIA_DRIVER_COUNT) {
        return UINT8_MAX;
    }
    return nvidia_drivers[index].subclass;
}

uint8_t nvidia_driver_interface(size_t index)
{
    if (index >= NVIDIA_DRIVER_COUNT) {
        return UINT8_MAX;
    }
    return nvidia_drivers[index].programming_interface;
}

enum nvidia_access nvidia_driver_access(size_t index)
{
    if (index >= NVIDIA_DRIVER_COUNT) {
        return NVIDIA_ACCESS_COUNT;
    }
    return nvidia_drivers[index].access;
}

bool nvidia_driver_writes_registers(size_t index)
{
    if (index >= NVIDIA_DRIVER_COUNT) {
        return false;
    }
    return nvidia_drivers[index].writes_registers;
}

bool nvidia_vbios_image_layout_self_test(void)
{
    return sizeof(struct nvidia_vbios_image) == 24U &&
        offsetof(struct nvidia_vbios_image, image_bytes) == 0U &&
        offsetof(struct nvidia_vbios_image, pcir_offset) == 4U &&
        offsetof(struct nvidia_vbios_image, bit_offset) == 8U &&
        offsetof(struct nvidia_vbios_image, vendor_id) == 12U &&
        offsetof(struct nvidia_vbios_image, device_id) == 14U &&
        offsetof(struct nvidia_vbios_image, class_code) == 16U &&
        offsetof(struct nvidia_vbios_image, subclass) == 17U &&
        offsetof(struct nvidia_vbios_image, programming_interface) == 18U &&
        offsetof(struct nvidia_vbios_image, code_type) == 19U &&
        offsetof(struct nvidia_vbios_image, bit_tokens) == 20U &&
        offsetof(struct nvidia_vbios_image, bit_token_bytes) == 21U &&
        offsetof(struct nvidia_vbios_image, last_image) == 22U;
}

/*
 * The kernel's table and Rust's are two independent statements of the same
 * image, and a Python record the build compares against this table is the
 * third. Any two of them disagreeing is caught here or by `make verify`
 * rather than by a parser that quietly accepts something else.
 */
static bool reference_vbios_agrees(void)
{
    static uint8_t written[NVIDIA_REFERENCE_VBIOS_BYTES];

    if (phipia_nvbios_reference(written, sizeof(written)) !=
            sizeof(written)) {
        return false;
    }
    for (size_t index = 0U; index < sizeof(written); ++index) {
        if (written[index] != reference_vbios[index]) {
            return false;
        }
    }
    return true;
}

/*
 * Everything provable without the device, and therefore everything this
 * increment can actually stand behind. Each control is a statement that would
 * be false if the code were wrong, and none of them needs an NVIDIA part to be
 * present.
 */
bool nvidia_foundation_self_test(size_t *completed_tests)
{
    static const struct { uint32_t boot0; enum nvidia_architecture family; }
        published[] = {
        { UINT32_C(0x050000A2), NVIDIA_ARCHITECTURE_TESLA },
        { UINT32_C(0x0C0000A3), NVIDIA_ARCHITECTURE_FERMI },
        { UINT32_C(0x0E4000A1), NVIDIA_ARCHITECTURE_KEPLER },
        { UINT32_C(0x124000A1), NVIDIA_ARCHITECTURE_MAXWELL },
        { UINT32_C(0x134000A1), NVIDIA_ARCHITECTURE_PASCAL },
        { UINT32_C(0x140000A1), NVIDIA_ARCHITECTURE_VOLTA },
        { UINT32_C(0x164000A1), NVIDIA_ARCHITECTURE_TURING },
        { UINT32_C(0x172000A1), NVIDIA_ARCHITECTURE_AMPERE },
        { UINT32_C(0x192000A1), NVIDIA_ARCHITECTURE_ADA }
    };
    const size_t published_count = sizeof(published) / sizeof(published[0]);
    size_t completed = 0U;
    struct nvidia_vbios_image image;
    size_t reference_length = 0U;
    const uint8_t *reference;

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;

    for (size_t index = 0U; index < NVIDIA_DRIVER_COUNT; ++index) {
        if (nvidia_drivers[index].name == NULL ||
            nvidia_drivers[index].probe == NULL) {
            return false;
        }
    }
    ++completed;

    /* Exactly one driver may write, and it must be the video BIOS one. */
    {
        size_t writers = 0U;

        for (size_t index = 0U; index < NVIDIA_DRIVER_COUNT; ++index) {
            if (nvidia_drivers[index].writes_registers) {
                ++writers;
                if (nvidia_drivers[index].probe != probe_video_bios) {
                    return false;
                }
            }
        }
        if (writers != 1U) {
            return false;
        }
    }
    ++completed;

    /* Every published encoding lands in the family Nouveau's table gives. */
    for (size_t index = 0U; index < published_count; ++index) {
        const struct nvidia_identity identity =
            nvidia_decode_identity(published[index].boot0);

        if (!identity.recognized ||
            identity.architecture != published[index].family ||
            identity.chipset != ((published[index].boot0 >> 20U) &
                NVIDIA_CHIPSET_MASK) ||
            identity.revision != (published[index].boot0 & UINT32_C(0xFF))) {
            return false;
        }
    }
    ++completed;

    /* A family the table does not carry is unknown, never the nearest one. */
    if (nvidia_decode_identity(UINT32_C(0x180000A1)).recognized ||
        nvidia_decode_identity(UINT32_C(0x1A0000A1)).recognized) {
        return false;
    }
    ++completed;

    /* An absent aperture reads as zero and a dead bus reads as all ones. */
    if (nvidia_decode_identity(0U).recognized ||
        nvidia_decode_identity(UINT32_MAX).recognized) {
        return false;
    }
    ++completed;

    /* Every architecture has a distinct name. */
    for (int outer = 0; outer < (int)NVIDIA_ARCHITECTURE_COUNT; ++outer) {
        const char *left = nvidia_architecture_name(
            (enum nvidia_architecture)outer);

        if (left == NULL) {
            return false;
        }
        for (int inner = 0; inner < outer; ++inner) {
            const char *right = nvidia_architecture_name(
                (enum nvidia_architecture)inner);
            size_t position = 0U;

            while (left[position] != '\0' && right[position] != '\0' &&
                left[position] == right[position]) {
                ++position;
            }
            if (left[position] == right[position]) {
                return false;
            }
        }
    }
    ++completed;

    if (!nvidia_vbios_image_layout_self_test()) {
        return false;
    }
    ++completed;

    /* The Rust validator runs every control it declares. */
    if (phipia_nvbios_controls() != NVIDIA_VBIOS_ROBUSTNESS_CONTROLS ||
        phipia_nvbios_self_test() != NVIDIA_VBIOS_ROBUSTNESS_CONTROLS) {
        return false;
    }
    ++completed;

    if (!reference_vbios_agrees()) {
        return false;
    }
    ++completed;

    /* The kernel's copy of the reference image is Rust's, byte for byte. */
    reference = nvidia_reference_vbios(&reference_length);
    if (reference == NULL || reference_length !=
            NVIDIA_REFERENCE_VBIOS_BYTES) {
        return false;
    }
    ++completed;

    /* And it parses through the same boundary a real image would. */
    zero_bytes(&image, sizeof(image));
    if (phipia_nvbios_parse(reference, reference_length, &image) != 0 ||
        image.vendor_id != NVIDIA_VENDOR_ID ||
        image.device_id != NVIDIA_REFERENCE_VBIOS_DEVICE ||
        image.image_bytes != NVIDIA_REFERENCE_VBIOS_BYTES ||
        image.class_code != UINT8_C(0x03) || image.code_type != 0U ||
        image.bit_tokens == 0U || !image.last_image) {
        return false;
    }
    ++completed;

    /* A truncated image is refused rather than read past. */
    zero_bytes(&image, sizeof(image));
    if (phipia_nvbios_parse(reference, 16U, &image) == 0 ||
        image.image_bytes != 0U) {
        return false;
    }
    ++completed;

    /* So is a null one. */
    zero_bytes(&image, sizeof(image));
    if (phipia_nvbios_parse(NULL, reference_length, &image) == 0) {
        return false;
    }
    ++completed;

    /* No two drivers may answer to the same class and subclass. */
    for (size_t index = 0U; index < NVIDIA_DRIVER_COUNT; ++index) {
        for (size_t other = 0U; other < index; ++other) {
            if (nvidia_drivers[index].class_code ==
                    nvidia_drivers[other].class_code &&
                nvidia_drivers[index].subclass ==
                    nvidia_drivers[other].subclass &&
                nvidia_drivers[index].probe ==
                    nvidia_drivers[other].probe) {
                return false;
            }
        }
    }
    ++completed;

    /*
     * Every driver's access mode agrees with what it declares. A driver that
     * maps a window has to name which one; a driver that maps nothing must not
     * name one at all, because a BAR index that is quietly ignored is a window
     * nobody decided to open.
     */
    for (size_t index = 0U; index < NVIDIA_DRIVER_COUNT; ++index) {
        const struct nvidia_driver_record *record = &nvidia_drivers[index];

        if (record->access >= NVIDIA_ACCESS_COUNT) {
            return false;
        }
        if (record->access == NVIDIA_ACCESS_MEMORY) {
            if (record->bar_index == UINT8_MAX ||
                record->minimum_register_bytes == 0U) {
                return false;
            }
        } else if (record->bar_index != UINT8_MAX ||
            record->minimum_register_bytes != 0U) {
            return false;
        }
    }
    ++completed;

    /*
     * The table's access census, stated here so a driver cannot quietly change
     * how much of the machine it takes: eight map one window each, one reads
     * the aperture descriptions a claim produces, six read configuration space
     * and take nothing at all, and only a driver with a window may write.
     */
    {
        size_t memory = 0U;
        size_t apertures = 0U;
        size_t configuration = 0U;

        for (size_t index = 0U; index < NVIDIA_DRIVER_COUNT; ++index) {
            const struct nvidia_driver_record *record = &nvidia_drivers[index];

            switch (record->access) {
            case NVIDIA_ACCESS_MEMORY: ++memory; break;
            case NVIDIA_ACCESS_APERTURE: ++apertures; break;
            case NVIDIA_ACCESS_CONFIGURATION: ++configuration; break;
            default: return false;
            }
            if (record->access != NVIDIA_ACCESS_MEMORY &&
                record->writes_registers) {
                return false;
            }
        }
        if (memory != 8U || apertures != 1U || configuration != 6U) {
            return false;
        }
    }
    ++completed;

    /*
     * No driver here pins a programming interface. The matcher supports it
     * because classes that need it exist, and this control is what keeps an
     * unused field from drifting into a silent wildcard: every record says
     * NVIDIA_MATCH_ANY on purpose.
     */
    for (size_t index = 0U; index < NVIDIA_DRIVER_COUNT; ++index) {
        if (nvidia_drivers[index].programming_interface != NVIDIA_MATCH_ANY) {
            return false;
        }
    }
    ++completed;

    /*
     * No probe function appears in the table twice. The control above catches
     * two records that would match the same function in the same way; this one
     * catches the same code being listed under two names, which is how a table
     * of fifteen drivers becomes a table of fourteen drivers and a duplicate.
     */
    for (size_t index = 0U; index < NVIDIA_DRIVER_COUNT; ++index) {
        for (size_t other = 0U; other < index; ++other) {
            if (nvidia_drivers[index].probe == nvidia_drivers[other].probe) {
                return false;
            }
        }
    }
    ++completed;

    /*
     * Five drivers here read something an earlier driver established, and each
     * of them is only meaningful if that earlier driver has already run. The
     * loop in nvidia_bind goes through the table in order, so those
     * dependencies are the table's order, and this is where the order is
     * written down rather than left as a comment nobody has to keep true.
     */
    {
        static const struct { nvidia_probe_t after; nvidia_probe_t before; }
            ordering[] = {
            /* The mirror and the ROM window pick offsets from the identity. */
            { probe_configuration_mirror, probe_master_control },
            { probe_video_bios, probe_master_control },
            /* Aliasing needs a window whose identity is known good. */
            { probe_master_control_engines, probe_master_control },
            /* The ROM declaration cross-checks the image the window read. */
            { probe_expansion_rom, probe_video_bios },
            /* The rate pair is checked against a counter known to move. */
            { probe_timer_scale, probe_timer }
        };
        const size_t pairs = sizeof(ordering) / sizeof(ordering[0]);

        for (size_t pair = 0U; pair < pairs; ++pair) {
            size_t after = NVIDIA_DRIVER_COUNT;
            size_t before = NVIDIA_DRIVER_COUNT;

            for (size_t index = 0U; index < NVIDIA_DRIVER_COUNT; ++index) {
                if (nvidia_drivers[index].probe == ordering[pair].after) {
                    after = index;
                }
                if (nvidia_drivers[index].probe == ordering[pair].before) {
                    before = index;
                }
            }
            if (after >= NVIDIA_DRIVER_COUNT ||
                before >= NVIDIA_DRIVER_COUNT || before >= after) {
                return false;
            }
        }
    }
    ++completed;

    /*
     * Every bit field this file decides something on is bounded by a limit
     * that fits inside the mask it came from, and the three fields of the
     * expansion ROM register cover that register exactly and do not overlap. A
     * limit outside its own mask is a check that can never fire, and a
     * reserved-bit mask that is wrong is a check that fires on the wrong bits.
     */
    if (PCI_POWER_VERSION_MAXIMUM > PCI_POWER_VERSION_MASK ||
        PCI_MSI_COUNT_MAXIMUM > PCI_MSI_CONTROL_COUNT_MASK ||
        PCI_EXPRESS_PAYLOAD_MAXIMUM > PCI_EXPRESS_PAYLOAD_MASK ||
        PCI_EXPRESS_TYPE_LEGACY_ENDPOINT > PCI_EXPRESS_TYPE_MASK ||
        PCI_EXPRESS_TYPE_INTEGRATED_ENDPOINT > PCI_EXPRESS_TYPE_MASK) {
        return false;
    }
    if ((PCI_EXPANSION_ROM_ENABLE | PCI_EXPANSION_ROM_RESERVED_MASK |
            PCI_EXPANSION_ROM_ADDRESS_MASK) != UINT32_MAX ||
        (PCI_EXPANSION_ROM_ENABLE & PCI_EXPANSION_ROM_RESERVED_MASK) != 0U ||
        (PCI_EXPANSION_ROM_ENABLE & PCI_EXPANSION_ROM_ADDRESS_MASK) != 0U ||
        (PCI_EXPANSION_ROM_RESERVED_MASK &
            PCI_EXPANSION_ROM_ADDRESS_MASK) != 0U) {
        return false;
    }
    ++completed;

    /* Every status this module can return has its own message. */
    for (int outer = 0; outer < (int)NVIDIA_STATUS_COUNT; ++outer) {
        const char *left = nvidia_status_string((enum nvidia_status)outer);

        if (left == NULL) {
            return false;
        }
        for (int inner = 0; inner < outer; ++inner) {
            const char *right =
                nvidia_status_string((enum nvidia_status)inner);
            size_t position = 0U;

            while (left[position] != '\0' && right[position] != '\0' &&
                left[position] == right[position]) {
                ++position;
            }
            if (left[position] == right[position]) {
                return false;
            }
        }
    }
    ++completed;

    *completed_tests = completed;
    return completed == NVIDIA_CONTROLLED_CONTROLS;
}

enum nvidia_status nvidia_bind(struct nvidia_result *result)
{
    struct nvidia_census before;
    struct nvidia_census after;
    size_t controls = 0U;

    if (result == NULL) {
        return NVIDIA_STATUS_NULL_ARGUMENT;
    }
    if (nvidia_active) {
        return NVIDIA_STATUS_BUSY;
    }
    if (!pci_is_initialized() || !pci_resource_get_state().active) {
        return NVIDIA_STATUS_PREREQUISITE;
    }
    nvidia_active = true;
    zero_bytes(&installed_result, sizeof(installed_result));
    zero_bytes(&current_identity, sizeof(current_identity));
    register_reads = 0U;
    register_writes = 0U;
    installed_result.declared = NVIDIA_DRIVER_COUNT;
    installed_result.failed_driver = NVIDIA_DRIVER_COUNT;
    installed_result.failed_status = NVIDIA_STATUS_OK;

    if (!nvidia_foundation_self_test(&controls) ||
        controls != NVIDIA_CONTROLLED_CONTROLS) {
        installed_result.failed_status = NVIDIA_STATUS_ROBUSTNESS;
        nvidia_active = false;
        *result = installed_result;
        return NVIDIA_STATUS_ROBUSTNESS;
    }
    installed_result.controls = (uint32_t)controls;

    capture_census(&before);
    for (size_t index = 0U; index < NVIDIA_DRIVER_COUNT; ++index) {
        const struct nvidia_driver_record *record = &nvidia_drivers[index];
        const struct pci_function *function = find_function(record);
        enum nvidia_status status;

        if (function == NULL) {
            continue;
        }
        ++installed_result.present;
        status = bind_one(record, function, &installed_result.probes[index]);
        if (status != NVIDIA_STATUS_OK) {
            if (installed_result.failed_status == NVIDIA_STATUS_OK) {
                installed_result.failed_status = status;
                installed_result.failed_driver = (uint32_t)index;
            }
            continue;
        }
        ++installed_result.bound;
    }
    capture_census(&after);

    installed_result.register_reads = register_reads;
    installed_result.register_writes = register_writes;
    installed_result.any_function_present = installed_result.present != 0U;
    installed_result.every_present_function_bound =
        installed_result.bound == installed_result.present;
    /*
     * The binding is over before the teardown is judged: nvidia_resources_
     * released() reports on a module that is not mid-bind, so asking it while
     * the flag is still set would answer about the wrong moment.
     */
    nvidia_active = false;
    installed_result.teardown_complete = nvidia_resources_released();
    installed_result.resource_census_equal = census_equal(&before, &after);
    *result = installed_result;

    if (!installed_result.teardown_complete) {
        return NVIDIA_STATUS_RELEASE_FAILURE;
    }
    if (!installed_result.resource_census_equal) {
        return NVIDIA_STATUS_RESOURCE_CENSUS;
    }
    if (installed_result.failed_status != NVIDIA_STATUS_OK) {
        return installed_result.failed_status;
    }
    /*
     * No NVIDIA function present is a healthy answer, not a failure: it is the
     * only answer this kernel has ever actually observed.
     */
    return NVIDIA_STATUS_OK;
}

struct nvidia_result nvidia_get_result(void)
{
    return installed_result;
}

bool nvidia_resources_released(void)
{
    const struct pci_resource_state state = pci_resource_get_state();

    return !nvidia_active && state.active_claims == 0U &&
        state.active_mappings == 0U && state.mapped_pages == 0U &&
        state.bus_masters == 0U;
}

const char *nvidia_status_string(enum nvidia_status status)
{
    static const char *const messages[NVIDIA_STATUS_COUNT] = {
        "ok",
        "null NVIDIA argument",
        "NVIDIA drivers are already binding",
        "NVIDIA prerequisite missing",
        "NVIDIA device claim failed",
        "NVIDIA register window mapping failed",
        "NVIDIA register window is too small",
        "NVIDIA register aperture is big-endian",
        "NVIDIA device identity was refused",
        "NVIDIA register aperture answers every offset alike",
        "NVIDIA board straps were refused",
        "NVIDIA memory apertures are not the published shape",
        "NVIDIA PCI Express link was not described",
        "NVIDIA function is not a PCI Express endpoint",
        "NVIDIA power management capability was refused",
        "NVIDIA message interrupts are not in the state this kernel left them",
        "NVIDIA expansion ROM declaration was refused",
        "NVIDIA timer rate registers are degenerate",
        "NVIDIA timer did not advance",
        "NVIDIA video BIOS window is empty",
        "NVIDIA video BIOS image is malformed",
        "NVIDIA ROM shadow bit was not restored",
        "NVIDIA function reported an unsupported version",
        "NVIDIA device release failed",
        "NVIDIA resource census changed",
        "NVIDIA controlled self-test failed"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        NVIDIA_STATUS_COUNT, "NVIDIA status messages drifted");
    if (status < NVIDIA_STATUS_OK || status >= NVIDIA_STATUS_COUNT) {
        return "unknown NVIDIA status";
    }
    return messages[status];
}
