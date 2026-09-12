/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Intel High Definition Audio: codec control and one kernel-owned PCM stream.
 *
 * This is the first Phipia driver whose device writes into kernel memory for
 * something other than storage or networking, and the ordering that makes that
 * safe is the whole point of the file. Phipia has no IOMMU. A device with bus
 * mastering enabled can write anywhere, so bus mastering is enabled only after
 * every ring, BDL and PCM region is a typed DMA allocation declared to the
 * claim, and it is withdrawn only after the stream and ring engines have been
 * stopped and the controller put back into reset. Memory is reclaimed after
 * that, never before.
 *
 * The conversation itself is the ordinary one. Software writes a verb into the
 * command ring and advances the write pointer; the controller reads it, puts
 * it on the link, and writes whatever the codec answers into the response ring,
 * advancing its own write pointer. Reading that pointer is how a driver knows
 * an answer arrived, and it is the only thing in this file that waits.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/audio.h>
#include <phipia/clock.h>
#include <phipia/cpu.h>
#include <phipia/dma.h>
#include <phipia/interrupt_vector.h>
#include <phipia/memory.h>
#include <phipia/msix.h>
#include <phipia/paging.h>
#include <phipia/pci.h>
#include <phipia/pci_resource.h>

/* PCI Code and ID Assignment Specification 1.19: multimedia, HD Audio. */
#define AUDIO_PCI_VENDOR UINT16_C(0x8086)
#define AUDIO_PCI_DEVICE UINT16_C(0x293E)
#define AUDIO_PCI_CLASS UINT8_C(0x04)
#define AUDIO_PCI_SUBCLASS UINT8_C(0x03)
#define AUDIO_REGISTER_BAR 0U
#define AUDIO_MINIMUM_REGISTER_BYTES UINT64_C(0x100)

/* High Definition Audio Specification 1.0a, section 3.3. */
#define HDA_GCAP UINT64_C(0x00)
#define HDA_VMIN UINT64_C(0x02)
#define HDA_VMAJ UINT64_C(0x03)
#define HDA_GCTL UINT64_C(0x08)
#define HDA_STATESTS UINT64_C(0x0E)
#define HDA_CORBLBASE UINT64_C(0x40)
#define HDA_CORBUBASE UINT64_C(0x44)
#define HDA_CORBWP UINT64_C(0x48)
#define HDA_CORBRP UINT64_C(0x4A)
#define HDA_CORBCTL UINT64_C(0x4C)
#define HDA_CORBSIZE UINT64_C(0x4E)
#define HDA_RIRBLBASE UINT64_C(0x50)
#define HDA_RIRBUBASE UINT64_C(0x54)
#define HDA_RIRBWP UINT64_C(0x58)
#define HDA_RINTCNT UINT64_C(0x5A)
#define HDA_RIRBCTL UINT64_C(0x5C)
#define HDA_RIRBSTS UINT64_C(0x5D)
#define HDA_RIRBSIZE UINT64_C(0x5E)
#define HDA_STREAM_DESCRIPTOR_BASE UINT64_C(0x80)
#define HDA_STREAM_DESCRIPTOR_BYTES UINT64_C(0x20)
#define HDA_SDCTL UINT64_C(0x00)
#define HDA_SDCTL_TAG UINT64_C(0x02)
#define HDA_SDSTS UINT64_C(0x03)
#define HDA_SDLPIB UINT64_C(0x04)
#define HDA_SDCBL UINT64_C(0x08)
#define HDA_SDLVI UINT64_C(0x0C)
#define HDA_SDFMT UINT64_C(0x12)
#define HDA_SDBDPL UINT64_C(0x18)
#define HDA_SDBDPU UINT64_C(0x1C)

#define HDA_GCTL_CONTROLLER_RESET UINT32_C(0x00000001)
#define HDA_CORBRP_RESET UINT16_C(0x8000)
#define HDA_CORBCTL_DMA_ENABLE UINT8_C(0x02)
#define HDA_RIRBWP_RESET UINT16_C(0x8000)
#define HDA_RIRBCTL_DMA_ENABLE UINT8_C(0x02)
#define HDA_SDCTL_STREAM_RESET UINT8_C(0x01)
#define HDA_SDCTL_RUN UINT8_C(0x02)
#define HDA_SDSTS_BUFFER_COMPLETE UINT8_C(0x04)
#define HDA_SDSTS_FIFO_ERROR UINT8_C(0x08)
#define HDA_SDSTS_DESCRIPTOR_ERROR UINT8_C(0x10)
#define HDA_SDSTS_ACKNOWLEDGE (HDA_SDSTS_BUFFER_COMPLETE | \
    HDA_SDSTS_FIFO_ERROR | HDA_SDSTS_DESCRIPTOR_ERROR)
#define HDA_BDL_INTERRUPT_ON_COMPLETION UINT32_C(0x00000001)

/*
 * Section 3.3.28 and 3.3.29: the controller counts responses and stops taking
 * commands once it has accumulated the number the interrupt count names, until
 * software acknowledges the response-interrupt flag. A driver that polls the
 * response ring rather than taking that interrupt has to do both things: set
 * the threshold beyond the number of answers it will ever collect in one
 * conversation, and acknowledge the flag after every answer anyway, because a
 * threshold is a bound and not a promise.
 */
#define HDA_RESPONSE_INTERRUPT_COUNT UINT16_C(0x00FF)
#define HDA_RIRBSTS_RESPONSE_INTERRUPT UINT8_C(0x01)
#define HDA_RIRBSTS_OVERRUN UINT8_C(0x04)
#define HDA_RIRBSTS_ACKNOWLEDGE \
    (HDA_RIRBSTS_RESPONSE_INTERRUPT | HDA_RIRBSTS_OVERRUN)

/*
 * Section 3.3.24 and 3.3.30: the low two bits select the ring size and bits
 * 6:4 report which sizes the controller supports - two entries, sixteen, or
 * two hundred and fifty six.
 */
#define HDA_RING_SIZE_SELECT_MASK UINT8_C(0x03)
#define HDA_RING_SIZE_CAPABILITY_SHIFT 4U
#define HDA_RING_SIZE_SUPPORTS_2 UINT8_C(0x01)
#define HDA_RING_SIZE_SUPPORTS_16 UINT8_C(0x02)
#define HDA_RING_SIZE_SUPPORTS_256 UINT8_C(0x04)

/* Section 3.3.2: the global capabilities field, taken apart. */
#define HDA_GCAP_OUTPUT_SHIFT 12U
#define HDA_GCAP_INPUT_SHIFT 8U
#define HDA_GCAP_BIDIRECTIONAL_SHIFT 3U
#define HDA_GCAP_SERIAL_DATA_SHIFT 1U
#define HDA_GCAP_STREAM_MASK UINT32_C(0x0F)
#define HDA_GCAP_BIDIRECTIONAL_MASK UINT32_C(0x1F)
#define HDA_GCAP_SERIAL_DATA_MASK UINT32_C(0x03)

/*
 * Section 7.3.3.1: GET_PARAMETER is a twelve-bit verb, so it sits in bits 19
 * through 8 of the command and its parameter identifier in the low eight.
 */
#define HDA_VERB_GET_PARAMETER UINT32_C(0xF00)
#define HDA_PARAMETER_VENDOR_ID UINT32_C(0x00)
#define HDA_PARAMETER_REVISION_ID UINT32_C(0x02)
#define HDA_PARAMETER_SUBORDINATE_NODES UINT32_C(0x04)
#define HDA_PARAMETER_FUNCTION_GROUP_TYPE UINT32_C(0x05)
#define HDA_PARAMETER_AUDIO_WIDGET_CAPS UINT32_C(0x09)
#define HDA_PARAMETER_PCM UINT32_C(0x0A)
#define HDA_PARAMETER_PIN_CAPS UINT32_C(0x0C)
#define HDA_PARAMETER_CONNECTION_LIST_LENGTH UINT32_C(0x0E)
#define HDA_FUNCTION_GROUP_AUDIO UINT8_C(0x01)
#define HDA_ROOT_NODE 0U

#define HDA_WIDGET_TYPE_SHIFT 20U
#define HDA_WIDGET_TYPE_MASK UINT32_C(0x0F)
#define HDA_WIDGET_AUDIO_OUTPUT UINT32_C(0x00)
#define HDA_WIDGET_PIN_COMPLEX UINT32_C(0x04)
#define HDA_WIDGET_STEREO UINT32_C(0x00000001)
#define HDA_PIN_CAP_OUTPUT UINT32_C(0x00000010)
#define HDA_PCM_RATE_48000 UINT32_C(0x00000040)
#define HDA_PCM_BITS_16 UINT32_C(0x00020000)

#define HDA_VERB_GET_CONNECTION_LIST UINT32_C(0xF02)
#define HDA_VERB_GET_CONVERTER UINT32_C(0xF06)
#define HDA_VERB_GET_PIN_CONTROL UINT32_C(0xF07)
#define HDA_VERB_SET_POWER_STATE UINT32_C(0x705)
#define HDA_VERB_SET_CHANNEL_STREAM UINT32_C(0x706)
#define HDA_VERB_SET_PIN_CONTROL UINT32_C(0x707)
#define HDA_VERB_SET_STREAM_FORMAT UINT32_C(0x2)
#define HDA_VERB_GET_STREAM_FORMAT UINT32_C(0xA)
#define HDA_VERB_SET_AMP_GAIN_MUTE UINT32_C(0x3)
#define HDA_PIN_CONTROL_OUTPUT UINT32_C(0x40)
#define HDA_AMP_OUTPUT_LEFT_RIGHT_MAX UINT32_C(0xB04A)
#define HDA_POWER_D0 UINT32_C(0x00)

#define HDA_CODEC_ADDRESS_SHIFT 28U
#define HDA_NODE_SHIFT 20U
#define HDA_VERB_SHIFT 8U
#define HDA_4BIT_VERB_SHIFT 16U

/* Section 7.3.4.11: the subordinate node count packs a start and a count. */
#define HDA_SUBORDINATE_START_SHIFT 16U

#define AUDIO_MAX_WIDGETS 64U
#define AUDIO_MAX_SERVICE_ITERATIONS 1000000U
#define AUDIO_MAX_UNDERRUN_RECOVERIES 3U
#define AUDIO_STREAM_FORMAT UINT16_C(0x0011)
#define AUDIO_TONE_HALF_PERIOD_FRAMES 32U
#define AUDIO_TONE_AMPLITUDE INT16_C(8192)
#define AUDIO_NATIVE_COALESCE_NS UINT64_C(10000000)
#define AUDIO_NATIVE_SERVICE_NS UINT64_C(100000)

_Static_assert(AUDIO_NATIVE_STREAMS == 2U,
    "the fixed mixer has exactly two logical inputs");

struct hda_bdl_entry {
    uint64_t address;
    uint32_t length;
    uint32_t flags;
} __attribute__((packed));

_Static_assert(sizeof(struct hda_bdl_entry) == 16U,
    "HD Audio BDL entry layout changed");

struct audio_route {
    uint8_t codec;
    uint8_t function_group;
    uint8_t converter;
    uint8_t pin;
    uint8_t pin_connection_index;
};

enum audio_stream_event {
    AUDIO_STREAM_EVENT_NONE = 0,
    AUDIO_STREAM_EVENT_COMPLETION,
    AUDIO_STREAM_EVENT_UNDERRUN,
    AUDIO_STREAM_EVENT_FATAL
};

struct audio_census {
    struct frame_allocator_stats frames;
    struct paging_state paging;
    struct dma_state dma;
    struct pci_resource_state pci;
    struct interrupt_vector_state vectors;
    struct msix_state msix;
    bool interrupts_enabled;
};

struct audio_controller {
    struct pci_device_claim claim;
    struct pci_mmio_region *region;
    volatile uint8_t *registers;
    struct dma_allocation command_ring;
    struct dma_allocation response_ring;
    struct dma_allocation buffer_list;
    struct dma_allocation pcm_buffer;
    volatile uint32_t *commands;
    volatile uint32_t *responses;
    volatile struct hda_bdl_entry *descriptors;
    volatile int16_t *samples;
    uint64_t output_stream_base;
    uint64_t buffer_list_physical;
    uint64_t pcm_physical;
    struct audio_route route;
    uint16_t command_entries;
    uint16_t response_entries;
    uint16_t command_write;
    uint16_t response_read;
    bool mapped;
    bool bus_master;
    bool rings_running;
    bool stream_descriptor_selected;
    bool stream_running;
    bool controller_running;
};

