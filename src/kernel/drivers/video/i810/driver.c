/*-*- linux-c -*-
 *  linux/drivers/video/i810_accel.c -- Hardware Acceleration
 *
 *      Copyright (C) 2001 Antonino Daplas<adaplas@pol.net>
 *      All Rights Reserved
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License. See the file COPYING in the main directory of this archive for
 *  more details.
 */

/* Additional original notice from the cursor-sequence donor; no mode/PLL code imported. */
 /*-*- linux-c -*-
 *  linux/drivers/video/i810_main.c -- Intel 810 frame buffer device
 *
 *      Copyright (C) 2001 Antonino Daplas<adaplas@pol.net>
 *      All Rights Reserved
 *
 *      Contributors:
 *         Michael Vogt <mvogt@acm.org> - added support for Intel 815 chipsets
 *                                        and enabling the power-on state of
 *                                        external VGA connectors for
 *                                        secondary displays
 *
 *         Fredrik Andersson <krueger@shell.linux.se> - alpha testing of
 *                                        the VESA GTF
 *
 *         Brad Corrion <bcorrion@web-co.com> - alpha testing of customized
 *                                        timings support
 *
 *	The code framework is a modification of vfb.c by Geert Uytterhoeven.
 *      DotClock and PLL calculations are partly based on i810_driver.c
 *              in xfree86 v4.0.3 by Precision Insight.
 *      Watermark calculation and tables are based on i810_wmark.c
 *              in xfre86 v4.0.3 by Precision Insight.  Slight modifications
 *              only to allow for integer operations instead of floating point.
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License. See the file COPYING in the main directory of this archive for
 *  more details.
 */


/* Origin: Linux v6.12 drivers/video/fbdev/i810/{i810_accel.c,i810_main.c,i810.h}.
 * Native bounded port: no Linux/AGP host, no modes, no unbounded donor waits.
 * Original donor copyright and licence notice retained above. */
#include "driver.h"
#include "registers.h"

#include "../render3d.h"
static const struct render3d_family render_family = {
    .status=R3D_NOT_IMPLEMENTED, .potential_units=2, .potential_texture_size=2048
};

