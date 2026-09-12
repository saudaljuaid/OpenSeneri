/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/clock.h>
#include <phipia/boot_ledger.h>
#include <phipia/console.h>
#include <phipia/cpu.h>
#include <phipia/framebuffer.h>
#include <phipia/fat32_fs.h>
#include <phipia/heap.h>
#include <phipia/keyboard.h>
#include <phipia/linux_userland.h>
#include <phipia/linux_syscall.h>
#include <phipia/memory.h>
#include <phipia/native_process.h>
#include <phipia/network.h>
#include <phipia/pci.h>
#include <phipia/screen.h>
#include <phipia/shell.h>
#include <phipia/thread.h>
#include <phipia/ui.h>

/*
 * A command line.
 *
 * The shape of this file is one decision: the part that parses and dispatches
 * knows nothing about a keyboard, and the part that reads keys does nothing
 * else. shell_feed takes a character from anywhere - a key, a boot proof, a
 * scenario - and shell_run is a loop that supplies them.
 *
 * That split is why boot can prove this at all. A shell tested by pretending to
 * type is a shell whose parser was never separated from its input, and the
 * pretending is the part that rots.
 *
 * The other decision is that nothing here panics. Everything below this layer
 * refuses loudly because a wrong answer would be worse than a stopped machine.
 * A shell is the opposite: it exists to be operated by someone who will make
 * mistakes, so an unknown command is a line of output and not an incident.
 */

#define SHELL_PROMPT "phip> "
#define SHELL_NETWORK_OWNER UINT64_C(1)

/* What splits a command from its arguments. Nothing exotic; space and tab. */
static bool is_separator(char character)
{
    return character == ' ' || character == '\t';
}

static struct shell_state state;
static char line[SHELL_LINE_LIMIT + 1U];
static bool linux_prompt_evidence_pending;
static bool ui_keyboard_operational;
static bool ui_keyboard_decided;
static char filesystem_cwd[PHIPFS_MAX_PATH + 1U] = ".";

struct foreground_input_state {
    uint8_t line[LINUX_CAT_INPUT_LINE_BYTES + 1U];
    uint64_t generation;
    size_t length;
    uint32_t delivered_lines;
    uint32_t delivered_bytes;
    bool active;
    bool overflow_notified;
};

static struct foreground_input_state foreground;