enum audio_native_terminal {
    AUDIO_NATIVE_TERMINAL_NONE = 0,
    AUDIO_NATIVE_TERMINAL_COMPLETE,
    AUDIO_NATIVE_TERMINAL_CANCELED,
    AUDIO_NATIVE_TERMINAL_ERROR
};

struct audio_native_stream {
    int16_t samples[AUDIO_PCM_FRAMES * AUDIO_PCM_CHANNELS];
    uint64_t owner_generation;
    uint64_t token;
    uint32_t left_q15;
    uint32_t right_q15;
    enum audio_native_terminal terminal;
    bool open;
    bool queued;
    bool active;
    bool cancel_after_active;
    bool closing;
};

struct audio_native_runtime {
    struct audio_controller controller;
    struct audio_proof_result proof;
    struct audio_native_stream streams[AUDIO_NATIVE_STREAMS];
    uint64_t owner_generation;
    uint64_t coalesce_deadline;
    uint64_t playback_deadline;
    uint32_t active_mask;
    uint32_t underrun_recoveries;
    bool initialized;
    bool stream_running;
    bool coalesce_service_grace;
};

static struct audio_proof_result installed_result;
static bool audio_active;
static bool audio_native_cleanup_clean = true;
/* This generation is intentionally outside the zeroed controller runtime.
 * A drain waiter may outlive the last close and must not resolve a newly
 * opened stream with the same process generation through a recycled token. */
static uint64_t audio_native_next_token = UINT64_C(1);
static struct audio_native_runtime audio_native;

static void zero_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static uint8_t mmio_read8(volatile uint8_t *base, uint64_t offset)
{
    return *(volatile uint8_t *)(void *)(base + offset);
}

static uint16_t mmio_read16(volatile uint8_t *base, uint64_t offset)
{
    return *(volatile uint16_t *)(void *)(base + offset);
}

static uint32_t mmio_read32(volatile uint8_t *base, uint64_t offset)
{
    return *(volatile uint32_t *)(void *)(base + offset);
}

static void mmio_write8(volatile uint8_t *base, uint64_t offset, uint8_t value)
{
    *(volatile uint8_t *)(void *)(base + offset) = value;
}

static void mmio_write16(
    volatile uint8_t *base,
    uint64_t offset,
    uint16_t value
)
{
    *(volatile uint16_t *)(void *)(base + offset) = value;
}

static void mmio_write32(
    volatile uint8_t *base,
    uint64_t offset,
    uint32_t value
)
{
    *(volatile uint32_t *)(void *)(base + offset) = value;
}

static bool deadline_reached(uint64_t now, uint64_t deadline)
{
    return now >= deadline;
}

static uint64_t output_stream_offset(uint32_t input_streams)
{
    return HDA_STREAM_DESCRIPTOR_BASE +
        (uint64_t)input_streams * HDA_STREAM_DESCRIPTOR_BYTES;
}

