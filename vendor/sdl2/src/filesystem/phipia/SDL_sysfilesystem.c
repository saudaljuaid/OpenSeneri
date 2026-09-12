/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  Phipia filesystem backend addition for SDL 2.32.10. This file is
  distributed under the same zlib license as SDL.
*/

#include "../../SDL_internal.h"

#ifdef SDL_FILESYSTEM_PHIPIA

#include "SDL_error.h"
#include "SDL_filesystem.h"

#include <phipia/runtime.h>

static Uint32 PHIPIA_HashText(Uint32 hash, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;

    while (cursor != NULL && *cursor != 0U) {
        hash ^= (Uint32)*cursor++;
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static int PHIPIA_EnsureDirectory(const char *path)
{
    const long result = phipia_path_mkdir(PHIPIA_VOLUME_DATA, path);

    return result == 0 || result == -PHIPIA_EEXIST ? 0 :
        SDL_SetError("Phipia could not create preference directory: %ld",
            result);
}

char *SDL_GetBasePath(void)
{
    return SDL_strdup("System:");
}

char *SDL_GetPrefPath(const char *organization, const char *application)
{
    static const char hexadecimal[] = "0123456789ABCDEF";
    const char *org = organization != NULL ? organization : "";
    Uint32 hash = PHIPIA_HashText(UINT32_C(2166136261), org);
    char native_path[13] = "SDL/00000000";
    char result_path[19] = "Data:SDL/00000000/";
    unsigned index;
    char *result;

    if (application == NULL || *application == '\0') {
        SDL_InvalidParamError("application");
        return NULL;
    }
    hash = PHIPIA_HashText(hash, "/");
    hash = PHIPIA_HashText(hash, application);
    for (index = 0U; index < 8U; ++index) {
        const unsigned shift = (7U - index) * 4U;
        const char digit = hexadecimal[(hash >> shift) & 15U];
        native_path[4U + index] = digit;
        result_path[9U + index] = digit;
    }
    if (PHIPIA_EnsureDirectory("SDL") != 0 ||
        PHIPIA_EnsureDirectory(native_path) != 0) {
        return NULL;
    }
    result = SDL_strdup(result_path);
    if (result == NULL) {
        SDL_OutOfMemory();
    }
    return result;
}

#endif /* SDL_FILESYSTEM_PHIPIA */
