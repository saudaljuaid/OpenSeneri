/* SPDX-License-Identifier: GPL-3.0-only */
/* General native application admission, scheduling, syscalls, and teardown. */

#include <phipia/native_process.h>

#include <phipia/abi.h>
#include <phipia/audio.h>
#include <phipia/clock.h>
#include <phipia/console.h>
#include <phipia/cpu.h>
#include <phipia/elf64_dynamic.h>
#include <phipia/fat32_fs.h>
#include <phipia/framebuffer.h>
#include <phipia/heap.h>
#include <phipia/interrupts.h>
#include <phipia/memory.h>
#include <phipia/native_fpu.h>
#include <phipia/native_handle.h>
#include <phipia/native_image.h>
#include <phipia/native_syscall.h>
#include <phipia/network.h>
#include <phipia/keyboard.h>
#include <phipia/paging.h>
#include <phipia/package_control.h>
#include <phipia/package_upload.h>
#include <phipia/process.h>
#include <phipia/random.h>
#include <phipia/timer.h>
#include <phipia/tsc.h>
#include <phipia/ui.h>
#include <phipia/wall_clock.h>

#define IA32_FS_BASE UINT32_C(0xC0000100)
#define NATIVE_MAIN_STACK_GUARD PAGING_NATIVE_STACK_BASE
#define NATIVE_MAIN_STACK_BASE (NATIVE_MAIN_STACK_GUARD + PAGING_PAGE_SIZE)
#define NATIVE_MAIN_STACK_END \
    (NATIVE_MAIN_STACK_BASE + NATIVE_STACK_PAGES * PAGING_PAGE_SIZE)
#define NATIVE_COPY_CHUNK 4096U
#define NATIVE_AUX_NULL UINT64_C(0)
#define NATIVE_AUX_PAGESZ UINT64_C(6)
#define NATIVE_AUX_ENTRY UINT64_C(9)
#define NATIVE_AUX_PHIPIA_ABI UINT64_C(0x53500001)
#define NATIVE_AUX_TLS_IMAGE UINT64_C(0x53500002)
#define NATIVE_AUX_TLS_SIZE UINT64_C(0x53500003)
#define NATIVE_AUX_TLS_ALIGN UINT64_C(0x53500004)
#define NATIVE_MAIN_TLS_GUARD PAGING_NATIVE_ANON_BASE
#define NATIVE_MAIN_TLS_BASE (NATIVE_MAIN_TLS_GUARD + PAGING_PAGE_SIZE)
#define NATIVE_TLS_TCB_BYTES sizeof(uint64_t)
#define ELF_PF_X UINT32_C(1)
#define ELF_PF_W UINT32_C(2)
#define NATIVE_RFLAGS UINT64_C(0x202)
#define NATIVE_EVENT_QUEUE_CAPACITY 64U
#define NATIVE_CONSOLE_INPUT_CAPACITY 256U
#define NATIVE_SURFACE_MAX_WIDTH 1280U
#define NATIVE_SURFACE_MAX_HEIGHT 720U
#define NATIVE_SCHEDULER_MIN_SLEEP_NS UINT64_C(100000)
#define NATIVE_DYNAMIC_WORKING_BYTES (8U * 1024U * 1024U)
#define NATIVE_DYNAMIC_GUARD_BYTES PAGING_PAGE_SIZE
#define NATIVE_DYNAMIC_TRAMPOLINE_PAGES 3U
#define NATIVE_DYNAMIC_START_PAGES 2U
#define NATIVE_SHARED_CODE_CACHE_CAPACITY 32768U
#define NATIVE_SHARED_CODE_EMPTY UINT8_C(0)
#define NATIVE_SHARED_CODE_LIVE UINT8_C(1)
#define NATIVE_SHARED_CODE_TOMBSTONE UINT8_C(2)

_Static_assert(PHIPIA_NETWORK_IO_MAX_BYTES <= NATIVE_COPY_CHUNK,
    "native network transfer bound exceeds the syscall copy buffer");
_Static_assert(
    (NATIVE_SHARED_CODE_CACHE_CAPACITY &
        (NATIVE_SHARED_CODE_CACHE_CAPACITY - 1U)) == 0U,
    "shared-code cache must remain a power of two"
);
_Static_assert(
    NATIVE_SHARED_CODE_CACHE_CAPACITY >=
        NATIVE_PROCESS_LIMIT * NATIVE_PROCESS_PAGE_LIMIT,
    "shared-code cache must hold the maximum live process-page census"
);

_Static_assert(PHIPIA_AUDIO_SAMPLE_RATE == AUDIO_PCM_SAMPLE_RATE,
    "kernel and public audio sample rates differ");
_Static_assert(PHIPIA_AUDIO_CHANNELS == AUDIO_PCM_CHANNELS,
    "kernel and public audio channel counts differ");
_Static_assert(PHIPIA_AUDIO_BITS_PER_SAMPLE == AUDIO_PCM_BITS_PER_SAMPLE,
    "kernel and public audio sample widths differ");
_Static_assert(PHIPIA_AUDIO_CHUNK_BYTES == AUDIO_PCM_BYTES,
    "kernel and public audio chunk sizes differ");
_Static_assert(PHIPIA_AUDIO_MAX_STREAMS == AUDIO_NATIVE_STREAMS,
    "kernel and public audio stream bounds differ");
_Static_assert(PHIPIA_AUDIO_VOLUME_UNITY == AUDIO_NATIVE_VOLUME_UNITY,
    "kernel and public audio gain scales differ");
_Static_assert(PHIPIA_PACKAGE_UPLOAD_WRITE_MAX == PACKAGE_UPLOAD_WRITE_MAX,
    "kernel and public package-upload write bounds differ");
_Static_assert(PHIPIA_PACKAGE_UPLOAD_MAX_BYTES == PACKAGE_UPLOAD_MAX_BYTES,
    "kernel and public package-upload size bounds differ");
_Static_assert(PHIPIA_PACKAGE_CONTROL_PLAN_MAX ==
    PACKAGE_CONTROL_PLAN_MAX_PACKAGES,
    "kernel and public package-control plan bounds differ");
_Static_assert(PHIPIA_PACKAGE_CONTROL_TEXT_BYTES ==
    PACKAGE_CONTROL_TEXT_BYTES,
    "kernel and public package-control text bounds differ");
_Static_assert(PHIPIA_PACKAGE_CONTROL_PATH_BYTES ==
    PACKAGE_CONTROL_PATH_BYTES,
    "kernel and public package-control path bounds differ");

enum native_thread_state {
    NATIVE_THREAD_UNUSED = 0,
    NATIVE_THREAD_RUNNABLE,
    NATIVE_THREAD_SLEEP_WAIT,
    NATIVE_THREAD_JOIN_WAIT,
    NATIVE_THREAD_FUTEX_WAIT,
    NATIVE_THREAD_CONSOLE_WAIT,
    NATIVE_THREAD_HANDLE_WAIT,
    NATIVE_THREAD_AUDIO_DRAIN_WAIT,
    NATIVE_THREAD_EXITED,
    NATIVE_THREAD_FAULTED
};

struct native_page {
    uint64_t virtual_address;
    uintptr_t physical_address;
    size_t shared_code_slot;
    uint32_t permissions;
    enum paging_process_mapping_kind kind;
    bool mapped;
    bool shared_code;
};

struct native_thread {
    struct process_user_context context;
    struct native_fpu_state fpu;
    uint64_t fs_base;
    uint64_t generation;
    uint64_t stack_base;
    uint64_t stack_end;
    uint64_t wait_generation;
    uint64_t futex_address;
    uint64_t deadline_ns;
    uint64_t console_address;
    uint64_t wait_items_address;
    uint64_t audio_token;
    size_t console_length;
    size_t wait_item_count;
    struct phipia_wait_item wait_items[PHIPIA_WAIT_MAX];
    int32_t exit_status;
    enum native_thread_state state;
};

struct native_directory_resource {
    phipfs_directory_handle iterator;
    bool active;
};

struct native_window_state {
    struct phipia_event events[NATIVE_EVENT_QUEUE_CAPACITY];
    uint32_t *shadow_pixels;
    uint64_t surface_address;
    uint64_t generation;
    size_t surface_bytes;
    size_t event_count;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    uint32_t ui_slot;
    uint64_t presented_pixels;
    uint64_t present_calls;
    bool allocated;
    bool visible;
    bool window_object_open;
    bool event_object_open;
    bool overflow_pending;
};

struct native_process {
    struct native_manifest manifest;
    struct native_validated_image image;
    struct paging_process_space address_space;
    struct paging_process_alias_set aliases;
    struct native_handle_table handles;
    struct native_thread threads[NATIVE_THREAD_LIMIT];
    struct native_page pages[NATIVE_PROCESS_PAGE_LIMIT];
    struct native_directory_resource directories[NATIVE_HANDLE_LIMIT];
    struct native_window_state window;
    uint8_t console_input[NATIVE_CONSOLE_INPUT_CAPACITY];
    uint64_t executable_frames[PAGING_PROCESS_ALIAS_MAX_PAGES];
    uint8_t transfer[NATIVE_COPY_CHUNK];
    uint64_t generation;
    size_t page_count;
    size_t executable_count;
    size_t thread_count;
    size_t current_thread;
    size_t console_input_head;
    size_t console_input_count;
    size_t peak_pages;
    size_t peak_handles;
    uint32_t syscall_count;
    uint32_t last_syscall;
    uint32_t failure_stage;
    uint32_t thread_switches;
    uint32_t shared_code_reuses;
    uint64_t context_cycles_without_fpu;
    uint64_t context_cycles_with_fpu;
    uint64_t dynamic_fini_entry;
    uint32_t context_transition_samples;
    int32_t exit_status;
    bool active;
    bool exiting;
    bool faulted;
    bool dynamic_fini_started;
};

struct native_dynamic_load {
    struct elf64_dynamic_catalog catalog;
    struct elf64_dynamic_image images[ELF64_DYNAMIC_MAX_OBJECTS];
    struct elf64_dynamic_prepared_object
        prepared[ELF64_DYNAMIC_MAX_OBJECTS];
    struct elf64_dynamic_lifecycle lifecycle;
    uint8_t *files[ELF64_DYNAMIC_MAX_OBJECTS];
    uint8_t *memories[ELF64_DYNAMIC_MAX_OBJECTS];
    size_t file_lengths[ELF64_DYNAMIC_MAX_OBJECTS];
    size_t memory_lengths[ELF64_DYNAMIC_MAX_OBJECTS];
    size_t source_indices[ELF64_DYNAMIC_MAX_OBJECTS];
    int64_t tls_offsets[ELF64_DYNAMIC_MAX_OBJECTS];
    uint8_t digests[ELF64_DYNAMIC_MAX_OBJECTS][32];
    size_t object_count;
    size_t library_count;
    size_t working_bytes;
    uint64_t tls_bytes;
    uint64_t tls_alignment;
    uint64_t next_virtual;
};

struct native_shared_code_page {
    uint8_t digest[32];
    uint64_t page_offset;
    uintptr_t physical_address;
    uint32_t references;
    uint8_t state;
};

static struct native_process processes[NATIVE_PROCESS_LIMIT];
static struct native_shared_code_page
    shared_code_cache[NATIVE_SHARED_CODE_CACHE_CAPACITY];
static size_t shared_code_live_pages;
static struct paging_process_expected_page
    validation_pages[NATIVE_PROCESS_PAGE_LIMIT];
static size_t current_process = SIZE_MAX;
static uint64_t next_process_generation = UINT64_C(1);
static uint64_t next_thread_generation = UINT64_C(1);
static uint64_t next_window_generation = UINT64_C(1);
static struct interrupt_process_gate native_gate;
static bool scheduler_active;
static size_t scheduler_process_cursor;

static void zero_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static size_t bounded_length(const uint8_t *text, size_t capacity)
{
    for (size_t index = 0U; index < capacity; ++index) {
        if (text[index] == 0U) {
            return index;
        }
    }
    return capacity;
}

static void copy_bytes(void *destination, const void *source, size_t length)
{
    uint8_t *output = destination;
    const uint8_t *input = source;

    for (size_t index = 0U; index < length; ++index) {
        output[index] = input[index];
    }
}

static bool add_u64(uint64_t left, uint64_t right, uint64_t *result)
{
    if (result == NULL || left > UINT64_MAX - right) {
        return false;
    }
    *result = left + right;
    return true;
}

static void record_context_transition(
    struct native_process *process,
    uint64_t without_fpu,
    uint64_t fpu
)
{
    uint64_t with_fpu;

    if (process == NULL || !add_u64(without_fpu, fpu, &with_fpu)) {
        return;
    }
    if (!add_u64(process->context_cycles_without_fpu, without_fpu,
            &process->context_cycles_without_fpu)) {
        process->context_cycles_without_fpu = UINT64_MAX;
    }
    if (!add_u64(process->context_cycles_with_fpu, with_fpu,
            &process->context_cycles_with_fpu)) {
        process->context_cycles_with_fpu = UINT64_MAX;
    }
    if (process->context_transition_samples != UINT32_MAX) {
        ++process->context_transition_samples;
    }
}

static bool canonical_user_range(uint64_t address, size_t length)
{
    uint64_t end;

    return address != 0U && length != 0U &&
        add_u64(address, (uint64_t)length - 1U, &end) &&
        address <= UINT64_C(0x00007FFFFFFFFFFF) &&
        end <= UINT64_C(0x00007FFFFFFFFFFF);
}

static struct native_process *running_process(void)
{
    if (!scheduler_active || current_process >= NATIVE_PROCESS_LIMIT ||
        !processes[current_process].active) {
        return NULL;
    }
    return &processes[current_process];
}

static struct native_thread *running_thread(struct native_process *process)
{
    if (process == NULL || process->current_thread >= process->thread_count ||
        process->threads[process->current_thread].state ==
            NATIVE_THREAD_UNUSED) {
        return NULL;
    }
    return &process->threads[process->current_thread];
}

