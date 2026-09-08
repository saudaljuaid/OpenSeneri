/* SPDX-License-Identifier: GPL-3.0-only */
/* Validate the actual C executor's completed-command cut point with NVMe doubles. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "../src/kernel/ext4_fs.c"

static jmp_buf cut_exit;
static unsigned writes, flushes, refuse_write;
static bool refuse_flush, exit_port;
static uint8_t disk[16384], input[8192];
static char transcript[4096];
static size_t transcript_length;

void console_putc(char byte)
{
    assert(transcript_length + 1U < sizeof(transcript));
    transcript[transcript_length++] = byte;
    transcript[transcript_length] = '\0';
}
void console_write(const char *text) { while (*text != '\0') console_putc(*text++); }
void console_write_u64(uint64_t value)
{
    char encoded[32];
    (void)snprintf(encoded, sizeof(encoded), "%llu", (unsigned long long)value);
    console_write(encoded);
}
void cpu_out32(uint16_t port, uint32_t value)
{
    assert(port == EXT4_POWER_CUT_EXIT_PORT && value == EXT4_POWER_CUT_EXIT_VALUE);
    exit_port = true;
}
_Noreturn void console_halt(void) { assert(exit_port); longjmp(cut_exit, 1); }

enum nvme_status nvme_volume_read(struct nvme_volume_session *session, uint64_t lba,
    uint8_t *destination, size_t length)
{
    assert(session->active && lba < 4U && length == 4096U);
    memcpy(destination, disk + lba * 4096U, length);
    return NVME_STATUS_OK;
}
enum nvme_status nvme_volume_write(struct nvme_volume_session *session, uint64_t lba,
    const uint8_t *source, size_t length)
{
    assert(session->active && session->writable && lba < 4U && length == 4096U);
    ++writes;
    if (writes == refuse_write) return NVME_STATUS_COMPLETION_STATUS;
    memcpy(disk + lba * 4096U, source, length);
    return NVME_STATUS_OK;
}
enum nvme_status nvme_volume_flush(struct nvme_volume_session *session)
{
    assert(session->active && session->writable);
    ++flushes;
    return refuse_flush ? NVME_STATUS_COMPLETION_STATUS : NVME_STATUS_OK;
}

static void configure(const char *command)
{
    ext4_test_configured = false;
    assert(ext4_backend_test_configure_power_cut(command, strlen(command)));
    writes = flushes = refuse_write = 0U;
    refuse_flush = exit_port = false;
    transcript_length = 0U;
    transcript[0] = '\0';
}

int main(void)
{
    const char *invalid[] = {"phipia.ext4-storage-cut=", "phipia.ext4-storage-cut=-1",
        "phipia.ext4-storage-cut=129", "phipia.ext4-storage-cut=4294967296",
        "phipia.ext4-storage-cut=1x", "phipia.ext4-storage-cut=1 phipia.ext4-cut=1",
        "phipia.ext4-cut=1 phipia.ext4-storage-cut=0",
        "phipia.ext4-storage-cut=0 phipia.ext4-storage-cut=0", "phipia.ext4-cut=0"};
    for (size_t index = 0U; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        ext4_test_configured = false;
        assert(!ext4_backend_test_configure_power_cut(invalid[index], strlen(invalid[index])));
        assert(!ext4_test_configured);
    }
    struct ext4_mount_state *mount = &ext4_mounts[0];
    mount->operation_active = true;
    mount->media_bytes = sizeof(disk);
    mount->session.active = mount->session.writable = true;
    mount->session.logical_block_bytes = 4096U;
    memset(input, 'n', sizeof(input));
    configure("");
    assert(phipia_ext4_block_write((uintptr_t)mount, 0U, input, sizeof(input)) == 0);
    assert(writes == 2U && ext4_test_storage_completed == 0U && transcript_length == 0U);
    configure("phipia.ext4-storage-cut=0");
    assert(ext4_backend_test_power_cut_configured() && !ext4_backend_test_fail_storage_once(1U));
    memset(disk, 'o', sizeof(disk));
    assert(phipia_ext4_block_write((uintptr_t)mount, 4095U, input, 4098U) == 0);
    assert(writes == 3U && ext4_test_storage_completed == 3U);
    assert(disk[4094] == 'o' && disk[4095] == 'n' && disk[8192] == 'n' && disk[8193] == 'o');
    assert(strstr(transcript, "ST EXT4 STORAGE 3 write 2\n") != NULL);
    configure("phipia.ext4-storage-cut=0");
    refuse_write = 2U;
    assert(phipia_ext4_block_write((uintptr_t)mount, 0U, input, sizeof(input)) == -1);
    assert(writes == 2U && ext4_test_storage_completed == 1U);
    refuse_flush = true;
    assert(phipia_ext4_block_flush((uintptr_t)mount, PHIPIA_EXT4_FLUSH_COMMIT) == -1);
    assert(ext4_test_storage_completed == 1U && ext4_test_durable_boundary == 0U);
    for (unsigned ordinal = 1U; ordinal <= 3U; ++ordinal) {
        char command[64];
        (void)snprintf(command, sizeof(command), "phipia.ext4-storage-cut=%u", ordinal);
        configure(command);
        if (setjmp(cut_exit) == 0) {
            assert(phipia_ext4_block_write((uintptr_t)mount, 0U, input, sizeof(input)) == 0);
            (void)phipia_ext4_block_flush((uintptr_t)mount, PHIPIA_EXT4_FLUSH_COMMIT);
            assert(!"configured cut failed to halt");
        }
        assert(exit_port && ext4_test_storage_completed == ordinal);
        assert(writes == (ordinal < 2U ? ordinal : 2U) && flushes == (ordinal == 3U ? 1U : 0U));
        assert(strstr(transcript, "ST EXT4 STORAGE CUT ") != NULL);
        if (ordinal == 3U) assert(strstr(transcript, "ST EXT4 DURABLE 1 commit\n") != NULL);
    }
    puts("ext4 device cuts: exact completed block/flush, partial-block preservation, refusal exclusion and strict configuration PASS");
    return 0;
}
