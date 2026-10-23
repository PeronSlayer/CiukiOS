/* Ciuki native driver lifecycle. SPDX-License-Identifier: GPL-2.0-only.
 * Ownership and legacy attempt-marker semantics follow display-driver-model
 * Amendment 1. No imported OS lifecycle or mode-setting code. */
#include "private.h"
#include <ciuki/swdib.h>
#include "../../../fs/vfs.h"

bool s3_eligible(const struct fb_device *f,const struct s3_backend *b)
{
    const char *why;
    const struct pci_func *p=display_driver_owner();
    if (display_accel_disabled(&why) || !f || !f->present || !f->mapped || !b->accept(p) ||
        !f->width || !f->height || f->width>2048 || f->height>2048 ||
        f->width<8 || f->height<4 || !f->pitch || !f->size ||
        f->pitch<(uint64_t)f->width*(f->bpp/8) ||
        (uint64_t)f->pitch*f->height>f->size || !b->format(f)) return false;
    /* Reuse the registry's complete read-only ownership proof, including
     * ambiguity, live framebuffer lease, E820 and enabled PCI decode. */
    const struct display_match m={p->vendor,p->device,3,255};
    const struct display_driver d={.matches=&m,.match_count=1,.framebuffer_bar=0};
    const struct display_driver *table[]={&d};
    return display_driver_select(f,table,1,0)==&d;
}
static int marker(struct s3_device *s,struct vfs *vfs,bool clear,bool *previous)
{
    /* Part A's retained unkeyed marker is deliberately shared. Never clear
     * another driver's interrupted attempt. Keying belongs to part B. */
    if (!vfs || !vfs->volumes[2] || !vfs->volumes[2]->mounted) return -FS_ENOENT;
    /* The VFS table exceeds the bounded kernel frame budget. One marker
     * transaction owns this static workspace, across sleeping disk I/O. */
    static struct vfs_table table;
    static bool busy;
    if (__atomic_exchange_n(&busy,true,__ATOMIC_ACQUIRE)) return -EBUSY;
    int e=vfs_table_init(vfs,&table,2);
    if (e) { __atomic_store_n(&busy,false,__ATOMIC_RELEASE); return e; }
    if (!clear) {
        struct fat_entry entry;
        e=vfs_stat(&table,"C:/system/settings/accel.cfg",&entry);
        if (!e && entry.size) { if (previous) *previous=true; goto done; }
        if (e && e!=-FS_ENOENT) goto done;
    }
    if (vfs->volumes[2]->readonly) { e=-FS_EROFS; goto done; }
    int fd=vfs_open(&table,"C:/system/settings/accel.cfg",VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0);
    if (fd<0) { e=fd; goto done; }
    if (clear) e=vfs_truncate(&table,fd,0);
    else {
        char data[80];
        int length=ksnprintf(data,sizeof(data),"accel=attempt\ndriver=%s\npci=%04x:%04x\n",
            s->id,s->pci.vendor,s->pci.device);
        size_t written=0;
        e=vfs_write(&table,fd,data,(size_t)length,&written);
        if (!e && written!=(size_t)length) e=-FS_EIO;
    }
    if (!e) e=vfs_commit(&table,fd);
    int closed=vfs_close(&table,fd); if (!e) e=closed;
done:
    vfs_table_destroy(&table);
    __atomic_store_n(&busy,false,__ATOMIC_RELEASE);
    return e;
}
int s3_attempt_begin(struct s3_device *s,struct vfs *vfs)
{
    const char *why;
    s->authorized=false;
    if (display_accel_disabled(&why)) { s->reason=why; return -EOPNOTSUPP; }
    if (s->quarantined || s->blocked || s->active) return -EIO;
    const struct pci_func *p=display_driver_owner();
    if (!s->backend->accept(p)) return -ENODEV;
    s->pci=*p;
    bool previous=false;
    int e=marker(s,vfs,false,&previous);
    if (e || previous) {
        s->blocked=true; s->reason=previous ? "previous_attempt" : "marker_unavailable";
        return e ? e : -EIO;
    }
    s->authorized=true;
    return 0;
}
int s3_attempt_end(struct s3_device *s,struct vfs *vfs)
{
    if (!s->active || s->quarantined || !s->cpu_safe) return -EIO;
    return marker(s,vfs,true,0);
}
int s3_attempt_failed(struct s3_device *s,struct vfs *vfs)
{
    (void)vfs;
    s->authorized=false; s->blocked=true;
    /* The committed attempt already preserves failure across boots. */
    return 0;
}
int s3_check_begin(struct s3_device *s,struct vfs *vfs,bool *retained)
{
    if (!retained) return -EINVAL;
    *retained=false;
    if (s->quarantined || s->blocked || s->active) return -EIO;
    int e=s3_attempt_begin(s,vfs);
    *retained=s->reason && !strncmp(s->reason,"previous_attempt",sizeof("previous_attempt"));
    return e;
}
static bool block_equal(struct s3_device *s,const uint8_t *expected)
{
    unsigned row=8*(s->fb.bpp/8);
    for (unsigned y=0;y<4;y++) if (memcmp(s->fb.mapped+y*s->fb.pitch,expected+y*row,row)) return false;
    return true;
}
static int pixel_probe(void *ctx)
{
    struct s3_device *s=ctx;
    int e=s3_engine_init(s); if (e) return e;
    uint8_t saved[96],expected[96];
    unsigned row=8*(s->fb.bpp/8);
    for (unsigned y=0;y<4;y++) memcpy(saved+y*row,s->fb.mapped+y*s->fb.pitch,row);
    memcpy(expected,saved,row*4);
    struct fb_device ref=s->fb;
    ref.width=8; ref.height=4; ref.pitch=row; ref.size=row*4; ref.mapped=expected;
    const struct fb_rect fill={0,0,4,2}, right={0,0,6,3}, left={1,1,6,3};
    uint32_t colour=swdib_pack(&s->fb,0x12c456);
    swdib_fill(&ref,&fill,colour);
    e=s3_fill(s,&fill,colour);
    if (!e && !block_equal(s,expected)) e=-EIO;
    if (!e) {
        swdib_copy(&ref,&right,1,1); e=s3_copy(s,&right,1,1);
        if (!e && !block_equal(s,expected)) e=-EIO;
    }
    if (!e) {
        swdib_copy(&ref,&left,0,0); e=s3_copy(s,&left,0,0);
        if (!e && !block_equal(s,expected)) e=-EIO;
    }
    /* Submitted COPY is never replayed. Restore only this saved test block,
     * and only after a fresh idle proof under the presenter's exclusion. */
    int idle=s3_sync(s);
    if (!idle && s->cpu_safe) {
        for (unsigned y=0;y<4;y++) memcpy(s->fb.mapped+y*s->fb.pitch,saved+y*row,row);
        s->io.drain(s->io.ctx);
    } else e=-EIO;
    if (e) s3_fault(s,"native_probe_failed");
    /* Probe activity is not a completed presenter draw. */
    s->fills=s->copies=0;
    return e;
}
const struct accel_ops *s3_start(struct s3_device *s,const struct fb_device *f,struct vfs *vfs)
{
    if (vfs && s3_attempt_begin(s,vfs)) return 0;
    if (!s->authorized || s->quarantined || s->blocked || !s3_eligible(f,s->backend)) return 0;
    s->authorized=false;
    s->fb=*f;
    /* Install rejecting ops even when access/setup cannot complete. */
    s->ops=(struct accel_ops){.ctx=s,.sync=s3_sync,.stop=s3_stop,.capabilities=s3_caps,
        .cpu_ready=s3_cpu_ready,.set_pause=s3_pause};
    int e=s3_access_begin(s);
    if (e) { s->reason="access_unavailable"; s->blocked=true; return 0; }
    e=fbdev_accel_probe(f,pixel_probe,s);
    if (e) {
        if (!s->quarantined) s3_fault(s,"start_failed");
        return 0;
    }
    if (vfs && s3_attempt_end(s,vfs)) { (void)s3_stop(s); s3_fault(s,"marker_clear_failed"); return 0; }
    return &s->ops;
}
const struct accel_ops *s3_fallback(struct s3_device *s)
{
    if (!s->quarantined && !s->active && !s->touched && !s->io.read) return 0;
    s3_fault(s,s->reason);
    (void)s3_sync(s); /* bounded idle proof; cached guard never waits */
    return &s->ops;
}
void s3_snapshot(struct s3_device *s,struct ciuki_accel_info *i)
{
    memset(i,0,sizeof(*i));
    i->driver=s->active ? CIUKI_ACCEL_DRIVER_LEGACY_NATIVE : CIUKI_ACCEL_DRIVER_NONE;
    i->capabilities=s3_caps(s); i->quarantined=s->quarantined;
    i->generation=s->fb.generation; i->operations=s->fills+s->copies;
}
bool s3_summary(struct s3_device *s,bool *ready,bool *used,const char **why)
{
    *ready=s->active && !s->quarantined; *used=false; *why=s->reason;
    return true;
}
void s3_record(struct s3_device *s)
{
    klog("[accel] driver=%s pci=%04x:%04x active=%u quarantined=%u cpu_ready=%u reason=%s",
        s->id,s->pci.vendor,s->pci.device,s->active,s->quarantined,s->cpu_safe,s->reason);
}
