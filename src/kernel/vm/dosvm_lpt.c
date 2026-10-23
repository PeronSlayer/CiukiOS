/* INT17 and trapped virtual LPT registers; physical DMA stays in the VMM.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/lpt.h>
#include "dosvm_internal.h"

static const uint16_t guest_lpt_bases[3]={0x378,0x278,0x3bc};
static bool guest_lpt_cancelled(void *context)
{
    struct dosvm *v=context;
    return !v || v->info.state>=DOSVM_STOPPING;
}
int dosvm_lpt_lease(struct dosvm *v, unsigned slot, unsigned physical, bool raw)
{
    if (!v || slot>=3 || physical>=CIUKI_LPT_MAX || v->info.state>=DOSVM_STOPPING) return -EINVAL;
    struct dosvm_lpt_state *s=&v->lpt[slot];
    if (s->generation) {
        if (s->index==physical && s->raw==raw) return 0;
        return -EBUSY;
    }
    uint32_t g=0; int e=lpt_open(physical,v->info.key.id,v->info.key.generation,raw,&g);
    if (!e) { s->index=physical; s->generation=g; s->raw=raw; }
    return e;
}
static int guest_lpt_acquire(struct dosvm *v, unsigned slot)
{
    return v->lpt[slot].generation ? 0 : dosvm_lpt_lease(v,slot,v->lpt[slot].index,false);
}
static int guest_lpt_port(struct dosvm *v, uint16_t port, unsigned width, bool write, uint32_t *value, void *arg)
{
    if (arg!=v) return -EINVAL;
    if (!v || v->info.state>=DOSVM_STOPPING) return -ENODEV;
    unsigned slot=0;
    while (slot<3 && !(port>=guest_lpt_bases[slot] && port<guest_lpt_bases[slot]+(slot==2 ? 3 : 5))) slot++;
    if (slot==3) return -EINVAL;
    if (!write) *value=width==4 ? UINT32_MAX : width==2 ? 0xffff : 0xff;
    if (width!=1) return 0; /* never split word cycles into hardware transactions */
    /* Preserve the OUT operand before initializing an IN result. */
    uint8_t b=(uint8_t)*value;
    int e=guest_lpt_acquire(v,slot);
    if (!e) e=lpt_raw_io(v->lpt[slot].index,v->lpt[slot].generation,
                         port-guest_lpt_bases[slot],write,&b);
    if (!write) *value=e ? 0xff : b;
    return 0; /* absent/denied hardware never faults the whole VM */
}
int dosvm_lpt_bios(struct dosvm *v, struct v86_frame *f)
{
    unsigned slot=(uint16_t)f->tf.edx, fn=(uint8_t)(f->tf.eax>>8);
    uint8_t status=1;
    if (slot<3 && fn<=2 && !guest_lpt_acquire(v,slot)) {
        struct dosvm_lpt_state *s=&v->lpt[slot];
        int e=s->raw ? -EBUSY : 0;
        if (!e && fn==0) {
            uint8_t b=(uint8_t)f->tf.eax;
            e=lpt_transfer(s->index,s->generation,&b,1,true,false,guest_lpt_cancelled,v);
            if (e==1) e=0;
        } else if (!e && fn==1) {
            struct ciuki_lpt_init init={1,sizeof(init),0};
            e=lpt_control(s->index,s->generation,LPT_INIT,&init,sizeof(init),true);
        }
        struct ciuki_lpt_status info={.version=1,.size=sizeof(info)};
        int query=lpt_control(s->index,s->generation,LPT_GET_STATUS,&info,sizeof(info),false);
        if (!query) status=(uint8_t)(((info.status^0x48)&0xf8)|(e ? 1 : 0));
    }
    f->tf.eax=(f->tf.eax&~0xff00u)|((uint32_t)status<<8);
    return 0; /* AL, FLAGS and every other register are preserved */
}
int dosvm_lpt_init(struct dosvm *v)
{
    unsigned count=0;
    for (unsigned i=0;i<3;i++) {
        v->lpt[i]=(struct dosvm_lpt_state){.index=i};
        struct ciuki_lpt_status s; uint16_t base=0;
        if (lpt_snapshot(i,&s,0) && (s.flags&CIUKI_LPT_READY)) { base=guest_lpt_bases[i]; count++; }
        uint8_t timeout=20;
        if (dosvm_write(v,0x408+i*2,&base,2) || dosvm_write(v,0x478+i,&timeout,1)) return -EFAULT;
        int e=dosvm_port_register(v->info.key,guest_lpt_bases[i],
                                  guest_lpt_bases[i]+(i==2 ? 2 : 4),guest_lpt_port,v);
        if (e) return e;
    }
    uint16_t equipment;
    int e=dosvm_read(v,0x410,&equipment,2);
    if (e) return e;
    equipment=(equipment & ~0xc000u)|(count<<14);
    return dosvm_write(v,0x410,&equipment,2);
}
void dosvm_lpt_stop(struct dosvm *v)
{
    for (unsigned i=0;i<3;i++) if (v->lpt[i].generation) {
        lpt_close(v->lpt[i].index,v->lpt[i].generation); v->lpt[i].generation=0; v->lpt[i].raw=false;
    }
}
