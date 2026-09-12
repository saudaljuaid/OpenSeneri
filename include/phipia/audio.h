/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_AUDIO_H
#define PHIPIA_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The first Phipia driver that lets a device write into kernel memory on its
 * own initiative for a reason other than storage or networking.
 *
 * High Definition Audio does not have registers a driver can ask a codec
 * through. It has two rings in memory: the controller reads commands out of
 * one and writes the codecs' answers into the other, both by bus-mastering
 * DMA. Phipia has no IOMMU, so a device that can write one ring can write
 * anything, and the whole of this driver's shape follows from that: both
 * rings, the BDL and the PCM buffer are typed DMA allocations; bus mastering
 * is enabled only with those exact allocations declared; and every engine is
 * stopped and reset before bus mastering is withdrawn and memory reclaimed.
 *
 * What it proves is a real conversation followed by a real, kernel-owned
 * output stream: codecs identify themselves, a converter-to-pin route is
 * discovered and configured, and the controller must advance LPIB and report
 * a BDL completion while consuming the fixed PCM profile below.
 */

/* HD Audio 1.0a section 3.3.9: fifteen codec addresses on the link. */
#define AUDIO_MAX_CODECS 15U

/*
 * Ring sizes. The specification allows 2, 16 or 256 entries and reports which
 * of them a controller supports; the driver takes the largest it offers, which
 * is what every real driver does and what makes the size-capability field
 * worth reading at all.
 */
#define AUDIO_CORB_MAX_ENTRIES 256U
#define AUDIO_RIRB_MAX_ENTRIES 256U
#define AUDIO_CORB_ENTRY_BYTES 4U
#define AUDIO_RIRB_ENTRY_BYTES 8U

/*
 * How long any one device handshake may take. The specification gives codec
 * enumeration 521 microseconds after the controller leaves reset and a command
 * a bounded response time; a second is far beyond both and turns a device that
 * never answers into a named status rather than a hang.
 */
#define AUDIO_TIMEOUT_NS UINT64_C(1000000000)

/* The single honest PCM profile supported by the QEMU hda-duplex proof. */
#define AUDIO_PCM_SAMPLE_RATE 48000U
#define AUDIO_PCM_CHANNELS 2U
#define AUDIO_PCM_BITS_PER_SAMPLE 16U
#define AUDIO_PCM_FRAME_BYTES 4U
#define AUDIO_PCM_FRAMES 1024U
#define AUDIO_PCM_BYTES (AUDIO_PCM_FRAMES * AUDIO_PCM_FRAME_BYTES)
#define AUDIO_PCM_PERIOD_BYTES AUDIO_PCM_BYTES
#define AUDIO_PCM_BDL_ENTRIES 2U
#define AUDIO_PCM_DMA_BYTES (AUDIO_PCM_PERIOD_BYTES * AUDIO_PCM_BDL_ENTRIES)
#define AUDIO_PCM_STREAM_TAG 1U
#define AUDIO_NATIVE_STREAMS 2U
#define AUDIO_NATIVE_VOLUME_UNITY 32768U
#define AUDIO_NATIVE_CONTROLLED_CONTROLS 8U

/* Controls the pure foundation stage exercises before any device is touched. */
#define AUDIO_CONTROLLED_CONTROLS 16U

enum audio_status {
    AUDIO_STATUS_OK = 0,
    AUDIO_STATUS_NULL_ARGUMENT,
    AUDIO_STATUS_BUSY,
    AUDIO_STATUS_PREREQUISITE,
    AUDIO_STATUS_ABSENT,
    AUDIO_STATUS_CLAIM_FAILURE,
    AUDIO_STATUS_MAPPING_FAILURE,
    AUDIO_STATUS_REGISTER_WINDOW,
    AUDIO_STATUS_VERSION,
    AUDIO_STATUS_RESET_TIMEOUT,
    AUDIO_STATUS_DMA_FAILURE,
    AUDIO_STATUS_BUS_MASTER_GUARD_FAILURE,
    AUDIO_STATUS_BUS_MASTER_FAILURE,
    AUDIO_STATUS_RING_SIZE,
    AUDIO_STATUS_RING_RESET,
    AUDIO_STATUS_RING_START,
    AUDIO_STATUS_CODEC_ABSENT,
    AUDIO_STATUS_VERB_TIMEOUT,
    AUDIO_STATUS_RESPONSE_AUTHENTICATION,
    AUDIO_STATUS_IDENTITY,
    AUDIO_STATUS_WIDGET_TOPOLOGY,
    AUDIO_STATUS_PCM_FORMAT,
    AUDIO_STATUS_STREAM_RESET,
    AUDIO_STATUS_STREAM_START,
    AUDIO_STATUS_STREAM_ERROR,
    AUDIO_STATUS_STREAM_TIMEOUT,
    AUDIO_STATUS_TEARDOWN,
    AUDIO_STATUS_RESOURCE_CENSUS,
    AUDIO_STATUS_COUNT
};

