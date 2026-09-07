/* SPDX-License-Identifier: GPL-3.0-only */
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "internal.h"

#define DESCRIPTOR_MAX 32

struct descriptor_record {
    phipia_handle_t handle;
    uint16_t volume;
    char path[PHIPIA_PATH_MAX + 1U];
    int active;
};

static struct descriptor_record descriptors[DESCRIPTOR_MAX];
static volatile uint32_t descriptor_lock;

static int descriptor_snapshot(int number, struct descriptor_record *record, int retire)
{
    int found = 0;
    phipia_runtime_lock(&descriptor_lock);
    if (number >= 3 && number < DESCRIPTOR_MAX && descriptors[number].active == 1) {
        *record = descriptors[number];
        if (retire) (void)memset(&descriptors[number], 0, sizeof(descriptors[number]));
        found = 1;
    }
    phipia_runtime_unlock(&descriptor_lock);
    return found;
}

int open(const char *path, int flags, ...)
{
    struct phipia_runtime_path parsed;
    uint32_t native = 0U;
    long handle;
    int number = -1;

    if (phipia_runtime_path(path, &parsed) != 0) return -1;
    if ((flags & O_EXCL) != 0 && (flags & O_CREAT) == 0) { errno = EINVAL; return -1; }
    if ((flags & O_RDWR) == O_RDWR) native |= PHIPIA_OPEN_READ | PHIPIA_OPEN_WRITE;
    else if ((flags & O_WRONLY) != 0) native |= PHIPIA_OPEN_WRITE;
    else native |= PHIPIA_OPEN_READ;
    if ((flags & O_CREAT) != 0) native |= PHIPIA_OPEN_CREATE;
    if ((flags & O_TRUNC) != 0) native |= PHIPIA_OPEN_TRUNCATE;
    if ((flags & O_APPEND) != 0) native |= PHIPIA_OPEN_APPEND;
    if ((flags & O_EXCL) != 0) native |= PHIPIA_OPEN_EXCLUSIVE;
    // Reserve the descriptor before a native create/truncate can mutate disk.
    // State 2 is private to this open and is not a usable descriptor.
    phipia_runtime_lock(&descriptor_lock);
    for (int index = 3; index < DESCRIPTOR_MAX; ++index) {
        if (!descriptors[index].active) { number = index; descriptors[index].active = 2; break; }
    }
    phipia_runtime_unlock(&descriptor_lock);
    if (number < 0) { errno = EMFILE; return -1; }
    if ((flags & O_CREAT) != 0) {
        va_list arguments;
        va_start(arguments, flags);
        const mode_t mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
        handle = phipia_file_open_mode(parsed.volume, parsed.text, native, (uint16_t)(mode & 07777U));
    } else {
        handle = phipia_file_open(parsed.volume, parsed.text, native);
    }
    phipia_runtime_lock(&descriptor_lock);
    if (handle >= 0) {
        descriptors[number].handle = (phipia_handle_t)handle;
        descriptors[number].volume = parsed.volume;
        (void)memcpy(descriptors[number].path, parsed.text, parsed.length + 1U);
        descriptors[number].active = 1;
    } else { (void)memset(&descriptors[number], 0, sizeof(descriptors[number])); }
    phipia_runtime_unlock(&descriptor_lock);
    if (handle < 0) { errno = (int)-handle; return -1; }
    if ((flags & O_APPEND) != 0 &&
        lseek(number, 0, SEEK_END) < 0) { (void)close(number); return -1; }
    return number;
}

ssize_t read(int number, void *buffer, size_t length)
{
    struct descriptor_record record;
    long result;
    if (number == STDIN_FILENO) { errno = EAGAIN; return -1; }
    if (!descriptor_snapshot(number, &record, 0) || buffer == NULL) { errno = EBADF; return -1; }
    result = phipia_file_read(record.handle, buffer, length);
    if (result < 0) { errno = (int)-result; return -1; }
    return (ssize_t)result;
}

