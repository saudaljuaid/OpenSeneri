/* SPDX-License-Identifier: Zlib */
/* Bounded Phipia harness for SDL 2.32.10's testdrawchessboard application. */

#define SDL_MAIN_HANDLED
#define main phipia_upstream_sdl_chess_main
#include "upstream.c"
#undef main

#include <phipia/runtime.h>

#define PROOF_FRAME_COUNT 8U

static int persist_launch(void)
{
    static const char proof[] = "SDL chess release-2.32.10\n";
    char state_path[96];
    char *preference_path = SDL_GetPrefPath("Phipia", "SDL Chess");
    SDL_RWops *state;

    if (preference_path == NULL ||
            SDL_snprintf(state_path, sizeof(state_path), "%sSTATE.TXT",
                preference_path) <= 0) {
        SDL_free(preference_path);
        return -1;
    }
    SDL_free(preference_path);
    state = SDL_RWFromFile(state_path, "wb");
    if (state == NULL) {
        return -1;
    }
    const size_t written = SDL_RWwrite(state, proof, 1U, sizeof(proof) - 1U);
    const int closed = SDL_RWclose(state);

    return written == sizeof(proof) - 1U && closed == 0 &&
        phipia_volume_sync(PHIPIA_VOLUME_DATA) == 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    uint32_t frames = 0U;
    int result = 1;

    (void)argc;
    (void)argv;
    (void)setvbuf(stdout, NULL, _IONBF, 0U);
    SDL_LogSetPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);
    if (SDL_Init(SDL_INIT_VIDEO) != 0 ||
            SDL_strcmp(SDL_GetPlatform(), "Phipia") != 0 ||
            SDL_strcmp(SDL_GetCurrentVideoDriver(), "phipia") != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SDL chess initialization failed: %s", SDL_GetError());
        goto cleanup;
    }
    window = SDL_CreateWindow("SDL Chess Board",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480,
        SDL_WINDOW_RESIZABLE);
    if (window == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SDL chess window creation failed: %s", SDL_GetError());
        goto cleanup;
    }
    surface = SDL_GetWindowSurface(window);
    if (surface == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SDL chess window surface failed: %s", SDL_GetError());
        goto cleanup;
    }
    renderer = SDL_CreateSoftwareRenderer(surface);
    if (renderer == NULL || persist_launch() != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SDL chess renderer or persistence failed: %s", SDL_GetError());
        goto cleanup;
    }
    done = 0;
    while (!done && frames < PROOF_FRAME_COUNT) {
        loop();
        ++frames;
    }
    if (frames != PROOF_FRAME_COUNT) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SDL chess exited before its bounded frame proof");
        goto cleanup;
    }
    printf("PHIPIA SDL CHESS PASS upstream=release-2.32.10 "
        "frames=%u persistent=yes\n", frames);
    result = 0;

cleanup:
    if (renderer != NULL) {
        SDL_DestroyRenderer(renderer);
        renderer = NULL;
    }
    if (window != NULL) {
        SDL_DestroyWindow(window);
        window = NULL;
    }
    SDL_Quit();
    return result;
}
