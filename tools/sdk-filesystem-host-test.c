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
static uint32_t expected_metadata_flags;
static struct phipia_path_metadata returned_metadata;
static int reenter_open;
static int nested_descriptor = -1;
static int reenter_close;
static int closing_descriptor;
static long close_result;
static long sync_result;
static unsigned file_sync_calls;

long phipia_syscall1(uint64_t number, uint64_t address)
{
    if (number == PHIPIA_SYS_FUTEX_WAKE) return 0;
    if (number == PHIPIA_SYS_FILE_SYNC) {
        if (address != 42U) invalid_request = 1;
        ++file_sync_calls;
        return sync_result;
    }
    if (number == PHIPIA_SYS_HANDLE_CLOSE) {
        if (address != 42U) invalid_request = 1;
        ++close_calls;
        if (reenter_close) {
            reenter_close = 0;
            if (close(closing_descriptor) != -1 || errno != EBADF) invalid_request = 1;
            nested_descriptor = open("nested", O_RDONLY);
        }
        return close_result;
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
    if (reenter_open) {
        reenter_open = 0;
        nested_descriptor = open("nested", O_RDONLY);
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

long phipia_syscall3(uint64_t number, uint64_t address, uint64_t output, uint64_t flags)
{
    const struct phipia_path *request = (const struct phipia_path *)(uintptr_t)address;
    if (number != PHIPIA_SYS_PATH_METADATA || flags != expected_metadata_flags ||
        request->volume != PHIPIA_VOLUME_DATA || request->length != 6U ||
        memcmp((const void *)(uintptr_t)request->address, "nested", 6U) != 0) invalid_request = 1;
    if (syscall_result >= 0) memcpy((void *)(uintptr_t)output, &returned_metadata, sizeof(returned_metadata));
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
    returned_metadata = (struct phipia_path_metadata){
        .size = sizeof(returned_metadata), .version = PHIPIA_ABI_VERSION,
        .byte_length = UINT64_C(67108864), .object_id = 1234U,
        .mode = 0100640U, .uid = 70000U, .gid = 90000U, .links = 3U,
        .atime_seconds = -1, .atime_nanos = 123U,
        .mtime_seconds = INT64_C(2147483648), .mtime_nanos = 999999999U,
        .ctime_seconds = INT64_C(-2147483648), .ctime_nanos = 0U,
        .flags = PHIPIA_METADATA_UNIX_FIELDS,
    };
    struct stat metadata;
    syscall_result = 0;
    if (stat("Data:/nested", &metadata) != 0 || metadata.st_mode != 0100640U ||
        metadata.st_uid != 70000U || metadata.st_gid != 90000U || metadata.st_nlink != 3U ||
        metadata.st_ino != 1234U || metadata.st_size != UINT64_C(67108864) ||
        metadata.st_atim.tv_sec != -1 || metadata.st_atim.tv_nsec != 123L ||
        metadata.st_mtim.tv_sec != INT64_C(2147483648) || metadata.st_mtim.tv_nsec != 999999999L ||
        metadata.st_ctime != INT64_C(-2147483648) || !S_ISREG(metadata.st_mode)) return 13;
    expected_metadata_flags = PHIPIA_METADATA_NOFOLLOW;
    returned_metadata.mode = 0120777U;
    if (lstat("nested", &metadata) != 0 || !S_ISLNK(metadata.st_mode) ||
        S_ISREG(metadata.st_mode) || S_ISDIR(metadata.st_mode)) return 14;
    returned_metadata.mode = 0042751U;
    if (lstat("nested", &metadata) != 0 || !S_ISDIR(metadata.st_mode) || S_ISREG(metadata.st_mode)) return 15;
    const struct stat saved = metadata;
    syscall_result = -PHIPIA_ENOENT;
    if (lstat("nested", &metadata) != -1 || errno != ENOENT || memcmp(&saved, &metadata, sizeof(saved)) != 0) return 16;
    syscall_result = 0;
    returned_metadata.atime_nanos = 1000000000U;
    if (lstat("nested", &metadata) != -1 || errno != EIO || memcmp(&saved, &metadata, sizeof(saved)) != 0) return 17;
    returned_metadata.atime_nanos = 0U;
    returned_metadata.size = 24U;
    if (lstat("nested", &metadata) != -1 || errno != EIO) return 18;
    if (stat("nested", NULL) != -1 || errno != EFAULT || invalid_request) return 19;
    expected_open_flags = PHIPIA_OPEN_CREATE | PHIPIA_OPEN_WRITE | PHIPIA_OPEN_MODE_PRESENT | PHIPIA_OPEN_EXCLUSIVE;
    expected_mode = 0600U;
    syscall_result = -PHIPIA_EEXIST;
    if (open("nested", O_CREAT | O_EXCL | O_WRONLY, 0600) != -1 || errno != EEXIST) return 20;
    syscall_result = 42;
    descriptor = open("nested", O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (descriptor < 3 || close(descriptor) != 0) return 21;
    if (open("nested", O_EXCL | O_WRONLY) != -1 || errno != EINVAL) return 22;
    if (open_calls != 12U || close_calls != 7U || invalid_request) return 23;
    expected_mode = 0U;
    expected_open_flags = PHIPIA_OPEN_READ;
    reenter_open = 1;
    descriptor = open("nested", O_RDONLY);
    if (descriptor < 3 || nested_descriptor < 3 || descriptor == nested_descriptor) return 24;
    if (close(descriptor) != 0 || close(nested_descriptor) != 0) return 25;
    int held[29];
    for (unsigned index = 0U; index < 29U; ++index) {
        held[index] = open("nested", O_RDONLY);
        if (held[index] != (int)index + 3) return 26;
    }
    const unsigned before_full = open_calls;
    if (open("nested", O_CREAT | O_TRUNC | O_WRONLY, 0600) != -1 || errno != EMFILE ||
        open_calls != before_full) return 27;
    for (unsigned index = 0U; index < 29U; ++index) if (close(held[index]) != 0) return 28;
    if (open_calls != 43U || close_calls != 38U || invalid_request) return 29;
    closing_descriptor = open("nested", O_RDONLY);
    reenter_close = 1;
    if (closing_descriptor != 3 || close(closing_descriptor) != 0 || nested_descriptor != closing_descriptor) return 30;
    if (close(nested_descriptor) != 0) return 31; // outer close did not erase the new descriptor
    descriptor = open("nested", O_RDONLY);
    close_result = -PHIPIA_EIO;
    if (close(descriptor) != -1 || errno != EIO) return 32;
    if (close(descriptor) != -1 || errno != EBADF) return 33;
    close_result = 0;
    descriptor = open("nested", O_RDONLY);
    if (descriptor != 3 || close(descriptor) != 0) return 34;
    if (open_calls != 47U || close_calls != 42U || invalid_request) return 35;
    descriptor = open("nested", O_RDONLY);
    sync_result = -PHIPIA_EIO;
    if (fsync(descriptor) != -1 || errno != EIO) return 36;
    sync_result = 0;
    if (fsync(descriptor) != 0 || close(descriptor) != 0) return 37;
    if (fsync(descriptor) != -1 || errno != EBADF || file_sync_calls != 2U) return 38;
    if (open_calls != 48U || close_calls != 43U || invalid_request) return 39;
    return 0;
}
