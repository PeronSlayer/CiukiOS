/* Intel 82077AA command/result/IRQ frontend. Independent implementation.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "floppy_model.h"
uint8_t floppy_bios_status(int e)
{
    if (!e) return 0;
    if (e==-FS_EROFS) return 3;
    if (e==-FS_EAGAIN) return 6;
    if (e==-FS_EINVAL) return 4;
    if (e==-FS_ENOENT || e==-FS_EACCES || e==-FS_EBUSY) return 0x80;
    return 0x20;
}
void vfdc_init(struct vfdc *f, const struct vfdc_ops *ops)
{ memset(f,0,sizeof(*f)); f->ops=*ops; f->dor=0x0c; }
static void vfdc_signal(struct vfdc *f) { if ((f->dor&0x0c)==0x0c && f->ops.irq) f->ops.irq(f->ops.ctx); }
static void vfdc_reply(struct vfdc *f, const uint8_t *p, unsigned n, bool irq)
{
    memcpy(f->result,p,n); f->position=0; f->results=n; f->length=f->needed=0;
    f->executing=false; if (irq) vfdc_signal(f);
}
static void vfdc_sense(struct vfdc *f, uint8_t st0, uint8_t cyl)
{
    if (f->senses<=6) { f->sense[f->senses++]=st0; f->sense[f->senses++]=cyl; }
    vfdc_signal(f);
}
static int vfdc_medium(struct vfdc *f, unsigned unit)
{
    if (unit>=2 || !f->present[unit]) return -FS_ENOENT;
    int e=f->ops.check ? f->ops.check(f->ops.ctx,unit) : 0;
    if (e==-FS_EAGAIN) f->changed[unit]=true;
    return e;
}
static void vfdc_execute(struct vfdc *f)
{
    uint8_t *c=f->command, op=c[0]&31, unit=c[1]&3, head=(c[1]>>2)&1;
    uint8_t r[7]={unit|(head<<2),0,0,c[2],head,c[4],2};
    int e=0; bool provider_error=false; f->executing=true;
    switch (op) {
    case 3: case 0x12: case 0x13: f->length=f->needed=0; f->executing=false; return;
    case 0x10: { uint8_t version=0x90; vfdc_reply(f,&version,1,false); return; }
    case 8:
        if (!f->senses) { uint8_t invalid=0x80; vfdc_reply(f,&invalid,1,false); return; }
        vfdc_reply(f,f->sense,2,false); f->senses-=2; memmove(f->sense,f->sense+2,f->senses); return;
    case 4:
        r[0]=unit|(head<<2)|(unit<2 && !f->cylinder[unit] ? 0x10 : 0);
        if (unit<2 && f->present[unit]) r[0]|=0x28;
        vfdc_reply(f,r,1,false); return;
    case 7: case 15:
        e=vfdc_medium(f,unit);
        if (e==-FS_EAGAIN && f->ops.revalidate) e=f->ops.revalidate(f->ops.ctx,unit);
        if (unit<2 && f->present[unit]) {
            /* A guest seek clears the guest change latch, never the physical
             * controller. Physical revalidation belongs to the provider. */
            f->cylinder[unit]=op==7 ? 0 : c[2];
            if (!e) f->changed[unit]=false;
        }
        f->length=f->needed=0; f->executing=false;
        vfdc_sense(f,(e ? 0x68 : 0x20)|unit,unit<2 ? f->cylinder[unit] : 0); return;
    case 10:
        e=vfdc_medium(f,unit);
        if (!e) {
            r[3]=f->cylinder[unit]; r[5]=1;
            if (f->rate!=f->geometry[unit].rate) e=-FS_EIO;
        }
        break;
    case 5: case 6: {
        bool write=op==5;
        e=vfdc_medium(f,unit);
        if (e) break;
        if (!(f->dor&8)) { e=-FS_EIO; r[1]=0x10; break; }
        const struct fdc_geometry *g=&f->geometry[unit];
        unsigned cyl=c[2], h=c[3], s=c[4], end=c[6];
        if (!(c[0]&0x40) || c[5]!=2 || h!=head || h>=g->heads || cyl>=g->cylinders ||
            !s || s>end || end>g->sectors || f->rate!=g->rate) { e=-FS_EINVAL; break; }
        unsigned bytes=f->ops.dma_bytes ? f->ops.dma_bytes(f->ops.ctx,!write) : 0;
        unsigned sectors=bytes/512, available=end-s+1;
        if (c[0]&0x80) available+=(g->heads-h-1)*g->sectors;
        if (!sectors || bytes%512 || !f->ops.dma || !f->ops.sector) { e=-FS_EIO; r[1]=0x10; break; }
        if (sectors>available) sectors=available;
        for (unsigned n=0;!e && n<sectors;n++) {
            uint32_t lba=(cyl*g->heads+h)*g->sectors+s-1;
            if (write) { e=f->ops.dma(f->ops.ctx,f->sector,512,false); if (e) r[1]=0x10; }
            if (!e) { e=f->ops.sector(f->ops.ctx,unit,lba,f->sector,write); provider_error=e!=0; }
            if (!e && !write) { e=f->ops.dma(f->ops.ctx,f->sector,512,true); if (e) r[1]=0x10; }
            r[3]=cyl; r[4]=h; r[5]=s;
            if (!e) {
                /* Intel table 5-6: result CHRN is the next sector ID. */
                if (s<end) r[5]=s+1;
                else { r[3]=cyl+((c[0]&0x80) ? h : 1); r[4]=(c[0]&0x80) ? h^1u : h; r[5]=1; }
            }
            if (++s>end) { s=1; h++; }
        }
        f->cylinder[unit]=cyl;
        if (!e && bytes/512>available) {
            r[0]|=0x40; r[1]=0x80; /* EOT before DMA terminal count */
            vfdc_reply(f,r,7,true); return;
        }
        break;
    }
    case 13: {
        e=vfdc_medium(f,unit); if (e && e!=-FS_EAGAIN) break;
        e=0; /* explicit format may establish a changed/blank medium */
        if (!(f->dor&8)) { e=-FS_EIO; r[1]=0x10; break; }
        const struct fdc_geometry *g=&f->geometry[unit];
        unsigned cyl=f->cylinder[unit], n=c[3]*4u;
        if (!(c[0]&0x40) || c[2]!=2 || c[3]!=g->sectors || n>sizeof(f->sector) ||
            cyl>=g->cylinders || head>=g->heads || f->rate!=g->rate || !f->ops.dma || !f->ops.format ||
            !f->ops.dma_bytes || f->ops.dma_bytes(f->ops.ctx,false)!=n) { e=-FS_EINVAL; break; }
        e=f->ops.dma(f->ops.ctx,f->sector,n,false);
        for (unsigned s=0;!e && s<g->sectors;s++) {
            const uint8_t *id=f->sector+s*4;
            if (id[0]!=cyl || id[1]!=head || id[2]!=s+1 || id[3]!=2) e=-FS_EINVAL;
        }
        if (!e) { e=f->ops.format(f->ops.ctx,unit,cyl,head,c[5]); provider_error=e!=0; }
        r[3]=cyl; r[5]=g->sectors; break;
    }
    default: r[0]=0x80; vfdc_reply(f,r,1,false); return;
    }
    if (e) {
        r[0]|=0x40;
        if (!r[1]) r[1]=e==-FS_EROFS ? 2 : e==-FS_EINVAL || e==-FS_EAGAIN ? 4 : 0x20;
        if (e==-FS_ENOENT) r[0]|=8;
        uint8_t status[3];
        if (e==-FS_EIO && provider_error && f->ops.status && f->ops.status(f->ops.ctx,unit,status)) {
            r[0]=(status[0]&~3u)|unit; r[1]=status[1]; r[2]=status[2];
        }
    }
    vfdc_reply(f,r,7,true);
}
uint8_t vfdc_io(struct vfdc *f, unsigned reg, bool write, uint8_t value)
{
    if (reg==2) {
        if (!write) return f->dor;
        uint8_t old=f->dor; f->dor=value;
        if (!(value&4)) { f->length=f->needed=f->results=f->position=f->senses=0; f->executing=false; }
        else if (!(old&4)) {
            for (unsigned i=0;i<4;i++) { f->sense[i*2]=0xc0|i; f->sense[i*2+1]=0; }
            f->senses=8; f->cylinder[0]=f->cylinder[1]=0; vfdc_signal(f);
        }
        return 0xff;
    }
    if (reg==7) {
        if (write) { f->rate=value&3; return 0xff; }
        unsigned unit=f->dor&3;
        if (unit<2 && f->present[unit] && f->ops.check && f->ops.check(f->ops.ctx,unit)==-FS_EAGAIN) f->changed[unit]=true;
        return unit>=2 || !f->present[unit] || f->changed[unit] ? 0x80 : 0;
    }
    if (reg==4) {
        if (write) {
            f->rate=value&3;
            if (value&0x80) { uint8_t dor=f->dor; vfdc_io(f,2,true,dor&~4u); vfdc_io(f,2,true,dor|4); }
            return 0xff;
        }
        if (!(f->dor&4)) return 0;
        return f->executing ? 0x10 : f->results ? 0xd0 : f->length ? 0x90 : 0x80;
    }
    if (reg!=5 || !(f->dor&4)) return 0xff;
    if (!write) {
        if (!f->results) return 0xff;
        uint8_t b=f->result[f->position++]; if (f->position==f->results) f->results=f->position=0;
        return b;
    }
    if (f->results || f->executing) return 0xff;
    if (!f->length) {
        memset(f->command,0,sizeof(f->command));
        switch (value&31) {
        case 3: case 15: f->needed=3; break;
        case 4: case 7: case 10: case 0x12: f->needed=2; break;
        case 5: case 6: f->needed=9; break;
        case 13: f->needed=6; break;
        case 0x13: f->needed=4; break;
        default: f->needed=1; break;
        }
    }
    f->command[f->length++]=value;
    if (f->length==f->needed) vfdc_execute(f);
    return 0xff;
}