static size_t page_lower_bound(
    const struct native_process *process,
    uint64_t virtual_address
)
{
    size_t low = 0U;
    size_t high = process->page_count;

    while (low < high) {
        const size_t middle = low + (high - low) / 2U;

        if (process->pages[middle].virtual_address < virtual_address) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    return low;
}

static struct native_page *page_at(
    struct native_process *process,
    uint64_t virtual_address
)
{
    const uint64_t page_address = virtual_address & ~(PAGING_PAGE_SIZE - 1U);
    const size_t index = page_lower_bound(process, page_address);

    if (index >= process->page_count ||
        process->pages[index].virtual_address != page_address) {
        return NULL;
    }
    return &process->pages[index];
}

static bool record_page(
    struct native_process *process,
    const struct native_page *page
)
{
    size_t index;

    if (process == NULL || page == NULL ||
        process->page_count >= NATIVE_PROCESS_PAGE_LIMIT) {
        return false;
    }
    index = page_lower_bound(process, page->virtual_address);
    if (index < process->page_count &&
        process->pages[index].virtual_address == page->virtual_address) {
        return false;
    }
    for (size_t move = process->page_count; move > index; --move) {
        process->pages[move] = process->pages[move - 1U];
    }
    process->pages[index] = *page;
    ++process->page_count;
    if (process->page_count > process->peak_pages) {
        process->peak_pages = process->page_count;
    }
    return true;
}

static bool bytes_equal(const uint8_t *left, const uint8_t *right,
    size_t count)
{
    uint8_t difference = 0U;

    for (size_t index = 0U; index < count; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0U;
}

static bool bytes_are_zero(const void *pointer, size_t count)
{
    const uint8_t *bytes = pointer;
    uint8_t combined = 0U;

    for (size_t index = 0U; index < count; ++index) {
        combined |= bytes[index];
    }
    return combined == 0U;
}

static size_t shared_code_hash(const uint8_t digest[32], uint64_t page_offset)
{
    uint64_t hash = UINT64_C(1469598103934665603);

    for (size_t index = 0U; index < 32U; ++index) {
        hash ^= digest[index];
        hash *= UINT64_C(1099511628211);
    }
    for (size_t index = 0U; index < sizeof(page_offset); ++index) {
        hash ^= (uint8_t)(page_offset >> (index * 8U));
        hash *= UINT64_C(1099511628211);
    }
    return (size_t)(hash & (NATIVE_SHARED_CODE_CACHE_CAPACITY - 1U));
}

static size_t shared_code_find(const uint8_t digest[32],
    uint64_t page_offset, bool *found)
{
    const size_t start = shared_code_hash(digest, page_offset);
    size_t tombstone = SIZE_MAX;

    *found = false;
    for (size_t probe = 0U; probe < NATIVE_SHARED_CODE_CACHE_CAPACITY;
         ++probe) {
        const size_t index = (start + probe) &
            (NATIVE_SHARED_CODE_CACHE_CAPACITY - 1U);
        const struct native_shared_code_page *entry =
            &shared_code_cache[index];

        if (entry->state == NATIVE_SHARED_CODE_LIVE) {
            if (entry->page_offset == page_offset &&
                bytes_equal(entry->digest, digest, 32U)) {
                *found = true;
                return index;
            }
        } else if (entry->state == NATIVE_SHARED_CODE_TOMBSTONE) {
            if (tombstone == SIZE_MAX) {
                tombstone = index;
            }
        } else {
            return tombstone == SIZE_MAX ? index : tombstone;
        }
    }
    return tombstone;
}

static void reset_shared_code_cache(void)
{
    for (size_t index = 0U; index < NATIVE_SHARED_CODE_CACHE_CAPACITY;
         ++index) {
        shared_code_cache[index].state = NATIVE_SHARED_CODE_EMPTY;
    }
    shared_code_live_pages = 0U;
}

static bool acquire_shared_code_page(
    struct native_process *process,
    uint64_t virtual_address,
    const uint8_t digest[32],
    uint64_t page_offset,
    const uint8_t *source,
    uintptr_t *physical_address
)
{
    struct native_page page;
    struct native_shared_code_page *entry;
    bool found;
    size_t slot;

    if (process == NULL || digest == NULL || source == NULL ||
        physical_address == NULL ||
        ((process->page_count + 1U) * PAGING_PAGE_SIZE) >
            process->manifest.memory_limit) {
        return false;
    }
    slot = shared_code_find(digest, page_offset, &found);
    if (slot == SIZE_MAX) {
        return false;
    }
    entry = &shared_code_cache[slot];
    if (found) {
        if (entry->references == UINT32_MAX ||
            !bytes_equal((const uint8_t *)(const void *)
                    entry->physical_address, source,
                (size_t)PAGING_PAGE_SIZE)) {
            return false;
        }
        *physical_address = entry->physical_address;
    } else {
        if (shared_code_live_pages == NATIVE_SHARED_CODE_CACHE_CAPACITY ||
            frame_allocate(physical_address) != FRAME_STATUS_OK) {
            return false;
        }
        copy_bytes((void *)*physical_address, source,
            (size_t)PAGING_PAGE_SIZE);
    }
    zero_bytes(&page, sizeof(page));
    page.virtual_address = virtual_address;
    page.physical_address = *physical_address;
    page.shared_code_slot = slot;
    page.permissions = PAGING_EXECUTE;
    page.kind = PAGING_PROCESS_MAPPING_NATIVE_IMAGE;
    page.shared_code = true;
    if (!record_page(process, &page)) {
        if (!found) {
            (void)frame_release(*physical_address);
        }
        *physical_address = 0U;
        return false;
    }
    if (found) {
        ++entry->references;
        if (process->shared_code_reuses != UINT32_MAX) {
            ++process->shared_code_reuses;
        }
    } else {
        copy_bytes(entry->digest, digest, 32U);
        entry->page_offset = page_offset;
        entry->physical_address = *physical_address;
        entry->references = 1U;
        entry->state = NATIVE_SHARED_CODE_LIVE;
        ++shared_code_live_pages;
    }
    return true;
}

static bool release_page_frame(const struct native_page *page)
{
    if (page == NULL) {
        return false;
    }
    if (!page->shared_code) {
        return frame_release(page->physical_address) == FRAME_STATUS_OK;
    }
    if (page->shared_code_slot >= NATIVE_SHARED_CODE_CACHE_CAPACITY) {
        return false;
    }
    {
        struct native_shared_code_page *entry =
            &shared_code_cache[page->shared_code_slot];

        if (entry->state != NATIVE_SHARED_CODE_LIVE ||
            entry->physical_address != page->physical_address ||
            entry->references == 0U) {
            return false;
        }
        --entry->references;
        if (entry->references != 0U) {
            return true;
        }
        if (frame_release(entry->physical_address) != FRAME_STATUS_OK) {
            ++entry->references;
            return false;
        }
        zero_bytes(entry, sizeof(*entry));
        entry->state = NATIVE_SHARED_CODE_TOMBSTONE;
        --shared_code_live_pages;
        if (shared_code_live_pages == 0U) {
            reset_shared_code_cache();
        }
    }
    return true;
}

static bool remove_page_record(
    struct native_process *process,
    uint64_t virtual_address,
    struct native_page *removed
)
{
    const size_t index = page_lower_bound(process, virtual_address);

    if (index >= process->page_count ||
        process->pages[index].virtual_address != virtual_address) {
        return false;
    }
    if (removed != NULL) {
        *removed = process->pages[index];
    }
    for (size_t move = index + 1U; move < process->page_count; ++move) {
        process->pages[move - 1U] = process->pages[move];
    }
    --process->page_count;
    zero_bytes(&process->pages[process->page_count],
        sizeof(process->pages[process->page_count]));
    return true;
}

static bool validate_user_range(
    struct native_process *process,
    uint64_t address,
    size_t length,
    bool output
)
{
    uint64_t cursor = address;
    size_t remaining = length;

    if (process == NULL || !canonical_user_range(address, length)) {
        return false;
    }
    while (remaining != 0U) {
        struct paging_translation translation;
        struct native_page *page = page_at(process, cursor);
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (cursor & (PAGING_PAGE_SIZE - 1U)));

        if (chunk > remaining) {
            chunk = remaining;
        }
        if (page == NULL || (output &&
                (page->permissions != PAGING_WRITE ||
                    page->kind == PAGING_PROCESS_MAPPING_NATIVE_IMAGE)) ||
            paging_process_translate(&process->address_space, cursor,
                &translation) != PAGING_STATUS_OK || !translation.user ||
            translation.level != 1U ||
            translation.permissions != page->permissions ||
            (translation.physical_address & ~(PAGING_PAGE_SIZE - 1U)) !=
                page->physical_address ||
            !frame_range_overlaps_allocatable_memory(
                translation.physical_address, chunk)) {
            return false;
        }
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool validate_futex_word(
    struct native_process *process,
    uint64_t address
)
{
    const struct native_page *page = page_at(process, address);

    return (address & (sizeof(uint32_t) - 1U)) == 0U &&
        page != NULL && page->permissions == PAGING_WRITE &&
        validate_user_range(process, address, sizeof(uint32_t), false);
}

static bool copy_from_user(
    struct native_process *process,
    void *destination,
    uint64_t address,
    size_t length
)
{
    uint8_t *output = destination;
    uint64_t cursor = address;
    size_t remaining = length;

    if (destination == NULL ||
        !validate_user_range(process, address, length, false)) {
        return false;
    }
    while (remaining != 0U) {
        struct paging_translation translation;
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (cursor & (PAGING_PAGE_SIZE - 1U)));

        if (chunk > remaining) {
            chunk = remaining;
        }
        if (paging_process_translate(&process->address_space, cursor,
                &translation) != PAGING_STATUS_OK) {
            return false;
        }
        copy_bytes(output, (const void *)(uintptr_t)translation.physical_address,
            chunk);
        output += chunk;
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool copy_to_user(
    struct native_process *process,
    uint64_t address,
    const void *source,
    size_t length
)
{
    const uint8_t *input = source;
    uint64_t cursor = address;
    size_t remaining = length;

    if (source == NULL || !validate_user_range(process, address, length, true)) {
        return false;
    }
    while (remaining != 0U) {
        struct paging_translation translation;
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (cursor & (PAGING_PAGE_SIZE - 1U)));

        if (chunk > remaining) {
            chunk = remaining;
        }
        if (paging_process_translate(&process->address_space, cursor,
                &translation) != PAGING_STATUS_OK) {
            return false;
        }
        copy_bytes((void *)(uintptr_t)translation.physical_address, input,
            chunk);
        input += chunk;
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

static int64_t handle_error(enum native_handle_status status)
{
    switch (status) {
    case NATIVE_HANDLE_OK:
        return 0;
    case NATIVE_HANDLE_FULL:
        return -PHIPIA_ENOMEM;
    case NATIVE_HANDLE_WRONG_TYPE:
        return -PHIPIA_EBADF;
    case NATIVE_HANDLE_STALE:
        return -PHIPIA_ESTALE;
    case NATIVE_HANDLE_CLOSE_FAILED:
        return -PHIPIA_EIO;
    case NATIVE_HANDLE_NULL_ARGUMENT:
    case NATIVE_HANDLE_BAD_LIMIT:
    case NATIVE_HANDLE_BAD_TYPE:
    case NATIVE_HANDLE_STATUS_COUNT:
    default:
        return -PHIPIA_EINVAL;
    }
}

static int64_t audio_error(enum audio_native_status status)
{
    switch (status) {
    case AUDIO_NATIVE_OK:
        return 0;
    case AUDIO_NATIVE_ABSENT:
        return -PHIPIA_ENOTSUP;
    case AUDIO_NATIVE_BUSY:
        return -PHIPIA_EBUSY;
    case AUDIO_NATIVE_STALE:
        return -PHIPIA_ESTALE;
    case AUDIO_NATIVE_CANCELED:
        return -PHIPIA_ECANCELED;
    case AUDIO_NATIVE_NULL_ARGUMENT:
    case AUDIO_NATIVE_INVALID:
        return -PHIPIA_EINVAL;
    case AUDIO_NATIVE_IO:
    case AUDIO_NATIVE_STATUS_COUNT:
    default:
        return -PHIPIA_EIO;
    }
}

static int64_t filesystem_error(enum phipfs_status status)
{
    switch (status) {
    case PHIPFS_STATUS_OK:
        return 0;
    case PHIPFS_STATUS_NOT_FOUND:
        return -PHIPIA_ENOENT;
    case PHIPFS_STATUS_EXISTS:
        return -PHIPIA_EEXIST;
    case PHIPFS_STATUS_READ_ONLY:
        return -PHIPIA_EROFS;
    case PHIPFS_STATUS_ACCESS:
        return -PHIPIA_EACCES;
    case PHIPFS_STATUS_NOT_DIRECTORY:
        return -PHIPIA_ENOTDIR;
    case PHIPFS_STATUS_IS_DIRECTORY:
        return -PHIPIA_EISDIR;
    case PHIPFS_STATUS_NOT_EMPTY:
        return -PHIPIA_ENOTEMPTY;
    case PHIPFS_STATUS_BUSY:
        return -PHIPIA_EBUSY;
    case PHIPFS_STATUS_NO_HANDLES:
        return -PHIPIA_ENOMEM;
    case PHIPFS_STATUS_STALE_HANDLE:
        return -PHIPIA_ESTALE;
    case PHIPFS_STATUS_FULL:
    case PHIPFS_STATUS_DIRECTORY_FULL:
        return -PHIPIA_ENOSPC;
    case PHIPFS_STATUS_NAME:
        return -PHIPIA_ENAMETOOLONG;
    case PHIPFS_STATUS_PATH:
    case PHIPFS_STATUS_INVALID_ARGUMENT:
    case PHIPFS_STATUS_RANGE:
        return -PHIPIA_EINVAL;
    case PHIPFS_STATUS_ABSENT:
    case PHIPFS_STATUS_NOT_MOUNTED:
        return -PHIPIA_ENOENT;
    case PHIPFS_STATUS_CORRUPT:
    case PHIPFS_STATUS_IO:
    case PHIPFS_STATUS_WRITEBACK:
    case PHIPFS_STATUS_RESET:
    case PHIPFS_STATUS_ALREADY_MOUNTED:
    case PHIPFS_STATUS_COUNT:
    default:
        return -PHIPIA_EIO;
    }
}

static int64_t package_upload_error(
    enum package_upload_status status,
    enum phipfs_status filesystem_status
)
{
    switch (status) {
    case PACKAGE_UPLOAD_STATUS_OK:
        return 0;
    case PACKAGE_UPLOAD_STATUS_NOT_INITIALIZED:
        return -PHIPIA_ENOTSUP;
    case PACKAGE_UPLOAD_STATUS_BUSY:
        return -PHIPIA_EBUSY;
    case PACKAGE_UPLOAD_STATUS_NO_SLOT:
        return -PHIPIA_ENOMEM;
    case PACKAGE_UPLOAD_STATUS_STALE:
        return -PHIPIA_ESTALE;
    case PACKAGE_UPLOAD_STATUS_DIGEST:
        return -PHIPIA_EACCES;
    case PACKAGE_UPLOAD_STATUS_FILESYSTEM:
        return filesystem_error(filesystem_status);
    case PACKAGE_UPLOAD_STATUS_DURABILITY:
        return -PHIPIA_EIO;
    case PACKAGE_UPLOAD_STATUS_NULL_ARGUMENT:
    case PACKAGE_UPLOAD_STATUS_STATE:
    case PACKAGE_UPLOAD_STATUS_RANGE:
    case PACKAGE_UPLOAD_STATUS_LENGTH:
    case PACKAGE_UPLOAD_STATUS_COUNT:
    default:
        return -PHIPIA_EINVAL;
    }
}

static int64_t package_control_error(
    enum package_control_status status,
    const struct package_control_report *report
)
{
    switch (status) {
    case PACKAGE_CONTROL_STATUS_OK:
        return 0;
    case PACKAGE_CONTROL_STATUS_BUSY:
        return -PHIPIA_EBUSY;
    case PACKAGE_CONTROL_STATUS_NO_SLOT:
    case PACKAGE_CONTROL_STATUS_RESOURCE:
        return -PHIPIA_ENOMEM;
    case PACKAGE_CONTROL_STATUS_STALE:
        return -PHIPIA_ESTALE;
    case PACKAGE_CONTROL_STATUS_UPLOAD:
        return report == NULL ? -PHIPIA_EIO : package_upload_error(
            report->upload_status, PHIPFS_STATUS_IO);
    case PACKAGE_CONTROL_STATUS_MANAGER:
        if (report == NULL) {
            return -PHIPIA_EIO;
        }
        if (report->manager_status == PACKAGE_MANAGER_STATUS_NOT_FOUND) {
            return -PHIPIA_ENOENT;
        }
        if (report->manager_status ==
                PACKAGE_MANAGER_STATUS_ALREADY_INSTALLED) {
            return -PHIPIA_EEXIST;
        }
        if (report->manager_status ==
                PACKAGE_MANAGER_STATUS_CRYPTO_UNAVAILABLE) {
            return -PHIPIA_ENOTSUP;
        }
        return -PHIPIA_EACCES;
    case PACKAGE_CONTROL_STATUS_CLOCK:
    case PACKAGE_CONTROL_STATUS_SERVICE:
        return -PHIPIA_EIO;
    case PACKAGE_CONTROL_STATUS_TRUST:
        return -PHIPIA_EACCES;
    case PACKAGE_CONTROL_STATUS_NULL_ARGUMENT:
    case PACKAGE_CONTROL_STATUS_STATE:
    case PACKAGE_CONTROL_STATUS_RANGE:
    case PACKAGE_CONTROL_STATUS_COUNT:
    default:
        return -PHIPIA_EINVAL;
    }
}

static uint32_t package_control_result_flags(
    const struct package_control_report *report
)
{
    uint32_t result = 0U;

    if (report->prepared) {
        result |= PHIPIA_PACKAGE_CONTROL_PREPARED;
    }
    if (report->committed) {
        result |= PHIPIA_PACKAGE_CONTROL_COMMITTED;
    }
    return result;
}

static void window_finalize_if_unreferenced(struct native_process *process)
{
    if (process != NULL && process->window.allocated &&
        !process->window.window_object_open &&
        !process->window.event_object_open) {
        zero_bytes(&process->window, sizeof(process->window));
    }
}

static bool window_release_surface(struct native_process *process)
{
    struct native_window_state *window;
    const bool enabled = cpu_interrupts_enabled();
    bool success = true;
    size_t page_count;

    if (process == NULL || !process->window.allocated) {
        return false;
    }
    window = &process->window;
    if (enabled) {
        cpu_interrupt_disable();
    }
    if (ui_native_window_is_open(window->ui_slot) &&
        ui_native_window_close(window->ui_slot) != UI_STATUS_OK) {
        success = false;
    }
    page_count = (window->surface_bytes + PAGING_PAGE_SIZE - 1U) /
        PAGING_PAGE_SIZE;
    for (size_t page = 0U; page < page_count; ++page) {
        struct native_page removed;
        const uint64_t address = window->surface_address +
            page * PAGING_PAGE_SIZE;
        struct native_page *record = page_at(process, address);

        if (record == NULL ||
            record->kind != PAGING_PROCESS_MAPPING_NATIVE_SURFACE) {
            success = false;
            continue;
        }
        if (record->mapped &&
            paging_process_unmap_user_page(&process->address_space,
                PAGING_PROCESS_MAPPING_NATIVE_SURFACE, address) !=
                    PAGING_STATUS_OK) {
            success = false;
            continue;
        }
        if (!remove_page_record(process, address, &removed) ||
            !release_page_frame(&removed)) {
            success = false;
        }
    }
    if (window->shadow_pixels != NULL &&
        heap_free(window->shadow_pixels) != HEAP_STATUS_OK) {
        success = false;
    }
    window->shadow_pixels = NULL;
    window->surface_address = 0U;
    window->surface_bytes = 0U;
    window->visible = false;
    window->window_object_open = false;
    if (enabled) {
        cpu_interrupt_enable();
    }
    window_finalize_if_unreferenced(process);
    return success;
}

static bool close_resource(
    uint8_t type,
    const struct native_resource *resource,
    void *context
)
{
    struct native_process *process = context;

    if (resource == NULL || process == NULL) {
        return false;
    }
    switch (type) {
    case PHIPIA_HANDLE_FILE:
        return phipfs_close((phipfs_handle)resource->words[0]) == PHIPFS_STATUS_OK;
    case PHIPIA_HANDLE_DIRECTORY:
        if (resource->words[0] >= NATIVE_HANDLE_LIMIT) {
            return false;
        }
        if (phipfs_directory_close(
                process->directories[resource->words[0]].iterator) !=
                PHIPFS_STATUS_OK) {
            return false;
        }
        zero_bytes(&process->directories[resource->words[0]],
            sizeof(process->directories[resource->words[0]]));
        return true;
    case PHIPIA_HANDLE_STREAM:
    case PHIPIA_HANDLE_DATAGRAM:
        return network_close(process->generation,
            (network_handle)resource->words[0]) == NETWORK_STATUS_OK;
    case PHIPIA_HANDLE_TIMER:
        return true;
    case PHIPIA_HANDLE_WINDOW:
        if (!process->window.allocated ||
            process->window.generation != resource->words[1] ||
            process->window.ui_slot != resource->words[0]) {
            return false;
        }
        return window_release_surface(process);
    case PHIPIA_HANDLE_EVENT_QUEUE:
        if (!process->window.allocated ||
            process->window.generation != resource->words[1]) {
            return false;
        }
        process->window.event_object_open = false;
        process->window.event_count = 0U;
        process->window.overflow_pending = false;
        window_finalize_if_unreferenced(process);
        return true;
    case PHIPIA_HANDLE_THREAD:
        return true;
    case PHIPIA_HANDLE_AUDIO_OUTPUT: {
        const bool enabled = cpu_interrupts_enabled();
        enum audio_native_status status;

        cpu_interrupt_disable();
        status = audio_native_close(process->generation,
            resource->words[0]);
        if (enabled) {
            cpu_interrupt_enable();
        }
        return status == AUDIO_NATIVE_OK;
    }
    case PHIPIA_HANDLE_PACKAGE_UPLOAD: {
        struct package_upload_report report;

        return package_upload_close(process->generation, resource->words[0],
            &report) == PACKAGE_UPLOAD_STATUS_OK;
    }
    case PHIPIA_HANDLE_PACKAGE_CONTROL: {
        struct package_control_report report;

        return package_control_close(process->generation, resource->words[0],
            &report) == PACKAGE_CONTROL_STATUS_OK;
    }
    default:
        return false;
    }
}

static bool safe_relative_path(const char *path, size_t length)
{
    size_t component_start = 0U;

    if (path == NULL || length == 0U || length >= PHIPFS_MAX_PATH ||
        path[0] == '/' || path[0] == '\\') {
        return false;
    }
    for (size_t index = 0U; index <= length; ++index) {
        const bool boundary = index == length || path[index] == '/';

        if (index < length && ((uint8_t)path[index] > UINT8_C(0x7F) ||
                path[index] == '\\' || path[index] == ':' ||
                path[index] == '\0')) {
            return false;
        }
        if (!boundary) {
            continue;
        }
        const size_t component_length = index - component_start;

        if (component_length == 0U ||
            (component_length == 2U && path[component_start] == '.' &&
                path[component_start + 1U] == '.')) {
            return false;
        }
        if (component_length == 1U && path[component_start] == '.' &&
            length != 1U) {
            return false;
        }
        component_start = index + 1U;
    }
    return true;
}

static bool path_from_user(
    struct native_process *process,
    const struct phipia_path *path,
    char output[PHIPFS_MAX_PATH],
    enum phipfs_volume *volume
)
{
    char relative[PHIPFS_MAX_PATH];
    size_t namespace_length;

    if (process == NULL || path == NULL || output == NULL || volume == NULL ||
        path->reserved != 0U || path->length == 0U ||
        path->length >= sizeof(relative) ||
        !copy_from_user(process, relative, path->address, path->length) ||
        !safe_relative_path(relative, path->length)) {
        return false;
    }
    relative[path->length] = '\0';
    zero_bytes(output, PHIPFS_MAX_PATH);
    if (path->volume == PHIPIA_VOLUME_SYSTEM) {
        size_t resource_length;

        if ((process->manifest.capabilities & PHIPIA_CAP_SYSTEM_READ) == 0U) {
            return false;
        }
        resource_length = bounded_length(process->manifest.resource_directory,
            sizeof(process->manifest.resource_directory));
        if (resource_length == 0U) {
            copy_bytes(output, relative, path->length + 1U);
        } else if (path->length == 1U && relative[0] == '.') {
            copy_bytes(output, process->manifest.resource_directory,
                resource_length + 1U);
        } else {
            if (resource_length + 1U + path->length >= PHIPFS_MAX_PATH) {
                return false;
            }
            copy_bytes(output, process->manifest.resource_directory,
                resource_length);
            output[resource_length] = '/';
            copy_bytes(output + resource_length + 1U, relative,
                path->length + 1U);
        }
        *volume = PHIPFS_VOLUME_SYSTEM;
        return true;
    }
    if (path->volume != PHIPIA_VOLUME_DATA ||
        (process->manifest.capabilities &
            (PHIPIA_CAP_DATA_READ | PHIPIA_CAP_DATA_WRITE)) == 0U) {
        return false;
    }
    namespace_length = bounded_length(process->manifest.data_namespace,
        sizeof(process->manifest.data_namespace));
    if (namespace_length == 0U ||
        namespace_length + 1U + path->length >= PHIPFS_MAX_PATH) {
        return false;
    }
    copy_bytes(output, process->manifest.data_namespace, namespace_length);
    if (path->length == 1U && relative[0] == '.') {
        output[namespace_length] = '\0';
    } else {
        output[namespace_length] = '/';
        copy_bytes(output + namespace_length + 1U, relative,
            path->length + 1U);
    }
    *volume = PHIPFS_VOLUME_DATA;
    return true;
}

static bool read_volume_file(
    enum phipfs_volume volume,
    const char *path,
    uint8_t *destination,
    size_t capacity,
    size_t *read_bytes
)
{
    phipfs_handle handle;
    size_t total = 0U;

    if (path == NULL || destination == NULL || read_bytes == NULL ||
        (volume != PHIPFS_VOLUME_SYSTEM && volume != PHIPFS_VOLUME_DATA) ||
        phipfs_open(volume, path, PHIPFS_ACCESS_READ, &handle) !=
            PHIPFS_STATUS_OK) {
        return false;
    }
    while (total < capacity) {
        size_t completed = 0U;
        enum phipfs_status status = phipfs_read(handle, destination + total,
            capacity - total, &completed);

        if (status != PHIPFS_STATUS_OK) {
            (void)phipfs_close(handle);
            return false;
        }
        total += completed;
        if (completed == 0U) {
            break;
        }
    }
    if (phipfs_close(handle) != PHIPFS_STATUS_OK) {
        return false;
    }
    *read_bytes = total;
    return true;
}

static bool read_system_file(
    const char *path,
    uint8_t *destination,
    size_t capacity,
    size_t *read_bytes
)
{
    return read_volume_file(PHIPFS_VOLUME_SYSTEM, path, destination, capacity,
        read_bytes);
}

static bool sibling_image_path(
    const char *manifest_path,
    const uint8_t *name,
    size_t name_length,
    char *output
)
{
    size_t manifest_length;
    size_t prefix_length = 0U;

    if (manifest_path == NULL || name == NULL || output == NULL) {
        return false;
    }
    manifest_length = bounded_length((const uint8_t *)manifest_path,
        PHIPFS_MAX_PATH);
    if (manifest_length == 0U || manifest_length >= PHIPFS_MAX_PATH ||
        name_length == 0U || name_length >= 16U ||
        (name_length == 1U && name[0] == '.') ||
        (name_length == 2U && name[0] == '.' && name[1] == '.')) {
        return false;
    }
    for (size_t index = 0U; index < name_length; ++index) {
        if (name[index] < UINT8_C(0x21) || name[index] > UINT8_C(0x7e) ||
            name[index] == '/' || name[index] == '\\') {
            return false;
        }
    }
    for (size_t index = 0U; index < manifest_length; ++index) {
        if (manifest_path[index] == '/') {
            prefix_length = index + 1U;
        }
    }
    if (prefix_length + name_length >= PHIPFS_MAX_PATH) {
        return false;
    }
    zero_bytes(output, PHIPFS_MAX_PATH);
    copy_bytes(output, manifest_path, prefix_length);
    copy_bytes(output + prefix_length, name, name_length);
    return true;
}

static bool installed_manifest_path(const char *path)
{
    static const char prefix[] = "pkgstate/gen/";
    static const char root[] = "/root/";
    size_t length;
    size_t offset = sizeof(prefix) - 1U;

    if (path == NULL) {
        return false;
    }
    length = bounded_length((const uint8_t *)path, PHIPFS_MAX_PATH);
    if (length <= offset + 8U + 1U + 8U + sizeof(root) - 1U ||
        length >= PHIPFS_MAX_PATH) {
        return false;
    }
    for (size_t index = 0U; index < sizeof(prefix) - 1U; ++index) {
        if (path[index] != prefix[index]) {
            return false;
        }
    }
    for (size_t component = 0U; component < 2U; ++component) {
        for (size_t index = 0U; index < 8U; ++index) {
            const char value = path[offset + index];

            if (!((value >= '0' && value <= '9') ||
                    (value >= 'a' && value <= 'f'))) {
                return false;
            }
        }
        offset += 8U;
        if (path[offset++] != '/') {
            return false;
        }
    }
    --offset;
    for (size_t index = 0U; index < sizeof(root) - 1U; ++index) {
        if (path[offset + index] != root[index]) {
            return false;
        }
    }
    return true;
}

static bool allocate_page(
    struct native_process *process,
    uint64_t virtual_address,
    uint32_t permissions,
    enum paging_process_mapping_kind kind,
    uintptr_t *physical_address
)
{
    struct native_page page;

    if (process == NULL || physical_address == NULL ||
        ((process->page_count + 1U) * PAGING_PAGE_SIZE) >
            process->manifest.memory_limit ||
        frame_allocate(physical_address) != FRAME_STATUS_OK) {
        return false;
    }
    zero_bytes((void *)*physical_address, PAGING_PAGE_SIZE);
    page.virtual_address = virtual_address;
    page.physical_address = *physical_address;
    page.shared_code_slot = SIZE_MAX;
    page.permissions = permissions;
    page.kind = kind;
    page.mapped = false;
    page.shared_code = false;
    if (!record_page(process, &page)) {
        (void)frame_release(*physical_address);
        *physical_address = 0U;
        return false;
    }
    return true;
}

static bool prepare_image_pages(
    struct native_process *process,
    const uint8_t *elf,
    size_t elf_length
)
{
    for (size_t segment_index = 0U;
         segment_index < process->image.segment_count; ++segment_index) {
        const struct native_elf_segment *segment =
            &process->image.segments[segment_index];
        const uint32_t permissions = (segment->flags & ELF_PF_X) != 0U ?
            PAGING_EXECUTE : ((segment->flags & ELF_PF_W) != 0U ?
                PAGING_WRITE : PAGING_READ);

        for (uint64_t virtual_address = segment->mapping_start;
             virtual_address < segment->mapping_end;
             virtual_address += PAGING_PAGE_SIZE) {
            uintptr_t physical_address;
            const uint64_t file_virtual_end = segment->virtual_address +
                segment->file_size;
            const uint64_t copy_start = virtual_address >
                segment->virtual_address ? virtual_address :
                segment->virtual_address;
            const uint64_t page_end = virtual_address + PAGING_PAGE_SIZE;
            const uint64_t copy_end = page_end < file_virtual_end ?
                page_end : file_virtual_end;

            if (!allocate_page(process, virtual_address, permissions,
                    PAGING_PROCESS_MAPPING_NATIVE_IMAGE, &physical_address)) {
                return false;
            }
            if (copy_start < copy_end) {
                const uint64_t source_offset = segment->file_offset +
                    (copy_start - segment->virtual_address);
                const size_t count = (size_t)(copy_end - copy_start);

                if (source_offset > elf_length ||
                    count > elf_length - (size_t)source_offset) {
                    return false;
                }
                copy_bytes((uint8_t *)(void *)physical_address +
                        (size_t)(copy_start - virtual_address),
                    elf + source_offset, count);
            }
            if ((permissions & PAGING_EXECUTE) != 0U) {
                if (process->executable_count >=
                        PAGING_PROCESS_ALIAS_MAX_PAGES) {
                    return false;
                }
                process->executable_frames[process->executable_count++] =
                    physical_address;
            }
        }
    }
    return process->executable_count != 0U;
}

static bool dynamic_name_equal(
    const struct elf64_dynamic_name *left,
    const struct elf64_dynamic_name *right
)
{
    if (left == NULL || right == NULL || left->length != right->length) {
        return false;
    }
    for (size_t index = 0U; index < left->length; ++index) {
        if (left->bytes[index] != right->bytes[index]) {
            return false;
        }
    }
    return true;
}

static bool dynamic_name_path(
    const struct native_process *process,
    const struct elf64_dynamic_name *name,
    char path[static PHIPFS_MAX_PATH]
)
{
    size_t catalog_length;
    size_t prefix_length = 0U;

    if (process == NULL || name == NULL || name->length == 0U ||
        name->length >= 64U) {
        return false;
    }
    catalog_length = bounded_length(process->manifest.dynamic_catalog,
        sizeof(process->manifest.dynamic_catalog));
    for (size_t index = 0U; index < catalog_length; ++index) {
        if (process->manifest.dynamic_catalog[index] == '/') {
            prefix_length = index + 1U;
        }
    }
    if (prefix_length + name->length >= PHIPFS_MAX_PATH) {
        return false;
    }
    zero_bytes(path, PHIPFS_MAX_PATH);
    copy_bytes(path, process->manifest.dynamic_catalog, prefix_length);
    copy_bytes(path + prefix_length, name->bytes, name->length);
    return true;
}

static bool dynamic_object_supported(
    const struct native_process *process,
    const struct elf64_dynamic_image *image,
    bool root
)
{
    uint64_t memory_bytes;

    if (process == NULL || image == NULL || (root && image->entry == 0U) ||
        image->segment_count == 0U ||
        image->segment_count > ELF64_DYNAMIC_MAX_LOAD_SEGMENTS ||
        image->needed_count > ELF64_DYNAMIC_MAX_NEEDED ||
        image->mapping_end <= image->mapping_start ||
        image->mapping_end - image->mapping_start > SIZE_MAX) {
        return false;
    }
    memory_bytes = image->mapping_end - image->mapping_start;
    if (memory_bytes > ELF64_DYNAMIC_MAX_PREPARATION_BYTES ||
        memory_bytes > process->manifest.memory_limit ||
        ((image->relro_start == 0U) != (image->relro_end == 0U)) ||
        (image->relro_start != 0U &&
            image->relro_start >= image->relro_end)) {
        return false;
    }
    return true;
}

static enum heap_status dynamic_heap_allocate(size_t bytes, void **result)
{
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    enum heap_status status;

    cpu_interrupt_disable();
    status = heap_allocate(bytes, result);
    if (interrupts_were_enabled) {
        cpu_interrupt_enable();
    }
    return status;
}

static enum heap_status dynamic_heap_free(void *pointer)
{
    const bool interrupts_were_enabled = cpu_interrupts_enabled();
    enum heap_status status;

    cpu_interrupt_disable();
    status = heap_free(pointer);
    if (interrupts_were_enabled) {
        cpu_interrupt_enable();
    }
    return status;
}

static bool dynamic_allocate_bytes(
    struct native_dynamic_load *load,
    size_t bytes,
    uint8_t **result
)
{
    if (load == NULL || result == NULL || bytes == 0U ||
        load->working_bytes > NATIVE_DYNAMIC_WORKING_BYTES ||
        bytes > NATIVE_DYNAMIC_WORKING_BYTES - load->working_bytes ||
        dynamic_heap_allocate(bytes, (void **)result) != HEAP_STATUS_OK) {
        return false;
    }
    load->working_bytes += bytes;
    return true;
}

static bool dynamic_release_bytes(
    struct native_dynamic_load *load,
    uint8_t **pointer,
    size_t bytes
)
{
    if (load == NULL || pointer == NULL || *pointer == NULL ||
        bytes > load->working_bytes ||
        dynamic_heap_free(*pointer) != HEAP_STATUS_OK) {
        return false;
    }
    *pointer = NULL;
    load->working_bytes -= bytes;
    return true;
}

static bool dynamic_load_release(struct native_dynamic_load **load_pointer)
{
    struct native_dynamic_load *load;
    bool success = true;

    if (load_pointer == NULL || *load_pointer == NULL) {
        return true;
    }
    load = *load_pointer;
    for (size_t index = 0U; index < load->object_count; ++index) {
        if (load->memories[index] != NULL &&
            !dynamic_release_bytes(load, &load->memories[index],
                load->memory_lengths[index])) {
            success = false;
        }
    }
    for (size_t index = 1U; index <= load->library_count; ++index) {
        if (load->files[index] != NULL &&
            !dynamic_release_bytes(load, &load->files[index],
                load->file_lengths[index])) {
            success = false;
        }
    }
    if (dynamic_heap_free(load) != HEAP_STATUS_OK) {
        success = false;
    }
    *load_pointer = NULL;
    return success;
}

static const struct elf64_dynamic_catalog_entry *dynamic_catalog_entry(
    const struct native_dynamic_load *load,
    const struct elf64_dynamic_name *name
)
{
    if (load == NULL || name == NULL) {
        return NULL;
    }
    for (size_t index = 0U; index < load->catalog.entry_count; ++index) {
        if (dynamic_name_equal(&load->catalog.entries[index].name, name)) {
            return &load->catalog.entries[index];
        }
    }
    return NULL;
}

static size_t dynamic_loaded_library(
    const struct native_dynamic_load *load,
    const struct elf64_dynamic_name *name
)
{
    for (size_t index = 1U; index <= load->library_count; ++index) {
        if (dynamic_name_equal(&load->images[index].soname, name)) {
            return index;
        }
    }
    return SIZE_MAX;
}

static enum native_process_status dynamic_read_catalog(
    const struct native_process *process,
    struct native_dynamic_load *load
)
{
    char path[NATIVE_MANIFEST_PATH_BYTES + 1U];
    struct phipfs_stat stat;
    uint8_t *bytes = NULL;
    size_t read_bytes = 0U;
    const size_t length = bounded_length(process->manifest.dynamic_catalog,
        sizeof(process->manifest.dynamic_catalog));
    enum native_process_status result = NATIVE_PROCESS_IMAGE_REFUSED;

    if (length == 0U || length == sizeof(process->manifest.dynamic_catalog)) {
        return result;
    }
    zero_bytes(path, sizeof(path));
    copy_bytes(path, process->manifest.dynamic_catalog, length);
    if (phipfs_stat_path(PHIPFS_VOLUME_SYSTEM, path, &stat) != PHIPFS_STATUS_OK ||
        stat.directory || stat.size != ELF64_DYNAMIC_CATALOG_BYTES) {
        return result;
    }
    if (!dynamic_allocate_bytes(load, ELF64_DYNAMIC_CATALOG_BYTES, &bytes)) {
        return NATIVE_PROCESS_MEMORY_LIMIT;
    }
    if (read_system_file(path, bytes, ELF64_DYNAMIC_CATALOG_BYTES,
            &read_bytes) && read_bytes == ELF64_DYNAMIC_CATALOG_BYTES &&
        phipia_elf64_dynamic_catalog_authenticate(bytes, read_bytes,
            process->manifest.dynamic_catalog_sha256, &load->catalog) ==
                ELF64_DYNAMIC_OK) {
        result = NATIVE_PROCESS_OK;
    }
    if (!dynamic_release_bytes(load, &bytes, ELF64_DYNAMIC_CATALOG_BYTES)) {
        result = NATIVE_PROCESS_TEARDOWN;
    }
    return result;
}

static enum native_process_status dynamic_load_library(
    const struct native_process *process,
    struct native_dynamic_load *load,
    const struct elf64_dynamic_name *name
)
{
    const struct elf64_dynamic_catalog_entry *catalog =
        dynamic_catalog_entry(load, name);
    char path[PHIPFS_MAX_PATH];
    struct phipfs_stat stat;
    size_t read_bytes = 0U;
    size_t index;

    if (catalog == NULL || !dynamic_name_path(process, name, path) ||
        load->library_count + 1U >= ELF64_DYNAMIC_MAX_OBJECTS) {
        return NATIVE_PROCESS_IMAGE_REFUSED;
    }
    if (phipfs_stat_path(PHIPFS_VOLUME_SYSTEM, path, &stat) != PHIPFS_STATUS_OK ||
        stat.directory || stat.size == 0U ||
        stat.size > NATIVE_ELF_MAX_FILE_BYTES) {
        return NATIVE_PROCESS_IMAGE_REFUSED;
    }
    index = load->library_count + 1U;
    if (!dynamic_allocate_bytes(load, (size_t)stat.size,
            &load->files[index])) {
        return NATIVE_PROCESS_MEMORY_LIMIT;
    }
    load->file_lengths[index] = (size_t)stat.size;
    if (!read_system_file(path, load->files[index], (size_t)stat.size,
            &read_bytes) || read_bytes != stat.size ||
        phipia_elf64_dynamic_object_authenticate(load->files[index], read_bytes,
            catalog->sha256, &load->images[index]) != ELF64_DYNAMIC_OK ||
        !dynamic_name_equal(&load->images[index].soname, name) ||
        !dynamic_object_supported(process, &load->images[index], false)) {
        if (!dynamic_release_bytes(load, &load->files[index],
                load->file_lengths[index])) {
            return NATIVE_PROCESS_TEARDOWN;
        }
        load->file_lengths[index] = 0U;
        return NATIVE_PROCESS_IMAGE_REFUSED;
    }
    copy_bytes(load->digests[index], catalog->sha256,
        sizeof(load->digests[index]));
    load->library_count = index;
    return NATIVE_PROCESS_OK;
}

static bool dynamic_align_u64(uint64_t value, uint64_t alignment,
    uint64_t *result)
{
    if (result == NULL || alignment == 0U ||
        (alignment & (alignment - 1U)) != 0U ||
        value > UINT64_MAX - (alignment - 1U)) {
        return false;
    }
    *result = (value + alignment - 1U) & ~(alignment - 1U);
    return true;
}

static enum native_process_status dynamic_build_scope(
    const struct native_process *process,
    struct native_dynamic_load *load,
    uint8_t *root_file,
    size_t root_length
)
{
    uint8_t order[ELF64_DYNAMIC_MAX_OBJECTS];
    struct elf64_dynamic_prepared_object
        lifecycle_scope[ELF64_DYNAMIC_MAX_OBJECTS];
    uint64_t tls_starts[ELF64_DYNAMIC_MAX_OBJECTS];
    size_t order_count = 0U;
    size_t scan = 0U;
    uint64_t cursor = PAGING_NATIVE_IMAGE_BASE;
    enum elf64_dynamic_status dynamic_status;

    if (load->images[0].needed_count != 0U) {
        enum native_process_status status = dynamic_read_catalog(process, load);
        if (status != NATIVE_PROCESS_OK) {
            return status;
        }
    }
    while (scan <= load->library_count) {
        const struct elf64_dynamic_image *image = &load->images[scan];
        for (size_t needed = 0U; needed < image->needed_count; ++needed) {
            const struct elf64_dynamic_name *name = &image->needed[needed];
            enum native_process_status status;

            if (load->images[0].soname.length != 0U &&
                dynamic_name_equal(&load->images[0].soname, name)) {
                return NATIVE_PROCESS_IMAGE_REFUSED;
            }
            if (dynamic_loaded_library(load, name) != SIZE_MAX) {
                continue;
            }
            status = dynamic_load_library(process, load, name);
            if (status != NATIVE_PROCESS_OK) {
                return status;
            }
        }
        ++scan;
    }
    dynamic_status = phipia_elf64_dynamic_dependency_order(&load->images[0],
        &load->images[1], load->library_count, order, sizeof(order),
        &order_count);
    if (dynamic_status != ELF64_DYNAMIC_OK || order_count != load->library_count) {
        return NATIVE_PROCESS_IMAGE_REFUSED;
    }
    load->object_count = order_count + 1U;
    for (size_t index = 0U; index < load->object_count; ++index) {
        /* Preserve root-first breadth-first DT_NEEDED order for preemption. */
        load->source_indices[index] = index;
    }
    zero_bytes(tls_starts, sizeof(tls_starts));
    load->tls_alignment = 1U;
    for (size_t index = 0U; index < load->object_count; ++index) {
        const struct elf64_dynamic_image *image =
            &load->images[load->source_indices[index]];
        uint64_t end;

        if (cursor < image->mapping_start) {
            return NATIVE_PROCESS_IMAGE_REFUSED;
        }
        load->prepared[index].image = image;
        load->prepared[index].load_bias = cursor - image->mapping_start;
        if (!add_u64(cursor, image->mapping_end - image->mapping_start, &end) ||
            !add_u64(end, NATIVE_DYNAMIC_GUARD_BYTES, &cursor)) {
            return NATIVE_PROCESS_IMAGE_REFUSED;
        }
    }
    /*
     * Variant-II local-exec code expects the root TLS block immediately below
     * the thread pointer. Lay dependencies out first and the PIE root last;
     * prepare_dynamic_main_tls places any whole-template alignment padding
     * before this byte range, never between the root block and FS:0.
     */
    for (size_t position = 0U; position < load->object_count; ++position) {
        const size_t index = position + 1U < load->object_count ?
            position + 1U : 0U;
        const struct elf64_dynamic_image *image =
            &load->images[load->source_indices[index]];

        if (image->tls.memory_size == 0U) {
            continue;
        }
        if (!dynamic_align_u64(load->tls_bytes, image->tls.alignment,
                &load->tls_bytes)) {
            return NATIVE_PROCESS_IMAGE_REFUSED;
        }
        tls_starts[index] = load->tls_bytes;
        if (!add_u64(load->tls_bytes, image->tls.memory_size,
                &load->tls_bytes)) {
            return NATIVE_PROCESS_IMAGE_REFUSED;
        }
        if (image->tls.alignment > load->tls_alignment) {
            load->tls_alignment = image->tls.alignment;
        }
    }
    if (load->tls_bytes > process->manifest.memory_limit ||
        load->tls_bytes > INT64_MAX) {
        return NATIVE_PROCESS_IMAGE_REFUSED;
    }
    for (size_t index = 0U; index < load->object_count; ++index) {
        const size_t source = load->source_indices[index];
        const struct elf64_dynamic_image *image = &load->images[source];
        const size_t memory_bytes = (size_t)(image->mapping_end -
            image->mapping_start);
        const uint8_t *file = source == 0U ? root_file : load->files[source];
        const size_t file_length = source == 0U ? root_length :
            load->file_lengths[source];

        if (!dynamic_allocate_bytes(load, memory_bytes,
                &load->memories[index])) {
            return NATIVE_PROCESS_MEMORY_LIMIT;
        }
        load->memory_lengths[index] = memory_bytes;
        if (image->tls.memory_size != 0U) {
            load->tls_offsets[index] = (int64_t)tls_starts[index] -
                (int64_t)load->tls_bytes;
        }
        load->prepared[index].input = file;
        load->prepared[index].input_length = file_length;
        load->prepared[index].memory = load->memories[index];
        load->prepared[index].memory_length = memory_bytes;
        load->prepared[index].tls_offset = load->tls_offsets[index];
    }
    {
        const uint64_t tls_pages = (load->tls_bytes + PAGING_PAGE_SIZE - 1U) /
            PAGING_PAGE_SIZE;
        uint64_t reserved_pages = NATIVE_DYNAMIC_TRAMPOLINE_PAGES;
        uint64_t reserved_bytes;

        if (tls_pages != 0U) {
            reserved_pages += tls_pages + 1U;
        }
        if (!add_u64(0U, reserved_pages * PAGING_PAGE_SIZE,
                &reserved_bytes) || cursor > PAGING_NATIVE_IMAGE_END ||
            reserved_bytes > PAGING_NATIVE_IMAGE_END - cursor) {
            return NATIVE_PROCESS_IMAGE_REFUSED;
        }
    }
    load->next_virtual = cursor;
    lifecycle_scope[0] = load->prepared[0];
    for (size_t index = 0U; index < order_count; ++index) {
        /* Constructors use dependencies-first order, separate from lookup. */
        lifecycle_scope[index + 1U] =
            load->prepared[(size_t)order[index] + 1U];
    }
    dynamic_status = phipia_elf64_dynamic_relocate_scope(load->prepared,
        load->object_count);
    if (dynamic_status != ELF64_DYNAMIC_OK) {
        console_write("Phipia: dynamic ELF relocation status ");
        console_write_u64((uint64_t)dynamic_status);
        console_putc('\n');
        return NATIVE_PROCESS_IMAGE_REFUSED;
    }
    dynamic_status = phipia_elf64_dynamic_lifecycle(lifecycle_scope,
        load->object_count, &load->lifecycle);
    if (dynamic_status != ELF64_DYNAMIC_OK) {
        console_write("Phipia: dynamic ELF lifecycle status ");
        console_write_u64((uint64_t)dynamic_status);
        console_putc('\n');
        return NATIVE_PROCESS_IMAGE_REFUSED;
    }
    return NATIVE_PROCESS_OK;
}

static enum native_process_status dynamic_load_create(
    const struct native_process *process,
    const struct elf64_dynamic_image *root,
    uint8_t *root_file,
    size_t root_length,
    struct native_dynamic_load **result
)
{
    struct native_dynamic_load *load = NULL;
    enum native_process_status status;

    if (process == NULL || root == NULL || root_file == NULL || result == NULL ||
        root_length > NATIVE_DYNAMIC_WORKING_BYTES -
            sizeof(struct native_dynamic_load) ||
        dynamic_heap_allocate(sizeof(*load), (void **)&load) !=
            HEAP_STATUS_OK) {
        return NATIVE_PROCESS_MEMORY_LIMIT;
    }
    zero_bytes(load, sizeof(*load));
    load->working_bytes = root_length + sizeof(*load);
    load->images[0] = *root;
    load->files[0] = root_file;
    load->file_lengths[0] = root_length;
    status = dynamic_build_scope(process, load, root_file, root_length);
    if (status != NATIVE_PROCESS_OK) {
        if (!dynamic_load_release(&load)) {
            return NATIVE_PROCESS_TEARDOWN;
        }
        return status;
    }
    *result = load;
    return NATIVE_PROCESS_OK;
}

static bool dynamic_native_image(
    struct native_process *process,
    const struct elf64_dynamic_image *dynamic,
    uint64_t load_bias
)
{
    struct native_validated_image *image;

    if (process == NULL || dynamic == NULL ||
        dynamic->segment_count > NATIVE_ELF_MAX_LOAD_SEGMENTS) {
        return false;
    }
    image = &process->image;
    zero_bytes(image, sizeof(*image));
    if (!add_u64(load_bias, dynamic->entry, &image->entry) ||
        !add_u64(load_bias, dynamic->mapping_start, &image->mapping_start) ||
        !add_u64(load_bias, dynamic->mapping_end, &image->mapping_end) ||
        image->mapping_start < PAGING_NATIVE_IMAGE_BASE ||
        image->mapping_end > PAGING_NATIVE_IMAGE_END) {
        return false;
    }
    image->valid = 1U;
    image->segment_count = dynamic->segment_count;
    for (size_t index = 0U; index < dynamic->segment_count; ++index) {
        const struct elf64_dynamic_segment *source =
            &dynamic->segments[index];
        struct native_elf_segment *destination = &image->segments[index];

        if (!add_u64(load_bias, source->virtual_address,
                &destination->virtual_address) ||
            !add_u64(load_bias, source->mapping_start,
                &destination->mapping_start) ||
            !add_u64(load_bias, source->mapping_end,
                &destination->mapping_end)) {
            return false;
        }
        destination->file_offset = source->virtual_address -
            dynamic->mapping_start;
        destination->file_size = source->memory_size;
        destination->memory_size = source->memory_size;
        destination->flags = source->flags;
    }
    return true;
}

static bool prepare_dynamic_frames(
    struct native_process *process,
    const struct elf64_dynamic_image *image,
    const uint8_t *prepared,
    size_t prepared_bytes,
    uint64_t load_bias,
    const uint8_t shared_digest[32]
)
{
    for (size_t segment_index = 0U;
         segment_index < image->segment_count; ++segment_index) {
        const struct elf64_dynamic_segment *segment =
            &image->segments[segment_index];
        const uint32_t base_permissions = (segment->flags & ELF_PF_X) != 0U ?
            PAGING_EXECUTE : ((segment->flags & ELF_PF_W) != 0U ?
                PAGING_WRITE : PAGING_READ);
        const uint64_t segment_end = segment->virtual_address +
            segment->memory_size;

        for (uint64_t link_page = segment->mapping_start;
             link_page < segment->mapping_end; link_page += PAGING_PAGE_SIZE) {
            const uint64_t copy_start = link_page > segment->virtual_address ?
                link_page : segment->virtual_address;
            const uint64_t page_end = link_page + PAGING_PAGE_SIZE;
            const uint64_t copy_end = page_end < segment_end ? page_end :
                segment_end;
            const uint32_t permissions = image->relro_start != 0U &&
                link_page >= image->relro_start &&
                link_page < image->relro_end ? PAGING_READ : base_permissions;
            uint64_t virtual_address;
            uintptr_t physical_address;

            if (!add_u64(load_bias, link_page, &virtual_address)) {
                return false;
            }
            if ((permissions & PAGING_EXECUTE) != 0U &&
                shared_digest != NULL) {
                const uint64_t source_offset = link_page -
                    image->mapping_start;

                if (source_offset > prepared_bytes ||
                    PAGING_PAGE_SIZE > prepared_bytes -
                        (size_t)source_offset ||
                    !acquire_shared_code_page(process, virtual_address,
                        shared_digest, source_offset,
                        prepared + source_offset, &physical_address)) {
                    return false;
                }
            } else if (!allocate_page(process, virtual_address, permissions,
                    PAGING_PROCESS_MAPPING_NATIVE_IMAGE,
                    &physical_address)) {
                return false;
            } else if (copy_start < copy_end) {
                const uint64_t source_offset = copy_start -
                    image->mapping_start;
                const size_t count = (size_t)(copy_end - copy_start);

                if (source_offset > prepared_bytes ||
                    count > prepared_bytes - (size_t)source_offset) {
                    return false;
                }
                copy_bytes((uint8_t *)(void *)physical_address +
                        (size_t)(copy_start - link_page),
                    prepared + source_offset, count);
            }
            if ((permissions & PAGING_EXECUTE) != 0U) {
                if (process->executable_count >=
                        PAGING_PROCESS_ALIAS_MAX_PAGES) {
                    return false;
                }
                process->executable_frames[process->executable_count++] =
                    physical_address;
            }
        }
    }
    return process->executable_count != 0U;
}

static bool dynamic_image_write(struct native_process *process,
    uint64_t address, const uint8_t *source, size_t length)
{
    uint64_t cursor = address;

    while (length != 0U) {
        struct native_page *page = page_at(process, cursor);
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (cursor & (PAGING_PAGE_SIZE - 1U)));

        if (page == NULL ||
            page->kind != PAGING_PROCESS_MAPPING_NATIVE_IMAGE) {
            return false;
        }
        if (chunk > length) {
            chunk = length;
        }
        copy_bytes((uint8_t *)(void *)page->physical_address +
                (size_t)(cursor - page->virtual_address), source, chunk);
        cursor += chunk;
        source += chunk;
        length -= chunk;
    }
    return true;
}

struct dynamic_code_writer {
    struct native_process *process;
    uint64_t base;
    size_t cursor;
    size_t capacity;
};

static bool dynamic_code_bytes(struct dynamic_code_writer *writer,
    const uint8_t *bytes, size_t count)
{
    if (writer == NULL || bytes == NULL ||
        writer->cursor > writer->capacity || count > writer->capacity -
            writer->cursor ||
        !dynamic_image_write(writer->process, writer->base + writer->cursor,
            bytes, count)) {
        return false;
    }
    writer->cursor += count;
    return true;
}

static bool dynamic_code_u64(struct dynamic_code_writer *writer,
    uint64_t value)
{
    uint8_t bytes[8];

    for (size_t index = 0U; index < sizeof(bytes); ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8U));
    }
    return dynamic_code_bytes(writer, bytes, sizeof(bytes));
}

static bool dynamic_code_call(struct dynamic_code_writer *writer,
    uint64_t address, bool arguments)
{
    static const uint8_t reload_arguments[] = {
        0x48U, 0x8bU, 0x7cU, 0x24U, 0x18U,
        0x48U, 0x8bU, 0x74U, 0x24U, 0x10U,
        0x48U, 0x8bU, 0x54U, 0x24U, 0x08U
    };
    static const uint8_t mov_rax[] = {0x48U, 0xb8U};
    static const uint8_t call_rax[] = {0xffU, 0xd0U};

    return (!arguments || dynamic_code_bytes(writer, reload_arguments,
            sizeof(reload_arguments))) &&
        dynamic_code_bytes(writer, mov_rax, sizeof(mov_rax)) &&
        dynamic_code_u64(writer, address) &&
        dynamic_code_bytes(writer, call_rax, sizeof(call_rax));
}

static bool dynamic_prepare_trampolines(
    struct native_process *process,
    const struct native_dynamic_load *load,
    uint64_t base,
    uint64_t *start_entry
)
{
    static const uint8_t start_prologue[] = {
        0x57U, 0x56U, 0x52U, 0x48U, 0x83U, 0xecU, 0x08U
    };
    static const uint8_t start_epilogue[] = {
        0x48U, 0x83U, 0xc4U, 0x08U, 0x5aU, 0x5eU, 0x5fU, 0x48U, 0xb8U
    };
    static const uint8_t jump_rax[] = {0xffU, 0xe0U};
    static const uint8_t fini_prologue[] = {
        /* mov %rdi,%r12; and $-16,%rsp -- independent of caller alignment. */
        0x49U, 0x89U, 0xfcU, 0x48U, 0x83U, 0xe4U, 0xf0U
    };
    static const uint8_t fini_epilogue[] = {
        /* mov %r12,%rdi; mov $SYS_EXIT,%eax; syscall; ud2. */
        0x4cU, 0x89U, 0xe7U, 0xb8U,
        (uint8_t)(PHIPIA_SYS_EXIT & 0xffU),
        (uint8_t)((PHIPIA_SYS_EXIT >> 8U) & 0xffU),
        (uint8_t)((PHIPIA_SYS_EXIT >> 16U) & 0xffU),
        (uint8_t)((PHIPIA_SYS_EXIT >> 24U) & 0xffU),
        0x0fU, 0x05U, 0x0fU, 0x0bU
    };
    struct dynamic_code_writer start = {
        process, base, 0U,
        NATIVE_DYNAMIC_START_PAGES * (size_t)PAGING_PAGE_SIZE
    };
    struct dynamic_code_writer fini = {
        process, base + NATIVE_DYNAMIC_START_PAGES * PAGING_PAGE_SIZE, 0U,
        (size_t)PAGING_PAGE_SIZE
    };

    if (load->lifecycle.constructor_count == 0U) {
        *start_entry = process->image.entry;
    } else {
        if (!dynamic_code_bytes(&start, start_prologue,
                sizeof(start_prologue))) {
            return false;
        }
        for (size_t index = 0U;
             index < load->lifecycle.constructor_count; ++index) {
            if (!dynamic_code_call(&start,
                    load->lifecycle.constructors[index], true)) {
                return false;
            }
        }
        if (!dynamic_code_bytes(&start, start_epilogue,
                sizeof(start_epilogue)) ||
            !dynamic_code_u64(&start, process->image.entry) ||
            !dynamic_code_bytes(&start, jump_rax, sizeof(jump_rax))) {
            return false;
        }
        *start_entry = base;
    }
    if (load->lifecycle.destructor_count != 0U) {
        if (!dynamic_code_bytes(&fini, fini_prologue,
                sizeof(fini_prologue))) {
            return false;
        }
        for (size_t index = 0U;
             index < load->lifecycle.destructor_count; ++index) {
            if (!dynamic_code_call(&fini,
                    load->lifecycle.destructors[index], false)) {
                return false;
            }
        }
        if (!dynamic_code_bytes(&fini, fini_epilogue,
                sizeof(fini_epilogue))) {
            return false;
        }
        process->dynamic_fini_entry = fini.base;
    }
    return true;
}

static bool dynamic_prepare_tls_template(
    struct native_process *process,
    const struct native_dynamic_load *load,
    uint64_t address
)
{
    const size_t pages = (size_t)((load->tls_bytes + PAGING_PAGE_SIZE - 1U) /
        PAGING_PAGE_SIZE);

    for (size_t page = 0U; page < pages; ++page) {
        uintptr_t physical_address;
        if (!allocate_page(process, address + page * PAGING_PAGE_SIZE,
                PAGING_READ, PAGING_PROCESS_MAPPING_NATIVE_IMAGE,
                &physical_address)) {
            return false;
        }
    }
    for (size_t index = 0U; index < load->object_count; ++index) {
        const struct elf64_dynamic_image *image = load->prepared[index].image;
        uint64_t source_offset;
        uint64_t destination_offset;

        if (image->tls.memory_size == 0U) {
            continue;
        }
        source_offset = image->tls.virtual_address - image->mapping_start;
        destination_offset = load->tls_bytes -
            (uint64_t)(-load->tls_offsets[index]);
        if (source_offset > load->memory_lengths[index] ||
            image->tls.memory_size > load->memory_lengths[index] -
                (size_t)source_offset ||
            !dynamic_image_write(process, address + destination_offset,
                load->memories[index] + source_offset,
                (size_t)image->tls.memory_size)) {
            return false;
        }
    }
    process->image.tls.file_offset = 0U;
    process->image.tls.virtual_address = address;
    process->image.tls.file_size = load->tls_bytes;
    process->image.tls.memory_size = load->tls_bytes;
    process->image.tls.alignment = load->tls_alignment;
    return true;
}

static enum native_process_status prepare_dynamic_image_pages(
    struct native_process *process,
    const struct native_dynamic_load *load,
    uint64_t *start_entry
)
{
    uint64_t cursor = load->next_virtual;

    if (process == NULL || load == NULL || start_entry == NULL ||
        !dynamic_native_image(process, load->prepared[0].image,
            load->prepared[0].load_bias)) {
        return NATIVE_PROCESS_IMAGE_REFUSED;
    }
    for (size_t index = 0U; index < load->object_count; ++index) {
        if (!prepare_dynamic_frames(process, load->prepared[index].image,
                load->memories[index], load->memory_lengths[index],
                load->prepared[index].load_bias,
                index == 0U ? NULL :
                    load->digests[load->source_indices[index]])) {
            return NATIVE_PROCESS_FRAME_ALLOCATION;
        }
    }
    if (load->tls_bytes != 0U) {
        const uint64_t pages = (load->tls_bytes + PAGING_PAGE_SIZE - 1U) /
            PAGING_PAGE_SIZE;
        if (!dynamic_prepare_tls_template(process, load, cursor)) {
            return NATIVE_PROCESS_FRAME_ALLOCATION;
        }
        cursor += (pages + 1U) * PAGING_PAGE_SIZE;
    }
    for (size_t page = 0U; page < NATIVE_DYNAMIC_TRAMPOLINE_PAGES; ++page) {
        uintptr_t physical_address;
        if (!allocate_page(process, cursor + page * PAGING_PAGE_SIZE,
                PAGING_EXECUTE, PAGING_PROCESS_MAPPING_NATIVE_IMAGE,
                &physical_address) || process->executable_count >=
                    PAGING_PROCESS_ALIAS_MAX_PAGES) {
            return NATIVE_PROCESS_FRAME_ALLOCATION;
        }
        process->executable_frames[process->executable_count++] =
            physical_address;
    }
    if (!dynamic_prepare_trampolines(process, load, cursor, start_entry)) {
        return NATIVE_PROCESS_IMAGE_REFUSED;
    }
    process->image.mapping_end = cursor +
        NATIVE_DYNAMIC_TRAMPOLINE_PAGES * PAGING_PAGE_SIZE;
    return NATIVE_PROCESS_OK;
}

static bool prepare_main_stack(struct native_process *process)
{
    for (size_t page = 0U; page < NATIVE_STACK_PAGES; ++page) {
        uintptr_t physical_address;

        if (!allocate_page(process,
                NATIVE_MAIN_STACK_BASE + page * PAGING_PAGE_SIZE,
                PAGING_WRITE, PAGING_PROCESS_MAPPING_NATIVE_STACK,
                &physical_address)) {
            return false;
        }
    }
    return true;
}

static bool prepared_tls_write(
    struct native_process *process,
    uint64_t address,
    const void *source,
    size_t length
)
{
    const uint8_t *input = source;
    uint64_t cursor = address;

    while (length != 0U) {
        struct native_page *page = page_at(process, cursor);
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (cursor & (PAGING_PAGE_SIZE - 1U)));

        if (page == NULL ||
            page->kind != PAGING_PROCESS_MAPPING_NATIVE_TLS) {
            return false;
        }
        if (chunk > length) {
            chunk = length;
        }
        copy_bytes((uint8_t *)(void *)page->physical_address +
                (size_t)(cursor - page->virtual_address), input, chunk);
        cursor += chunk;
        input += chunk;
        length -= chunk;
    }
    return true;
}

/*
 * x86-64 uses ELF TLS variant II: FS names the byte immediately after the
 * initial-exec block and local-exec offsets are negative.  A kernel-owned
 * mapping kind keeps the allocator and memory-unmap service from accidentally
 * handing out or removing the initial thread's template.
 */
static bool prepare_main_tls(
    struct native_process *process,
    const uint8_t *elf,
    size_t elf_length,
    uint64_t *thread_pointer
)
{
    const struct native_elf_tls *tls = &process->image.tls;
    uint64_t aligned_size;
    uint64_t allocation_size;
    size_t page_count;

    if (thread_pointer == NULL) {
        return false;
    }
    *thread_pointer = 0U;
    if (tls->memory_size == 0U) {
        return true;
    }
    if (tls->alignment == 0U || tls->memory_size > UINT64_MAX -
            (tls->alignment - 1U) || tls->file_offset > elf_length ||
        tls->file_size > elf_length - (size_t)tls->file_offset) {
        return false;
    }
    aligned_size = (tls->memory_size + tls->alignment - 1U) &
        ~(tls->alignment - 1U);
    if (!add_u64(aligned_size, NATIVE_TLS_TCB_BYTES, &allocation_size) ||
        allocation_size > UINT64_MAX - (PAGING_PAGE_SIZE - 1U)) {
        return false;
    }
    page_count = (size_t)((allocation_size + PAGING_PAGE_SIZE - 1U) /
        PAGING_PAGE_SIZE);
    if (page_count == 0U || page_count > NATIVE_PROCESS_PAGE_LIMIT ||
        page_count >= (PAGING_NATIVE_ANON_END - NATIVE_MAIN_TLS_BASE) /
            PAGING_PAGE_SIZE) {
        return false;
    }
    for (size_t page = 0U; page < page_count; ++page) {
        uintptr_t physical_address;

        if (!allocate_page(process,
                NATIVE_MAIN_TLS_BASE + page * PAGING_PAGE_SIZE,
                PAGING_WRITE, PAGING_PROCESS_MAPPING_NATIVE_TLS,
                &physical_address)) {
            return false;
        }
    }
    *thread_pointer = NATIVE_MAIN_TLS_BASE + aligned_size;
    return (tls->file_size == 0U || prepared_tls_write(process,
            *thread_pointer - tls->memory_size,
            elf + (size_t)tls->file_offset, (size_t)tls->file_size)) &&
        prepared_tls_write(process, *thread_pointer, thread_pointer,
            sizeof(*thread_pointer));
}

static bool prepared_image_copy_to_tls(
    struct native_process *process,
    uint64_t source_address,
    uint64_t destination_address,
    size_t length
)
{
    while (length != 0U) {
        const struct native_page *source = page_at(process, source_address);
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (source_address & (PAGING_PAGE_SIZE - 1U)));

        if (source == NULL ||
            source->kind != PAGING_PROCESS_MAPPING_NATIVE_IMAGE) {
            return false;
        }
        if (chunk > length) {
            chunk = length;
        }
        if (!prepared_tls_write(process, destination_address,
                (const uint8_t *)(const void *)source->physical_address +
                    (size_t)(source_address - source->virtual_address),
                chunk)) {
            return false;
        }
        source_address += chunk;
        destination_address += chunk;
        length -= chunk;
    }
    return true;
}

