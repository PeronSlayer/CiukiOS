/* Private native-port lifecycle, bounded polling and activation reference.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "native.h"
#include <ciuki/vga_ports.h>
#include <ciuki/mm.h>
#include <ciuki/swdib.h>
#include "../../fs/vfs.h"

uint32_t video_read(struct video_native *s, unsigned width, uint32_t reg)
{ return s->io.read(s->io.ctx,width,reg); }
void video_write(struct video_native *s, unsigned width, uint32_t reg, uint32_t value)
{ s->io.write(s->io.ctx,width,reg,value); }
int video_wait(struct video_native *s, int (*poll)(struct video_native *))
{
    uint64_t start=s->io.now(s->io.ctx), slice=start;
    s->waits++;
    for (unsigned i=0;i<VIDEO_WAIT_POLLS;i++) {
        int e=poll(s);
        if (e!=-EAGAIN) return e;
        uint64_t now=s->io.now(s->io.ctx);
        if (now-start>=VIDEO_WAIT_US) return -ETIMEDOUT;
        if (s->pause && (now-slice>=1000 || (i && !(i%512)))) {
            e=s->pause(s->pause_arg);
            if (e) return e;
            slice=s->io.now(s->io.ctx);
        }
    }
    return -ETIMEDOUT; /* also bounded with a stopped clock */
}
int video_fault(struct video_native *s, const char *why, int e)
{
    s->quarantined=true; s->ready=false;
    if (s->touched) s->cpu_safe=false;
    s->reason=why; s->pending_record=true;
    if (s->touched && s->hide) (void)s->hide(s);
    return e;
}
int video_sync(void *ctx)
{
    struct video_native *s=ctx;
    if (!s->touched) return s->cpu_safe ? 0 : -EIO;
    s->io.drain(s->io.ctx);
    int e=video_wait(s,s->idle);
    if (e) return video_fault(s,"idle_unproved",e);
    /* Quarantine alone never grants CPU or software-cursor access. */
    if (s->quarantined && s->hide && s->hide(s))
        return video_fault(s,"cursor_disable_unproved",-EIO);
    s->cpu_safe=true;
    return 0;
}
int video_stop(void *ctx)
{
    struct video_native *s=ctx;
    if (!s->touched) return s->cpu_safe ? 0 : -EIO;
    int e=video_sync(s);
    if (!e && s->hide) e=s->hide(s);
    if (!e && s->restore) e=s->restore(s);
    if (e) return video_fault(s,"stop_unproved",e);
    s->ready=false; s->touched=false; s->shape=false;
    s->cpu_safe=true; s->reason=s->quarantined ? s->reason : "stopped";
    s->pending_record=true;
    /* A mode change may enter firmware immediately after stop returns. Revoke
     * access here, while idle is authoritative; deferred record only logs. */
    video_release(s);
    return 0;
}
bool video_cpu_ready(void *ctx) { return ((struct video_native *)ctx)->cpu_safe; }
unsigned video_capabilities(void *ctx)
{
    struct video_native *s=ctx;
    return s->ready && !s->quarantined ? 1u | (s->cursor_available ? 2u : 0) : 0;
}
void video_pause(void *ctx, int (*pause)(void *), void *arg)
{ struct video_native *s=ctx; s->pause=pause; s->pause_arg=arg; }
bool video_rect(const struct video_native *s, const struct fb_rect *r)
{
    return r && r->x>=0 && r->y>=0 && r->width>0 && r->height>0 &&
        (uint64_t)(uint32_t)r->x+(uint32_t)r->width<=s->fb.width &&
        (uint64_t)(uint32_t)r->y+(uint32_t)r->height<=s->fb.height;
}
bool video_format(const struct fb_device *d)
{
    return d && d->present && d->mapped && d->width>=16 && d->height>=16 &&
        d->width<=2048 && d->height<=2048 && d->pitch && !(d->pitch&7) &&
        d->pitch<=8191 && (uint64_t)d->width*(d->bpp/8)<=d->pitch &&
        (uint64_t)d->pitch*d->height<=d->size && d->blue_size==5+(d->bpp==24)*3 &&
        d->blue_pos==0 && ((d->bpp==16 && d->red_size==5 && d->red_pos==11 &&
        d->green_size==6 && d->green_pos==5) || (d->bpp==24 &&
        d->red_size==8 && d->red_pos==16 && d->green_size==8 && d->green_pos==8));
}
/* Legacy opacity/image -> 64x64 MSB-left AND/XOR planes, 8 bytes each/row.
 * Outside the 32x32 shape: AND=1, XOR=0, hence fully transparent. */
