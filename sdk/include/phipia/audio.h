/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_USER_AUDIO_H
#define PHIPIA_USER_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#include <phipia/abi.h>

long phipia_audio_open(void);
long phipia_audio_submit(phipia_handle_t output, const int16_t *samples,
    size_t byte_length);
long phipia_audio_set_volume(phipia_handle_t output, uint32_t left_q15,
    uint32_t right_q15);
long phipia_audio_drain(phipia_handle_t output, uint64_t deadline_ns);
long phipia_audio_cancel(phipia_handle_t output);
long phipia_audio_close(phipia_handle_t output);

#endif