static bool prepare_dynamic_main_tls(
    struct native_process *process,
    uint64_t *thread_pointer
)
{
    const struct native_elf_tls *tls = &process->image.tls;
    uint64_t aligned_size;
    uint64_t allocation_size;
    size_t page_count;

    if (thread_pointer == NULL) {
        return false;
    }
    *thread_pointer = 0U;
    if (tls->memory_size == 0U) {
        return true;
    }
    if (tls->alignment == 0U ||
        (tls->alignment & (tls->alignment - 1U)) != 0U ||
        tls->memory_size > UINT64_MAX - (tls->alignment - 1U) ||
        tls->file_size != tls->memory_size) {
        return false;
    }
    aligned_size = (tls->memory_size + tls->alignment - 1U) &
        ~(tls->alignment - 1U);
    if (!add_u64(aligned_size, NATIVE_TLS_TCB_BYTES, &allocation_size) ||
        allocation_size > UINT64_MAX - (PAGING_PAGE_SIZE - 1U)) {
        return false;
    }
    page_count = (size_t)((allocation_size + PAGING_PAGE_SIZE - 1U) /
        PAGING_PAGE_SIZE);
    if (page_count == 0U || page_count > NATIVE_PROCESS_PAGE_LIMIT ||
        page_count >= (PAGING_NATIVE_ANON_END - NATIVE_MAIN_TLS_BASE) /
            PAGING_PAGE_SIZE) {
        return false;
    }
    for (size_t page = 0U; page < page_count; ++page) {
        uintptr_t physical_address;

        if (!allocate_page(process,
                NATIVE_MAIN_TLS_BASE + page * PAGING_PAGE_SIZE,
                PAGING_WRITE, PAGING_PROCESS_MAPPING_NATIVE_TLS,
                &physical_address)) {
            return false;
        }
    }
    *thread_pointer = NATIVE_MAIN_TLS_BASE + aligned_size;
    return prepared_image_copy_to_tls(process, tls->virtual_address,
            *thread_pointer - tls->memory_size, (size_t)tls->memory_size) &&
        prepared_tls_write(process, *thread_pointer, thread_pointer,
            sizeof(*thread_pointer));
}

static bool map_prepared_pages(struct native_process *process)
{
    if (paging_process_space_build(&process->address_space) !=
            PAGING_STATUS_OK ||
        paging_process_alias_set_narrow(&process->address_space,
            process->executable_frames, process->executable_count,
            &process->aliases) != PAGING_STATUS_OK) {
        return false;
    }
    for (size_t index = 0U; index < process->page_count; ++index) {
        const struct native_page *page = &process->pages[index];

        if (paging_process_map_user_page(&process->address_space, page->kind,
                page->virtual_address, page->physical_address,
                page->permissions) != PAGING_STATUS_OK) {
            return false;
        }
        process->pages[index].mapped = true;
        validation_pages[index].virtual_address = page->virtual_address;
        validation_pages[index].physical_address = page->physical_address;
        validation_pages[index].permissions = page->permissions;
    }
    return paging_process_validate_native(&process->address_space,
        validation_pages, process->page_count) == PAGING_STATUS_OK;
}

