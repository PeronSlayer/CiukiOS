/* The fixed descriptor vector fits one page on i686 and native host builds.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_FD_STORAGE_H
#define CIUKI_FD_STORAGE_H
#include <ciuki/kernel.h>
/* Storage host fixtures supply their own physical-token translation. */
#if !defined(P2V) || !defined(V2P)
#include <ciuki/cpu.h>
#endif
#include <ciuki/process.h>
_Static_assert(CIUKI_OPEN_MAX*sizeof(struct proc_fd)<=PAGE_SIZE,
               "process descriptor table fits one owned page");
static inline struct proc_fd *proc_fds_new(void)
{
    uint32_t phys=pmm_alloc();
    if (!phys) return 0;
    mm_claim(phys,1,MM_HEAP);
    struct proc_fd *fds=P2V(phys);
    memset(fds,0,PAGE_SIZE);
    return fds;
}
static inline void proc_fds_free(struct proc_fd *fds)
{
    if (fds) mm_free(V2P(fds),MM_HEAP);
}
#endif
