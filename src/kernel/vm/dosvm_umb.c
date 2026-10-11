/* XMS UMB arena; DOS's MCB chain and INT21/58 remain personality-owned.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include "dosvm_internal.h"

static unsigned umb_pages(struct dosvm *v)
{ uint32_t end=v->memory.frame_segment==0xD000 ? 0xD0000 : VM_UMB_END; return (end-VM_UMB_FIRST)/PAGE_SIZE; }
static unsigned umb_largest(struct dosvm *v)
{
    unsigned run=0, best=0;
    for (unsigned i=0;i<umb_pages(v);i++) { run=v->memory.umb[i] ? 0 : run+1; if (run>best) best=run; }
    return best;
}
uint8_t dosvm_umb(struct dosvm *v, struct v86_frame *f, unsigned fn)
{
    if (!v->memory.umb_enabled) return 0x80;
    unsigned request=(uint16_t)(fn==0x12 ? f->tf.ebx : f->tf.edx);
    unsigned pages=(request+255)/256, total=umb_pages(v), first=total;
    if (fn==0x10) {
        unsigned run=0;
        if (pages) for (unsigned i=0;i<total;i++) {
            run=v->memory.umb[i] ? 0 : run+1;
            if (run==pages) { first=i+1-pages; break; }
        }
        if (first==total) {
            unsigned largest=umb_largest(v); vm_low(&f->tf.edx,largest*256);
            return largest ? 0xB0 : 0xB1;
        }
    } else {
        uint32_t address=(uint16_t)f->tf.edx*16u;
        if (address<VM_UMB_FIRST || address>=VM_UMB_FIRST+total*PAGE_SIZE || (address&(PAGE_SIZE-1))) return 0xB2;
        first=(address-VM_UMB_FIRST)/PAGE_SIZE;
        unsigned old=v->memory.umb[first];
        if (!old || old==0xFFFF) return 0xB2;
        if (fn==0x11) { memset(v->memory.umb+first,0,old*sizeof(uint16_t)); return 0; }
        bool fits=pages && first+pages<=total;
        for (unsigned i=old;fits && i<pages;i++) fits=!v->memory.umb[first+i];
        if (!fits) { vm_low(&f->tf.edx,umb_largest(v)*256); return 0xB0; }
        memset(v->memory.umb+first,0,old*sizeof(uint16_t));
    }
    v->memory.umb[first]=pages;
    for (unsigned i=1;i<pages;i++) v->memory.umb[first+i]=0xFFFF;
    if (fn==0x10) { vm_low(&f->tf.ebx,(VM_UMB_FIRST+first*PAGE_SIZE)/16); vm_low(&f->tf.edx,pages*256); }
    return 0;
}
