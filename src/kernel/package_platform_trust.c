/* SPDX-License-Identifier: GPL-3.0-only */
#include <phipia/package_platform_trust.h>

static struct package_trust_table platform_table;
static bool platform_ready;

enum package_trust_status package_platform_trust_initialize(void)
{
    enum package_trust_status status;
    if (platform_ready) {
        return PACKAGE_TRUST_STATUS_OK;
    }
    status = package_trust_table_open(phipia_package_trust_asset,
        phipia_package_trust_asset_bytes, &platform_table);
    if (status == PACKAGE_TRUST_STATUS_OK) {
        platform_ready = true;
    }
    return status;
}

bool package_platform_trust_manager(struct package_manager_trust *result)
{
    if (result == NULL) {
        return false;
    }
    result->lookup = NULL;
    result->verify = NULL;
    result->context = NULL;
    if (!platform_ready) {
        return false;
    }
    package_trust_manager(&platform_table.store, result);
    return true;
}

size_t package_platform_trust_key_count(void)
{
    return platform_ready ? platform_table.store.key_count : 0U;
}