void video_cursor_bits(uint8_t out[1024], const struct accel_cursor *c)
{
    for (unsigned y=0;y<64;y++) {
        memset(out+y*16,255,8); memset(out+y*16+8,0,8);
        if (y<32) for (unsigned b=0;b<4;b++) {
            out[y*16+b]=(uint8_t)(~c->mask[y]>>(24-b*8));
            out[y*16+8+b]=(uint8_t)((c->image[y]&c->mask[y])>>(24-b*8));
        }
    }
}
uint32_t video_cursor_rgb(const struct fb_device *d, uint32_t colour)
{
    if (d->bpp==24) return colour&0xffffff;
    unsigned r=(colour>>11)&31, g=(colour>>5)&63, b=colour&31;
    return ((r<<3|r>>2)<<16)|((g<<2|g>>4)<<8)|(b<<3|b>>2);
}
void video_palette_save(struct video_native *s)
{
    s->dac_mask=(uint8_t)video_read(s,8,0x3c6);
    s->dac_index=(uint8_t)video_read(s,8,0x3c8);
    video_write(s,8,0x3c7,4);
    for (unsigned i=0;i<6;i++) s->palette_saved[i]=(uint8_t)video_read(s,8,0x3c9);
    video_write(s,8,0x3c8,s->dac_index);
}
void video_palette_load(struct video_native *s, const uint8_t colours[6])
{
    video_write(s,8,0x3c6,255); video_write(s,8,0x3c8,4);
    for (unsigned i=0;i<6;i++) video_write(s,8,0x3c9,colours[i]);
    video_write(s,8,0x3c6,s->dac_mask); video_write(s,8,0x3c8,s->dac_index);
}
bool video_disjoint(uint32_t a, uint32_t n, uint32_t b, uint32_t m)
{ return (uint64_t)a+n<=b || (uint64_t)b+m<=a; }
bool video_reserved(uint32_t base, uint32_t bytes)
{
    if (!bytes || base<0x100000 || (uint64_t)base+bytes>UINT32_MAX ||
        !video_disjoint(base,bytes,g_boot.kernel_start,g_boot.kernel_end-g_boot.kernel_start)) return false;
    bool covered=false;
    for (unsigned i=0;i<g_boot.e820_count;i++) {
        const struct ciuki_e820 *e=&g_boot.e820[i];
        if (e->type==CBI_E820_RAM && base<e->base+e->length && (uint64_t)base+bytes>e->base) return false;
        if (e->type==2 && base>=e->base && (uint64_t)base+bytes<=e->base+e->length) covered=true;
    }
    return covered;
}
static bool access_idle(int handle, gen_t generation)
{ (void)handle; (void)generation; return true; /* caller already revoked an idle access lease */ }
static void *claim_access(struct video_native *s, enum res_type type, uint32_t base, uint32_t bytes, bool map)
{
    if (!bytes || (uint64_t)base+bytes>UINT32_MAX || s->access_count==ARRAY_SIZE(s->access)) return 0;
    if (type==RES_MMIO) {
        if (base<0x100000 || (base&4095) || (bytes&4095) ||
            !video_disjoint(base,bytes,s->fb.phys,s->fb.size) ||
            !video_disjoint(base,bytes,g_boot.kernel_start,g_boot.kernel_end-g_boot.kernel_start)) return 0;
        for (unsigned i=0;i<g_boot.e820_count;i++) {
            const struct ciuki_e820 *e=&g_boot.e820[i];
            if (e->type==CBI_E820_RAM && base<e->base+e->length && (uint64_t)base+bytes>e->base) return 0;
        }
    }
    int h=registry_claim(type,base,base+bytes,s->id,false);
    if (h<0) return 0;
    struct video_access *a=&s->access[s->access_count++];
    *a=(struct video_access){.handle=h,.generation=registry_get((unsigned)h)->generation,.bytes=bytes};
    if (registry_activate(h,a->generation)) return 0;
    if (type==RES_PORT || !map) return (void *)(uintptr_t)1;
    a->window=vmm_reserve_mmio(bytes);
    a->mapped=a->window ? vmm_replace_mmio(a->window,bytes,base,bytes) : 0;
    return a->mapped;
}
void *video_map(struct video_native *s, enum res_type type, uint32_t base, uint32_t bytes)
{ return claim_access(s,type,base,bytes,true); }
/* VGA legacy I/O as Windows assigns it: 3B0h-3BBh and 3C0h-3DFh, never 3BCh-3BFh
 * (a parallel port lives there). A partial success leaves its lease in the
 * access list; the caller's failure path releases it with video_release. */