#define RING_HEAD_MASK 0x001ffffcu
#define RING_SIZE_MASK 0x001ff000u
#define RING_START_MASK 0xfffff000u
#define RING_TAIL_MASK 0x001ffff8u
#define CURSOR_ENABLE 0x1000u
#define GTT_ENTRIES 16384u
static struct i810_engine engine;
static uint8_t idx(struct video_native *s, uint8_t reg)
{ return s->io.indexed_read(s->io.ctx,0x3d4,reg); }
static bool owned_live(struct i810_engine *e)
{
    struct video_native *s=&e->native;
    if (!e->dma_phys) return true; /* isolated transport fixture */
    const struct fb_device *fb=fbdev_get();
    if (!fb || fb->generation!=s->fb.generation || fb->phys!=s->fb.phys ||
        fb->mapped!=s->fb.mapped || !registry_valid(fb->handle,fb->generation)) return false;
    for (unsigned i=0;i<s->access_count;i++) {
        struct video_access *a=&s->access[i];
        if (!registry_valid(a->handle,a->generation) ||
            registry_get((unsigned)a->handle)->state!=RS_ACTIVE) return false;
    }
    if (video_read(s,32,PGTBL_CTL)!=e->gtt_root) return false;
    for (unsigned i=0;i<e->gtt_modified;i++)
        if (video_read(s,32,GTT+(e->gtt_first+i)*4)!=((e->dma_phys+i*4096u)|1u)) return false;
    return true;
}
static int free_owned(struct i810_engine *e)
{
    if (!e->dma_phys) return 0;
    if (e->gtt_modified || e->ring_started) return -EIO;
    if (e->pin.ticket && (!mm_unpin || mm_unpin(&e->pin,e->pin.generation))) return -EIO;
    i810_dma_free(e->dma_phys);
    e->dma_phys=0; e->ring=e->cursor=0;
    memset(&e->pin,0,sizeof(e->pin));
    return 0;
}
static int idle(struct video_native *s)
{
    struct i810_engine *e=(struct i810_engine *)s;
    if (!owned_live(e)) return -EIO;
    uint32_t tail=video_read(s,32,IRING), head=video_read(s,32,IRING+4);
    uint32_t done=video_read(s,16,INSTDONE), error=video_read(s,32,PGTBL_ER);
    if (tail==UINT32_MAX || head==UINT32_MAX || done==0xffff || error) return -EIO;
    if ((tail&RING_TAIL_MASK)!=(head&RING_HEAD_MASK) || (done&0x7b)!=0x7b) return -EAGAIN;
    if (e->ring_started && (tail&RING_TAIL_MASK)!=e->tail) return -EIO;
    return 0;
}
static int hide(struct video_native *s)
{
    if (!owned_live((struct i810_engine *)s)) return -EIO;
    uint32_t v=video_read(s,32,PIXCONF);
    if (v==UINT32_MAX) return -EIO;
    video_write(s,32,PIXCONF,v&~CURSOR_ENABLE);
    return video_read(s,32,PIXCONF)==(v&~CURSOR_ENABLE) ? 0 : -EIO;
}
static int restore(struct video_native *s)
{
    struct i810_engine *e=(struct i810_engine *)s;
    if (e->ring_started) {
        uint32_t control=video_read(s,32,IRING+12);
        if (control==UINT32_MAX) return -EIO;
        video_write(s,32,IRING+12,control&~1u);
        if (video_read(s,32,IRING+12)!=(control&~1u)) return -EIO;
        if (idle(s)) return -EIO;
        if (!e->dma_phys)
            for (unsigned i=0;i<e->ring_bytes;i++) e->ring[i]=e->ring_saved[i];
        s->io.drain(s->io.ctx);
        for (unsigned i=0;i<4;i++) {
            video_write(s,32,IRING+i*4,e->saved_ring[i]);
            if (video_read(s,32,IRING+i*4)!=e->saved_ring[i]) return -EIO;
        }
        e->ring_started=false;
    }
    if (e->cursor_saved_valid) {
        for (unsigned i=0;i<4096;i++) e->cursor[i]=e->cursor_saved[i];
        s->io.drain(s->io.ctx);
        video_write(s,32,CURBASE,e->saved_cursor[1]);
        video_write(s,32,CURPOS,e->saved_cursor[2]);
        video_write(s,16,CURCNTR,e->saved_cursor[0]);
        uint8_t palette=(uint8_t)video_read(s,8,PIXCONF1);
        video_write(s,8,PIXCONF1,palette|1);
        video_palette_load(s,s->palette_saved);
        video_write(s,8,PIXCONF1,palette);
        if (video_read(s,32,CURBASE)!=e->saved_cursor[1] ||
            video_read(s,32,CURPOS)!=e->saved_cursor[2] ||
            video_read(s,16,CURCNTR)!=e->saved_cursor[0]) return -EIO;
    }
    int err=hide(s); /* precondition: firmware cursor was disabled */
    if (err) return err;
    if (e->gtt_modified) {
        if (!owned_live(e) || idle(s)) return -EIO;
        for (unsigned i=0;i<e->gtt_modified;i++) {
            video_write(s,32,GTT+(e->gtt_first+i)*4,e->saved_pte[i]);
            if (video_read(s,32,GTT+(e->gtt_first+i)*4)!=e->saved_pte[i]) return -EIO;
        }
        i810_dma_publish();
        for (unsigned i=0;i<e->gtt_modified;i++)
            if (video_read(s,32,GTT+(e->gtt_first+i)*4)!=e->saved_pte[i]) return -EIO;
        e->gtt_modified=0;
    }
    return free_owned(e);
}
static int emit(struct i810_engine *e, const uint32_t packet[6])
{
    struct video_native *s=&e->native;
    if (!s->ready || s->quarantined) return -EIO;
    int err=video_sync(s);
    if (err) return err;
    /* Fully synchronous: an empty ring has room for one complete 24-byte
     * packet plus an 8-byte guard. Tail is always qword aligned, even at wrap. */
    for (unsigned i=0;i<6;i++) {
        *(volatile uint32_t *)(e->ring+e->tail)=packet[i];
        e->tail=(e->tail+4)&(e->ring_bytes-1);
    }
    s->cpu_safe=false;
    s->io.drain(s->io.ctx);
    if (e->dma_phys) i810_dma_publish();
    video_write(s,32,IRING,e->tail);
    if ((video_read(s,32,IRING)&RING_TAIL_MASK)!=e->tail)
        return video_fault(s,"ring_tail_readback",-EIO);
    err=video_sync(s);
    if (!err) s->operations++;
    return err;
}
static uint32_t depth(struct video_native *s)
{ return s->fb.bpp==16 ? 1u<<24 : 2u<<24; }
static int fill(void *ctx, const struct fb_rect *r, uint32_t colour)
{
    struct i810_engine *e=ctx; struct video_native *s=&e->native;
    if (!video_rect(s,r)) return -EOPNOTSUPP;
    uint32_t packet[6]={0x50000003,0x80000000u|(1u<<26)|depth(s)|(0xf0u<<16)|s->fb.pitch,
        (uint32_t)r->height<<16 | (uint32_t)r->width*(s->fb.bpp/8),
        s->fb.phys+(uint32_t)r->y*s->fb.pitch+(uint32_t)r->x*(s->fb.bpp/8),colour,0};
    return emit(e,packet);
}
static int copy(void *ctx, const struct fb_rect *r, int32_t x, int32_t y)
{
    struct i810_engine *e=ctx; struct video_native *s=&e->native;
    struct fb_rect dst={x,y,r ? r->width : 0,r ? r->height : 0};
    if (!video_rect(s,r) || !video_rect(s,&dst)) return -EOPNOTSUPP;
    uint32_t bytes=s->fb.bpp/8, width=(uint32_t)r->width*bytes;
    uint32_t sx=(uint32_t)r->x*bytes, dx=(uint32_t)x*bytes, sy=r->y, dy=y;
    uint32_t pitch=s->fb.pitch, dir=0;
    if (x>r->x) { dir=1u<<30; sx+=width-1; dx+=width-1; }
    if (y>r->y) { pitch=(0u-pitch)&0xffff; sy+=r->height-1; dy+=r->height-1; }
    uint32_t packet[6]={0x50c00004,dir|(1u<<26)|depth(s)|(0xccu<<16)|pitch,
        (uint32_t)r->height<<16|width,s->fb.phys+dy*s->fb.pitch+dx,
        pitch,s->fb.phys+sy*s->fb.pitch+sx};
    return emit(e,packet);
}
static int cursor_show(void *ctx, bool show)
{
    struct i810_engine *e=ctx; struct video_native *s=&e->native;
    if (!s->cursor_available) return -EOPNOTSUPP;
    if (!show) { int err=hide(s); return err ? video_fault(s,"cursor_hide_readback",err) : 0; }
    if (!s->shape) return -EINVAL;
    if (!s->ready || s->quarantined) return -EIO;
    if (!owned_live(e)) return video_fault(s,"cursor_ownership_lost",-EIO);
    uint32_t conf=video_read(s,32,PIXCONF);
    if (conf==UINT32_MAX) return video_fault(s,"cursor_dead_read",-EIO);
    video_write(s,32,PIXCONF,conf|CURSOR_ENABLE);
    if (video_read(s,32,PIXCONF)!=(conf|CURSOR_ENABLE)) return video_fault(s,"cursor_show_readback",-EIO);
    return 0;
}
static int cursor_set(void *ctx, const struct accel_cursor *c)
{
    struct i810_engine *e=ctx; struct video_native *s=&e->native;
    if (!s->cursor_available) return -EOPNOTSUPP;
    if (!c) return -EINVAL;
    if (!s->ready || s->quarantined) return -EIO;
    uint8_t image[1024]; video_cursor_bits(image,c);
    int err=hide(s);
    if (err) return video_fault(s,"cursor_hide_readback",err);
    for (unsigned i=0;i<1024;i++) e->cursor[i]=image[i];
    s->io.drain(s->io.ctx);
    if (e->dma_phys) i810_dma_publish();
    uint8_t palette=(uint8_t)video_read(s,8,PIXCONF1);
    video_write(s,8,PIXCONF1,palette|1);
    uint8_t colours[6];
    for (unsigned n=0;n<2;n++) {
        uint32_t rgb=video_cursor_rgb(&s->fb,n ? c->foreground : c->background);
        for (unsigned b=0;b<3;b++) colours[n*3+b]=(uint8_t)((rgb>>(16-b*8)&255) >> ((palette&0x80) ? 0 : 2));
    }
    video_palette_load(s,colours);
    video_write(s,8,PIXCONF1,palette);
    video_write(s,32,CURBASE,e->cursor_phys);
    video_write(s,16,CURCNTR,0x15); /* active-display origin, 64x64 AND/XOR */
    if (video_read(s,32,CURBASE)!=e->cursor_phys || video_read(s,16,CURCNTR)!=0x15)
        return video_fault(s,"cursor_set_readback",-EIO);
    s->shape=true; return 0;
}
static int cursor_move(void *ctx, int32_t x, int32_t y)
{
    struct video_native *s=ctx;
    if (!s->cursor_available) return -EOPNOTSUPP;
    if (!s->shape) return -EINVAL;
    if (x < -2047 || y < -2047 || x>2047 || y>2047) return -EOPNOTSUPP;
    if (!s->ready || s->quarantined) return -EIO;
    if (!owned_live((struct i810_engine *)s)) return video_fault(s,"cursor_ownership_lost",-EIO);
    uint32_t px=x<0 ? (uint32_t)-x|0x8000 : (uint32_t)x;
    uint32_t py=y<0 ? (uint32_t)-y|0x8000 : (uint32_t)y;
    uint32_t value=py<<16|px;
    video_write(s,32,CURPOS,value);
    if (video_read(s,32,CURPOS)!=value) return video_fault(s,"cursor_position_readback",-EIO);
    return 0;
}
int i810_engine_prepare(struct i810_engine *e)
{
    struct video_native *s=&e->native;
    s->idle=idle; s->hide=hide; s->restore=restore;
    if (!video_format(&s->fb) || !e->ring || e->ring_bytes<4096 || e->ring_bytes>65536 ||
        (e->ring_bytes&(e->ring_bytes-1)) || (e->ring_phys&4095)) return -EOPNOTSUPP;
    uint32_t conf=video_read(s,32,PIXCONF);
    if (conf==UINT32_MAX || (conf&CURSOR_ENABLE) || (video_read(s,32,LRING+12)&1u)) return -EOPNOTSUPP;
    for (unsigned i=0;i<4;i++) e->saved_ring[i]=video_read(s,32,IRING+i*4);
    if ((e->saved_ring[3]&1u) || (!e->dma_phys &&
        ((e->saved_ring[2]&RING_START_MASK)!=e->ring_phys ||
         ((e->saved_ring[3]&RING_SIZE_MASK)+4096)!=e->ring_bytes))) return -EOPNOTSUPP;
    if (video_wait(s,idle)) return -EOPNOTSUPP; /* firmware writer not ours */
    e->saved_pixconf=conf;
    if (!e->dma_phys)
        for (unsigned i=0;i<e->ring_bytes;i++) e->ring_saved[i]=e->ring[i];
    e->saved_cursor[0]=video_read(s,16,CURCNTR); e->saved_cursor[1]=video_read(s,32,CURBASE);
    e->saved_cursor[2]=video_read(s,32,CURPOS);
    e->cursor_saved_valid=e->cursor && e->cursor_phys && (e->dma_phys || e->saved_cursor[1]==e->cursor_phys);
    if (e->cursor_saved_valid) {
        for (unsigned i=0;i<4096;i++) e->cursor_saved[i]=e->cursor[i];
        uint8_t palette=(uint8_t)video_read(s,8,PIXCONF1);
        video_write(s,8,PIXCONF1,palette|1); video_palette_save(s);
        video_write(s,8,PIXCONF1,palette);
    }
    s->cursor_available=e->cursor_saved_valid;
    s->ops=(struct accel_ops){.ctx=s,.sync=video_sync,.copy_rect=copy,.fill_rect=fill,
        .cursor_set=cursor_set,.cursor_move=cursor_move,.cursor_show=cursor_show,
        .stop=video_stop,.capabilities=video_capabilities,.cpu_ready=video_cpu_ready,.set_pause=video_pause};
    if (e->dma_phys) {
        /* Revalidate every firmware mapping under the drawing claim before
         * exposing any kernel page. Valid firmware entries are never replaced. */
        if (!owned_live(e)) return -EIO;
        for (unsigned i=0;i<GTT_ENTRIES;i++) {
            uint32_t pte=video_read(s,32,GTT+i*4);
            if (i>=e->gtt_first && i<e->gtt_first+I810_PRIVATE_PAGES) {
                if ((pte&1u) || pte!=e->saved_pte[i-e->gtt_first]) return -EIO;
            } else if (pte&1u) {
                uint32_t phys=pte&~4095u;
                if (i>=e->gtt_first || !video_disjoint(phys,4096,e->dma_phys,I810_PRIVATE_PAGES*4096u)) return -EIO;
            }
        }
        s->touched=true;
        for (unsigned i=0;i<I810_PRIVATE_PAGES;i++) {
            e->gtt_modified=i+1;
            uint32_t pte=(e->dma_phys+i*4096u)|1u;
            video_write(s,32,GTT+(e->gtt_first+i)*4,pte);
            if (video_read(s,32,GTT+(e->gtt_first+i)*4)!=pte)
                return video_fault(s,"gtt_install_readback",-EIO);
        }
        i810_dma_publish();
    }
    s->touched=true; e->ring_started=true; e->tail=0;
    video_write(s,32,IRING,0); video_write(s,32,IRING+4,0);
    video_write(s,32,IRING+8,e->ring_phys);
    s->io.drain(s->io.ctx);
    uint32_t control=(e->saved_ring[3]&~(RING_SIZE_MASK|1u))|(e->ring_bytes-4096u)|1u;
    video_write(s,32,IRING+12,control);
    if (video_read(s,32,IRING+12)!=control)
        return video_fault(s,"ring_enable_readback",-EIO);
    s->ready=true; s->cpu_safe=true; return 0;
}

