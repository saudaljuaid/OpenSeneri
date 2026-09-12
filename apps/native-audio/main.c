/* SPDX-License-Identifier: GPL-3.0-only */
#include <phipia/audio.h>
#include <phipia/event.h>
#include <phipia/runtime.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define OPERATION_NS UINT64_C(3000000000)

static int16_t first_pcm[PHIPIA_AUDIO_CHUNK_FRAMES * PHIPIA_AUDIO_CHANNELS];
static int16_t second_pcm[PHIPIA_AUDIO_CHUNK_FRAMES * PHIPIA_AUDIO_CHANNELS];
static int16_t canceled_pcm[PHIPIA_AUDIO_CHUNK_FRAMES * PHIPIA_AUDIO_CHANNELS];

static uint64_t deadline(void)
{
    return phipia_monotonic_ns() + OPERATION_NS;
}

static void fill_pcm(void)
{
    for (size_t frame = 0U; frame < PHIPIA_AUDIO_CHUNK_FRAMES; ++frame) {
        const int16_t first = (frame / 32U) % 2U == 0U ?
            INT16_C(8192) : -INT16_C(8192);
        const int16_t second = (frame / 64U) % 2U == 0U ?
            INT16_C(4096) : -INT16_C(4096);
        const size_t sample = frame * PHIPIA_AUDIO_CHANNELS;

        first_pcm[sample] = first;
        first_pcm[sample + 1U] = first;
        second_pcm[sample] = second;
        second_pcm[sample + 1U] = second;
        canceled_pcm[sample] = INT16_C(30000);
        canceled_pcm[sample + 1U] = -INT16_C(30000);
    }
}

static int wait_writable(phipia_handle_t first, phipia_handle_t second)
{
    struct phipia_wait_item items[2] = {
        {first, PHIPIA_WAIT_WRITABLE, 0U},
        {second, PHIPIA_WAIT_WRITABLE, 0U}
    };
    const long ready = phipia_wait(items, 2U, deadline());

    return ready == 2 && items[0].ready == PHIPIA_WAIT_WRITABLE &&
        items[1].ready == PHIPIA_WAIT_WRITABLE ? 0 : -1;
}

static int run_refusal(void)
{
    if (phipia_audio_open() != -PHIPIA_EACCES) {
        return 10;
    }
    puts("PHIPIA AUDIO REFUSAL PASS capability=EACCES");
    return 0;
}

static int run_proof(void)
{
    struct phipia_wait_item canceled;
    long first_opened;
    long second_opened;
    long leaked;
    phipia_handle_t first;
    phipia_handle_t second;

    fill_pcm();
    first_opened = phipia_audio_open();
    if (first_opened < 0) {
        return 20;
    }
    first = (phipia_handle_t)first_opened;
    second_opened = phipia_audio_open();
    if (second_opened < 0) {
        (void)phipia_audio_close(first);
        return 21;
    }
    second = (phipia_handle_t)second_opened;
    if (phipia_audio_open() != -PHIPIA_EBUSY ||
        phipia_audio_submit(first, first_pcm,
            PHIPIA_AUDIO_CHUNK_BYTES - PHIPIA_AUDIO_FRAME_BYTES) !=
                -PHIPIA_EINVAL ||
        wait_writable(first, second) != 0) {
        (void)phipia_audio_close(second);
        (void)phipia_audio_close(first);
        return 22;
    }
    puts("PHIPIA AUDIO PHASE open-limit-readiness PASS");

    if (phipia_audio_set_volume(first, PHIPIA_AUDIO_VOLUME_UNITY,
            PHIPIA_AUDIO_VOLUME_UNITY / 2U) != 0 ||
        phipia_audio_set_volume(second, PHIPIA_AUDIO_VOLUME_UNITY / 2U,
            PHIPIA_AUDIO_VOLUME_UNITY) != 0 ||
        phipia_audio_submit(first, first_pcm, sizeof(first_pcm)) !=
            (long)sizeof(first_pcm) ||
        phipia_audio_submit(second, second_pcm, sizeof(second_pcm)) !=
            (long)sizeof(second_pcm) ||
        phipia_audio_drain(first, deadline()) != 0 ||
        phipia_audio_drain(second, deadline()) != 0 ||
        wait_writable(first, second) != 0) {
        (void)phipia_audio_close(second);
        (void)phipia_audio_close(first);
        return 23;
    }
    puts("PHIPIA AUDIO PHASE two-stream-mix-drain PASS");

    if (phipia_audio_submit(first, canceled_pcm, sizeof(canceled_pcm)) !=
            (long)sizeof(canceled_pcm) || phipia_audio_cancel(first) != 0 ||
        phipia_audio_drain(first, deadline()) != -PHIPIA_ECANCELED) {
        (void)phipia_audio_close(second);
        (void)phipia_audio_close(first);
        return 24;
    }
    canceled = (struct phipia_wait_item){
        first, PHIPIA_WAIT_WRITABLE | PHIPIA_WAIT_CLOSED, 0U
    };
    if (phipia_wait(&canceled, 1U, deadline()) != 1 ||
        canceled.ready != (PHIPIA_WAIT_WRITABLE | PHIPIA_WAIT_CLOSED)) {
        (void)phipia_audio_close(second);
        (void)phipia_audio_close(first);
        return 25;
    }
    puts("PHIPIA AUDIO PHASE cancel-terminal-readiness PASS");

    if (phipia_audio_close(second) != 0 || phipia_audio_close(first) != 0 ||
        phipia_audio_close(first) != -PHIPIA_ESTALE) {
        return 26;
    }
    leaked = phipia_audio_open();
    if (leaked < 0) {
        return 27;
    }
    puts("PHIPIA AUDIO PASS frames=1024 format=48000/S16LE/2 close=stale teardown=process");
    /* The last typed handle intentionally exercises process-exit cleanup. */
    return 0;
}

int main(int argc, char **argv, char **environment)
{
    (void)environment;
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && strcmp(argv[1], "refusal") == 0) {
        return run_refusal();
    }
    if (argc == 2 && strcmp(argv[1], "proof") == 0) {
        return run_proof();
    }
    return 2;
}
