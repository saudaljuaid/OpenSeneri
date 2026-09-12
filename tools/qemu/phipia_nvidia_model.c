/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * QEMU model for the NVIDIA register probes. It implements the master-control
 * registers, configuration mirror, timer, PROM window, ROM shadow bit, and PCI
 * capabilities used by Phipia. It has no graphics, display, interrupt, or
 * memory-management engine.
 *
 * Command-line properties supply identity values, a caller-supplied file
 * supplies the ROM, and QEMU's clock drives the timer.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pcie.h"
#include "hw/pci/msi.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"

#define TYPE_PHIPIA_NVIDIA_MODEL "phipia-nvidia-model"
OBJECT_DECLARE_SIMPLE_TYPE(PhipiaNvidiaState, PHIPIA_NVIDIA_MODEL)

/* envytools hw/pmc.txt */
#define NV_PMC_BOOT_0            0x000000
#define NV_PMC_BOOT_1            0x000004
/* envytools hw/ptimer.txt */
#define NV_PTIMER_TIME_0         0x009400
#define NV_PTIMER_TIME_1         0x009410
/* Nouveau nvkm/subdev/pci: the configuration mirror on NV50 and later. */
#define NV_PBUS_PCI_MIRROR       0x088000
#define NV_PBUS_PCI_MIRROR_BYTES 0x1000
#define NV_PBUS_PCI_NV_20        0x088050
#define NV_ROM_SHADOW_BIT        0x00000001
/* Nouveau nvkm/subdev/bios/shadowrom.c */
#define NV_PROM_BASE             0x300000
#define NV_PROM_BYTES            0x10000
/* envytools hw/pmc.txt: the engine enable mask and its interrupt registers. */
#define NV_PMC_INTR_0            0x000100
#define NV_PMC_INTR_EN_0         0x000140
#define NV_PMC_ENABLE            0x000200
/* Nouveau nvkm/subdev/devinit: the board strap register. */
#define NV_PEXTDEV_BOOT_0        0x101000
/* Nouveau nvkm/subdev/timer/nv04.c: the two halves of the timer's rate. */
#define NV_PTIMER_NUMERATOR      0x009200
#define NV_PTIMER_DENOMINATOR    0x009210

/*
 * Where the capabilities go. All three offsets are stated here rather than
 * left to QEMU's own placement, because that placement packs capabilities
 * byte-tight and can land one on an offset that is not dword aligned. The PCI
 * specification requires the pointers between them to be dword aligned -- the
 * bottom two bits are reserved -- so a conforming driver masks them off, and a
 * capability at an odd offset is one such a driver cannot reach. Real parts do
 * not do that; this model must not either.
 */
#define PHIPIA_NVIDIA_PM_OFFSET  0x40
#define PHIPIA_NVIDIA_MSI_OFFSET 0x50
#define PHIPIA_NVIDIA_EXPRESS_OFFSET 0x60
#define PHIPIA_NVIDIA_MSI_VECTORS 1

/*
 * Deliberate defects, for negative controls. A driver's check is worth nothing
 * until something has failed it, and these are the switches that make a
 * conforming model stop conforming in one named way at a time. Nothing here is
 * a description of real hardware: these are lies a device could tell, injected
 * so a driver can be shown to catch them.
 */
#define PHIPIA_NVIDIA_DEFECT_NO_POWER_MANAGEMENT 0x1
#define PHIPIA_NVIDIA_DEFECT_MESSAGE_INTERRUPTS  0x2
#define PHIPIA_NVIDIA_DEFECT_ROOT_PORT           0x4
#define PHIPIA_NVIDIA_DEFECT_ALIASED_SCALE       0x8

#define PHIPIA_NVIDIA_BAR_BYTES  (16 * MiB)
/*
 * The framebuffer aperture beside the register window. It is prefetchable and
 * the register window is not, which is the distinction a driver checking the
 * shape of these apertures is checking for.
 */
#define PHIPIA_NVIDIA_FB_BYTES   (256 * MiB)
#define NVIDIA_VENDOR_ID         0x10DE

