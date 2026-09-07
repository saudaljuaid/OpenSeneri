/* SPDX-License-Identifier: GPL-3.0-only */
#include <phipia/runtime.h>

#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#define PHIPIA_AUX_NULL UINT64_C(0)
#define PHIPIA_AUX_TLS_IMAGE UINT64_C(0x53500002)
#define PHIPIA_AUX_TLS_SIZE UINT64_C(0x53500003)
#define PHIPIA_AUX_TLS_ALIGN UINT64_C(0x53500004)
#ifndef PHIPIA_HOSTED
#define PHIPIA_ATEXIT_MAX 16U
#endif

_Thread_local int errno;
static struct phipia_startup startup;
#ifndef PHIPIA_HOSTED
static void (*exit_functions[PHIPIA_ATEXIT_MAX])(void);
static size_t exit_function_count;
static volatile uint32_t exit_lock;
#endif

void phipia_runtime_initialize(int argc, char **argv, char **environment)
{
    char **cursor = environment;
    const uint64_t *auxiliary;

    startup.argc = argc;
    startup.argv = argv;
    startup.environment = environment;
    while (*cursor != NULL) {
        ++cursor;
    }
    auxiliary = (const uint64_t *)(const void *)(cursor + 1);
    while (auxiliary[0] != PHIPIA_AUX_NULL) {
        if (auxiliary[0] == PHIPIA_AUX_TLS_IMAGE) {
            startup.tls_image = auxiliary[1];
        } else if (auxiliary[0] == PHIPIA_AUX_TLS_SIZE) {
            startup.tls_size = auxiliary[1];
        } else if (auxiliary[0] == PHIPIA_AUX_TLS_ALIGN) {
            startup.tls_alignment = auxiliary[1];
        }
        auxiliary += 2;
    }
}

const struct phipia_startup *phipia_startup_information(void)
{
    return &startup;
}

int phipia_result(long result)
{
    if (result < 0) {
        errno = (int)-result;
        return -1;
    }
    return (int)result;
}

long phipia_handle_close(phipia_handle_t handle)
{
    return phipia_syscall1(PHIPIA_SYS_HANDLE_CLOSE, handle);
}

long phipia_handle_duplicate(phipia_handle_t handle)
{
    return phipia_syscall1(PHIPIA_SYS_HANDLE_DUPLICATE, handle);
}

long phipia_console_read(void *buffer, size_t length)
{
    return phipia_syscall2(PHIPIA_SYS_CONSOLE_READ,
        (uint64_t)(uintptr_t)buffer, length);
}

long phipia_memory_allocate(
    size_t length,
    uint32_t flags,
    struct phipia_memory_map_response *response
)
{
    const struct phipia_memory_map_request request = {
        sizeof(request), PHIPIA_ABI_VERSION, length, 0U, flags, 0U
    };

    return phipia_syscall2(PHIPIA_SYS_MEMORY_MAP,
        (uint64_t)(uintptr_t)&request, (uint64_t)(uintptr_t)response);
}

long phipia_memory_release(uint64_t address, uint64_t length)
{
    return phipia_syscall2(PHIPIA_SYS_MEMORY_UNMAP, address, length);
}

static long file_open_request(uint16_t volume, const char *path, uint32_t flags, uint16_t mode)
{
    struct phipia_file_open_request request;

    if (path == NULL) {
        return -PHIPIA_EFAULT;
    }
    request.size = sizeof(request);
    request.version = PHIPIA_ABI_VERSION;
    request.path.address = (uint64_t)(uintptr_t)path;
    request.path.length = (uint32_t)strlen(path);
    request.path.volume = volume;
    request.path.reserved = 0U;
    request.flags = flags;
    request.reserved = mode;
    return phipia_syscall1(PHIPIA_SYS_FILE_OPEN,
        (uint64_t)(uintptr_t)&request);
}

long phipia_file_open(uint16_t volume, const char *path, uint32_t flags)
{
    return file_open_request(volume, path, flags, 0U);
}

long phipia_file_open_mode(uint16_t volume, const char *path, uint32_t flags, uint16_t mode)
{
    if ((flags & PHIPIA_OPEN_CREATE) == 0U || (mode & ~07777U) != 0U) return -PHIPIA_EINVAL;
    return file_open_request(volume, path, flags | PHIPIA_OPEN_MODE_PRESENT, mode);
}

