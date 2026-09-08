/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_SLOT_CLAIM_H
#define PHIPIA_SLOT_CLAIM_H

#include <stdbool.h>
#include <stddef.h>

/* Ownership covers initialization and all live use. Retire the object
 * before release; acquiring a claim is not a lock for published object data. */
static inline size_t phipia_slot_claim(bool *claims, size_t capacity)
{
    for (size_t index = 0U; index < capacity; ++index) {
        bool unclaimed = false;
        if (__atomic_compare_exchange_n(&claims[index], &unclaimed,
                true, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return index;
    }
    return capacity;
}

static inline void phipia_slot_release(bool *claims, size_t index)
{
    __atomic_store_n(&claims[index], false, __ATOMIC_RELEASE);
}

#endif
