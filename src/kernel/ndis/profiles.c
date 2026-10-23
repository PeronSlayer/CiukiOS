/* No profile has runtime qualification in this worktree. The lead adds the
 * exact revision, source operations and evidence after permitted runtime tests.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "native.h"
/* A sentinel is deliberate: an arbitrary caller-supplied record cannot open
 * execution. This array is the only production authority. */
static const struct ndis_qualification profiles[]={{0}};
static bool accepted(const struct ndis_qualification *q)
{ return q->qualified && q->shared_mask && q->name && q->evidence && q->runtime_cpu && q->runtime_irq &&
    q->runtime_fence && q->mask && q->cause && q->stop; }
bool ndis_profiles_available(void)
{
    for(unsigned i=0;i<sizeof(profiles)/sizeof(profiles[0]);i++) if(accepted(&profiles[i])) return true;
    return false;
}
const struct ndis_qualification *ndis_qualified_profile(const struct ntimage *image,const struct pci_func *p)
{
    if(!image || !p || !image->loaded || image->quarantined || !image->platform.user_host ||
       !image->platform.flag_table || !image->platform.flag_table->sealed) return 0;
    for(unsigned i=0;i<sizeof(profiles)/sizeof(profiles[0]);i++) {
        const struct ndis_qualification *q=&profiles[i];unsigned different=0;
        for(unsigned j=0;j<32;j++) different|=q->hash[j]^image->hash[j];
        if(accepted(q) && !different && q->vendor==p->vendor && q->device==p->device &&
           q->sub_vendor==p->sub_vendor && q->sub_device==p->sub_device && q->revision==p->revision) return q;
    }
    return 0;
}
