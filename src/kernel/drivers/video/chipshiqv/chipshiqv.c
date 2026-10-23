/*
 * Copyright 1993 by Jon Block <block@frc.com>
 * Modified by Mike Hollick <hollick@graphics.cis.upenn.edu>
 * Modified 1994 by Régis Cridlig <cridlig@dmi.ens.fr>
 *
 * Major Contributors to XFree86 3.2
 *   Modified 1995/6 by Nozomi Ytow
 *   Modified 1996 by Egbert Eich <eich@xfree86.org>
 *   Modified 1996 by David Bateman <dbateman@club-internet.fr>
 *   Modified 1996 by Xavier Ducoin <xavier@rd.lectra.fr>
 *
 * Contributors to XFree86 3.2
 *   Modified 1995/6 by Ken Raeburn <raeburn@raeburn.org>
 *   Modified 1996 by Shigehiro Nomura <nomura@sm.sony.co.jp>
 *   Modified 1996 by Marc de Courville <marc@courville.org>
 *   Modified 1996 by Adam Sulmicki <adam@cfar.umd.edu>
 *   Modified 1996 by Jens Maurer <jmaurer@cck.uni-kl.de>
 *
 * Large parts rewritten for XFree86 4.0
 *   Modified 1998 by David Bateman <dbateman@club-internet.fr>
 *   Modified 1998 by Egbert Eich <eich@xfree86.org>
 *   Modified 1998 by Nozomi Ytow
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the authors not be used in
 * advertising or publicity pertaining to distribution of the software without
 * specific, written prior permission.  The authors makes no representations
 * about the suitability of this software for any purpose.  It is provided
 * "as is" without express or implied warranty.
 *
 * THE AUTHORS DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Copyright 1994  The XFree86 Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * DAVID WEXELBLAT BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Hardware Cursor for Trident utilizing XAA Cursor code.
 * Written by Alan Hourihane <alanh@fairlite.demon.co.uk>
 * Modified for Chips and Technologies by David Bateman <dbateman@eng.uts.edu.au>
 */
/* Native adapter derived from X.Org xf86-video-chips src/ct_driver.c,
 * ct_cursor.c, revision 9187d76bd4f8d500fc3403b6d4a7bef55508bf2b.
 * Original notices below; CiukiOS resource/lifecycle glue is GPL-2.0-only. */
#include "hiqv.h"
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/mtrr.h>
#include <ciuki/timing.h>
#include "../../../fs/vfs.h"

static const struct display_match hiqv_matches[]={
    {0x102c,0x00e0,3,255}, {0x102c,0x00e4,3,255}, {0x102c,0x00e5,3,255}
};
static struct hiqv_engine engine;
static volatile uint8_t *registers;
static uint8_t *scratch;
static void *register_window, *scratch_window;
static int port_claim=-1, reg_claim=-1, scratch_claim=-1;
static gen_t port_gen, reg_gen, scratch_gen;
static bool attempt_owned, attempt_blocked;
static const char *decline_reason="pending";
static int (*engine_stop)(void *);
static struct vfs_table marker_table;

static bool hiqv_probe(const struct pci_func *p)
{
    if (!p || p->vendor!=0x102c || p->class_code!=3 || (p->header_type&0x7f) ||
        (p->command&3u)!=3u || (p->bar[0]&7u)) return false;
    for (unsigned i=0;i<ARRAY_SIZE(hiqv_matches);i++) if (p->device==hiqv_matches[i].device) return true;
    return false;
}
static bool safe_extent(uint32_t base,uint32_t bytes)
{
    if (base<0x100000 || !bytes || bytes>UINT32_MAX-base) return false;
    uint64_t end=(uint64_t)base+bytes;
    if (base<g_boot.kernel_end && end>g_boot.kernel_start) return false;
    for (unsigned i=0;i<g_boot.e820_count;i++) {
        const struct ciuki_e820 *e=&g_boot.e820[i];
        if (e->type==CBI_E820_RAM && base<e->base+e->length && end>e->base) return false;
    }
    return true;
}
static bool hiqv_eligible(const struct fb_device *fb)
{
    const char *reason;
    if (display_accel_disabled(&reason)) { decline_reason=reason; return false; }
    if (attempt_blocked || engine.quarantined || engine.active || engine.configured) return false;
    const struct display_driver *const table[]={&chipshiqv_driver};
    const struct pci_func *p=0;
    if (!hiqv_mode_supported(fb)) { decline_reason="unsupported_mode"; return false; }
    if (!display_driver_select(fb,table,1,&p) || !p ||
        (fb->phys&0xffffffu) || !safe_extent(fb->phys,0x1000000u) ||
        g_boot.display.vram_bytes>(p->device==0xe0 ? 0x200000u : 0x400000u)) {
        decline_reason="unproved_framebuffer"; return false;
    }
    return true;
}
/* Preserve indexed-port selection in short IRQ sections. No wait or logging
 * is allowed inside these sections. Never use the potentially wedging BR reads. */