static void zero_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void copy_bytes(uint8_t *destination, const uint8_t *source, size_t length)
{
    for (size_t index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
}

static bool matches(const char *text, const char *name)
{
    size_t index = 0U;

    while (text[index] != '\0' && name[index] != '\0') {
        if (text[index] != name[index]) {
            return false;
        }

        index += 1U;
    }

    /*
     * The command ends where the name does, and the rest of the line has to
     * begin with a separator or nothing at all. Without this, "e" would match
     * "echo" and "echoes" would too.
     */
    if (name[index] != '\0') {
        return false;
    }

    return text[index] == '\0' || is_separator(text[index]);
}

static bool argument_equals(const char *argument, const char *name)
{
    size_t index = 0U;

    while (argument[index] != '\0' && name[index] != '\0') {
        if (argument[index] != name[index]) {
            return false;
        }
        ++index;
    }
    if (name[index] != '\0') {
        return false;
    }
    while (is_separator(argument[index])) {
        ++index;
    }
    return argument[index] == '\0';
}

/* Everything after the command word, with leading separators removed. */
static const char *arguments_of(const char *text)
{
    size_t index = 0U;

    while (text[index] != '\0' && !is_separator(text[index])) {
        index += 1U;
    }

    while (text[index] != '\0' && is_separator(text[index])) {
        index += 1U;
    }

    return &text[index];
}

static void print_size(uint64_t bytes)
{
    console_write_u64(bytes);
    console_write(" bytes");

    if (bytes >= 1024U) {
        console_write(" (");
        console_write_u64(bytes / 1024U);
        console_write(" KiB)");
    }
}

static void command_help(void)
{
    console_write("  help      this list\n");
    console_write("  echo      print the rest of the line\n");
    console_write("  linux     run measured echo, uname, or bounded cat userspace\n");
    console_write("  native    launch one native application manifest\n");
    console_write("  native-start/native-go  stage and run several native apps\n");
    console_write("  drives    mounted FAT32 system and data volumes\n");
    console_write("  mount     retry a recoverable FAT32 mount\n");
    console_write("  ls/cd/pwd  browse the writable data volume\n");
    console_write("  mkdir/touch create files and directories\n");
    console_write("  read      print one file\n");
    console_write("  write     replace a file with one line\n");
    console_write("  append    append one line to a file\n");
    console_write("  writeat   overwrite from a byte offset\n");
    console_write("  truncate  set a file's byte length\n");
    console_write("  stat/mv/rm  inspect, move, or remove a path\n");
    console_write("  sync      persist completed data operations\n");
    console_write("  network/dhcp/ip  inspect or configure IPv4\n");
    console_write("  arp/ping/resolve inspect and test the network\n");
    console_write("  http      stream an HTTP response to the data volume\n");
    console_write("  netstat   bounded socket and packet counters\n");
    console_write("  reboot    sync, unmount, and restart cleanly\n");
    console_write("  clear     clear the screen\n");
    console_write("  fetch     Phipia identity and live system summary\n");
    console_write("  uptime    nanoseconds since the clock started\n");
    console_write("  mem       physical frames and kernel heap\n");
    console_write("  pci       every function enumeration found\n");
    console_write("  keys      keyboard counters\n");
    console_write("  threads   scheduler counters\n");
    console_write("  ledger    typed boot record\n");
    console_write("  version   what this is\n");
}

static void command_echo(const char *arguments)
{
    console_write(arguments);
    console_putc('\n');
}

static void command_linux(const char *arguments)
{
    struct linux_userland_result result;
    enum linux_userland_profile profile;
    enum linux_userland_status status;

    if (argument_equals(arguments, "echo")) {
        profile = LINUX_USERLAND_PROFILE_ECHO;
    } else if (argument_equals(arguments, "uname")) {
        profile = LINUX_USERLAND_PROFILE_UNAME;
    } else if (argument_equals(arguments, "cat")) {
        profile = LINUX_USERLAND_PROFILE_CAT;
    } else {
        console_write(
            "linux: use 'linux echo', 'linux uname', or 'linux cat'\n");
        console_serial_write("RW USERLAND unsupported profile refused\n");
        return;
    }
    console_serial_write("RW USERLAND command accepted through Phipia shell linux ");
    console_serial_write(linux_userland_profile_name(profile));
    console_serial_write("\n");
    status = linux_userland_launch(profile, &result);
    if (status == LINUX_USERLAND_STATUS_WAITING &&
        profile == LINUX_USERLAND_PROFILE_CAT) {
        zero_bytes(&foreground, sizeof(foreground));
        foreground.generation = result.generation;
        foreground.active = true;
        return;
    }
    if (status != LINUX_USERLAND_STATUS_OK) {
        console_write("linux: ");
        console_write(linux_userland_status_string(status));
        console_putc('\n');
    }
}

static void report_native_result(
    enum native_process_status status,
    const struct native_process_result *result
)
{
    console_write("native: ");
    console_write(native_process_status_string(status));
    if (status == NATIVE_PROCESS_OK && result != NULL) {
        console_write(" exit=");
        if (result->exit_status < 0) {
            console_putc('-');
            console_write_u64((uint64_t)(-(int64_t)result->exit_status));
        } else {
            console_write_u64((uint32_t)result->exit_status);
        }
        console_write(" syscalls=");
        console_write_u64(result->syscall_count);
        console_write(" switches=");
        console_write_u64(result->thread_switches);
        console_write(" released=");
        console_write(result->resources_released ? "yes" : "no");
    }
    console_putc('\n');
}

static void command_native(const char *arguments)
{
    struct native_process_result result;

    if (arguments[0] == '\0') {
        console_write("native: supply an 8.3 System manifest path\n");
        return;
    }
    report_native_result(native_process_launch(arguments, &result), &result);
}

static void command_native_start(const char *arguments)
{
    uint64_t generation;
    enum native_process_status status;

    if (arguments[0] == '\0') {
        console_write("native-start: supply an 8.3 System manifest path\n");
        return;
    }
    status = native_process_spawn(arguments, &generation);
    console_write("native-start: ");
    console_write(native_process_status_string(status));
    if (status == NATIVE_PROCESS_OK) {
        console_write(" generation=");
        console_write_u64(generation);
    }
    console_putc('\n');
}

static void command_native_go(void)
{
    struct native_process_result result;

    report_native_result(native_process_run(&result), &result);
}

static void filesystem_error(const char *command, enum phipfs_status status)
{
    console_write(command);
    console_write(": ");
    console_write(phipfs_status_string(status));
    console_putc('\n');
}

static bool filesystem_path(const char *argument, char *output)
{
    char combined[PHIPFS_MAX_PATH + 1U];
    size_t used = 0U;
    size_t index = 0U;
    size_t output_used = 0U;
    size_t component_starts[PHIPFS_MAX_DEPTH];
    size_t depth = 0U;

    if (argument == NULL || output == NULL || argument[0] == '/') {
        return false;
    }
    if (filesystem_cwd[0] != '.' || filesystem_cwd[1] != '\0') {
        while (filesystem_cwd[used] != '\0' && used < PHIPFS_MAX_PATH) {
            combined[used] = filesystem_cwd[used];
            ++used;
        }
        if (argument[0] != '\0' && used < PHIPFS_MAX_PATH) {
            combined[used++] = '/';
        }
    }
    while (argument[index] != '\0' && used < PHIPFS_MAX_PATH) {
        combined[used++] = argument[index++];
    }
    if (argument[index] != '\0' || used == 0U) {
        return false;
    }
    combined[used] = '\0';
    index = 0U;
    while (index < used) {
        size_t end = index;

        while (end < used && combined[end] != '/') {
            ++end;
        }
        if (end == index) {
            return false;
        }
        if (end - index == 1U && combined[index] == '.') {
            /* Current directory is a no-op. */
        } else if (end - index == 2U && combined[index] == '.' &&
                combined[index + 1U] == '.') {
            if (depth == 0U) {
                return false;
            }
            output_used = component_starts[--depth];
            if (output_used != 0U && output[output_used - 1U] == '/') {
                --output_used;
            }
        } else {
            if (depth >= PHIPFS_MAX_DEPTH) {
                return false;
            }
            if (output_used != 0U) {
                if (output_used >= PHIPFS_MAX_PATH) {
                    return false;
                }
                output[output_used++] = '/';
            }
            component_starts[depth++] = output_used;
            for (size_t source = index; source < end; ++source) {
                if (output_used >= PHIPFS_MAX_PATH) {
                    return false;
                }
                output[output_used++] = combined[source];
            }
        }
        index = end + 1U;
    }
    if (output_used == 0U) {
        output[0] = '.';
        output[1] = '\0';
    } else {
        output[output_used] = '\0';
    }
    return true;
}

static bool first_argument(
    const char *arguments,
    char *first,
    const char **remainder
)
{
    size_t length = 0U;
    size_t index = 0U;

    if (arguments == NULL || first == NULL || remainder == NULL) {
        return false;
    }
    while (is_separator(arguments[index])) {
        ++index;
    }
    while (arguments[index] != '\0' &&
        !is_separator(arguments[index])) {
        if (length >= PHIPFS_MAX_PATH) {
            return false;
        }
        first[length++] = arguments[index++];
    }
    if (length == 0U) {
        return false;
    }
    first[length] = '\0';
    while (is_separator(arguments[index])) {
        ++index;
    }
    *remainder = &arguments[index];
    return true;
}

static bool decimal_u32(const char *text, uint32_t *value)
{
    uint32_t result = 0U;
    size_t index = 0U;

    if (text == NULL || value == NULL || text[0] == '\0') {
        return false;
    }
    while (text[index] != '\0') {
        uint32_t digit;

        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
        digit = (uint32_t)(text[index] - '0');
        if (result > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        result = result * 10U + digit;
        ++index;
    }
    *value = result;
    return true;
}

static bool line_content(
    const char *text,
    uint8_t *content,
    size_t capacity,
    size_t *length
)
{
    size_t start = 0U;
    size_t end;

    if (text == NULL || content == NULL || length == NULL) {
        return false;
    }
    end = 0U;
    while (text[end] != '\0') {
        ++end;
    }
    if (end >= 2U && text[0] == '"' && text[end - 1U] == '"') {
        start = 1U;
        --end;
    } else if ((end != 0U && text[0] == '"') ||
            (end != 0U && text[end - 1U] == '"')) {
        return false;
    }
    if (end - start + 1U > capacity) {
        return false;
    }
    copy_bytes(content, (const uint8_t *)&text[start], end - start);
    content[end - start] = (uint8_t)'\n';
    *length = end - start + 1U;
    return true;
}

static void print_drive(const char *name, struct phipfs_drive_info drive)
{
    console_write(name);
    console_write("  fat32  ");
    if (!drive.present) {
        console_write("absent\n");
    } else if (!drive.healthy || !drive.mounted) {
        console_write("unavailable\n");
    } else {
        console_write(drive.read_only ? "read-only" : "read-write");
        console_write("  ");
        print_size(drive.free_bytes);
        console_write(" free\n");
    }
}

static void command_drives(void)
{
    print_drive("system", phipfs_drive(PHIPFS_VOLUME_SYSTEM));
    print_drive("data  ", phipfs_drive(PHIPFS_VOLUME_DATA));
}

static void command_mount(const char *arguments)
{
    enum phipfs_volume first = PHIPFS_VOLUME_SYSTEM;
    enum phipfs_volume last = PHIPFS_VOLUME_DATA;

    if (argument_equals(arguments, "system")) {
        last = PHIPFS_VOLUME_SYSTEM;
    } else if (argument_equals(arguments, "data")) {
        first = PHIPFS_VOLUME_DATA;
    } else if (arguments[0] != '\0') {
        console_write("mount: use 'mount system' or 'mount data'\n");
        return;
    }
    for (enum phipfs_volume volume = first; volume <= last;
         volume = (enum phipfs_volume)(volume + 1)) {
        struct phipfs_drive_info drive = phipfs_drive(volume);
        enum phipfs_status status;

        if (drive.mounted) {
            continue;
        }
        status = phipfs_mount(volume);
        if (status != PHIPFS_STATUS_OK) {
            filesystem_error(volume == PHIPFS_VOLUME_SYSTEM ?
                "mount system" : "mount data", status);
        }
    }
    command_drives();
}

static void command_pwd(void)
{
    console_putc('/');
    if (filesystem_cwd[0] != '.' || filesystem_cwd[1] != '\0') {
        console_write(filesystem_cwd);
    }
    console_putc('\n');
}

static void command_cd(const char *arguments)
{
    char path[PHIPFS_MAX_PATH + 1U];
    struct phipfs_stat stat;
    enum phipfs_status status;

    if (!filesystem_path(arguments[0] == '\0' ? "." : arguments, path)) {
        console_write("cd: malformed path\n");
        return;
    }
    status = phipfs_stat_path(PHIPFS_VOLUME_DATA, path, &stat);
    if (status != PHIPFS_STATUS_OK) {
        filesystem_error("cd", status);
        return;
    }
    if (!stat.directory) {
        filesystem_error("cd", PHIPFS_STATUS_NOT_DIRECTORY);
        return;
    }
    size_t index = 0U;
    do {
        filesystem_cwd[index] = path[index];
    } while (path[index++] != '\0');
}

static void command_ls(const char *arguments)
{
    char path[PHIPFS_MAX_PATH + 1U];
    struct phipfs_list_entry entries[PHIPFS_MAX_LIST_ENTRIES];
    size_t count = 0U;
    enum phipfs_status status;

    if (!filesystem_path(arguments[0] == '\0' ? "." : arguments, path)) {
        console_write("ls: malformed path\n");
        return;
    }
    status = phipfs_list(PHIPFS_VOLUME_DATA, path, entries,
        PHIPFS_MAX_LIST_ENTRIES, &count);
    if (status != PHIPFS_STATUS_OK) {
        filesystem_error("ls", status);
        return;
    }
    for (size_t index = 0U; index < count; ++index) {
        console_write(entries[index].directory ? "d  " : "-  ");
        console_write(entries[index].name);
        if (!entries[index].directory) {
            console_write("  ");
            console_write_u64(entries[index].size);
        }
        console_putc('\n');
    }
}

static void command_mkdir(const char *arguments)
{
    char path[PHIPFS_MAX_PATH + 1U];
    enum phipfs_status status;

    if (arguments[0] == '\0' || !filesystem_path(arguments, path)) {
        console_write("mkdir: provide one relative 8.3 path\n");
        return;
    }
    status = phipfs_mkdir(PHIPFS_VOLUME_DATA, path);
    if (status != PHIPFS_STATUS_OK) {
        filesystem_error("mkdir", status);
    }
}

static void command_touch(const char *arguments)
{
    char path[PHIPFS_MAX_PATH + 1U];
    struct phipfs_stat stat;
    enum phipfs_status status;

    if (arguments[0] == '\0' || !filesystem_path(arguments, path)) {
        console_write("touch: provide one relative 8.3 path\n");
        return;
    }
    status = phipfs_stat_path(PHIPFS_VOLUME_DATA, path, &stat);
    if (status == PHIPFS_STATUS_OK) {
        if (stat.directory) {
            filesystem_error("touch", PHIPFS_STATUS_IS_DIRECTORY);
        }
        return;
    }
    if (status != PHIPFS_STATUS_NOT_FOUND) {
        filesystem_error("touch", status);
        return;
    }
    status = phipfs_create(PHIPFS_VOLUME_DATA, path);
    if (status != PHIPFS_STATUS_OK) {
        filesystem_error("touch", status);
    }
}

static void command_read(const char *arguments)
{
    char path[PHIPFS_MAX_PATH + 1U];
    uint8_t buffer[128];
    phipfs_handle handle;
    enum phipfs_status status;

    if (arguments[0] == '\0' || !filesystem_path(arguments, path)) {
        console_write("read: provide one relative 8.3 path\n");
        return;
    }
    status = phipfs_open(PHIPFS_VOLUME_DATA, path, PHIPFS_ACCESS_READ, &handle);
    if (status != PHIPFS_STATUS_OK) {
        filesystem_error("read", status);
        return;
    }
    for (;;) {
        size_t read_bytes = 0U;

        status = phipfs_read(handle, buffer, sizeof(buffer), &read_bytes);
        if (read_bytes != 0U) {
            console_write_n((const char *)buffer, read_bytes);
        }
        if (status != PHIPFS_STATUS_OK || read_bytes == 0U) {
            break;
        }
    }
    if (phipfs_close(handle) != PHIPFS_STATUS_OK && status == PHIPFS_STATUS_OK) {
        status = PHIPFS_STATUS_STALE_HANDLE;
    }
    if (status != PHIPFS_STATUS_OK) {
        filesystem_error("read", status);
    }
}

static void command_write_line(const char *arguments, bool append)
{
    char argument_path[PHIPFS_MAX_PATH + 1U];
    char path[PHIPFS_MAX_PATH + 1U];
    const char *text;
    uint8_t content[SHELL_LINE_LIMIT + 1U];
    size_t content_bytes;
    size_t written = 0U;
    phipfs_handle handle;
    bool opened = false;
    enum phipfs_status status;

    if (!first_argument(arguments, argument_path, &text) ||
        !filesystem_path(argument_path, path) ||
        !line_content(text, content, sizeof(content), &content_bytes)) {
        console_write(append ?
            "append: use append PATH \"text\"\n" :
            "write: use write PATH \"text\"\n");
        return;
    }
    struct phipfs_stat stat;
    status = phipfs_stat_path(PHIPFS_VOLUME_DATA, path, &stat);
    if (status == PHIPFS_STATUS_NOT_FOUND) {
        status = phipfs_create(PHIPFS_VOLUME_DATA, path);
    }
    if (status == PHIPFS_STATUS_OK && !append) {
        status = phipfs_truncate(PHIPFS_VOLUME_DATA, path, 0U);
    }
    if (status == PHIPFS_STATUS_OK) {
        status = phipfs_open(PHIPFS_VOLUME_DATA, path,
            PHIPFS_ACCESS_WRITE, &handle);
        opened = status == PHIPFS_STATUS_OK;
    }
    if (status == PHIPFS_STATUS_OK && append) {
        uint64_t position;

        status = phipfs_seek(handle, 0, PHIPFS_SEEK_END, &position);
    }
    if (status == PHIPFS_STATUS_OK) {
        status = phipfs_write(handle, content, content_bytes, &written);
    }
    if (opened && phipfs_close(handle) != PHIPFS_STATUS_OK &&
        status == PHIPFS_STATUS_OK) {
        status = PHIPFS_STATUS_STALE_HANDLE;
    }
    if (status != PHIPFS_STATUS_OK || written != content_bytes) {
        filesystem_error(append ? "append" : "write",
            status != PHIPFS_STATUS_OK ? status : PHIPFS_STATUS_WRITEBACK);
    }
}

static void command_write_at(const char *arguments)
{
    char argument_path[PHIPFS_MAX_PATH + 1U];
    char argument_offset[PHIPFS_MAX_PATH + 1U];
    char path[PHIPFS_MAX_PATH + 1U];
    const char *after_path;
    const char *text;
    uint8_t content[SHELL_LINE_LIMIT + 1U];
    size_t content_bytes;
    size_t written = 0U;
    uint32_t offset;
    uint64_t position = 0U;
    phipfs_handle handle;
    bool opened = false;
    enum phipfs_status status;

    if (!first_argument(arguments, argument_path, &after_path) ||
        !first_argument(after_path, argument_offset, &text) ||
        !filesystem_path(argument_path, path) ||
        !decimal_u32(argument_offset, &offset) ||
        !line_content(text, content, sizeof(content), &content_bytes)) {
        console_write("writeat: use writeat PATH OFFSET \"text\"\n");
        return;
    }
    status = phipfs_open(PHIPFS_VOLUME_DATA, path,
        PHIPFS_ACCESS_WRITE, &handle);
    opened = status == PHIPFS_STATUS_OK;
    if (status == PHIPFS_STATUS_OK) {
        status = phipfs_seek(handle, (int64_t)offset,
            PHIPFS_SEEK_START, &position);
    }
    if (status == PHIPFS_STATUS_OK) {
        status = phipfs_write(handle, content, content_bytes, &written);
    }
    if (opened && phipfs_close(handle) != PHIPFS_STATUS_OK &&
        status == PHIPFS_STATUS_OK) {
        status = PHIPFS_STATUS_STALE_HANDLE;
    }
    if (status != PHIPFS_STATUS_OK || position != offset ||
        written != content_bytes) {
        filesystem_error("writeat", status != PHIPFS_STATUS_OK ? status :
            PHIPFS_STATUS_WRITEBACK);
    }
}

static void command_truncate(const char *arguments)
{
    char argument_path[PHIPFS_MAX_PATH + 1U];
    char path[PHIPFS_MAX_PATH + 1U];
    const char *size_text;
    uint32_t size;
    enum phipfs_status status;

    if (!first_argument(arguments, argument_path, &size_text) ||
        !filesystem_path(argument_path, path) ||
        !decimal_u32(size_text, &size)) {
        console_write("truncate: use truncate PATH BYTES\n");
        return;
    }
    status = phipfs_truncate(PHIPFS_VOLUME_DATA, path, size);
    if (status != PHIPFS_STATUS_OK) {
        filesystem_error("truncate", status);
    }
}

static void command_stat(const char *arguments)
{
    char path[PHIPFS_MAX_PATH + 1U];
    struct phipfs_stat stat;
    enum phipfs_status status;

    if (arguments[0] == '\0' || !filesystem_path(arguments, path)) {
        console_write("stat: provide one relative 8.3 path\n");
        return;
    }
    status = phipfs_stat_path(PHIPFS_VOLUME_DATA, path, &stat);
    if (status != PHIPFS_STATUS_OK) {
        filesystem_error("stat", status);
        return;
    }
    console_write(stat.directory ? "directory  " : "file       ");
    console_write_u64(stat.size);
    console_write(" bytes  cluster ");
    console_write_u64(stat.first_cluster);
    console_write("  chain ");
    console_write_u64(stat.cluster_count);
    console_write(stat.read_only ? "  read-only\n" : "  read-write\n");
}

static void command_mv(const char *arguments)
{
    char first[PHIPFS_MAX_PATH + 1U];
    char source[PHIPFS_MAX_PATH + 1U];
    char destination[PHIPFS_MAX_PATH + 1U];
    const char *second;
    enum phipfs_status status;

    if (!first_argument(arguments, first, &second) || second[0] == '\0' ||
        !filesystem_path(first, source) ||
        !filesystem_path(second, destination)) {
        console_write("mv: use mv SOURCE DESTINATION\n");
        return;
    }
    status = phipfs_rename(PHIPFS_VOLUME_DATA, source, destination);
    if (status != PHIPFS_STATUS_OK) {
        filesystem_error("mv", status);
    }
}

static void command_rm(const char *arguments)
{
    char path[PHIPFS_MAX_PATH + 1U];
    struct phipfs_stat stat;
    enum phipfs_status status;

    if (arguments[0] == '\0' || !filesystem_path(arguments, path)) {
        console_write("rm: provide one relative 8.3 path\n");
        return;
    }
    status = phipfs_stat_path(PHIPFS_VOLUME_DATA, path, &stat);
    if (status == PHIPFS_STATUS_OK) {
        status = stat.directory ? phipfs_rmdir(PHIPFS_VOLUME_DATA, path) :
            phipfs_unlink(PHIPFS_VOLUME_DATA, path);
    }
    if (status != PHIPFS_STATUS_OK) {
        filesystem_error("rm", status);
    }
}

static void command_sync(void)
{
    enum phipfs_status status = phipfs_sync(PHIPFS_VOLUME_DATA);

    if (status == PHIPFS_STATUS_OK) {
        console_write("data synchronized\n");
    } else {
        filesystem_error("sync", status);
    }
}

static void command_reboot(void)
{
    enum phipfs_status status = phipfs_unmount(PHIPFS_VOLUME_DATA);

    if (status != PHIPFS_STATUS_OK && status != PHIPFS_STATUS_NOT_MOUNTED) {
        filesystem_error("reboot", status);
        return;
    }
    status = phipfs_unmount(PHIPFS_VOLUME_SYSTEM);
    if (status != PHIPFS_STATUS_OK && status != PHIPFS_STATUS_NOT_MOUNTED) {
        filesystem_error("reboot", status);
        (void)phipfs_mount(PHIPFS_VOLUME_DATA);
        return;
    }
    console_write("restarting after clean synchronization\n");
    cpu_interrupt_disable();
    cpu_out8(UINT16_C(0x0064), UINT8_C(0xFE));
    cpu_interrupt_enable();
    console_write("reboot: platform reset failed\n");
    (void)phipfs_mount(PHIPFS_VOLUME_SYSTEM);
    (void)phipfs_mount(PHIPFS_VOLUME_DATA);
}

static void command_uptime(void)
{
    const uint64_t now = clock_monotonic_ns();

    console_write_u64(now);
    console_write(" ns (");
    console_write_u64(now / UINT64_C(1000000));
    console_write(" ms)\n");
}

static void command_mem(void)
{
    const struct frame_allocator_stats frames = frame_allocator_get_stats();
    const struct heap_state heap = heap_get_state();

    console_write("frames  free ");
    console_write_u64(frames.free_frames);
    console_write(" of ");
    console_write_u64(frames.allocatable_frames);
    console_write(" allocatable, ");
    console_write_u64(frames.reserved_frames);
    console_write(" reserved\n");

    console_write("heap    ");
    print_size(heap.allocated_bytes);
    console_write(" live in ");
    console_write_u64(heap.live_allocations);
    console_write(" allocations, ");
    print_size(heap.committed_bytes);
    console_write(" committed of ");
    print_size(heap.size);
    console_putc('\n');
}

static void command_pci(void)
{
    const size_t count = pci_function_count();

    for (size_t index = 0; index < count; ++index) {
        const struct pci_function *function = pci_function_at(index);

        if (function == NULL) {
            continue;
        }

        console_write_u64(function->address.bus);
        console_putc(':');
        console_write_u64(function->address.device);
        console_putc('.');
        console_write_u64(function->address.function);
        console_write("  ");
        console_write_hex(function->vendor_id);
        console_putc(':');
        console_write_hex(function->device_id);
        console_write("  ");
        console_write(pci_class_string(function->class_code));
        console_putc('\n');
    }

    console_write_u64(count);
    console_write(" functions\n");
}

static void command_keys(void)
{
    const struct keyboard_state keyboard = keyboard_get_state();

    console_write("interrupts ");
    console_write_u64(keyboard.interrupts);
    console_write("  events ");
    console_write_u64(keyboard.events);
    console_write("  dropped ");
    console_write_u64(keyboard.dropped);
    console_write("  waiting ");
    console_write_u64(keyboard.queued);
    console_putc('\n');
}

static void command_threads(void)
{
    const struct thread_system_state threads = thread_get_state();

    console_write("switches ");
    console_write_u64(threads.switches);
    console_write("  preemptions ");
    console_write_u64(threads.preemptions);
    console_write("  preemptive ");
    console_write(threads.preemptive ? "yes" : "no");
    console_putc('\n');
}

static void command_version(void)
{
    const struct screen_state screen = screen_get_state();

    console_write("Phipia 2.2.0 dev, a proof-driven x86_64 operating system.\n");
    console_write("console ");
    console_write_u64(screen.columns);
    console_putc('x');
    console_write_u64(screen.rows);
    console_write(" characters\n");
}

static void print_fetch_drive(struct phipfs_drive_info drive)
{
    if (!drive.present || !drive.healthy || !drive.mounted) {
        console_write("unavailable");
    } else {
        console_write(drive.read_only ? "fat32 ro" : "fat32 rw");
    }
}

static void command_fetch(void)
{
    const struct screen_state screen = screen_get_state();
    const struct heap_state heap = heap_get_state();
    const struct phipfs_drive_info system = phipfs_drive(PHIPFS_VOLUME_SYSTEM);
    const struct phipfs_drive_info data = phipfs_drive(PHIPFS_VOLUME_DATA);

    console_write("\n");
    if (ui_terminal_draw_logo() != UI_STATUS_OK) {
        console_write("  [ Phipia ]\n");
    }
    console_write("\n");
    console_write("  Phipia\n");
    console_write("  kernel      Phipia 2.2.0 dev / x86_64\n");
    console_write("  terminal    ");
    console_write_u64(screen.columns);
    console_putc('x');
    console_write_u64(screen.rows);
    console_write(" cells\n");
    console_write("  filesystem  system ");
    print_fetch_drive(system);
    console_write(" / data ");
    print_fetch_drive(data);
    console_putc('\n');
    console_write("  heap        ");
    print_size(heap.allocated_bytes);
    console_write(" allocated\n\n");
}

static void command_ledger(void)
{
    const struct boot_ledger *ledger = boot_ledger_installed();

    if (ledger == NULL || !ledger->executed) {
        console_write("boot ledger :: unavailable\n");
        return;
    }

    console_write("boot ledger :: ");
    console_write(ledger->degraded ? "DEGRADED" : "PASS");
    console_putc('\n');
    console_write("plan ");
    console_write_u64(ledger->planned_count);
    console_write("  run ");
    console_write_u64(ledger->executed_count);
    console_write("  skip ");
    console_write_u64(ledger->optional_skip_count);
    console_write("  caps ");
    console_write_u64(ledger->established_capability_count);
    console_write("  receipts ");
    console_write_u64(ledger->receipt_count);
    console_putc('\n');
    console_write("fingerprint ");
    console_write_hex(ledger->fingerprint);
    console_putc('\n');
}

static void print_ipv4(uint32_t address)
{
    char text[16];

    network_format_ipv4(address, text);
    console_write(text);
}

static void print_mac(const uint8_t mac[6])
{
    for (size_t index = 0U; index < 6U; ++index) {
        static const char digits[] = "0123456789abcdef";

        console_putc(digits[mac[index] >> 4U]);
        console_putc(digits[mac[index] & UINT8_C(0x0f)]);
        if (index != 5U) {
            console_putc(':');
        }
    }
}

static void command_network(void)
{
    const struct network_state network = network_get_state();

    if (!network.device.present) {
        console_write("virtio-net0  unavailable\n");
        return;
    }
    console_write("virtio-net0  ");
    console_write(network.device.link_up ? "link up\n" : "link down\n");
    console_write("mac          ");
    print_mac(network.device.mac);
    console_putc('\n');
    if (!network.configuration.configured) {
        console_write("ipv4         unconfigured\n");
        return;
    }
    console_write("ipv4         ");
    print_ipv4(network.configuration.address);
    console_putc('\n');
    console_write("gateway      ");
    print_ipv4(network.configuration.gateway);
    console_putc('\n');
    console_write("dns          ");
    print_ipv4(network.configuration.dns_server);
    console_putc('\n');
    console_write("source       ");
    console_write(network.configuration.source == NETWORK_CONFIGURATION_DHCP ?
        "dhcp\n" : "static\n");
}

static void network_error(const char *operation, enum network_status status)
{
    console_write(operation);
    console_write(": ");
    console_write(network_status_string(status));
    console_putc('\n');
}

static void command_dhcp(void)
{
    const enum network_status status = network_start_dhcp(
        NETWORK_DEFAULT_OPERATION_TIMEOUT_NS);

    if (status != NETWORK_STATUS_OK) {
        network_error("dhcp", status);
        return;
    }
    command_network();
}

static void command_ip(const char *arguments)
{
    char address_text[PHIPFS_MAX_PATH + 1U];
    char mask_text[PHIPFS_MAX_PATH + 1U];
    char gateway_text[PHIPFS_MAX_PATH + 1U];
    char dns_text[PHIPFS_MAX_PATH + 1U];
    const char *remainder;
    uint32_t address;
    uint32_t mask;
    uint32_t gateway;
    uint32_t dns;

    if (arguments[0] == '\0') {
        command_network();
        return;
    }
    if (!first_argument(arguments, address_text, &remainder) ||
        !first_argument(remainder, mask_text, &remainder) ||
        !first_argument(remainder, gateway_text, &remainder) ||
        !first_argument(remainder, dns_text, &remainder) ||
        remainder[0] != '\0' ||
        !network_parse_ipv4(address_text, &address) ||
        !network_parse_ipv4(mask_text, &mask) ||
        !network_parse_ipv4(gateway_text, &gateway) ||
        !network_parse_ipv4(dns_text, &dns)) {
        console_write("ip: use 'ip ADDRESS MASK GATEWAY DNS'\n");
        return;
    }
    const enum network_status status = network_configure_static(address, mask,
        gateway, dns);

    if (status != NETWORK_STATUS_OK) {
        network_error("ip", status);
    } else {
        command_network();
    }
}

static void command_arp(void)
{
    const struct network_state network = network_get_state();

    console_write_u64(network.arp_entries);
    console_write(" authenticated ARP entr");
    console_write(network.arp_entries == 1U ? "y\n" : "ies\n");
    console_write("conflicts    ");
    console_write_u64(network.statistics.arp_conflicts);
    console_putc('\n');
}

static void command_ping(const char *arguments)
{
    char address_text[PHIPFS_MAX_PATH + 1U];
    const char *remainder;
    uint32_t address;
    uint32_t count = 3U;
    struct network_ping_result result;
    enum network_status status;

    if (!first_argument(arguments, address_text, &remainder) ||
        !network_parse_ipv4(address_text, &address) ||
        (remainder[0] != '\0' && !decimal_u32(remainder, &count)) ||
        count == 0U || count > NETWORK_PING_MAX_COUNT) {
        console_write("ping: use 'ping ADDRESS [1-8]'\n");
        return;
    }
    status = network_ping(address, count, UINT64_C(1000000000), &result);
    for (uint32_t index = 0U; index < count; ++index) {
        if (result.result[index] == NETWORK_STATUS_OK) {
            console_write("reply seq=");
            console_write_u64(index + 1U);
            console_write(" time=");
            console_write_u64(result.round_trip_ns[index] / UINT64_C(1000));
            console_write(" us\n");
        }
    }
    console_write_u64(result.sent);
    console_write(" sent, ");
    console_write_u64(result.received);
    console_write(" received\n");
    if (status != NETWORK_STATUS_OK && result.received == 0U) {
        network_error("ping", status);
    }
}

static void command_resolve(const char *arguments)
{
    char hostname[PHIPFS_MAX_PATH + 1U];
    const char *remainder;
    uint32_t address;
    enum network_status status;

    if (!first_argument(arguments, hostname, &remainder) ||
        remainder[0] != '\0') {
        console_write("resolve: use 'resolve HOSTNAME'\n");
        return;
    }
    status = network_resolve(hostname, &address,
        NETWORK_DEFAULT_OPERATION_TIMEOUT_NS);
    if (status != NETWORK_STATUS_OK) {
        network_error("resolve", status);
        return;
    }
    print_ipv4(address);
    console_putc('\n');
}

static void command_http(const char *arguments)
{
    char url[PHIPFS_MAX_PATH + 1U];
    char path[PHIPFS_MAX_PATH + 1U];
    const char *remainder;
    struct network_http_result result;
    enum network_status status;

    if (!first_argument(arguments, url, &remainder) ||
        !first_argument(remainder, path, &remainder) ||
        remainder[0] != '\0') {
        console_write("http: use 'http URL DATA-PATH'\n");
        return;
    }
    status = network_http_download(SHELL_NETWORK_OWNER, url, path,
        false, UINT64_C(15000000000), &result);
    if (status != NETWORK_STATUS_OK) {
        network_error("http", status);
        return;
    }
    console_write_u64(result.status_code);
    console_write(" HTTP response\nsaved ");
    console_write(path);
    console_putc('\n');
    console_write_u64(result.body_bytes);
    console_write(" bytes synchronized\n");
}

static void command_netstat(void)
{
    const struct network_state network = network_get_state();

    console_write("udp ");
    console_write_u64(network.udp_sockets);
    console_write("/8  tcp ");
    console_write_u64(network.tcp_connections);
    console_write("/8  timers ");
    console_write_u64(network.timers);
    console_write("/32\nrx ");
    console_write_u64(network.device.statistics.rx_frames);
    console_write("  tx ");
    console_write_u64(network.device.statistics.tx_frames);
    console_write("  malformed ");
    console_write_u64(network.statistics.malformed_packets);
    console_putc('\n');
    console_write("ethernet ");
    console_write_u64(network.statistics.ethernet_accepted);
    console_write("  ipv4 ");
    console_write_u64(network.statistics.ipv4_accepted);
    console_write("  ipv4-checksum-fail ");
    console_write_u64(network.statistics.ipv4_checksum_failures);
    console_write("  udp ");
    console_write_u64(network.statistics.udp_accepted);
    console_putc('\n');
}

enum shell_status shell_execute(const char *text)
{
    size_t start = 0U;

    if (text == NULL) {
        return SHELL_STATUS_BAD_ARGUMENT;
    }

    while (text[start] != '\0' && is_separator(text[start])) {
        start += 1U;
    }

    text = &text[start];
    state.lines += 1U;
    linux_prompt_evidence_pending = false;

    /* An empty line is not a mistake and is not a command. */
    if (text[0] == '\0') {
        return SHELL_STATUS_OK;
    }

    if (matches(text, "help")) {
        command_help();
    } else if (matches(text, "echo")) {
        command_echo(arguments_of(text));
    } else if (matches(text, "linux")) {
        linux_prompt_evidence_pending = true;
        command_linux(arguments_of(text));
    } else if (matches(text, "native-start")) {
        command_native_start(arguments_of(text));
    } else if (matches(text, "native-go")) {
        command_native_go();
    } else if (matches(text, "native")) {
        command_native(arguments_of(text));
    } else if (matches(text, "drives")) {
        command_drives();
    } else if (matches(text, "mount")) {
        command_mount(arguments_of(text));
    } else if (matches(text, "ls")) {
        command_ls(arguments_of(text));
    } else if (matches(text, "cd")) {
        command_cd(arguments_of(text));
    } else if (matches(text, "pwd")) {
        command_pwd();
    } else if (matches(text, "mkdir")) {
        command_mkdir(arguments_of(text));
    } else if (matches(text, "touch")) {
        command_touch(arguments_of(text));
    } else if (matches(text, "read")) {
        command_read(arguments_of(text));
    } else if (matches(text, "write")) {
        command_write_line(arguments_of(text), false);
    } else if (matches(text, "append")) {
        command_write_line(arguments_of(text), true);
    } else if (matches(text, "writeat")) {
        command_write_at(arguments_of(text));
    } else if (matches(text, "truncate")) {
        command_truncate(arguments_of(text));
    } else if (matches(text, "stat")) {
        command_stat(arguments_of(text));
    } else if (matches(text, "mv")) {
        command_mv(arguments_of(text));
    } else if (matches(text, "rm")) {
        command_rm(arguments_of(text));
    } else if (matches(text, "sync")) {
        command_sync();
    } else if (matches(text, "network")) {
        command_network();
    } else if (matches(text, "dhcp")) {
        command_dhcp();
    } else if (matches(text, "ip")) {
        command_ip(arguments_of(text));
    } else if (matches(text, "arp")) {
        command_arp();
    } else if (matches(text, "ping")) {
        command_ping(arguments_of(text));
    } else if (matches(text, "resolve")) {
        command_resolve(arguments_of(text));
    } else if (matches(text, "http")) {
        command_http(arguments_of(text));
    } else if (matches(text, "netstat")) {
        command_netstat();
    } else if (matches(text, "reboot")) {
        command_reboot();
    } else if (matches(text, "clear")) {
        if (screen_is_active()) {
            (void)screen_clear();
        }
    } else if (matches(text, "fetch")) {
        command_fetch();
    } else if (matches(text, "uptime")) {
        command_uptime();
    } else if (matches(text, "mem")) {
        command_mem();
    } else if (matches(text, "pci")) {
        command_pci();
    } else if (matches(text, "keys")) {
        command_keys();
    } else if (matches(text, "threads")) {
        command_threads();
    } else if (matches(text, "ledger")) {
        command_ledger();
    } else if (matches(text, "version")) {
        command_version();
    } else {
        state.unknown += 1U;
        console_write("no such command: ");
        console_write(text);
        console_write("\n");
        return SHELL_STATUS_UNKNOWN_COMMAND;
    }

    state.commands += 1U;
    return SHELL_STATUS_OK;
}

static void write_prompt_restored(void)
{
    console_write(SHELL_PROMPT);
    if (linux_prompt_evidence_pending) {
        console_serial_write("\nRW USERLAND Phipia prompt restored\n");
        console_serial_write(SHELL_PROMPT);
        linux_prompt_evidence_pending = false;
    }
}

static void foreground_release(void)
{
    zero_bytes(&foreground, sizeof(foreground));
}

static void foreground_refusal(const char *message)
{
    console_putc('\n');
    console_write("linux cat: ");
    console_write(message);
    console_putc('\n');
    if (foreground.length != 0U) {
        console_write_n((const char *)foreground.line,
            foreground.length);
    }
}

static void foreground_fail(enum linux_userland_status status)
{
    console_write("linux cat: ");
    console_write(linux_userland_status_string(status));
    console_putc('\n');
    (void)linux_userland_abort_foreground();
    foreground_release();
    write_prompt_restored();
}

static void foreground_deliver(bool eof)
{
    struct linux_userland_result result;
    enum linux_userland_status status;
    size_t byte_count = 0U;

    if (!eof) {
        foreground.line[foreground.length] = (uint8_t)'\n';
        byte_count = foreground.length + 1U;
        console_putc('\n');
        console_serial_write(
            "RW CAT terminal input accepted through keyboard events bytes ");
        console_serial_write_u64(byte_count);
        console_serial_write("\n");
    } else {
        console_write("^D\n");
    }
    status = linux_userland_deliver_cat_input(
        eof ? NULL : foreground.line, byte_count, eof, &result);
    if (status == LINUX_USERLAND_STATUS_WAITING) {
        foreground.length = 0U;
        foreground.line[0] = 0U;
        foreground.delivered_lines = result.input_lines;
        foreground.delivered_bytes = result.input_bytes;
        foreground.overflow_notified = false;
        return;
    }
    if (status == LINUX_USERLAND_STATUS_OK && result.teardown_complete &&
        result.eof_delivered) {
        foreground_release();
        write_prompt_restored();
        return;
    }
    foreground_fail(status);
}

static bool foreground_handle_event(const struct keyboard_event *event)
{
    if (!foreground.active || event == NULL) {
        return false;
    }
    if (!linux_userland_foreground_waiting() ||
        foreground.generation != linux_userland_active_generation()) {
        foreground_fail(LINUX_USERLAND_STATUS_INPUT_REFUSED);
        return true;
    }
    if (!event->pressed) {
        return true;
    }
    if (event->control && event->scancode == UINT8_C(0x20)) {
        if (foreground.length != 0U) {
            foreground_refusal("Ctrl-D needs an empty current line");
            return true;
        }
        foreground_deliver(true);
        return true;
    }
    if (event->character == '\b') {
        if (foreground.length != 0U) {
            --foreground.length;
            foreground.line[foreground.length] = 0U;
            foreground.overflow_notified = false;
            console_putc('\b');
            console_putc(' ');
            console_putc('\b');
        }
        return true;
    }
    if (event->character == '\n' || event->character == '\r') {
        const size_t byte_count = foreground.length + 1U;

        if (foreground.delivered_lines >= LINUX_CAT_INPUT_LINES) {
            foreground_refusal("four complete lines is the launch bound");
            return true;
        }
        if (byte_count > LINUX_CAT_INPUT_TOTAL_BYTES -
                foreground.delivered_bytes) {
            foreground_refusal("total input is limited to 256 bytes");
            return true;
        }
        foreground_deliver(false);
        return true;
    }
    if (event->character < ' ' || event->character > '~') {
        return true;
    }
    if (foreground.delivered_lines >= LINUX_CAT_INPUT_LINES) {
        if (!foreground.overflow_notified) {
            foreground_refusal("four complete lines is the launch bound");
            foreground.overflow_notified = true;
        }
        return true;
    }
    if (foreground.length >= LINUX_CAT_INPUT_LINE_BYTES) {
        if (!foreground.overflow_notified) {
            foreground_refusal("a line is limited to 64 printable bytes");
            foreground.overflow_notified = true;
        }
        return true;
    }
    foreground.line[foreground.length] = (uint8_t)event->character;
    ++foreground.length;
    console_putc(event->character);
    return true;
}

enum shell_status shell_feed(char character)
{
    enum shell_status status;

    if (!state.active) {
        return SHELL_STATUS_NOT_INITIALIZED;
    }

    if (character == '\n' || character == '\r') {
        console_putc('\n');
        line[state.length] = '\0';
        state.length = 0U;
        status = shell_execute(line);
        if (linux_userland_foreground_waiting()) {
            return status;
        }
        write_prompt_restored();
        return status;
    }

    if (character == '\b') {
        if (state.length == 0U) {
            return SHELL_STATUS_OK;
        }

        state.length -= 1U;

        /*
         * Erasing is three characters: step back, write a space over what was
         * there, step back again. A lone backspace moves the cursor and leaves
         * the character on the screen.
         */
        console_putc('\b');
        console_putc(' ');
        console_putc('\b');
        return SHELL_STATUS_OK;
    }

    /* Anything the font cannot draw is not put in a line either. */
    if (character < ' ' || character > '~') {
        return SHELL_STATUS_OK;
    }

    if (state.length >= SHELL_LINE_LIMIT) {
        /*
         * Refused at the keystroke that would overflow, rather than truncated
         * when the line is run. A truncated command is a different command.
         */
        state.rejected += 1U;
        return SHELL_STATUS_LINE_TOO_LONG;
    }

    line[state.length] = character;
    state.length += 1U;
    console_putc(character);
    return SHELL_STATUS_OK;
}

enum shell_status shell_initialize(void)
{
    state.active = true;
    state.length = 0U;
    line[0] = '\0';
    filesystem_cwd[0] = '.';
    filesystem_cwd[1] = '\0';
    return SHELL_STATUS_OK;
}

bool shell_is_active(void)
{
    return state.active;
}

struct shell_state shell_get_state(void)
{
    return state;
}

void shell_process_keyboard_events(void)
{
    struct keyboard_event event;

    if (!ui_keyboard_decided) {
        ui_keyboard_operational = ui_is_active();
        ui_keyboard_decided = true;
    }
    while (keyboard_read(&event) == KEYBOARD_STATUS_OK) {
        if (foreground_handle_event(&event)) {
            continue;
        }
        if (!event.pressed) {
            continue;
        }
        if (ui_keyboard_operational &&
            ui_get_state()->active_panel != UI_PANEL_TERMINAL) {
            if (ui_handle_keyboard(&event) != UI_STATUS_OK) {
                ui_keyboard_operational = false;
            }
            continue;
        }
        if (ui_keyboard_operational &&
            (event.scancode == 0x0FU || event.scancode == 0x01U)) {
            if (ui_handle_keyboard(&event) != UI_STATUS_OK) {
                ui_keyboard_operational = false;
            }
            continue;
        }
        if (event.character != '\0' &&
            (!ui_keyboard_operational ||
                ui_get_state()->active_panel == UI_PANEL_TERMINAL)) {
            (void)shell_feed(event.character);
        }
    }
}

_Noreturn void shell_run(void)
{
    bool ui_operational = ui_is_active();

    if (!state.active) {
        (void)shell_initialize();
    }

    if (ui_operational) {
        ui_animation_attach();
    }

    console_write("\n");
    console_write(SHELL_PROMPT);

    for (;;) {
        ui_keyboard_operational = ui_operational;
        ui_keyboard_decided = true;
        shell_process_keyboard_events();
        (void)network_service();
        ui_operational = ui_keyboard_operational;

        if (ui_operational) {
            enum ui_status status = ui_process_events();

            if (status == UI_STATUS_OK) {
                status = ui_flush();
            }
            if (status != UI_STATUS_OK) {
                const struct framebuffer_state framebuffer =
                    framebuffer_get_state();

                ui_operational = false;
                (void)screen_set_deferred_present(false);
                (void)screen_set_viewport((struct surface_rect){
                    0U, 0U, framebuffer.width, framebuffer.height
                }, true);
                console_write("Phipia: runtime disabled: ");
                console_write(ui_status_string(status));
                console_putc('\n');
            }
        }
        if (ui_operational) {
            char manifest[PHIPFS_MAX_PATH + 1U];

            if (ui_application_launch_dequeue(manifest,
                    sizeof(manifest))) {
                struct native_process_result result;

                report_native_result(native_process_launch(manifest,
                    &result), &result);
            }
        }

        /*
         * A moving window is active rendering work: keep producing frames
         * until the monotonic-clock animation settles.  Once it is still,
         * the ordinary sti/hlt path below resumes immediately, so an idle
         * desktop never burns the only core.
         */
        if (ui_operational && ui_animation_active()) {
            continue;
        }

        /*
         * Halt rather than spin. keyboard_read does not block - there is no way
         * yet for an interrupt to wake a thread - so this waits the only way it
         * can: it stops the processor until an interrupt arrives, then looks
         * again.
         *
         * cpu_enable_and_halt is sti followed by hlt, and the pairing is the
         * point. sti does not take effect until after the instruction following
         * it, so no interrupt can be delivered in the gap between enabling and
         * halting - which is exactly the race that would otherwise leave the
         * machine asleep with a keystroke already waiting.
         */
        cpu_enable_and_halt();
    }
}

const char *shell_status_string(enum shell_status status)
{
    static const char *const messages[] = {
        "ok",
        "the shell is not initialized",
        "the line is longer than the shell will accept",
        "no such command",
        "bad argument"
    };

    _Static_assert(
        sizeof(messages) / sizeof(messages[0]) ==
            (size_t)SHELL_STATUS_BAD_ARGUMENT + 1U,
        "shell status messages are out of sync with enum shell_status"
    );

    if ((size_t)status >= sizeof(messages) / sizeof(messages[0])) {
        return "unknown shell status";
    }

    return messages[status];
}

/*
 * What the line editor and the dispatcher must get right, checked with no
 * keyboard, no screen, and nothing to type on.
 *
 * This is the half of a shell that has no hardware in it, and separating it
 * from the input loop is what makes it reachable at all. A shell tested by
 * pretending to type is a shell whose parser was never separated from its
 * input, and the pretending is the part that rots.
 */
static bool matching_is_right(void)
{
    /* A name matches itself and nothing longer or shorter. */
    if (!matches("help", "help")) {
        return false;
    }

    if (matches("hel", "help") || matches("helpme", "help")) {
        return false;
    }

    /* A name ends at a separator, and the arguments start after it. */
    if (!matches("echo hello", "echo")) {
        return false;
    }

    if (!matches("echo\thello", "echo")) {
        return false;
    }

    /*
     * A prefix must not match a longer command. This is the check that stops
     * "e" running "echo", which is the classic way a hand-written dispatcher
     * goes wrong.
     */
    if (matches("e", "echo")) {
        return false;
    }

    return true;
}

static bool argument_splitting_is_right(void)
{
    /* Everything after the word, with the separators between them removed. */
    if (!matches(arguments_of("echo hello"), "hello")) {
        return false;
    }

    if (arguments_of("echo")[0] != '\0') {
        return false;
    }

    if (arguments_of("echo   ")[0] != '\0') {
        return false;
    }

    /* Separators inside the arguments are the caller's, not the splitter's. */
    if (!matches(arguments_of("echo  a b"), "a")) {
        return false;
    }

    return true;
}

static bool line_editing_is_right(void)
{
    const struct shell_state before = shell_get_state();

    if (!state.active) {
        return false;
    }

    if (state.length != 0U) {
        return false;
    }

    /* Characters accumulate. */
    if (shell_feed('a') != SHELL_STATUS_OK ||
        shell_feed('b') != SHELL_STATUS_OK) {
        return false;
    }

    if (state.length != 2U) {
        return false;
    }

    /* Backspace removes one, and on an empty line removes nothing. */
    if (shell_feed('\b') != SHELL_STATUS_OK || state.length != 1U) {
        return false;
    }

    if (shell_feed('\b') != SHELL_STATUS_OK || state.length != 0U) {
        return false;
    }

    if (shell_feed('\b') != SHELL_STATUS_OK || state.length != 0U) {
        return false;
    }

    /*
     * A line at the limit refuses the keystroke that would overflow it, and
     * refuses every one after, and does not grow.
     */
    for (size_t index = 0; index < SHELL_LINE_LIMIT; ++index) {
        if (shell_feed('x') != SHELL_STATUS_OK) {
            return false;
        }
    }

    if (state.length != SHELL_LINE_LIMIT) {
        return false;
    }

    if (shell_feed('x') != SHELL_STATUS_LINE_TOO_LONG) {
        return false;
    }

    if (shell_feed('x') != SHELL_STATUS_LINE_TOO_LONG) {
        return false;
    }

    if (state.length != SHELL_LINE_LIMIT) {
        return false;
    }

    /* Clear it back out the way a person would. */
    for (size_t index = 0; index < SHELL_LINE_LIMIT; ++index) {
        if (shell_feed('\b') != SHELL_STATUS_OK) {
            return false;
        }
    }

    if (state.length != 0U) {
        return false;
    }

    /* Anything unprintable is neither buffered nor echoed. */
    if (shell_feed('\x01') != SHELL_STATUS_OK || state.length != 0U) {
        return false;
    }

    if (shell_feed('\x7F') != SHELL_STATUS_OK || state.length != 0U) {
        return false;
    }

    /* None of the above should have counted as a line. */
    return shell_get_state().lines == before.lines;
}

static bool dispatch_is_right(void)
{
    const struct shell_state before = shell_get_state();

    /* An empty line runs nothing and is not an error. */
    if (shell_execute("") != SHELL_STATUS_OK) {
        return false;
    }

    if (shell_execute("   ") != SHELL_STATUS_OK) {
        return false;
    }

    if (shell_get_state().commands != before.commands) {
        return false;
    }

    /* An unknown command is reported and counted, and does not stop anything. */
    if (shell_execute("nonesuch") != SHELL_STATUS_UNKNOWN_COMMAND) {
        return false;
    }

    if (shell_get_state().unknown != before.unknown + 1U) {
        return false;
    }

    /* A null line is refused rather than dereferenced. */
    if (shell_execute(NULL) != SHELL_STATUS_BAD_ARGUMENT) {
        return false;
    }

    /* Leading whitespace does not change which command a line names. */
    if (shell_execute("   echo") != SHELL_STATUS_OK) {
        return false;
    }

    return shell_get_state().lines == before.lines + 4U;
}

static bool refusals_are_named(void)
{
    static const enum shell_status every[] = {
        SHELL_STATUS_OK,
        SHELL_STATUS_NOT_INITIALIZED,
        SHELL_STATUS_LINE_TOO_LONG,
        SHELL_STATUS_UNKNOWN_COMMAND,
        SHELL_STATUS_BAD_ARGUMENT
    };

    for (size_t index = 0; index < sizeof(every) / sizeof(every[0]); ++index) {
        const char *message = shell_status_string(every[index]);

        if (message == NULL || message[0] == '\0') {
            return false;
        }
    }

    return shell_status_string((enum shell_status)99) != NULL;
}

bool shell_self_test(void)
{
    struct shell_state saved;

    if (!matching_is_right()) {
        return false;
    }

    if (!argument_splitting_is_right()) {
        return false;
    }

    if (!refusals_are_named()) {
        return false;
    }

    /*
     * Before initialization, feeding a character is refused. Checked first,
     * because the rest of this needs the shell up and there is no way back.
     */
    if (!state.active) {
        if (shell_feed('a') != SHELL_STATUS_NOT_INITIALIZED) {
            return false;
        }
    }

    saved = state;

    if (shell_initialize() != SHELL_STATUS_OK) {
        return false;
    }

    if (!line_editing_is_right() || !dispatch_is_right()) {
        return false;
    }

    /*
     * Put the counters back. This runs before boot has finished, and a shell
     * that starts life claiming to have run five commands is a shell whose
     * statistics are already a lie.
     */
    state = saved;
    state.length = 0U;
    line[0] = '\0';
    return true;
}
