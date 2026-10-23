/* Ciuki PCI/resource/UC-MMIO glue. SPDX-License-Identifier: GPL-2.0-only.
 * Aperture: Haiku r1beta5 kernel s3/driver.cpp MapDevice, BAR0+16 MiB,
 * 64 KiB. Firmware owns PCI decoding, VRAM size and the VBE scanout. */
#include "private.h"
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/mtrr.h>
#include <ciuki/timing.h>

static uint32_t read_reg(void *ctx,unsigned offset,unsigned bits)
{
    struct s3_device *s=ctx;
    if (!s->registers || offset>0x10000-bits/8 || offset%(bits/8)) return UINT32_MAX;
    return bits==16 ? *(volatile uint16_t *)(s->registers+offset) :
        *(volatile uint32_t *)(s->registers+offset);
}
static void write_reg(void *ctx,unsigned offset,uint32_t v,unsigned bits)
{
    struct s3_device *s=ctx;
    if (!s->registers || offset>0x10000-bits/8 || offset%(bits/8)) return;
    if (bits==16) *(volatile uint16_t *)(s->registers+offset)=(uint16_t)v;
    else *(volatile uint32_t *)(s->registers+offset)=v;
}
static uint8_t cr_read(void *ctx,uint8_t index)
{
    (void)ctx;
    uint32_t flags=irq_save();
    uint8_t previous=inb(0x3d4); outb(0x3d4,index);
    uint8_t v=inb(0x3d5); outb(0x3d4,previous);
    irq_restore(flags); return v;
}
static void cr_write(void *ctx,uint8_t index,uint8_t v)
{
    (void)ctx;
    uint32_t flags=irq_save();
    uint8_t previous=inb(0x3d4);
    outb(0x3d4,0x38); uint8_t lock1=inb(0x3d5); outb(0x3d5,0x48);
    outb(0x3d4,0x39); uint8_t lock2=inb(0x3d5); outb(0x3d5,0xa5);
    outb(0x3d4,index); outb(0x3d5,v);
    outb(0x3d4,0x39); outb(0x3d5,lock2);
    outb(0x3d4,0x38); outb(0x3d5,lock1);
    outb(0x3d4,previous); irq_restore(flags);
}
static uint64_t now(void *ctx)
{
    (void)ctx;
    if (!g_cpu_tsc || !g_tsc_per_ms) return g_ticks*1000;
    uint64_t t=ktime_cycles();
    return t/g_tsc_per_ms*1000+t%g_tsc_per_ms*1000/g_tsc_per_ms;
}
static int delay(void *ctx,unsigned us) { (void)ctx; return udelay(us); }
static void drain(void *ctx) { (void)ctx; framebuffer_write_drain(); }
static bool extent(uint32_t base,uint32_t bytes)
{
    if (base<0x100000 || !bytes || bytes>UINT32_MAX-base) return false;
    uint64_t end=(uint64_t)base+bytes;
    if (base<g_boot.kernel_end && end>g_boot.kernel_start) return false;
    for (unsigned n=0;n<g_boot.e820_count;n++) {
        const struct ciuki_e820 *e=&g_boot.e820[n];
        if (e->type==CBI_E820_RAM && base<e->base+e->length && end>e->base) return false;
    }
    return true;
}
static int claim(enum res_type type,uint32_t base,uint32_t bytes,const char *name,gen_t *generation)
{
    int h=registry_claim(type,base,base+bytes,name,false);
    if (h<0) return h;
    *generation=registry_get((unsigned)h)->generation;
    if (registry_activate(h,*generation)) {
        registry_quarantine(h,*generation); return -EIO;
    }
    return h;
}
int s3_access_begin(struct s3_device *s)
{
    const struct fb_device *f=&s->fb;
    uint32_t vram=g_boot.display.vram_bytes;
    /* No PCI writes or destructive BAR sizing. Legacy VGA ports must
     * already be decoded; monochrome VGA and disabled engines punt. */
    if ((s->pci.command&3)!=3 || f->phys>UINT32_MAX-0x1010000 ||
        !vram || vram>0x1000000 || f->size>vram ||
        !extent(f->phys+0x1000000,0x10000)) return -ENODEV;
    if (s->port_claim>=0 || s->reg_claim>=0) return -EBUSY;
    s->port_claim=claim(RES_PORT,0x3b0,0x30,s->id,&s->port_generation);
    if (s->port_claim<0) return s->port_claim;
    s->io=(struct s3_io){s,read_reg,write_reg,cr_read,cr_write,now,delay,drain};
    if (!(inb(0x3cc)&1)) { (void)s3_access_end(s); return -EOPNOTSUPP; }
    s->reg_claim=claim(RES_MMIO,f->phys+0x1000000,0x10000,s->id,&s->reg_generation);
    if (s->reg_claim<0) { (void)s3_access_end(s); return -EBUSY; }
    if (!s->reg_window) s->reg_window=vmm_reserve_mmio(0x10000);
    s->registers=s->reg_window ? vmm_replace_mmio(s->reg_window,0x10000,f->phys+0x1000000,0x10000) : 0;
    if (!s->registers) { (void)s3_access_end(s); return -ENOMEM; }
    /* Cursor base is a 12-bit KiB offset. Reserve a whole page; the Trio
     * donor requires 2048 bytes to avoid a stray white line. */
    uint32_t ceiling=vram<0x400000 ? vram : 0x400000;
    uint32_t off=ceiling>=PAGE_SIZE ? (ceiling-PAGE_SIZE)&~(PAGE_SIZE-1) : 0;
    s->cursor=0;
    if (off>=f->size && off && extent(f->phys+off,PAGE_SIZE)) {
        s->cursor_claim=claim(RES_MMIO,f->phys+off,PAGE_SIZE,s->id,&s->cursor_generation);
        if (s->cursor_claim>=0) {
            if (!s->cursor_window) s->cursor_window=vmm_reserve_mmio(PAGE_SIZE);
            s->cursor=s->cursor_window ? vmm_replace_mmio(s->cursor_window,PAGE_SIZE,f->phys+off,PAGE_SIZE) : 0;
            s->cursor_offset=off;
        }
    }
    return 0;
}
/* Registry proof callback has no context. At most one native adapter can
 * own scanout; this pointer is used only synchronously by access_end. */
