/* SPDX-License-Identifier: MIT
 * fx-14: free physical memory as the browser process sees it. Static, so
 * each library that includes it (libdom's binding, libnsgif's image ledger)
 * stays self-contained. CiukiOS: call 70 (ciuki_proc_list) with a buffer
 * that holds only the ciuki_proc_system record (free_pages is
 * pmm_free_count(); the call needs no authority). Host builds: the model of
 * the 128 MiB baseline machine (docs/design/hardware-baseline.md), which
 * tests can override with CIUKI_NETSURF_FREE_BYTES=<bytes>. */
#ifndef CIUKI_NETSURF_DOCMEM_QUERY_H
#define CIUKI_NETSURF_DOCMEM_QUERY_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "sdk/netsurf/port/docmem.h"
#if defined(__CIUKIOS__) && !defined(CIUKI_HOST)
#include <ciuki/proc.h>
#endif
static size_t ciuki_docmem_query(void)
{
    uint64_t bytes=CIUKI_DOC_MODEL_FREE_BYTES;
#if defined(__CIUKIOS__) && !defined(CIUKI_HOST)
    struct ciuki_proc_system sys;
    _Static_assert(sizeof(sys)>=CIUKI_PROC_LIST_MIN,"call 70 minimum buffer");
    if (ciuki_proc_list(&sys,sizeof(sys))>=0 && sys.version==CIUKI_PROC_VERSION &&
        sys.size>=sizeof(sys) && sys.usable_pages)
        bytes=(uint64_t)sys.free_pages*4096u;
#else
    const char *text=getenv("CIUKI_NETSURF_FREE_BYTES");
    if (text && *text) {
        char *end=NULL;
        unsigned long long value=strtoull(text,&end,10);
        if (end && !*end) bytes=value;
    }
#endif
    /* Budgets are fractions of this value; keep the arithmetic in range. */
    return bytes>SIZE_MAX/2 ? SIZE_MAX/2 : (size_t)bytes;
}
#endif
