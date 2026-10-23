/* PC parallel-port service, independent implementation of PC87338/97338
 * (November 1998), parallel-port register/protocol chapter. Behavioral checks:
 * Linux v6.12 drivers/parport/parport_pc.c (Phil Blundell, Tim Waugh et al.)
 * and SeaBIOS rel-1.16.3 src/serial.c. No donor code is incorporated.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/lpt.h>
#include <ciuki/task.h>
#include <ciuki/mm.h>
#include <ciuki/arch.h>
#include <ciuki/firmware.h>
#pragma weak firmware_inventory_page

struct lpt_port {
    struct lpt_resource resource;
    struct lpt_resource pending;
    struct lpt_io io;
    struct ciuki_lpt_status status;
    struct ciuki_lpt_config config;
    int claims[4]; /* low, high, DMA channel, exclusive masked DMA terminal IRQ */
    gen_t generations[4];
    uint32_t bounce;
    struct mm_pin pin;
    uint8_t saved_data, saved_control, saved_ecr, control;
    bool present, busy, closing, dma_active, negotiated;
    const char *reason;
};
static struct lpt_port lpts[CIUKI_LPT_MAX];
static uint32_t lpt_generation;
static bool lpt_initialized;
static const char *const lpt_names[CIUKI_LPT_MAX] =
    {"lpt1","lpt2","lpt3","lpt4","lpt5","lpt6","lpt7","lpt8"};
static uint8_t lpt_read(void *c, uint16_t p) { (void)c; return inb(p); }
static void lpt_write(void *c, uint16_t p, uint8_t b) { (void)c; outb(p,b); }
static void lpt_sleep(void *c) { (void)c; task_sleep_ms(1); }
static int lpt_delay(void *c, unsigned us) { (void)c; return udelay(us); }
static const struct lpt_io lpt_real = {lpt_read,lpt_write,lpt_sleep,lpt_delay,0};
static uint8_t lr(struct lpt_port *p, unsigned r)
{ return p->io.read(p->io.context,(uint16_t)(r<8 ? p->resource.base+r : p->resource.base_hi+r-8)); }
static void lw(struct lpt_port *p, unsigned r, uint8_t v)
{ p->io.write(p->io.context,(uint16_t)(r<8 ? p->resource.base+r : p->resource.base_hi+r-8),v); }
static void lc(struct lpt_port *p, uint8_t v) { p->control=v&0x2f; lw(p,2,p->control); }
static void le(struct lpt_port *p, unsigned mode, bool dma)
{ lw(p,10,(uint8_t)((mode<<5)|0x10|(dma ? 8 : 4))); }
static bool lpt_claims_live(struct lpt_port *p)
{
    for (unsigned i=0;i<4;i++)
        if (p->claims[i]>=0 && !registry_valid(p->claims[i],p->generations[i])) return false;
    return p->claims[0]>=0;
}
static bool lpt_live(struct lpt_port *p, uint32_t g)
{ return g && p->status.generation==g && !p->closing &&
    (p->status.flags&CIUKI_LPT_OPEN) && (p->status.flags&CIUKI_LPT_READY) && lpt_claims_live(p); }