static uint8_t read_index(uint16_t port,uint8_t index)
{
    uint32_t flags=irq_save(); uint8_t saved=inb(port);
    outb(port,index); uint8_t value=inb(port+1); outb(port,saved); irq_restore(flags);
    return value;
}
static uint8_t hw_read_xr(void *ctx,uint8_t index)
{ (void)ctx; return read_index(0x3d6,index); }
static void hw_write_xr(void *ctx,uint8_t index,uint8_t value)
{
    (void)ctx;
    uint32_t flags=irq_save(); uint8_t saved=inb(0x3d6);
    outb(0x3d6,index); outb(0x3d7,value); outb(0x3d6,saved); irq_restore(flags);
}
static void hw_write_br(void *ctx,uint32_t reg,uint32_t value)
{ (void)ctx; *(volatile uint32_t *)(registers+reg)=value; }
static void hw_palette(void *ctx,const uint8_t rgb[6])
{
    (void)ctx;
    uint32_t flags=irq_save();
    uint8_t index=inb(0x3d6), dac=inb(0x3c8);
    outb(0x3d6,0x80); uint8_t xr80=inb(0x3d7);
    outb(0x3d7,xr80|1u); outb(0x3c8,4);
    for (unsigned i=0;i<6;i++) outb(0x3c9,(xr80&0x80u) ? rgb[i] : rgb[i]>>2);
    outb(0x3d6,0x80); outb(0x3d7,xr80); outb(0x3c8,dac); outb(0x3d6,index);
    irq_restore(flags);
}
static uint64_t hw_now(void *ctx)
{
    (void)ctx;
    if (!g_cpu_tsc || !g_tsc_per_ms) return g_ticks*1000;
    uint64_t cycles=ktime_cycles();
    return cycles/g_tsc_per_ms*1000 + cycles%g_tsc_per_ms*1000/g_tsc_per_ms;
}
static int hw_delay(void *ctx,uint32_t us) { (void)ctx; return udelay(us); }
static void hw_drain(void *ctx) { (void)ctx; framebuffer_write_drain(); }
static bool hw_valid(void *ctx)
{
    (void)ctx;
    const struct fb_device *fb=fbdev_get();
    const struct pci_func *p=display_driver_owner();
    return registers && hiqv_probe(p) && (p->bar[0]&~15u)==engine.fb.phys &&
        fb->present && fb->mapped==engine.fb.mapped && fb->generation==engine.fb.generation &&
        fb->phys==engine.fb.phys && fb->pitch==engine.fb.pitch && fb->size==engine.fb.size &&
        fb->bpp==engine.fb.bpp && fb->width==engine.fb.width && fb->height==engine.fb.height &&
        registry_valid(fb->handle,fb->generation) && registry_valid(port_claim,port_gen) &&
        registry_valid(reg_claim,reg_gen) && (!scratch || registry_valid(scratch_claim,scratch_gen));
}
static bool release_proof(int handle,gen_t generation)
{
    (void)generation;
    if (!engine.cpu_safe || engine.configured) return false;
    if (handle==reg_claim && registers) { vmm_revoke_mmio((void *)registers,PAGE_SIZE); registers=0; }
    if (handle==scratch_claim && scratch) { vmm_revoke_mmio(scratch,HIQV_SCRATCH_BYTES); scratch=0; }
    return true;
}
static int release_one(int *handle,gen_t generation)
{
    if (*handle<0) return 0;
    const struct resource *r=registry_get((unsigned)*handle);
    if (!r || r->generation!=generation) return -EIO;
    int e=r->state==RS_CLAIMED ? 0 : registry_quiesce(*handle,generation,release_proof);
    if (!e) e=registry_release(*handle,generation);
    if (!e) *handle=-1;
    return e;
}
static int release_resources(void)
{
    int a=release_one(&scratch_claim,scratch_gen),b=release_one(&reg_claim,reg_gen),c=release_one(&port_claim,port_gen);
    return a ? a : b ? b : c;
}
static int claim(enum res_type type,uint32_t begin,uint32_t bytes,int *handle,gen_t *generation)
{
    *handle=registry_claim(type,begin,begin+bytes,"chipshiqv",false);
    if (*handle<0) return -EBUSY;
    const struct resource *r=registry_get((unsigned)*handle);
    if (!r) return -EIO;
    *generation=r->generation;
    return registry_activate(*handle,*generation);
}
static int adapter_stop(void *ctx)
{
    int e=engine_stop(ctx);
    if (!e) e=release_resources();
    return e;
}
static int activation_probe(void *ctx) { return hiqv_engine_check(ctx); }
static const struct accel_ops *hiqv_start(const struct fb_device *fb,struct vfs *vfs)
{
    (void)vfs;
    if (!attempt_owned || !hiqv_eligible(fb) || port_claim>=0 || reg_claim>=0) return 0;
    engine.cpu_safe=true;
    if (claim(RES_PORT,0x3c0,0x20,&port_claim,&port_gen) ||
        claim(RES_MMIO,fb->phys+HIQV_MMIO_OFFSET,PAGE_SIZE,&reg_claim,&reg_gen)) goto refused;
    if (!register_window) register_window=vmm_reserve_mmio(PAGE_SIZE);
    registers=register_window ? vmm_replace_mmio(register_window,PAGE_SIZE,fb->phys+HIQV_MMIO_OFFSET,PAGE_SIZE) : 0;
    if (!registers) goto refused;
    const struct pci_func *p=display_driver_owner();
    if (!p || hw_read_xr(0,0)!=0x2c || hw_read_xr(0,1)!=0x10 ||
        hw_read_xr(0,2)!=(uint8_t)p->device || hw_read_xr(0,3)!=0 ||
        (hw_read_xr(0,0x0a)&0x36u)!=2u) { decline_reason="register_identity_or_mapping"; goto refused; }
    uint32_t offset=PAGE_ALIGN_UP(fb->size);
    /* FR1A includes the internal STN panel buffer. Do not allocate in an
     * unknown firmware reservation, even if VBE reports more total VRAM. */
    if (!(read_index(0x3d0,0x1a)&1u) && offset<=g_boot.display.vram_bytes &&
        HIQV_SCRATCH_BYTES<=g_boot.display.vram_bytes-offset && safe_extent(fb->phys+offset,HIQV_SCRATCH_BYTES)) {
        if (!claim(RES_MMIO,fb->phys+offset,HIQV_SCRATCH_BYTES,&scratch_claim,&scratch_gen)) {
            if (!scratch_window) scratch_window=vmm_reserve_mmio(HIQV_SCRATCH_BYTES);
            scratch=scratch_window ? vmm_replace_mmio(scratch_window,HIQV_SCRATCH_BYTES,fb->phys+offset,HIQV_SCRATCH_BYTES) : 0;
        }
        if (!scratch) release_one(&scratch_claim,scratch_gen);
    }
    const struct hiqv_io io={0,hw_read_xr,hw_write_xr,hw_write_br,hw_palette,hw_now,hw_delay,hw_drain,hw_valid};
    if (hiqv_engine_open(&engine,&io,fb,scratch,offset)) goto refused;
    engine_stop=engine.ops.stop; engine.ops.stop=adapter_stop;
    if (fbdev_accel_probe(fb,activation_probe,&engine)) {
        decline_reason=engine.reason; attempt_blocked=true;
        if (!engine.configured) release_resources();
        return 0;
    }
    decline_reason="none";
    return &engine.ops;
refused:
    if (!strncmp("pending",decline_reason,sizeof("pending"))) decline_reason="resource_unavailable";
    release_resources(); attempt_blocked=true;
    return 0;
}
static const struct accel_ops *hiqv_fallback(void)
{
    if (!engine.configured) {
        (void)hiqv_engine_recover(&engine);
        return engine.cpu_safe ? 0 : &engine.ops;
    }
    if (!engine.quarantined) hiqv_engine_quarantine(&engine,"activation_failed");
    (void)hiqv_engine_recover(&engine);
    return &engine.ops; /* Preserve stop authority and cached CPU-safety proof. */
}
/* Shared legacy marker is intentionally conservative across adapters. Part B
 * owns keyed records/migration. All VFS calls occur outside the presenter. */
