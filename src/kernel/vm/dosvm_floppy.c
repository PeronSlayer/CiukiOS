/* Per-VM FDC/INT13 provider; VMM owns all physical resources.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include "dosvm_internal.h"
#pragma weak floppy_controller
#pragma weak fdc_acquire
#pragma weak fdc_release
#pragma weak fdc_revalidate
#pragma weak fdc_check
#pragma weak fdc_transfer
#pragma weak fdc_format

static int floppy_provider_check(void *ctx, unsigned unit)
{
    struct dosvm *v=ctx;
    if (unit>=2 || !v->floppy.binding[unit].assigned || v->info.state>=DOSVM_STOPPING) return -FS_ENOENT;
    struct dosvm_floppy_binding *b=&v->floppy.binding[unit];
    if (b->file) return 0; /* VFS description pins identity/generation */
    struct fdc_drive *d=&b->physical.controller->drive[b->physical.drive];
    /* Guest polling reads retained state. Physical I/O checks DIR itself. */
    if (d->block.quarantined) return -FS_EQUARANTINED;
    return d->changed || d->block.media_generation!=b->media_generation ? -FS_EAGAIN : 0;
}
static int floppy_provider_sector(void *ctx, unsigned unit, uint32_t lba, void *data, bool write)
{
    struct dosvm *v=ctx;
    int e=floppy_provider_check(ctx,unit); if (e) return e;
    struct dosvm_floppy_binding *b=&v->floppy.binding[unit];
    if (write && !b->writable) return -FS_EROFS;
    if (!b->file) return fdc_transfer(&b->physical,lba,1,data,write);
    e=dosvm_disk_gate_enter(v); if (e) return e;
    uint64_t at; size_t done=0;
    e=vfs_seek(&v->disk_table,b->image,(uint64_t)lba*512,VFS_SEEK_SET,&at);
    if (!e && at!=(uint64_t)lba*512) e=-FS_EIO;
    if (!e) e=write ? vfs_write(&v->disk_table,b->image,data,512,&done) :
                      vfs_read(&v->disk_table,b->image,data,512,&done);
    if (!e && done!=512) e=-FS_EIO;
    if (!e && write) e=vfs_commit(&v->disk_table,b->image);
    dosvm_disk_gate_leave(v);
    return e;
}
static int floppy_provider_format(void *ctx, unsigned unit, unsigned cyl, unsigned head, uint8_t fill)
{
    struct dosvm *v=ctx;
    int e=floppy_provider_check(ctx,unit); if (e && e!=-FS_EAGAIN) return e;
    struct dosvm_floppy_binding *b=&v->floppy.binding[unit];
    if (!b->writable) return -FS_EROFS;
    if (!b->file) {
        e=fdc_format(&b->physical,cyl,head,fill);
        if (!e) {
            b->media_generation=b->physical.controller->drive[b->physical.drive].block.media_generation;
            v->floppy.model.changed[unit]=false;
        }
        return e;
    }
    e=0;
    const struct fdc_geometry *g=&v->floppy.model.geometry[unit];
    memset(v->sector,fill,512);
    for (unsigned s=0;!e && s<g->sectors;s++)
        e=floppy_provider_sector(ctx,unit,(cyl*g->heads+head)*g->sectors+s,v->sector,true);
    return e;
}
static int floppy_provider_revalidate(void *ctx, unsigned unit)
{
    struct dosvm *v=ctx;
    if (unit>=2 || !v->floppy.binding[unit].assigned) return -FS_ENOENT;
    struct dosvm_floppy_binding *b=&v->floppy.binding[unit];
    if (b->file) return 0;
    struct fdc_drive *d=&b->physical.controller->drive[b->physical.drive];
    unsigned type=v->floppy.model.rate==2 ? 3 : v->floppy.model.rate==3 ? 5 : d->type==2 ? 2 : 4;
    int e=fdc_revalidate(&b->physical,type);
    if (!e) { b->media_generation=d->block.media_generation; v->floppy.model.geometry[unit]=d->geometry; }
    return e;
}
static bool floppy_provider_status(void *ctx, unsigned unit, uint8_t result[3])
{
    struct dosvm *v=ctx;
    if (unit>=2 || !v->floppy.binding[unit].assigned || v->floppy.binding[unit].file) return false;
    const struct fdc *c=v->floppy.binding[unit].physical.controller;
    if (!(c->result[0]&0xc0) || !(c->result[1]|c->result[2])) return false;
    memcpy(result,c->result,3); return true;
}
static unsigned floppy_dma_bytes(void *ctx, bool to_guest)
{
    struct dosvm *v=ctx; struct dosvm_dma *d=&v->audio.dma;
    struct dosvm_dma_channel *ch=&d->ch[2];
    unsigned bytes=(unsigned)ch->count+1;
    uint32_t linear=((uint32_t)d->page[1]<<16)|ch->address;
    if (ch->masked || (d->command[0]&4) || (ch->mode&0x20) || (ch->mode>>6)==3 ||
        ((ch->mode>>2)&3)!=(to_guest ? 1u : 2u) || bytes>65536u-ch->address ||
        !dosvm_range(v,linear,bytes,to_guest)) return 0;
    return bytes;
}
static int floppy_dma(void *ctx, void *p, unsigned bytes, bool to_guest)
{
    struct dosvm *v=ctx;
    if (!bytes || bytes>floppy_dma_bytes(ctx,to_guest)) return -FS_EIO;
    struct dosvm_dma *d=&v->audio.dma; struct dosvm_dma_channel *ch=&d->ch[2];
    uint32_t linear=((uint32_t)d->page[1]<<16)|ch->address;
    int e=to_guest ? dosvm_write(v,linear,p,bytes) : dosvm_read(v,linear,p,bytes);
    if (e) return e;
    unsigned left=(unsigned)ch->count+1;
    ch->address=(uint16_t)(ch->address+bytes); ch->count=(uint16_t)(ch->count-bytes);
    if (bytes==left) {
        d->status[0]|=4; d->request[0]&=~4;
        if (ch->mode&0x10) { ch->address=ch->base_address; ch->count=ch->base_count; }
        else ch->masked=true;
    }
    v->memory.pool.dma_dirty=true;
    return 0;
}
static void floppy_virtual_irq(void *ctx)
{ struct dosvm *v=ctx; (void)dosvm_irq_raise(v->info.key,6); }
static int floppy_port(struct dosvm *v, uint16_t port, unsigned width, bool write, uint32_t *value, void *arg)
{
    (void)arg;
    if (width!=1) { if (!write) *value=UINT32_MAX; return 0; }
    uint8_t b=vfdc_io(&v->floppy.model,port-0x3f0,write,(uint8_t)*value);
    if (!write) *value=b;
    return 0;
}
int dosvm_floppy_init(struct dosvm *v)
{
    memset(&v->floppy,0,sizeof(v->floppy));
    struct vfdc_ops ops={floppy_provider_check,floppy_provider_sector,floppy_provider_format,
        floppy_dma,floppy_dma_bytes,floppy_virtual_irq,v,floppy_provider_revalidate,floppy_provider_status};
    vfdc_init(&v->floppy.model,&ops);
    int e=dosvm_port_register(v->info.key,0x3f2,0x3f2,floppy_port,0);
    if (!e) e=dosvm_port_register(v->info.key,0x3f4,0x3f5,floppy_port,0);
    if (!e) e=dosvm_port_register(v->info.key,0x3f7,0x3f7,floppy_port,0);
    return e;
}
uint16_t dosvm_floppy_equipment(const struct dosvm *v)
{
    unsigned count=v->floppy.binding[1].assigned ? 2 : v->floppy.binding[0].assigned ? 1 : 0;
    return 0x22 | (count ? 1 | ((count-1)<<6) : 0);
}
static void floppy_publish(struct dosvm *v, unsigned guest, const struct fdc_geometry *g)
{
    v->floppy.model.geometry[guest]=*g; v->floppy.model.present[guest]=true;
    uint16_t equipment=0;
    (void)dosvm_read(v,0x410,&equipment,2);
    equipment=(equipment & ~0xc1u)|(dosvm_floppy_equipment(v) & 0xc1u);
    (void)dosvm_write(v,0x410,&equipment,2);
    uint8_t table[]={0xaf,2,37,2,g->sectors,g->gap,0xff,g->format_gap,0xf6,15,8};
    /* Spare bytes after the INT 1Eh/1Fh three-byte BIOS stubs. Do not use
     * the lead-reserved DFVM/DPMI page or change any stub's code bytes. */
    memcpy((uint8_t *)P2V(v->frames[0xf0])+0x1e3+guest*16,table,sizeof(table));
    if (!guest || !v->floppy.binding[0].assigned) {
        uint16_t pointer[]={0x1e3+guest*16,0xf000};
        (void)dosvm_write(v,0x78,pointer,sizeof(pointer));
    }
}
int dosvm_floppy_assign(struct dosvm_key key, unsigned guest, unsigned physical)
{
    struct dosvm *v=dosvm_lookup(key);
    if (!v || guest>=2 || v->started || v->info.state!=DOSVM_CREATING) return -FS_EINVAL;
    /* sec-k05: BIOS INT 13h of a user VM (administrators included) reaches
     * only its private providers; a physical FDC lease is internal-only. */
    if (!v->identity.system) return -FS_EACCES;
    if (v->floppy.binding[guest].assigned) return -FS_EBUSY;
    struct fdc *c=floppy_controller ? floppy_controller() : 0;
    if (!c || !fdc_acquire || !fdc_revalidate || !fdc_release || physical>=2) return -FS_ENOENT;
    struct fdc_lease lease;
    int e=fdc_acquire(c,physical,key.id,key.generation,&lease); if (e) return e;
    e=fdc_revalidate(&lease,c->drive[physical].type);
    if (e && !c->quarantined && c->drive[physical].type==5) e=fdc_revalidate(&lease,4);
    if (e && !c->quarantined && c->drive[physical].type>=4) e=fdc_revalidate(&lease,3);
    if (e && e!=-FS_EIO) { (void)fdc_release(&lease); return e; }
    if (e) {
        /* The drive is assignable with empty/unformatted media. Raw reads
         * still fail; FORMAT can create IDs using the hardware geometry. */
        if (c->quarantined) { (void)fdc_release(&lease); return -FS_EQUARANTINED; }
        (void)fdc_geometry(c->drive[physical].type,&c->drive[physical].geometry);
        const struct fdc_geometry *g=&c->drive[physical].geometry;
        c->drive[physical].block.capacity=(uint32_t)g->cylinders*g->heads*g->sectors;
    }
    v->floppy.binding[guest]=(struct dosvm_floppy_binding){.physical=lease,.assigned=true,.writable=true,
        .media_generation=c->drive[physical].block.media_generation};
    floppy_publish(v,guest,&c->drive[physical].geometry); return 0;
}
int dosvm_floppy_image(struct dosvm_key key, unsigned guest, const char *path, bool writable)
{
    struct dosvm *v=dosvm_lookup(key);
    if (!v || guest>=2 || !path || !v->table_ready || v->started || v->info.state!=DOSVM_CREATING) return -FS_EINVAL;
    if (v->floppy.binding[guest].assigned) return -FS_EBUSY;
    int h=vfs_open(&v->disk_table,path,VFS_READ|(writable ? VFS_WRITE : 0),VFS_DENY_ALL,0);
    if (h<0) return h;
    struct fat_entry entry; int e=vfs_handle_stat(&v->disk_table,h,&entry,0);
    struct fdc_geometry g; bool found=false;
    for (unsigned type=1;!e && type<=5;type++) {
        if (fdc_geometry(type,&g) && entry.size==(uint32_t)g.cylinders*g.heads*g.sectors*512) { found=true; break; }
    }
    if (!e && (!found || (entry.attr&FAT_ATTR_DIR))) e=-FS_EINVAL;
    if (e) { (void)vfs_close(&v->disk_table,h); return e; }
    v->floppy.binding[guest]=(struct dosvm_floppy_binding){.image=h,.assigned=true,.file=true,.writable=writable};
    floppy_publish(v,guest,&g); return 0;
}
int dosvm_floppy_close(struct dosvm *v)
{
    int e=0;
    for (unsigned unit=0;unit<2;unit++) {
        struct dosvm_floppy_binding *b=&v->floppy.binding[unit];
        if (!b->assigned) continue;
        int r=b->file ? vfs_close(&v->disk_table,b->image) : fdc_release(&b->physical);
        if (r && !e) e=r;
        if (!r || (b->file && vfs_handle_stat(&v->disk_table,b->image,0,0)==-FS_EBADF)) {
            memset(b,0,sizeof(*b)); v->floppy.model.present[unit]=false;
        }
    }
    return e;
}
static int floppy_bios_result(struct dosvm *v, struct v86_frame *f, uint8_t status)
{
    v->floppy.status=status;
    f->tf.eax=(f->tf.eax&~0xff00u)|((uint32_t)status<<8);
    f->tf.eflags=(f->tf.eflags&~V86_CF)|(status ? V86_CF : 0);
    (void)dosvm_write(v,0x441,&status,1); return 0;
}
int dosvm_floppy_bios(struct dosvm *v, struct v86_frame *f)
{
    unsigned unit=(uint8_t)f->tf.edx, fn=(uint8_t)(f->tf.eax>>8);
    if (unit>=2 || !v->floppy.binding[unit].assigned) return floppy_bios_result(v,f,0x80);
    struct dosvm_floppy_binding *b=&v->floppy.binding[unit];
    const struct fdc_geometry *g=&v->floppy.model.geometry[unit];
    if (fn==1) return floppy_bios_result(v,f,v->floppy.status);
    if (fn==0 || fn==0x16) {
        int e=b->file ? 0 : fdc_check(&b->physical);
        if (e==-FS_EAGAIN && fn==0) {
            /* Explicit reset/revalidation acknowledges changed physical media;
             * old native mounts cannot coexist with this VM lease. */
            e=fdc_revalidate(&b->physical,g->type);
            if (!e) b->media_generation=b->physical.controller->drive[b->physical.drive].block.media_generation;
        }
        return floppy_bios_result(v,f,floppy_bios_status(e));
    }
    if (fn==8) {
        unsigned cyl=g->cylinders-1;
        f->tf.eax&=~255u;
        f->tf.ebx=(f->tf.ebx&~255u)|g->type;
        f->tf.ecx=(f->tf.ecx&0xffff0000u)|((cyl&255)<<8)|((cyl>>2)&0xc0)|g->sectors;
        f->tf.edx=(f->tf.edx&0xffff0000u)|((g->heads-1)<<8)|
            (v->floppy.binding[1].assigned ? 2 : 1);
        /* Table lives in the existing synthetic BIOS ROM, outside code. */
        f->es=0xf000; f->tf.edi=(f->tf.edi&0xffff0000u)|(0x1e3+unit*16);
        return floppy_bios_result(v,f,0);
    }
    if (fn==0x15) {
        floppy_bios_result(v,f,0); f->tf.eax=(f->tf.eax&~0xff00u)|0x0200; return 0;
    }
    if (fn==0x17 || fn==0x18) return floppy_bios_result(v,f,1); /* no unaudited geometry mutation */
    if (fn!=2 && fn!=3 && fn!=4 && fn!=5) return floppy_bios_result(v,f,1);
    unsigned cyl=((f->tf.ecx>>8)&255)|((f->tf.ecx&0xc0)<<2);
    unsigned head=(f->tf.edx>>8)&255, sector=f->tf.ecx&63, count=(uint8_t)f->tf.eax;
    bool write=fn==3, format=fn==5, verify=fn==4;
    uint32_t offset=(uint16_t)f->tf.ebx, linear=(f->es<<4)+offset;
    unsigned bytes=format ? g->sectors*4 : count*512;
    uint32_t first=(cyl*g->heads+head)*g->sectors+(sector ? sector-1 : 0);
    uint32_t capacity=(uint32_t)g->cylinders*g->heads*g->sectors;
    if (cyl>=g->cylinders || head>=g->heads || (!format && (!sector || sector>g->sectors || !count || count>128 || count>capacity-first)))
        return floppy_bios_result(v,f,4);
    if (f->es>0xffff || bytes>65536-offset || (!verify && !dosvm_range(v,linear,bytes,!write && !format)))
        return floppy_bios_result(v,f,9);
    int e=0; unsigned done=0;
    if (format) {
        if (count!=g->sectors) return floppy_bios_result(v,f,1);
        e=dosvm_read(v,linear,v->sector,bytes);
        for (unsigned s=0;!e && s<g->sectors;s++) {
            uint8_t *id=v->sector+s*4;
            if (id[0]!=cyl || id[1]!=head || id[2]!=s+1 || id[3]!=2) e=-FS_EINVAL;
        }
        if (!e) e=floppy_provider_format(v,unit,cyl,head,0xf6);
        if (!e) done=count;
    } else for (;done<count;done++) {
        uint32_t lba=(cyl*g->heads+head)*g->sectors+sector-1+done;
        if (write) e=dosvm_read(v,linear+done*512,v->sector,512);
        if (!e) e=floppy_provider_sector(v,unit,lba,v->sector,write);
        if (!e && !write && !verify) e=dosvm_write(v,linear+done*512,v->sector,512);
        if (e) break;
    }
    f->tf.eax=(f->tf.eax&~255u)|done;
    uint8_t code=floppy_bios_status(e),raw[3];
    if (e==-FS_EIO && floppy_provider_status(v,unit,raw)) {
        if ((raw[1]|raw[2])&0x20) code=0x10;
        else if ((raw[1]|raw[2])&1) code=2;
        else if (raw[1]&4) code=4;
        else if (raw[1]&0x10) code=8;
    }
    return floppy_bios_result(v,f,code);
}
