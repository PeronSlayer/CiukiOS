/* Accepted endpoint leases over the frozen call-101 ABI. SPDX-License-Identifier: MIT */
#ifndef CIUKI_REGISTRY_KERNEL_ADAPTER_H
#define CIUKI_REGISTRY_KERNEL_ADAPTER_H
#include "broker.h"
#include <ciuki/abi.h>
struct rb_kernel_link {
    struct ciuki_security_link wire;
    struct ciuki_credential credential;
    uint32_t serial,refs;
    int retiring;
};
struct rb_kernel {
    struct rb_kernel_link links[RB_LINK_MAX];
    uint32_t serial;
    /* SYSTEM/ACCOUNT binding supplies only public account state, committed
     * profile and validated installation SID; no verifier crosses this seam. */
    void *context;
    int (*account)(void *,uint32_t,struct rb_identity *,int *enabled);
};
int rb_kernel_init(struct rb_kernel *,struct rb_authority *);
int rb_kernel_accept(struct rb_kernel *,uintptr_t *lease,int *fd,uint32_t *generation);
void rb_kernel_retire(struct rb_kernel *,uintptr_t lease);
int rb_kernel_live(struct rb_kernel *,uintptr_t lease);
#endif