ssize_t write(int number, const void *buffer, size_t length)
{
    struct descriptor_record record;
    long result;
    if ((number == STDOUT_FILENO || number == STDERR_FILENO) && buffer != NULL) {
        result = phipia_syscall2(PHIPIA_SYS_CONSOLE_WRITE,
            (uint64_t)(uintptr_t)buffer, length);
    } else if (buffer != NULL && descriptor_snapshot(number, &record, 0)) {
        result = phipia_file_write(record.handle, buffer, length);
    } else { errno = EBADF; return -1; }
    if (result < 0) { errno = (int)-result; return -1; }
    return (ssize_t)result;
}

off_t lseek(int number, off_t offset, int origin)
{
    struct descriptor_record record;
    long result;
    if (!descriptor_snapshot(number, &record, 0) || origin < SEEK_SET || origin > SEEK_END) { errno = EBADF; return -1; }
    result = phipia_file_seek(record.handle, offset, (uint32_t)origin);
    if (result < 0) { errno = (int)-result; return -1; }
    return (off_t)result;
}

int close(int number)
{
    struct descriptor_record record;
    long result;
    // Retire before entering native teardown: a nested/concurrent open may
    // reuse the number, and this close must never erase that replacement.
    if (!descriptor_snapshot(number, &record, 1)) { errno = EBADF; return -1; }
    result = phipia_handle_close(record.handle);
    return phipia_result(result);
}

static int path_metadata(const char *path, struct stat *result, uint32_t flags)
{
    struct phipia_runtime_path parsed;
    struct phipia_path_metadata native = {0};
    long status;
    if (result == NULL) { errno = EFAULT; return -1; }
    if (phipia_runtime_path(path, &parsed) != 0) return -1;
    status = phipia_path_metadata(parsed.volume, parsed.text, flags, &native);
    if (status < 0) { errno = (int)-status; return -1; }
    if (native.size != sizeof(native) || native.version != PHIPIA_ABI_VERSION ||
        native.atime_nanos >= 1000000000U || native.mtime_nanos >= 1000000000U ||
        native.ctime_nanos >= 1000000000U) { errno = EIO; return -1; }
    (void)memset(result, 0, sizeof(*result));
    result->st_size = native.byte_length;
    result->st_mode = native.mode;
    result->st_uid = native.uid;
    result->st_gid = native.gid;
    result->st_nlink = native.links;
    result->st_ino = native.object_id;
    result->st_atim = (struct timespec){native.atime_seconds, (long)native.atime_nanos};
    result->st_mtim = (struct timespec){native.mtime_seconds, (long)native.mtime_nanos};
    result->st_ctim = (struct timespec){native.ctime_seconds, (long)native.ctime_nanos};
    return 0;
}

int stat(const char *path, struct stat *result) { return path_metadata(path, result, 0U); }
int lstat(const char *path, struct stat *result) { return path_metadata(path, result, PHIPIA_METADATA_NOFOLLOW); }

int access(const char *path, int mode)
{
    struct stat result;
    if ((mode & ~(R_OK | W_OK)) != 0) { errno = EINVAL; return -1; }
    if (stat(path, &result) != 0) return -1;
    if ((mode & W_OK) != 0 && (result.st_mode & S_IWUSR) == 0U) { errno = EACCES; return -1; }
    return 0;
}