static bool stack_write(
    struct native_process *process,
    uint64_t address,
    const void *source,
    size_t length
)
{
    const uint8_t *input = source;
    uint64_t cursor = address;
    size_t remaining = length;

    if (address < NATIVE_MAIN_STACK_BASE ||
        length > NATIVE_MAIN_STACK_END - address) {
        return false;
    }
    while (remaining != 0U) {
        struct native_page *page = page_at(process, cursor);
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (cursor & (PAGING_PAGE_SIZE - 1U)));

        if (page == NULL || page->kind != PAGING_PROCESS_MAPPING_NATIVE_STACK) {
            return false;
        }
        if (chunk > remaining) {
            chunk = remaining;
        }
        copy_bytes((uint8_t *)(void *)page->physical_address +
                (size_t)(cursor - page->virtual_address), input, chunk);
        cursor += chunk;
        input += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool initialize_stack(
    struct native_process *process,
    struct native_thread *thread
)
{
    uint64_t argument_addresses[NATIVE_MANIFEST_ARGUMENTS + 1U];
    uint64_t environment_addresses[3];
    uint64_t cursor = NATIVE_MAIN_STACK_END;
    uint64_t vector[40];
    size_t vector_count = 0U;
    const size_t executable_length = bounded_length(
        process->manifest.executable, sizeof(process->manifest.executable));
    const size_t identifier_length = bounded_length(
        process->manifest.identifier, sizeof(process->manifest.identifier));
    const size_t namespace_length = bounded_length(
        process->manifest.data_namespace,
        sizeof(process->manifest.data_namespace));
    char environment[3][48];
    size_t environment_lengths[3];
    const size_t argc = (size_t)process->manifest.argument_count + 1U;

    zero_bytes(environment, sizeof(environment));
    copy_bytes(environment[0], "PHIPIA_ABI=1", 13U);
    copy_bytes(environment[1], "PHIPIA_APP_ID=", 14U);
    copy_bytes(environment[1] + 14U, process->manifest.identifier,
        identifier_length);
    copy_bytes(environment[2], "PHIPIA_DATA=", 12U);
    copy_bytes(environment[2] + 12U, process->manifest.data_namespace,
        namespace_length);
    environment_lengths[0] = 13U;
    environment_lengths[1] = 14U + identifier_length + 1U;
    environment_lengths[2] = 12U + namespace_length + 1U;

    for (size_t reverse = 3U; reverse > 0U; --reverse) {
        const size_t index = reverse - 1U;

        cursor -= environment_lengths[index];
        if (!stack_write(process, cursor, environment[index],
                environment_lengths[index])) {
            return false;
        }
        environment_addresses[index] = cursor;
    }
    for (size_t reverse = process->manifest.argument_count;
         reverse > 0U; --reverse) {
        const size_t index = reverse - 1U;
        const size_t length = bounded_length(process->manifest.arguments[index],
            NATIVE_MANIFEST_ARGUMENT_BYTES) + 1U;

        cursor -= length;
        if (!stack_write(process, cursor,
                process->manifest.arguments[index], length)) {
            return false;
        }
        argument_addresses[index + 1U] = cursor;
    }
    cursor -= executable_length + 1U;
    if (!stack_write(process, cursor, process->manifest.executable,
            executable_length + 1U)) {
        return false;
    }
    argument_addresses[0] = cursor;

    vector[vector_count++] = argc;
    for (size_t index = 0U; index < argc; ++index) {
        vector[vector_count++] = argument_addresses[index];
    }
    vector[vector_count++] = 0U;
    for (size_t index = 0U; index < 3U; ++index) {
        vector[vector_count++] = environment_addresses[index];
    }
    vector[vector_count++] = 0U;
    vector[vector_count++] = NATIVE_AUX_PAGESZ;
    vector[vector_count++] = PAGING_PAGE_SIZE;
    vector[vector_count++] = NATIVE_AUX_ENTRY;
    vector[vector_count++] = process->image.entry;
    vector[vector_count++] = NATIVE_AUX_PHIPIA_ABI;
    vector[vector_count++] = PHIPIA_ABI_VERSION;
    vector[vector_count++] = NATIVE_AUX_TLS_IMAGE;
    vector[vector_count++] = process->image.tls.virtual_address;
    vector[vector_count++] = NATIVE_AUX_TLS_SIZE;
    vector[vector_count++] = process->image.tls.memory_size;
    vector[vector_count++] = NATIVE_AUX_TLS_ALIGN;
    vector[vector_count++] = process->image.tls.alignment;
    vector[vector_count++] = NATIVE_AUX_NULL;
    vector[vector_count++] = 0U;
    cursor = (cursor - vector_count * sizeof(uint64_t)) & ~UINT64_C(0xF);
    if (!stack_write(process, cursor, vector,
            vector_count * sizeof(uint64_t))) {
        return false;
    }
    zero_bytes(&thread->context, sizeof(thread->context));
    thread->context.rip = process->image.entry;
    thread->context.rsp = cursor;
    thread->context.rflags = NATIVE_RFLAGS;
    thread->context.rdi = argc;
    thread->context.rsi = cursor + sizeof(uint64_t);
    thread->context.rdx = cursor + (2U + argc) * sizeof(uint64_t);
    return true;
}

static bool process_cleanup(struct native_process *process)
{
    bool success = true;
    const bool interrupts_were_enabled = cpu_interrupts_enabled();

    if (process == NULL) {
        return false;
    }
    cpu_interrupt_disable();
    if (process->address_space.state == PAGING_PROCESS_SPACE_ACTIVE &&
        paging_process_restore_kernel(&process->address_space) !=
            PAGING_STATUS_OK) {
        success = false;
    }
    cpu_interrupt_enable();
    if (process->handles.initialized &&
        native_handle_close_all(&process->handles, close_resource, process) !=
            NATIVE_HANDLE_OK) {
        success = false;
    }
    network_process_terminated(process->generation);
    cpu_interrupt_disable();
    audio_native_process_terminated(process->generation);
    for (size_t remaining = process->page_count; remaining > 0U; --remaining) {
        const struct native_page *page = &process->pages[remaining - 1U];

        if (page->mapped &&
            process->address_space.state != PAGING_PROCESS_SPACE_INVALID &&
            process->address_space.state != PAGING_PROCESS_SPACE_RELEASED &&
            paging_process_unmap_user_page(&process->address_space, page->kind,
                page->virtual_address) != PAGING_STATUS_OK) {
            success = false;
        }
    }
    if (process->aliases.active &&
        paging_process_alias_set_restore(&process->address_space,
            &process->aliases) != PAGING_STATUS_OK) {
        success = false;
    }
    if (process->address_space.state != PAGING_PROCESS_SPACE_INVALID &&
        process->address_space.state != PAGING_PROCESS_SPACE_RELEASED &&
        paging_process_space_release(&process->address_space) !=
            PAGING_STATUS_OK) {
        success = false;
    }
    for (size_t remaining = process->page_count; remaining > 0U; --remaining) {
        if (!release_page_frame(&process->pages[remaining - 1U])) {
            success = false;
        }
    }
    zero_bytes(process, sizeof(*process));
    if (interrupts_were_enabled) {
        cpu_interrupt_enable();
    }
    return success;
}

static enum native_process_status load_process(
    struct native_process *process,
    const char *manifest_path,
    enum phipfs_volume image_volume
)
{
    uint8_t manifest_bytes[NATIVE_MANIFEST_BYTES];
    struct phipfs_stat executable_stat;
    uint8_t *elf = NULL;
    char executable[PHIPFS_MAX_PATH];
    char data_namespace[PHIPFS_MAX_PATH];
    size_t manifest_read = 0U;
    size_t elf_read = 0U;
    size_t executable_length;
    size_t namespace_length;
    uint64_t main_thread_pointer = 0U;
    enum native_process_status result = NATIVE_PROCESS_OK;
    enum native_image_status admission_status;
    struct elf64_dynamic_image dynamic_image;
    struct native_dynamic_load *dynamic_load = NULL;
    uint64_t dynamic_start_entry = 0U;
    bool dynamic = false;

    if (!read_volume_file(image_volume, manifest_path, manifest_bytes,
            sizeof(manifest_bytes), &manifest_read) ||
        manifest_read != sizeof(manifest_bytes)) {
        return NATIVE_PROCESS_MANIFEST_READ;
    }
    /*
     * The executable path is needed before the Rust admission boundary can
     * authenticate the complete manifest/image pair.  Require the complete
     * current manifest magic before reading that path so an unrelated 1 KiB
     * file cannot steer the System-volume lookup.
     */
    if (manifest_bytes[0] != 'P' || manifest_bytes[1] != 'H' ||
            manifest_bytes[2] != 'I' || manifest_bytes[3] != 'P' ||
            manifest_bytes[4] != 'I' || manifest_bytes[5] != 'A' ||
            manifest_bytes[6] != 'A' || manifest_bytes[7] != '1') {
        return NATIVE_PROCESS_IMAGE_REFUSED;
    }
    executable_length = bounded_length(manifest_bytes + 112U, 16U);
    if (executable_length == 0U || executable_length >= 16U) {
        return NATIVE_PROCESS_IMAGE_REFUSED;
    }
    if (!sibling_image_path(manifest_path, manifest_bytes + 112U,
            executable_length, executable) ||
        phipfs_stat_path(image_volume, executable, &executable_stat) !=
            PHIPFS_STATUS_OK || executable_stat.directory ||
        executable_stat.size == 0U ||
        executable_stat.size > NATIVE_ELF_MAX_FILE_BYTES) {
        return NATIVE_PROCESS_EXECUTABLE_OPEN;
    }
    cpu_interrupt_disable();
    if (heap_allocate(executable_stat.size, (void **)&elf) != HEAP_STATUS_OK) {
        cpu_interrupt_enable();
        return NATIVE_PROCESS_MEMORY_LIMIT;
    }
    cpu_interrupt_enable();
    if (!read_volume_file(image_volume, executable, elf, executable_stat.size,
            &elf_read) ||
        elf_read != executable_stat.size) {
        result = NATIVE_PROCESS_EXECUTABLE_READ;
        goto finish;
    }
    admission_status = phipia_native_image_validate(manifest_bytes,
        sizeof(manifest_bytes), elf, elf_read, &process->manifest,
        &process->image);
    if (admission_status != NATIVE_IMAGE_OK) {
        enum elf64_dynamic_status dynamic_status;

        if (image_volume != PHIPFS_VOLUME_SYSTEM ||
            admission_status != NATIVE_IMAGE_ELF_TYPE ||
            phipia_native_manifest_authenticate(manifest_bytes,
                sizeof(manifest_bytes), elf, elf_read,
                &process->manifest) != NATIVE_IMAGE_OK) {
            console_write("Phipia: native admission status ");
            console_write_u64((uint64_t)admission_status);
            console_putc('\n');
            result = NATIVE_PROCESS_IMAGE_REFUSED;
            goto finish;
        }
        dynamic_status = phipia_elf64_dynamic_parse(elf, elf_read,
            &dynamic_image);
        if (dynamic_status != ELF64_DYNAMIC_OK) {
            console_write("Phipia: dynamic ELF admission status ");
            console_write_u64((uint64_t)dynamic_status);
            console_putc('\n');
            result = NATIVE_PROCESS_IMAGE_REFUSED;
            goto finish;
        }
        if (!dynamic_object_supported(process, &dynamic_image, true)) {
            console_write("Phipia: dynamic ELF root policy refused\n");
            result = NATIVE_PROCESS_IMAGE_REFUSED;
            goto finish;
        }
        result = dynamic_load_create(process, &dynamic_image, elf, elf_read,
            &dynamic_load);
        if (result != NATIVE_PROCESS_OK) {
            console_write("Phipia: dynamic ELF dependency scope refused\n");
            goto finish;
        }
        dynamic = true;
    }
    namespace_length = bounded_length(process->manifest.data_namespace,
        sizeof(process->manifest.data_namespace));
    zero_bytes(data_namespace, sizeof(data_namespace));
    copy_bytes(data_namespace, process->manifest.data_namespace,
        namespace_length);
    {
        const enum phipfs_status mkdir_status = phipfs_mkdir(PHIPFS_VOLUME_DATA,
            data_namespace);

        if (mkdir_status != PHIPFS_STATUS_OK &&
            mkdir_status != PHIPFS_STATUS_EXISTS) {
            result = NATIVE_PROCESS_DATA_NAMESPACE;
            goto finish;
        }
    }
    cpu_interrupt_disable();
    if (dynamic) {
        result = prepare_dynamic_image_pages(process, dynamic_load,
            &dynamic_start_entry);
        if (result != NATIVE_PROCESS_OK) {
            goto finish_disabled;
        }
    } else if (!prepare_image_pages(process, elf, elf_read)) {
        result = NATIVE_PROCESS_FRAME_ALLOCATION;
        goto finish_disabled;
    }
    if (!(dynamic ? prepare_dynamic_main_tls(process, &main_thread_pointer) :
            prepare_main_tls(process, elf, elf_read, &main_thread_pointer)) ||
        !prepare_main_stack(process)) {
        result = NATIVE_PROCESS_FRAME_ALLOCATION;
        goto finish_disabled;
    }
    if (!map_prepared_pages(process)) {
        result = NATIVE_PROCESS_MAPPING;
        goto finish_disabled;
    }
    process->thread_count = 1U;
    process->current_thread = 0U;
    process->threads[0].generation = next_thread_generation++;
    if (next_thread_generation == 0U) {
        next_thread_generation = 1U;
    }
    process->threads[0].state = NATIVE_THREAD_RUNNABLE;
    process->threads[0].stack_base = NATIVE_MAIN_STACK_BASE;
    process->threads[0].stack_end = NATIVE_MAIN_STACK_END;
    process->threads[0].fs_base = main_thread_pointer;
    if (!native_fpu_state_initialize(&process->threads[0].fpu) ||
        !initialize_stack(process, &process->threads[0])) {
        result = NATIVE_PROCESS_STACK;
        goto finish_disabled;
    }
    if (dynamic) {
        process->threads[0].context.rip = dynamic_start_entry;
    }
    if (native_handle_table_initialize(&process->handles,
            process->manifest.max_handles) != NATIVE_HANDLE_OK) {
        result = NATIVE_PROCESS_MEMORY_LIMIT;
        goto finish_disabled;
    }
    process->active = true;
    if (dynamic && process->shared_code_reuses != 0U) {
        console_write("Phipia: dynamic immutable RX shared pages ");
        console_write_u64(process->shared_code_reuses);
        console_putc('\n');
    }
finish_disabled:
    cpu_interrupt_enable();
finish:
    cpu_interrupt_disable();
    if (!dynamic_load_release(&dynamic_load) &&
        result == NATIVE_PROCESS_OK) {
        result = NATIVE_PROCESS_TEARDOWN;
    }
    if (elf != NULL && heap_free(elf) != HEAP_STATUS_OK &&
        result == NATIVE_PROCESS_OK) {
        result = NATIVE_PROCESS_TEARDOWN;
    }
    cpu_interrupt_enable();
    return result;
}

static enum native_process_status native_process_spawn_from_volume(
    const char *manifest_path,
    enum phipfs_volume image_volume,
    uint64_t *generation
)
{
    struct native_process *process = NULL;
    enum native_process_status status;

    if (manifest_path == NULL || generation == NULL ||
        (image_volume != PHIPFS_VOLUME_SYSTEM &&
         image_volume != PHIPFS_VOLUME_DATA)) {
        return NATIVE_PROCESS_NULL_ARGUMENT;
    }
    *generation = 0U;
    if (scheduler_active) {
        return NATIVE_PROCESS_BUSY;
    }
    if (!native_fpu_initialize()) {
        return NATIVE_PROCESS_CPU;
    }
    for (size_t index = 0U; index < NATIVE_PROCESS_LIMIT; ++index) {
        if (!processes[index].active && processes[index].generation == 0U) {
            process = &processes[index];
            break;
        }
    }
    if (process == NULL) {
        return NATIVE_PROCESS_NO_SLOT;
    }
    zero_bytes(process, sizeof(*process));
    process->generation = next_process_generation++;
    if (next_process_generation == 0U) {
        next_process_generation = 1U;
    }
    status = load_process(process, manifest_path, image_volume);
    if (status != NATIVE_PROCESS_OK) {
        if (!process_cleanup(process)) {
            return NATIVE_PROCESS_TEARDOWN;
        }
        return status;
    }
    *generation = process->generation;
    return NATIVE_PROCESS_OK;
}

enum native_process_status native_process_spawn(
    const char *manifest_path,
    uint64_t *generation
)
{
    return native_process_spawn_from_volume(manifest_path,
        PHIPFS_VOLUME_SYSTEM, generation);
}

static int64_t syscall_console_write(
    struct native_process *process,
    uint64_t address,
    size_t length
)
{
    size_t completed = 0U;

    if ((process->manifest.capabilities & PHIPIA_CAP_CONSOLE) == 0U) {
        return -PHIPIA_EACCES;
    }
    if (length == 0U) {
        return 0;
    }
    if (!validate_user_range(process, address, length, false)) {
        return -PHIPIA_EFAULT;
    }
    while (completed < length) {
        size_t chunk = length - completed;

        if (chunk > sizeof(process->transfer)) {
            chunk = sizeof(process->transfer);
        }
        if (!copy_from_user(process, process->transfer, address + completed,
                chunk)) {
            return completed == 0U ? -PHIPIA_EFAULT : (int64_t)completed;
        }
        console_write_n((const char *)process->transfer, chunk);
        completed += chunk;
    }
    return (int64_t)completed;
}

static size_t console_input_copy(
    const struct native_process *process,
    uint8_t *destination,
    size_t capacity
)
{
    size_t length = process->console_input_count;

    if (length > capacity) {
        length = capacity;
    }
    for (size_t index = 0U; index < length; ++index) {
        destination[index] = process->console_input[
            (process->console_input_head + index) %
                NATIVE_CONSOLE_INPUT_CAPACITY];
    }
    return length;
}

static void console_input_consume(
    struct native_process *process,
    size_t length
)
{
    process->console_input_head = (process->console_input_head + length) %
        NATIVE_CONSOLE_INPUT_CAPACITY;
    process->console_input_count -= length;
}

static int64_t syscall_console_read(
    struct native_process *process,
    uint64_t address,
    size_t length
)
{
    struct native_thread *thread = running_thread(process);
    size_t copied;

    if ((process->manifest.capabilities & PHIPIA_CAP_CONSOLE) == 0U) {
        return -PHIPIA_EACCES;
    }
    if (thread == NULL) {
        return -PHIPIA_EIO;
    }
    if (length == 0U) {
        return 0;
    }
    if (length > sizeof(process->transfer)) {
        return -PHIPIA_EINVAL;
    }
    if (!validate_user_range(process, address, length, true)) {
        return -PHIPIA_EFAULT;
    }
    if (process->console_input_count == 0U) {
        thread->console_address = address;
        thread->console_length = length;
        thread->state = NATIVE_THREAD_CONSOLE_WAIT;
        return 0;
    }
    copied = console_input_copy(process, process->transfer, length);
    if (!copy_to_user(process, address, process->transfer, copied)) {
        return -PHIPIA_EFAULT;
    }
    console_input_consume(process, copied);
    return (int64_t)copied;
}

static bool anonymous_span_free(
    struct native_process *process,
    uint64_t base,
    size_t page_count,
    bool guard_before,
    bool guard_after
)
{
    const uint64_t first = guard_before ? base - PAGING_PAGE_SIZE : base;
    const size_t total = page_count + (guard_before ? 1U : 0U) +
        (guard_after ? 1U : 0U);

    if (base < PAGING_NATIVE_ANON_BASE ||
        page_count > (PAGING_NATIVE_ANON_END - base) / PAGING_PAGE_SIZE ||
        first < PAGING_NATIVE_ANON_BASE ||
        total > (PAGING_NATIVE_ANON_END - first) / PAGING_PAGE_SIZE) {
        return false;
    }
    for (size_t page = 0U; page < total; ++page) {
        if (page_at(process, first + page * PAGING_PAGE_SIZE) != NULL) {
            return false;
        }
    }
    return true;
}

static int64_t syscall_memory_unmap(
    struct native_process *process,
    uint64_t address,
    uint64_t length
);

static int64_t syscall_memory_map(
    struct native_process *process,
    uint64_t request_address,
    uint64_t response_address
)
{
    struct phipia_memory_map_request request;
    struct phipia_memory_map_response response = {
        sizeof(response), PHIPIA_ABI_VERSION, 0U, 0U
    };
    size_t page_count;
    uint64_t length;
    uint64_t base;
    bool guard_before_flag;
    bool guard_after_flag;

    if (!copy_from_user(process, &request, request_address,
            sizeof(request)) ||
        !validate_user_range(process, response_address, sizeof(response),
            true)) {
        return -PHIPIA_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != PHIPIA_ABI_VERSION || request.reserved != 0U ||
        (request.flags & ~PHIPIA_MEMORY_FLAGS_V1) != 0U ||
        (request.flags & (PHIPIA_MEMORY_READ | PHIPIA_MEMORY_WRITE)) !=
            (PHIPIA_MEMORY_READ | PHIPIA_MEMORY_WRITE) ||
        request.length == 0U || request.length > UINT64_MAX -
            (PAGING_PAGE_SIZE - 1U)) {
        return -PHIPIA_EINVAL;
    }
    guard_before_flag =
        (request.flags & PHIPIA_MEMORY_GUARD_BEFORE) != 0U;
    guard_after_flag =
        (request.flags & PHIPIA_MEMORY_GUARD_AFTER) != 0U;
    length = (request.length + PAGING_PAGE_SIZE - 1U) &
        ~(PAGING_PAGE_SIZE - 1U);
    page_count = (size_t)(length / PAGING_PAGE_SIZE);
    if (page_count == 0U || page_count > NATIVE_PROCESS_PAGE_LIMIT ||
        process->page_count > NATIVE_PROCESS_PAGE_LIMIT - page_count ||
        (process->page_count + page_count) * PAGING_PAGE_SIZE >
            process->manifest.memory_limit) {
        return -PHIPIA_ENOMEM;
    }
    if (request.address_hint != 0U) {
        if ((request.address_hint & (PAGING_PAGE_SIZE - 1U)) != 0U) {
            return -PHIPIA_EINVAL;
        }
        base = request.address_hint;
        if (!anonymous_span_free(process, base, page_count,
                guard_before_flag, guard_after_flag)) {
            return -PHIPIA_EBUSY;
        }
    } else {
        const size_t prefix = guard_before_flag ? 1U : 0U;
        const size_t total = page_count + prefix +
            (guard_after_flag ? 1U : 0U);
        uint64_t candidate = PAGING_NATIVE_ANON_BASE +
            prefix * PAGING_PAGE_SIZE;

        base = 0U;
        while (candidate < PAGING_NATIVE_ANON_END &&
            total <= (PAGING_NATIVE_ANON_END -
                (candidate - prefix * PAGING_PAGE_SIZE)) /
                    PAGING_PAGE_SIZE) {
            if (anonymous_span_free(process, candidate, page_count,
                    guard_before_flag, guard_after_flag)) {
                base = candidate;
                break;
            }
            candidate += PAGING_PAGE_SIZE;
        }
        if (base == 0U) {
            return -PHIPIA_ENOMEM;
        }
    }
    for (size_t page = 0U; page < page_count; ++page) {
        uintptr_t physical_address;
        const uint64_t address = base + page * PAGING_PAGE_SIZE;

        if (!allocate_page(process, address, PAGING_WRITE,
                PAGING_PROCESS_MAPPING_NATIVE_ANON, &physical_address) ||
            paging_process_map_user_page(&process->address_space,
                PAGING_PROCESS_MAPPING_NATIVE_ANON, address,
                physical_address, PAGING_WRITE) != PAGING_STATUS_OK) {
            for (size_t rollback = 0U; rollback <= page; ++rollback) {
                struct native_page removed;
                const uint64_t rollback_address = base +
                    rollback * PAGING_PAGE_SIZE;
                struct native_page *record = page_at(process,
                    rollback_address);

                if (record == NULL) {
                    continue;
                }
                if (record->mapped) {
                    (void)paging_process_unmap_user_page(
                        &process->address_space,
                        PAGING_PROCESS_MAPPING_NATIVE_ANON,
                        rollback_address);
                }
                if (remove_page_record(process, rollback_address, &removed)) {
                    (void)release_page_frame(&removed);
                }
            }
            return -PHIPIA_ENOMEM;
        }
        page_at(process, address)->mapped = true;
    }
    response.address = base;
    response.length = length;
    if (!copy_to_user(process, response_address, &response, sizeof(response))) {
        (void)syscall_memory_unmap(process, base, length);
        return -PHIPIA_EFAULT;
    }
    return 0;
}

static int64_t syscall_memory_unmap(
    struct native_process *process,
    uint64_t address,
    uint64_t length
)
{
    size_t page_count;

    if (length == 0U || (address & (PAGING_PAGE_SIZE - 1U)) != 0U ||
        (length & (PAGING_PAGE_SIZE - 1U)) != 0U ||
        address < PAGING_NATIVE_ANON_BASE ||
        length > PAGING_NATIVE_ANON_END - address) {
        return -PHIPIA_EINVAL;
    }
    page_count = (size_t)(length / PAGING_PAGE_SIZE);
    for (size_t page = 0U; page < page_count; ++page) {
        const struct native_page *record = page_at(process,
            address + page * PAGING_PAGE_SIZE);

        if (record == NULL || !record->mapped ||
            record->kind != PAGING_PROCESS_MAPPING_NATIVE_ANON) {
            return -PHIPIA_EFAULT;
        }
    }
    for (size_t page = 0U; page < page_count; ++page) {
        struct native_page removed;
        const uint64_t page_address = address + page * PAGING_PAGE_SIZE;

        if (paging_process_unmap_user_page(&process->address_space,
                PAGING_PROCESS_MAPPING_NATIVE_ANON, page_address) !=
                PAGING_STATUS_OK ||
            !remove_page_record(process, page_address, &removed) ||
            !release_page_frame(&removed)) {
            process->faulted = true;
            process->exiting = true;
            return -PHIPIA_EIO;
        }
    }
    return 0;
}

static int64_t syscall_file_open(
    struct native_process *process,
    uint64_t request_address
)
{
    struct phipia_file_open_request request;
    struct native_resource resource = {{0U, 0U, 0U, 0U}};
    char path[PHIPFS_MAX_PATH];
    enum phipfs_volume volume;
    enum phipfs_access access;
    phipfs_handle file;
    phipia_handle_t handle;
    enum phipfs_status status;

    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -PHIPIA_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != PHIPIA_ABI_VERSION || request.reserved != 0U ||
        (request.flags & ~PHIPIA_OPEN_FLAGS_V1) != 0U ||
        (request.flags & (PHIPIA_OPEN_READ | PHIPIA_OPEN_WRITE)) == 0U ||
        !path_from_user(process, &request.path, path, &volume)) {
        return -PHIPIA_EINVAL;
    }
    if (volume == PHIPFS_VOLUME_SYSTEM &&
        (request.flags & (PHIPIA_OPEN_WRITE | PHIPIA_OPEN_CREATE |
            PHIPIA_OPEN_TRUNCATE)) != 0U) {
        return -PHIPIA_EACCES;
    }
    if ((request.flags & (PHIPIA_OPEN_WRITE | PHIPIA_OPEN_CREATE |
            PHIPIA_OPEN_TRUNCATE)) != 0U &&
        (process->manifest.capabilities & PHIPIA_CAP_DATA_WRITE) == 0U) {
        return -PHIPIA_EACCES;
    }
    access = (request.flags & PHIPIA_OPEN_WRITE) != 0U ?
        ((request.flags & PHIPIA_OPEN_READ) != 0U ? PHIPFS_ACCESS_READ_WRITE :
            PHIPFS_ACCESS_WRITE) : PHIPFS_ACCESS_READ;
    cpu_interrupt_enable();
    status = phipfs_stat_path(volume, path, &(struct phipfs_stat){0});
    if (status == PHIPFS_STATUS_NOT_FOUND &&
        (request.flags & PHIPIA_OPEN_CREATE) != 0U) {
        status = phipfs_create(volume, path);
    }
    if (status == PHIPFS_STATUS_OK &&
        (request.flags & PHIPIA_OPEN_TRUNCATE) != 0U) {
        status = phipfs_truncate(volume, path, 0U);
    }
    if (status == PHIPFS_STATUS_OK) {
        status = phipfs_open(volume, path, access, &file);
    }
    cpu_interrupt_disable();
    if (status != PHIPFS_STATUS_OK) {
        return filesystem_error(status);
    }
    resource.words[0] = file;
    {
        const enum native_handle_status handle_status = native_handle_install(
            &process->handles, PHIPIA_HANDLE_FILE, &resource, &handle);

        if (handle_status != NATIVE_HANDLE_OK) {
            cpu_interrupt_enable();
            (void)phipfs_close(file);
            cpu_interrupt_disable();
            return handle_error(handle_status);
        }
    }
    if (process->handles.active_handles > process->peak_handles) {
        process->peak_handles = process->handles.active_handles;
    }
    return (int64_t)handle;
}

static int64_t syscall_file_io(
    struct native_process *process,
    uint64_t request_address,
    bool write
)
{
    struct phipia_io_request request;
    struct native_resource *resource;
    size_t completed = 0U;
    enum native_handle_status handle_status;

    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -PHIPIA_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != PHIPIA_ABI_VERSION || request.flags != 0U) {
        return -PHIPIA_EINVAL;
    }
    handle_status = native_handle_resolve(&process->handles, request.handle,
        PHIPIA_HANDLE_FILE, &resource);
    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    if (request.length == 0U) {
        return 0;
    }
    if (!validate_user_range(process, request.buffer, request.length, !write)) {
        return -PHIPIA_EFAULT;
    }
    if (write && request.offset != UINT64_MAX) {
        uint64_t position;

        if (request.offset > INT64_MAX) {
            return -PHIPIA_EINVAL;
        }
        cpu_interrupt_enable();
        const enum phipfs_status seek_status = phipfs_seek(
            (phipfs_handle)resource->words[0], (int64_t)request.offset,
            PHIPFS_SEEK_START, &position);
        cpu_interrupt_disable();
        if (seek_status != PHIPFS_STATUS_OK) {
            return filesystem_error(seek_status);
        }
    }
    while (completed < request.length) {
        size_t chunk = request.length - completed;
        size_t transferred = 0U;
        enum phipfs_status status;

        if (chunk > sizeof(process->transfer)) {
            chunk = sizeof(process->transfer);
        }
        if (write && !copy_from_user(process, process->transfer,
                request.buffer + completed, chunk)) {
            return completed == 0U ? -PHIPIA_EFAULT : (int64_t)completed;
        }
        cpu_interrupt_enable();
        if (write) {
            status = phipfs_write((phipfs_handle)resource->words[0],
                process->transfer, chunk, &transferred);
        } else if (request.offset != UINT64_MAX) {
            if (completed > UINT64_MAX - request.offset) {
                cpu_interrupt_disable();
                return completed == 0U ? -PHIPIA_EINVAL : (int64_t)completed;
            }
            status = phipfs_pread((phipfs_handle)resource->words[0],
                process->transfer, chunk, request.offset + completed,
                &transferred);
        } else {
            status = phipfs_read((phipfs_handle)resource->words[0],
                process->transfer, chunk, &transferred);
        }
        cpu_interrupt_disable();
        if (status != PHIPFS_STATUS_OK) {
            return completed == 0U ? filesystem_error(status) :
                (int64_t)completed;
        }
        if (!write && transferred != 0U &&
            !copy_to_user(process, request.buffer + completed,
                process->transfer, transferred)) {
            return completed == 0U ? -PHIPIA_EFAULT : (int64_t)completed;
        }
        completed += transferred;
        if (transferred < chunk) {
            break;
        }
    }
    return (int64_t)completed;
}