struct audio_codec {
    uint8_t address;
    uint8_t first_group_node;
    uint8_t group_node_count;
    uint8_t function_group_type;
    uint32_t vendor_device;
    uint32_t revision;
    bool audio_function_group;
    bool identified;
};

struct audio_proof_result {
    uint32_t version;
    uint32_t capability;
    uint32_t output_streams;
    uint32_t input_streams;
    uint32_t bidirectional_streams;
    uint32_t serial_data_out_signals;
    uint32_t corb_entries;
    uint32_t rirb_entries;
    uint32_t codecs_present;
    uint32_t codecs_identified;
    uint32_t verbs_issued;
    uint32_t responses_received;
    uint32_t controls;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t bits_per_sample;
    uint32_t pcm_frames;
    uint32_t pcm_bytes;
    uint32_t bdl_entries;
    uint32_t stream_format;
    uint32_t stream_descriptor_index;
    uint32_t initial_link_position;
    uint32_t final_link_position;
    uint32_t service_iterations;
    uint32_t period_completions;
    uint32_t underrun_recoveries;
    uint64_t pcm_hash;
    uint8_t playback_codec;
    uint8_t playback_function_group;
    uint8_t playback_converter;
    uint8_t playback_pin;
    uint8_t playback_stream_tag;
    bool controller_reset;
    bool rings_running;
    bool audio_function_group_found;
    bool device_wrote_response_ring;
    bool output_route_found;
    bool pcm_profile_supported;
    bool pcm_device_owned_during_run;
    bool bdl_device_owned_during_run;
    bool stream_reset;
    bool stream_started;
    bool link_position_advanced;
    bool stream_status_observed;
    bool stream_stopped_before_reset;
    bool bus_master_withdrawn_before_release;
    bool teardown_complete;
    bool resource_census_equal;
    struct audio_codec codecs[AUDIO_MAX_CODECS];
};

enum audio_native_status {
    AUDIO_NATIVE_OK = 0,
    AUDIO_NATIVE_NULL_ARGUMENT,
    AUDIO_NATIVE_ABSENT,
    AUDIO_NATIVE_BUSY,
    AUDIO_NATIVE_STALE,
    AUDIO_NATIVE_INVALID,
    AUDIO_NATIVE_CANCELED,
    AUDIO_NATIVE_IO,
    AUDIO_NATIVE_STATUS_COUNT
};

enum audio_native_drain_state {
    AUDIO_NATIVE_DRAIN_COMPLETE = 0,
    AUDIO_NATIVE_DRAIN_PENDING,
    AUDIO_NATIVE_DRAIN_CANCELED,
    AUDIO_NATIVE_DRAIN_ERROR,
    AUDIO_NATIVE_DRAIN_STALE
};

bool audio_foundation_self_test(size_t *completed_tests);
enum audio_status audio_prove(struct audio_proof_result *result);
struct audio_proof_result audio_get_proof_result(void);
bool audio_resources_released(void);
const char *audio_status_string(enum audio_status status);
enum audio_native_status audio_native_open(
    uint64_t owner_generation,
    uint64_t *stream_token
);
enum audio_native_status audio_native_submit(
    uint64_t owner_generation,
    uint64_t stream_token,
    const int16_t *samples,
    size_t byte_length
);
enum audio_native_status audio_native_set_volume(
    uint64_t owner_generation,
    uint64_t stream_token,
    uint32_t left_q15,
    uint32_t right_q15
);
enum audio_native_status audio_native_cancel(
    uint64_t owner_generation,
    uint64_t stream_token
);
enum audio_native_status audio_native_close(
    uint64_t owner_generation,
    uint64_t stream_token
);
enum audio_native_drain_state audio_native_drain(
    uint64_t owner_generation,
    uint64_t stream_token
);
enum audio_native_status audio_native_poll(
    uint64_t owner_generation,
    uint64_t stream_token,
    bool *writable,
    bool *closed
);
bool audio_native_service(void);
bool audio_native_next_deadline(uint64_t *deadline_ns);
void audio_native_process_terminated(uint64_t owner_generation);
bool audio_native_resources_released(void);
bool audio_native_self_test(size_t *completed_tests);

#endif
