/* Borrowed user aliases and PE staging policy, kernel-owned storage only.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NDIS_USER_MEMORY_H
#define CIUKI_NDIS_USER_MEMORY_H
#include "userhost.h"
#include <ciuki/mm.h>
#include <ciuki/registry.h>
struct ndis_user_grant {
    struct ndis_host_map view;
    int claim;
    gen_t claim_generation;
    struct mm_pin pin;
    bool admitted;
};
struct ndis_user_export { const struct ndis_export *symbol; uint32_t address, bytes; };
struct ndis_user_memory {
    struct aspace *as;
    struct ndis_user_grant grant[NDIS_HOST_MAPS];
    const struct ndis_user_export *exports; /* private, qualified shim views */
    unsigned export_count;
    uint32_t generation, execution_base, reserved_bytes, physical, bytes, mapped;
    struct mm_pin image_pin;
    bool retained;
    /* Sole user task is stopped; no fork/exec/native mmap owns this AS. */
    bool (*quiescent)(void *);
    void *context;
    struct ndis_flag_table *flag_table;
    struct ndis_flag_workspace *flag_workspace;
};
/* Called in passive context before exposure. BAR grants must be a whole-page
 * qualified aperture, exactly owned by the active claim; no page rounding. */
int ndis_user_grant_bar(struct ndis_user_memory *,unsigned slot,int claim,gen_t,
                        uint32_t physical,uint32_t bytes);
/* The trusted allocating object gives its original ledger token. No syscall
 * accepts an arbitrary token/physical address from the driver. */
int ndis_user_grant_dma(struct ndis_user_memory *,unsigned slot,const struct mm_pin *);
bool ndis_user_mapping_owned(void *,const struct ndis_host_map *);
int ndis_user_mapping_apply(void *,const struct ndis_host_map *,bool install);
void ntimage_user_platform(struct ntimage_platform *,struct ndis_user_memory *);
#endif
