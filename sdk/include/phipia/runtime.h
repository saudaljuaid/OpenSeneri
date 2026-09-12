/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_RUNTIME_H
#define PHIPIA_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include <phipia/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

struct phipia_startup {
    int argc;
    char **argv;
    char **environment;
    uint64_t tls_image;
    uint64_t tls_size;
    uint64_t tls_alignment;
};

long phipia_syscall0(uint64_t number);
long phipia_syscall1(uint64_t number, uint64_t argument0);
long phipia_syscall2(uint64_t number, uint64_t argument0, uint64_t argument1);
long phipia_syscall3(uint64_t number, uint64_t argument0, uint64_t argument1,
    uint64_t argument2);
long phipia_syscall4(uint64_t number, uint64_t argument0, uint64_t argument1,
    uint64_t argument2, uint64_t argument3);
long phipia_syscall5(uint64_t number, uint64_t argument0, uint64_t argument1,
    uint64_t argument2, uint64_t argument3, uint64_t argument4);
long phipia_syscall6(uint64_t number, uint64_t argument0, uint64_t argument1,
    uint64_t argument2, uint64_t argument3, uint64_t argument4,
    uint64_t argument5);

void phipia_runtime_initialize(int argc, char **argv, char **environment);
const struct phipia_startup *phipia_startup_information(void);
int phipia_result(long result);
long phipia_handle_close(phipia_handle_t handle);
long phipia_handle_duplicate(phipia_handle_t handle);
long phipia_console_read(void *buffer, size_t length);
long phipia_memory_allocate(size_t length, uint32_t flags,
    struct phipia_memory_map_response *response);
long phipia_memory_release(uint64_t address, uint64_t length);
long phipia_file_open(uint16_t volume, const char *path, uint32_t flags);
long phipia_file_read(phipia_handle_t handle, void *buffer, size_t length);
long phipia_file_write(phipia_handle_t handle, const void *buffer,
    size_t length);
long phipia_file_seek(phipia_handle_t handle, int64_t offset,
    uint32_t origin);
long phipia_path_stat(uint16_t volume, const char *path,
    struct phipia_path_stat *result);
long phipia_directory_open(uint16_t volume, const char *path);
long phipia_directory_read(phipia_handle_t handle,
    struct phipia_directory_entry *entry);
long phipia_path_mkdir(uint16_t volume, const char *path);
long phipia_path_rename(uint16_t volume, const char *source,
    const char *destination);
long phipia_path_replace(uint16_t volume, const char *source,
    const char *destination);
long phipia_path_unlink(uint16_t volume, const char *path);
long phipia_path_truncate(uint16_t volume, const char *path, uint64_t length);
long phipia_volume_sync(uint16_t volume);
long phipia_volume_space(uint16_t volume, struct phipia_volume_space *space);
uint64_t phipia_monotonic_ns(void);
long phipia_realtime_seconds(void);
long phipia_sleep_until(uint64_t deadline_ns);
long phipia_random(void *buffer, size_t length);
long phipia_random_strong(void *buffer, size_t length);

#ifdef __cplusplus
}
#endif

#endif