static uint64_t fill_pcm_tone(volatile int16_t *samples, size_t frames)
{
    uint64_t hash = UINT64_C(1469598103934665603);

    for (size_t frame = 0U; frame < frames; ++frame) {
        const int16_t value =
            (frame / AUDIO_TONE_HALF_PERIOD_FRAMES) % 2U == 0U ?
                AUDIO_TONE_AMPLITUDE : -AUDIO_TONE_AMPLITUDE;

        for (size_t channel = 0U; channel < AUDIO_PCM_CHANNELS; ++channel) {
            samples[frame * AUDIO_PCM_CHANNELS + channel] = value;
            hash ^= (uint16_t)value;
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash;
}

static uint64_t pcm_hash(const volatile int16_t *samples, size_t sample_count)
{
    uint64_t hash = UINT64_C(1469598103934665603);

    for (size_t index = 0U; index < sample_count; ++index) {
        hash ^= (uint16_t)samples[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int16_t clamp_sample(int32_t value)
{
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

static void mix_two_streams(
    volatile int16_t *destination,
    const int16_t *first,
    uint32_t first_left,
    uint32_t first_right,
    const int16_t *second,
    uint32_t second_left,
    uint32_t second_right,
    size_t frames
)
{
    for (size_t frame = 0U; frame < frames; ++frame) {
        for (size_t channel = 0U; channel < AUDIO_PCM_CHANNELS; ++channel) {
            const size_t index = frame * AUDIO_PCM_CHANNELS + channel;
            const uint32_t first_gain = channel == 0U ?
                first_left : first_right;
            const uint32_t second_gain = channel == 0U ?
                second_left : second_right;
            int32_t mixed = 0;

            if (first != NULL) {
                mixed += ((int32_t)first[index] * (int32_t)first_gain) /
                    (int32_t)AUDIO_NATIVE_VOLUME_UNITY;
            }
            if (second != NULL) {
                mixed += ((int32_t)second[index] * (int32_t)second_gain) /
                    (int32_t)AUDIO_NATIVE_VOLUME_UNITY;
            }
            destination[index] = clamp_sample(mixed);
        }
    }
}

static bool fill_buffer_list(
    volatile struct hda_bdl_entry *entries,
    uint64_t pcm_physical,
    bool every_period_reports
)
{
    if (entries == NULL ||
        AUDIO_PCM_BDL_ENTRIES * sizeof(*entries) > PAGING_PAGE_SIZE ||
        AUDIO_PCM_PERIOD_BYTES * AUDIO_PCM_BDL_ENTRIES !=
            AUDIO_PCM_DMA_BYTES ||
        AUDIO_PCM_PERIOD_BYTES % 128U != 0U) {
        return false;
    }

    for (size_t index = 0U; index < AUDIO_PCM_BDL_ENTRIES; ++index) {
        entries[index].address = pcm_physical +
            index * AUDIO_PCM_PERIOD_BYTES;
        entries[index].length = AUDIO_PCM_PERIOD_BYTES;
        entries[index].flags = (every_period_reports || index == 0U) ?
                HDA_BDL_INTERRUPT_ON_COMPLETION : 0U;
    }
    return true;
}

static enum audio_stream_event classify_stream_status(uint8_t status)
{
    if ((status & HDA_SDSTS_DESCRIPTOR_ERROR) != 0U) {
        return AUDIO_STREAM_EVENT_FATAL;
    }
    if ((status & HDA_SDSTS_FIFO_ERROR) != 0U) {
        return AUDIO_STREAM_EVENT_UNDERRUN;
    }
    if ((status & HDA_SDSTS_BUFFER_COMPLETE) != 0U) {
        return AUDIO_STREAM_EVENT_COMPLETION;
    }
    return AUDIO_STREAM_EVENT_NONE;
}

static bool wait_gctl(volatile uint8_t *base, uint32_t expected)
{
    const uint64_t start = clock_monotonic_ns();
    const uint64_t deadline = start + AUDIO_TIMEOUT_NS;

    if (deadline < start) {
        return false;
    }
    while ((mmio_read32(base, HDA_GCTL) & HDA_GCTL_CONTROLLER_RESET) !=
            expected) {
        if (deadline_reached(clock_monotonic_ns(), deadline)) {
            return false;
        }
        __asm__ volatile ("" : : : "memory");
    }
    return true;
}

static bool wait_stream_control(
    const struct audio_controller *controller,
    uint8_t mask,
    bool set
)
{
    const uint64_t start = clock_monotonic_ns();
    const uint64_t deadline = start + AUDIO_TIMEOUT_NS;

    if (deadline < start) {
        return false;
    }
    for (;;) {
        const bool observed = (mmio_read8(controller->registers,
            controller->output_stream_base + HDA_SDCTL) & mask) != 0U;

        if (observed == set) {
            return true;
        }
        if (deadline_reached(clock_monotonic_ns(), deadline)) {
            return false;
        }
        __asm__ volatile ("" : : : "memory");
    }
}

static bool stop_output_stream(
    struct audio_controller *controller,
    struct audio_proof_result *result
)
{
    uint8_t control;
    bool was_running;

    if (!controller->stream_descriptor_selected) {
        return true;
    }
    control = mmio_read8(controller->registers,
        controller->output_stream_base + HDA_SDCTL);
    was_running = controller->stream_running ||
        (control & HDA_SDCTL_RUN) != 0U;
    mmio_write8(controller->registers,
        controller->output_stream_base + HDA_SDCTL,
        (uint8_t)(control & (uint8_t)~HDA_SDCTL_RUN));
    if (!wait_stream_control(controller, HDA_SDCTL_RUN, false)) {
        return false;
    }
    controller->stream_running = false;
    if (result != NULL && was_running) {
        result->stream_stopped_before_reset = true;
    }
    return true;
}

static bool reset_output_stream(
    struct audio_controller *controller,
    struct audio_proof_result *result
)
{
    uint8_t control;

    if (!controller->stream_descriptor_selected ||
        !stop_output_stream(controller, result)) {
        return false;
    }
    control = mmio_read8(controller->registers,
        controller->output_stream_base + HDA_SDCTL);
    mmio_write8(controller->registers,
        controller->output_stream_base + HDA_SDCTL,
        (uint8_t)(control | HDA_SDCTL_STREAM_RESET));
    if (!wait_stream_control(controller, HDA_SDCTL_STREAM_RESET, true)) {
        return false;
    }
    control = mmio_read8(controller->registers,
        controller->output_stream_base + HDA_SDCTL);
    mmio_write8(controller->registers,
        controller->output_stream_base + HDA_SDCTL,
        (uint8_t)(control & (uint8_t)~HDA_SDCTL_STREAM_RESET));
    if (!wait_stream_control(controller, HDA_SDCTL_STREAM_RESET, false)) {
        return false;
    }
    if (result != NULL) {
        result->stream_reset = true;
    }
    return true;
}

static bool program_output_stream(struct audio_controller *controller)
{
    if (!controller->stream_descriptor_selected ||
        controller->buffer_list_physical == 0U ||
        controller->pcm_physical == 0U) {
        return false;
    }

    mmio_write8(controller->registers,
        controller->output_stream_base + HDA_SDSTS,
        HDA_SDSTS_ACKNOWLEDGE);
    mmio_write32(controller->registers,
        controller->output_stream_base + HDA_SDBDPL,
        (uint32_t)controller->buffer_list_physical);
    mmio_write32(controller->registers,
        controller->output_stream_base + HDA_SDBDPU,
        (uint32_t)(controller->buffer_list_physical >> 32U));
    mmio_write32(controller->registers,
        controller->output_stream_base + HDA_SDCBL, AUDIO_PCM_DMA_BYTES);
    mmio_write16(controller->registers,
        controller->output_stream_base + HDA_SDLVI,
        AUDIO_PCM_BDL_ENTRIES - 1U);
    mmio_write16(controller->registers,
        controller->output_stream_base + HDA_SDFMT, AUDIO_STREAM_FORMAT);
    mmio_write8(controller->registers,
        controller->output_stream_base + HDA_SDCTL_TAG,
        (uint8_t)(AUDIO_PCM_STREAM_TAG << 4U));
    return mmio_read32(controller->registers,
            controller->output_stream_base + HDA_SDCBL) ==
                AUDIO_PCM_DMA_BYTES &&
        mmio_read16(controller->registers,
            controller->output_stream_base + HDA_SDLVI) ==
                AUDIO_PCM_BDL_ENTRIES - 1U &&
        mmio_read16(controller->registers,
            controller->output_stream_base + HDA_SDFMT) ==
                AUDIO_STREAM_FORMAT;
}

static bool start_output_stream(
    struct audio_controller *controller,
    struct audio_proof_result *result
)
{
    uint8_t control;

    if (!program_output_stream(controller)) {
        return false;
    }
    control = mmio_read8(controller->registers,
        controller->output_stream_base + HDA_SDCTL);
    mmio_write8(controller->registers,
        controller->output_stream_base + HDA_SDCTL,
        (uint8_t)(control | HDA_SDCTL_RUN));
    if (!wait_stream_control(controller, HDA_SDCTL_RUN, true)) {
        return false;
    }
    controller->stream_running = true;
    if (result != NULL) {
        result->stream_started = true;
    }
    return true;
}

static void capture_census(struct audio_census *census)
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
    const struct audio_census *left,
    const struct audio_census *right
)
{
    return left->frames.free_frames == right->frames.free_frames &&
        left->frames.allocated_frames == right->frames.allocated_frames &&
        left->paging.table_frames == right->paging.table_frames &&
        left->paging.root_physical_address ==
            right->paging.root_physical_address &&
        left->dma.active_allocations == right->dma.active_allocations &&
        left->dma.cpu_owned_allocations == right->dma.cpu_owned_allocations &&
        left->dma.device_owned_allocations ==
            right->dma.device_owned_allocations &&
        left->pci.active_claims == right->pci.active_claims &&
        left->pci.active_mappings == right->pci.active_mappings &&
        left->pci.mapped_pages == right->pci.mapped_pages &&
        left->pci.bus_masters == right->pci.bus_masters &&
        left->vectors.allocated == right->vectors.allocated &&
        left->msix.active_bindings == right->msix.active_bindings &&
        left->interrupts_enabled == right->interrupts_enabled;
}

static const struct pci_function *discover_controller(void)
{
    for (size_t index = 0U; index < pci_function_count(); ++index) {
        const struct pci_function *function = pci_function_at(index);

        if (function != NULL && function->vendor_id == AUDIO_PCI_VENDOR &&
            function->device_id == AUDIO_PCI_DEVICE &&
            function->class_code == AUDIO_PCI_CLASS &&
            function->subclass == AUDIO_PCI_SUBCLASS) {
            return function;
        }
    }
    return NULL;
}

/*
 * The largest ring the controller says it supports, and the encoding that
 * selects it. A controller that supports nothing is refused rather than
 * assumed to mean the smallest.
 */
static bool select_ring_size(
    uint8_t size_register,
    uint16_t *entries,
    uint8_t *selection
)
{
    const uint8_t capability = (uint8_t)(size_register >>
        HDA_RING_SIZE_CAPABILITY_SHIFT);

    if ((capability & HDA_RING_SIZE_SUPPORTS_256) != 0U) {
        *entries = 256U;
        *selection = 2U;
        return true;
    }
    if ((capability & HDA_RING_SIZE_SUPPORTS_16) != 0U) {
        *entries = 16U;
        *selection = 1U;
        return true;
    }
    if ((capability & HDA_RING_SIZE_SUPPORTS_2) != 0U) {
        *entries = 2U;
        *selection = 0U;
        return true;
    }
    return false;
}

/*
 * Section 3.3.21: the command read pointer is reset by setting its reset bit,
 * observing the controller acknowledge it, clearing it, and observing that.
 * Both halves are waited on, because a controller that never acknowledges is a
 * controller whose ring position is unknown.
 */
static bool reset_command_read_pointer(volatile uint8_t *base)
{
    uint64_t deadline = clock_monotonic_ns() + AUDIO_TIMEOUT_NS;

    mmio_write16(base, HDA_CORBRP, HDA_CORBRP_RESET);
    while ((mmio_read16(base, HDA_CORBRP) & HDA_CORBRP_RESET) == 0U) {
        if (deadline_reached(clock_monotonic_ns(), deadline)) {
            return false;
        }
        __asm__ volatile ("" : : : "memory");
    }
    mmio_write16(base, HDA_CORBRP, 0U);
    deadline = clock_monotonic_ns() + AUDIO_TIMEOUT_NS;
    while ((mmio_read16(base, HDA_CORBRP) & HDA_CORBRP_RESET) != 0U) {
        if (deadline_reached(clock_monotonic_ns(), deadline)) {
            return false;
        }
        __asm__ volatile ("" : : : "memory");
    }
    return true;
}

static uint32_t build_verb(uint8_t codec, uint8_t node, uint32_t parameter)
{
    return ((uint32_t)codec << HDA_CODEC_ADDRESS_SHIFT) |
        ((uint32_t)node << HDA_NODE_SHIFT) |
        (HDA_VERB_GET_PARAMETER << HDA_VERB_SHIFT) | parameter;
}

static uint32_t build_command(
    uint8_t codec,
    uint8_t node,
    uint32_t command
)
{
    return ((uint32_t)codec << HDA_CODEC_ADDRESS_SHIFT) |
        ((uint32_t)node << HDA_NODE_SHIFT) |
        (command & UINT32_C(0x000FFFFF));
}

static uint32_t command_12bit(uint32_t verb, uint32_t payload)
{
    return (verb << HDA_VERB_SHIFT) | (payload & UINT32_C(0xFF));
}

static uint32_t command_4bit(uint32_t verb, uint32_t payload)
{
    return (verb << HDA_4BIT_VERB_SHIFT) | (payload & UINT32_C(0xFFFF));
}

/*
 * One command, one answer. The write pointer is advanced only after the ring
 * entry itself is visible, and the answer is taken from the entry the
 * controller's own write pointer names - never from a position this side
 * guessed.
 */
static enum audio_status issue_command(
    struct audio_controller *controller,
    uint8_t codec,
    uint8_t node,
    uint32_t command,
    uint32_t *response
)
{
    const uint16_t next = (uint16_t)((controller->command_write + 1U) %
        controller->command_entries);
    uint64_t deadline;
    uint16_t observed;

    if (response == NULL || controller->command_entries == 0U ||
        controller->response_entries == 0U) {
        return AUDIO_STATUS_NULL_ARGUMENT;
    }
    controller->commands[next] = build_command(codec, node, command);
    cpu_store_fence();
    controller->command_write = next;
    mmio_write16(controller->registers, HDA_CORBWP, next);

    deadline = clock_monotonic_ns() + AUDIO_TIMEOUT_NS;
    for (;;) {
        observed = (uint16_t)(mmio_read16(controller->registers,
            HDA_RIRBWP) & (uint16_t)(controller->response_entries - 1U));
        if (observed != controller->response_read) {
            break;
        }
        if (deadline_reached(clock_monotonic_ns(), deadline)) {
            return AUDIO_STATUS_VERB_TIMEOUT;
        }
        __asm__ volatile ("" : : : "memory");
    }
    controller->response_read =
        (uint16_t)((controller->response_read + 1U) %
            controller->response_entries);
    if (controller->response_read != observed) {
        /*
         * The controller answered more than once for one command, which means
         * the ring holds something this driver did not ask for.
         */
        return AUDIO_STATUS_RESPONSE_AUTHENTICATION;
    }
    *response = controller->responses[
        (size_t)controller->response_read * 2U];
    /*
     * Section 4.4.2: the extended half of a response carries the address of
     * the codec that sent it, and marks the ones no command asked for.
     */
    if ((controller->responses[(size_t)controller->response_read * 2U + 1U] &
            UINT32_C(0x1F)) != (uint32_t)codec) {
        return AUDIO_STATUS_RESPONSE_AUTHENTICATION;
    }
    /*
     * An overrun means the controller wrote past what this side had read,
     * which for a polled driver is a lost answer rather than a late one.
     */
    if ((mmio_read8(controller->registers, HDA_RIRBSTS) &
            HDA_RIRBSTS_OVERRUN) != 0U) {
        return AUDIO_STATUS_RESPONSE_AUTHENTICATION;
    }
    mmio_write8(controller->registers, HDA_RIRBSTS,
        HDA_RIRBSTS_ACKNOWLEDGE);
    return AUDIO_STATUS_OK;
}

static enum audio_status issue_verb(
    struct audio_controller *controller,
    uint8_t codec,
    uint8_t node,
    uint32_t parameter,
    uint32_t *response
)
{
    return issue_command(controller, codec, node,
        command_12bit(HDA_VERB_GET_PARAMETER, parameter), response);
}

static enum audio_status counted_command(
    struct audio_controller *controller,
    struct audio_proof_result *result,
    uint8_t codec,
    uint8_t node,
    uint32_t command,
    uint32_t *response
)
{
    enum audio_status status;

    ++result->verbs_issued;
    status = issue_command(controller, codec, node, command, response);
    if (status == AUDIO_STATUS_OK) {
        ++result->responses_received;
    }
    return status;
}

static enum audio_status counted_parameter(
    struct audio_controller *controller,
    struct audio_proof_result *result,
    uint8_t codec,
    uint8_t node,
    uint32_t parameter,
    uint32_t *response
)
{
    enum audio_status status;

    ++result->verbs_issued;
    status = issue_verb(controller, codec, node, parameter, response);
    if (status == AUDIO_STATUS_OK) {
        ++result->responses_received;
    }
    return status;
}

/*
 * Everything that could be holding the device to memory, undone in the only
 * order that is safe: stop the ring engines, put the controller back in reset
 * so it cannot start them again, withdraw bus mastering, take the rings back
 * from the device, and only then release the memory they were.
 */
static enum audio_status release_controller(
    struct audio_controller *controller,
    struct audio_proof_result *result,
    enum audio_status status
)
{
    bool failed = false;

    if (controller->registers != NULL) {
        if (controller->stream_descriptor_selected) {
            if (!stop_output_stream(controller, result) ||
                !reset_output_stream(controller, result)) {
                failed = true;
            }
        }
        if (controller->rings_running) {
            mmio_write8(controller->registers, HDA_RIRBCTL, 0U);
            mmio_write8(controller->registers, HDA_CORBCTL, 0U);
            controller->rings_running = false;
        }
        if (controller->controller_running) {
            mmio_write32(controller->registers, HDA_GCTL, 0U);
            if (!wait_gctl(controller->registers, 0U)) {
                failed = true;
            }
            controller->controller_running = false;
        }
    }
    if (controller->bus_master) {
        if (pci_claim_disable_bus_master(&controller->claim) !=
                PCI_RESOURCE_STATUS_OK) {
            failed = true;
        }
        controller->bus_master = false;
    }
    if (result != NULL) {
        result->bus_master_withdrawn_before_release =
            !controller->bus_master &&
            (controller->command_ring.active ||
                controller->response_ring.active ||
                controller->buffer_list.active ||
                controller->pcm_buffer.active);
    }
    if (controller->pcm_buffer.active &&
        controller->pcm_buffer.owner == DMA_OWNER_DEVICE &&
        dma_transfer_to_cpu(&controller->pcm_buffer) != DMA_STATUS_OK) {
        failed = true;
    }
    if (controller->buffer_list.active &&
        controller->buffer_list.owner == DMA_OWNER_DEVICE &&
        dma_transfer_to_cpu(&controller->buffer_list) != DMA_STATUS_OK) {
        failed = true;
    }
    if (controller->response_ring.active &&
        controller->response_ring.owner == DMA_OWNER_DEVICE &&
        dma_transfer_to_cpu(&controller->response_ring) != DMA_STATUS_OK) {
        failed = true;
    }
    if (controller->command_ring.active &&
        controller->command_ring.owner == DMA_OWNER_DEVICE &&
        dma_transfer_to_cpu(&controller->command_ring) != DMA_STATUS_OK) {
        failed = true;
    }
    if (controller->pcm_buffer.active &&
        dma_release(&controller->pcm_buffer) != DMA_STATUS_OK) {
        failed = true;
    }
    if (controller->buffer_list.active &&
        dma_release(&controller->buffer_list) != DMA_STATUS_OK) {
        failed = true;
    }
    if (controller->response_ring.active &&
        dma_release(&controller->response_ring) != DMA_STATUS_OK) {
        failed = true;
    }
    if (controller->command_ring.active &&
        dma_release(&controller->command_ring) != DMA_STATUS_OK) {
        failed = true;
    }
    controller->commands = NULL;
    controller->responses = NULL;
    controller->descriptors = NULL;
    controller->samples = NULL;
    if (controller->mapped &&
        pci_claim_unmap_last_bar(&controller->claim, AUDIO_REGISTER_BAR) !=
            PCI_RESOURCE_STATUS_OK) {
        failed = true;
    }
    controller->mapped = false;
    controller->registers = NULL;
    controller->region = NULL;
    if (controller->claim.active &&
        pci_release_device(&controller->claim) != PCI_RESOURCE_STATUS_OK) {
        failed = true;
    }
    audio_active = false;
    return failed ? AUDIO_STATUS_TEARDOWN : status;
}

static enum audio_status identify_codec(
    struct audio_controller *controller,
    struct audio_proof_result *result,
    uint8_t address,
    struct audio_codec *codec
)
{
    uint32_t identity = 0U;
    uint32_t revision = 0U;
    uint32_t nodes = 0U;
    uint32_t group = 0U;
    enum audio_status status;

    codec->address = address;
    status = counted_parameter(controller, result, address, HDA_ROOT_NODE,
        HDA_PARAMETER_VENDOR_ID, &identity);
    if (status != AUDIO_STATUS_OK) {
        return status;
    }
    status = counted_parameter(controller, result, address, HDA_ROOT_NODE,
        HDA_PARAMETER_REVISION_ID, &revision);
    if (status != AUDIO_STATUS_OK) {
        return status;
    }
    status = counted_parameter(controller, result, address, HDA_ROOT_NODE,
        HDA_PARAMETER_SUBORDINATE_NODES, &nodes);
    if (status != AUDIO_STATUS_OK) {
        return status;
    }
    codec->vendor_device = identity;
    codec->revision = revision;
    codec->first_group_node = (uint8_t)(nodes >> HDA_SUBORDINATE_START_SHIFT);
    codec->group_node_count = (uint8_t)nodes;
    /*
     * Section 7.3.4.1 and 7.3.4.11: a codec names a vendor and a device, and
     * the root node names at least one function group somewhere above it.
     */
    if (identity == 0U || identity == UINT32_C(0xFFFFFFFF) ||
        (identity >> 16U) == 0U || codec->group_node_count == 0U ||
        codec->first_group_node == 0U) {
        return AUDIO_STATUS_IDENTITY;
    }
    status = counted_parameter(controller, result, address,
        codec->first_group_node, HDA_PARAMETER_FUNCTION_GROUP_TYPE, &group);
    if (status != AUDIO_STATUS_OK) {
        return status;
    }
    codec->function_group_type = (uint8_t)group;
    codec->audio_function_group =
        codec->function_group_type == HDA_FUNCTION_GROUP_AUDIO;
    codec->identified = true;
    return AUDIO_STATUS_OK;
}

static enum audio_status pin_connection_index(
    struct audio_controller *controller,
    struct audio_proof_result *result,
    uint8_t codec,
    uint8_t pin,
    uint8_t converter,
    bool *connected,
    uint8_t *connection_index
)
{
    uint32_t length_response = 0U;
    uint32_t response = 0U;
    uint32_t count;
    uint32_t entry_bits;
    uint32_t entries_per_response;
    uint32_t range_bit;
    uint32_t node_mask;
    enum audio_status status;

    if (connected == NULL || connection_index == NULL) {
        return AUDIO_STATUS_NULL_ARGUMENT;
    }
    *connected = false;
    *connection_index = 0U;
    status = counted_parameter(controller, result, codec, pin,
        HDA_PARAMETER_CONNECTION_LIST_LENGTH, &length_response);
    if (status != AUDIO_STATUS_OK) {
        return status;
    }

    count = length_response & UINT32_C(0x7F);
    entry_bits = (length_response & UINT32_C(0x80)) != 0U ? 16U : 8U;
    entries_per_response = 32U / entry_bits;
    range_bit = UINT32_C(1) << (entry_bits - 1U);
    node_mask = range_bit - 1U;
    if (count == 0U || count > AUDIO_MAX_WIDGETS) {
        return AUDIO_STATUS_WIDGET_TOPOLOGY;
    }

    for (uint32_t first = 0U; first < count;
         first += entries_per_response) {
        status = counted_command(controller, result, codec, pin,
            command_12bit(HDA_VERB_GET_CONNECTION_LIST, first), &response);
        if (status != AUDIO_STATUS_OK) {
            return status;
        }
        for (uint32_t slot = 0U; slot < entries_per_response &&
             first + slot < count; ++slot) {
            const uint32_t entry =
                (response >> (slot * entry_bits)) &
                    ((UINT32_C(1) << entry_bits) - 1U);

            /* The bounded QEMU route uses direct entries, not ranges. */
            if ((entry & range_bit) != 0U || (entry & node_mask) > UINT8_MAX) {
                return AUDIO_STATUS_WIDGET_TOPOLOGY;
            }
            if ((uint8_t)(entry & node_mask) == converter) {
                *connected = true;
                *connection_index = (uint8_t)(first + slot);
                return AUDIO_STATUS_OK;
            }
        }
    }
    return AUDIO_STATUS_OK;
}

static enum audio_status discover_output_route(
    struct audio_controller *controller,
    struct audio_proof_result *result
)
{
    uint8_t converters[AUDIO_MAX_WIDGETS];
    uint8_t pins[AUDIO_MAX_WIDGETS];
    size_t converter_count = 0U;
    size_t pin_count = 0U;
    bool audio_group_seen = false;

    for (size_t codec_index = 0U; codec_index < AUDIO_MAX_CODECS;
         ++codec_index) {
        const struct audio_codec *codec = &result->codecs[codec_index];
        uint32_t nodes = 0U;
        uint8_t first;
        uint8_t count;
        enum audio_status status;

        if (!codec->identified || !codec->audio_function_group) {
            continue;
        }
        audio_group_seen = true;
        status = counted_parameter(controller, result, codec->address,
            codec->first_group_node, HDA_PARAMETER_SUBORDINATE_NODES, &nodes);
        if (status != AUDIO_STATUS_OK) {
            return status;
        }
        first = (uint8_t)(nodes >> HDA_SUBORDINATE_START_SHIFT);
        count = (uint8_t)nodes;
        if (first == 0U || count == 0U || count > AUDIO_MAX_WIDGETS ||
            (uint16_t)first + count > UINT8_MAX) {
            return AUDIO_STATUS_WIDGET_TOPOLOGY;
        }

        converter_count = 0U;
        pin_count = 0U;
        for (uint16_t offset = 0U; offset < count; ++offset) {
            const uint8_t node = (uint8_t)((uint16_t)first + offset);
            uint32_t capabilities = 0U;
            uint32_t parameter = 0U;
            uint32_t type;

            status = counted_parameter(controller, result, codec->address,
                node, HDA_PARAMETER_AUDIO_WIDGET_CAPS, &capabilities);
            if (status != AUDIO_STATUS_OK) {
                return status;
            }
            type = (capabilities >> HDA_WIDGET_TYPE_SHIFT) &
                HDA_WIDGET_TYPE_MASK;
            if (type == HDA_WIDGET_AUDIO_OUTPUT &&
                (capabilities & HDA_WIDGET_STEREO) != 0U) {
                status = counted_parameter(controller, result,
                    codec->address, node, HDA_PARAMETER_PCM, &parameter);
                if (status != AUDIO_STATUS_OK) {
                    return status;
                }
                if ((parameter & (HDA_PCM_RATE_48000 | HDA_PCM_BITS_16)) ==
                        (HDA_PCM_RATE_48000 | HDA_PCM_BITS_16) &&
                    converter_count < AUDIO_MAX_WIDGETS) {
                    converters[converter_count++] = node;
                    result->pcm_profile_supported = true;
                }
            } else if (type == HDA_WIDGET_PIN_COMPLEX) {
                status = counted_parameter(controller, result,
                    codec->address, node, HDA_PARAMETER_PIN_CAPS, &parameter);
                if (status != AUDIO_STATUS_OK) {
                    return status;
                }
                if ((parameter & HDA_PIN_CAP_OUTPUT) != 0U &&
                    pin_count < AUDIO_MAX_WIDGETS) {
                    pins[pin_count++] = node;
                }
            }
        }

        for (size_t pin_index = 0U; pin_index < pin_count; ++pin_index) {
            for (size_t converter_index = 0U;
                 converter_index < converter_count; ++converter_index) {
                bool connected;
                uint8_t connection_index;

                status = pin_connection_index(controller, result,
                    codec->address, pins[pin_index],
                    converters[converter_index], &connected,
                    &connection_index);
                if (status != AUDIO_STATUS_OK) {
                    return status;
                }
                /* QEMU's output pin has one fixed, direct connection. */
                if (connected && connection_index == 0U) {
                    controller->route.codec = codec->address;
                    controller->route.function_group =
                        codec->first_group_node;
                    controller->route.converter =
                        converters[converter_index];
                    controller->route.pin = pins[pin_index];
                    controller->route.pin_connection_index = connection_index;
                    result->playback_codec = controller->route.codec;
                    result->playback_function_group =
                        controller->route.function_group;
                    result->playback_converter = controller->route.converter;
                    result->playback_pin = controller->route.pin;
                    result->output_route_found = true;
                    return AUDIO_STATUS_OK;
                }
            }
        }
    }

    if (!audio_group_seen) {
        return AUDIO_STATUS_WIDGET_TOPOLOGY;
    }
    return result->pcm_profile_supported ? AUDIO_STATUS_WIDGET_TOPOLOGY :
        AUDIO_STATUS_PCM_FORMAT;
}

static enum audio_status configure_output_route(
    struct audio_controller *controller,
    struct audio_proof_result *result
)
{
    const struct audio_route *route = &controller->route;
    uint32_t response = 0U;
    enum audio_status status;

    const uint32_t power = command_12bit(HDA_VERB_SET_POWER_STATE,
        HDA_POWER_D0);
    const uint32_t commands[][2] = {
        {route->function_group, power},
        {route->converter, power},
        {route->pin, power},
        {route->pin, command_12bit(HDA_VERB_SET_PIN_CONTROL,
            HDA_PIN_CONTROL_OUTPUT)},
        {route->converter, command_12bit(HDA_VERB_SET_CHANNEL_STREAM,
            AUDIO_PCM_STREAM_TAG << 4U)},
        {route->converter, command_4bit(HDA_VERB_SET_STREAM_FORMAT,
            AUDIO_STREAM_FORMAT)},
        {route->converter, command_4bit(HDA_VERB_SET_AMP_GAIN_MUTE,
            HDA_AMP_OUTPUT_LEFT_RIGHT_MAX)}
    };

    for (size_t index = 0U; index < sizeof(commands) / sizeof(commands[0]);
         ++index) {
        status = counted_command(controller, result, route->codec,
            (uint8_t)commands[index][0], commands[index][1], &response);
        if (status != AUDIO_STATUS_OK) {
            return status;
        }
    }

    status = counted_command(controller, result, route->codec,
        route->converter, command_12bit(HDA_VERB_GET_CONVERTER, 0U),
        &response);
    if (status != AUDIO_STATUS_OK || response !=
            (AUDIO_PCM_STREAM_TAG << 4U)) {
        return status == AUDIO_STATUS_OK ? AUDIO_STATUS_PCM_FORMAT : status;
    }
    status = counted_command(controller, result, route->codec,
        route->converter, command_4bit(HDA_VERB_GET_STREAM_FORMAT, 0U),
        &response);
    if (status != AUDIO_STATUS_OK || response != AUDIO_STREAM_FORMAT) {
        return status == AUDIO_STATUS_OK ? AUDIO_STATUS_PCM_FORMAT : status;
    }
    status = counted_command(controller, result, route->codec, route->pin,
        command_12bit(HDA_VERB_GET_PIN_CONTROL, 0U), &response);
    if (status != AUDIO_STATUS_OK || response != HDA_PIN_CONTROL_OUTPUT) {
        return status == AUDIO_STATUS_OK ?
            AUDIO_STATUS_WIDGET_TOPOLOGY : status;
    }
    return AUDIO_STATUS_OK;
}

static enum audio_status recover_output_underrun(
    struct audio_controller *controller,
    struct audio_proof_result *result
)
{
    if (result->underrun_recoveries >= AUDIO_MAX_UNDERRUN_RECOVERIES) {
        return AUDIO_STATUS_STREAM_ERROR;
    }
    if (!reset_output_stream(controller, result)) {
        return AUDIO_STATUS_STREAM_RESET;
    }
    if (!start_output_stream(controller, result)) {
        return AUDIO_STATUS_STREAM_START;
    }
    ++result->underrun_recoveries;
    return AUDIO_STATUS_OK;
}

static enum audio_status service_output_stream(
    struct audio_controller *controller,
    struct audio_proof_result *result
)
{
    const uint8_t status = mmio_read8(controller->registers,
        controller->output_stream_base + HDA_SDSTS);
    const enum audio_stream_event event = classify_stream_status(status);
    const uint32_t position = mmio_read32(controller->registers,
        controller->output_stream_base + HDA_SDLPIB);

    ++result->service_iterations;
    if (position >= AUDIO_PCM_DMA_BYTES) {
        return AUDIO_STATUS_STREAM_ERROR;
    }
    if (position != result->final_link_position) {
        result->link_position_advanced = true;
        result->final_link_position = position;
    }

    switch (event) {
    case AUDIO_STREAM_EVENT_NONE:
        return AUDIO_STATUS_OK;
    case AUDIO_STREAM_EVENT_COMPLETION:
        mmio_write8(controller->registers,
            controller->output_stream_base + HDA_SDSTS,
            HDA_SDSTS_BUFFER_COMPLETE);
        ++result->period_completions;
        result->stream_status_observed = true;
        return AUDIO_STATUS_OK;
    case AUDIO_STREAM_EVENT_UNDERRUN:
        mmio_write8(controller->registers,
            controller->output_stream_base + HDA_SDSTS,
            HDA_SDSTS_ACKNOWLEDGE);
        return recover_output_underrun(controller, result);
    case AUDIO_STREAM_EVENT_FATAL:
    default:
        mmio_write8(controller->registers,
            controller->output_stream_base + HDA_SDSTS,
            HDA_SDSTS_ACKNOWLEDGE);
        return AUDIO_STATUS_STREAM_ERROR;
    }
}

static enum audio_status prove_pcm_playback(
    struct audio_controller *controller,
    struct audio_proof_result *result
)
{
    const uint64_t start = clock_monotonic_ns();
    const uint64_t deadline = start + AUDIO_TIMEOUT_NS;

    if (deadline < start || !reset_output_stream(controller, result)) {
        return AUDIO_STATUS_STREAM_RESET;
    }
    if (!start_output_stream(controller, result)) {
        return AUDIO_STATUS_STREAM_START;
    }
    result->initial_link_position = mmio_read32(controller->registers,
        controller->output_stream_base + HDA_SDLPIB);
    if (result->initial_link_position >= AUDIO_PCM_DMA_BYTES) {
        return AUDIO_STATUS_STREAM_ERROR;
    }
    result->final_link_position = result->initial_link_position;

    while ((!result->link_position_advanced ||
            !result->stream_status_observed) &&
        result->service_iterations < AUDIO_MAX_SERVICE_ITERATIONS) {
        const enum audio_status status = service_output_stream(controller,
            result);

        if (status != AUDIO_STATUS_OK) {
            return status;
        }
        if (deadline_reached(clock_monotonic_ns(), deadline)) {
            return AUDIO_STATUS_STREAM_TIMEOUT;
        }
        __asm__ volatile ("" : : : "memory");
    }
    return result->link_position_advanced && result->stream_status_observed &&
        result->period_completions != 0U ? AUDIO_STATUS_OK :
            AUDIO_STATUS_STREAM_TIMEOUT;
}

static enum audio_status bring_up(
    struct audio_controller *controller,
    struct audio_proof_result *result,
    const struct pci_function *function,
    bool playback_proof
)
{
    struct pci_bus_master_request bus_master;
    struct dma_request request;
    volatile void *pointer = NULL;
    uint64_t command_physical;
    uint64_t response_physical;
    uint16_t present;
    uint8_t command_selection = 0U;
    uint8_t response_selection = 0U;

    if (pci_claim_device(function, &controller->claim) !=
            PCI_RESOURCE_STATUS_OK) {
        return AUDIO_STATUS_CLAIM_FAILURE;
    }
    if (pci_claim_map_bar(&controller->claim, AUDIO_REGISTER_BAR,
            &controller->region) != PCI_RESOURCE_STATUS_OK ||
        controller->region == NULL) {
        return AUDIO_STATUS_MAPPING_FAILURE;
    }
    controller->mapped = true;
    if (controller->region->size < AUDIO_MINIMUM_REGISTER_BYTES ||
        pci_mmio_subregion(controller->region, 0U, controller->region->size,
            &pointer) != PCI_RESOURCE_STATUS_OK || pointer == NULL) {
        return AUDIO_STATUS_REGISTER_WINDOW;
    }
    controller->registers = (volatile uint8_t *)pointer;

    /* Section 4.2.2: out of reset, then in again, then wait for the codecs. */
    mmio_write32(controller->registers, HDA_GCTL, 0U);
    if (!wait_gctl(controller->registers, 0U)) {
        return AUDIO_STATUS_RESET_TIMEOUT;
    }
    mmio_write32(controller->registers, HDA_GCTL,
        HDA_GCTL_CONTROLLER_RESET);
    if (!wait_gctl(controller->registers, HDA_GCTL_CONTROLLER_RESET)) {
        return AUDIO_STATUS_RESET_TIMEOUT;
    }
    controller->controller_running = true;
    result->controller_reset = true;

    result->capability = mmio_read16(controller->registers, HDA_GCAP);
    result->version = ((uint32_t)mmio_read8(controller->registers,
        HDA_VMAJ) << 8U) | mmio_read8(controller->registers, HDA_VMIN);
    result->output_streams = (result->capability >> HDA_GCAP_OUTPUT_SHIFT) &
        HDA_GCAP_STREAM_MASK;
    result->input_streams = (result->capability >> HDA_GCAP_INPUT_SHIFT) &
        HDA_GCAP_STREAM_MASK;
    result->bidirectional_streams =
        (result->capability >> HDA_GCAP_BIDIRECTIONAL_SHIFT) &
            HDA_GCAP_BIDIRECTIONAL_MASK;
    result->serial_data_out_signals =
        (result->capability >> HDA_GCAP_SERIAL_DATA_SHIFT) &
            HDA_GCAP_SERIAL_DATA_MASK;
    if ((result->version >> 8U) != 1U ||
        result->output_streams == 0U) {
        return AUDIO_STATUS_VERSION;
    }
    controller->output_stream_base = output_stream_offset(
        result->input_streams);
    if (controller->output_stream_base > controller->region->size ||
        HDA_STREAM_DESCRIPTOR_BYTES >
            controller->region->size - controller->output_stream_base) {
        return AUDIO_STATUS_REGISTER_WINDOW;
    }
    controller->stream_descriptor_selected = true;
    result->stream_descriptor_index = result->input_streams;

    {
        const uint64_t deadline = clock_monotonic_ns() + AUDIO_TIMEOUT_NS;

        for (;;) {
            present = mmio_read16(controller->registers, HDA_STATESTS);
            if (present != 0U) {
                break;
            }
            if (deadline_reached(clock_monotonic_ns(), deadline)) {
                return AUDIO_STATUS_CODEC_ABSENT;
            }
            __asm__ volatile ("" : : : "memory");
        }
    }

    /* Both engines stopped before either base register is written. */
    mmio_write8(controller->registers, HDA_CORBCTL, 0U);
    mmio_write8(controller->registers, HDA_RIRBCTL, 0U);

    if (!select_ring_size(mmio_read8(controller->registers, HDA_CORBSIZE),
            &controller->command_entries, &command_selection) ||
        !select_ring_size(mmio_read8(controller->registers, HDA_RIRBSIZE),
            &controller->response_entries, &response_selection) ||
        controller->command_entries > AUDIO_CORB_MAX_ENTRIES ||
        controller->response_entries > AUDIO_RIRB_MAX_ENTRIES) {
        return AUDIO_STATUS_RING_SIZE;
    }

    request.page_count = 1U;
    request.alignment = PAGING_PAGE_SIZE;
    request.maximum_physical_address = UINT64_C(0xFFFFFFFF);
    if (dma_allocate(&request, &controller->command_ring) != DMA_STATUS_OK ||
        dma_allocate(&request, &controller->response_ring) != DMA_STATUS_OK ||
        dma_allocate(&request, &controller->buffer_list) != DMA_STATUS_OK) {
        return AUDIO_STATUS_DMA_FAILURE;
    }
    if (AUDIO_PCM_DMA_BYTES % PAGING_PAGE_SIZE != 0U) {
        return AUDIO_STATUS_DMA_FAILURE;
    }
    request.page_count = AUDIO_PCM_DMA_BYTES / PAGING_PAGE_SIZE;
    if (dma_allocate(&request, &controller->pcm_buffer) != DMA_STATUS_OK) {
        return AUDIO_STATUS_DMA_FAILURE;
    }
    controller->commands = controller->command_ring.cpu_address;
    controller->responses = controller->response_ring.cpu_address;
    controller->descriptors = controller->buffer_list.cpu_address;
    controller->samples = controller->pcm_buffer.cpu_address;
    if (controller->commands == NULL || controller->responses == NULL ||
        controller->descriptors == NULL || controller->samples == NULL ||
        (uint64_t)controller->command_entries * AUDIO_CORB_ENTRY_BYTES >
            controller->command_ring.byte_length ||
        (uint64_t)controller->response_entries * AUDIO_RIRB_ENTRY_BYTES >
            controller->response_ring.byte_length ||
        AUDIO_PCM_BDL_ENTRIES * sizeof(*controller->descriptors) >
            controller->buffer_list.byte_length ||
        AUDIO_PCM_DMA_BYTES > controller->pcm_buffer.byte_length) {
        return AUDIO_STATUS_DMA_FAILURE;
    }
    for (uint64_t index = 0U;
         index < controller->command_ring.byte_length / 4U; ++index) {
        controller->commands[index] = 0U;
    }
    for (uint64_t index = 0U;
         index < controller->response_ring.byte_length / 4U; ++index) {
        controller->responses[index] = 0U;
    }
    zero_bytes(controller->buffer_list.cpu_address,
        (size_t)controller->buffer_list.byte_length);
    zero_bytes(controller->pcm_buffer.cpu_address,
        (size_t)controller->pcm_buffer.byte_length);

    command_physical =
        (uint64_t)controller->command_ring.frames.physical_base;
    response_physical =
        (uint64_t)controller->response_ring.frames.physical_base;
    controller->buffer_list_physical =
        (uint64_t)controller->buffer_list.frames.physical_base;
    controller->pcm_physical =
        (uint64_t)controller->pcm_buffer.frames.physical_base;
    if (!fill_buffer_list(controller->descriptors,
            controller->pcm_physical, playback_proof)) {
        return AUDIO_STATUS_DMA_FAILURE;
    }
    result->pcm_hash = playback_proof ?
        fill_pcm_tone(controller->samples, AUDIO_PCM_FRAMES) :
        pcm_hash(controller->samples,
            AUDIO_PCM_FRAMES * AUDIO_PCM_CHANNELS);
    result->sample_rate = AUDIO_PCM_SAMPLE_RATE;
    result->channels = AUDIO_PCM_CHANNELS;
    result->bits_per_sample = AUDIO_PCM_BITS_PER_SAMPLE;
    result->pcm_frames = AUDIO_PCM_FRAMES;
    result->pcm_bytes = AUDIO_PCM_BYTES;
    result->bdl_entries = AUDIO_PCM_BDL_ENTRIES;
    result->stream_format = AUDIO_STREAM_FORMAT;
    result->playback_stream_tag = AUDIO_PCM_STREAM_TAG;
    mmio_write32(controller->registers, HDA_CORBLBASE,
        (uint32_t)command_physical);
    mmio_write32(controller->registers, HDA_CORBUBASE,
        (uint32_t)(command_physical >> 32U));
    mmio_write8(controller->registers, HDA_CORBSIZE,
        (uint8_t)((mmio_read8(controller->registers, HDA_CORBSIZE) &
            (uint8_t)~HDA_RING_SIZE_SELECT_MASK) | command_selection));
    mmio_write32(controller->registers, HDA_RIRBLBASE,
        (uint32_t)response_physical);
    mmio_write32(controller->registers, HDA_RIRBUBASE,
        (uint32_t)(response_physical >> 32U));
    mmio_write8(controller->registers, HDA_RIRBSIZE,
        (uint8_t)((mmio_read8(controller->registers, HDA_RIRBSIZE) &
            (uint8_t)~HDA_RING_SIZE_SELECT_MASK) | response_selection));
    mmio_write16(controller->registers, HDA_RINTCNT,
        HDA_RESPONSE_INTERRUPT_COUNT);

    if (!reset_command_read_pointer(controller->registers)) {
        return AUDIO_STATUS_RING_RESET;
    }
    mmio_write16(controller->registers, HDA_CORBWP, 0U);
    mmio_write16(controller->registers, HDA_RIRBWP, HDA_RIRBWP_RESET);
    controller->command_write = 0U;
    controller->response_read = 0U;

    /*
     * Bus mastering is refused while the rings still belong to this side.
     * Proving that refusal happens is worth as much as the enable that
     * follows it: it is the check that stops a device being let loose on
     * memory nobody declared.
     */
    bus_master.allocations[0] = &controller->command_ring;
    bus_master.allocations[1] = &controller->response_ring;
    bus_master.allocations[2] = &controller->buffer_list;
    bus_master.allocations[3] = &controller->pcm_buffer;
    bus_master.allocation_count = 4U;
    if (pci_claim_enable_bus_master(&controller->claim, &bus_master) !=
            PCI_RESOURCE_STATUS_DMA_NOT_PREPARED) {
        return AUDIO_STATUS_BUS_MASTER_GUARD_FAILURE;
    }
    if (dma_mark_initialized(&controller->command_ring) != DMA_STATUS_OK ||
        dma_mark_initialized(&controller->response_ring) != DMA_STATUS_OK ||
        dma_mark_initialized(&controller->buffer_list) != DMA_STATUS_OK ||
        dma_mark_initialized(&controller->pcm_buffer) != DMA_STATUS_OK ||
        dma_transfer_to_device(&controller->command_ring) != DMA_STATUS_OK ||
        dma_transfer_to_device(&controller->response_ring) != DMA_STATUS_OK ||
        dma_transfer_to_device(&controller->buffer_list) != DMA_STATUS_OK ||
        dma_transfer_to_device(&controller->pcm_buffer) != DMA_STATUS_OK) {
        return AUDIO_STATUS_DMA_FAILURE;
    }
    result->bdl_device_owned_during_run =
        dma_is_device_owned(&controller->buffer_list);
    result->pcm_device_owned_during_run =
        dma_is_device_owned(&controller->pcm_buffer);
    if (pci_claim_enable_bus_master(&controller->claim, &bus_master) !=
            PCI_RESOURCE_STATUS_OK) {
        return AUDIO_STATUS_BUS_MASTER_FAILURE;
    }
    controller->bus_master = true;

    cpu_store_fence();
    mmio_write8(controller->registers, HDA_CORBCTL,
        HDA_CORBCTL_DMA_ENABLE);
    mmio_write8(controller->registers, HDA_RIRBCTL,
        HDA_RIRBCTL_DMA_ENABLE);
    if ((mmio_read8(controller->registers, HDA_CORBCTL) &
            HDA_CORBCTL_DMA_ENABLE) == 0U ||
        (mmio_read8(controller->registers, HDA_RIRBCTL) &
            HDA_RIRBCTL_DMA_ENABLE) == 0U) {
        return AUDIO_STATUS_RING_START;
    }
    controller->rings_running = true;
    result->rings_running = true;
    result->corb_entries = controller->command_entries;
    result->rirb_entries = controller->response_entries;

    for (uint8_t address = 0U; address < AUDIO_MAX_CODECS; ++address) {
        if ((present & (uint16_t)(1U << address)) == 0U) {
            continue;
        }
        ++result->codecs_present;
    }
    if (result->codecs_present == 0U) {
        return AUDIO_STATUS_CODEC_ABSENT;
    }
    for (uint8_t address = 0U; address < AUDIO_MAX_CODECS; ++address) {
        enum audio_status status;

        if ((present & (uint16_t)(1U << address)) == 0U) {
            continue;
        }
        status = identify_codec(controller, result,
            address, &result->codecs[address]);
        if (status != AUDIO_STATUS_OK) {
            return status;
        }
        ++result->codecs_identified;
        if (result->codecs[address].audio_function_group) {
            result->audio_function_group_found = true;
        }
    }
    {
        enum audio_status status = discover_output_route(controller, result);

        if (status != AUDIO_STATUS_OK) {
            return status;
        }
        status = configure_output_route(controller, result);
        if (status != AUDIO_STATUS_OK) {
            return status;
        }
        if (playback_proof) {
            status = prove_pcm_playback(controller, result);
            if (status != AUDIO_STATUS_OK) {
                return status;
            }
        }
    }
    result->device_wrote_response_ring = result->responses_received ==
        result->verbs_issued && result->responses_received != 0U;
    if (!result->device_wrote_response_ring ||
        !result->audio_function_group_found || !result->output_route_found ||
        !result->pcm_profile_supported ||
        !result->pcm_device_owned_during_run ||
        !result->bdl_device_owned_during_run) {
        return AUDIO_STATUS_IDENTITY;
    }
    return AUDIO_STATUS_OK;
}

bool audio_foundation_self_test(size_t *completed_tests)
{
    struct audio_controller probe;
    struct hda_bdl_entry descriptors[AUDIO_PCM_BDL_ENTRIES];
    int16_t samples[AUDIO_TONE_HALF_PERIOD_FRAMES *
        AUDIO_PCM_CHANNELS * 2U];
    uint64_t tone_hash;
    uint16_t entries = 0U;
    uint8_t selection = 0xFFU;
    size_t completed = 0U;

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;

    /* A verb is exactly the command word the specification describes. */
    if (build_verb(0U, HDA_ROOT_NODE, HDA_PARAMETER_VENDOR_ID) !=
            UINT32_C(0x000F0000) ||
        build_verb(1U, HDA_ROOT_NODE, HDA_PARAMETER_REVISION_ID) !=
            UINT32_C(0x100F0002) ||
        build_verb(2U, 1U, HDA_PARAMETER_FUNCTION_GROUP_TYPE) !=
            UINT32_C(0x201F0005)) {
        return false;
    }
    ++completed;
    /* The codec address and node fields do not overlap the verb. */
    if ((build_verb(AUDIO_MAX_CODECS, 0x7FU, 0U) & UINT32_C(0x000FFFFF)) !=
            (HDA_VERB_GET_PARAMETER << HDA_VERB_SHIFT)) {
        return false;
    }
    ++completed;
    /* The largest supported ring is taken, and only a supported one. */
    if (!select_ring_size(UINT8_C(0x40), &entries, &selection) ||
        entries != 256U || selection != 2U) {
        return false;
    }
    ++completed;
    if (!select_ring_size(UINT8_C(0x20), &entries, &selection) ||
        entries != 16U || selection != 1U) {
        return false;
    }
    ++completed;
    if (!select_ring_size(UINT8_C(0x10), &entries, &selection) ||
        entries != 2U || selection != 0U) {
        return false;
    }
    ++completed;
    if (select_ring_size(UINT8_C(0x00), &entries, &selection) ||
        select_ring_size(UINT8_C(0x0F), &entries, &selection)) {
        return false;
    }
    ++completed;
    /* Both rings fit the one page each is allocated from. */
    if (AUDIO_CORB_MAX_ENTRIES * AUDIO_CORB_ENTRY_BYTES > PAGING_PAGE_SIZE ||
        AUDIO_RIRB_MAX_ENTRIES * AUDIO_RIRB_ENTRY_BYTES > PAGING_PAGE_SIZE) {
        return false;
    }
    ++completed;
    /* A ring size is a power of two, which is what the wrap depends on. */
    if ((AUDIO_CORB_MAX_ENTRIES & (AUDIO_CORB_MAX_ENTRIES - 1U)) != 0U ||
        (AUDIO_RIRB_MAX_ENTRIES & (AUDIO_RIRB_MAX_ENTRIES - 1U)) != 0U) {
        return false;
    }
    ++completed;
    /* Twelve- and four-bit codec commands occupy the documented 20 bits. */
    if (build_command(0U, 2U,
            command_12bit(HDA_VERB_SET_CHANNEL_STREAM, 0x10U)) !=
            UINT32_C(0x00270610) ||
        build_command(0U, 2U,
            command_4bit(HDA_VERB_SET_STREAM_FORMAT,
                AUDIO_STREAM_FORMAT)) != UINT32_C(0x00220011)) {
        return false;
    }
    ++completed;
    /* Input descriptors precede output descriptors in the register window. */
    if (output_stream_offset(0U) != UINT64_C(0x80) ||
        output_stream_offset(4U) != UINT64_C(0x100)) {
        return false;
    }
    ++completed;
    /* The fixed payload is followed by one page of zero drain guard. */
    if (AUDIO_STREAM_FORMAT != UINT16_C(0x0011) ||
        AUDIO_PCM_FRAME_BYTES != 4U || AUDIO_PCM_BYTES != PAGING_PAGE_SIZE ||
        AUDIO_PCM_PERIOD_BYTES != AUDIO_PCM_BYTES ||
        AUDIO_PCM_DMA_BYTES != 2U * PAGING_PAGE_SIZE) {
        return false;
    }
    ++completed;
    /* The proof observes both periods; native playback reports after payload. */
    zero_bytes(descriptors, sizeof(descriptors));
    if (!fill_buffer_list(descriptors, UINT64_C(0x00100000), true) ||
        descriptors[0].address != UINT64_C(0x00100000) ||
        descriptors[1].address != UINT64_C(0x00101000) ||
        descriptors[0].length != AUDIO_PCM_PERIOD_BYTES ||
        descriptors[1].length != AUDIO_PCM_PERIOD_BYTES ||
        descriptors[0].flags != HDA_BDL_INTERRUPT_ON_COMPLETION ||
        descriptors[1].flags != HDA_BDL_INTERRUPT_ON_COMPLETION) {
        return false;
    }
    ++completed;
    /* The integer-only square wave is non-silent, stereo and deterministic. */
    tone_hash = fill_pcm_tone(samples,
        AUDIO_TONE_HALF_PERIOD_FRAMES * 2U);
    if (tone_hash == 0U || samples[0] != AUDIO_TONE_AMPLITUDE ||
        samples[1] != AUDIO_TONE_AMPLITUDE ||
        samples[AUDIO_TONE_HALF_PERIOD_FRAMES * AUDIO_PCM_CHANNELS] !=
            -AUDIO_TONE_AMPLITUDE ||
        fill_pcm_tone(samples, AUDIO_TONE_HALF_PERIOD_FRAMES * 2U) !=
            tone_hash) {
        return false;
    }
    ++completed;
    /* Descriptor faults dominate underruns; completions remain acknowledgeable. */
    if (classify_stream_status(0U) != AUDIO_STREAM_EVENT_NONE ||
        classify_stream_status(HDA_SDSTS_BUFFER_COMPLETE) !=
            AUDIO_STREAM_EVENT_COMPLETION ||
        classify_stream_status(HDA_SDSTS_FIFO_ERROR) !=
            AUDIO_STREAM_EVENT_UNDERRUN ||
        classify_stream_status((uint8_t)(HDA_SDSTS_DESCRIPTOR_ERROR |
            HDA_SDSTS_FIFO_ERROR)) != AUDIO_STREAM_EVENT_FATAL) {
        return false;
    }
    ++completed;
    /* Releasing a controller that was never brought up changes nothing. */
    zero_bytes(&probe, sizeof(probe));
    audio_active = true;
    if (release_controller(&probe, NULL, AUDIO_STATUS_OK) !=
            AUDIO_STATUS_OK || audio_active) {
        return false;
    }
    ++completed;
    /* Every status has a message and the table is complete. */
    for (int status = 0; status < (int)AUDIO_STATUS_COUNT; ++status) {
        const char *message = audio_status_string((enum audio_status)status);

        if (message == NULL || message[0] == '\0') {
            return false;
        }
    }
    if (audio_status_string(AUDIO_STATUS_COUNT)[0] != 'u' ||
        !audio_resources_released()) {
        return false;
    }
    ++completed;
    *completed_tests = completed;
    return completed == AUDIO_CONTROLLED_CONTROLS;
}

enum audio_status audio_prove(struct audio_proof_result *result)
{
    struct audio_controller controller;
    struct audio_census before;
    struct audio_census after;
    const struct pci_function *function;
    const bool restore_interrupts = cpu_interrupts_enabled();
    enum audio_status status;
    size_t completed = 0U;

    if (result == NULL) {
        return AUDIO_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(result, sizeof(*result));
    if (audio_active) {
        return AUDIO_STATUS_BUSY;
    }
    if (!pci_is_initialized() || !pci_resource_get_state().active ||
        !dma_get_state().active) {
        return AUDIO_STATUS_PREREQUISITE;
    }
    if (!audio_foundation_self_test(&completed) ||
        completed != AUDIO_CONTROLLED_CONTROLS) {
        return AUDIO_STATUS_PREREQUISITE;
    }
    result->controls = (uint32_t)completed;
    function = discover_controller();
    if (function == NULL) {
        return AUDIO_STATUS_ABSENT;
    }

    cpu_interrupt_disable();
    capture_census(&before);
    zero_bytes(&controller, sizeof(controller));
    audio_active = true;
    status = bring_up(&controller, result, function, true);
    status = release_controller(&controller, result, status);
    capture_census(&after);
    if (restore_interrupts) {
        cpu_interrupt_enable();
    }
    result->teardown_complete = pci_resource_verify() ==
            PCI_RESOURCE_STATUS_OK && dma_verify() == DMA_STATUS_OK &&
        dma_get_state().active_allocations == 0U &&
        pci_resource_get_state().bus_masters == 0U;
    result->resource_census_equal = census_equal(&before, &after);
    if (status != AUDIO_STATUS_OK) {
        zero_bytes(result, sizeof(*result));
        return status;
    }
    if (!result->teardown_complete) {
        zero_bytes(result, sizeof(*result));
        return AUDIO_STATUS_TEARDOWN;
    }
    if (!result->resource_census_equal) {
        zero_bytes(result, sizeof(*result));
        return AUDIO_STATUS_RESOURCE_CENSUS;
    }
    installed_result = *result;
    return AUDIO_STATUS_OK;
}

static struct audio_native_stream *native_stream(
    uint64_t owner_generation,
    uint64_t stream_token
)
{
    if (!audio_native.initialized || owner_generation == 0U ||
        stream_token == 0U ||
        audio_native.owner_generation != owner_generation) {
        return NULL;
    }
    for (size_t index = 0U; index < AUDIO_NATIVE_STREAMS; ++index) {
        struct audio_native_stream *stream = &audio_native.streams[index];

        if (stream->open && stream->owner_generation == owner_generation &&
            stream->token == stream_token) {
            return stream;
        }
    }
    return NULL;
}

static size_t native_open_streams(void)
{
    size_t count = 0U;

    for (size_t index = 0U; index < AUDIO_NATIVE_STREAMS; ++index) {
        if (audio_native.streams[index].open) {
            ++count;
        }
    }
    return count;
}

static size_t native_queued_streams(void)
{
    size_t count = 0U;

    for (size_t index = 0U; index < AUDIO_NATIVE_STREAMS; ++index) {
        if (audio_native.streams[index].open &&
            audio_native.streams[index].queued &&
            !audio_native.streams[index].active) {
            ++count;
        }
    }
    return count;
}

static bool native_mix_ready(
    size_t queued,
    size_t open,
    uint64_t now,
    uint64_t deadline,
    bool service_grace
)
{
    return queued != 0U &&
        (queued == open || (!service_grace && now >= deadline));
}

static uint64_t native_allocate_token(void)
{
    uint64_t token = audio_native_next_token++;

    if (token == 0U) {
        token = audio_native_next_token++;
    }
    return token;
}

static bool native_release_runtime(void)
{
    enum audio_status status;
    bool clean;

    if (!audio_native.initialized) {
        return true;
    }
    status = release_controller(&audio_native.controller,
        &audio_native.proof, AUDIO_STATUS_OK);
    clean = status == AUDIO_STATUS_OK &&
        !audio_native.controller.claim.active &&
        !audio_native.controller.command_ring.active &&
        !audio_native.controller.response_ring.active &&
        !audio_native.controller.buffer_list.active &&
        !audio_native.controller.pcm_buffer.active &&
        pci_resource_verify() == PCI_RESOURCE_STATUS_OK &&
        dma_verify() == DMA_STATUS_OK;
    zero_bytes(&audio_native, sizeof(audio_native));
    audio_native_cleanup_clean = clean;
    return clean;
}

static void native_finish_active(enum audio_native_terminal terminal)
{
    const uint32_t active = audio_native.active_mask;

    for (size_t index = 0U; index < AUDIO_NATIVE_STREAMS; ++index) {
        struct audio_native_stream *stream = &audio_native.streams[index];

        if ((active & (UINT32_C(1) << index)) == 0U) {
            continue;
        }
        stream->active = false;
        stream->queued = false;
        stream->terminal = stream->cancel_after_active ?
            AUDIO_NATIVE_TERMINAL_CANCELED : terminal;
        stream->cancel_after_active = false;
        if (stream->closing) {
            zero_bytes(stream, sizeof(*stream));
        }
    }
    audio_native.active_mask = 0U;
    audio_native.stream_running = false;
    audio_native.playback_deadline = 0U;
    audio_native.underrun_recoveries = 0U;
    if (native_queued_streams() != 0U) {
        const uint64_t now = clock_monotonic_ns();

        audio_native.coalesce_deadline = now <=
                UINT64_MAX - AUDIO_NATIVE_COALESCE_NS ?
            now + AUDIO_NATIVE_COALESCE_NS : now;
        audio_native.coalesce_service_grace = false;
    } else {
        audio_native.coalesce_deadline = 0U;
        audio_native.coalesce_service_grace = false;
    }
}

static void native_fail_queued(void)
{
    for (size_t index = 0U; index < AUDIO_NATIVE_STREAMS; ++index) {
        struct audio_native_stream *stream = &audio_native.streams[index];

        if (stream->open && stream->queued && !stream->active) {
            stream->queued = false;
            stream->terminal = AUDIO_NATIVE_TERMINAL_ERROR;
            if (stream->closing) {
                zero_bytes(stream, sizeof(*stream));
            }
        }
    }
    audio_native.coalesce_deadline = 0U;
    audio_native.coalesce_service_grace = false;
}

static bool native_start_mix(void)
{
    const int16_t *sources[AUDIO_NATIVE_STREAMS] = {NULL, NULL};
    uint32_t left[AUDIO_NATIVE_STREAMS] = {0U, 0U};
    uint32_t right[AUDIO_NATIVE_STREAMS] = {0U, 0U};
    uint32_t mask = 0U;
    uint64_t now;

    if (!audio_native.initialized || audio_native.stream_running ||
        audio_native.controller.pcm_buffer.owner != DMA_OWNER_DEVICE ||
        !reset_output_stream(&audio_native.controller,
            &audio_native.proof)) {
        return false;
    }
    for (size_t index = 0U; index < AUDIO_NATIVE_STREAMS; ++index) {
        struct audio_native_stream *stream = &audio_native.streams[index];

        if (!stream->open || !stream->queued || stream->active) {
            continue;
        }
        sources[index] = stream->samples;
        left[index] = stream->left_q15;
        right[index] = stream->right_q15;
        mask |= UINT32_C(1) << index;
    }
    if (mask == 0U ||
        dma_transfer_to_cpu(&audio_native.controller.pcm_buffer) !=
            DMA_STATUS_OK) {
        return false;
    }
    mix_two_streams(audio_native.controller.samples,
        sources[0], left[0], right[0], sources[1], left[1], right[1],
        AUDIO_PCM_FRAMES);
    audio_native.proof.pcm_hash = pcm_hash(
        audio_native.controller.samples,
        AUDIO_PCM_FRAMES * AUDIO_PCM_CHANNELS);
    if (dma_transfer_to_device(&audio_native.controller.pcm_buffer) !=
            DMA_STATUS_OK) {
        return false;
    }
    cpu_store_fence();
    if (!start_output_stream(&audio_native.controller,
            &audio_native.proof)) {
        (void)stop_output_stream(&audio_native.controller,
            &audio_native.proof);
        (void)reset_output_stream(&audio_native.controller,
            &audio_native.proof);
        return false;
    }
    for (size_t index = 0U; index < AUDIO_NATIVE_STREAMS; ++index) {
        if ((mask & (UINT32_C(1) << index)) != 0U) {
            audio_native.streams[index].active = true;
        }
    }
    audio_native.active_mask = mask;
    audio_native.stream_running = true;
    audio_native.coalesce_deadline = 0U;
    audio_native.coalesce_service_grace = false;
    now = clock_monotonic_ns();
    audio_native.playback_deadline = now <= UINT64_MAX - AUDIO_TIMEOUT_NS ?
        now + AUDIO_TIMEOUT_NS : now;
    return true;
}

enum audio_native_status audio_native_open(
    uint64_t owner_generation,
    uint64_t *stream_token
)
{
    struct audio_native_stream *stream = NULL;

    if (owner_generation == 0U || stream_token == NULL) {
        return AUDIO_NATIVE_NULL_ARGUMENT;
    }
    *stream_token = 0U;
    if (cpu_interrupts_enabled()) {
        return AUDIO_NATIVE_IO;
    }
    if (!audio_native.initialized) {
        const struct pci_function *function;
        enum audio_status status;

        if (audio_active) {
            return AUDIO_NATIVE_BUSY;
        }
        function = discover_controller();
        if (function == NULL) {
            return AUDIO_NATIVE_ABSENT;
        }
        zero_bytes(&audio_native, sizeof(audio_native));
        audio_active = true;
        audio_native_cleanup_clean = false;
        status = bring_up(&audio_native.controller, &audio_native.proof,
            function, false);
        if (status != AUDIO_STATUS_OK) {
            const enum audio_status release_status = release_controller(
                &audio_native.controller, &audio_native.proof, status);
            const bool clean = release_status != AUDIO_STATUS_TEARDOWN &&
                !audio_native.controller.claim.active &&
                !audio_native.controller.command_ring.active &&
                !audio_native.controller.response_ring.active &&
                !audio_native.controller.buffer_list.active &&
                !audio_native.controller.pcm_buffer.active &&
                pci_resource_verify() == PCI_RESOURCE_STATUS_OK &&
                dma_verify() == DMA_STATUS_OK;

            zero_bytes(&audio_native, sizeof(audio_native));
            audio_native_cleanup_clean = clean;
            return status == AUDIO_STATUS_ABSENT ?
                AUDIO_NATIVE_ABSENT : AUDIO_NATIVE_IO;
        }
        audio_native.initialized = true;
        audio_native.owner_generation = owner_generation;
    } else if (audio_native.owner_generation != owner_generation) {
        return AUDIO_NATIVE_BUSY;
    }
    for (size_t index = 0U; index < AUDIO_NATIVE_STREAMS; ++index) {
        if (!audio_native.streams[index].open) {
            stream = &audio_native.streams[index];
            break;
        }
    }
    if (stream == NULL) {
        return AUDIO_NATIVE_BUSY;
    }
    zero_bytes(stream, sizeof(*stream));
    stream->open = true;
    stream->owner_generation = owner_generation;
    stream->left_q15 = AUDIO_NATIVE_VOLUME_UNITY;
    stream->right_q15 = AUDIO_NATIVE_VOLUME_UNITY;
    stream->token = native_allocate_token();
    *stream_token = stream->token;
    return AUDIO_NATIVE_OK;
}

enum audio_native_status audio_native_submit(
    uint64_t owner_generation,
    uint64_t stream_token,
    const int16_t *samples,
    size_t byte_length
)
{
    struct audio_native_stream *stream = native_stream(owner_generation,
        stream_token);

    if (samples == NULL) {
        return AUDIO_NATIVE_NULL_ARGUMENT;
    }
    if (stream == NULL) {
        return AUDIO_NATIVE_STALE;
    }
    if (byte_length != AUDIO_PCM_BYTES || stream->closing) {
        return AUDIO_NATIVE_INVALID;
    }
    if (stream->queued || stream->active) {
        return AUDIO_NATIVE_BUSY;
    }
    for (size_t index = 0U;
         index < AUDIO_PCM_FRAMES * AUDIO_PCM_CHANNELS; ++index) {
        stream->samples[index] = samples[index];
    }
    stream->queued = true;
    stream->terminal = AUDIO_NATIVE_TERMINAL_NONE;
    stream->cancel_after_active = false;
    if (!audio_native.stream_running) {
        const uint64_t now = clock_monotonic_ns();
        const size_t queued = native_queued_streams();

        audio_native.coalesce_deadline = now <=
                UINT64_MAX - AUDIO_NATIVE_COALESCE_NS ?
            now + AUDIO_NATIVE_COALESCE_NS : now;
        audio_native.coalesce_service_grace =
            queued != 0U && queued < native_open_streams();
    }
    return AUDIO_NATIVE_OK;
}

enum audio_native_status audio_native_set_volume(
    uint64_t owner_generation,
    uint64_t stream_token,
    uint32_t left_q15,
    uint32_t right_q15
)
{
    struct audio_native_stream *stream = native_stream(owner_generation,
        stream_token);

    if (stream == NULL) {
        return AUDIO_NATIVE_STALE;
    }
    if (left_q15 > AUDIO_NATIVE_VOLUME_UNITY ||
        right_q15 > AUDIO_NATIVE_VOLUME_UNITY || stream->closing) {
        return AUDIO_NATIVE_INVALID;
    }
    stream->left_q15 = left_q15;
    stream->right_q15 = right_q15;
    return AUDIO_NATIVE_OK;
}

enum audio_native_status audio_native_cancel(
    uint64_t owner_generation,
    uint64_t stream_token
)
{
    struct audio_native_stream *stream = native_stream(owner_generation,
        stream_token);

    if (stream == NULL) {
        return AUDIO_NATIVE_STALE;
    }
    if (stream->active) {
        /* One mixed DMA chunk is atomic once the controller owns it. */
        stream->cancel_after_active = true;
    } else {
        stream->queued = false;
        stream->terminal = AUDIO_NATIVE_TERMINAL_CANCELED;
    }
    return AUDIO_NATIVE_OK;
}

enum audio_native_status audio_native_close(
    uint64_t owner_generation,
    uint64_t stream_token
)
{
    struct audio_native_stream *stream = native_stream(owner_generation,
        stream_token);

    if (stream == NULL) {
        return AUDIO_NATIVE_STALE;
    }
    if (stream->active) {
        stream->closing = true;
        stream->cancel_after_active = true;
    } else {
        zero_bytes(stream, sizeof(*stream));
    }
    if (native_open_streams() == 0U && !audio_native.stream_running) {
        return native_release_runtime() ? AUDIO_NATIVE_OK : AUDIO_NATIVE_IO;
    }
    return AUDIO_NATIVE_OK;
}

enum audio_native_drain_state audio_native_drain(
    uint64_t owner_generation,
    uint64_t stream_token
)
{
    const struct audio_native_stream *stream = native_stream(
        owner_generation, stream_token);

    if (stream == NULL) {
        return AUDIO_NATIVE_DRAIN_STALE;
    }
    if (stream->queued || stream->active) {
        return AUDIO_NATIVE_DRAIN_PENDING;
    }
    if (stream->terminal == AUDIO_NATIVE_TERMINAL_CANCELED) {
        return AUDIO_NATIVE_DRAIN_CANCELED;
    }
    if (stream->terminal == AUDIO_NATIVE_TERMINAL_ERROR) {
        return AUDIO_NATIVE_DRAIN_ERROR;
    }
    return AUDIO_NATIVE_DRAIN_COMPLETE;
}

enum audio_native_status audio_native_poll(
    uint64_t owner_generation,
    uint64_t stream_token,
    bool *writable,
    bool *closed
)
{
    const struct audio_native_stream *stream = native_stream(
        owner_generation, stream_token);

    if (writable == NULL || closed == NULL) {
        return AUDIO_NATIVE_NULL_ARGUMENT;
    }
    *writable = false;
    *closed = false;
    if (stream == NULL) {
        return AUDIO_NATIVE_STALE;
    }
    *writable = !stream->queued && !stream->active && !stream->closing;
    *closed = stream->closing ||
        stream->terminal == AUDIO_NATIVE_TERMINAL_CANCELED ||
        stream->terminal == AUDIO_NATIVE_TERMINAL_ERROR;
    return AUDIO_NATIVE_OK;
}

bool audio_native_service(void)
{
    uint64_t now;

    if (!audio_native.initialized) {
        return true;
    }
    if (cpu_interrupts_enabled()) {
        return false;
    }
    now = clock_monotonic_ns();
    if (audio_native.stream_running) {
        const uint8_t status = mmio_read8(audio_native.controller.registers,
            audio_native.controller.output_stream_base + HDA_SDSTS);
        const enum audio_stream_event event = classify_stream_status(status);

        if (event == AUDIO_STREAM_EVENT_COMPLETION) {
            bool stopped;
            bool reset;

            mmio_write8(audio_native.controller.registers,
                audio_native.controller.output_stream_base + HDA_SDSTS,
                HDA_SDSTS_BUFFER_COMPLETE);
            stopped = stop_output_stream(&audio_native.controller,
                &audio_native.proof);
            reset = reset_output_stream(&audio_native.controller,
                &audio_native.proof);
            if (!stopped || !reset) {
                native_finish_active(AUDIO_NATIVE_TERMINAL_ERROR);
            } else {
                native_finish_active(AUDIO_NATIVE_TERMINAL_COMPLETE);
            }
        } else if (event == AUDIO_STREAM_EVENT_UNDERRUN &&
            audio_native.underrun_recoveries <
                AUDIO_MAX_UNDERRUN_RECOVERIES) {
            mmio_write8(audio_native.controller.registers,
                audio_native.controller.output_stream_base + HDA_SDSTS,
                HDA_SDSTS_ACKNOWLEDGE);
            ++audio_native.underrun_recoveries;
            if (!reset_output_stream(&audio_native.controller,
                    &audio_native.proof) ||
                !start_output_stream(&audio_native.controller,
                    &audio_native.proof)) {
                native_finish_active(AUDIO_NATIVE_TERMINAL_ERROR);
            }
        } else if (event == AUDIO_STREAM_EVENT_FATAL ||
            event == AUDIO_STREAM_EVENT_UNDERRUN ||
            now >= audio_native.playback_deadline) {
            mmio_write8(audio_native.controller.registers,
                audio_native.controller.output_stream_base + HDA_SDSTS,
                HDA_SDSTS_ACKNOWLEDGE);
            (void)stop_output_stream(&audio_native.controller,
                &audio_native.proof);
            (void)reset_output_stream(&audio_native.controller,
                &audio_native.proof);
            native_finish_active(AUDIO_NATIVE_TERMINAL_ERROR);
        }
    }
    if (!audio_native.stream_running) {
        const size_t queued = native_queued_streams();
        const size_t open = native_open_streams();

        if (queued != 0U && audio_native.coalesce_service_grace &&
            queued < open) {
            /*
             * Every native syscall returns through this service loop.  Give
             * the owner one guaranteed scheduler return after the first of
             * two submissions, even if a paused host advanced the wall-time
             * window while the syscall was in flight.
             */
            audio_native.coalesce_service_grace = false;
            audio_native.coalesce_deadline = now <=
                    UINT64_MAX - AUDIO_NATIVE_COALESCE_NS ?
                now + AUDIO_NATIVE_COALESCE_NS : now;
        } else if (native_mix_ready(queued, open, now,
                audio_native.coalesce_deadline,
                audio_native.coalesce_service_grace)) {
            if (!native_start_mix()) {
                native_fail_queued();
            }
        }
    }
    if (native_open_streams() == 0U && !audio_native.stream_running &&
        native_queued_streams() == 0U) {
        return native_release_runtime();
    }
    return true;
}

bool audio_native_next_deadline(uint64_t *deadline_ns)
{
    uint64_t now;

    if (deadline_ns == NULL || !audio_native.initialized) {
        return false;
    }
    now = clock_monotonic_ns();
    if (audio_native.stream_running) {
        const uint64_t service = now <= UINT64_MAX - AUDIO_NATIVE_SERVICE_NS ?
            now + AUDIO_NATIVE_SERVICE_NS : now;

        *deadline_ns = service < audio_native.playback_deadline ?
            service : audio_native.playback_deadline;
        return true;
    }
    if (native_queued_streams() != 0U) {
        *deadline_ns = audio_native.coalesce_deadline;
        return true;
    }
    return false;
}

void audio_native_process_terminated(uint64_t owner_generation)
{
    if (!audio_native.initialized || owner_generation == 0U ||
        audio_native.owner_generation != owner_generation) {
        return;
    }
    if (audio_native.stream_running) {
        (void)stop_output_stream(&audio_native.controller,
            &audio_native.proof);
        (void)reset_output_stream(&audio_native.controller,
            &audio_native.proof);
        audio_native.stream_running = false;
    }
    for (size_t index = 0U; index < AUDIO_NATIVE_STREAMS; ++index) {
        zero_bytes(&audio_native.streams[index],
            sizeof(audio_native.streams[index]));
    }
    audio_native.active_mask = 0U;
    (void)native_release_runtime();
}

bool audio_native_resources_released(void)
{
    return !audio_native.initialized && audio_native_cleanup_clean &&
        !audio_active;
}

bool audio_native_self_test(size_t *completed_tests)
{
    int16_t first[4] = {INT16_C(20000), -INT16_C(20000),
        INT16_C(1000), -INT16_C(1000)};
    int16_t second[4] = {INT16_C(20000), -INT16_C(20000),
        -INT16_C(1000), INT16_C(1000)};
    int16_t mixed[4] = {0, 0, 0, 0};
    struct hda_bdl_entry descriptors[AUDIO_PCM_BDL_ENTRIES];
    uint64_t first_token;
    uint64_t second_token;
    size_t completed = 0U;

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;
    mix_two_streams(mixed, first, AUDIO_NATIVE_VOLUME_UNITY,
        AUDIO_NATIVE_VOLUME_UNITY, second, AUDIO_NATIVE_VOLUME_UNITY,
        AUDIO_NATIVE_VOLUME_UNITY, 2U);
    if (mixed[0] != INT16_MAX || mixed[1] != INT16_MIN ||
        mixed[2] != 0 || mixed[3] != 0) {
        return false;
    }
    ++completed;
    mix_two_streams(mixed, first, AUDIO_NATIVE_VOLUME_UNITY / 2U,
        AUDIO_NATIVE_VOLUME_UNITY / 2U, NULL, 0U, 0U, 2U);
    if (mixed[0] != INT16_C(10000) || mixed[1] != -INT16_C(10000) ||
        mixed[2] != INT16_C(500) || mixed[3] != -INT16_C(500)) {
        return false;
    }
    ++completed;
    mix_two_streams(mixed, first, 0U, 0U, second, 0U, 0U, 2U);
    if (mixed[0] != 0 || mixed[1] != 0 || mixed[2] != 0 ||
        mixed[3] != 0) {
        return false;
    }
    ++completed;
    zero_bytes(descriptors, sizeof(descriptors));
    if (!fill_buffer_list(descriptors, UINT64_C(0x00200000), false) ||
        descriptors[0].flags != HDA_BDL_INTERRUPT_ON_COMPLETION ||
        descriptors[1].flags != 0U) {
        return false;
    }
    ++completed;
    if (AUDIO_NATIVE_STREAMS != 2U || AUDIO_PCM_BYTES != 4096U ||
        AUDIO_NATIVE_VOLUME_UNITY != 32768U) {
        return false;
    }
    ++completed;
    first_token = native_allocate_token();
    second_token = native_allocate_token();
    if (first_token == 0U || second_token == 0U ||
        first_token == second_token) {
        return false;
    }
    ++completed;
    if (native_mix_ready(1U, 2U, UINT64_C(100), UINT64_C(99), true) ||
        !native_mix_ready(1U, 2U, UINT64_C(100), UINT64_C(99), false)) {
        return false;
    }
    ++completed;
    if (!native_mix_ready(2U, 2U, 0U, UINT64_MAX, true) ||
        native_mix_ready(0U, 2U, UINT64_MAX, 0U, false)) {
        return false;
    }
    ++completed;
    *completed_tests = completed;
    return completed == AUDIO_NATIVE_CONTROLLED_CONTROLS;
}

struct audio_proof_result audio_get_proof_result(void)
{
    return installed_result;
}

bool audio_resources_released(void)
{
    return !audio_active;
}

const char *audio_status_string(enum audio_status status)
{
    static const char *const messages[AUDIO_STATUS_COUNT] = {
        "ok",
        "null audio argument",
        "the bounded audio proof is already active",
        "audio prerequisites are incomplete",
        "no HD Audio controller is present",
        "audio controller claim failed",
        "audio register window mapping failed",
        "the mapped audio register window is too small",
        "the controller does not report a known interface version",
        "an audio controller reset did not complete inside its bound",
        "audio ring DMA allocation or ownership failed",
        "bus mastering was granted before the rings were prepared",
        "enabling bus mastering for the audio rings failed",
        "the controller supports no usable ring size",
        "the command ring read pointer refused to reset",
        "an audio ring DMA engine refused to start",
        "the controller reports no codec on the link",
        "a codec did not answer inside its bound",
        "a response did not come from the codec that was asked",
        "a codec did not identify itself as its specification requires",
        "no bounded output converter and pin route was found",
        "the output route does not support 48 kHz signed 16-bit stereo PCM",
        "the HD Audio output stream descriptor refused to reset",
        "the HD Audio output stream refused to start",
        "the HD Audio output stream reported a descriptor or repeated FIFO error",
        "the HD Audio output stream produced no bounded position/status evidence",
        "audio teardown leaked or failed",
        "audio pre/post resource census differs"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        AUDIO_STATUS_COUNT, "audio status messages are out of sync");
    if (status < AUDIO_STATUS_OK || status >= AUDIO_STATUS_COUNT) {
        return "unknown audio status";
    }
    return messages[status];
}