struct PhipiaNvidiaState {
    PCIDevice parent_obj;
    MemoryRegion registers;
    MemoryRegion framebuffer;

    /* Pinned by the caller so the driver is checked against outside values. */
    uint32_t boot0;
    uint32_t boot1;
    uint32_t straps;
    uint32_t enable;
    uint32_t subsystem;
    uint32_t numerator;
    uint32_t denominator;
    uint32_t rom_declaration;
    uint32_t defects;
    char *vbios_path;

    uint32_t shadow;
    uint8_t rom[NV_PROM_BYTES];
    bool rom_present;
};

static uint64_t phipia_nvidia_read(void *opaque, hwaddr addr, unsigned size)
{
    PhipiaNvidiaState *state = opaque;

    if (addr >= NV_PROM_BASE && addr < NV_PROM_BASE + NV_PROM_BYTES) {
        uint64_t value = 0;
        hwaddr offset = addr - NV_PROM_BASE;

        /*
         * While the shadow bit is set the window answers with the shadow copy
         * rather than the ROM, which is exactly why a driver has to clear it.
         */
        for (unsigned index = 0; index < size; ++index) {
            uint8_t byte = 0xFF;

            if (!(state->shadow & NV_ROM_SHADOW_BIT) && state->rom_present &&
                offset + index < NV_PROM_BYTES) {
                byte = state->rom[offset + index];
            }
            value |= (uint64_t)byte << (index * 8);
        }
        return value;
    }

    if (addr >= NV_PBUS_PCI_MIRROR &&
        addr < NV_PBUS_PCI_MIRROR + NV_PBUS_PCI_MIRROR_BYTES) {
        uint32_t offset = addr - NV_PBUS_PCI_MIRROR;

        if (offset == (NV_PBUS_PCI_NV_20 - NV_PBUS_PCI_MIRROR)) {
            return state->shadow;
        }
        /*
         * The mirror is the device's own configuration space, read through
         * the same accessor the configuration cycles use. It is a mirror, not
         * a copy: a driver comparing the two paths is comparing one source.
         */
        return pci_default_read_config(PCI_DEVICE(state), offset, size);
    }

    switch (addr) {
    case NV_PMC_BOOT_0:
        return state->boot0;
    case NV_PMC_BOOT_1:
        return state->boot1;
    case NV_PMC_ENABLE:
        return state->enable;
    case NV_PMC_INTR_EN_0:
        return 0;
    case NV_PMC_INTR_0:
        return 0;
    case NV_PEXTDEV_BOOT_0:
        return state->straps;
    /*
     * The rate pair, straight from the command line. This model's counter is
     * QEMU's own nanosecond clock and does NOT obey these two numbers: they
     * are here so a driver that reads them has something pinned outside itself
     * to read, and a driver that claimed the ratio explained the rate would be
     * claiming something this model cannot support.
     */
    case NV_PTIMER_NUMERATOR:
        /*
         * The aliased-scale defect is the one an offset-decoding mistake looks
         * like from outside: the window accepts the read and answers with the
         * counter next door instead of the configuration register asked for.
         */
        if (state->defects & PHIPIA_NVIDIA_DEFECT_ALIASED_SCALE) {
            return (uint32_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        return state->numerator;
    case NV_PTIMER_DENOMINATOR:
        return state->denominator;
    case NV_PTIMER_TIME_0:
        return (uint32_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    case NV_PTIMER_TIME_1:
        return (uint32_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >> 32);
    default:
        return 0;
    }
}

static void phipia_nvidia_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    PhipiaNvidiaState *state = opaque;

    /* The shadow bit is the only writable register this model has. */
    if (addr == NV_PBUS_PCI_NV_20) {
        state->shadow = (uint32_t)value;
    }
}

static const MemoryRegionOps phipia_nvidia_ops = {
    .read = phipia_nvidia_read,
    .write = phipia_nvidia_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * Patch one 16-bit field inside a configuration read that covers it. The
 * defects below are told on the wire rather than written into QEMU's own copy
 * of configuration space, for two reasons: a device reset would undo a written
 * one before the guest ever looked, and rewriting the Express capability's
 * type in place makes QEMU's own PCI Express code assert. A device that lies
 * only when asked is also the more faithful model of a device that lies.
 */
static uint32_t patch_field(uint32_t value, uint32_t address, int len,
                            uint32_t field, uint16_t clear, uint16_t set)
{
    unsigned shift;

    if (address > field || address + (uint32_t)len < field + 2) {
        return value;
    }
    shift = (field - address) * 8;
    value &= ~((uint32_t)clear << shift);
    value |= (uint32_t)set << shift;
    return value;
}

static uint32_t phipia_nvidia_config_read(PCIDevice *dev, uint32_t address,
                                          int len)
{
    PhipiaNvidiaState *state = PHIPIA_NVIDIA_MODEL(dev);
    uint32_t value = pci_default_read_config(dev, address, len);

    if (state->defects & PHIPIA_NVIDIA_DEFECT_MESSAGE_INTERRUPTS) {
        value = patch_field(value, address, len,
                            PHIPIA_NVIDIA_MSI_OFFSET + PCI_MSI_FLAGS,
                            0, PCI_MSI_FLAGS_ENABLE);
    }
    if (state->defects & PHIPIA_NVIDIA_DEFECT_ROOT_PORT) {
        value = patch_field(value, address, len,
                            PHIPIA_NVIDIA_EXPRESS_OFFSET + PCI_EXP_FLAGS,
                            PCI_EXP_FLAGS_TYPE,
                            PCI_EXP_TYPE_ROOT_PORT <<
                                PCI_EXP_FLAGS_TYPE_SHIFT);
    }
    return value;
}

static void phipia_nvidia_realize(PCIDevice *dev, Error **errp)
{
    PhipiaNvidiaState *state = PHIPIA_NVIDIA_MODEL(dev);

    if (state->vbios_path) {
        GError *error = NULL;
        gchar *contents = NULL;
        gsize length = 0;

        if (!g_file_get_contents(state->vbios_path, &contents, &length,
                                 &error)) {
            error_setg(errp, "could not read the ROM image: %s",
                       error ? error->message : "unknown error");
            g_clear_error(&error);
            return;
        }
        if (length > NV_PROM_BYTES) {
            length = NV_PROM_BYTES;
        }
        memset(state->rom, 0xFF, sizeof(state->rom));
        memcpy(state->rom, contents, length);
        state->rom_present = true;
        g_free(contents);
    }

    /*
     * The board this chip is soldered to. NVIDIA sells the silicon to add-in
     * board partners, so the subsystem identity is theirs; it is a property
     * here so a driver reading it is checked against a value pinned outside
     * both the driver and this model.
     */
    pci_set_word(dev->config + PCI_SUBSYSTEM_VENDOR_ID,
                 (uint16_t)state->subsystem);
    pci_set_word(dev->config + PCI_SUBSYSTEM_ID,
                 (uint16_t)(state->subsystem >> 16));

    /* Powers up with the shadow enabled, which is what makes it worth a bit. */
    state->shadow = NV_ROM_SHADOW_BIT;

    memory_region_init_io(&state->registers, OBJECT(state),
                          &phipia_nvidia_ops, state,
                          "phipia-nvidia-model/registers",
                          PHIPIA_NVIDIA_BAR_BYTES);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_32, &state->registers);

    /*
     * Reading a register can change it, so the window above is not
     * prefetchable. The framebuffer is memory and is, and it is 64-bit
     * because it is larger than a 32-bit aperture is worth spending.
     */
    memory_region_init_ram(&state->framebuffer, OBJECT(state),
                           "phipia-nvidia-model/framebuffer",
                           PHIPIA_NVIDIA_FB_BYTES, &error_fatal);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &state->framebuffer);

    /*
     * A PCI Express function carries power management, and QEMU's own
     * capability machinery installs it: the version and the D0 power state a
     * driver reads back are the specification's defaults through QEMU's code
     * rather than values this file made up.
     */
    if (!(state->defects & PHIPIA_NVIDIA_DEFECT_NO_POWER_MANAGEMENT)) {
        if (pci_add_capability(dev, PCI_CAP_ID_PM, PHIPIA_NVIDIA_PM_OFFSET,
                               PCI_PM_SIZEOF, errp) < 0) {
            return;
        }
        pci_set_word(dev->config + PHIPIA_NVIDIA_PM_OFFSET + PCI_PM_PMC,
                     PCI_PM_CAP_VER_1_2);
        pci_set_word(dev->wmask + PHIPIA_NVIDIA_PM_OFFSET + PCI_PM_CTRL,
                     PCI_PM_CTRL_STATE_MASK);
    }

    /*
     * And message-signalled interrupts, which this model declares and never
     * enables. A driver whose job is to notice that nothing turned them on
     * needs a function that could have been turned on.
     */
    if (msi_init(dev, PHIPIA_NVIDIA_MSI_OFFSET, PHIPIA_NVIDIA_MSI_VECTORS,
                 true, false, errp) < 0) {
        return;
    }

    /*
     * The expansion ROM declaration. This is a declaration and nothing more:
     * there is no memory behind it, which is why it is left disabled and made
     * read-only. A model that presented an executable option ROM would have it
     * executed by the firmware long before the kernel under test ever ran.
     */
    pci_set_long(dev->config + PCI_ROM_ADDRESS, state->rom_declaration);
    pci_set_long(dev->wmask + PCI_ROM_ADDRESS, 0);

    /*
     * Every NVIDIA graphics part made this century is a PCI Express endpoint,
     * so the model is one too: a driver that asks how wide its link is, or
     * what kind of port it is, gets a real capability structure to read rather
     * than a fabricated answer.
     */
    if (pcie_endpoint_cap_init(dev, PHIPIA_NVIDIA_EXPRESS_OFFSET) < 0) {
        error_setg(errp, "could not install the PCI Express capability");
        return;
    }
}

static void phipia_nvidia_exit(PCIDevice *dev)
{
    msi_uninit(dev);
}

static Property phipia_nvidia_properties[] = {
    DEFINE_PROP_UINT32("boot0", PhipiaNvidiaState, boot0, 0x134000A1),
    DEFINE_PROP_UINT32("boot1", PhipiaNvidiaState, boot1, 0x00000000),
    DEFINE_PROP_UINT32("straps", PhipiaNvidiaState, straps, 0x0000042C),
    DEFINE_PROP_UINT32("enable", PhipiaNvidiaState, enable, 0x11111111),
    DEFINE_PROP_UINT32("subsystem", PhipiaNvidiaState, subsystem, 0x87651043),
    DEFINE_PROP_UINT32("numerator", PhipiaNvidiaState, numerator, 27),
    DEFINE_PROP_UINT32("denominator", PhipiaNvidiaState, denominator, 1000),
    DEFINE_PROP_UINT32("rom", PhipiaNvidiaState, rom_declaration, 0),
    DEFINE_PROP_UINT32("defects", PhipiaNvidiaState, defects, 0),
    DEFINE_PROP_STRING("vbios", PhipiaNvidiaState, vbios_path),
    DEFINE_PROP_END_OF_LIST(),
};

static void phipia_nvidia_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pci = PCI_DEVICE_CLASS(klass);

    pci->realize = phipia_nvidia_realize;
    pci->exit = phipia_nvidia_exit;
    pci->config_read = phipia_nvidia_config_read;
    pci->vendor_id = NVIDIA_VENDOR_ID;
    pci->device_id = 0x1B80;
    pci->revision = 0xA1;
    pci->class_id = PCI_CLASS_DISPLAY_VGA;
    dc->desc = "Model of the NVIDIA register interface (not a GPU)";
    device_class_set_props(dc, phipia_nvidia_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo phipia_nvidia_info = {
    .name = TYPE_PHIPIA_NVIDIA_MODEL,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PhipiaNvidiaState),
    .class_init = phipia_nvidia_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { },
    },
};

static void phipia_nvidia_register_types(void)
{
    type_register_static(&phipia_nvidia_info);
}

type_init(phipia_nvidia_register_types)