/* Existing system-memory GTT pages only. A valid PTE is not an ownership
 * proof: reserved E820 backing, no table/status/kernel alias and uniqueness
 * throughout the complete 64-KiB GTT are required as well. */
bool i810_gtt_region(struct i810_engine *e, uint32_t offset, uint32_t bytes, uint32_t *physical)
{
    struct video_native *s=&e->native;
    if (!bytes || (offset&4095) || (bytes&4095) || (uint64_t)offset+bytes>GTT_ENTRIES*4096u) return false;
    uint32_t root=video_read(s,32,PGTBL_CTL), first=0;
    if (root==UINT32_MAX || !(root&1) || !video_reserved(root&~4095u,65536)) return false;
    for (unsigned n=0;n<bytes/4096;n++) {
        uint32_t entry=video_read(s,32,GTT+(offset/4096+n)*4), p=entry&~4095u;
        if ((entry&0xfff)!=1 || !video_reserved(p,4096) ||
            !video_disjoint(p,4096,root&~4095u,65536) ||
            !video_disjoint(p,4096,video_read(s,32,HWS_PGA)&~4095u,4096) ||
            !video_disjoint(p,4096,s->fb.phys,s->fb.size)) return false;
        if (!n) first=p;
        else if (p!=(uint64_t)first+n*4096) return false;
        for (unsigned i=0;i<GTT_ENTRIES;i++) {
            if (i==offset/4096+n) continue;
            uint32_t other=video_read(s,32,GTT+i*4);
            if ((other&1) && (other&~4095u)==p) return false;
        }
    }
    *physical=first; return true;
}
static const struct display_match matches[]={{0x8086,0x7121,3,255},{0x8086,0x7123,3,255},
    {0x8086,0x7125,3,255},{0x8086,0x1132,3,255}};