static long file_io(
    uint64_t number,
    phipia_handle_t handle,
    void *buffer,
    size_t length
)
{
    const struct phipia_io_request request = {
        sizeof(request), PHIPIA_ABI_VERSION, handle,
        (uint64_t)(uintptr_t)buffer, UINT64_MAX, (uint32_t)length, 0U
    };

    if (length > UINT32_MAX) {
        return -PHIPIA_EINVAL;
    }
    return phipia_syscall1(number, (uint64_t)(uintptr_t)&request);
}

long phipia_file_read(phipia_handle_t handle, void *buffer, size_t length)
{
    return file_io(PHIPIA_SYS_FILE_READ, handle, buffer, length);
}

long phipia_file_write(
    phipia_handle_t handle,
    const void *buffer,
    size_t length
)
{
    return file_io(PHIPIA_SYS_FILE_WRITE, handle,
        (void *)(uintptr_t)buffer, length);
}

long phipia_file_seek(phipia_handle_t handle, int64_t offset, uint32_t origin)
{
    const struct phipia_seek_request request = {
        sizeof(request), PHIPIA_ABI_VERSION, handle, offset, origin, 0U
    };

    return phipia_syscall1(PHIPIA_SYS_FILE_SEEK,
        (uint64_t)(uintptr_t)&request);
}

static struct phipia_path make_path(uint16_t volume, const char *path)
{
    const struct phipia_path result = {
        (uint64_t)(uintptr_t)path, (uint32_t)strlen(path), volume, 0U
    };

    return result;
}

static long single_path(uint64_t number, uint16_t volume, const char *path,
    uint64_t value)
{
    struct phipia_path input;

    if (path == NULL) {
        return -PHIPIA_EFAULT;
    }
    input = make_path(volume, path);
    return phipia_syscall2(number, (uint64_t)(uintptr_t)&input, value);
}

long phipia_path_stat(
    uint16_t volume,
    const char *path,
    struct phipia_path_stat *result
)
{
    struct phipia_path input;

    if (path == NULL || result == NULL) {
        return -PHIPIA_EFAULT;
    }
    input = make_path(volume, path);

    return phipia_syscall2(PHIPIA_SYS_PATH_STAT,
        (uint64_t)(uintptr_t)&input, (uint64_t)(uintptr_t)result);
}

long phipia_path_metadata(uint16_t volume, const char *path, uint32_t flags,
    struct phipia_path_metadata *result)
{
    if (path == NULL || result == NULL) return -PHIPIA_EFAULT;
    if ((flags & ~PHIPIA_METADATA_NOFOLLOW) != 0U) return -PHIPIA_EINVAL;
    const struct phipia_path input = make_path(volume, path);
    return phipia_syscall3(PHIPIA_SYS_PATH_METADATA, (uint64_t)(uintptr_t)&input,
        (uint64_t)(uintptr_t)result, flags);
}

long phipia_directory_open(uint16_t volume, const char *path)
{
    struct phipia_path input;

    if (path == NULL) {
        return -PHIPIA_EFAULT;
    }
    input = make_path(volume, path);

    return phipia_syscall1(PHIPIA_SYS_DIRECTORY_OPEN,
        (uint64_t)(uintptr_t)&input);
}

long phipia_directory_read_long(phipia_handle_t handle, struct phipia_directory_entry_long *entry)
{
    return phipia_syscall2(PHIPIA_SYS_DIRECTORY_READ_LONG, handle, (uint64_t)(uintptr_t)entry);
}

long phipia_directory_read(
    phipia_handle_t handle,
    struct phipia_directory_entry *entry
)
{
    return phipia_syscall2(PHIPIA_SYS_DIRECTORY_READ, handle,
        (uint64_t)(uintptr_t)entry);
}

long phipia_path_mkdir(uint16_t volume, const char *path)
{
    return single_path(PHIPIA_SYS_PATH_MKDIR, volume, path, 0U);
}

static long rename_path(uint64_t number, uint16_t volume, const char *source,
    const char *destination)
{
    struct phipia_rename_request request;

    if (source == NULL || destination == NULL) {
        return -PHIPIA_EFAULT;
    }
    request.size = sizeof(request);
    request.version = PHIPIA_ABI_VERSION;
    request.source = make_path(volume, source);
    request.destination = make_path(volume, destination);
    request.flags = 0U;
    request.reserved = 0U;
    return phipia_syscall1(number, (uint64_t)(uintptr_t)&request);
}

long phipia_path_rename(uint16_t volume, const char *source,
    const char *destination)
{
    return rename_path(PHIPIA_SYS_PATH_RENAME, volume, source, destination);
}