static void lpt_quarantine(struct lpt_port *p, const char *reason)
{
    p->status.flags=(p->status.flags&~CIUKI_LPT_READY)|CIUKI_LPT_QUARANTINED;
    p->reason=reason;
    for (unsigned i=0;i<4;i++) if (p->claims[i]>=0)
        registry_quarantine(p->claims[i],p->generations[i]);
}
static int lpt_claim(struct lpt_port *p, unsigned slot, enum res_type t, uint32_t a, uint32_t b, const char *name)
{
    int h=registry_claim(t,a,b,name,false); if (h<0) return h;
    p->claims[slot]=h; p->generations[slot]=registry_get((unsigned)h)->generation; return 0;
}
static bool lpt_restored(int handle, gen_t generation)
{ return registry_valid(handle,generation); /* probe restored; no engine/request admitted */ }
static void lpt_drop_claim(struct lpt_port *p, unsigned i)
{
    if (p->claims[i]>=0) {
        const struct resource *r=registry_get((unsigned)p->claims[i]);
        if (r->state==RS_CLAIMED) registry_activate(p->claims[i],p->generations[i]);
        if (!registry_quiesce(p->claims[i],p->generations[i],lpt_restored) &&
            !registry_release(p->claims[i],p->generations[i])) p->claims[i]=-1;
        else registry_quarantine(p->claims[i],p->generations[i]);
    }
}
static void lpt_unclaim(struct lpt_port *p)
{ for (unsigned i=4;i-->0;) lpt_drop_claim(p,i); }
static bool lpt_epp_clear(struct lpt_port *p)
{
    uint8_t s=lr(p,1);
    if (!(s&1)) return true;
    (void)lr(p,1); /* clear-on-read clones */
    lw(p,1,s|1); lw(p,1,s&~1u); /* write-one and write-zero variants */
    return !(lr(p,1)&1);
}
static int lpt_probe(struct lpt_port *p)
{
    p->saved_control=lr(p,2); p->saved_data=lr(p,0);
    if (p->saved_control==0xff || (p->saved_control&0x31)) return -EBUSY;
    if (p->resource.base_hi) {
        p->saved_ecr=lr(p,10);
        /* Never reset an inherited FIFO or an enabled DMA request. */
        if (p->saved_ecr==0xff || (p->saved_ecr&0xe8) || (p->saved_ecr&0x15)!=0x15) return -EBUSY;
        le(p,0,false);
    }
    lc(p,p->saved_control&~0x20u);
    lw(p,0,0x55); bool exists=lr(p,0)==0x55;
    lw(p,0,0xaa); exists=exists && lr(p,0)==0xaa;
    unsigned caps=CIUKI_LPT_CAP_SPP;
    if (exists) {
        if (p->resource.base_hi) le(p,1,false);
        lc(p,(p->saved_control&~0x11u)|0x20);
        lw(p,0,0x55); bool reverse=lr(p,0)!=0x55;
        lw(p,0,0xaa); reverse=reverse || lr(p,0)!=0xaa;
        if ((lr(p,2)&0x20) && reverse) caps|=CIUKI_LPT_CAP_PS2;
        lc(p,p->saved_control&~0x31u);
        if (p->resource.length>=5 && (p->resource.capabilities&CIUKI_LPT_CAP_EPP) && lpt_epp_clear(p))
            caps|=CIUKI_LPT_CAP_EPP;
        if (p->resource.base_hi && (caps&CIUKI_LPT_CAP_PS2)) {
            le(p,0,false); le(p,6,false); /* isolated FIFO test, no cable cycles */
            unsigned n=0;
            while (n<32 && !(lr(p,10)&2)) lw(p,8,(uint8_t)n++);
            bool fifo=n && n<32 && (lr(p,10)&2);
            for (unsigned k=0;k<n;k++) if (lr(p,8)!=(uint8_t)k) fifo=false;
            fifo=fifo && (lr(p,10)&1);
            le(p,0,false);
            if (fifo) { caps|=CIUKI_LPT_CAP_ECP; p->status.fifo_depth=n; }
        }
    }
    if (p->resource.base_hi) lw(p,10,p->saved_ecr);
    lw(p,0,p->saved_data); lw(p,2,p->saved_control);
    p->control=p->saved_control;
    p->status.capabilities=exists ? caps : 0;
    return exists ? 0 : -ENODEV;
}
int lpt_admit(const struct lpt_resource *r, const struct lpt_io *io)
{
    if (!r || !io || !io->read || !io->write || !io->sleep || !io->delay ||
        !r->base || r->base==0xffff || r->length<3 || r->length>8 ||
        r->base>0x10000-r->length || (r->base_hi && (r->base_hi>0xfffd ||
        (r->base_hi<r->base+r->length && r->base<r->base_hi+3))) ||
        r->number>CIUKI_LPT_MAX || r->origin<1 || r->origin>3 ||
        (r->dma!=CIUKI_LPT_NO_DMA && r->dma>3)) return -EINVAL;
    unsigned index=r->number ? r->number-1 : CIUKI_LPT_MAX;
    for (unsigned i=0;i<CIUKI_LPT_MAX;i++) if (lpts[i].present) {
        struct lpt_resource *old=&lpts[i].resource;
        if (old->base==r->base) return (int)i; /* retain first assigned map/owner */
        if (r->base<old->base+old->length && old->base<r->base+r->length) return -EBUSY;
    } else if (!r->number && index==CIUKI_LPT_MAX) index=i;
    if (index==CIUKI_LPT_MAX) return -ENOSPC;
    struct lpt_port *p=&lpts[index]; if (p->present) return -EBUSY;
    *p=(struct lpt_port){.resource=*r,.io=*io,.claims={-1,-1,-1,-1},.present=true,
        .reason="Assigned resources; register service unavailable."};
    p->status=(struct ciuki_lpt_status){.version=1,.size=sizeof(p->status),.number=index+1,
        .base=r->base,.base_hi=r->base_hi,.origin=r->origin,.dma_channel=CIUKI_LPT_NO_DMA};
    if (lpt_claim(p,0,RES_PORT,r->base,r->base+r->length,lpt_names[index])) return (int)index;
    if (r->base_hi && lpt_claim(p,1,RES_PORT,r->base_hi,r->base_hi+3,lpt_names[index])) {
        lpt_unclaim(p); p->reason="ECP range conflict; no probe."; return (int)index;
    }
    int e=lpt_probe(p);
    if (e) {
        if (e==-EBUSY) lpt_quarantine(p,"Inherited active state; resources retained.");
        else { lpt_unclaim(p); p->reason="Failed register probe; restored."; }
        return (int)index;
    }
    if (r->dma!=CIUKI_LPT_NO_DMA && r->origin!=CIUKI_LPT_PCI && (r->irq==5 || r->irq==7) &&
        (p->status.capabilities&CIUKI_LPT_CAP_ECP) &&
        !lpt_claim(p,2,RES_DMA,r->dma,r->dma+1,lpt_names[index])) {
        if (!lpt_claim(p,3,RES_IRQ,r->irq,r->irq+1,lpt_names[index])) {
            /* ECR serviceIntr=0 is required for DMA and also enables the TC
             * interrupt. DCR's ACK mask does not mask that source. Only an
             * exclusively claimed ISA route may be PIC-masked for polling. */
            pic_mask(r->irq);
            p->status.capabilities|=CIUKI_LPT_CAP_DMA; p->status.dma_channel=r->dma;
        } else lpt_drop_claim(p,2);
    }
    for (unsigned i=0;i<4;i++) if (p->claims[i]>=0 && registry_activate(p->claims[i],p->generations[i])) {
        lpt_quarantine(p,"Resource activation failed; retained."); return (int)index;
    }
    p->status.flags=CIUKI_LPT_READY; p->reason="Exclusive polled parallel port; IRQ sources masked.";
    return (int)index;
}
void lpt_bda_discover(const uint16_t bases[3])
{
    for (unsigned i=0;i<3;i++) {
        struct lpt_resource r={i+1,bases[i],3,0,0,CIUKI_LPT_NO_DMA,CIUKI_LPT_BDA,CIUKI_LPT_CAP_SPP};
        (void)lpt_admit(&r,&lpt_real);
    }
}
int lpt_pnp_assigned(const char *id, uint32_t base, uint32_t length, uint32_t high, unsigned irq, unsigned dma)
{
    if (!id || (strncmp(id,"PNP0400",8) && strncmp(id,"PNP0401",8))) return -EINVAL;
    bool ecp=!strncmp(id,"PNP0401",8);
    if (!base || base>0x10000u || length<3 || length>8 || base>0x10000-length ||
        (ecp && high && (high>0xfffd || (high<base+length && base<high+3))) ||
        (ecp && dma!=CIUKI_LPT_NO_DMA && dma>3)) return -EINVAL;
    struct lpt_resource r={0,base,length,ecp ? high : 0,irq,ecp ? dma : CIUKI_LPT_NO_DMA,
        CIUKI_LPT_PNP,CIUKI_LPT_CAP_SPP}; /* EPP requires a separate proven layout */
    for (unsigned i=0;i<CIUKI_LPT_MAX;i++) if (lpts[i].present && lpts[i].resource.base==base &&
        lpts[i].resource.origin==CIUKI_LPT_BDA && (lpts[i].status.flags&CIUKI_LPT_READY)) {
        if ((lpts[i].status.flags&CIUKI_LPT_OPEN) || lpts[i].busy || lpts[i].closing) {
            r.number=i+1; lpts[i].pending=r; return (int)i;
        }
        lpt_unclaim(&lpts[i]);
        if (lpts[i].claims[0]>=0) return -EBUSY;
        r.number=i+1; lpts[i].present=false; break;
    }
    return lpt_admit(&r,&lpt_real);
}
void lpt_pci_discover(const struct pci_func *f)
{
    /* Linux v6.12 parport_pc.c: exact NetMos 9805/9815 BAR pairs. */
    if (!f || f->vendor!=0x9710 || (f->device!=0x9805 && f->device!=0x9815) ||
        f->class_code!=7 || !(f->command&1)) return;
    unsigned count=f->device==0x9815 ? 2 : 1;
    for (unsigned i=0;i<count;i++) {
        uint32_t lo=f->bar[2*i], hi=f->bar[2*i+1];
        if (!(lo&1) || !(hi&1) || !(lo&~3u) || !(hi&~3u)) continue;
        struct lpt_resource r={0,lo&~3u,8,hi&~3u,f->irq_line,CIUKI_LPT_NO_DMA,
            CIUKI_LPT_PCI,CIUKI_LPT_CAP_SPP|CIUKI_LPT_CAP_EPP};
        (void)lpt_admit(&r,&lpt_real);
    }
}
static void lpt_firmware(void *unused)
{
    (void)unused;
    struct ciuki_firmware_page *page=kzalloc(sizeof(*page));
    if (!page) { klog("[lpt] firmware discovery unavailable: allocation"); return; }
    for (unsigned wait=0;wait<30000;wait++) {
        *page=(struct ciuki_firmware_page){.version=CIUKI_FIRMWARE_VERSION,.size=sizeof(*page)};
        if (firmware_inventory_page(page)) break;
        if (!(page->flags&CIUKI_FIRMWARE_PENDING)) break;
        task_sleep_ms(1);
    }
    if (!(page->flags&CIUKI_FIRMWARE_PENDING)) do {
        for (unsigned i=0;i<page->count;i++) {
            const struct ciuki_firmware_record *r=&page->records[i];
            const char *id=r->hid;
            for (unsigned c=0;c<r->cid_count && c<4;c++)
                if (!strncmp(r->cid[c],"PNP040",6)) id=r->cid[c];
            if (strncmp(id,"PNP0400",8) && strncmp(id,"PNP0401",8)) continue;
            if (r->flags || r->state==CIUKI_FIRMWARE_UNSUPPORTED) continue;
            uint32_t base=0,len=0,high=0,irq=0,dma=CIUKI_LPT_NO_DMA; bool invalid=false;
            for (unsigned n=0;n<r->resource_count && n<8;n++) {
                const struct ciuki_firmware_resource *e=&r->resources[n];
                if (e->flags || e->end<=e->start || e->end>UINT32_MAX) { invalid=true; break; }
                if (e->type==CIUKI_RESOURCE_IO) {
                    if (!base) { base=e->start; len=e->end-e->start; }
                    else if (!high && e->end-e->start==3) high=e->start;
                    else invalid=true;
                } else if (e->type==CIUKI_RESOURCE_IRQ && e->end==e->start+1) irq=e->start;
                else if (e->type==CIUKI_RESOURCE_DMA && e->end==e->start+1) dma=e->start;
            }
            if (!invalid) (void)lpt_pnp_assigned(id,base,len,high,irq,dma);
        }
        if (!page->next) break;
        page->cursor=page->next;
        if (firmware_inventory_page(page)) break;
    } while (true);
    else klog("[lpt] firmware discovery still pending; no PnP activation");
    kfree(page);
}
void lpt_init(void)
{
    if (lpt_initialized) return;
    lpt_initialized=true;
    uint16_t bases[3]; memcpy(bases,P2V(0x408),sizeof(bases)); lpt_bda_discover(bases);
    for (unsigned i=0;i<pci_count();i++) lpt_pci_discover(pci_get(i));
    if (firmware_inventory_page && !task_create_kernel("lpt-pnp",lpt_firmware,0,P_DEVICE))
        klog("[lpt] firmware discovery worker unavailable");
    struct registry_stats s; registry_snapshot(&s);
    klog("[lpt] metadata_bytes=%u registry_live=%u limit=64 dma_bounce_per_open=4096",
         (unsigned)sizeof(lpts),s.live);
}
bool lpt_snapshot(unsigned i, struct ciuki_lpt_status *out, const char **reason)
{
    if (i>=CIUKI_LPT_MAX || !out || !lpts[i].present) return false;
    struct lpt_port *p=&lpts[i]; *out=p->status;
    if ((out->flags&CIUKI_LPT_READY) && !lpt_claims_live(p))
        out->flags=(out->flags&~CIUKI_LPT_READY)|CIUKI_LPT_QUARANTINED;
    if (reason) *reason=p->reason;
    return true;
}
int lpt_open(unsigned i, uint32_t owner, uint32_t owner_generation, bool raw, uint32_t *g)
{
    if (i>=CIUKI_LPT_MAX || !g || !!owner!=!!owner_generation || (raw && !owner)) return -EINVAL;
    struct lpt_port *p=&lpts[i];
    if (p->pending.base && !(p->status.flags&CIUKI_LPT_OPEN) && !p->busy && !p->closing &&
        (p->status.flags&CIUKI_LPT_READY)) {
        uint32_t flags=irq_save(); irq_restore(flags);
        if (!(flags&0x200)) return -EBUSY;
        struct lpt_resource r=p->pending; struct lpt_io io=p->io;
        lpt_unclaim(p);
        if (p->claims[0]>=0) return -ENODEV;
        p->present=false;
        int e=lpt_admit(&r,&io); if (e<0) return e;
    }
    if (!p->present || !(p->status.flags&CIUKI_LPT_READY) || !lpt_claims_live(p)) return -ENODEV;
    if (p->status.flags&CIUKI_LPT_OPEN || p->busy || p->closing) return -EBUSY;
    if (lpt_generation==UINT32_MAX) return -EOVERFLOW;
    p->saved_data=lr(p,0); p->saved_control=lr(p,2);
    if (p->resource.base_hi) {
        p->saved_ecr=lr(p,10);
        if ((p->saved_ecr&0xe8) || (p->saved_ecr&0x15)!=0x15) return -EBUSY;
        le(p,0,false);
    }
    lc(p,0x0c);
    p->status.generation=++lpt_generation; p->status.owner_id=owner; p->status.owner_generation=owner_generation;
    p->status.flags|=CIUKI_LPT_OPEN|(owner ? CIUKI_LPT_VM : 0)|(raw ? CIUKI_LPT_RAW : 0);
    p->status.transferred=0;
    p->config=(struct ciuki_lpt_config){1,sizeof(p->config),CIUKI_LPT_SPP,0,1000,0,0,0};
    *g=p->status.generation; return 0;
}
struct lpt_wait { struct lpt_port *port; uint32_t generation; uint64_t end; unsigned polls; bool nonblock; bool (*cancelled)(void *); void *cancel_arg; };
static int lpt_check(struct lpt_wait *w)
{
    if (!lpt_live(w->port,w->generation)) return -ENODEV;
    if (w->cancelled && w->cancelled(w->cancel_arg)) return -EINTR;
    if (!w->nonblock && deadline_passed(w->end)) return -ETIMEDOUT;
    return 0;
}
static int lpt_step(struct lpt_wait *w)
{
    int e=lpt_check(w); if (e) return e;
    if (w->nonblock) return -EAGAIN;
    if (deadline_passed(w->end) || ++w->polls>60000) return -ETIMEDOUT;
    w->port->io.sleep(w->port->io.context); return lpt_check(w);
}
static int lpt_wait_status(struct lpt_wait *w, uint8_t mask, uint8_t value)
{
    unsigned spins=0;
    for (;;) {
        int e=lpt_check(w); if (e) return e;
        uint8_t s=lr(w->port,1); w->port->status.status=s;
        if (s==0xff) return -EIO;
        if ((s&mask)==value) return 0;
        /* Catch short ACK edges before yielding; no IRQ-disabled polling.
         * The wall deadline and cancellation still cover the spin window. */
        if (!w->nonblock && !deadline_passed(w->end) && spins++<8) {
            e=w->port->io.delay(w->port->io.context,1); if (e) return e;
            continue;
        }
        e=lpt_step(w); if (e) return e;
        spins=0;
    }
}
static int lpt_terminate(struct lpt_wait *w)
{
    struct lpt_port *p=w->port;
    if (!p->negotiated) return 0;
    int e;
    if (p->config.mode==CIUKI_LPT_ECP && p->config.reverse) {
        lc(p,0x26); /* request forward while data pins are still inputs */
        if ((e=lpt_wait_status(w,0x20,0x20))) {
            lpt_quarantine(p,"ECP forward direction unproved; cable state retained."); return e;
        }
        lc(p,6);
    }
    uint8_t reverse=p->config.reverse && p->config.mode!=CIUKI_LPT_ECP ? 0x20 : 0;
    lc(p,0x0c|reverse);
    e=lpt_wait_status(w,0x40,0);
    if (!e) { lc(p,0x0e|reverse); e=lpt_wait_status(w,0x40,0x40); }
    if (e) { lpt_quarantine(p,"IEEE1284 termination unproved; cable state retained."); return e; }
    lc(p,0x0c); p->negotiated=false; return 0;
}
static int lpt_negotiate(struct lpt_wait *w, unsigned mode, bool reverse)
{
    struct lpt_port *p=w->port;
    int e=lpt_terminate(w); if (e || mode==CIUKI_LPT_SPP || (mode==CIUKI_LPT_PS2 && !reverse)) return e;
    lw(p,0,mode==CIUKI_LPT_ECP ? 0x10 : mode==CIUKI_LPT_EPP ? 0x40 : 1);
    if ((e=p->io.delay(p->io.context,1))) return e;
    lc(p,6);
    if ((e=lpt_wait_status(w,0x78,0x38))) goto failed;
    p->negotiated=true; /* peer responded; failure now requires containment */
    lc(p,7); if ((e=p->io.delay(p->io.context,1))) goto failed;
    lc(p,4);
    if ((e=lpt_wait_status(w,0x40,0x40))) goto failed;
    if (!(lr(p,1)&0x10)) { e=-EOPNOTSUPP; goto failed; }
    p->negotiated=true;
    if (mode==CIUKI_LPT_ECP) {
        lc(p,6); if ((e=lpt_wait_status(w,0x20,0x20))) goto failed;
        if (reverse) {
            lc(p,2); if ((e=lpt_wait_status(w,0x20,0))) goto failed;
            lc(p,0x22);
        } else lc(p,4);
    } else lc(p,(uint8_t)(4|(reverse ? 0x20 : 0)));
    return 0;
failed:
    if (p->negotiated) lpt_quarantine(p,"IEEE1284 negotiation incomplete; cable state retained.");
    else lc(p,0x0c);
    return e;
}
/* 8237 channel 0..3 broker: registry_init owns controller register ranges;
 * clients own channels. Each complete flip-flop transaction is IRQ-excluded.
 * No all-channel mask writes or destructive shared terminal-status reads. */