static struct s3_device *releasing;
static bool release_proof(int h,gen_t g)
{
    struct s3_device *s=releasing;
    (void)g;
    if (!s || !s->cpu_safe || !s->cursor_hidden || s->active) return false;
    if (h==s->cursor_claim && s->cursor) {
        vmm_revoke_mmio(s->cursor,PAGE_SIZE); s->cursor=0;
    }
    if (h==s->reg_claim && s->registers) {
        vmm_revoke_mmio((void *)s->registers,0x10000); s->registers=0;
    }
    return true;
}
int s3_access_end(struct s3_device *s)
{
    if (!s->cpu_safe || !s->cursor_hidden || s->active) return -EIO;
    if (s->mmio_changed) {
        s->io.cr_write(s->io.ctx,0x53,s->saved_cr53);
        if (s->io.cr_read(s->io.ctx,0x53)!=s->saved_cr53) return -EIO;
        s->mmio_changed=false;
    }
    int *claims[]={&s->cursor_claim,&s->reg_claim,&s->port_claim};
    gen_t gens[]={s->cursor_generation,s->reg_generation,s->port_generation};
    releasing=s;
    for (unsigned n=0;n<3;n++) if (*claims[n]>=0) {
        int e=registry_quiesce(*claims[n],gens[n],release_proof);
        if (!e) e=registry_release(*claims[n],gens[n]);
        if (e) { registry_quarantine(*claims[n],gens[n]); releasing=0; return e; }
        *claims[n]=-1;
    }
    releasing=0;
    memset(&s->io,0,sizeof(s->io)); s->touched=false; s->cursor_available=false;
    return 0;
}
