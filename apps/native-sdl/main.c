/* SPDX-License-Identifier: GPL-3.0-only */
#include <SDL.h>

#include <phipia/runtime.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define INPUT_DEADLINE_MS UINT32_C(15000)
#define AUDIO_FRAMES UINT32_C(4096)

static int16_t pcm[AUDIO_FRAMES * 2U];

static int persistent_run(char **preference_path)
{
    uint32_t run = 0U;
    char state_path[64];
    SDL_RWops *state;

    *preference_path = SDL_GetPrefPath("Phipia", "SDL proof");
    if (*preference_path == NULL ||
        SDL_snprintf(state_path, sizeof(state_path), "%sSTATE.BIN",
            *preference_path) <= 0) {
        return -1;
    }
    state = SDL_RWFromFile(state_path, "rb");
    if (state != NULL) {
        if (SDL_RWread(state, &run, sizeof(run), 1U) != 1U) {
            run = 0U;
        }
        (void)SDL_RWclose(state);
    }
    if (run == UINT32_MAX) {
        return -1;
    }
    ++run;
    state = SDL_RWFromFile(state_path, "wb");
    if (state == NULL) {
        return -1;
    }
    const size_t written = SDL_RWwrite(state, &run, sizeof(run), 1U);
    const int close_status = SDL_RWclose(state);

    if (written != 1U || close_status != 0 ||
        phipia_volume_sync(PHIPIA_VOLUME_DATA) != 0) {
        return -1;
    }
    return (int)run;
}

static int draw_window(SDL_Window *window, SDL_Renderer **renderer)
{
    SDL_Rect tiles[12];

    *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (*renderer == NULL ||
        SDL_SetRenderDrawColor(*renderer, 16U, 24U, 48U, 255U) != 0 ||
        SDL_RenderClear(*renderer) != 0) {
        return -1;
    }
    for (int index = 0; index < 12; ++index) {
        tiles[index] = (SDL_Rect){
            18 + (index % 4) * 82,
            18 + (index / 4) * 68,
            64,
            48
        };
        if (SDL_SetRenderDrawColor(*renderer,
                (Uint8)(48 + index * 13),
                (Uint8)(190 - index * 9),
                (Uint8)(92 + index * 7), 255U) != 0 ||
            SDL_RenderFillRect(*renderer, &tiles[index]) != 0) {
            return -1;
        }
    }
    SDL_RenderPresent(*renderer);

    SDL_Surface *surface = SDL_GetWindowSurface(window);
    SDL_Rect damage = { 145, 96, 70, 48 };
    if (surface == NULL ||
        SDL_FillRect(surface, &damage,
            SDL_MapRGB(surface->format, 236U, 72U, 92U)) != 0 ||
        SDL_UpdateWindowSurfaceRects(window, &damage, 1) != 0) {
        return -1;
    }
    return 0;
}

static int play_audio(SDL_AudioDeviceID *device)
{
    SDL_AudioSpec wanted;
    SDL_AudioSpec obtained;

    for (size_t frame = 0U; frame < AUDIO_FRAMES; ++frame) {
        const uint32_t value = UINT32_C(1000) +
            ((uint32_t)frame * UINT32_C(73)) % UINT32_C(12000);
        const int16_t sample = (int16_t)value;
        pcm[frame * 2U] = sample;
        pcm[frame * 2U + 1U] = (int16_t)-sample;
    }
    SDL_zero(wanted);
    wanted.freq = 48000;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 2U;
    wanted.samples = 1024U;
    *device = SDL_OpenAudioDevice(NULL, 0, &wanted, &obtained, 0);
    if (*device == 0U || obtained.freq != wanted.freq ||
        obtained.format != wanted.format ||
        obtained.channels != wanted.channels ||
        obtained.samples != wanted.samples ||
        SDL_QueueAudio(*device, pcm, sizeof(pcm)) != 0) {
        return -1;
    }
    SDL_PauseAudioDevice(*device, 0);
    return 0;
}

static int await_input(void)
{
    const Uint64 deadline = SDL_GetTicks64() + INPUT_DEADLINE_MS;
    SDL_bool key = SDL_FALSE;
    SDL_bool pointer = SDL_FALSE;

    while (SDL_GetTicks64() < deadline && (!key || !pointer)) {
        SDL_Event event;

        if (SDL_WaitEventTimeout(&event, 250) == 0) {
            continue;
        }
        if (event.type == SDL_KEYDOWN) {
            key = SDL_TRUE;
        } else if (event.type == SDL_MOUSEMOTION ||
                   event.type == SDL_MOUSEBUTTONDOWN) {
            pointer = SDL_TRUE;
        } else if (event.type == SDL_QUIT) {
            return -1;
        }
    }
    return key && pointer ? 0 :
        SDL_SetError("input deadline expired: key=%d pointer=%d",
            (int)key, (int)pointer);
}

static int wait_for_audio(SDL_AudioDeviceID device)
{
    const Uint64 deadline = SDL_GetTicks64() + UINT64_C(5000);

    while (SDL_GetQueuedAudioSize(device) != 0U &&
           SDL_GetTicks64() < deadline) {
        SDL_Delay(10U);
    }
    return SDL_GetQueuedAudioSize(device) == 0U ? 0 :
        SDL_SetError("audio queue did not drain before its deadline");
}

int main(int argc, char **argv, char **environment)
{
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_AudioDeviceID audio = 0U;
    char *preference_path = NULL;
    int run;
    int result = 1;

    (void)argc;
    (void)argv;
    (void)environment;
    (void)setvbuf(stdout, NULL, _IONBF, 0U);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0 ||
        strcmp(SDL_GetPlatform(), "Phipia") != 0 ||
        strcmp(SDL_GetCurrentVideoDriver(), "phipia") != 0) {
        printf("PHIPIA SDL FAIL init=%s\n", SDL_GetError());
        goto cleanup;
    }
    run = persistent_run(&preference_path);
    window = SDL_CreateWindow("Phipia SDL 2 proof", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, 360, 240, SDL_WINDOW_SHOWN);
    if (run <= 0 || window == NULL || draw_window(window, &renderer) != 0 ||
        play_audio(&audio) != 0) {
        printf("PHIPIA SDL FAIL setup=%s\n", SDL_GetError());
        goto cleanup;
    }
    printf("PHIPIA SDL READY run=%d video=%s audio=%s pref=%s\n", run,
        SDL_GetCurrentVideoDriver(), SDL_GetCurrentAudioDriver(),
        preference_path);
    if ((run == 1 && await_input() != 0) || wait_for_audio(audio) != 0) {
        printf("PHIPIA SDL FAIL evidence=%s\n", SDL_GetError());
        goto cleanup;
    }
    printf("PHIPIA SDL PASS run=%d present=partial input=%s audio=non-silent persistent=yes\n",
        run, run == 1 ? "key-pointer" : "prior-run");
    result = 0;

cleanup:
    if (audio != 0U) {
        SDL_CloseAudioDevice(audio);
    }
    if (renderer != NULL) {
        SDL_DestroyRenderer(renderer);
    }
    if (window != NULL) {
        SDL_DestroyWindow(window);
    }
    SDL_free(preference_path);
    SDL_Quit();
    return result;
}