long phipia_path_link(uint16_t volume, const char *source, const char *destination)
{
    return rename_path(PHIPIA_SYS_PATH_LINK, volume, source, destination);
}

long phipia_path_replace(uint16_t volume, const char *source,
    const char *destination)
{
    return rename_path(PHIPIA_SYS_PATH_REPLACE, volume, source, destination);
}

long phipia_path_unlink(uint16_t volume, const char *path)
{
    return single_path(PHIPIA_SYS_PATH_UNLINK, volume, path, 0U);
}

long phipia_path_set_times(uint16_t volume, const char *path, const struct phipia_file_times *times)
{
    if (path == NULL || times == NULL) return -PHIPIA_EFAULT;
    const struct phipia_set_times_request request = {
        sizeof(request), PHIPIA_ABI_VERSION, make_path(volume, path), *times
    };
    return phipia_syscall1(PHIPIA_SYS_PATH_SET_TIMES, (uint64_t)(uintptr_t)&request);
}

long phipia_file_truncate(phipia_handle_t handle, uint64_t size)
{
    return phipia_syscall2(PHIPIA_SYS_FILE_TRUNCATE, handle, size);
}

long phipia_file_sync(phipia_handle_t handle)
{
    return phipia_syscall1(PHIPIA_SYS_FILE_SYNC, handle);
}

long phipia_file_metadata(phipia_handle_t handle, struct phipia_path_metadata *result)
{
    if (result == NULL) return -PHIPIA_EFAULT;
    return phipia_syscall2(PHIPIA_SYS_FILE_METADATA, handle, (uint64_t)(uintptr_t)result);
}

long phipia_path_chmod(uint16_t volume, const char *path, uint16_t mode)
{
    return single_path(PHIPIA_SYS_PATH_CHMOD, volume, path, mode);
}

static long path_xattr(uint16_t volume, const char *path, const char *name,
    uint64_t value, size_t length, uint32_t operation)
{
    if (path == NULL || name == NULL || (length != 0U && value == 0U)) return -PHIPIA_EFAULT;
    const size_t name_length = strlen(name);
    if (name_length == 0U || name_length > 255U || length > 4096U) return -PHIPIA_EINVAL;
    struct phipia_xattr_request request = {
        sizeof(request), PHIPIA_ABI_VERSION, make_path(volume, path),
        (uint64_t)(uintptr_t)name, (uint32_t)name_length, operation,
        value, (uint32_t)length, 0U
    };
    return phipia_syscall1(PHIPIA_SYS_PATH_XATTR, (uint64_t)(uintptr_t)&request);
}

long phipia_path_set_xattr(uint16_t volume, const char *path, const char *name,
    const void *value, size_t length)
{
    return path_xattr(volume, path, name, (uint64_t)(uintptr_t)value, length, PHIPIA_XATTR_SET);
}

long phipia_path_remove_xattr(uint16_t volume, const char *path, const char *name)
{
    return path_xattr(volume, path, name, 0U, 0U, PHIPIA_XATTR_REMOVE);
}

long phipia_path_get_xattr(uint16_t volume, const char *path, const char *name,
    void *output, size_t capacity)
{
    return path_xattr(volume, path, name, (uint64_t)(uintptr_t)output, capacity, PHIPIA_XATTR_GET);
}

long phipia_path_symlink(uint16_t volume, const char *path, const char *target)
{
    if (path == NULL || target == NULL) return -PHIPIA_EFAULT;
    struct phipia_path request = make_path(volume, path);
    return phipia_syscall3(PHIPIA_SYS_PATH_SYMLINK, (uint64_t)(uintptr_t)&request,
        (uint64_t)(uintptr_t)target, strlen(target));
}

long phipia_path_readlink(uint16_t volume, const char *path, void *output, size_t capacity)
{
    if (path == NULL || output == NULL) return -PHIPIA_EFAULT;
    struct phipia_path request = make_path(volume, path);
    return phipia_syscall3(PHIPIA_SYS_PATH_READLINK, (uint64_t)(uintptr_t)&request,
        (uint64_t)(uintptr_t)output, capacity);
}

long phipia_path_truncate(uint16_t volume, const char *path, uint64_t length)
{
    return single_path(PHIPIA_SYS_PATH_TRUNCATE, volume, path, length);
}

long phipia_volume_sync(uint16_t volume)
{
    return phipia_syscall1(PHIPIA_SYS_VOLUME_SYNC, volume);
}

