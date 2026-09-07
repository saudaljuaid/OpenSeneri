/* SPDX-License-Identifier: GPL-3.0-only */
/* Exercise the production POSIX wrapper through its native syscall boundary. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../sdk/src/internal.h"

static uint64_t expected_mode;
static uint32_t expected_open_flags;
static long syscall_result;
static unsigned calls;
static unsigned open_calls;
static unsigned close_calls;
static int invalid_request;

long phipia_syscall1(uint64_t number, uint64_t address)
{
    if (number == PHIPIA_SYS_FUTEX_WAKE) return 0;
    if (number == PHIPIA_SYS_HANDLE_CLOSE) {
        if (address != 42U) invalid_request = 1;
        ++close_calls;
        return 0;
    }
    if (number != PHIPIA_SYS_FILE_OPEN) { invalid_request = 1; return -PHIPIA_EINVAL; }
    const struct phipia_file_open_request *request = (const struct phipia_file_open_request *)(uintptr_t)address;
    ++open_calls;
    if (request->size != sizeof(*request) || request->version != PHIPIA_ABI_VERSION ||
        request->flags != expected_open_flags || request->reserved != expected_mode ||
        request->path.volume != PHIPIA_VOLUME_DATA || request->path.reserved != 0U ||
        request->path.length != 6U || memcmp((const void *)(uintptr_t)request->path.address, "nested", 6U) != 0) {
        invalid_request = 1;
    }
    return syscall_result;
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
    if (mkdir(NULL, 0700U) != -1 || errno != EINVAL || calls != 8U) return 4;
    expected_open_flags = PHIPIA_OPEN_CREATE | PHIPIA_OPEN_WRITE | PHIPIA_OPEN_MODE_PRESENT;
    for (unsigned index = 0U; index < sizeof(modes) / sizeof(modes[0]); ++index) {
        expected_mode = modes[index];
        syscall_result = -PHIPIA_EIO;
        if (open("Data:/nested", O_CREAT | O_WRONLY, (int)modes[index]) != -1 || errno != EIO) return 5;
        syscall_result = 42;
        int descriptor = open("Data:/nested", O_CREAT | O_WRONLY, (int)modes[index]);
        if (descriptor < 3 || close(descriptor) != 0) return 6;
    }
    if (open_calls != 8U || close_calls != 4U || invalid_request) return 7;
    expected_mode = 0U;
    expected_open_flags = PHIPIA_OPEN_READ;
    int descriptor = open("nested", O_RDONLY); // no variadic argument
    if (descriptor < 3 || close(descriptor) != 0) return 8;
    expected_open_flags = PHIPIA_OPEN_READ | PHIPIA_OPEN_CREATE;
    if (phipia_file_open(PHIPIA_VOLUME_DATA, "nested", expected_open_flags) != 42) return 9;
    if (phipia_handle_close(42U) != 0) return 10;
    if (phipia_file_open_mode(PHIPIA_VOLUME_DATA, "nested", PHIPIA_OPEN_READ, 0700U) != -PHIPIA_EINVAL ||
        phipia_file_open_mode(PHIPIA_VOLUME_DATA, "nested", PHIPIA_OPEN_CREATE, 010000U) != -PHIPIA_EINVAL) return 11;
    if (open_calls != 10U || close_calls != 6U || invalid_request) return 12;
    return 0;
}