static bool probe(const struct pci_func *p)
{
    if (!p || p->vendor!=0x8086 || p->class_code!=3 || p->subclass || (p->header_type&0x7f) ||
        (p->command&3)!=3 || (p->bar[0]&7) || (p->bar[1]&7) || !(p->bar[0]&~15u) ||
        !(p->bar[1]&~15u)) return false;
    for (unsigned i=0;i<ARRAY_SIZE(matches);i++) if (p->device==matches[i].device) return true;
    return false;
}
static bool eligible(const struct fb_device *fb)
{
    struct video_native *s=&engine.native; const char *why;
    if (display_accel_disabled(&why)) { s->reason=why; return false; }
    if (s->quarantined || s->attempted) return false;
    if (!video_format(fb)) { s->reason="unsupported_format"; return false; }
    const struct pci_func *p=display_driver_owner();
    if (!p || !(p->command&4)) { s->reason="bus_master_disabled"; return false; }
    return true;
}
static int activation(void *ctx)
{
    struct i810_engine *e=ctx;
    int err=i810_engine_prepare(e);
    return err ? err : video_probe_pixels(&e->native);
}
static const struct accel_ops *start(const struct fb_device *fb, struct vfs *vfs)
{
    (void)vfs;
    struct i810_engine *e=&engine; struct video_native *s=&e->native;
    const struct pci_func *p=display_driver_owner(), *proved=0;
    const struct display_driver *table[]={&i810_driver}; const char *why;
    if (display_accel_disabled(&why) || s->quarantined || !s->armed || !video_format(fb) ||
        fb!=fbdev_get() || display_driver_select(fb,table,1,&proved)!=&i810_driver || p!=proved || !(p->command&4)) return 0;
    s->fb=*fb; s->cpu_safe=true; s->reason="transport_refused"; s->pending_record=true;
    uint32_t base=p->bar[1]&~15u;
    if ((base&0x7ffff) || !video_disjoint(base,0x80000,fb->phys,g_boot.display.vram_bytes)) return 0;
    void *mmio=video_map(s,RES_MMIO,base,0x80000);
    if (!mmio || !video_claim_vga_ports(s)) goto fail;
    video_hardware_io(s,mmio);
    if (video_read(s,32,DPLYBASE)==UINT32_MAX ||
        (video_read(s,32,DPLYBASE)&0x03fffffc)!=(fb->phys&0x03fffffc) ||
        ((video_read(s,32,PIXCONF)>>16)&15)!=(fb->bpp==16 ? 5u : 6u) ||
        (((uint32_t)idx(s,0x13)|((idx(s,0x41)&15u)<<8))*8)!=fb->pitch) goto fail;
    for (unsigned i=0;i<8;i++) if (video_read(s,32,FENCE+i*4)) goto fail;
    if ((video_read(s,32,IRING+12)&1) || (video_read(s,32,LRING+12)&1) ||
        (video_read(s,32,PIXCONF)&CURSOR_ENABLE) || video_wait(s,idle)) goto fail;
    uint32_t visible_backing=0;
    if (!i810_gtt_region(e,0,PAGE_ALIGN_UP(fb->size),&visible_backing) ||
        !video_disjoint(visible_backing,PAGE_ALIGN_UP(fb->size),base,0x80000) ||
        !video_claim(s,visible_backing,PAGE_ALIGN_UP(fb->size))) goto fail;
    if (!video_claim(s,video_read(s,32,PGTBL_CTL)&~4095u,65536)) goto fail;
    e->aperture_bytes=i810_aperture_bytes(p);
    if ((e->aperture_bytes!=(32u<<20) && e->aperture_bytes!=(64u<<20)) ||
        (fb->phys&(e->aperture_bytes-1)) || fb->size>e->aperture_bytes ||
        (uint64_t)fb->phys+e->aperture_bytes>UINT32_MAX) goto fail;
    e->gtt_first=e->aperture_bytes/4096u-I810_PRIVATE_PAGES;
    if (PAGE_ALIGN_UP(fb->size)/4096u>e->gtt_first) goto fail;
    e->gtt_root=video_read(s,32,PGTBL_CTL);
    for (unsigned i=e->gtt_first;i<GTT_ENTRIES;i++) {
        uint32_t pte=video_read(s,32,GTT+i*4);
        if (pte&1u) goto fail;
        if (i<e->gtt_first+I810_PRIVATE_PAGES) e->saved_pte[i-e->gtt_first]=pte;
    }
    e->ring=i810_dma_allocate(&e->dma_phys);
    if (!e->ring || !e->dma_phys || (e->dma_phys&4095u) ||
        (uint64_t)e->dma_phys+I810_PRIVATE_PAGES*4096u>0x20000000u) goto fail;
    e->pin=(struct mm_pin){.generation=fb->generation,.first_pfn=e->dma_phys/4096u,
        .pages=I810_PRIVATE_PAGES,.address_mask=0x1fffffffu,.alignment=4096,
        .direction=MM_PIN_BIDIRECTIONAL};
    if (!mm_pin || !mm_unpin || mm_pin(&e->pin)) goto fail;
    e->ring_phys=fb->phys+e->gtt_first*4096u;
    e->ring_bytes=I810_RING_PAGES*4096u;
    /* Claim only the unused GPU aperture. Kernel RAM ownership is the PMM
     * allocation plus DMA pin, never an MMIO claim over usable RAM. */
    if (!video_claim(s,e->ring_phys,I810_PRIVATE_PAGES*4096u)) goto fail;
    memset((void *)e->ring,0,I810_PRIVATE_PAGES*4096u);
    e->cursor=video_read(s,32,CURBASE) ? e->ring+e->ring_bytes : 0;
    e->cursor_phys=e->dma_phys+e->ring_bytes;
    if (fbdev_accel_probe(fb,activation,e)) goto fail;
    return &s->ops;
fail:
    if (s->touched) (void)video_stop(s);
    if (!s->touched && free_owned(e)) {
        s->reason="dma_release_unproved";
        s->quarantined=true;
    }
    video_release(s); return 0;
}
static const struct accel_ops *fallback(void)
{ struct video_native *s=&engine.native; if (s->touched) { (void)video_sync(s); return &s->ops; } return 0; }
static int begin(struct vfs *v) { return video_marker_begin(&engine.native,v); }
static int end(struct vfs *v) { return video_marker_end(&engine.native,v); }
static int failed(struct vfs *v) { return video_marker_failed(&engine.native,v); }
static void record(void) { video_record(&engine.native); }
static void quarantine(const char *why) { (void)video_fault(&engine.native,why,-EIO); }
static bool summary(bool *r, bool *u, const char **why) { return video_summary(&engine.native,r,u,why); }
static void snapshot(struct ciuki_accel_info *out) { video_snapshot(&engine.native,out); }
static struct i810_engine engine={.native={.id="i810",.name="Intel i810/i815",
    .marker="C:/system/settings/accel-i810.cfg",.reason="pending",.cpu_safe=true}};
const struct display_driver i810_driver={
    .render3d=&render_family,.id="i810",.attempt_files={"C:/system/settings/accel.cfg","C:/system/settings/accel-i810.cfg"},.name="Intel i810/i815",.matches=matches,
    .match_count=ARRAY_SIZE(matches),.framebuffer_bar=0,.probe=probe,.eligible=eligible,.start=start,
    .fallback=fallback,.attempt_begin=begin,.attempt_end=end,.attempt_failed=failed,.record=record,
    .quarantine=quarantine,.summary=summary,.snapshot=snapshot,.cursor_test=video_cursor_test};
