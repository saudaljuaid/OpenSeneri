/* SPDX-License-Identifier: GPL-3.0-only */
#include "proof.h"

static _Thread_local int dynamic_tls = 41;

__attribute__((constructor)) static void library_initialize(void)
{
    static const char marker[] = "PHIPIA DYNAMIC LIB INIT\n";

    ++dynamic_tls;
    proof_write(marker, sizeof(marker) - 1U);
}

__attribute__((destructor)) static void library_finalize(void)
{
    static const char marker[] = "PHIPIA DYNAMIC LIB FINI\n";

    proof_write(marker, sizeof(marker) - 1U);
}

int dynamic_add(int value)
{
    return value + dynamic_tls;
}

int dynamic_tls_value(void)
{
    return dynamic_tls;
}
