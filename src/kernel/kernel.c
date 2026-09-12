/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Boot has one policy here: validate the complete typed plan, execute it, then
 * verify the installed receipts before handing the machine to a test or shell.
 * Subsystem ordering lives in boot_plan.c as capability edges, not calls.
 */
#include <stdint.h>

#include <phipia/boot_ledger.h>
#include <phipia/boot_plan.h>
#include <phipia/console.h>
#include <phipia/ext4_fs.h>
#include <phipia/fat32_fs.h>
#include <phipia/native_process.h>
#include <phipia/package_platform_trust.h>
#include <phipia/package_service.h>
#include <phipia/package_upload.h>
#include <phipia/shell.h>
#include <phipia/test.h>
#include <phipia/ui.h>

_Noreturn void kernel_main(uint32_t magic, uintptr_t boot_information);

/*
 * Both objects must outlive kernel_main's early stack and the later shell.
 * Their ownership is singular and explicit: kernel_main initializes them,
 * boot-plan stages populate the context, and the published ledger is read-only.
 */
static struct boot_context installed_context;
static struct boot_ledger installed_ledger;

static void initialize_package_trust(void)
{
    enum package_trust_status status = package_platform_trust_initialize();
    if (status != PACKAGE_TRUST_STATUS_OK ||
        package_platform_trust_key_count() == 0U) {
        console_panic("immutable package trust table refused");
    }
}

static void recover_package_state(void)
{
    enum phipfs_status filesystem_status = phipfs_mount(PHIPFS_VOLUME_DATA);

    if (filesystem_status != PHIPFS_STATUS_OK &&
        filesystem_status != PHIPFS_STATUS_ALREADY_MOUNTED) {
        console_write("Phipia: package recovery unavailable: ");
        console_write(phipfs_status_string(filesystem_status));
        console_putc('\n');
        return;
    }
    struct package_service_report report;
    enum package_service_status status = package_service_recover(&report);

    if (status == PACKAGE_SERVICE_STATUS_ABSENT) {
        console_write("Phipia: package transaction state absent\n");
        return;
    }
    if (status != PACKAGE_SERVICE_STATUS_OK) {
        console_write("Phipia: package recovery refused: ");
        console_write(package_service_status_string(status));
        console_write("; state ");
        console_write(package_state_status_string(report.state_status));
        console_write("; filesystem ");
        console_write(phipfs_status_string(report.filesystem_status));
        console_putc('\n');
        console_panic("unsafe package transaction state");
    }
    console_write("Phipia: package generation ");
    console_write_u64(report.generation);
    console_write(" verified files ");
    console_write_u64(report.files_verified);
    console_write(" resources released\n");
}

static void initialize_package_uploads(void)
{
    struct package_upload_report report;
    enum package_upload_status status = package_upload_initialize(&report);

    if (status != PACKAGE_UPLOAD_STATUS_OK) {
        console_write("Phipia: package upload service unavailable: ");
        console_write(package_upload_status_string(status));
        console_write("; filesystem ");
        console_write(phipfs_status_string(report.filesystem_status));
        console_putc('\n');
    }
}

static void report_ledger_refusal(
    const struct boot_ledger *ledger,
    const struct boot_context *context
)
{
    console_write("Phipia: Boot Ledger refusal: ");
    console_write(boot_ledger_status_string(ledger->status));

    if (ledger->refusal_stage != BOOT_STAGE_INVALID) {
        console_write("; stage ");
        console_write(boot_stage_name(ledger->refusal_stage));
    }

    if (ledger->refusal_capability != BOOT_CAPABILITY_INVALID) {
        console_write("; capability ");
        console_write(boot_capability_string(ledger->refusal_capability));
    }

    if (context->stage_failure_detail != NULL) {
        console_write("; detail ");
        console_write(context->stage_failure_detail);
    }

    console_putc('\n');
}

