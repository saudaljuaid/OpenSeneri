/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_LINUX_USERLAND_H
#define PHIPIA_LINUX_USERLAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum linux_userland_profile {
    LINUX_USERLAND_PROFILE_ECHO = 0,
    LINUX_USERLAND_PROFILE_UNAME,
    LINUX_USERLAND_PROFILE_CAT,
    LINUX_USERLAND_PROFILE_COUNT
};

enum linux_userland_status {
    LINUX_USERLAND_STATUS_OK = 0,
    LINUX_USERLAND_STATUS_WAITING,
    LINUX_USERLAND_STATUS_NULL_ARGUMENT,
    LINUX_USERLAND_STATUS_PROFILE,
    LINUX_USERLAND_STATUS_BOOT_LEDGER,
    LINUX_USERLAND_STATUS_VOLUME_ABSENT,
    LINUX_USERLAND_STATUS_PROFILE_REFUSED,
    LINUX_USERLAND_STATUS_LAUNCH_REFUSED,
    LINUX_USERLAND_STATUS_INPUT_REFUSED,
    LINUX_USERLAND_STATUS_TEARDOWN,
    LINUX_USERLAND_STATUS_COUNT
};

struct linux_userland_result {
    enum linux_userland_profile profile;
    uint64_t generation;
    uint32_t file_bytes;
    uint32_t stdout_bytes;
    uint32_t syscall_count;
    uint32_t exit_status;
    bool rust_validated;
    bool ring_three;
    bool real_syscall_entry;
    bool stdout_valid;
    bool teardown_complete;
    uint32_t input_bytes;
    uint32_t input_lines;
    uint32_t resume_count;
    bool waiting_for_input;
    bool eof_delivered;
};

enum linux_userland_status linux_userland_launch(
    enum linux_userland_profile profile,
    struct linux_userland_result *result
);
enum linux_userland_status linux_userland_deliver_cat_input(
    const uint8_t *bytes,
    size_t byte_count,
    bool eof,
    struct linux_userland_result *result
);
bool linux_userland_foreground_waiting(void);
uint64_t linux_userland_active_generation(void);
enum linux_userland_status linux_userland_abort_foreground(void);
bool linux_userland_resources_released(void);
uint32_t linux_userland_completed(enum linux_userland_profile profile);
const char *linux_userland_profile_name(enum linux_userland_profile profile);
const char *linux_userland_status_string(enum linux_userland_status status);

#endif