static int64_t syscall_file_seek(
    struct native_process *process,
    uint64_t request_address
)
{
    struct phipia_seek_request request;
    struct native_resource *resource;
    enum phipfs_seek_origin origin;
    uint64_t position;
    enum phipfs_status status;

    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -PHIPIA_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != PHIPIA_ABI_VERSION || request.reserved != 0U ||
        request.origin > PHIPIA_SEEK_END) {
        return -PHIPIA_EINVAL;
    }
    if (native_handle_resolve(&process->handles, request.handle,
            PHIPIA_HANDLE_FILE, &resource) != NATIVE_HANDLE_OK) {
        return -PHIPIA_EBADF;
    }
    origin = request.origin == PHIPIA_SEEK_START ? PHIPFS_SEEK_START :
        (request.origin == PHIPIA_SEEK_CURRENT ? PHIPFS_SEEK_CURRENT :
            PHIPFS_SEEK_END);
    cpu_interrupt_enable();
    status = phipfs_seek((phipfs_handle)resource->words[0], request.offset,
        origin, &position);
    cpu_interrupt_disable();
    return status == PHIPFS_STATUS_OK ? (int64_t)position :
        filesystem_error(status);
}

static int64_t syscall_path_stat(
    struct native_process *process,
    uint64_t path_address,
    uint64_t output_address
)
{
    struct phipia_path path_request;
    struct phipia_path_stat output = {
        sizeof(output), PHIPIA_ABI_VERSION, 0U, 0U, 0U
    };
    struct phipfs_stat stat;
    char path[PHIPFS_MAX_PATH];
    enum phipfs_volume volume;
    enum phipfs_status status;

    if (!copy_from_user(process, &path_request, path_address,
            sizeof(path_request)) ||
        !validate_user_range(process, output_address, sizeof(output), true)) {
        return -PHIPIA_EFAULT;
    }
    if (!path_from_user(process, &path_request, path, &volume)) {
        return -PHIPIA_EINVAL;
    }
    cpu_interrupt_enable();
    status = phipfs_stat_path(volume, path, &stat);
    cpu_interrupt_disable();
    if (status != PHIPFS_STATUS_OK) {
        return filesystem_error(status);
    }
    output.byte_length = stat.size;
    output.attributes = (stat.directory ? PHIPIA_PATH_DIRECTORY : 0U) |
        (stat.read_only ? PHIPIA_PATH_READ_ONLY : 0U);
    return copy_to_user(process, output_address, &output, sizeof(output)) ?
        0 : -PHIPIA_EFAULT;
}

static int64_t syscall_directory_open(
    struct native_process *process,
    uint64_t path_address
)
{
    struct phipia_path path_request;
    struct native_resource resource = {{0U, 0U, 0U, 0U}};
    char path[PHIPFS_MAX_PATH];
    enum phipfs_volume volume;
    enum phipfs_status status;
    phipfs_directory_handle iterator = 0U;
    phipia_handle_t handle;
    size_t slot = SIZE_MAX;

    if (!copy_from_user(process, &path_request, path_address,
            sizeof(path_request))) {
        return -PHIPIA_EFAULT;
    }
    if (!path_from_user(process, &path_request, path, &volume)) {
        return -PHIPIA_EINVAL;
    }
    for (size_t index = 0U; index < NATIVE_HANDLE_LIMIT; ++index) {
        if (!process->directories[index].active) {
            slot = index;
            break;
        }
    }
    if (slot == SIZE_MAX) {
        return -PHIPIA_ENOMEM;
    }
    cpu_interrupt_enable();
    status = phipfs_directory_open(volume, path, &iterator);
    cpu_interrupt_disable();
    if (status != PHIPFS_STATUS_OK) {
        return filesystem_error(status);
    }
    process->directories[slot].iterator = iterator;
    process->directories[slot].active = true;
    resource.words[0] = slot;
    {
        const enum native_handle_status handle_status = native_handle_install(
            &process->handles, PHIPIA_HANDLE_DIRECTORY, &resource, &handle);

        if (handle_status != NATIVE_HANDLE_OK) {
            (void)phipfs_directory_close(iterator);
            zero_bytes(&process->directories[slot],
                sizeof(process->directories[slot]));
            return handle_error(handle_status);
        }
    }
    if (process->handles.active_handles > process->peak_handles) {
        process->peak_handles = process->handles.active_handles;
    }
    return (int64_t)handle;
}

static int64_t syscall_directory_read(
    struct native_process *process,
    phipia_handle_t handle,
    uint64_t output_address
)
{
    struct native_resource *resource;
    struct phipfs_list_entry entry;
    struct phipia_directory_entry output;
    struct native_directory_resource *directory;
    bool present = false;
    enum phipfs_status status;

    if (!validate_user_range(process, output_address, sizeof(output), true)) {
        return -PHIPIA_EFAULT;
    }
    {
        const enum native_handle_status handle_status = native_handle_resolve(
            &process->handles, handle, PHIPIA_HANDLE_DIRECTORY, &resource);

        if (handle_status != NATIVE_HANDLE_OK) {
            return handle_error(handle_status);
        }
    }
    if (resource->words[0] >= NATIVE_HANDLE_LIMIT) {
        return -PHIPIA_EBADF;
    }
    directory = &process->directories[resource->words[0]];
    if (!directory->active) {
        return -PHIPIA_ESTALE;
    }
    cpu_interrupt_enable();
    status = phipfs_directory_read(directory->iterator, &entry, &present);
    cpu_interrupt_disable();
    if (status != PHIPFS_STATUS_OK) {
        return filesystem_error(status);
    }
    if (!present) {
        return 0;
    }
    zero_bytes(&output, sizeof(output));
    output.size = sizeof(output);
    output.version = PHIPIA_ABI_VERSION;
    output.byte_length = entry.size;
    output.attributes = entry.directory ?
        PHIPIA_PATH_DIRECTORY : 0U;
    output.name_length = (uint16_t)bounded_length(
        (const uint8_t *)entry.name, sizeof(entry.name));
    if (output.name_length > sizeof(output.name)) {
        return -PHIPIA_EIO;
    }
    copy_bytes(output.name, entry.name, output.name_length);
    if (!copy_to_user(process, output_address, &output, sizeof(output))) {
        return -PHIPIA_EFAULT;
    }
    return 1;
}

static int64_t syscall_single_path_mutation(
    struct native_process *process,
    uint64_t path_address,
    uint64_t value,
    uint64_t number
)
{
    struct phipia_path path_request;
    struct phipfs_stat stat;
    char path[PHIPFS_MAX_PATH];
    enum phipfs_volume volume;
    enum phipfs_status status;

    if (!copy_from_user(process, &path_request, path_address,
            sizeof(path_request))) {
        return -PHIPIA_EFAULT;
    }
    if (!path_from_user(process, &path_request, path, &volume)) {
        return -PHIPIA_EINVAL;
    }
    if (volume != PHIPFS_VOLUME_DATA ||
        (process->manifest.capabilities & PHIPIA_CAP_DATA_WRITE) == 0U) {
        return -PHIPIA_EACCES;
    }
    cpu_interrupt_enable();
    if (number == PHIPIA_SYS_PATH_MKDIR) {
        status = phipfs_mkdir(volume, path);
    } else if (number == PHIPIA_SYS_PATH_TRUNCATE) {
        status = phipfs_truncate(volume, path, value);
    } else {
        status = phipfs_stat_path(volume, path, &stat);
        if (status == PHIPFS_STATUS_OK) {
            status = stat.directory ? phipfs_rmdir(volume, path) :
                phipfs_unlink(volume, path);
        }
    }
    cpu_interrupt_disable();
    return filesystem_error(status);
}

static bool replacement_backup_path(
    const char *destination,
    char backup[PHIPFS_MAX_PATH]
)
{
    size_t length = bounded_length((const uint8_t *)destination,
        PHIPFS_MAX_PATH);
    size_t slash = SIZE_MAX;

    if (length == PHIPFS_MAX_PATH) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        if (destination[index] == '/') {
            slash = index;
        }
    }
    zero_bytes(backup, PHIPFS_MAX_PATH);
    if (slash != SIZE_MAX) {
        if (slash + 1U + 10U >= PHIPFS_MAX_PATH) {
            return false;
        }
        copy_bytes(backup, destination, slash + 1U);
        copy_bytes(backup + slash + 1U, "PHIPBAK.TMP", 11U);
    } else {
        copy_bytes(backup, "PHIPBAK.TMP", 11U);
    }
    return true;
}

static int64_t syscall_rename(
    struct native_process *process,
    uint64_t request_address,
    bool replace
)
{
    struct phipia_rename_request request;
    struct phipfs_stat destination_stat;
    struct phipfs_stat backup_stat;
    char source[PHIPFS_MAX_PATH];
    char destination[PHIPFS_MAX_PATH];
    char backup[PHIPFS_MAX_PATH];
    enum phipfs_volume source_volume;
    enum phipfs_volume destination_volume;
    enum phipfs_status status;

    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -PHIPIA_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != PHIPIA_ABI_VERSION || request.flags != 0U ||
        request.reserved != 0U ||
        !path_from_user(process, &request.source, source, &source_volume) ||
        !path_from_user(process, &request.destination, destination,
            &destination_volume)) {
        return -PHIPIA_EINVAL;
    }
    if (source_volume != PHIPFS_VOLUME_DATA ||
        destination_volume != PHIPFS_VOLUME_DATA ||
        (process->manifest.capabilities & PHIPIA_CAP_DATA_WRITE) == 0U) {
        return -PHIPIA_EACCES;
    }
    cpu_interrupt_enable();
    status = phipfs_rename(PHIPFS_VOLUME_DATA, source, destination);
    if (status == PHIPFS_STATUS_EXISTS && replace &&
        replacement_backup_path(destination, backup) &&
        phipfs_stat_path(PHIPFS_VOLUME_DATA, destination, &destination_stat) ==
            PHIPFS_STATUS_OK && !destination_stat.directory &&
        phipfs_stat_path(PHIPFS_VOLUME_DATA, backup, &backup_stat) ==
            PHIPFS_STATUS_NOT_FOUND) {
        status = phipfs_rename(PHIPFS_VOLUME_DATA, destination, backup);
        if (status == PHIPFS_STATUS_OK) {
            status = phipfs_rename(PHIPFS_VOLUME_DATA, source, destination);
            if (status == PHIPFS_STATUS_OK) {
                status = phipfs_unlink(PHIPFS_VOLUME_DATA, backup);
            } else {
                (void)phipfs_rename(PHIPFS_VOLUME_DATA, backup, destination);
            }
        }
    }
    cpu_interrupt_disable();
    return filesystem_error(status);
}

static int64_t syscall_volume_sync(
    struct native_process *process,
    uint64_t volume_number
)
{
    enum phipfs_volume volume;

    if (volume_number == PHIPIA_VOLUME_SYSTEM) {
        if ((process->manifest.capabilities & PHIPIA_CAP_SYSTEM_READ) == 0U) {
            return -PHIPIA_EACCES;
        }
        volume = PHIPFS_VOLUME_SYSTEM;
    } else if (volume_number == PHIPIA_VOLUME_DATA) {
        if ((process->manifest.capabilities & PHIPIA_CAP_DATA_WRITE) == 0U) {
            return -PHIPIA_EACCES;
        }
        volume = PHIPFS_VOLUME_DATA;
    } else {
        return -PHIPIA_EINVAL;
    }
    cpu_interrupt_enable();
    const enum phipfs_status status = phipfs_sync(volume);
    cpu_interrupt_disable();
    return filesystem_error(status);
}

static int64_t syscall_volume_space(
    struct native_process *process,
    uint64_t volume_number,
    uint64_t output_address
)
{
    struct phipia_volume_space output = {
        sizeof(output), PHIPIA_ABI_VERSION, 0U, 0U,
        (uint32_t)PAGING_PAGE_SIZE, 0U
    };
    enum phipfs_volume volume;
    struct phipfs_drive_info drive;

    if (!validate_user_range(process, output_address, sizeof(output), true)) {
        return -PHIPIA_EFAULT;
    }
    if (volume_number == PHIPIA_VOLUME_SYSTEM &&
        (process->manifest.capabilities & PHIPIA_CAP_SYSTEM_READ) != 0U) {
        volume = PHIPFS_VOLUME_SYSTEM;
    } else if (volume_number == PHIPIA_VOLUME_DATA &&
        (process->manifest.capabilities & PHIPIA_CAP_DATA_READ) != 0U) {
        volume = PHIPFS_VOLUME_DATA;
    } else {
        return -PHIPIA_EACCES;
    }
    drive = phipfs_drive(volume);
    if (!drive.mounted || !drive.healthy) {
        return -PHIPIA_EIO;
    }
    output.total_bytes = drive.total_bytes;
    output.free_bytes = drive.free_bytes;
    return copy_to_user(process, output_address, &output, sizeof(output)) ?
        0 : -PHIPIA_EFAULT;
}

static int64_t network_error(enum network_status status)
{
    switch (status) {
    case NETWORK_STATUS_OK:
        return 0;
    case NETWORK_STATUS_TIMEOUT:
        return -PHIPIA_ETIMEDOUT;
    case NETWORK_STATUS_CANCELLED:
        return -PHIPIA_ECANCELED;
    case NETWORK_STATUS_WOULD_BLOCK:
        return -PHIPIA_EAGAIN;
    case NETWORK_STATUS_NO_RESOURCES:
        return -PHIPIA_ENOMEM;
    case NETWORK_STATUS_STALE_HANDLE:
        return -PHIPIA_ESTALE;
    case NETWORK_STATUS_WRONG_OWNER:
    case NETWORK_STATUS_WRONG_MODE:
        return -PHIPIA_EBADF;
    case NETWORK_STATUS_ALREADY_BOUND:
    case NETWORK_STATUS_PORT_IN_USE:
        return -PHIPIA_EBUSY;
    case NETWORK_STATUS_CONNECTION_CLOSED:
        return -PHIPIA_EPIPE;
    case NETWORK_STATUS_RESET:
    case NETWORK_STATUS_CONNECTION_RESET:
        return -PHIPIA_EIO;
    case NETWORK_STATUS_INVALID_ARGUMENT:
    case NETWORK_STATUS_RANGE:
    case NETWORK_STATUS_NULL_ARGUMENT:
        return -PHIPIA_EINVAL;
    case NETWORK_STATUS_UNSUPPORTED:
        return -PHIPIA_ENOTSUP;
    case NETWORK_STATUS_UNAVAILABLE:
    case NETWORK_STATUS_LINK_DOWN:
    case NETWORK_STATUS_UNCONFIGURED:
    case NETWORK_STATUS_NOT_INITIALIZED:
    case NETWORK_STATUS_UNREACHABLE:
    case NETWORK_STATUS_DHCP_NAK:
    case NETWORK_STATUS_DNS_FAILURE:
    case NETWORK_STATUS_MALFORMED:
    case NETWORK_STATUS_CHECKSUM:
    case NETWORK_STATUS_FRAGMENTED:
    case NETWORK_STATUS_HTTP_FAILURE:
    case NETWORK_STATUS_FILESYSTEM:
    case NETWORK_STATUS_TOO_LARGE:
    case NETWORK_STATUS_ALREADY_INITIALIZED:
    case NETWORK_STATUS_COUNT:
    default:
        return -PHIPIA_EIO;
    }
}

static bool deadline_timeout(uint64_t deadline, uint64_t *timeout)
{
    const uint64_t now = clock_monotonic_ns();

    if (timeout == NULL || deadline <= now) {
        return false;
    }
    *timeout = deadline - now;
    if (*timeout > UINT64_C(30000000000)) {
        *timeout = UINT64_C(30000000000);
    }
    return true;
}

static enum network_status prepare_native_network(
    uint64_t deadline,
    uint64_t *timeout
)
{
    enum network_status status;

    if (!deadline_timeout(deadline, timeout)) {
        return NETWORK_STATUS_TIMEOUT;
    }
    if (network_get_state().configuration.configured) {
        return NETWORK_STATUS_OK;
    }
    status = network_start_dhcp(*timeout);
    if (status != NETWORK_STATUS_OK) {
        return status;
    }
    return deadline_timeout(deadline, timeout) ? NETWORK_STATUS_OK :
        NETWORK_STATUS_TIMEOUT;
}

static int64_t syscall_random(
    struct native_process *process,
    uint64_t address,
    size_t length,
    bool require_strong
)
{
    size_t completed = 0U;

    if ((process->manifest.capabilities & PHIPIA_CAP_ENTROPY) == 0U) {
        return -PHIPIA_EACCES;
    }
    if (length > RANDOM_MAX_REQUEST_BYTES) {
        return -PHIPIA_EINVAL;
    }
    if (length == 0U) {
        return 0;
    }
    if (!validate_user_range(process, address, length, true)) {
        return -PHIPIA_EFAULT;
    }
    while (completed < length) {
        size_t chunk = length - completed;

        if (chunk > RANDOM_MAX_REQUEST_BYTES) {
            chunk = RANDOM_MAX_REQUEST_BYTES;
        }
        const enum random_status status = require_strong ?
            random_strong_bytes(process->transfer, chunk) :
            random_bytes(process->transfer, chunk);

        if (status != RANDOM_STATUS_OK ||
            !copy_to_user(process, address + completed, process->transfer,
                chunk)) {
            return completed == 0U ? -PHIPIA_EIO : (int64_t)completed;
        }
        completed += chunk;
    }
    return (int64_t)completed;
}

static int64_t syscall_time_realtime(const struct native_process *process)
{
    int64_t seconds;

    if ((process->manifest.capabilities & PHIPIA_CAP_TIME) == 0U) {
        return -PHIPIA_EACCES;
    }
    return wall_clock_read_unix_seconds(&seconds) == WALL_CLOCK_STATUS_OK ?
        seconds : -PHIPIA_EIO;
}

static int64_t syscall_timer_create(struct native_process *process)
{
    struct native_resource resource = {{0U, 0U, 0U, 0U}};
    phipia_handle_t handle;
    const enum native_handle_status status = native_handle_install(
        &process->handles, PHIPIA_HANDLE_TIMER, &resource, &handle);

    if (status != NATIVE_HANDLE_OK) {
        return handle_error(status);
    }
    if (process->handles.active_handles > process->peak_handles) {
        process->peak_handles = process->handles.active_handles;
    }
    return (int64_t)handle;
}

static int64_t syscall_timer_set(
    struct native_process *process,
    uint64_t request_address
)
{
    struct phipia_timer_set_request request;
    struct native_resource *resource;
    enum native_handle_status status;

    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -PHIPIA_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != PHIPIA_ABI_VERSION || request.flags != 0U ||
        request.reserved != 0U) {
        return -PHIPIA_EINVAL;
    }
    status = native_handle_resolve(&process->handles, request.handle,
        PHIPIA_HANDLE_TIMER, &resource);
    if (status != NATIVE_HANDLE_OK) {
        return handle_error(status);
    }
    resource->words[0] = request.deadline_ns;
    return 0;
}

static int64_t syscall_sleep_until(
    struct native_process *process,
    uint64_t deadline
)
{
    struct native_thread *thread = running_thread(process);
    const uint64_t now = clock_monotonic_ns();

    if ((process->manifest.capabilities & PHIPIA_CAP_TIME) == 0U) {
        return -PHIPIA_EACCES;
    }
    if (thread == NULL) {
        return -PHIPIA_EIO;
    }
    if (deadline <= now) {
        return 0;
    }
    thread->deadline_ns = deadline;
    thread->state = NATIVE_THREAD_SLEEP_WAIT;
    return 0;
}

static int64_t poll_wait_items(
    struct native_process *process,
    struct phipia_wait_item *items,
    size_t count
)
{
    struct network_poll_request network_requests[PHIPIA_WAIT_MAX];
    struct network_poll_result network_results[PHIPIA_WAIT_MAX];
    size_t network_indices[PHIPIA_WAIT_MAX];
    size_t network_count = 0U;
    size_t ready_count = 0U;

    for (size_t index = 0U; index < count; ++index) {
        struct native_resource *resource;
        const enum native_handle_status status = native_handle_resolve(
            &process->handles, items[index].handle, 0U, &resource);
        const uint8_t type = status == NATIVE_HANDLE_OK ?
            (uint8_t)((items[index].handle >> 16U) & UINT64_C(0xFF)) : 0U;

        items[index].ready = 0U;
        if (status != NATIVE_HANDLE_OK) {
            return handle_error(status);
        }
        if ((items[index].interests & ~PHIPIA_WAIT_INTERESTS_V1) != 0U ||
            items[index].interests == 0U) {
            return -PHIPIA_EINVAL;
        }
        if (type == PHIPIA_HANDLE_FILE || type == PHIPIA_HANDLE_DIRECTORY) {
            items[index].ready = items[index].interests &
                (PHIPIA_WAIT_READABLE | PHIPIA_WAIT_WRITABLE);
        } else if (type == PHIPIA_HANDLE_TIMER) {
            if (resource->words[0] != 0U &&
                clock_monotonic_ns() >= resource->words[0]) {
                items[index].ready = items[index].interests &
                    PHIPIA_WAIT_SIGNALED;
            }
        } else if (type == PHIPIA_HANDLE_EVENT_QUEUE) {
            if (process->window.allocated &&
                process->window.generation == resource->words[1] &&
                (process->window.event_count != 0U ||
                    process->window.overflow_pending)) {
                items[index].ready = items[index].interests &
                    PHIPIA_WAIT_READABLE;
            }
        } else if (type == PHIPIA_HANDLE_AUDIO_OUTPUT) {
            bool writable;
            bool closed;
            const enum audio_native_status audio_status = audio_native_poll(
                process->generation, resource->words[0], &writable, &closed);

            if ((items[index].interests &
                    ~(PHIPIA_WAIT_WRITABLE | PHIPIA_WAIT_CLOSED)) != 0U) {
                return -PHIPIA_EINVAL;
            }
            if (audio_status != AUDIO_NATIVE_OK) {
                return audio_error(audio_status);
            }
            if (writable) {
                items[index].ready |= items[index].interests &
                    PHIPIA_WAIT_WRITABLE;
            }
            if (closed) {
                items[index].ready |= items[index].interests &
                    PHIPIA_WAIT_CLOSED;
            }
        } else if (type == PHIPIA_HANDLE_STREAM ||
            type == PHIPIA_HANDLE_DATAGRAM) {
            network_requests[network_count].handle = resource->words[0];
            network_requests[network_count].interests = 0U;
            if ((items[index].interests & PHIPIA_WAIT_READABLE) != 0U) {
                network_requests[network_count].interests |=
                    NETWORK_READY_READABLE;
            }
            if ((items[index].interests & PHIPIA_WAIT_WRITABLE) != 0U) {
                network_requests[network_count].interests |=
                    NETWORK_READY_WRITABLE;
            }
            network_indices[network_count++] = index;
        }
        if (items[index].ready != 0U) {
            ++ready_count;
        }
    }
    if (network_count != 0U) {
        size_t result_count = 0U;

        cpu_interrupt_enable();
        const enum network_status status = network_poll(process->generation,
            network_requests, network_count, network_results, network_count,
            &result_count, 1U);
        cpu_interrupt_disable();
        if (status != NETWORK_STATUS_OK && status != NETWORK_STATUS_TIMEOUT) {
            return network_error(status);
        }
        for (size_t result = 0U; result < result_count; ++result) {
            const size_t index = network_indices[result];

            if ((network_results[result].ready & NETWORK_READY_READABLE) != 0U) {
                items[index].ready |= PHIPIA_WAIT_READABLE;
            }
            if ((network_results[result].ready & NETWORK_READY_WRITABLE) != 0U) {
                items[index].ready |= PHIPIA_WAIT_WRITABLE;
            }
            if ((network_results[result].ready &
                    (NETWORK_READY_PEER_CLOSED | NETWORK_READY_ERROR |
                        NETWORK_READY_CANCELLED)) != 0U) {
                items[index].ready |= PHIPIA_WAIT_CLOSED;
            }
            if (items[index].ready != 0U) {
                ++ready_count;
            }
        }
    }
    return (int64_t)ready_count;
}

static int64_t syscall_wait(
    struct native_process *process,
    uint64_t request_address
)
{
    struct phipia_wait_request request;
    struct phipia_wait_item items[PHIPIA_WAIT_MAX];
    struct native_thread *thread = running_thread(process);
    int64_t ready;

    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -PHIPIA_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != PHIPIA_ABI_VERSION || request.flags != 0U ||
        request.count == 0U || request.count > PHIPIA_WAIT_MAX) {
        return -PHIPIA_EINVAL;
    }
    if (thread == NULL || !validate_user_range(process, request.items,
            request.count * sizeof(items[0]), true) ||
        !copy_from_user(process, items, request.items,
            request.count * sizeof(items[0]))) {
        return -PHIPIA_EFAULT;
    }
    ready = poll_wait_items(process, items, request.count);
    if (ready < 0) {
        return ready;
    }
    if (ready != 0 || request.deadline_ns <= clock_monotonic_ns()) {
        if (!copy_to_user(process, request.items, items,
                request.count * sizeof(items[0]))) {
            return -PHIPIA_EFAULT;
        }
        return ready == 0 ? -PHIPIA_ETIMEDOUT : ready;
    }
    copy_bytes(thread->wait_items, items,
        request.count * sizeof(items[0]));
    thread->wait_items_address = request.items;
    thread->wait_item_count = request.count;
    thread->deadline_ns = request.deadline_ns;
    thread->state = NATIVE_THREAD_HANDLE_WAIT;
    return 0;
}

static void native_ui_event(
    uint32_t slot,
    const struct ui_native_event *source,
    void *context
)
{
    struct native_process *process = context;
    struct native_window_state *window;
    struct phipia_event event;

    if (process == NULL || source == NULL || !process->active ||
        !process->window.allocated ||
        process->window.ui_slot != slot ||
        !process->window.event_object_open) {
        return;
    }
    if ((source->type == UI_NATIVE_EVENT_KEY ||
            source->type == UI_NATIVE_EVENT_POINTER_MOVE ||
            source->type == UI_NATIVE_EVENT_POINTER_BUTTON) &&
        (process->manifest.capabilities & PHIPIA_CAP_INPUT) == 0U) {
        return;
    }
    window = &process->window;
    zero_bytes(&event, sizeof(event));
    event.size = sizeof(event);
    event.version = PHIPIA_ABI_VERSION;
    event.monotonic_ns = source->monotonic_ns;
    event.x = source->x;
    event.y = source->y;
    event.delta_x = source->delta_x;
    event.delta_y = source->delta_y;
    event.code = source->code;
    event.value = source->value;
    event.modifiers = source->modifiers;
    switch (source->type) {
    case UI_NATIVE_EVENT_KEY:
        event.type = PHIPIA_EVENT_KEY;
        break;
    case UI_NATIVE_EVENT_POINTER_MOVE:
        event.type = PHIPIA_EVENT_POINTER_MOVE;
        break;
    case UI_NATIVE_EVENT_POINTER_BUTTON:
        event.type = PHIPIA_EVENT_POINTER_BUTTON;
        break;
    case UI_NATIVE_EVENT_FOCUS:
        event.type = PHIPIA_EVENT_FOCUS;
        break;
    case UI_NATIVE_EVENT_CLOSE:
        event.type = PHIPIA_EVENT_CLOSE;
        break;
    default:
        return;
    }
    if (event.type == PHIPIA_EVENT_POINTER_MOVE && window->event_count != 0U &&
        window->events[window->event_count - 1U].type ==
            PHIPIA_EVENT_POINTER_MOVE) {
        window->events[window->event_count - 1U] = event;
        return;
    }
    if (window->event_count == NATIVE_EVENT_QUEUE_CAPACITY) {
        size_t remove = 0U;

        for (size_t index = 0U; index < window->event_count; ++index) {
            if (window->events[index].type == PHIPIA_EVENT_POINTER_MOVE) {
                remove = index;
                break;
            }
        }
        for (size_t index = remove + 1U; index < window->event_count;
             ++index) {
            window->events[index - 1U] = window->events[index];
        }
        --window->event_count;
        window->overflow_pending = true;
    }
    window->events[window->event_count++] = event;
}

