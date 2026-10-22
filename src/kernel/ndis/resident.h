/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NDIS_RESIDENT_H
#define CIUKI_NDIS_RESIDENT_H
#include "ntimage.h"
#include <ciuki/mm.h>
struct ndis_resident_memory {
    struct mm_pin pin;
    uint32_t physical, bytes;
    /* Caller reserves this grant as part of the shared F6 worst-case profile;
     * this allocator cannot mint a second independent 4 MiB budget. */
    uint32_t reserved_bytes, generation;
};
void ntimage_kernel_platform(struct ntimage_platform *, struct ndis_resident_memory *);
#endif
