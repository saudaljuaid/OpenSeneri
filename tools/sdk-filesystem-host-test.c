/* SPDX-License-Identifier: GPL-3.0-only */
/* Exercise the production POSIX wrapper through its native syscall boundary. */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include "../sdk/src/internal.h"

_Thread_local int errno;
static uint64_t expected_mode;
static long syscall_result;
static unsigned calls;
static int invalid_request;

int phipia_runtime_path(const char *input, struct phipia_runtime_path *result)
{
    if (input == NULL) { errno = EFAULT; return -1; }
    *result = (struct phipia_runtime_path){PHIPIA_VOLUME_DATA, input, strlen(input)};
    return 0;
}

int phipia_result(long result)
{
    if (result < 0) { errno = (int)-result; return -1; }
    return (int)result;
}

long phipia_syscall2(uint64_t number, uint64_t address, uint64_t value)
{
    const struct phipia_path *request = (const struct phipia_path *)(uintptr_t)address;
    ++calls;
    if (number != PHIPIA_SYS_PATH_MKDIR || value != (PHIPIA_MKDIR_MODE_PRESENT | expected_mode) ||
        request->volume != PHIPIA_VOLUME_DATA || request->reserved != 0U ||
        request->length != 6U || memcmp((const void *)(uintptr_t)request->address, "nested", 6U) != 0) {
        invalid_request = 1;
    }
    return syscall_result;
}

int main(void)
{
    const mode_t modes[] = {0U, 0700U, 01720U, 07777U};
    for (unsigned index = 0U; index < sizeof(modes) / sizeof(modes[0]); ++index) {
        expected_mode = modes[index];
        syscall_result = -PHIPIA_EIO;
        if (mkdir("nested", modes[index]) != -1 || errno != EIO) return 1;
        syscall_result = 0;
        if (mkdir("nested", modes[index]) != 0) return 2;
    }
    if (calls != 8U || invalid_request) return 3;
    if (mkdir(NULL, 0700U) != -1 || errno != EFAULT || calls != 8U) return 4;
    return 0;
}