static int64_t syscall_window_create(
    struct native_process *process,
    uint64_t request_address,
    uint64_t response_address
)
{
    struct phipia_window_create_request request;
    struct phipia_window_create_response response = {
        sizeof(response), PHIPIA_ABI_VERSION, PHIPIA_HANDLE_INVALID,
        PHIPIA_HANDLE_INVALID, 0U, 0U, 0U, 0U, PHIPIA_PIXEL_XRGB8888
    };
    struct native_resource window_resource = {{0U, 0U, 0U, 0U}};
    struct native_resource event_resource = {{0U, 0U, 0U, 0U}};
    struct native_window_state *window = &process->window;
    char title[PHIPIA_WINDOW_TITLE_MAX + 1U];
    uint64_t byte_length;
    size_t page_count;
    phipia_handle_t window_handle;
    phipia_handle_t event_handle;

    if ((process->manifest.capabilities & PHIPIA_CAP_WINDOW) == 0U) {
        return -PHIPIA_EACCES;
    }
    if (!copy_from_user(process, &request, request_address,
            sizeof(request)) ||
        !validate_user_range(process, response_address, sizeof(response),
            true)) {
        return -PHIPIA_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != PHIPIA_ABI_VERSION || request.flags != 0U ||
        request.reserved != 0U || request.title_length == 0U ||
        request.title_length > PHIPIA_WINDOW_TITLE_MAX ||
        request.width < 64U || request.width > NATIVE_SURFACE_MAX_WIDTH ||
        request.height < 64U || request.height > NATIVE_SURFACE_MAX_HEIGHT ||
        request.pixel_format != PHIPIA_PIXEL_XRGB8888 || window->allocated ||
        !copy_from_user(process, title, request.title,
            request.title_length)) {
        return -PHIPIA_EINVAL;
    }
    for (size_t index = 0U; index < request.title_length; ++index) {
        if (title[index] < ' ' || title[index] > '~') {
            return -PHIPIA_EINVAL;
        }
    }
    title[request.title_length] = '\0';
    if (request.width > UINT32_MAX / SURFACE_BYTES_PER_PIXEL) {
        return -PHIPIA_EINVAL;
    }
    window->stride_bytes = request.width * SURFACE_BYTES_PER_PIXEL;
    byte_length = (uint64_t)window->stride_bytes * request.height;
    if (byte_length == 0U || byte_length > SIZE_MAX ||
        byte_length > process->manifest.memory_limit ||
        process->page_count > NATIVE_PROCESS_PAGE_LIMIT -
            (size_t)((byte_length + PAGING_PAGE_SIZE - 1U) /
                PAGING_PAGE_SIZE)) {
        zero_bytes(window, sizeof(*window));
        return -PHIPIA_ENOMEM;
    }
    window->allocated = true;
    window->window_object_open = true;
    window->surface_address = PAGING_NATIVE_SURFACE_BASE;
    window->surface_bytes = (size_t)byte_length;
    window->width = request.width;
    window->height = request.height;
    window->ui_slot = (uint32_t)current_process;
    window->generation = next_window_generation++;
    if (next_window_generation == 0U) {
        next_window_generation = 1U;
    }
    if (heap_allocate(window->surface_bytes,
            (void **)&window->shadow_pixels) != HEAP_STATUS_OK) {
        zero_bytes(window, sizeof(*window));
        return -PHIPIA_ENOMEM;
    }
    zero_bytes(window->shadow_pixels, window->surface_bytes);
    page_count = (window->surface_bytes + PAGING_PAGE_SIZE - 1U) /
        PAGING_PAGE_SIZE;
    for (size_t page = 0U; page < page_count; ++page) {
        uintptr_t physical_address;
        const uint64_t address = window->surface_address +
            page * PAGING_PAGE_SIZE;

        if (!allocate_page(process, address, PAGING_WRITE,
                PAGING_PROCESS_MAPPING_NATIVE_SURFACE, &physical_address) ||
            paging_process_map_user_page(&process->address_space,
                PAGING_PROCESS_MAPPING_NATIVE_SURFACE, address,
                physical_address, PAGING_WRITE) != PAGING_STATUS_OK) {
            (void)window_release_surface(process);
            return -PHIPIA_ENOMEM;
        }
        page_at(process, address)->mapped = true;
    }
    window_resource.words[0] = window->ui_slot;
    window_resource.words[1] = window->generation;
    if (native_handle_install(&process->handles, PHIPIA_HANDLE_WINDOW,
            &window_resource, &window_handle) != NATIVE_HANDLE_OK) {
        (void)window_release_surface(process);
        return -PHIPIA_ENOMEM;
    }
    event_resource.words[0] = window->ui_slot;
    event_resource.words[1] = window->generation;
    if (native_handle_install(&process->handles, PHIPIA_HANDLE_EVENT_QUEUE,
            &event_resource, &event_handle) != NATIVE_HANDLE_OK) {
        (void)native_handle_close(&process->handles, window_handle,
            close_resource, process);
        return -PHIPIA_ENOMEM;
    }
    window->event_object_open = true;
    const enum ui_status ui_status = ui_native_window_open(window->ui_slot,
        title, window->shadow_pixels, window->width, window->height,
        window->stride_bytes, native_ui_event, process);

    if (ui_status != UI_STATUS_OK) {
        console_write("Phipia: native window open failed: ");
        console_write(ui_status_string(ui_status));
        console_write("\n");
        (void)native_handle_close(&process->handles, event_handle,
            close_resource, process);
        (void)native_handle_close(&process->handles, window_handle,
            close_resource, process);
        return -PHIPIA_EIO;
    }
    window->visible = true;
    response.window = window_handle;
    response.events = event_handle;
    response.surface_address = window->surface_address;
    response.width = window->width;
    response.height = window->height;
    response.stride_bytes = window->stride_bytes;
    if (!copy_to_user(process, response_address, &response,
            sizeof(response))) {
        (void)native_handle_close(&process->handles, event_handle,
            close_resource, process);
        (void)native_handle_close(&process->handles, window_handle,
            close_resource, process);
        return -PHIPIA_EFAULT;
    }
    if (process->handles.active_handles > process->peak_handles) {
        process->peak_handles = process->handles.active_handles;
    }
    return 0;
}

static int64_t syscall_surface_present(
    struct native_process *process,
    uint64_t request_address
)
{
    struct phipia_present_request request;
    struct phipia_rect rectangles[PHIPIA_DAMAGE_MAX];
    struct ui_rect damage[PHIPIA_DAMAGE_MAX];
    struct native_resource *resource;
    struct native_window_state *window = &process->window;
    uint64_t pixel_count = 0U;

    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -PHIPIA_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != PHIPIA_ABI_VERSION || request.flags != 0U ||
        request.rectangle_count == 0U ||
        request.rectangle_count > PHIPIA_DAMAGE_MAX ||
        !copy_from_user(process, rectangles, request.rectangles,
            request.rectangle_count * sizeof(rectangles[0]))) {
        return -PHIPIA_EINVAL;
    }
    if (native_handle_resolve(&process->handles, request.window,
            PHIPIA_HANDLE_WINDOW, &resource) != NATIVE_HANDLE_OK ||
        !window->allocated || !window->window_object_open ||
        resource->words[1] != window->generation) {
        return -PHIPIA_EBADF;
    }
    for (size_t index = 0U; index < request.rectangle_count; ++index) {
        const struct phipia_rect rectangle = rectangles[index];

        if (rectangle.width == 0U || rectangle.height == 0U ||
            rectangle.x >= window->width || rectangle.y >= window->height ||
            rectangle.width > window->width - rectangle.x ||
            rectangle.height > window->height - rectangle.y) {
            return -PHIPIA_EINVAL;
        }
        for (uint32_t row = 0U; row < rectangle.height; ++row) {
            const uint64_t address = window->surface_address +
                (uint64_t)(rectangle.y + row) * window->stride_bytes +
                (uint64_t)rectangle.x * SURFACE_BYTES_PER_PIXEL;

            if (!validate_user_range(process, address,
                    (size_t)rectangle.width * SURFACE_BYTES_PER_PIXEL,
                    false)) {
                return -PHIPIA_EFAULT;
            }
        }
        damage[index] = (struct ui_rect){ rectangle.x, rectangle.y,
            rectangle.width, rectangle.height };
        pixel_count += (uint64_t)rectangle.width * rectangle.height;
    }
    for (size_t index = 0U; index < request.rectangle_count; ++index) {
        const struct phipia_rect rectangle = rectangles[index];

        for (uint32_t row = 0U; row < rectangle.height; ++row) {
            uint32_t completed = 0U;

            while (completed < rectangle.width) {
                uint32_t pixels = rectangle.width - completed;
                const uint64_t address = window->surface_address +
                    (uint64_t)(rectangle.y + row) * window->stride_bytes +
                    (uint64_t)(rectangle.x + completed) *
                        SURFACE_BYTES_PER_PIXEL;

                if (pixels > sizeof(process->transfer) /
                        SURFACE_BYTES_PER_PIXEL) {
                    pixels = sizeof(process->transfer) /
                        SURFACE_BYTES_PER_PIXEL;
                }
                if (!copy_from_user(process, process->transfer, address,
                        (size_t)pixels * SURFACE_BYTES_PER_PIXEL)) {
                    return -PHIPIA_EFAULT;
                }
                for (uint32_t pixel = 0U; pixel < pixels; ++pixel) {
                    const size_t offset = (size_t)pixel * 4U;
                    const uint32_t xrgb =
                        (uint32_t)process->transfer[offset] |
                        (uint32_t)process->transfer[offset + 1U] << 8U |
                        (uint32_t)process->transfer[offset + 2U] << 16U |
                        (uint32_t)process->transfer[offset + 3U] << 24U;

                    window->shadow_pixels[
                        (size_t)(rectangle.y + row) * window->width +
                        rectangle.x + completed + pixel] = framebuffer_pack(
                            (uint8_t)(xrgb >> 16U),
                            (uint8_t)(xrgb >> 8U), (uint8_t)xrgb);
                }
                completed += pixels;
            }
        }
    }
    if (ui_native_window_damage(window->ui_slot, damage,
            request.rectangle_count) != UI_STATUS_OK) {
        return -PHIPIA_EIO;
    }
    ++window->present_calls;
    window->presented_pixels += pixel_count;
    return (int64_t)pixel_count;
}

static int64_t syscall_event_read(
    struct native_process *process,
    phipia_handle_t handle,
    uint64_t output_address
)
{
    struct native_resource *resource;
    struct phipia_event event;
    struct native_window_state *window = &process->window;
    enum native_handle_status status;

    if (!validate_user_range(process, output_address, sizeof(event), true)) {
        return -PHIPIA_EFAULT;
    }
    status = native_handle_resolve(&process->handles, handle,
        PHIPIA_HANDLE_EVENT_QUEUE, &resource);
    if (status != NATIVE_HANDLE_OK) {
        return handle_error(status);
    }
    if (!window->allocated || !window->event_object_open ||
        resource->words[1] != window->generation) {
        return -PHIPIA_ESTALE;
    }
    if (window->overflow_pending) {
        zero_bytes(&event, sizeof(event));
        event.size = sizeof(event);
        event.version = PHIPIA_ABI_VERSION;
        event.type = PHIPIA_EVENT_QUEUE_OVERFLOW;
        event.monotonic_ns = clock_monotonic_ns();
        window->overflow_pending = false;
    } else {
        if (window->event_count == 0U) {
            return -PHIPIA_EAGAIN;
        }
        event = window->events[0];
        for (size_t index = 1U; index < window->event_count; ++index) {
            window->events[index - 1U] = window->events[index];
        }
        --window->event_count;
    }
    return copy_to_user(process, output_address, &event, sizeof(event)) ?
        1 : -PHIPIA_EFAULT;
}

static int64_t syscall_pointer_capture(
    struct native_process *process,
    phipia_handle_t handle,
    uint64_t capture
)
{
    struct native_resource *resource;
    enum native_handle_status status;

    if ((process->manifest.capabilities & PHIPIA_CAP_INPUT) == 0U) {
        return -PHIPIA_EACCES;
    }
    if (capture > 1U) {
        return -PHIPIA_EINVAL;
    }
    status = native_handle_resolve(&process->handles, handle,
        PHIPIA_HANDLE_WINDOW, &resource);
    if (status != NATIVE_HANDLE_OK) {
        return handle_error(status);
    }
    return ui_native_pointer_capture((uint32_t)resource->words[0],
        capture != 0U) == UI_STATUS_OK ? 0 : -PHIPIA_EBUSY;
}

static int64_t syscall_dns_resolve(
    struct native_process *process,
    uint64_t hostname_address,
    size_t hostname_length,
    uint64_t deadline
)
{
    char hostname[NETWORK_MAX_HOSTNAME + 1U];
    uint64_t timeout;
    uint32_t address;
    enum network_status status;

    if ((process->manifest.capabilities & PHIPIA_CAP_NETWORK) == 0U) {
        return -PHIPIA_EACCES;
    }
    if (hostname_length == 0U || hostname_length > NETWORK_MAX_HOSTNAME ||
        !copy_from_user(process, hostname, hostname_address,
            hostname_length)) {
        return -PHIPIA_EFAULT;
    }
    for (size_t index = 0U; index < hostname_length; ++index) {
        if (hostname[index] == '\0' ||
            (uint8_t)hostname[index] > UINT8_C(0x7F)) {
            return -PHIPIA_EINVAL;
        }
    }
    hostname[hostname_length] = '\0';
    cpu_interrupt_enable();
    status = prepare_native_network(deadline, &timeout);
    if (status == NETWORK_STATUS_OK) {
        status = network_resolve(hostname, &address, timeout);
    }
    cpu_interrupt_disable();
    return status == NETWORK_STATUS_OK ? (int64_t)address :
        network_error(status);
}

static int64_t syscall_network_open(
    struct native_process *process,
    bool datagram
)
{
    network_handle network = 0U;
    struct native_resource resource = {{0U, 0U, 0U, 0U}};
    phipia_handle_t handle;
    enum network_status status;
    enum native_handle_status handle_status;

    if ((process->manifest.capabilities & PHIPIA_CAP_NETWORK) == 0U) {
        return -PHIPIA_EACCES;
    }
    cpu_interrupt_enable();
    status = datagram ? network_udp_open(process->generation, &network) :
        network_tcp_open(process->generation, &network);
    cpu_interrupt_disable();
    if (status != NETWORK_STATUS_OK) {
        return network_error(status);
    }
    resource.words[0] = network;
    handle_status = native_handle_install(&process->handles,
        datagram ? PHIPIA_HANDLE_DATAGRAM : PHIPIA_HANDLE_STREAM,
        &resource, &handle);
    if (handle_status != NATIVE_HANDLE_OK) {
        cpu_interrupt_enable();
        (void)network_close(process->generation, network);
        cpu_interrupt_disable();
        return handle_error(handle_status);
    }
    if (process->handles.active_handles > process->peak_handles) {
        process->peak_handles = process->handles.active_handles;
    }
    return (int64_t)handle;
}

static int64_t syscall_stream_connect(
    struct native_process *process,
    phipia_handle_t handle,
    uint64_t endpoint_address,
    uint64_t deadline
)
{
    struct phipia_ipv4_endpoint endpoint;
    struct native_resource *resource;
    uint64_t timeout;
    enum native_handle_status handle_status;
    enum network_status status;

    if (!copy_from_user(process, &endpoint, endpoint_address,
            sizeof(endpoint))) {
        return -PHIPIA_EFAULT;
    }
    if (endpoint.reserved != 0U || endpoint.address == 0U ||
        endpoint.port == 0U) {
        return -PHIPIA_EINVAL;
    }
    handle_status = native_handle_resolve(&process->handles, handle,
        PHIPIA_HANDLE_STREAM, &resource);
    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    cpu_interrupt_enable();
    status = prepare_native_network(deadline, &timeout);
    if (status == NETWORK_STATUS_OK) {
        status = network_tcp_connect(process->generation, resource->words[0],
            endpoint.address, endpoint.port, timeout);
    }
    cpu_interrupt_disable();
    return network_error(status);
}

static int64_t syscall_network_io(
    struct native_process *process,
    uint64_t request_address,
    bool datagram,
    bool write
)
{
    struct phipia_network_io request;
    struct native_resource *resource;
    uint64_t timeout;
    size_t transferred = 0U;
    uint32_t source = 0U;
    uint16_t port = 0U;
    enum native_handle_status handle_status;
    enum network_status status;

    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -PHIPIA_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != PHIPIA_ABI_VERSION || request.flags != 0U ||
        request.endpoint.reserved != 0U || request.length == 0U ||
        request.length > (datagram ? NETWORK_MAX_UDP_DATAGRAM :
            PHIPIA_NETWORK_IO_MAX_BYTES) ||
        !validate_user_range(process, request.buffer, request.length, !write) ||
        (!write && datagram && !validate_user_range(process, request_address,
            sizeof(request), true))) {
        return -PHIPIA_EINVAL;
    }
    handle_status = native_handle_resolve(&process->handles, request.handle,
        datagram ? PHIPIA_HANDLE_DATAGRAM : PHIPIA_HANDLE_STREAM, &resource);
    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    if (write && !copy_from_user(process, process->transfer, request.buffer,
            request.length)) {
        return -PHIPIA_EFAULT;
    }
    cpu_interrupt_enable();
    status = prepare_native_network(request.deadline_ns, &timeout);
    if (status == NETWORK_STATUS_OK && datagram && write) {
        status = network_udp_send(process->generation, resource->words[0],
            request.endpoint.address, request.endpoint.port, process->transfer,
            request.length, timeout);
        transferred = status == NETWORK_STATUS_OK ? request.length : 0U;
    } else if (status == NETWORK_STATUS_OK && datagram) {
        status = network_udp_receive(process->generation, resource->words[0],
            &source, &port, process->transfer, request.length, &transferred,
            timeout);
    } else if (status == NETWORK_STATUS_OK && write) {
        status = network_tcp_write(process->generation, resource->words[0],
            process->transfer, request.length, &transferred, timeout);
    } else if (status == NETWORK_STATUS_OK) {
        status = network_tcp_read(process->generation, resource->words[0],
            process->transfer, request.length, &transferred, timeout);
    }
    cpu_interrupt_disable();
    if (status != NETWORK_STATUS_OK) {
        return network_error(status);
    }
    if (!write && transferred != 0U &&
        !copy_to_user(process, request.buffer, process->transfer,
            transferred)) {
        return -PHIPIA_EFAULT;
    }
    if (!write && datagram) {
        request.endpoint.address = source;
        request.endpoint.port = port;
        request.length = (uint32_t)transferred;
        if (!copy_to_user(process, request_address, &request,
                sizeof(request))) {
            return -PHIPIA_EFAULT;
        }
    }
    return (int64_t)transferred;
}

static int64_t syscall_datagram_bind(
    struct native_process *process,
    phipia_handle_t handle,
    uint16_t port
)
{
    struct native_resource *resource;
    enum native_handle_status handle_status = native_handle_resolve(
        &process->handles, handle, PHIPIA_HANDLE_DATAGRAM, &resource);

    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    cpu_interrupt_enable();
    const enum network_status status = network_udp_bind(process->generation,
        resource->words[0], port);
    cpu_interrupt_disable();
    return network_error(status);
}

static int64_t syscall_stream_shutdown(
    struct native_process *process,
    phipia_handle_t handle,
    uint32_t flags,
    uint64_t deadline
)
{
    struct native_resource *resource;
    uint64_t timeout;
    enum native_handle_status handle_status;

    if ((flags & ~(PHIPIA_SHUTDOWN_READ | PHIPIA_SHUTDOWN_WRITE)) != 0U ||
        (flags & PHIPIA_SHUTDOWN_WRITE) == 0U ||
        !deadline_timeout(deadline, &timeout)) {
        return -PHIPIA_EINVAL;
    }
    handle_status = native_handle_resolve(&process->handles, handle,
        PHIPIA_HANDLE_STREAM, &resource);
    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    cpu_interrupt_enable();
    const enum network_status status = network_tcp_shutdown(
        process->generation, resource->words[0], timeout);
    cpu_interrupt_disable();
    return network_error(status);
}

static int64_t syscall_network_address(
    struct native_process *process,
    phipia_handle_t handle,
    bool peer,
    uint64_t output_address
)
{
    struct native_resource *resource;
    struct phipia_ipv4_endpoint endpoint = {0U, 0U, 0U};
    uint32_t address = 0U;
    uint16_t port = 0U;
    enum native_handle_status handle_status;
    enum network_status status;

    if (!validate_user_range(process, output_address, sizeof(endpoint), true)) {
        return -PHIPIA_EFAULT;
    }
    handle_status = native_handle_resolve(&process->handles, handle, 0U,
        &resource);
    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    const uint8_t type = (uint8_t)((handle >> 16U) & UINT64_C(0xFF));

    if (type != PHIPIA_HANDLE_STREAM && type != PHIPIA_HANDLE_DATAGRAM) {
        return -PHIPIA_EBADF;
    }
    cpu_interrupt_enable();
    status = network_address(process->generation, resource->words[0], peer,
        &address, &port);
    cpu_interrupt_disable();
    if (status != NETWORK_STATUS_OK) {
        return network_error(status);
    }
    endpoint.address = address;
    endpoint.port = port;
    return copy_to_user(process, output_address, &endpoint, sizeof(endpoint)) ?
        0 : -PHIPIA_EFAULT;
}

static int64_t syscall_audio_open(struct native_process *process)
{
    struct native_resource resource = {{0U, 0U, 0U, 0U}};
    phipia_handle_t handle;
    enum audio_native_status audio_status;
    enum native_handle_status handle_status;

    if ((process->manifest.capabilities & PHIPIA_CAP_AUDIO) == 0U) {
        return -PHIPIA_EACCES;
    }
    audio_status = audio_native_open(process->generation,
        &resource.words[0]);
    if (audio_status != AUDIO_NATIVE_OK) {
        return audio_error(audio_status);
    }
    handle_status = native_handle_install(&process->handles,
        PHIPIA_HANDLE_AUDIO_OUTPUT, &resource, &handle);
    if (handle_status != NATIVE_HANDLE_OK) {
        (void)audio_native_close(process->generation, resource.words[0]);
        return handle_error(handle_status);
    }
    if (process->handles.active_handles > process->peak_handles) {
        process->peak_handles = process->handles.active_handles;
    }
    return (int64_t)handle;
}

static int64_t syscall_audio_submit(
    struct native_process *process,
    uint64_t request_address
)
{
    struct phipia_audio_submit_request request;
    struct native_resource *resource;
    enum native_handle_status handle_status;
    enum audio_native_status audio_status;

    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -PHIPIA_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != PHIPIA_ABI_VERSION || request.flags != 0U ||
        request.length != PHIPIA_AUDIO_CHUNK_BYTES) {
        return -PHIPIA_EINVAL;
    }
    handle_status = native_handle_resolve(&process->handles, request.handle,
        PHIPIA_HANDLE_AUDIO_OUTPUT, &resource);
    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    if (!copy_from_user(process, process->transfer, request.buffer,
            request.length)) {
        return -PHIPIA_EFAULT;
    }
    audio_status = audio_native_submit(process->generation,
        resource->words[0], (const int16_t *)(const void *)process->transfer,
        request.length);
    return audio_status == AUDIO_NATIVE_OK ? (int64_t)request.length :
        audio_error(audio_status);
}

static int64_t syscall_audio_volume(
    struct native_process *process,
    uint64_t request_address
)
{
    struct phipia_audio_volume_request request;
    struct native_resource *resource;
    enum native_handle_status handle_status;

    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -PHIPIA_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != PHIPIA_ABI_VERSION || request.flags != 0U ||
        request.reserved != 0U ||
        request.left_q15 > PHIPIA_AUDIO_VOLUME_MAX ||
        request.right_q15 > PHIPIA_AUDIO_VOLUME_MAX) {
        return -PHIPIA_EINVAL;
    }
    handle_status = native_handle_resolve(&process->handles, request.handle,
        PHIPIA_HANDLE_AUDIO_OUTPUT, &resource);
    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    return audio_error(audio_native_set_volume(process->generation,
        resource->words[0], request.left_q15, request.right_q15));
}

static int64_t syscall_audio_drain(
    struct native_process *process,
    phipia_handle_t handle,
    uint64_t deadline_ns
)
{
    struct native_resource *resource;
    struct native_thread *thread = running_thread(process);
    const enum native_handle_status handle_status = native_handle_resolve(
        &process->handles, handle, PHIPIA_HANDLE_AUDIO_OUTPUT, &resource);
    enum audio_native_drain_state state;

    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    if (thread == NULL) {
        return -PHIPIA_EIO;
    }
    state = audio_native_drain(process->generation, resource->words[0]);
    if (state == AUDIO_NATIVE_DRAIN_COMPLETE) {
        return 0;
    }
    if (state == AUDIO_NATIVE_DRAIN_CANCELED) {
        return -PHIPIA_ECANCELED;
    }
    if (state == AUDIO_NATIVE_DRAIN_ERROR) {
        return -PHIPIA_EIO;
    }
    if (state == AUDIO_NATIVE_DRAIN_STALE) {
        return -PHIPIA_ESTALE;
    }
    if (deadline_ns != 0U && deadline_ns <= clock_monotonic_ns()) {
        return -PHIPIA_ETIMEDOUT;
    }
    thread->audio_token = resource->words[0];
    thread->deadline_ns = deadline_ns;
    thread->state = NATIVE_THREAD_AUDIO_DRAIN_WAIT;
    return 0;
}

static int64_t syscall_package_upload_open(struct native_process *process)
{
    struct package_upload_report report;
    struct native_resource resource = {{0U, 0U, 0U, 0U}};
    phipia_handle_t handle;

    if ((process->manifest.capabilities & PHIPIA_CAP_PACKAGES) == 0U) {
        return -PHIPIA_EACCES;
    }
    if (process->handles.active_handles >= process->handles.limit ||
        process->handles.active_objects >= process->handles.limit) {
        return -PHIPIA_ENOMEM;
    }
    cpu_interrupt_enable();
    enum package_upload_status upload_status = package_upload_open(
        process->generation, &report);
    if (upload_status == PACKAGE_UPLOAD_STATUS_NOT_INITIALIZED &&
        package_upload_initialize(&report) == PACKAGE_UPLOAD_STATUS_OK) {
        upload_status = package_upload_open(process->generation, &report);
    }
    cpu_interrupt_disable();
    if (upload_status != PACKAGE_UPLOAD_STATUS_OK) {
        return package_upload_error(upload_status, report.filesystem_status);
    }
    resource.words[0] = report.token;
    enum native_handle_status handle_status = native_handle_install(
        &process->handles, PHIPIA_HANDLE_PACKAGE_UPLOAD, &resource, &handle);

    if (handle_status != NATIVE_HANDLE_OK) {
        cpu_interrupt_enable();
        (void)package_upload_close(process->generation, report.token, &report);
        cpu_interrupt_disable();
        return handle_error(handle_status);
    }
    if (process->handles.active_handles > process->peak_handles) {
        process->peak_handles = process->handles.active_handles;
    }
    return (int64_t)handle;
}

static int64_t syscall_package_upload_write(
    struct native_process *process,
    uint64_t request_address
)
{
    struct phipia_package_upload_write_request request;
    struct package_upload_report report;
    struct native_resource *resource;
    size_t written = 0U;

    if ((process->manifest.capabilities & PHIPIA_CAP_PACKAGES) == 0U) {
        return -PHIPIA_EACCES;
    }
    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -PHIPIA_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != PHIPIA_ABI_VERSION || request.flags != 0U ||
        request.length > PHIPIA_PACKAGE_UPLOAD_WRITE_MAX) {
        return -PHIPIA_EINVAL;
    }
    if (request.length != 0U && !copy_from_user(process, process->transfer,
            request.buffer, request.length)) {
        return -PHIPIA_EFAULT;
    }
    enum native_handle_status handle_status = native_handle_resolve(
        &process->handles, request.handle, PHIPIA_HANDLE_PACKAGE_UPLOAD,
        &resource);

    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    cpu_interrupt_enable();
    enum package_upload_status upload_status = package_upload_write(
        process->generation, resource->words[0], process->transfer,
        request.length, &written, &report);
    cpu_interrupt_disable();
    return upload_status == PACKAGE_UPLOAD_STATUS_OK ? (int64_t)written :
        package_upload_error(upload_status, report.filesystem_status);
}

static int64_t syscall_package_upload_seal(
    struct native_process *process,
    uint64_t request_address
)
{
    struct phipia_package_upload_seal_request request;
    struct package_upload_report report;
    struct native_resource *resource;

    if ((process->manifest.capabilities & PHIPIA_CAP_PACKAGES) == 0U) {
        return -PHIPIA_EACCES;
    }
    if (!validate_user_range(process, request_address, sizeof(request), true) ||
        !copy_from_user(process, &request, request_address, sizeof(request))) {
        return -PHIPIA_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != PHIPIA_ABI_VERSION || request.expected_bytes == 0U ||
        request.expected_bytes > PHIPIA_PACKAGE_UPLOAD_MAX_BYTES ||
        request.actual_bytes != 0U || request.result_flags != 0U ||
        request.reserved != 0U) {
        return -PHIPIA_EINVAL;
    }
    for (size_t index = 0U; index < sizeof(request.actual_sha256); ++index) {
        if (request.actual_sha256[index] != 0U) {
            return -PHIPIA_EINVAL;
        }
    }
    enum native_handle_status handle_status = native_handle_resolve(
        &process->handles, request.handle, PHIPIA_HANDLE_PACKAGE_UPLOAD,
        &resource);

    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    cpu_interrupt_enable();
    enum package_upload_status upload_status = package_upload_seal(
        process->generation, resource->words[0], request.expected_bytes,
        request.expected_sha256, &report);
    cpu_interrupt_disable();
    request.actual_bytes = report.byte_count;
    for (size_t index = 0U; index < sizeof(request.actual_sha256); ++index) {
        request.actual_sha256[index] = report.sha256[index];
    }
    if (report.sealed) {
        request.result_flags |= PHIPIA_PACKAGE_UPLOAD_SEALED;
    }
    if (report.durable) {
        request.result_flags |= PHIPIA_PACKAGE_UPLOAD_DURABLE;
    }
    if (!copy_to_user(process, request_address, &request, sizeof(request))) {
        return -PHIPIA_EFAULT;
    }
    return package_upload_error(upload_status, report.filesystem_status);
}

