/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "src/kernel/ndis/fence.h"
static struct pci_func pci={.bus=2,.dev=3,.vendor=0x8086,.device=0x100e,
    .class_code=2,.irq_line=11,.bar={0x1001}};
static struct resource resources[2]={
    {.type=RES_PORT,.start=0x1000,.end=0x1020,.generation=11,.state=RS_ACTIVE,.owner="fixture"},
    {.type=RES_IRQ,.start=11,.end=12,.generation=12,.state=RS_ACTIVE,.shareable=true,.owner="fixture"}};
static unsigned disables, quarantines;
static bool readback=true;
bool registry_valid(int h,gen_t g) { return h>=0 && h<2 && resources[h].generation==g && resources[h].state==RS_ACTIVE; }
const struct resource *registry_get(unsigned h) { return h<2?&resources[h]:0; }
const struct pci_func *pci_get(unsigned i) { return i?0:&pci; }
int registry_quarantine(int h,gen_t g) {
    assert(h>=0 && h<2 && resources[h].generation==g);
    if(resources[h].state!=RS_QUARANTINED) { resources[h].state=RS_QUARANTINED; quarantines++; }
    return 0;
}
bool ndis_pci_disable(const struct pci_func *p,uint16_t *before,uint16_t *after) {
    assert(p->bus==2 && p->dev==3 && p->device==0x100e); disables++;
    *before=7; *after=readback?0x400:7; return readback;
}
static struct ntimage image;
static struct ndis_callback c;
static struct ndis_pci_fence f;
static void fresh(void) {
    resources[0].state=resources[1].state=RS_ACTIVE;
    image=(struct ntimage){.loaded=true,.scanned=true,.pe={.base=0xc1000000,.size=0x1000,
        .section_count=1,.sections={{.span=0x1000,.flags=0x20000000}}}};
    c=(struct ndis_callback){.image=&image,.owner="fixture",.dirql=5,.claims={0,1},.generations={11,12},.claim_count=2};
    f=(struct ndis_pci_fence){0};
    assert(ndis_fence_prepare(&f,&c,0)); assert(!ndis_fence_prepare(&f,&c,0));
    c.ops=(struct ndis_callback_ops){.context=&f,.fence=ndis_fence_emergency};
}
int main(void) {
    for(unsigned level=0;level<3;level++) {
        fresh(); unsigned before=disables, q=quarantines;
        assert(ndis_callback_begin(&c,0xc1000000,level==0?0:level==1?2:5,100,50));
        assert(!ndis_callback_expired(&c,149)); ndis_callback_watchdog(&c,150);
        assert(disables==before+1 && quarantines==q+2 && image.quarantined && !c.fence_idle);
        assert(!c.depth && !c.irql && f.command_verified);
        ndis_callback_watchdog(&c,999); ndis_callback_fault(&c,"again");
        assert(disables==before+1 && !ndis_callback_begin(&c,0xc1000000,0,200,50));
        struct ndis_callback replacement={.image=&image,.dirql=5,
            .ops={.context=&f,.fence=ndis_fence_emergency}};
        assert(!ndis_callback_begin(&replacement,0xc1000000,0,200,50));
        assert(replacement.state==NDIS_CALLBACK_QUARANTINED && disables==before+1);
    }
    fresh(); assert(ndis_callback_begin(&c,0xc1000000,0,1,100)); uint32_t a=0,b=0; uint8_t old=99;
    assert(ndis_spin_acquire(&c,&a,false,&old) && old==0 && a==1 && c.irql==2);
    assert(ndis_spin_acquire(&c,&b,true,&old) && old==2 && b==1);
    assert(!ndis_spin_acquire(&c,&a,true,&old)); /* UP reentry faults, never spins */
    assert(!a && !b && !c.lock_count && !c.irql && image.quarantined);
    fresh(); assert(ndis_callback_begin(&c,0xc1000000,2,1,100));
    assert(!ndis_spin_release(&c,&a,true,2) && image.quarantined);
    fresh(); assert(ndis_callback_begin(&c,0xc1000000,5,1,100));
    assert(!ndis_spin_acquire(&c,&a,false,&old) && image.quarantined);
    fresh(); assert(ndis_callback_begin(&c,0xc1000000,0,1,100));
    readback=false; ndis_callback_fault(&c,"readback-failure");
    assert(!f.command_verified && !c.fence_idle && image.quarantined);
    fresh(); resources[0].start++; f=(struct ndis_pci_fence){0};
    assert(!ndis_fence_prepare(&f,&c,0)); resources[0].start--;
    f=(struct ndis_pci_fence){0}; c.generations[0]++;
    assert(!ndis_fence_prepare(&f,&c,0));
    puts("PASS callback budgets locks cleanup quarantine fence ownership and failed readback retention");
    fflush(stdout);
}