bool video_claim_vga_ports(struct video_native *s)
{
    return video_map(s,RES_PORT,CIUKI_VGA_MONO_FIRST,CIUKI_VGA_MONO_END-CIUKI_VGA_MONO_FIRST) &&
           video_map(s,RES_PORT,CIUKI_VGA_COLOR_FIRST,CIUKI_VGA_COLOR_END-CIUKI_VGA_COLOR_FIRST);
}
bool video_claim(struct video_native *s, uint32_t base, uint32_t bytes)
{ return claim_access(s,RES_MMIO,base,bytes,false)!=0; }
void video_release(struct video_native *s)
{
    if (s->touched || !s->cpu_safe) return;
    while (s->access_count) {
        struct video_access *a=&s->access[--s->access_count];
        if (a->mapped) vmm_revoke_mmio(a->mapped,a->bytes);
        int e=registry_quiesce(a->handle,a->generation,access_idle);
        if (!e) e=registry_release(a->handle,a->generation);
        if (e) (void)registry_quarantine(a->handle,a->generation);
        a->mapped=0;
    }
}

/* No VFS I/O in any engine hook. Keep an interrupted legacy marker intact,
 * then persist only this adapter's bounded marker on the settings volume. */
static int marker_nonempty(struct vfs_table *t, const char *path)
{
    struct fat_entry entry;
    int e=vfs_stat(t,path,&entry);
    return !e ? (entry.size ? -EIO : 0) : e==-FS_ENOENT ? 0 : e;
}
static int marker_io(struct video_native *s, struct vfs *vfs, bool clear)
{
    if (!vfs || !vfs->volumes[2] || !vfs->volumes[2]->mounted) return -EIO;
    /* fbdev's single marker worker serializes the one bound adapter. A VFS
     * descriptor table is larger than the kernel stack; never put it there. */
    static struct vfs_table table;
    int e=vfs_table_init(vfs,&table,2);
    if (e) return e;
    if (!clear) {
        e=marker_nonempty(&table,"C:/system/settings/accel.cfg");
        if (!e) e=marker_nonempty(&table,s->marker);
    }
    if (!e && vfs->volumes[2]->readonly) e=-FS_EROFS;
    if (!e) {
        int fd=vfs_open(&table,s->marker,VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0);
        if (fd<0) e=fd;
        else {
            if (clear) e=vfs_truncate(&table,fd,0);
            else {
                size_t done=0, length=strlen(s->id);
                e=vfs_write(&table,fd,s->id,length,&done);
                if (!e && done!=length) e=-EIO;
            }
            if (!e) e=vfs_commit(&table,fd);
            int closed=vfs_close(&table,fd);
            if (!e) e=closed;
        }
    }
    vfs_table_destroy(&table);
    return e;
}
int video_marker_begin(struct video_native *s, struct vfs *vfs)
{
    const char *reason;
    if (display_accel_disabled(&reason) || s->quarantined || s->attempted) return -EIO;
    s->attempted=true;
    int e=marker_io(s,vfs,false);
    s->armed=!e;
    if (e) { s->reason="retained_or_unavailable_marker"; s->pending_record=true; }
    return e;
}
int video_marker_end(struct video_native *s, struct vfs *vfs)
{
    if (!s->armed || !s->ready || s->quarantined || !s->cpu_safe) return -EIO;
    int e=marker_io(s,vfs,true);
    if (!e) s->armed=false;
    return e;
}
int video_marker_failed(struct video_native *s, struct vfs *vfs)
{ (void)vfs; s->armed=false; return 0; /* preserve the committed failure marker */ }
void video_record(struct video_native *s)
{
    if (s->pending_record) {
        s->pending_record=false;
        klog("[accel] driver=%s ready=%u quarantined=%u cpu_ready=%u reason=%s ops=%llu",
             s->id,s->ready,s->quarantined,s->cpu_safe,s->reason,s->operations);
    }
    video_release(s);
}
void video_snapshot(struct video_native *s, struct ciuki_accel_info *out)
{
    memset(out,0,sizeof(*out)); /* legacy driver value stays legacy-only */
    out->capabilities=video_capabilities(s); out->generation=s->fb.generation;
    out->quarantined=s->quarantined; out->operations=s->operations; out->idle_waits=s->waits;
}
bool video_summary(struct video_native *s, bool *ready, bool *used, const char **reason)
{ *ready=s->ready && !s->quarantined; *used=false; *reason=s->reason; return true; }
int video_cursor_test(const struct accel_ops *ops, uint64_t deadline, bool (*cancel)(void *), void *ctx)
{
    if (!ops || !ops->cursor_set || !ops->cursor_move || !ops->cursor_show ||
        !(ops->capabilities(ops->ctx)&2)) return -EOPNOTSUPP;
    struct accel_cursor transparent={0};
    int e=0;
    for (unsigned n=0;n<4 && !e;n++) {
        if (g_ticks>=deadline || (cancel && cancel(ctx))) { e=g_ticks>=deadline ? -ETIMEDOUT : -EINTR; break; }
        if (!n) e=ops->cursor_show(ops->ctx,false);
        else if (n==1) e=ops->cursor_set(ops->ctx,&transparent);
        else if (n==2) e=ops->cursor_move(ops->ctx,0,0);
        else e=ops->cursor_show(ops->ctx,true);
    }
    int hidden=ops->cursor_show(ops->ctx,false);
    return e ? e : hidden;
}
/* Called exclusively through fbdev_accel_probe, before publishing ops. Native
 * functions cannot punt here; saved pixels are restored only after idle proof. */