static int64_t syscall_package_control_open_install(
    struct native_process *process,
    uint64_t request_address
)
{
    struct phipia_package_control_open_request request;
    struct package_control_report report;
    struct native_resource *upload = NULL;
    struct native_resource resource = {{0U, 0U, 0U, 0U}};
    phipia_handle_t handle;

    if ((process->manifest.capabilities & PHIPIA_CAP_PACKAGES) == 0U) {
        return -PHIPIA_EACCES;
    }
    if (!validate_user_range(process, request_address, sizeof(request), true) ||
        !copy_from_user(process, &request, request_address, sizeof(request))) {
        return -PHIPIA_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != PHIPIA_ABI_VERSION ||
        request.flags > PHIPIA_PACKAGE_CONTROL_OPEN_REPAIR ||
        (request.flags == PHIPIA_PACKAGE_CONTROL_OPEN_REPAIR ?
            (request.identifier != 0U || request.identifier_bytes != 0U) :
            request.identifier_bytes == 0U) ||
        request.identifier_bytes >= PHIPIA_PACKAGE_CONTROL_TEXT_BYTES ||
        request.repository_version != 0U || request.generation != 0U ||
        request.plan_count != 0U || request.result_flags != 0U) {
        return -PHIPIA_EINVAL;
    }
    if (request.identifier_bytes != 0U &&
        !copy_from_user(process, process->transfer, request.identifier,
            request.identifier_bytes)) {
        return -PHIPIA_EFAULT;
    }
    enum native_handle_status handle_status = NATIVE_HANDLE_OK;

    if (request.flags == PHIPIA_PACKAGE_CONTROL_OPEN_INSTALL ||
            request.flags == PHIPIA_PACKAGE_CONTROL_OPEN_REPAIR) {
        handle_status = native_handle_resolve(&process->handles,
            request.repository_upload, PHIPIA_HANDLE_PACKAGE_UPLOAD,
            &upload);
    } else if (request.repository_upload != PHIPIA_HANDLE_INVALID) {
        return -PHIPIA_EINVAL;
    }

    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    if (process->handles.active_handles >= process->handles.limit ||
        process->handles.active_objects >= process->handles.limit) {
        return -PHIPIA_ENOMEM;
    }
    cpu_interrupt_enable();
    enum package_control_status control_status;
    if (request.flags == PHIPIA_PACKAGE_CONTROL_OPEN_INSTALL) {
        control_status = package_control_open_install(process->generation,
            upload->words[0], process->transfer, request.identifier_bytes,
            &report);
    } else if (request.flags == PHIPIA_PACKAGE_CONTROL_OPEN_REMOVE) {
        control_status = package_control_open_remove(process->generation,
            process->transfer, request.identifier_bytes, &report);
    } else {
        control_status = package_control_open_repair(process->generation,
            upload->words[0], &report);
    }
    cpu_interrupt_disable();
    if (control_status != PACKAGE_CONTROL_STATUS_OK) {
        return package_control_error(control_status, &report);
    }
    resource.words[0] = report.token;
    handle_status = native_handle_install(&process->handles,
        PHIPIA_HANDLE_PACKAGE_CONTROL, &resource, &handle);
    if (handle_status != NATIVE_HANDLE_OK) {
        cpu_interrupt_enable();
        (void)package_control_close(process->generation, report.token, &report);
        cpu_interrupt_disable();
        return handle_error(handle_status);
    }
    request.repository_version = report.repository_version;
    request.generation = report.generation;
    request.plan_count = report.plan_count;
    request.result_flags = package_control_result_flags(&report);
    if (!copy_to_user(process, request_address, &request, sizeof(request))) {
        cpu_interrupt_enable();
        (void)native_handle_close(&process->handles, handle, close_resource,
            process);
        cpu_interrupt_disable();
        return -PHIPIA_EFAULT;
    }
    if (process->handles.active_handles > process->peak_handles) {
        process->peak_handles = process->handles.active_handles;
    }
    return (int64_t)handle;
}

static int64_t syscall_package_control_item(
    struct native_process *process,
    uint64_t request_address
)
{
    struct phipia_package_control_item_request request;
    struct package_control_item item;
    struct package_control_report report;
    struct native_resource *control;

    if ((process->manifest.capabilities & PHIPIA_CAP_PACKAGES) == 0U) {
        return -PHIPIA_EACCES;
    }
    if (!validate_user_range(process, request_address, sizeof(request), true) ||
        !copy_from_user(process, &request, request_address, sizeof(request))) {
        return -PHIPIA_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != PHIPIA_ABI_VERSION || request.flags != 0U ||
        request.index >= PHIPIA_PACKAGE_CONTROL_PLAN_MAX ||
        request.package_bytes != 0U || request.identifier_bytes != 0U ||
        request.version_bytes != 0U || request.path_bytes != 0U ||
        request.reserved != 0U ||
        !bytes_are_zero(request.package_sha256,
            sizeof(request.package_sha256)) ||
        !bytes_are_zero(request.identifier, sizeof(request.identifier)) ||
        !bytes_are_zero(request.package_version,
            sizeof(request.package_version)) ||
        !bytes_are_zero(request.download_path,
            sizeof(request.download_path))) {
        return -PHIPIA_EINVAL;
    }
    enum native_handle_status handle_status = native_handle_resolve(
        &process->handles, request.control, PHIPIA_HANDLE_PACKAGE_CONTROL,
        &control);

    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    cpu_interrupt_enable();
    enum package_control_status control_status = package_control_item(
        process->generation, control->words[0], request.index, &item, &report);
    cpu_interrupt_disable();
    if (control_status != PACKAGE_CONTROL_STATUS_OK) {
        return package_control_error(control_status, &report);
    }
    request.package_bytes = item.package_bytes;
    request.identifier_bytes = item.identifier_bytes;
    request.version_bytes = item.version_bytes;
    request.path_bytes = item.path_bytes;
    copy_bytes(request.package_sha256, item.package_sha256,
        sizeof(request.package_sha256));
    copy_bytes(request.identifier, item.identifier, sizeof(request.identifier));
    copy_bytes(request.package_version, item.version,
        sizeof(request.package_version));
    copy_bytes(request.download_path, item.download_path,
        sizeof(request.download_path));
    return copy_to_user(process, request_address, &request, sizeof(request)) ?
        0 : -PHIPIA_EFAULT;
}

static int64_t syscall_package_control_attach(
    struct native_process *process,
    uint64_t request_address
)
{
    struct phipia_package_control_attach_request request;
    struct package_control_report report;
    struct native_resource *control;
    struct native_resource *upload;

    if ((process->manifest.capabilities & PHIPIA_CAP_PACKAGES) == 0U) {
        return -PHIPIA_EACCES;
    }
    if (!validate_user_range(process, request_address, sizeof(request), true) ||
        !copy_from_user(process, &request, request_address, sizeof(request))) {
        return -PHIPIA_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != PHIPIA_ABI_VERSION || request.flags != 0U ||
        request.index >= PHIPIA_PACKAGE_CONTROL_PLAN_MAX ||
        request.attached_count != 0U || request.result_flags != 0U) {
        return -PHIPIA_EINVAL;
    }
    enum native_handle_status handle_status = native_handle_resolve(
        &process->handles, request.control, PHIPIA_HANDLE_PACKAGE_CONTROL,
        &control);

    if (handle_status == NATIVE_HANDLE_OK) {
        handle_status = native_handle_resolve(&process->handles,
            request.package_upload, PHIPIA_HANDLE_PACKAGE_UPLOAD, &upload);
    }
    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    cpu_interrupt_enable();
    enum package_control_status control_status = package_control_attach(
        process->generation, control->words[0], request.index,
        upload->words[0], &report);
    cpu_interrupt_disable();
    request.attached_count = report.attached_count;
    request.result_flags = package_control_result_flags(&report);
    if (!copy_to_user(process, request_address, &request, sizeof(request))) {
        return -PHIPIA_EFAULT;
    }
    return package_control_error(control_status, &report);
}

static int64_t syscall_package_control_commit(
    struct native_process *process,
    uint64_t request_address
)
{
    struct phipia_package_control_commit_request request;
    struct package_control_report report;
    struct native_resource *control;

    if ((process->manifest.capabilities & PHIPIA_CAP_PACKAGES) == 0U) {
        return -PHIPIA_EACCES;
    }
    if (!validate_user_range(process, request_address, sizeof(request), true) ||
        !copy_from_user(process, &request, request_address, sizeof(request))) {
        return -PHIPIA_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != PHIPIA_ABI_VERSION || request.flags != 0U ||
        request.reserved != 0U || request.generation != 0U ||
        request.plan_count != 0U || request.attached_count != 0U ||
        request.result_flags != 0U || request.result_reserved != 0U) {
        return -PHIPIA_EINVAL;
    }
    enum native_handle_status handle_status = native_handle_resolve(
        &process->handles, request.control, PHIPIA_HANDLE_PACKAGE_CONTROL,
        &control);

    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    cpu_interrupt_enable();
    enum package_control_status control_status = package_control_commit(
        process->generation, control->words[0], &report);
    cpu_interrupt_disable();
    request.generation = report.generation;
    request.plan_count = report.plan_count;
    request.attached_count = report.attached_count;
    request.result_flags = package_control_result_flags(&report);
    if (!copy_to_user(process, request_address, &request, sizeof(request))) {
        return -PHIPIA_EFAULT;
    }
    return package_control_error(control_status, &report);
}

static int64_t syscall_cancel(
    struct native_process *process,
    phipia_handle_t handle
)
{
    struct native_resource *resource;
    enum native_handle_status handle_status = native_handle_resolve(
        &process->handles, handle, 0U, &resource);
    const uint8_t type = (uint8_t)((handle >> 16U) & UINT64_C(0xFF));

    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    if (type == PHIPIA_HANDLE_TIMER) {
        resource->words[0] = 0U;
        return 0;
    }
    if (type == PHIPIA_HANDLE_AUDIO_OUTPUT) {
        return audio_error(audio_native_cancel(process->generation,
            resource->words[0]));
    }
    if (type != PHIPIA_HANDLE_STREAM && type != PHIPIA_HANDLE_DATAGRAM) {
        return -PHIPIA_ENOTSUP;
    }
    cpu_interrupt_enable();
    const enum network_status status = network_cancel(process->generation,
        resource->words[0]);
    cpu_interrupt_disable();
    return network_error(status);
}

static bool release_runtime_pages(
    struct native_process *process,
    uint64_t base,
    size_t page_count,
    enum paging_process_mapping_kind kind
)
{
    bool success = true;

    for (size_t page = 0U; page < page_count; ++page) {
        struct native_page removed;
        const uint64_t address = base + page * PAGING_PAGE_SIZE;
        struct native_page *record = page_at(process, address);

        if (record == NULL || record->kind != kind) {
            success = false;
            continue;
        }
        if (record->mapped &&
            paging_process_unmap_user_page(&process->address_space, kind,
                address) != PAGING_STATUS_OK) {
            success = false;
            continue;
        }
        if (!remove_page_record(process, address, &removed) ||
            !release_page_frame(&removed)) {
            success = false;
        }
    }
    return success;
}

static int64_t syscall_thread_create(
    struct native_process *process,
    uint64_t request_address
)
{
    struct phipia_thread_create_request request;
    struct native_thread *thread;
    struct native_resource resource = {{0U, 0U, 0U, 0U}};
    phipia_handle_t handle;
    size_t index;
    size_t stack_pages;
    uint64_t guard;
    uint64_t stack_base;

    if ((process->manifest.capabilities & PHIPIA_CAP_THREADS) == 0U) {
        return -PHIPIA_EACCES;
    }
    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -PHIPIA_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != PHIPIA_ABI_VERSION || request.flags != 0U ||
        request.entry < process->image.mapping_start ||
        request.entry >= process->image.mapping_end ||
        page_at(process, request.entry) == NULL ||
        page_at(process, request.entry)->permissions != PAGING_EXECUTE ||
        request.stack_bytes < 4U * PAGING_PAGE_SIZE ||
        request.stack_bytes > NATIVE_STACK_PAGES * PAGING_PAGE_SIZE ||
        (request.tls_base != 0U &&
            !validate_user_range(process, request.tls_base, 1U, false))) {
        return -PHIPIA_EINVAL;
    }
    if (process->thread_count >= process->manifest.max_threads ||
        process->thread_count >= NATIVE_THREAD_LIMIT) {
        return -PHIPIA_ENOMEM;
    }
    index = process->thread_count;
    stack_pages = (request.stack_bytes + PAGING_PAGE_SIZE - 1U) /
        PAGING_PAGE_SIZE;
    guard = PAGING_NATIVE_STACK_BASE +
        index * (NATIVE_STACK_PAGES + 1U) * PAGING_PAGE_SIZE;
    stack_base = guard + PAGING_PAGE_SIZE;
    if (stack_base + stack_pages * PAGING_PAGE_SIZE > PAGING_NATIVE_STACK_END) {
        return -PHIPIA_ENOMEM;
    }
    for (size_t page = 0U; page < stack_pages; ++page) {
        uintptr_t physical_address;
        const uint64_t address = stack_base + page * PAGING_PAGE_SIZE;

        if (!allocate_page(process, address, PAGING_WRITE,
                PAGING_PROCESS_MAPPING_NATIVE_STACK, &physical_address) ||
            paging_process_map_user_page(&process->address_space,
                PAGING_PROCESS_MAPPING_NATIVE_STACK, address,
                physical_address, PAGING_WRITE) != PAGING_STATUS_OK) {
            for (size_t rollback = 0U; rollback <= page; ++rollback) {
                struct native_page removed;
                const uint64_t rollback_address = stack_base +
                    rollback * PAGING_PAGE_SIZE;
                struct native_page *record = page_at(process,
                    rollback_address);

                if (record == NULL) {
                    continue;
                }
                if (record->mapped) {
                    (void)paging_process_unmap_user_page(
                        &process->address_space,
                        PAGING_PROCESS_MAPPING_NATIVE_STACK,
                        rollback_address);
                }
                if (remove_page_record(process, rollback_address, &removed)) {
                    (void)release_page_frame(&removed);
                }
            }
            return -PHIPIA_ENOMEM;
        }
        page_at(process, address)->mapped = true;
    }
    thread = &process->threads[index];
    zero_bytes(thread, sizeof(*thread));
    if (!native_fpu_state_initialize(&thread->fpu)) {
        (void)release_runtime_pages(process, stack_base, stack_pages,
            PAGING_PROCESS_MAPPING_NATIVE_STACK);
        return -PHIPIA_EIO;
    }
    thread->generation = next_thread_generation++;
    if (next_thread_generation == 0U) {
        next_thread_generation = 1U;
    }
    thread->fs_base = request.tls_base;
    thread->stack_base = stack_base;
    thread->stack_end = stack_base + stack_pages * PAGING_PAGE_SIZE;
    thread->state = NATIVE_THREAD_RUNNABLE;
    thread->context.rip = request.entry;
    /* A raw C entry observes the SysV post-CALL stack alignment. */
    thread->context.rsp = thread->stack_end - sizeof(uint64_t);
    thread->context.rflags = NATIVE_RFLAGS;
    thread->context.rdi = request.argument;
    resource.words[0] = index;
    resource.words[1] = thread->generation;
    {
        const enum native_handle_status status = native_handle_install(
            &process->handles, PHIPIA_HANDLE_THREAD, &resource, &handle);

        if (status != NATIVE_HANDLE_OK) {
            const int64_t error = handle_error(status);

            zero_bytes(thread, sizeof(*thread));
            if (!release_runtime_pages(process, stack_base, stack_pages,
                    PAGING_PROCESS_MAPPING_NATIVE_STACK)) {
                process->faulted = true;
                process->exit_status = -PHIPIA_EIO;
                process->exiting = true;
                return -PHIPIA_EIO;
            }
            return error;
        }
    }
    ++process->thread_count;
    if (process->handles.active_handles > process->peak_handles) {
        process->peak_handles = process->handles.active_handles;
    }
    return (int64_t)handle;
}

static int64_t syscall_thread_join(
    struct native_process *process,
    phipia_handle_t handle
)
{
    struct native_resource *resource;
    struct native_thread *current = running_thread(process);
    struct native_thread *target;
    enum native_handle_status status = native_handle_resolve(
        &process->handles, handle, PHIPIA_HANDLE_THREAD, &resource);

    if (status != NATIVE_HANDLE_OK) {
        return handle_error(status);
    }
    if (resource->words[0] >= process->thread_count || current == NULL) {
        return -PHIPIA_EBADF;
    }
    target = &process->threads[resource->words[0]];
    if (target == current || target->generation != resource->words[1]) {
        return -PHIPIA_EINVAL;
    }
    if (target->state == NATIVE_THREAD_EXITED ||
        target->state == NATIVE_THREAD_FAULTED) {
        return target->exit_status;
    }
    current->wait_generation = target->generation;
    current->state = NATIVE_THREAD_JOIN_WAIT;
    return 0;
}

static int64_t syscall_tls_set(
    struct native_process *process,
    uint64_t tls_base
)
{
    struct native_thread *thread = running_thread(process);

    if (thread == NULL || (tls_base != 0U &&
            !validate_user_range(process, tls_base, 1U, false))) {
        return -PHIPIA_EFAULT;
    }
    thread->fs_base = tls_base;
    return 0;
}

static int64_t syscall_futex_wait(
    struct native_process *process,
    uint64_t request_address
)
{
    struct phipia_futex_request request;
    struct native_thread *thread = running_thread(process);
    uint32_t observed;

    if (thread == NULL || !copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -PHIPIA_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != PHIPIA_ABI_VERSION || request.count != 0U ||
        !validate_futex_word(process, request.address) ||
        !copy_from_user(process, &observed, request.address,
            sizeof(observed))) {
        return -PHIPIA_EINVAL;
    }
    if (observed != request.expected) {
        return -PHIPIA_EAGAIN;
    }
    thread->futex_address = request.address;
    thread->deadline_ns = request.deadline_ns;
    thread->state = NATIVE_THREAD_FUTEX_WAIT;
    return 0;
}

static int64_t syscall_futex_wake(
    struct native_process *process,
    uint64_t request_address
)
{
    struct phipia_futex_request request;
    size_t woken = 0U;

    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -PHIPIA_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != PHIPIA_ABI_VERSION || request.expected != 0U ||
        request.deadline_ns != 0U || request.count == 0U ||
        !validate_futex_word(process, request.address)) {
        return -PHIPIA_EINVAL;
    }
    for (size_t index = 0U; index < process->thread_count &&
         woken < request.count; ++index) {
        struct native_thread *thread = &process->threads[index];

        if (thread->state == NATIVE_THREAD_FUTEX_WAIT &&
            thread->futex_address == request.address) {
            thread->state = NATIVE_THREAD_RUNNABLE;
            thread->futex_address = 0U;
            thread->deadline_ns = 0U;
            thread->context.rax = 0U;
            ++woken;
        }
    }
    return (int64_t)woken;
}

static uint64_t safe_user_rflags(uint64_t value)
{
    const uint64_t forbidden = (UINT64_C(3) << 12U) |
        (UINT64_C(1) << 17U);

    return (value | UINT64_C(2) | UINT64_C(1) << 9U) & ~forbidden;
}

static void save_syscall_context(
    struct native_thread *thread,
    const struct native_syscall_frame *frame
)
{
    thread->context.rax = frame->rax;
    thread->context.rbx = frame->rbx;
    thread->context.rcx = frame->rip;
    thread->context.rdx = frame->rdx;
    thread->context.rsi = frame->rsi;
    thread->context.rdi = frame->rdi;
    thread->context.rbp = frame->rbp;
    thread->context.r8 = frame->r8;
    thread->context.r9 = frame->r9;
    thread->context.r10 = frame->r10;
    thread->context.r11 = frame->rflags;
    thread->context.r12 = frame->r12;
    thread->context.r13 = frame->r13;
    thread->context.r14 = frame->r14;
    thread->context.r15 = frame->r15;
    thread->context.rip = frame->rip;
    thread->context.rsp = frame->rsp;
    thread->context.rflags = safe_user_rflags(frame->rflags);
}

static void save_interrupt_context(
    struct native_thread *thread,
    const struct interrupt_frame *frame
)
{
    thread->context.rax = frame->rax;
    thread->context.rbx = frame->rbx;
    thread->context.rcx = frame->rcx;
    thread->context.rdx = frame->rdx;
    thread->context.rsi = frame->rsi;
    thread->context.rdi = frame->rdi;
    thread->context.rbp = frame->rbp;
    thread->context.r8 = frame->r8;
    thread->context.r9 = frame->r9;
    thread->context.r10 = frame->r10;
    thread->context.r11 = frame->r11;
    thread->context.r12 = frame->r12;
    thread->context.r13 = frame->r13;
    thread->context.r14 = frame->r14;
    thread->context.r15 = frame->r15;
    thread->context.rip = frame->rip;
    thread->context.rsp = interrupt_frame_stack_pointer(frame);
    thread->context.rflags = safe_user_rflags(frame->rflags);
}

static void terminate_process(struct native_process *process, int32_t status)
{
    process->exit_status = status;
    process->exiting = true;
    for (size_t index = 0U; index < process->thread_count; ++index) {
        if (process->threads[index].state != NATIVE_THREAD_FAULTED) {
            process->threads[index].state = NATIVE_THREAD_EXITED;
        }
        process->threads[index].exit_status = status;
    }
}

static bool begin_dynamic_finalizers(
    struct native_process *process,
    struct native_thread *thread,
    int32_t status
)
{
    if (process->dynamic_fini_entry == 0U ||
        process->dynamic_fini_started) {
        return false;
    }
    process->dynamic_fini_started = true;
    process->exit_status = status;
    for (size_t index = 0U; index < process->thread_count; ++index) {
        struct native_thread *candidate = &process->threads[index];

        candidate->exit_status = status;
        if (candidate != thread &&
            candidate->state != NATIVE_THREAD_FAULTED) {
            candidate->state = NATIVE_THREAD_EXITED;
        }
    }
    thread->state = NATIVE_THREAD_RUNNABLE;
    thread->context.rip = process->dynamic_fini_entry;
    thread->context.rdi = (uint64_t)(int64_t)status;
    return true;
}

static int64_t syscall_handle_close(
    struct native_process *process,
    phipia_handle_t handle
)
{
    enum native_handle_status status;

    cpu_interrupt_enable();
    status = native_handle_close(&process->handles, handle, close_resource,
        process);
    cpu_interrupt_disable();
    return handle_error(status);
}

static int64_t syscall_handle_duplicate(
    struct native_process *process,
    phipia_handle_t handle
)
{
    phipia_handle_t duplicate;
    const enum native_handle_status status = native_handle_duplicate(
        &process->handles, handle, &duplicate);

    if (status != NATIVE_HANDLE_OK) {
        return handle_error(status);
    }
    if (process->handles.active_handles > process->peak_handles) {
        process->peak_handles = process->handles.active_handles;
    }
    return (int64_t)duplicate;
}

static int64_t dispatch_syscall(
    struct native_process *process,
    struct native_thread *thread,
    const struct native_syscall_frame *frame
)
{
    switch (frame->rax) {
    case PHIPIA_SYS_ABI_VERSION:
        return PHIPIA_ABI_VERSION;
    case PHIPIA_SYS_EXIT:
        if (!begin_dynamic_finalizers(process, thread,
                (int32_t)frame->rdi)) {
            terminate_process(process, (int32_t)frame->rdi);
        }
        return 0;
    case PHIPIA_SYS_CONSOLE_WRITE:
        return syscall_console_write(process, frame->rdi, (size_t)frame->rsi);
    case PHIPIA_SYS_CONSOLE_READ:
        return syscall_console_read(process, frame->rdi, (size_t)frame->rsi);
    case PHIPIA_SYS_HANDLE_CLOSE:
        return syscall_handle_close(process, frame->rdi);
    case PHIPIA_SYS_HANDLE_DUPLICATE:
        return syscall_handle_duplicate(process, frame->rdi);
    case PHIPIA_SYS_MEMORY_MAP:
        return syscall_memory_map(process, frame->rdi, frame->rsi);
    case PHIPIA_SYS_MEMORY_UNMAP:
        return syscall_memory_unmap(process, frame->rdi, frame->rsi);
    case PHIPIA_SYS_FILE_OPEN:
        return syscall_file_open(process, frame->rdi);
    case PHIPIA_SYS_FILE_READ:
        return syscall_file_io(process, frame->rdi, false);
    case PHIPIA_SYS_FILE_WRITE:
        return syscall_file_io(process, frame->rdi, true);
    case PHIPIA_SYS_FILE_SEEK:
        return syscall_file_seek(process, frame->rdi);
    case PHIPIA_SYS_PATH_STAT:
        return syscall_path_stat(process, frame->rdi, frame->rsi);
    case PHIPIA_SYS_DIRECTORY_OPEN:
        return syscall_directory_open(process, frame->rdi);
    case PHIPIA_SYS_DIRECTORY_READ:
        return syscall_directory_read(process, frame->rdi, frame->rsi);
    case PHIPIA_SYS_PATH_MKDIR:
    case PHIPIA_SYS_PATH_UNLINK:
    case PHIPIA_SYS_PATH_TRUNCATE:
        return syscall_single_path_mutation(process, frame->rdi, frame->rsi,
            frame->rax);
    case PHIPIA_SYS_PATH_RENAME:
        return syscall_rename(process, frame->rdi, false);
    case PHIPIA_SYS_PATH_REPLACE:
        return syscall_rename(process, frame->rdi, true);
    case PHIPIA_SYS_VOLUME_SYNC:
        return syscall_volume_sync(process, frame->rdi);
    case PHIPIA_SYS_VOLUME_SPACE:
        return syscall_volume_space(process, frame->rdi, frame->rsi);
    case PHIPIA_SYS_TIME_MONOTONIC:
        return (process->manifest.capabilities & PHIPIA_CAP_TIME) != 0U ?
            (int64_t)clock_monotonic_ns() : -PHIPIA_EACCES;
    case PHIPIA_SYS_TIME_REALTIME:
        return syscall_time_realtime(process);
    case PHIPIA_SYS_SLEEP_UNTIL:
        return syscall_sleep_until(process, frame->rdi);
    case PHIPIA_SYS_WAIT:
        return syscall_wait(process, frame->rdi);
    case PHIPIA_SYS_RANDOM:
        return syscall_random(process, frame->rdi, (size_t)frame->rsi, false);
    case PHIPIA_SYS_RANDOM_STRONG:
        return syscall_random(process, frame->rdi, (size_t)frame->rsi, true);
    case PHIPIA_SYS_TIMER_CREATE:
        return (process->manifest.capabilities & PHIPIA_CAP_TIME) != 0U ?
            syscall_timer_create(process) : -PHIPIA_EACCES;
    case PHIPIA_SYS_TIMER_SET:
        return (process->manifest.capabilities & PHIPIA_CAP_TIME) != 0U ?
            syscall_timer_set(process, frame->rdi) : -PHIPIA_EACCES;
    case PHIPIA_SYS_CANCEL:
        return syscall_cancel(process, frame->rdi);
    case PHIPIA_SYS_WINDOW_CREATE:
        return syscall_window_create(process, frame->rdi, frame->rsi);
    case PHIPIA_SYS_SURFACE_PRESENT:
        return syscall_surface_present(process, frame->rdi);
    case PHIPIA_SYS_EVENT_READ:
        return syscall_event_read(process, frame->rdi, frame->rsi);
    case PHIPIA_SYS_POINTER_CAPTURE:
        return syscall_pointer_capture(process, frame->rdi, frame->rsi);
    case PHIPIA_SYS_DNS_RESOLVE:
        return syscall_dns_resolve(process, frame->rdi, (size_t)frame->rsi,
            frame->rdx);
    case PHIPIA_SYS_STREAM_OPEN:
        return syscall_network_open(process, false);
    case PHIPIA_SYS_STREAM_CONNECT:
        return syscall_stream_connect(process, frame->rdi, frame->rsi,
            frame->rdx);
    case PHIPIA_SYS_STREAM_READ:
        return syscall_network_io(process, frame->rdi, false, false);
    case PHIPIA_SYS_STREAM_WRITE:
        return syscall_network_io(process, frame->rdi, false, true);
    case PHIPIA_SYS_STREAM_SHUTDOWN:
        return syscall_stream_shutdown(process, frame->rdi,
            (uint32_t)frame->rsi, frame->rdx);
    case PHIPIA_SYS_DATAGRAM_OPEN:
        return syscall_network_open(process, true);
    case PHIPIA_SYS_DATAGRAM_BIND:
        return syscall_datagram_bind(process, frame->rdi,
            (uint16_t)frame->rsi);
    case PHIPIA_SYS_DATAGRAM_SEND:
        return syscall_network_io(process, frame->rdi, true, true);
    case PHIPIA_SYS_DATAGRAM_RECEIVE:
        return syscall_network_io(process, frame->rdi, true, false);
    case PHIPIA_SYS_NETWORK_ADDRESS:
        if (frame->rsi > 1U) {
            return -PHIPIA_EINVAL;
        }
        return syscall_network_address(process, frame->rdi,
            frame->rsi != 0U, frame->rdx);
    case PHIPIA_SYS_THREAD_CREATE:
        return syscall_thread_create(process, frame->rdi);
    case PHIPIA_SYS_THREAD_EXIT:
        thread->exit_status = (int32_t)frame->rdi;
        thread->state = NATIVE_THREAD_EXITED;
        return 0;
    case PHIPIA_SYS_THREAD_JOIN:
        return syscall_thread_join(process, frame->rdi);
    case PHIPIA_SYS_TLS_SET:
        return syscall_tls_set(process, frame->rdi);
    case PHIPIA_SYS_TLS_GET:
        return (int64_t)thread->fs_base;
    case PHIPIA_SYS_FUTEX_WAIT:
        return syscall_futex_wait(process, frame->rdi);
    case PHIPIA_SYS_FUTEX_WAKE:
        return syscall_futex_wake(process, frame->rdi);
    case PHIPIA_SYS_AUDIO_OPEN:
        return syscall_audio_open(process);
    case PHIPIA_SYS_AUDIO_SUBMIT:
        return syscall_audio_submit(process, frame->rdi);
    case PHIPIA_SYS_AUDIO_VOLUME:
        return syscall_audio_volume(process, frame->rdi);
    case PHIPIA_SYS_AUDIO_DRAIN:
        return syscall_audio_drain(process, frame->rdi, frame->rsi);
    case PHIPIA_SYS_PACKAGE_UPLOAD_OPEN:
        return syscall_package_upload_open(process);
    case PHIPIA_SYS_PACKAGE_UPLOAD_WRITE:
        return syscall_package_upload_write(process, frame->rdi);
    case PHIPIA_SYS_PACKAGE_UPLOAD_SEAL:
        return syscall_package_upload_seal(process, frame->rdi);
    case PHIPIA_SYS_PACKAGE_CONTROL_OPEN_INSTALL:
        return syscall_package_control_open_install(process, frame->rdi);
    case PHIPIA_SYS_PACKAGE_CONTROL_ITEM:
        return syscall_package_control_item(process, frame->rdi);
    case PHIPIA_SYS_PACKAGE_CONTROL_ATTACH:
        return syscall_package_control_attach(process, frame->rdi);
    case PHIPIA_SYS_PACKAGE_CONTROL_COMMIT:
        return syscall_package_control_commit(process, frame->rdi);
    default:
        return -PHIPIA_ENOSYS;
    }
}