static int marker_io(struct vfs *vfs,bool clear)
{
    if (!vfs || !vfs->volumes[2] || !vfs->volumes[2]->mounted || vfs->volumes[2]->readonly) return -EIO;
    int e=vfs_table_init(vfs,&marker_table,2); if (e) return e;
    if (!clear) {
        struct fat_entry entry;
        e=vfs_stat(&marker_table,"C:/system/settings/accel.cfg",&entry);
        if (!e && entry.size) { e=-EIO; decline_reason="previous_attempt"; goto out; }
        if (e && e!=-FS_ENOENT) goto out;
    }
    int fd=vfs_open(&marker_table,"C:/system/settings/accel.cfg",VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0);
    if (fd<0) { e=fd; goto out; }
    if (clear) e=vfs_truncate(&marker_table,fd,0);
    else {
        const char mark[]="accel=attempt\ndriver=chipshiqv\n";
        size_t done=0;
        e=vfs_write(&marker_table,fd,mark,sizeof(mark)-1,&done);
        if (!e && done!=sizeof(mark)-1) e=-EIO;
    }
    if (!e) e=vfs_commit(&marker_table,fd);
    int closed=vfs_close(&marker_table,fd); if (!e) e=closed;
out:
    vfs_table_destroy(&marker_table); return e;
}
static int hiqv_attempt_begin(struct vfs *vfs)
{
    if (!hiqv_eligible(fbdev_get()) || attempt_owned) return -EIO;
    int e=marker_io(vfs,false);
    if (e) { attempt_blocked=true; if (!strncmp(decline_reason,"pending",sizeof("pending"))) decline_reason="marker_unavailable"; }
    else attempt_owned=true;
    return e;
}
static int hiqv_attempt_end(struct vfs *vfs)
{
    if (!attempt_owned || !engine.active || engine.quarantined) return -EIO;
    int e=marker_io(vfs,true);
    if (e) { attempt_blocked=true; decline_reason="marker_clear_failed"; }
    else attempt_owned=false;
    return e;
}
static int hiqv_attempt_failed(struct vfs *vfs)
{ (void)vfs; attempt_blocked=true; attempt_owned=false; return 0; }
static void hiqv_quarantine(const char *reason)
{ attempt_blocked=true; decline_reason=reason; hiqv_engine_quarantine(&engine,reason); }
static bool hiqv_summary(bool *ready,bool *used,const char **reason)
{
    *ready=engine.active && !engine.quarantined; *used=false;
    *reason=engine.quarantined ? engine.reason : decline_reason;
    return true;
}
static void hiqv_snapshot(struct ciuki_accel_info *out)
{
    memset(out,0,sizeof(*out)); out->driver=CIUKI_ACCEL_DRIVER_LEGACY_NATIVE;
    out->capabilities=engine.ops.capabilities ? engine.ops.capabilities(&engine) : 0;
    out->quarantined=engine.quarantined; out->operations=engine.operations;
    out->idle_waits=engine.waits; out->generation=engine.fb.generation;
}
static void hiqv_record(void)
{
    klog("[accel] driver=chipshiqv ready=%u quarantined=%u cpu_ready=%u reason=%s",
         engine.active,engine.quarantined,engine.cpu_safe,engine.quarantined ? engine.reason : decline_reason);
}
const struct display_driver chipshiqv_driver={
    .id="chipshiqv",.name="Chips & Technologies HiQV",.matches=hiqv_matches,
    .attempt_files={"C:/system/settings/accel.cfg"},
    .match_count=ARRAY_SIZE(hiqv_matches),.framebuffer_bar=0,.probe=hiqv_probe,.eligible=hiqv_eligible,
    .start=hiqv_start,.fallback=hiqv_fallback,.attempt_begin=hiqv_attempt_begin,.attempt_end=hiqv_attempt_end,
    .attempt_failed=hiqv_attempt_failed,.quarantine=hiqv_quarantine,.summary=hiqv_summary,
    .snapshot=hiqv_snapshot,.record=hiqv_record,.cursor_test=hiqv_cursor_check
};