static void ldma_write(struct lpt_port *p, unsigned port, uint8_t b)
{ p->io.write(p->io.context,(uint16_t)port,b); }
static uint8_t ldma_read(struct lpt_port *p, unsigned port)
{ return p->io.read(p->io.context,(uint16_t)port); }
static unsigned ldma_count(struct lpt_port *p)
{
    unsigned c=p->resource.dma; uint32_t f=irq_save();
    ldma_write(p,0x0c,0); unsigned lo=ldma_read(p,2*c+1), hi=ldma_read(p,2*c+1);
    irq_restore(f); return lo|(hi<<8);
}
static bool lpt_dma_stop(struct lpt_port *p)
{
    if (!p->dma_active) return true;
    if (!lpt_claims_live(p)) return false;
    unsigned c=p->resource.dma;
    uint32_t f=irq_save(); ldma_write(p,0x0a,(uint8_t)(4|c)); le(p,3,false); irq_restore(f);
    unsigned a=ldma_count(p);
    if (p->io.delay(p->io.context,5)) return false;
    unsigned b=ldma_count(p);
    /* 8237 mask registers are write-only. In-order channel mask plus ECR
     * DMA-disable readback and stable count corroborate inhibition. Never
     * read the shared, clear-on-read terminal-count status register. */
    if ((lr(p,10)&8) || a!=b) return false;
    p->dma_active=false; return true;
}
static bool lpt_close_status(struct lpt_port *p, uint8_t mask, uint8_t value)
{
    for (unsigned n=0;n<10;n++) {
        uint8_t s=lr(p,1);
        if (s!=0xff && (s&mask)==value) return true;
        if (p->io.delay(p->io.context,5)) break;
    }
    return false;
}
static void lpt_finish_close(struct lpt_port *p)
{
    if (!lpt_claims_live(p)) { lpt_quarantine(p,"Resource lease revoked; state retained."); goto done; }
    bool idle=lpt_dma_stop(p);
    if (p->resource.base_hi && (p->config.mode==CIUKI_LPT_ECP) && !(lr(p,10)&1)) idle=false;
    uint32_t flags=irq_save(); irq_restore(flags);
    if (p->negotiated && !(flags&0x200)) idle=false;
    if (idle && p->negotiated) {
        /* Non-sleeping termination also runs during killed-task cleanup.
         * Failure retains ownership: a later client must not inherit an
         * uncertain cable protocol. At most 150 us with interrupts enabled. */
        if (p->resource.base_hi) le(p,1,false);
        if (p->config.mode==CIUKI_LPT_ECP && p->config.reverse) {
            lc(p,0x26);
            idle=lpt_close_status(p,0x20,0x20);
            if (idle) lc(p,6);
        }
        uint8_t reverse=p->config.reverse && p->config.mode!=CIUKI_LPT_ECP ? 0x20 : 0;
        if (idle) {
            lc(p,0x0c|reverse);
            idle=lpt_close_status(p,0x40,0);
            if (idle) { lc(p,0x0e|reverse); idle=lpt_close_status(p,0x40,0x40); }
        }
    }
    if (!idle) { lpt_quarantine(p,"DMA/FIFO stop unproved; claims and bounce retained."); goto done; }
    if (p->resource.base_hi) { le(p,0,false); lw(p,10,p->saved_ecr); }
    lw(p,0,p->saved_data); lw(p,2,p->saved_control);
    if (p->bounce) {
        if (mm_unpin && mm_unpin(&p->pin,p->pin.generation)) {
            lpt_quarantine(p,"DMA unpin failed; bounce retained."); goto done;
        }
        mm_free(p->bounce,MM_DMA); p->bounce=0;
    }
done:
    p->status.generation=p->status.owner_id=p->status.owner_generation=0;
    p->status.flags&=~(CIUKI_LPT_OPEN|CIUKI_LPT_VM|CIUKI_LPT_RAW);
    p->negotiated=p->busy=p->closing=false;
}
void lpt_close(unsigned i, uint32_t g)
{
    if (i>=CIUKI_LPT_MAX) return;
    struct lpt_port *p=&lpts[i];
    if (!g || p->status.generation!=g) return;
    p->closing=true; /* a sleeping transfer observes revocation before more I/O */
    if (!p->busy) lpt_finish_close(p);
}
/* A killed native operation owns the only active transfer stack. Called by
 * its pinned operation cleanup; never waits, quarantines a nonempty FIFO. */
