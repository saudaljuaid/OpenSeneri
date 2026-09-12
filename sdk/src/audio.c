/* SPDX-License-Identifier: GPL-3.0-only */
#include <phipia/audio.h>
#include <phipia/runtime.h>

long phipia_audio_open(void)
{
    return phipia_syscall0(PHIPIA_SYS_AUDIO_OPEN);
}

long phipia_audio_submit(
    phipia_handle_t output,
    const int16_t *samples,
    size_t byte_length
)
{
    const struct phipia_audio_submit_request request = {
        sizeof(request), PHIPIA_ABI_VERSION, output,
        (uint64_t)(uintptr_t)samples, (uint32_t)byte_length, 0U
    };

    if (samples == NULL || byte_length != PHIPIA_AUDIO_CHUNK_BYTES) {
        return -PHIPIA_EINVAL;
    }
    return phipia_syscall1(PHIPIA_SYS_AUDIO_SUBMIT,
        (uint64_t)(uintptr_t)&request);
}

long phipia_audio_set_volume(
    phipia_handle_t output,
    uint32_t left_q15,
    uint32_t right_q15
)
{
    const struct phipia_audio_volume_request request = {
        sizeof(request), PHIPIA_ABI_VERSION, output, left_q15, right_q15,
        0U, 0U
    };

    if (left_q15 > PHIPIA_AUDIO_VOLUME_MAX ||
        right_q15 > PHIPIA_AUDIO_VOLUME_MAX) {
        return -PHIPIA_EINVAL;
    }
    return phipia_syscall1(PHIPIA_SYS_AUDIO_VOLUME,
        (uint64_t)(uintptr_t)&request);
}

long phipia_audio_drain(phipia_handle_t output, uint64_t deadline_ns)
{
    return phipia_syscall2(PHIPIA_SYS_AUDIO_DRAIN, output, deadline_ns);
}

long phipia_audio_cancel(phipia_handle_t output)
{
    return phipia_syscall1(PHIPIA_SYS_CANCEL, output);
}

long phipia_audio_close(phipia_handle_t output)
{
    return phipia_handle_close(output);
}
