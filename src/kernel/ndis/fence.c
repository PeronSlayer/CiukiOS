/* SPDX-License-Identifier: GPL-2.0-only */
#include "fence.h"
static bool owner_matches(const char *a,const char *b) {
    if(!a || !b) return false;
    for(;*a && *a==*b;a++,b++) { }
    return *a==*b;
}
int ndis_fence_prepare(struct ndis_pci_fence *f,struct ndis_callback *c,unsigned index) {
    const struct pci_func *p=pci_get(index);
    if(!f || f->prepared || !c || c->depth || c->state==NDIS_CALLBACK_QUARANTINED ||
       !c->image || !c->image->loaded || c->image->quarantined || !c->owner ||
       !p || (p->header_type&0x7f) || p->class_code!=2 || p->subclass!=0 || !c->claim_count ||
       c->claim_count>NDIS_CALLBACK_CLAIMS) return 0;
    bool bar=false;
    for(unsigned i=0;i<c->claim_count;i++) {
        int h=c->claims[i];
        if(h<0 || !registry_valid(h,c->generations[i])) return 0;
        const struct resource *r=registry_get((unsigned)h);
        if(!r || r->state!=RS_ACTIVE || r->start>=r->end || !owner_matches(c->owner,r->owner)) return 0;
        if(r->type==RES_IRQ) {
            if(r->start!=p->irq_line || r->end!=r->start+1 || !r->shareable) return 0;
        } else if(r->type==RES_PORT || r->type==RES_MMIO) {
            if(r->type==RES_PORT && r->end>65536) return 0;
            bool match=false;
            for(unsigned b=0;b<6;b++) {
                uint32_t raw=p->bar[b];
                if(!(raw&1) && (raw&6)==4) { b++; continue; } /* skip high DWORD too */
                if(!raw || raw==UINT32_MAX || (!(raw&1) && (raw&6))) continue; /* 32-bit BARs only */
                if(((raw&1)?RES_PORT:RES_MMIO)==r->type &&
                   (raw&((raw&1)?~3u:~15u))==r->start) match=true;
            }
            if(!match) return 0;
            bar=true;
        } else return 0;
    }
    if(!bar) return 0;
    f->function=*p; f->callback=c; f->prepared=true;
    return 1;
}
bool ndis_fence_emergency(void *context,const char *reason) {
    (void)reason;
    struct ndis_pci_fence *f=context;
    if(!f || !f->prepared) return false;
    f->command_verified=ndis_pci_disable(&f->function,&f->command_before,&f->command_after);
    /* callback_fault immediately detaches exact-generation IRQ claims via
     * registry_quarantine. Neither command readback nor detach proves idle. */
    return false;
}