void lpt_abort(unsigned i, uint32_t g)
{
    if (i<CIUKI_LPT_MAX && g && lpts[i].status.generation==g) {
        lpts[i].closing=true; lpt_finish_close(&lpts[i]);
    }
}
static int lpt_fifo_idle(struct lpt_wait *w)
{
    while (!(lr(w->port,10)&1)) { int e=lpt_step(w); if (e) return e; }
    return 0;
}
static int lpt_dma_transfer(struct lpt_wait *w, const void *data, unsigned n)
{
    struct lpt_port *p=w->port;
    if (!p->bounce) {
        uint32_t phys=pmm_alloc_dma_run(1,1);
        if (!phys) return -ENOMEM;
        if (phys>=0x1000000 || (phys&4095)) { pmm_free(phys); return -EIO; }
        p->bounce=phys;
        mm_claim(phys,1,MM_DMA);
        p->pin=(struct mm_pin){.generation=p->status.generation,.first_pfn=phys/PAGE_SIZE,.pages=1,
            .address_mask=0x00ffffff,.alignment=PAGE_SIZE,.boundary=65536,.direction=MM_PIN_TO_DEVICE};
        int e=mm_pin ? mm_pin(&p->pin) : 0;
        if (e) { mm_free(phys,MM_DMA); p->bounce=0; return e; }
    }
    memcpy(P2V(p->bounce),data,n);
    static const uint8_t pages[]={0x87,0x83,0x81,0x82};
    unsigned c=p->resource.dma, count=n-1; uint32_t f=irq_save();
    ldma_write(p,0x0a,(uint8_t)(4|c)); ldma_write(p,0x0c,0);
    ldma_write(p,0x0b,(uint8_t)(0x48|c)); /* single, memory->I/O, no autoinit */
    ldma_write(p,2*c,(uint8_t)p->bounce); ldma_write(p,2*c,(uint8_t)(p->bounce>>8));
    ldma_write(p,pages[c],(uint8_t)(p->bounce>>16));
    ldma_write(p,2*c+1,(uint8_t)count); ldma_write(p,2*c+1,(uint8_t)(count>>8));
    p->dma_active=true; le(p,3,true); ldma_write(p,0x0a,(uint8_t)c); irq_restore(f);
    int e=0;
    while (ldma_count(p)!=0xffff) { e=lpt_step(w); if (e) break; }
    if (!lpt_dma_stop(p)) {
        lpt_quarantine(p,"DMA stop unproved; bounce retained."); return -EIO;
    }
    if (!e) e=lpt_fifo_idle(w);
    return e ? e : (int)n;
}
int lpt_transfer(unsigned i, uint32_t g, void *data, unsigned bytes, bool write, bool nonblock, bool (*cancelled)(void *), void *cancel_arg)
{
    if (i>=CIUKI_LPT_MAX || bytes>CIUKI_LPT_CHUNK || (!data && bytes)) return -EINVAL;
    struct lpt_port *p=&lpts[i];
    if (!lpt_live(p,g)) return -ENODEV;
    if (p->status.flags&CIUKI_LPT_RAW || p->busy) return -EBUSY;
    if (!bytes) return 0;
    struct ciuki_lpt_config c=p->config;
    if (write==!!c.reverse) return -EACCES;
    struct lpt_wait w={p,g,deadline_after_ms(c.timeout_ms),0,nonblock || !c.timeout_ms,cancelled,cancel_arg};
    int e=lpt_check(&w); if (e) return e;
    p->busy=true;
    unsigned n=0; uint8_t *b=data;
    if (c.dma) {
        e=w.nonblock ? -EAGAIN : lpt_dma_transfer(&w,data,bytes);
        if (e>0) { n=(unsigned)e; e=0; }
    } else for (;n<bytes;n++) {
        if ((e=lpt_check(&w))) break;
        if (c.mode==CIUKI_LPT_EPP) {
            if (!lpt_epp_clear(p)) { e=-EIO; break; }
            if (write) lw(p,c.address ? 3 : 4,b[n]); else b[n]=lr(p,c.address ? 3 : 4);
            if (lr(p,1)&1) { (void)lpt_epp_clear(p); e=-ETIMEDOUT; break; }
        } else if (c.mode==CIUKI_LPT_ECP) {
            while (lr(p,10)&(write ? 2 : 1)) { e=lpt_step(&w); if (e) break; }
            if (e) break;
            if (write) lw(p,8,b[n]); else b[n]=lr(p,8);
        } else if (write) {
            if ((e=lpt_wait_status(&w,0xf8,0xd8))) break; /* ready, selected, no paper/error */
            lw(p,0,b[n]);
            if ((e=p->io.delay(p->io.context,1))) break;
            lc(p,0x0d);
            e=p->io.delay(p->io.context,5); lc(p,0x0c); if (e) break;
            if ((e=lpt_wait_status(&w,0x40,0))) break;
            if ((e=lpt_wait_status(&w,0x40,0x40))) break;
        } else { /* negotiated IEEE1284 byte reverse: HostBusy/PtrClk */
            lc(p,0x26); if ((e=lpt_wait_status(&w,0x40,0))) break;
            b[n]=lr(p,0); lc(p,0x24);
            if ((e=lpt_wait_status(&w,0x40,0x40))) break;
        }
    }
    if (!e && c.mode==CIUKI_LPT_ECP && write) e=lpt_fifo_idle(&w);
    if (c.mode==CIUKI_LPT_ECP && e && lpt_claims_live(p) && !(lr(p,10)&1))
        { lpt_quarantine(p,"FIFO completion unproved; port retained."); n=0; }
    p->status.transferred+=n;
    p->busy=false;
    if (p->closing) lpt_finish_close(p);
    return n ? (int)n : e;
}
int lpt_control(unsigned i, uint32_t g, uint32_t op, void *record, unsigned bytes, bool writable)
{
    unsigned size=op==LPT_GET_CONFIG || op==LPT_SET_CONFIG ? sizeof(struct ciuki_lpt_config) :
        op==LPT_GET_STATUS ? sizeof(struct ciuki_lpt_status) : op==LPT_INIT ? sizeof(struct ciuki_lpt_init) : 0;
    if (!size) return -ENOTTY;
    if (i>=CIUKI_LPT_MAX || !record || bytes!=size) return -EINVAL;
    uint32_t *h=record;
    if (h[0]!=1 || h[1]!=size || h[size/4-1]) return -EINVAL;
    struct lpt_port *p=&lpts[i];
    if (!lpt_live(p,g)) return -ENODEV;
    if (op==LPT_GET_STATUS) { p->status.status=lr(p,1); *(struct ciuki_lpt_status *)record=p->status; return 0; }
    if (op==LPT_GET_CONFIG) { *(struct ciuki_lpt_config *)record=p->config; return 0; }
    if (!writable) return -EACCES;
    if (p->busy || (p->status.flags&CIUKI_LPT_RAW)) return -EBUSY;
    struct ciuki_lpt_config c=p->config;
    if (op==LPT_SET_CONFIG) {
        c=*(struct ciuki_lpt_config *)record;
        if (c.mode>3 || c.reverse>1 || c.dma>1 || c.address>1 || c.timeout_ms>60000 ||
            !(p->status.capabilities&(1u<<c.mode)) ||
            (c.reverse && c.mode==CIUKI_LPT_SPP) || (c.address && c.mode!=CIUKI_LPT_EPP) ||
            (c.dma && (c.mode!=CIUKI_LPT_ECP || c.reverse || !(p->status.capabilities&CIUKI_LPT_CAP_DMA))))
            return -EINVAL;
    }
    p->busy=true;
    struct lpt_wait w={p,g,deadline_after_ms(c.timeout_ms),0,!c.timeout_ms,0,0};
    int e=0;
    if (p->config.mode==CIUKI_LPT_ECP && (e=lpt_fifo_idle(&w))) goto done;
    if (p->resource.base_hi) le(p,1,false);
    if (op==LPT_INIT) {
        e=lpt_terminate(&w);
        if (!e) { lc(p,8); e=p->io.delay(p->io.context,5); lc(p,0x0c); }
        c=(struct ciuki_lpt_config){1,sizeof(c),0,0,p->config.timeout_ms,0,0,0};
    } else e=lpt_negotiate(&w,c.mode,c.reverse);
    if (!e) {
        if (p->resource.base_hi) le(p,c.mode==CIUKI_LPT_ECP ? 3 : c.mode==CIUKI_LPT_EPP ? 4 : c.mode==CIUKI_LPT_PS2 ? 1 : 0,false);
        p->config=c;
    } else if (lpt_claims_live(p)) {
        if (p->resource.base_hi) le(p,0,false);
        p->config=(struct ciuki_lpt_config){1,sizeof(c),0,0,c.timeout_ms,0,0,0};
    }
done:
    p->busy=false;
    if (p->closing) lpt_finish_close(p);
    return e;
}
int lpt_raw_io(unsigned i, uint32_t g, unsigned r, bool write, uint8_t *b)
{
    if (i>=CIUKI_LPT_MAX || !b) return -EINVAL;
    struct lpt_port *p=&lpts[i];
    if (!lpt_live(p,g)) return -ENODEV;
    if (!(p->status.flags&CIUKI_LPT_VM) || p->busy) return -EACCES;
    if (!(p->status.flags&CIUKI_LPT_RAW) && r>2) return -EACCES;
    if (r>=p->resource.length || r>4) return -EOPNOTSUPP;
    if (r>=3 && !(p->status.capabilities&CIUKI_LPT_CAP_EPP)) return -EOPNOTSUPP;
    if (r>=3 && p->resource.base_hi) le(p,4,false);
    if (write) {
        if (r==2) {
            if ((*b&0x20) && !(p->status.capabilities&CIUKI_LPT_CAP_PS2)) return -EOPNOTSUPP;
            if (p->resource.base_hi) le(p,1,false);
            lc(p,*b); /* never allow the physical IRQ-enable bit */
        } else if (r!=1) lw(p,r,*b);
        else if (*b&1) (void)lpt_epp_clear(p);
    } else *b=lr(p,r);
    return 0;
}