static bool current_user_position_valid(
    struct native_process *process,
    uint64_t rip,
    uint64_t rsp
)
{
    const struct native_page *instruction = page_at(process, rip);

    return instruction != NULL && instruction->permissions == PAGING_EXECUTE &&
        rsp != 0U && validate_user_range(process, rsp - 1U, 1U, false);
}

uintptr_t native_process_on_syscall(struct native_syscall_frame *frame)
{
    struct native_process *process = running_process();
    struct native_thread *thread = running_thread(process);
    uintptr_t resume_stack;
    uint64_t without_started;
    uint64_t without_cycles;
    uint64_t fpu_started;
    uint64_t fpu_cycles;
    int64_t result;

    if (frame == NULL || process == NULL || thread == NULL ||
        thread->state != NATIVE_THREAD_RUNNABLE ||
        process->address_space.state != PAGING_PROCESS_SPACE_ACTIVE ||
        !current_user_position_valid(process, frame->rip, frame->rsp)) {
        return 0U;
    }
    without_started = tsc_read();
    save_syscall_context(thread, frame);
    without_cycles = tsc_read() - without_started;
    fpu_started = tsc_read();
    if (!native_fpu_save(&thread->fpu)) {
        return 0U;
    }
    fpu_cycles = tsc_read() - fpu_started;
    without_started = tsc_read();
    thread->fs_base = cpu_read_msr(IA32_FS_BASE);
    cpu_write_msr(IA32_FS_BASE, 0U);
    if (paging_process_restore_kernel(&process->address_space) !=
            PAGING_STATUS_OK) {
        return 0U;
    }
    without_cycles += tsc_read() - without_started;
    record_context_transition(process, without_cycles, fpu_cycles);
    process->last_syscall = (uint32_t)frame->rax;
    result = dispatch_syscall(process, thread, frame);
    thread->context.rax = (uint64_t)result;
    ++process->syscall_count;
    resume_stack = process_user_resume_stack();
    return resume_stack;
}

bool native_process_interrupt_active(void)
{
    const struct native_process *process = running_process();

    return process != NULL && process_user_boundary_active() &&
        process->address_space.state == PAGING_PROCESS_SPACE_ACTIVE;
}

static void report_user_backtrace(
    struct native_process *process,
    uint64_t frame_pointer
)
{
    for (size_t depth = 0U; depth < 8U; ++depth) {
        uint64_t words[2];

        if ((frame_pointer & (sizeof(uint64_t) - 1U)) != 0U ||
            !copy_from_user(process, words, frame_pointer, sizeof(words))) {
            break;
        }
        console_write("Phipia: native backtrace ");
        console_write_u64(depth);
        console_write(" frame ");
        console_write_hex(frame_pointer);
        console_write(" return ");
        console_write_hex(words[1]);
        console_write("\n");
        if (words[0] <= frame_pointer) {
            break;
        }
        frame_pointer = words[0];
    }
}

void native_process_on_interrupt(struct interrupt_frame *frame, void *context)
{
    struct native_process *process = running_process();
    struct native_thread *thread = running_thread(process);
    uintptr_t resume_stack = process_user_resume_stack();
    uint64_t without_started;
    uint64_t without_cycles;
    uint64_t fpu_started;
    uint64_t fpu_cycles;
    bool valid = frame != NULL && process != NULL && thread != NULL &&
        context == &native_gate && interrupt_frame_has_stack_tail(frame);

    if (valid) {
        without_started = tsc_read();
        save_interrupt_context(thread, frame);
        without_cycles = tsc_read() - without_started;
        fpu_started = tsc_read();
        valid = native_fpu_save(&thread->fpu);
        fpu_cycles = tsc_read() - fpu_started;
        without_started = tsc_read();
        thread->fs_base = cpu_read_msr(IA32_FS_BASE);
        cpu_write_msr(IA32_FS_BASE, 0U);
        if (paging_process_restore_kernel(&process->address_space) !=
                PAGING_STATUS_OK) {
            valid = false;
        }
        without_cycles += tsc_read() - without_started;
        if (valid) {
            record_context_transition(process, without_cycles, fpu_cycles);
        }
        if (frame->vector < INTERRUPT_EXCEPTION_COUNT) {
            console_write("Phipia: native thread fault vector ");
            console_write_u64(frame->vector);
            console_write(" error ");
            console_write_hex(frame->error_code);
            console_write(" rip ");
            console_write_hex(frame->rip);
            console_write(" address ");
            console_write_hex(frame->cr2);
            console_write(" rsp ");
            console_write_hex(interrupt_frame_stack_pointer(frame));
            console_write(" rax ");
            console_write_hex(frame->rax);
            console_write(" rbx ");
            console_write_hex(frame->rbx);
            console_write(" rbp ");
            console_write_hex(frame->rbp);
            console_write(" rdi ");
            console_write_hex(frame->rdi);
            console_write(" rsi ");
            console_write_hex(frame->rsi);
            console_write("\n");
            report_user_backtrace(process, frame->rbp);
            thread->state = NATIVE_THREAD_FAULTED;
            thread->exit_status = -PHIPIA_EFAULT;
            process->faulted = true;
            terminate_process(process, -PHIPIA_EFAULT);
        }
    }
    if (!valid && process != NULL) {
        process->faulted = true;
        terminate_process(process, -PHIPIA_EIO);
    }
    if (resume_stack == 0U ||
        interrupt_request_kernel_resume(frame, resume_stack) !=
            INTERRUPT_STATUS_OK) {
        console_panic("native process interrupt return failed");
    }
}

static bool process_has_runnable_thread(const struct native_process *process)
{
    for (size_t index = 0U; index < process->thread_count; ++index) {
        if (process->threads[index].state == NATIVE_THREAD_RUNNABLE) {
            return true;
        }
    }
    return false;
}

static bool process_has_live_thread(const struct native_process *process)
{
    for (size_t index = 0U; index < process->thread_count; ++index) {
        if (process->threads[index].state != NATIVE_THREAD_UNUSED &&
            process->threads[index].state != NATIVE_THREAD_EXITED &&
            process->threads[index].state != NATIVE_THREAD_FAULTED) {
            return true;
        }
    }
    return false;
}

static void update_waiting_threads(
    struct native_process *process,
    uint64_t now
)
{
    for (size_t index = 0U; index < process->thread_count; ++index) {
        struct native_thread *thread = &process->threads[index];

        if (thread->state == NATIVE_THREAD_SLEEP_WAIT &&
            now >= thread->deadline_ns) {
            thread->deadline_ns = 0U;
            thread->context.rax = 0U;
            thread->state = NATIVE_THREAD_RUNNABLE;
        } else if (thread->state == NATIVE_THREAD_FUTEX_WAIT &&
            thread->deadline_ns != 0U && now >= thread->deadline_ns) {
            thread->deadline_ns = 0U;
            thread->futex_address = 0U;
            thread->context.rax = (uint64_t)-(int64_t)PHIPIA_ETIMEDOUT;
            thread->state = NATIVE_THREAD_RUNNABLE;
        } else if (thread->state == NATIVE_THREAD_JOIN_WAIT) {
            for (size_t target = 0U; target < process->thread_count; ++target) {
                const struct native_thread *joined = &process->threads[target];

                if (joined->generation == thread->wait_generation &&
                    (joined->state == NATIVE_THREAD_EXITED ||
                        joined->state == NATIVE_THREAD_FAULTED)) {
                    thread->wait_generation = 0U;
                    thread->context.rax = (uint64_t)(int64_t)
                        joined->exit_status;
                    thread->state = NATIVE_THREAD_RUNNABLE;
                    break;
                }
            }
        } else if (thread->state == NATIVE_THREAD_HANDLE_WAIT) {
            int64_t ready = poll_wait_items(process, thread->wait_items,
                thread->wait_item_count);
            const bool timed_out = thread->deadline_ns != 0U &&
                now >= thread->deadline_ns;

            if (ready != 0 || timed_out) {
                if (ready >= 0 && !copy_to_user(process,
                        thread->wait_items_address, thread->wait_items,
                        thread->wait_item_count *
                            sizeof(thread->wait_items[0]))) {
                    ready = -PHIPIA_EFAULT;
                } else if (ready == 0) {
                    ready = -PHIPIA_ETIMEDOUT;
                }
                thread->wait_items_address = 0U;
                thread->wait_item_count = 0U;
                thread->deadline_ns = 0U;
                thread->context.rax = (uint64_t)ready;
                thread->state = NATIVE_THREAD_RUNNABLE;
            }
        } else if (thread->state == NATIVE_THREAD_AUDIO_DRAIN_WAIT) {
            const enum audio_native_drain_state state = audio_native_drain(
                process->generation, thread->audio_token);
            const bool timed_out = thread->deadline_ns != 0U &&
                now >= thread->deadline_ns;
            int64_t result = 0;
            bool complete = true;

            if (state == AUDIO_NATIVE_DRAIN_PENDING && !timed_out) {
                complete = false;
            } else if (timed_out) {
                result = -PHIPIA_ETIMEDOUT;
            } else if (state == AUDIO_NATIVE_DRAIN_CANCELED) {
                result = -PHIPIA_ECANCELED;
            } else if (state == AUDIO_NATIVE_DRAIN_ERROR) {
                result = -PHIPIA_EIO;
            } else if (state == AUDIO_NATIVE_DRAIN_STALE) {
                result = -PHIPIA_ESTALE;
            }
            if (complete) {
                thread->audio_token = 0U;
                thread->deadline_ns = 0U;
                thread->context.rax = (uint64_t)result;
                thread->state = NATIVE_THREAD_RUNNABLE;
            }
        } else if (thread->state == NATIVE_THREAD_CONSOLE_WAIT &&
            process->console_input_count != 0U) {
            const size_t copied = console_input_copy(process,
                process->transfer, thread->console_length);

            if (copy_to_user(process, thread->console_address,
                    process->transfer, copied)) {
                console_input_consume(process, copied);
                thread->context.rax = (uint64_t)copied;
            } else {
                thread->context.rax = (uint64_t)-(int64_t)PHIPIA_EFAULT;
            }
            thread->console_address = 0U;
            thread->console_length = 0U;
            thread->state = NATIVE_THREAD_RUNNABLE;
        }
    }
    if (!process->exiting && !process_has_live_thread(process)) {
        terminate_process(process, process->faulted ? -PHIPIA_EFAULT :
            process->exit_status);
    }
}

static bool nearest_deadline(uint64_t *deadline)
{
    bool found = false;

    for (size_t process_index = 0U; process_index < NATIVE_PROCESS_LIMIT;
         ++process_index) {
        const struct native_process *process = &processes[process_index];

        if (!process->active || process->exiting) {
            continue;
        }
        for (size_t thread_index = 0U; thread_index < process->thread_count;
             ++thread_index) {
            const struct native_thread *thread =
                &process->threads[thread_index];

            if ((thread->state == NATIVE_THREAD_SLEEP_WAIT ||
                    thread->state == NATIVE_THREAD_FUTEX_WAIT ||
                    thread->state == NATIVE_THREAD_HANDLE_WAIT ||
                    thread->state == NATIVE_THREAD_AUDIO_DRAIN_WAIT) &&
                thread->deadline_ns != 0U &&
                (!found || thread->deadline_ns < *deadline)) {
                *deadline = thread->deadline_ns;
                found = true;
            }
        }
    }
    {
        uint64_t audio_deadline;

        if (audio_native_next_deadline(&audio_deadline) &&
            (!found || audio_deadline < *deadline)) {
            *deadline = audio_deadline;
            found = true;
        }
    }
    return found;
}

static bool select_runnable_thread(void)
{
    for (size_t process_offset = 0U; process_offset < NATIVE_PROCESS_LIMIT;
         ++process_offset) {
        const size_t process_index = (scheduler_process_cursor +
            process_offset) % NATIVE_PROCESS_LIMIT;
        struct native_process *process = &processes[process_index];

        if (!process->active || process->exiting ||
            !process_has_runnable_thread(process)) {
            continue;
        }
        for (size_t thread_offset = 1U;
             thread_offset <= process->thread_count; ++thread_offset) {
            const size_t thread_index = (process->current_thread +
                thread_offset) % process->thread_count;

            if (process->threads[thread_index].state ==
                    NATIVE_THREAD_RUNNABLE) {
                current_process = process_index;
                process->current_thread = thread_index;
                scheduler_process_cursor = (process_index + 1U) %
                    NATIVE_PROCESS_LIMIT;
                return true;
            }
        }
    }
    return false;
}

static bool any_active_process(void)
{
    for (size_t index = 0U; index < NATIVE_PROCESS_LIMIT; ++index) {
        if (processes[index].active) {
            return true;
        }
    }
    return false;
}

static size_t newest_active_process(void)
{
    uint64_t generation = 0U;
    size_t selected = SIZE_MAX;

    for (size_t index = 0U; index < NATIVE_PROCESS_LIMIT; ++index) {
        if (processes[index].active &&
            processes[index].generation > generation) {
            generation = processes[index].generation;
            selected = index;
        }
    }
    return selected;
}

static struct native_process *console_input_target(void)
{
    struct native_process *selected = NULL;

    for (size_t index = 0U; index < NATIVE_PROCESS_LIMIT; ++index) {
        struct native_process *process = &processes[index];

        if (process->active && !process->exiting &&
            !process->window.allocated &&
            (process->manifest.capabilities & PHIPIA_CAP_CONSOLE) != 0U &&
            (selected == NULL ||
                process->generation > selected->generation)) {
            selected = process;
        }
    }
    return selected;
}

static bool console_input_push(struct native_process *process, uint8_t value)
{
    size_t tail;

    if (process == NULL ||
        process->console_input_count == NATIVE_CONSOLE_INPUT_CAPACITY) {
        return false;
    }
    tail = (process->console_input_head + process->console_input_count) %
        NATIVE_CONSOLE_INPUT_CAPACITY;
    process->console_input[tail] = value;
    ++process->console_input_count;
    return true;
}

static bool any_console_waiter(void)
{
    for (size_t process_index = 0U; process_index < NATIVE_PROCESS_LIMIT;
         ++process_index) {
        const struct native_process *process = &processes[process_index];

        if (!process->active || process->exiting) {
            continue;
        }
        for (size_t thread_index = 0U;
             thread_index < process->thread_count; ++thread_index) {
            if (process->threads[thread_index].state ==
                    NATIVE_THREAD_CONSOLE_WAIT) {
                return true;
            }
        }
    }
    return false;
}

static bool any_handle_waiter(void)
{
    for (size_t process_index = 0U; process_index < NATIVE_PROCESS_LIMIT;
         ++process_index) {
        const struct native_process *process = &processes[process_index];

        if (!process->active || process->exiting) {
            continue;
        }
        for (size_t thread_index = 0U;
             thread_index < process->thread_count; ++thread_index) {
            if (process->threads[thread_index].state ==
                    NATIVE_THREAD_HANDLE_WAIT ||
                process->threads[thread_index].state ==
                    NATIVE_THREAD_AUDIO_DRAIN_WAIT) {
                return true;
            }
        }
    }
    return false;
}

static void report_scheduler_stall(void)
{
    console_write("Phipia: native scheduler stalled\n");
    for (size_t process_index = 0U; process_index < NATIVE_PROCESS_LIMIT;
         ++process_index) {
        const struct native_process *process = &processes[process_index];

        if (!process->active || process->exiting) {
            continue;
        }
        console_write("Phipia: stalled process ");
        console_write_u64(process_index);
        console_write(" generation ");
        console_write_u64(process->generation);
        console_write(" threads ");
        console_write_u64(process->thread_count);
        console_write("\n");
        for (size_t thread_index = 0U;
             thread_index < process->thread_count; ++thread_index) {
            const struct native_thread *thread =
                &process->threads[thread_index];

            console_write("Phipia: stalled thread ");
            console_write_u64(thread_index);
            console_write(" generation ");
            console_write_u64(thread->generation);
            console_write(" state ");
            console_write_u64((uint64_t)thread->state);
            console_write(" rip ");
            console_write_hex(thread->context.rip);
            console_write(" rsp ");
            console_write_hex(thread->context.rsp);
            console_write(" wait-generation ");
            console_write_u64(thread->wait_generation);
            console_write(" futex ");
            console_write_hex(thread->futex_address);
            console_write(" deadline ");
            console_write_u64(thread->deadline_ns);
            console_write("\n");
        }
    }
}

static void capture_result(
    const struct native_process *process,
    struct native_process_result *result
)
{
    result->generation = process->generation;
    result->exit_status = process->exit_status;
    result->syscall_count = process->syscall_count;
    result->last_syscall = process->last_syscall;
    result->failure_stage = process->failure_stage;
    result->thread_switches = process->thread_switches;
    result->peak_pages = (uint32_t)process->peak_pages;
    result->peak_handles = (uint32_t)process->peak_handles;
    result->context_cycles_without_fpu =
        process->context_cycles_without_fpu;
    result->context_cycles_with_fpu = process->context_cycles_with_fpu;
    result->context_transition_samples =
        process->context_transition_samples;
    result->exited = process->exiting;
    result->faulted = process->faulted;
    result->resources_released = false;
}

static bool service_native_devices(void)
{
    struct keyboard_event event;
    bool success = true;

    cpu_interrupt_enable();
    while (keyboard_read(&event) == KEYBOARD_STATUS_OK) {
        struct native_process *target = console_input_target();
        const bool console_event = target != NULL && event.pressed &&
            event.character != '\0';

        if (console_event) {
            /* The bounded byte stream retains older input and drops newest. */
            (void)console_input_push(target, (uint8_t)event.character);
        }
        if (!console_event && ui_handle_keyboard(&event) != UI_STATUS_OK) {
            success = false;
        }
    }
    if (ui_is_active() && (ui_process_events() != UI_STATUS_OK ||
            ui_flush() != UI_STATUS_OK)) {
        success = false;
    }
    (void)network_service();
    cpu_interrupt_disable();
    if (!audio_native_service()) {
        success = false;
    }
    return success;
}

enum native_process_status native_process_run(struct native_process_result *result)
{
    struct native_process_result selected_result;
    bool have_result = false;
    bool cleanup_ok = true;

    if (result == NULL) {
        return NATIVE_PROCESS_NULL_ARGUMENT;
    }
    zero_bytes(result, sizeof(*result));
    zero_bytes(&selected_result, sizeof(selected_result));
    if (scheduler_active || !any_active_process()) {
        return NATIVE_PROCESS_BUSY;
    }
    cpu_interrupt_disable();
    if (interrupt_process_gate_arm(native_process_on_interrupt, &native_gate,
            &native_gate) != INTERRUPT_STATUS_OK) {
        cpu_interrupt_enable();
        return NATIVE_PROCESS_GATE;
    }
    if (!native_syscall_arm()) {
        (void)interrupt_process_gate_disarm(&native_gate);
        cpu_interrupt_enable();
        return NATIVE_PROCESS_SYSCALL;
    }
    scheduler_active = true;
    while (any_active_process()) {
        const uint64_t now = clock_monotonic_ns();

        if (!service_native_devices()) {
            cleanup_ok = false;
        }

        for (size_t index = 0U; index < NATIVE_PROCESS_LIMIT; ++index) {
            if (processes[index].active && !processes[index].exiting) {
                update_waiting_threads(&processes[index], now);
            }
        }
        if (select_runnable_thread()) {
            struct native_process *process = &processes[current_process];
            struct native_thread *thread = running_thread(process);
            uint64_t without_started;
            uint64_t without_cycles;
            uint64_t fpu_started;
            uint64_t fpu_cycles;
            enum paging_status activation;

            if (interrupt_process_gate_validate(&native_gate) !=
                    INTERRUPT_STATUS_OK) {
                cleanup_ok = false;
                process->failure_stage =
                    NATIVE_PROCESS_FAILURE_GATE_VALIDATE;
                terminate_process(process, -PHIPIA_EIO);
            } else if (native_gate.state == INTERRUPT_PROCESS_GATE_RETURNED &&
                interrupt_process_gate_rearm(&native_gate) !=
                    INTERRUPT_STATUS_OK) {
                cleanup_ok = false;
                process->failure_stage = NATIVE_PROCESS_FAILURE_GATE_REARM;
                terminate_process(process, -PHIPIA_EIO);
            } else {
                without_started = tsc_read();
                activation = paging_process_activate(&process->address_space);
                without_cycles = tsc_read() - without_started;
                fpu_started = tsc_read();
                if (activation != PAGING_STATUS_OK) {
                    cleanup_ok = false;
                    process->failure_stage =
                        NATIVE_PROCESS_FAILURE_ADDRESS_SPACE_ACTIVATE;
                    terminate_process(process, -PHIPIA_EIO);
                } else if (!native_fpu_restore(&thread->fpu)) {
                    cleanup_ok = false;
                    process->failure_stage =
                        NATIVE_PROCESS_FAILURE_FPU_RESTORE;
                    terminate_process(process, -PHIPIA_EIO);
                } else {
                    fpu_cycles = tsc_read() - fpu_started;
                    without_started = tsc_read();
                    cpu_write_msr(IA32_FS_BASE, thread->fs_base);
                    without_cycles += tsc_read() - without_started;
                    record_context_transition(process, without_cycles,
                        fpu_cycles);
                    ++process->thread_switches;
                    process_enter_user_context(&thread->context);
                }
            }
        } else {
            uint64_t deadline;

            /*
             * A handle wait is also a device wait, but its absolute timeout
             * still needs a programmed interrupt.  Service deadlines first so
             * an idle system cannot halt forever after the last device event.
             */
            if (nearest_deadline(&deadline)) {
                const uint64_t current = clock_monotonic_ns();

                if (deadline > current) {
                    uint64_t interval = deadline - current;

                    if (interval < NATIVE_SCHEDULER_MIN_SLEEP_NS) {
                        interval = NATIVE_SCHEDULER_MIN_SLEEP_NS;
                    }
                    cpu_interrupt_enable();
                    (void)timer_sleep_ns(interval);
                    cpu_interrupt_disable();
                }
            } else if (any_handle_waiter() || any_console_waiter()) {
                /* Device and UI interrupts wake an unbounded wait for polling. */
                cpu_enable_and_halt();
                cpu_interrupt_disable();
            } else {
                report_scheduler_stall();
                for (size_t index = 0U; index < NATIVE_PROCESS_LIMIT;
                     ++index) {
                    if (processes[index].active &&
                        !processes[index].exiting) {
                        processes[index].faulted = true;
                        terminate_process(&processes[index], -PHIPIA_EBUSY);
                    }
                }
            }
        }

        for (;;) {
            const size_t newest = newest_active_process();
            struct native_process_result completed;

            if (newest == SIZE_MAX || !processes[newest].exiting) {
                break;
            }
            capture_result(&processes[newest], &completed);
            if (!have_result || completed.generation >
                    selected_result.generation) {
                selected_result = completed;
                have_result = true;
            }
            if (!process_cleanup(&processes[newest])) {
                cleanup_ok = false;
                break;
            }
        }
    }
    current_process = SIZE_MAX;
    cpu_write_msr(IA32_FS_BASE, 0U);
    scheduler_active = false;
    if (!native_syscall_disarm()) {
        cleanup_ok = false;
    }
    if (interrupt_process_gate_disarm(&native_gate) != INTERRUPT_STATUS_OK) {
        cleanup_ok = false;
    }
    cpu_interrupt_enable();
    if (have_result) {
        selected_result.resources_released = native_process_resources_released();
        *result = selected_result;
    }
    return cleanup_ok && have_result ? NATIVE_PROCESS_OK :
        NATIVE_PROCESS_TEARDOWN;
}

enum native_process_status native_process_launch(
    const char *manifest_path,
    struct native_process_result *result
)
{
    uint64_t generation;
    enum native_process_status status;

    if (result == NULL) {
        return NATIVE_PROCESS_NULL_ARGUMENT;
    }
    status = native_process_spawn(manifest_path, &generation);
    if (status != NATIVE_PROCESS_OK) {
        return status;
    }
    status = native_process_run(result);
    if (status == NATIVE_PROCESS_OK && result->generation != generation) {
        return NATIVE_PROCESS_TEARDOWN;
    }
    return status;
}

enum native_process_status native_process_launch_installed(
    const char *manifest_path,
    struct native_process_result *result
)
{
    uint64_t generation;
    enum native_process_status status;

    if (result == NULL) {
        return NATIVE_PROCESS_NULL_ARGUMENT;
    }
    if (!installed_manifest_path(manifest_path)) {
        return NATIVE_PROCESS_IMAGE_REFUSED;
    }
    status = native_process_spawn_from_volume(manifest_path,
        PHIPFS_VOLUME_DATA, &generation);
    if (status != NATIVE_PROCESS_OK) {
        return status;
    }
    status = native_process_run(result);
    if (status == NATIVE_PROCESS_OK && result->generation != generation) {
        return NATIVE_PROCESS_TEARDOWN;
    }
    return status;
}

bool native_process_resources_released(void)
{
    const struct paging_state paging = paging_get_state();

    return !scheduler_active && !any_active_process() &&
        shared_code_live_pages == 0U &&
        !native_syscall_is_active() && !process_user_boundary_active() &&
        audio_native_resources_released() &&
        package_upload_resources_released() &&
        package_control_resources_released() &&
        interrupt_process_gate_resources_released() &&
        (!paging.active ||
            (cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U)) ==
                paging.root_physical_address);
}

bool native_process_self_test(size_t *completed_tests)
{
    size_t handle_tests;
    size_t fpu_tests;
    size_t audio_tests;
    const uint32_t image_tests = phipia_native_image_self_test();
    const uint32_t dynamic_tests = phipia_elf64_dynamic_self_test();

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;
    if (image_tests != 12U) {
        return false;
    }
    *completed_tests += image_tests;
    if (dynamic_tests != 9U) {
        return false;
    }
    *completed_tests += dynamic_tests;
    if (!native_handle_self_test(&handle_tests)) {
        return false;
    }
    *completed_tests += handle_tests;
    /* Ordinary boots inspect capabilities without changing CR0/CR4 or FPU state. */
    if (!native_fpu_capability_self_test(&fpu_tests)) {
        return false;
    }
    *completed_tests += fpu_tests;
    if (!audio_native_self_test(&audio_tests) ||
        audio_tests != AUDIO_NATIVE_CONTROLLED_CONTROLS) {
        return false;
    }
    *completed_tests += audio_tests;
    if (!process_user_context_layout_self_test() ||
        sizeof(struct native_syscall_frame) != 144U ||
        sizeof(struct phipia_event) != 56U) {
        return false;
    }
    *completed_tests += 3U;
    return true;
}

const char *native_process_status_string(enum native_process_status status)
{
    static const char *const messages[NATIVE_PROCESS_STATUS_COUNT] = {
        "ok",
        "null native-process argument",
        "native scheduler is busy or has no process",
        "native process limit reached",
        "application manifest could not be opened",
        "application manifest could not be read exactly",
        "application executable could not be opened",
        "application executable could not be read exactly",
        "Rust admission refused the manifest or executable",
        "application Data namespace could not be prepared",
        "application memory or handle limit was refused",
        "application frame allocation failed",
        "application address space could not be built",
        "application mapping contract failed",
        "guarded initial stack could not be constructed",
        "native x87/SSE CPU contract is unavailable",
        "native interrupt return gate could not be armed",
        "native syscall boundary could not be armed",
        "native process teardown failed"
    };

    return status < NATIVE_PROCESS_STATUS_COUNT ? messages[status] :
        "unknown native-process status";
}
