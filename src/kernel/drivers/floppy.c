/* Intel 82077AA 290166-007, sections 2/4/5/7; independent implementation.
 * Source comparison: Linux v6.12 drivers/block/floppy.c and SeaBIOS
 * rel-1.16.3 src/hw/floppy.c. No donor implementation copied.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "floppy.h"
#define FDC_TIMEOUT 8000u
#define FDC_COMMAND 2000u

int fdc_discover(struct fdc_evidence *e, uint16_t equipment, uint8_t cmos)
{
    if (!e) return -FS_EINVAL;
    *e=(struct fdc_evidence){.equipment=equipment,.cmos=cmos,.base=0x3f0,.irq=6,.dma=2};
    /* BDA and CMOS must agree that at least one drive exists. */
    if (!(equipment&1) || (!(cmos>>4) && !(cmos&15))) return -FS_ENOENT;
    if ((cmos>>4)>5 || (cmos&15)>5) return -FS_EOPNOTSUPP;
    e->routed=true; return 0;
}
int fdc_pnp(struct fdc_evidence *e, const char *id, uint64_t base,
            uint64_t length, unsigned irq, unsigned dma)
{
    if (!e || !id || strncmp(id,"PNP0700",8)) return -FS_EINVAL;
    e->pnp=true;
    /* Secondary controllers need separate routing evidence; never truncate. */
    if (base!=0x3f0 || length<6 || irq!=6 || dma!=2) { e->routed=false; return -FS_EOPNOTSUPP; }
    e->base=(uint16_t)base; e->irq=irq; e->dma=dma; e->routed=true; return 0;
}
static uint8_t rd(struct fdc *c, unsigned r) { return c->io.read(c->io.ctx,c->evidence.base+r); }
static void wr(struct fdc *c, unsigned r, uint8_t b) { c->io.write(c->io.ctx,c->evidence.base+r,b); }
static bool expired(struct fdc *c, uint64_t end)
{
    return c->io.now(c->io.ctx)>=end || c->io.now(c->io.ctx)>=c->deadline ||
        (c->io.cancelled && c->io.cancelled(c->io.ctx));
}
static int gate(struct fdc *c)
{
    uint64_t end=c->io.now(c->io.ctx)+FDC_TIMEOUT;
    for (unsigned n=0;n<FDC_TIMEOUT;n++) {
        bool expected=false;
        if (__atomic_compare_exchange_n(&c->busy,&expected,true,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {
            c->deadline=c->io.now(c->io.ctx)+FDC_TIMEOUT; c->touched=false; return 0;
        }
        if (c->io.now(c->io.ctx)>=end || (c->io.cancelled && c->io.cancelled(c->io.ctx))) break;
        c->io.pause(c->io.ctx,1);
    }
    return -FS_EBUSY;
}
static void ungate(struct fdc *c) { __atomic_store_n(&c->busy,false,__ATOMIC_RELEASE); }
static int byte(struct fdc *c, uint8_t *b, bool input)
{
    uint64_t end=c->io.now(c->io.ctx)+100;
    for (unsigned n=0;n<10000;n++) {
        uint8_t s=rd(c,4);
        if ((s&0xc0)==(input ? 0xc0 : 0x80)) {
            if (input) *b=rd(c,5); else wr(c,5,*b);
            return 0;
        }
        if ((s&0x80) && !!(s&0x40)!=input) return -FS_EIO;
        if (expired(c,end)) break;
        c->io.pause(c->io.ctx,1);
    }
    return -FS_EIO;
}
static int send(struct fdc *c, const uint8_t *cmd, unsigned n)
{
    for (unsigned i=0;i<n;i++) { uint8_t b=cmd[i]; int e=byte(c,&b,false); if (e) return e; }
    return 0;
}
static int receive(struct fdc *c, uint8_t *b, unsigned n)
{
    for (unsigned i=0;i<n;i++) { int e=byte(c,b+i,true); if (e) return e; }
    uint8_t msr=rd(c,4);
    return (msr&0xd0)==0x80 ? 0 : -FS_EIO;
}
static int wait_irq(struct fdc *c, uint32_t before)
{
    uint64_t end=c->io.now(c->io.ctx)+FDC_COMMAND;
    for (unsigned n=0;n<FDC_COMMAND;n++) {
        if (__atomic_load_n(&c->interrupts,__ATOMIC_ACQUIRE)!=before) return 0;
        if (expired(c,end)) break;
        c->io.pause(c->io.ctx,1);
    }
    return -FS_EIO;
}
void fdc_irq(struct fdc *c) { if (c) __atomic_add_fetch(&c->interrupts,1,__ATOMIC_RELEASE); }
static bool mask_dma(struct fdc *c)
{
    uint32_t f=c->io.exclude(c->io.ctx);
    c->io.write(c->io.ctx,0x0a,6);
    bool stopped=(c->io.read(c->io.ctx,0x0f)&4)!=0;
    c->io.restore(c->io.ctx,f); c->dma_armed=false;
    return stopped;
}
static bool stop(struct fdc *c)
{
    c->dor=0; c->motor_until=0; wr(c,2,0);
    bool stopped=mask_dma(c) && !(rd(c,2)&0x0c);
    for (unsigned i=0;i<FDC_DRIVES;i++) c->drive[i].calibrated=false;
    return stopped;
}
static void quarantine(struct fdc *c)
{
    if (c->irq_attached) { c->io.irq(c->io.ctx,false); c->irq_attached=false; }
    c->ready=false; c->quarantined=true; c->reason="fdc_stop_unproved";
    for (unsigned i=0;i<FDC_DRIVES;i++) c->drive[i].block.quarantined=true;
    for (unsigned i=0;i<c->claim_count;i++) {
        const struct resource *r=registry_get(c->claims[i]);
        if (r && r->state!=RS_RELEASED) (void)registry_quarantine(c->claims[i],c->generations[i]);
    }
}
/* Only called after stop + IRQ detach, or before the first hardware access.
 * No queued producer exists: all physical work is synchronous under gate. */
static bool stopped_claim(int handle, gen_t generation)
{ (void)handle; (void)generation; return true; }
static int release_claims(struct fdc *c)
{
    for (unsigned i=0;i<c->claim_count;i++) {
        int e=registry_quiesce(c->claims[i],c->generations[i],stopped_claim);
        if (!e) e=registry_release(c->claims[i],c->generations[i]);
        if (e) { quarantine(c); return -FS_EQUARANTINED; }
    }
    c->claim_count=0; return 0;
}
static int reset(struct fdc *c)
{
    if (!stop(c)) { quarantine(c); return -FS_EQUARANTINED; }
    c->io.pause(c->io.ctx,1); /* exceeds the 4 us DOR reset minimum */
    uint32_t before=c->interrupts;
    c->dor=0x0c; wr(c,2,c->dor);
    int e=wait_irq(c,before);
    for (unsigned i=0;!e && i<4;i++) {
        uint8_t cmd=8, result[2];
        e=send(c,&cmd,1); if (!e) e=receive(c,result,2);
        if (!e && (result[0]&0xc0)!=0xc0) e=-FS_EIO;
    }
    return e;
}
static int rate(struct fdc *c, struct fdc_drive *d)
{
    wr(c,7,d->geometry.rate); c->io.pause(c->io.ctx,20);
    uint8_t specify[]={3,0xaf,2}; /* slow mechanics; DMA, HLT >= 4 ms */
    int e=send(c,specify,sizeof(specify));
    if (!e && d->type==5) {
        uint8_t perp[]={0x12,d->geometry.perpendicular ? 3 : 0};
        e=send(c,perp,sizeof(perp));
    }
    return e;
}
static int motor(struct fdc *c, unsigned drive)
{
    if (expired(c,c->deadline)) return -FS_EIO;
    bool running=(c->dor&(0x10u<<drive))!=0;
    c->dor=0x0c | drive | (0x10u<<drive); wr(c,2,c->dor);
    if (!running) {
        uint64_t now=c->io.now(c->io.ctx);
        if (now>=c->deadline) return -FS_EIO;
        uint64_t left=c->deadline-now;
        c->io.pause(c->io.ctx,left<1000 ? (unsigned)left : 1000);
    }
    return expired(c,c->deadline) ? -FS_EIO : 0;
}
static int seek(struct fdc *c, unsigned drive, unsigned cylinder, bool recal)
{
    uint8_t cmd[]={recal ? 7 : 15,(uint8_t)drive,(uint8_t)cylinder};
    uint32_t before=c->interrupts;
    int e=send(c,cmd,recal ? 2 : 3);
    if (!e) e=wait_irq(c,before);
    uint8_t sense=8, result[2];
    if (!e) e=send(c,&sense,1);
    if (!e) e=receive(c,result,2);
    if (!e && ((result[0]&0xf3)!=(0x20|drive) || result[1]!=cylinder)) e=-FS_EIO;
    if (!e) {
        c->drive[drive].cylinder=cylinder; c->drive[drive].calibrated=true;
        c->io.pause(c->io.ctx,20);
    }
    return e;
}
static void changed(struct fdc_drive *d)
{
    if (!d->change_high) {
        if (d->block.media_generation==UINT32_MAX) {
            (void)stop(d->controller); quarantine(d->controller);
            d->controller->reason="fdc_media_generation_exhausted"; return;
        }
        d->block.media_generation++;
    }
    d->changed=d->change_high=true; d->established=d->calibrated=false;
}
static int media(struct fdc *c, unsigned unit)
{
    struct fdc_drive *d=&c->drive[unit];
    if (rd(c,7)&0x80) changed(d); else d->change_high=false;
    if (c->quarantined) return -FS_EQUARANTINED;
    return d->changed || !d->established ? -FS_EAGAIN : 0;
}
static int dma(struct fdc *c, unsigned n, bool write)
{
    if (expired(c,c->deadline)) return -FS_EIO;
    if (!n || n>4096 || c->physical>0xffffff || n-1>0xffffff-c->physical ||
        (c->physical&0xffff)+n>65536) return -FS_EINVAL;
    uint32_t f=c->io.exclude(c->io.ctx);
    c->io.write(c->io.ctx,0x0a,6);
    c->io.write(c->io.ctx,0x0c,0);
    c->io.write(c->io.ctx,0x0b,write ? 0x4a : 0x46);
    c->io.write(c->io.ctx,4,c->physical);
    c->io.write(c->io.ctx,4,c->physical>>8);
    c->io.write(c->io.ctx,0x81,c->physical>>16);
    c->io.write(c->io.ctx,5,(n-1)&255);
    c->io.write(c->io.ctx,5,(n-1)>>8);
    c->dma_armed=true;
    c->io.write(c->io.ctx,0x0a,2);
    c->io.restore(c->io.ctx,f); return 0;
}
static bool complete_dma(struct fdc *c)
{
    if (!mask_dma(c)) { quarantine(c); return false; }
    uint32_t f=c->io.exclude(c->io.ctx);
    c->io.write(c->io.ctx,0x0c,0);
    uint16_t count=c->io.read(c->io.ctx,5);
    count|=(uint16_t)c->io.read(c->io.ctx,5)<<8;
    c->io.restore(c->io.ctx,f);
    return count==0xffff;
}
static int status(struct fdc *c)
{
    if (c->result[1]&2) return -FS_EROFS;
    return ((c->result[0]&0xc0) || c->result[1] || c->result[2]) ? -FS_EIO : 0;
}
static bool valid(struct fdc_lease *l)
{
    if (!l || !l->controller || l->drive>=FDC_DRIVES || !l->serial) return false;
    struct fdc_drive *d=&l->controller->drive[l->drive];
    return d->owner==l->owner && d->owner_generation==l->owner_generation && d->serial==l->serial;
}
static int admitted(struct fdc_lease *l)
{
    if (!valid(l)) return -FS_EACCES;
    struct fdc *c=l->controller;
    if (c->quarantined) return -FS_EQUARANTINED;
    if (!c->ready) return -FS_EIO;
    if (c->io.cancelled && c->io.cancelled(c->io.ctx)) return -VFS_ECANCELLED;
    for (unsigned i=0;i<c->claim_count;i++) if (!registry_valid(c->claims[i],c->generations[i])) return -FS_EACCES;
    return 0;
}
static int finish(struct fdc *c, int e)
{
    /* Successful commands are fully drained and DMA-masked. Retain motor
     * speed/head position for nearby reads; the idle worker turns it off.
     * Errors use the stronger reset/gate proof before any client returns. */
    if (c->touched && !c->quarantined) {
        if (!e && (!mask_dma(c) || (rd(c,4)&0xd0)!=0x80)) e=-FS_EIO;
        if (e) {
            if (!stop(c)) { quarantine(c); e=-FS_EQUARANTINED; }
        } else c->motor_until=c->io.now(c->io.ctx)+2000;
    }
    ungate(c); return e;
}
void fdc_idle(struct fdc *c)
{
    if (!c || !c->ready || c->quarantined || !(c->dor&0xf0) || c->io.now(c->io.ctx)<c->motor_until) return;
    if (gate(c)) return;
    if (!c->quarantined && c->io.now(c->io.ctx)>=c->motor_until) {
        if (!mask_dma(c) || (rd(c,4)&0xd0)!=0x80) {
            if (!stop(c)) quarantine(c);
        } else { c->dor&=0x0f; wr(c,2,c->dor); c->motor_until=0; }
    }
    ungate(c);
}
static int start(struct fdc_lease *l)
{
    struct fdc *c=l->controller;
    int e=admitted(l);
    if (!e) {
        c->touched=true;
        if (!(c->dor&4) || (rd(c,4)&0xd0)!=0x80) e=reset(c);
    }
    if (!e) e=rate(c,&c->drive[l->drive]);
    if (!e) e=motor(c,l->drive);
    return e;
}
int fdc_check(struct fdc_lease *l)
{
    if (!l || !l->controller) return -FS_EINVAL;
    struct fdc *c=l->controller;
    int e=gate(c); if (e) return e;
    e=admitted(l);
    if (!e) {
        /* DIR is a drive-select signal, not a read command. No spin-up or
         * physical reset is needed to sample it. Preserve a peer's motor. */
        uint8_t old=c->dor;
        wr(c,2,(old&~3u)|l->drive);
        e=media(c,l->drive);
        if (!c->quarantined) wr(c,2,old);
    }
    ungate(c); return e;
}
int fdc_revalidate(struct fdc_lease *l, unsigned type)
{
    if (!l || !l->controller || l->drive>=FDC_DRIVES) return -FS_EINVAL;
    struct fdc_geometry g;
    if (!fdc_geometry(type,&g)) return -FS_EINVAL;
    struct fdc *c=l->controller;
    int e=gate(c); if (e) return e;
    e=admitted(l);
    struct fdc_drive *d=&c->drive[l->drive];
    if (!e && type!=d->type && !(type==3 && (d->type==4 || d->type==5)) && !(type==4 && d->type==5)) e=-FS_EOPNOTSUPP;
    if (!e) {
        if (d->established && d->geometry.type!=type) {
            if (d->block.media_generation==UINT32_MAX) {
                quarantine(c); c->reason="fdc_media_generation_exhausted"; e=-FS_EQUARANTINED;
            } else d->block.media_generation++;
        }
        d->established=false; d->changed=true; d->geometry=g;
        if (!e) e=start(l);
    }
    if (!e) {
        if (rd(c,7)&0x80) changed(d);
        e=seek(c,l->drive,0,true);
        if (!e) e=seek(c,l->drive,1,false); /* actual step clears DSKCHG */
        if (!e) e=seek(c,l->drive,0,false);
        if (!e && (rd(c,7)&0x80)) e=-FS_EIO;
        /* READ ID establishes the selected media rate without writing. */
        uint8_t id[]={0x4a,l->drive}; uint32_t before=c->interrupts;
        if (!e) e=send(c,id,sizeof(id));
        if (!e) e=wait_irq(c,before);
        if (!e) e=receive(c,c->result,7);
        if (!e) e=status(c);
        if (!e && c->result[6]!=2) e=-FS_EOPNOTSUPP;
    }
    if (!e) {
        d->changed=d->change_high=false; d->established=true;
        d->block.capacity=(uint32_t)g.cylinders*g.heads*g.sectors;
    }
    return finish(c,e);
}
static int command(struct fdc *c, const uint8_t *cmd, unsigned n, unsigned bytes, bool write)
{
    int e=dma(c,bytes,write);
    uint32_t before=c->interrupts;
    if (!e) e=send(c,cmd,n);
    if (!e) e=wait_irq(c,before);
    if (!e) e=receive(c,c->result,7);
    bool tc=complete_dma(c);
    if (!e) e=status(c);
    if (!e && !tc) e=-FS_EIO;
    return c->quarantined ? -FS_EQUARANTINED : e;
}
int fdc_transfer(struct fdc_lease *l, uint64_t lba, unsigned count, void *buffer, bool write)
{
    if (!l || !l->controller || l->drive>=FDC_DRIVES || !buffer || count>128) return -FS_EINVAL;
    struct fdc *c=l->controller;
    int e=gate(c); if (e) return e;
    struct fdc_drive *d=&c->drive[l->drive];
    e=admitted(l);
    if (!e) e=blkdev_range(&d->block,lba,count);
    memset(c->result,0,sizeof(c->result));
    if (!e) e=start(l);
    if (!e) e=media(c,l->drive);
    uint32_t generation=d->block.media_generation;
    for (unsigned n=0;!e && n<count;n++) {
        uint32_t a=(uint32_t)lba+n;
        unsigned sector=a%d->geometry.sectors+1, head=a/d->geometry.sectors%d->geometry.heads;
        unsigned cyl=a/(d->geometry.sectors*d->geometry.heads);
        for (unsigned attempt=0;attempt<FDC_ATTEMPTS;attempt++) {
            if (attempt) {
                c->retries++;
                e=reset(c); if (!e) e=rate(c,d); if (!e) e=motor(c,l->drive);
            }
            if (!e) e=media(c,l->drive);
            if (!e && !d->calibrated) e=seek(c,l->drive,0,true);
            if (!e && d->cylinder!=cyl) e=seek(c,l->drive,cyl,false);
            if (!e && write) memcpy(c->bounce,(uint8_t *)buffer+n*512,512);
            uint8_t cmd[]={write ? 0x45 : 0x46,(head<<2)|l->drive,cyl,head,sector,2,sector,d->geometry.gap,0xff};
            if (!e) e=command(c,cmd,sizeof(cmd),512,write);
            if (!e) e=media(c,l->drive);
            if (!e && d->block.media_generation!=generation) e=-FS_EAGAIN;
            if (!e && !write) memcpy((uint8_t *)buffer+n*512,c->bounce,512);
            if (!e || e==-FS_EROFS || e==-FS_EAGAIN || c->quarantined || expired(c,c->deadline)) break;
            if (attempt+1<FDC_ATTEMPTS) e=0;
        }
    }
    return finish(c,e);
}
int fdc_format(struct fdc_lease *l, unsigned cyl, unsigned head, uint8_t fill)
{
    if (!l || !l->controller || l->drive>=FDC_DRIVES) return -FS_EINVAL;
    struct fdc *c=l->controller; struct fdc_drive *d=&c->drive[l->drive];
    int e=gate(c); if (e) return e;
    e=admitted(l);
    if (!e && (cyl>=d->geometry.cylinders || head>=d->geometry.heads)) e=-FS_EINVAL;
    if (!e) e=start(l);
    if (!e) {
        e=media(c,l->drive);
        if (e==-FS_EAGAIN) {
            /* Formatting establishes IDs; a blank disk cannot pass READ ID.
             * A real step must acknowledge DIR first, and no-medium keeps
             * DIR asserted. Never bypass mechanical positioning or WP. */
            e=seek(c,l->drive,0,true);
            if (!e) e=seek(c,l->drive,1,false);
            if (!e) e=seek(c,l->drive,0,false);
            if (!e && (rd(c,7)&0x80)) e=-FS_EIO;
            if (!e) {
                d->changed=d->change_high=false; d->established=true;
                d->block.capacity=(uint32_t)d->geometry.cylinders*d->geometry.heads*d->geometry.sectors;
            }
        }
    }
    for (unsigned a=0;!e && a<FDC_ATTEMPTS;a++) {
        if (!d->calibrated) e=seek(c,l->drive,0,true);
        if (!e && d->cylinder!=cyl) e=seek(c,l->drive,cyl,false);
        for (unsigned s=0;s<d->geometry.sectors;s++) {
            c->bounce[s*4]=cyl; c->bounce[s*4+1]=head; c->bounce[s*4+2]=s+1; c->bounce[s*4+3]=2;
        }
        uint8_t cmd[]={0x4d,(head<<2)|l->drive,2,d->geometry.sectors,d->geometry.format_gap,fill};
        if (!e) e=command(c,cmd,sizeof(cmd),d->geometry.sectors*4,true);
        if (!e) e=media(c,l->drive);
        if (!e || e==-FS_EROFS || e==-FS_EAGAIN || c->quarantined || a+1==FDC_ATTEMPTS || expired(c,c->deadline)) break;
        c->retries++; e=reset(c); if (!e) e=rate(c,d); if (!e) e=motor(c,l->drive);
    }
    return finish(c,e);
}
int fdc_acquire(struct fdc *c, unsigned drive, uint32_t owner, uint32_t generation, struct fdc_lease *out)
{
    if (!c || drive>=FDC_DRIVES || !out || !owner || !generation) return -FS_EINVAL;
    int e=gate(c); if (e) return e;
    struct fdc_drive *d=&c->drive[drive];
    if (c->quarantined) e=-FS_EQUARANTINED;
    else if (!c->ready || !d->present) e=-FS_ENOENT;
    else if (d->owner) e=-FS_EBUSY;
    else if (c->serial==UINT32_MAX) e=-FS_EQUARANTINED;
    else {
        d->owner=owner; d->owner_generation=generation; d->serial=++c->serial;
        *out=(struct fdc_lease){c,owner,generation,d->serial,drive};
    }
    ungate(c); return e;
}
int fdc_release(struct fdc_lease *l)
{
    if (!l || !l->controller) return -FS_EINVAL;
    struct fdc *c=l->controller;
    /* Cleanup must complete even if the caller has been cancelled. */
    int e=gate(c); if (e) return e;
    if (!valid(l)) e=-FS_EACCES;
    else {
        struct fdc_drive *d=&c->drive[l->drive];
        d->owner=d->owner_generation=d->serial=0; memset(l,0,sizeof(*l));
    }
    ungate(c); return e;
}
static int native_read(struct blkdev *b, uint64_t lba, uint32_t n, void *p)
{ struct fdc_drive *d=b->ctx; return fdc_transfer(&d->native,lba,n,p,false); }
static int native_write(struct blkdev *b, uint64_t lba, uint32_t n, const void *p)
{ struct fdc_drive *d=b->ctx; return fdc_transfer(&d->native,lba,n,(void *)p,true); }
static int native_check(struct blkdev *b)
{ struct fdc_drive *d=b->ctx; return fdc_check(&d->native); }
static int native_flush(struct blkdev *b)
{ return native_check(b); /* no volatile drive cache; writes complete on media */ }
int fdc_native_open(struct fdc *c, unsigned drive, struct blkdev **b)
{
    if (!c || !b || drive>=FDC_DRIVES) return -FS_EINVAL;
    struct fdc_drive *d=&c->drive[drive];
    struct fdc_lease lease;
    int e=fdc_acquire(c,drive,FDC_NATIVE,1,&lease); if (e) return e;
    d->native=lease;
    e=fdc_revalidate(&d->native,d->type);
    if (e && !c->quarantined && d->type==5) e=fdc_revalidate(&d->native,4);
    if (e && !c->quarantined && d->type>=4) e=fdc_revalidate(&d->native,3);
    if (e) { (void)fdc_release(&d->native); return e; }
    *b=&d->block; return 0;
}
int fdc_native_close(struct fdc *c, unsigned drive)
{ return !c || drive>=FDC_DRIVES ? -FS_EINVAL : fdc_release(&c->drive[drive].native); }
int fdc_attach(struct fdc *c, const struct fdc_io *io, const struct fdc_evidence *ev,
               void *bounce, uint32_t physical, const struct mm_pin *pin)
{
    if (!c || !io || !ev || !bounce || !pin || !io->read || !io->write || !io->now ||
        !io->pause || !io->exclude || !io->restore || !io->irq || !ev->routed ||
        ev->base!=0x3f0 || ev->irq!=6 || ev->dma!=2 || physical&4095 || physical>0xfff000 ||
        pin->pages!=1 || pin->first_pfn!=physical/4096 || !pin->ticket ||
        pin->address_mask>0xffffff || pin->boundary!=65536 || pin->direction!=MM_PIN_BIDIRECTIONAL)
        return -FS_EINVAL;
    memset(c,0,sizeof(*c)); c->io=*io; c->evidence=*ev;
    c->bounce=bounce; c->physical=physical; c->pin=pin; c->reason="fdc_attach";
    const struct { enum res_type type; unsigned a,b; } resources[]={
        {RES_PORT,0x3f2,0x3f3},{RES_PORT,0x3f4,0x3f6},{RES_PORT,0x3f7,0x3f8},
        {RES_IRQ,6,7},{RES_DMA,2,3},{RES_MMIO,physical,physical+4096}
    };
    int e=0;
    for (unsigned i=0;i<6;i++) {
        int h=registry_claim(resources[i].type,resources[i].a,resources[i].b,"fdc",false);
        if (h<0) { e=-FS_EBUSY; break; }
        c->claims[i]=h; c->generations[i]=registry_get(h)->generation; c->claim_count++;
        e=registry_activate(h,c->generations[i]); if (e) break;
    }
    if (e) {
        if (release_claims(c)) return -FS_EQUARANTINED;
        c->reason="fdc_resource_conflict"; return e;
    }
    for (unsigned i=0;i<FDC_DRIVES;i++) {
        struct fdc_drive *d=&c->drive[i]; d->controller=c;
        d->type=i ? ev->cmos&15 : ev->cmos>>4;
        d->present=fdc_geometry(d->type,&d->geometry);
        d->block=(struct blkdev){.read=native_read,.write=native_write,.flush=native_flush,
            .media_check=native_check,.capacity=(uint32_t)d->geometry.cylinders*d->geometry.heads*d->geometry.sectors,
            .sector_size=512,.media_generation=1,.write_cache_state=BLKDEV_CACHE_DISABLED,.ctx=d};
    }
    c->deadline=c->io.now(c->io.ctx)+FDC_TIMEOUT;
    c->io.irq(c->io.ctx,true);
    c->irq_attached=true;
    e=reset(c);
    if (!e) { c->ready=true; c->reason="fdc_ready"; }
    if (!c->quarantined && !stop(c)) { quarantine(c); e=-FS_EQUARANTINED; }
    if (e && !c->quarantined) { c->reason="fdc_reset_failed"; (void)fdc_detach(c); }
    return e;
}
int fdc_detach(struct fdc *c)
{
    if (!c || !c->claim_count) return 0;
    int e=gate(c); if (e) return e;
    for (unsigned i=0;i<FDC_DRIVES;i++) if (c->drive[i].owner) { ungate(c); return -FS_EBUSY; }
    if (c->quarantined || !stop(c)) { quarantine(c); ungate(c); return -FS_EQUARANTINED; }
    c->io.irq(c->io.ctx,false); c->irq_attached=false; c->ready=false;
    e=release_claims(c);
    ungate(c); return e;
}
