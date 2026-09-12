/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  Phipia audio backend addition for SDL 2.32.10. This file is distributed
  under the same zlib license as SDL.
*/

#include "../../SDL_internal.h"

#ifdef SDL_AUDIO_DRIVER_PHIPIA

#include "SDL_audio.h"
#include "../SDL_audio_c.h"
#include "../SDL_sysaudio.h"

#include <phipia/audio.h>
#include <phipia/event.h>
#include <phipia/runtime.h>

#define _THIS SDL_AudioDevice *_this

struct SDL_PrivateAudioData
{
    phipia_handle_t output;
    Uint8 buffer[PHIPIA_AUDIO_CHUNK_BYTES];
    SDL_bool failed;
};

static uint64_t PHIPIAAUDIO_Deadline(void)
{
    const uint64_t now = phipia_monotonic_ns();
    const uint64_t allowance = UINT64_C(1000000000);
    return now > UINT64_MAX - allowance ? UINT64_MAX : now + allowance;
}

static void PHIPIAAUDIO_WaitDevice(_THIS)
{
    struct phipia_wait_item item;
    long result;

    if (_this->hidden->failed == SDL_TRUE) {
        return;
    }
    item.handle = _this->hidden->output;
    item.interests = PHIPIA_WAIT_WRITABLE | PHIPIA_WAIT_CLOSED;
    item.ready = 0U;
    result = phipia_wait(&item, 1U, PHIPIAAUDIO_Deadline());
    if (result != 1 || item.ready != PHIPIA_WAIT_WRITABLE) {
        _this->hidden->failed = SDL_TRUE;
        SDL_OpenedAudioDeviceDisconnected(_this);
    }
}

static void PHIPIAAUDIO_PlayDevice(_THIS)
{
    long result;

    if (_this->hidden->failed == SDL_TRUE) {
        return;
    }
    result = phipia_audio_submit(_this->hidden->output,
        (const int16_t *)(const void *)_this->hidden->buffer,
        sizeof(_this->hidden->buffer));
    if (result >= 0) {
        result = phipia_audio_drain(_this->hidden->output,
            PHIPIAAUDIO_Deadline());
    }
    if (result < 0) {
        _this->hidden->failed = SDL_TRUE;
        SDL_OpenedAudioDeviceDisconnected(_this);
    }
}

static Uint8 *PHIPIAAUDIO_GetDeviceBuf(_THIS)
{
    return _this->hidden->buffer;
}

static void PHIPIAAUDIO_CloseDevice(_THIS)
{
    if (_this->hidden != NULL) {
        (void)phipia_audio_cancel(_this->hidden->output);
        (void)phipia_audio_close(_this->hidden->output);
        SDL_free(_this->hidden);
        _this->hidden = NULL;
    }
}

static int PHIPIAAUDIO_OpenDevice(_THIS, const char *devname)
{
    struct SDL_PrivateAudioData *hidden;
    long result;
    (void)devname;

    if (_this->iscapture) {
        return SDL_SetError("Phipia SDL audio capture is unsupported");
    }
    hidden = (struct SDL_PrivateAudioData *)SDL_calloc(1, sizeof(*hidden));
    if (hidden == NULL) {
        return SDL_OutOfMemory();
    }
    result = phipia_audio_open();
    if (result < 0) {
        SDL_free(hidden);
        return SDL_SetError("Phipia audio open failed: %ld", result);
    }
    hidden->output = (phipia_handle_t)result;
    _this->hidden = hidden;
    _this->spec.freq = (int)PHIPIA_AUDIO_SAMPLE_RATE;
    _this->spec.format = AUDIO_S16SYS;
    _this->spec.channels = (Uint8)PHIPIA_AUDIO_CHANNELS;
    _this->spec.samples = (Uint16)PHIPIA_AUDIO_CHUNK_FRAMES;
    SDL_CalculateAudioSpec(&_this->spec);
    if (_this->spec.size != PHIPIA_AUDIO_CHUNK_BYTES) {
        PHIPIAAUDIO_CloseDevice(_this);
        return SDL_SetError("Phipia SDL audio format calculation changed");
    }
    return 0;
}

static SDL_bool PHIPIAAUDIO_Init(SDL_AudioDriverImpl *impl)
{
    impl->OpenDevice = PHIPIAAUDIO_OpenDevice;
    impl->WaitDevice = PHIPIAAUDIO_WaitDevice;
    impl->PlayDevice = PHIPIAAUDIO_PlayDevice;
    impl->GetDeviceBuf = PHIPIAAUDIO_GetDeviceBuf;
    impl->CloseDevice = PHIPIAAUDIO_CloseDevice;
    impl->OnlyHasDefaultOutputDevice = SDL_TRUE;
    impl->HasCaptureSupport = SDL_FALSE;
    impl->SupportsNonPow2Samples = SDL_FALSE;
    return SDL_TRUE;
}

AudioBootStrap PHIPIAAUDIO_bootstrap = {
    "phipia",
    "Phipia native mixed PCM output",
    PHIPIAAUDIO_Init,
    SDL_FALSE
};

#undef _THIS

#endif /* SDL_AUDIO_DRIVER_PHIPIA */