static int path_operation(const char *path, uint64_t number, uint64_t value)
{
    struct phipia_runtime_path parsed;
    struct phipia_path request;
    long result;
    if (phipia_runtime_path(path, &parsed) != 0) return -1;
    request = (struct phipia_path){(uint64_t)(uintptr_t)parsed.text,
        (uint32_t)parsed.length, parsed.volume, 0U};
    result = phipia_syscall2(number, (uint64_t)(uintptr_t)&request, value);
    return phipia_result(result);
}
int unlink(const char *path) { return path_operation(path, PHIPIA_SYS_PATH_UNLINK, PHIPIA_UNLINK_FILE); }
int chmod(const char *path, mode_t mode) { return path_operation(path, PHIPIA_SYS_PATH_CHMOD, mode); }
int symlink(const char *target, const char *path)
{
    struct phipia_runtime_path parsed;
    if (phipia_runtime_path(path, &parsed) != 0) return -1;
    return phipia_result(phipia_path_symlink(parsed.volume, parsed.text, target));
}
int link(const char *source, const char *destination)
{
    struct phipia_runtime_path from;
    struct phipia_runtime_path to;
    if (phipia_runtime_path(source, &from) != 0 ||
        phipia_runtime_path(destination, &to) != 0) return -1;
    if (from.volume != to.volume) { errno = EXDEV; return -1; }
    return phipia_result(phipia_path_link(from.volume, from.text, to.text));
}
ssize_t readlink(const char *path, char *output, size_t capacity)
{
    struct phipia_runtime_path parsed;
    if (phipia_runtime_path(path, &parsed) != 0) return -1;
    return (ssize_t)phipia_result(phipia_path_readlink(parsed.volume, parsed.text, output, capacity));
}
int rmdir(const char *path) { return path_operation(path, PHIPIA_SYS_PATH_UNLINK, PHIPIA_UNLINK_DIRECTORY); }
int mkdir(const char *path, mode_t mode)
{
    return path_operation(path, PHIPIA_SYS_PATH_MKDIR,
        PHIPIA_MKDIR_MODE_PRESENT | (uint64_t)(mode & 07777U));
}
int ftruncate(int number, int64_t length)
{
    struct descriptor_record record;
    if (!descriptor_snapshot(number, &record, 0)) { errno = EBADF; return -1; }
    if (length < 0) { errno = EINVAL; return -1; }
    return phipia_result(phipia_file_truncate(record.handle, (uint64_t)length));
}
int fsync(int number)
{
    struct descriptor_record record;
    if (!descriptor_snapshot(number, &record, 0)) { errno = EBADF; return -1; }
    return phipia_result(phipia_file_sync(record.handle));
}
unsigned int sleep(unsigned int seconds)
{
    const struct timespec request = {(time_t)seconds, 0};
    return nanosleep(&request, NULL) == 0 ? 0U : seconds;
}
int usleep(unsigned int microseconds)
{
    const struct timespec request = {(time_t)(microseconds / 1000000U),
        (long)(microseconds % 1000000U) * 1000L};
    return nanosleep(&request, NULL);
}
int getpid(void) { return 1; }

DIR *opendir(const char *path)
{
    struct phipia_runtime_path parsed;
    long handle;
    DIR *result;
    if (phipia_runtime_path(path, &parsed) != 0) return NULL;
    handle = phipia_directory_open(parsed.volume, parsed.text);
    if (handle < 0) { errno = (int)-handle; return NULL; }
    result = calloc(1U, sizeof(*result));
    if (result == NULL) { (void)phipia_handle_close((phipia_handle_t)handle); return NULL; }
    result->handle = (phipia_handle_t)handle;
    return result;
}
struct dirent *readdir(DIR *directory)
{
    struct phipia_directory_entry_long native;
    long result;
    if (directory == NULL) { errno = EBADF; return NULL; }
    result = phipia_directory_read_long(directory->handle, &native);
    if (result <= 0) { if (result < 0) errno = (int)-result; return NULL; }
    if (native.name_length >= sizeof(directory->entry.d_name)) { errno = EIO; return NULL; }
    (void)memcpy(directory->entry.d_name, native.name, native.name_length);
    directory->entry.d_name[native.name_length] = '\0';
    directory->entry.d_type = (native.attributes & PHIPIA_PATH_DIRECTORY) != 0U ? DT_DIR : DT_REG;
    return &directory->entry;
}
int closedir(DIR *directory)
{
    long result;
    if (directory == NULL) { errno = EBADF; return -1; }
    result = phipia_handle_close(directory->handle);
    free(directory);
    return phipia_result(result);
}