_Noreturn void kernel_main(uint32_t magic, uintptr_t boot_information)
{
    enum boot_ledger_status status;
    size_t filesystem_tests;
    size_t native_process_tests;

    /* Reversible bootstrap: named planner refusals need somewhere to speak. */
    console_initialize();
    initialize_package_trust();
    boot_context_initialize(&installed_context, magic, boot_information);

    /* Pure and bounded; runs before PAT, WBINVD or CR3 replacement. */
    if (!boot_ledger_self_test()) {
        console_panic("Boot Ledger planner self-test failed");
    }

    status = boot_plan_build(&installed_ledger);
    if (status == BOOT_LEDGER_STATUS_OK) {
        status = boot_ledger_validate(&installed_ledger);
    }

    if (status != BOOT_LEDGER_STATUS_OK) {
        report_ledger_refusal(&installed_ledger, &installed_context);
        console_panic(boot_ledger_status_string(status));
    }

    status = boot_ledger_execute(&installed_ledger, &installed_context);

    if (status != BOOT_LEDGER_STATUS_OK) {
        report_ledger_refusal(&installed_ledger, &installed_context);
        console_panic(boot_ledger_status_string(status));
    }

    status = boot_ledger_verify_installed(&installed_ledger,
        &installed_context);

    if (status != BOOT_LEDGER_STATUS_OK) {
        report_ledger_refusal(&installed_ledger, &installed_context);
        console_panic(boot_ledger_status_string(status));
    }

    boot_ledger_publish(&installed_ledger);
    console_write("Phipia: Boot Ledger installed proof passed\n");
    if (!phipfs_self_test(&filesystem_tests)) {
        console_panic("FAT32 store self-test failed");
    }
    console_write("Phipia: FAT32 store controls ");
    console_write_u64(filesystem_tests);
    console_write("/6 passed\n");
    if (installed_context.test_scenario == KERNEL_TEST_EXT4_RECOVERY &&
        !ext4_backend_test_configure_power_cut(
            installed_context.information.command_line,
            installed_context.information.command_line_length)) {
        console_panic("invalid ext4 power-cut configuration");
    }
    phipfs_initialize();
    if (installed_context.test_scenario == KERNEL_TEST_NORMAL) {
        recover_package_state();
        initialize_package_uploads();
    }
    if (!native_process_self_test(&native_process_tests)) {
        console_panic("native userspace foundation self-test failed");
    }
    console_write("Phipia: native userspace controls ");
    console_write_u64(native_process_tests);
    console_write(" passed\n");
    if (ui_is_active() && ui_flush() != UI_STATUS_OK) {
        console_write("Phipia: ledger status redraw failed\n");
    }

    if (installed_context.test_scenario == KERNEL_TEST_NORMAL) {
        kernel_test_complete_normal();
    }

    if (installed_context.test_scenario == KERNEL_TEST_BOOT_LEDGER) {
        kernel_test_complete_boot_ledger(&installed_context);
    }

    if (installed_context.test_scenario == KERNEL_TEST_PHIPIA_PROOF) {
        kernel_test_complete_phipia_proof();
    }

    if (installed_context.test_scenario == KERNEL_TEST_DEVICE_SUBSTRATE) {
        kernel_test_complete_device_substrate();
    }

    if (installed_context.test_scenario == KERNEL_TEST_XHCI) {
        kernel_test_complete_xhci();
    }

    if (installed_context.test_scenario == KERNEL_TEST_NVME) {
        kernel_test_complete_nvme();
    }

    if (installed_context.test_scenario == KERNEL_TEST_FILESYSTEM) {
        kernel_test_complete_filesystem();
    }

    if (installed_context.test_scenario == KERNEL_TEST_PROCESS) {
        kernel_test_complete_process();
    }

    if (installed_context.test_scenario == KERNEL_TEST_LINUX_ABI) {
        kernel_test_complete_linux_abi();
    }

    if (installed_context.test_scenario == KERNEL_TEST_LINUX_ABI_UNAME) {
        kernel_test_complete_linux_uname();
    }

    if (installed_context.test_scenario == KERNEL_TEST_PHIPIA_PROOF_USERLAND) {
        kernel_test_complete_phipia_proof_userland();
    }

    if (installed_context.test_scenario ==
            KERNEL_TEST_PHIPIA_PROOF_USERLAND_ABSENT) {
        kernel_test_complete_phipia_proof_userland_absent();
    }

    if (installed_context.test_scenario ==
            KERNEL_TEST_PHIPIA_PROOF_USERLAND_INTERACTIVE) {
        kernel_test_complete_phipia_proof_userland_interactive();
    }

    if (installed_context.test_scenario ==
            KERNEL_TEST_PHIPIA_PROOF_USERLAND_INTERACTIVE_ABSENT) {
        kernel_test_complete_phipia_proof_userland_interactive_absent();
    }

    if (installed_context.test_scenario >= KERNEL_TEST_FAT32_SYSTEM &&
        installed_context.test_scenario <= KERNEL_TEST_FAT32_HANDLES) {
        kernel_test_complete_fat32();
    }

    if (installed_context.test_scenario == KERNEL_TEST_MULTIPROCESS) {
        kernel_test_complete_multiprocess();
    }

    if (installed_context.test_scenario == KERNEL_TEST_DRIVER_MATRIX ||
        installed_context.test_scenario ==
            KERNEL_TEST_DRIVER_MATRIX_BUILTIN) {
        kernel_test_complete_driver_matrix();
    }

    if (installed_context.test_scenario == KERNEL_TEST_AUDIO) {
        kernel_test_complete_audio();
    }

    if (installed_context.test_scenario == KERNEL_TEST_NVIDIA ||
        installed_context.test_scenario == KERNEL_TEST_NVIDIA_BUILTIN) {
        kernel_test_complete_nvidia();
    }

    if (installed_context.test_scenario == KERNEL_TEST_NATIVE) {
        kernel_test_complete_native();
    }

    if (installed_context.test_scenario == KERNEL_TEST_NATIVE_LUA) {
        kernel_test_complete_native_lua();
    }

    if (installed_context.test_scenario == KERNEL_TEST_NATIVE_SQLITE) {
        kernel_test_complete_native_sqlite();
    }

    if (installed_context.test_scenario == KERNEL_TEST_NATIVE_CANVAS) {
        kernel_test_complete_native_canvas();
    }

    if (installed_context.test_scenario == KERNEL_TEST_NATIVE_NETWORK) {
        kernel_test_complete_native_network();
    }

    if (installed_context.test_scenario == KERNEL_TEST_NATIVE_RUST) {
        kernel_test_complete_native_rust();
    }

    if (installed_context.test_scenario == KERNEL_TEST_NATIVE_CRASH) {
        kernel_test_complete_native_crash();
    }

    if (installed_context.test_scenario >= KERNEL_TEST_NATIVE_ELF_REFUSAL &&
        installed_context.test_scenario <= KERNEL_TEST_NATIVE_ABI_REFUSAL) {
        kernel_test_complete_native_admission_refusal();
    }

    if (installed_context.test_scenario == KERNEL_TEST_NATIVE_RELAUNCH) {
        kernel_test_complete_native_relaunch();
    }

    if (installed_context.test_scenario == KERNEL_TEST_NATIVE_AUDIO) {
        kernel_test_complete_native_audio();
    }

    if (installed_context.test_scenario == KERNEL_TEST_NATIVE_SDL) {
        kernel_test_complete_native_sdl();
    }

    if (installed_context.test_scenario == KERNEL_TEST_NATIVE_DYNAMIC) {
        kernel_test_complete_native_dynamic();
    }

    if (installed_context.test_scenario == KERNEL_TEST_NATIVE_HTTPS) {
        kernel_test_complete_native_https();
    }

    if (installed_context.test_scenario == KERNEL_TEST_NATIVE_PHIP) {
        kernel_test_complete_native_phip();
    }

    if (installed_context.test_scenario == KERNEL_TEST_EXT4_RECOVERY) {
        kernel_test_complete_ext4_recovery();
    }

    if (installed_context.test_scenario >=
            KERNEL_TEST_NETWORK_NIC_DISCOVERY &&
        installed_context.test_scenario <=
            KERNEL_TEST_NETWORK_TCP_REFUSED) {
        kernel_test_complete_network();
    }

    shell_run();
}