long phipia_volume_space(uint16_t volume, struct phipia_volume_space *space)
{
    if (space == NULL) {
        return -PHIPIA_EFAULT;
    }
    return phipia_syscall2(PHIPIA_SYS_VOLUME_SPACE, volume,
        (uint64_t)(uintptr_t)space);
}

uint64_t phipia_monotonic_ns(void)
{
    const long result = phipia_syscall0(PHIPIA_SYS_TIME_MONOTONIC);

    if (result < 0) {
        errno = (int)-result;
        return 0U;
    }
    return (uint64_t)result;
}

long phipia_realtime_seconds(void)
{
    return phipia_syscall0(PHIPIA_SYS_TIME_REALTIME);
}

long phipia_sleep_until(uint64_t deadline_ns)
{
    return phipia_syscall1(PHIPIA_SYS_SLEEP_UNTIL, deadline_ns);
}

long phipia_random(void *buffer, size_t length)
{
    return phipia_syscall2(PHIPIA_SYS_RANDOM, (uint64_t)(uintptr_t)buffer,
        length);
}

long phipia_random_strong(void *buffer, size_t length)
{
    return phipia_syscall2(PHIPIA_SYS_RANDOM_STRONG,
        (uint64_t)(uintptr_t)buffer, length);
}

int phipia_runtime_path(const char *input, struct phipia_runtime_path *result)
{
    if (input == NULL || result == NULL || *input == '\0') {
        errno = EINVAL;
        return -1;
    }
    result->volume = PHIPIA_VOLUME_DATA;
    result->text = input;
    if (strncmp(input, "System:", 7U) == 0) {
        result->volume = PHIPIA_VOLUME_SYSTEM;
        result->text += 7;
    } else if (strncmp(input, "Data:", 5U) == 0) {
        result->text += 5;
    }
    while (*result->text == '/') {
        ++result->text;
    }
    result->length = strlen(result->text);
    if (result->length == 0U || result->length > PHIPIA_PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

void phipia_runtime_lock(volatile uint32_t *lock)
{
    while (__atomic_exchange_n(lock, 1U, __ATOMIC_ACQUIRE) != 0U) {
        const struct phipia_futex_request request = {
            sizeof(request), PHIPIA_ABI_VERSION,
            (uint64_t)(uintptr_t)lock, 0U, 1U, 0U
        };
        (void)phipia_syscall1(PHIPIA_SYS_FUTEX_WAIT,
            (uint64_t)(uintptr_t)&request);
    }
}

void phipia_runtime_unlock(volatile uint32_t *lock)
{
    const struct phipia_futex_request request = {
        sizeof(request), PHIPIA_ABI_VERSION, (uint64_t)(uintptr_t)lock,
        0U, 0U, 1U
    };

    __atomic_store_n(lock, 0U, __ATOMIC_RELEASE);
    (void)phipia_syscall1(PHIPIA_SYS_FUTEX_WAKE,
        (uint64_t)(uintptr_t)&request);
}

#ifndef PHIPIA_HOSTED
int atexit(void (*function)(void))
{
    if (function == NULL) {
        errno = EINVAL;
        return -1;
    }
    phipia_runtime_lock(&exit_lock);
    if (exit_function_count == PHIPIA_ATEXIT_MAX) {
        phipia_runtime_unlock(&exit_lock);
        errno = ENOMEM;
        return -1;
    }
    exit_functions[exit_function_count++] = function;
    phipia_runtime_unlock(&exit_lock);
    return 0;
}

_Noreturn void exit(int status)
{
    (void)fflush(NULL);
    while (exit_function_count != 0U) {
        exit_functions[--exit_function_count]();
    }
    (void)phipia_syscall1(PHIPIA_SYS_EXIT, (uint64_t)(int64_t)status);
    __builtin_unreachable();
}

_Noreturn void abort(void)
{
    static const char message[] = "abort\n";

    (void)phipia_syscall2(PHIPIA_SYS_CONSOLE_WRITE,
        (uint64_t)(uintptr_t)message, sizeof(message) - 1U);
    exit(134);
}
#endif

char *getenv(const char *name)
{
    const size_t length = name == NULL ? 0U : strlen(name);

    if (length == 0U) {
        return NULL;
    }
    for (char **entry = startup.environment; entry != NULL && *entry != NULL;
         ++entry) {
        if (strncmp(*entry, name, length) == 0 && (*entry)[length] == '=') {
            return *entry + length + 1U;
        }
    }
    return NULL;
}

void __phipia_assert(const char *expression, const char *file, int line)
{
    fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line,
        expression);
    abort();
}