int video_probe_pixels(void *ctx)
{
    struct video_native *s=ctx;
    const unsigned bytes=s->fb.bpp/8, pitch=16*bytes;
    uint8_t saved[16*16*3], reference[sizeof(saved)];
    struct fb_device ref=s->fb;
    ref.width=ref.height=16; ref.pitch=pitch; ref.size=16*pitch; ref.mapped=reference;
    int e=video_sync(s);
    if (e) return e;
    for (unsigned y=0;y<16;y++) memcpy(saved+y*pitch,s->fb.mapped+y*s->fb.pitch,pitch);
    memcpy(reference,saved,ref.size);
    uint64_t start=s->io.now(s->io.ctx);
    for (unsigned op=0;op<5 && !e;op++) {
        if (s->io.now(s->io.ctx)-start>=100000) { e=-ETIMEDOUT; break; }
        struct fb_rect r={0,0,16,16};
        if (op<2) {
            if (op) r=(struct fb_rect){0,0,6,6};
            uint32_t c=swdib_pack(&s->fb,op ? 0xce7312 : 0x39a6e7);
            swdib_fill(&ref,&r,c); e=s->ops.fill_rect(s,&r,c);
        } else if (op<4) {
            r=(struct fb_rect){op==2 ? 0 : 4,op==2 ? 0 : 4,12,12};
            int dst=op==2 ? 4 : 0;
            swdib_copy(&ref,&r,dst,dst); e=s->ops.copy_rect(s,&r,dst,dst);
        } else if (s->ops.pattern_rect) {
            const uint8_t pattern[8]={0x81,0x42,0x24,0x18,0x18,0x24,0x42,0x81};
            uint32_t fg=swdib_pack(&s->fb,0x57ad23), bg=swdib_pack(&s->fb,0xcc318a);
            swdib_pattern(&ref,&r,pattern,fg,bg);
            e=s->ops.pattern_rect(s,&r,pattern,fg,bg);
        }
        if (!e) e=video_sync(s);
        if (!e) for (unsigned y=0;y<16;y++)
            if (memcmp(reference+y*pitch,s->fb.mapped+y*s->fb.pitch,pitch)) { e=-EIO; break; }
    }
    if (!e && s->cursor_available) e=video_cursor_test(&s->ops,g_ticks+100,NULL,NULL);
    int idle=video_sync(s);
    if (!idle && s->cpu_safe) {
        for (unsigned y=0;y<16;y++) memcpy(s->fb.mapped+y*s->fb.pitch,saved+y*pitch,pitch);
        s->io.drain(s->io.ctx);
    }
    if (e || idle) return video_fault(s,"native_comparison_failed",e ? e : idle);
    s->pending_record=true; s->reason="active";
    return 0;
}
