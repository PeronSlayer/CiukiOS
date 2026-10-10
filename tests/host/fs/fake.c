/* File-backed device with volatile write cache and sector-level undo.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "fake.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
static int rd(struct fake *f, uint64_t lba, void *b) { return pread(f->fd,b,512,(off_t)(lba*512))==512 ? 0 : -FS_EIO; }
static int save(struct fake *f, uint64_t lba) {
    if (!f->undo_enabled) return 0;
    for (unsigned i=0;i<f->undo_count;i++) if (f->undo[i].lba==lba) return 0;
    if (f->undo_count==FAKE_LIMIT) abort();
    struct fake_sector *s=&f->undo[f->undo_count++]; s->lba=lba; return rd(f,lba,s->data);
}
static int wr(struct fake *f, uint64_t lba, const void *b, size_t n) {
    int e=save(f,lba); if (e) return e;
    return pwrite(f->fd,b,n,(off_t)(lba*512))==(ssize_t)n ? 0 : -FS_EIO;
}
int fake_raw_read(struct fake *f, uint64_t lba, void *b) { return rd(f,lba,b); }
int fake_raw_write(struct fake *f, uint64_t lba, const void *b) { return wr(f,lba,b,512); }
static int event(struct fake *, char, uint64_t);
static int drain(struct fake *f, bool loss) {
    /* Deterministic adversarial stable subset: reversed suffix, even-indexed
     * subset, or none. Full flush always drains ALL writes in reverse order. */
    for (unsigned i=f->pending_count;i>0;i--) {
        if (loss && (f->cut_mode==0 || (f->cut_mode==1 && i<=f->pending_count/2) || (f->cut_mode==2 && (i&1)))) continue;
        struct fake_sector *s=&f->pending[i-1]; int e=wr(f,s->lba,s->data,512); if (e) return e;
        if (!loss && (e=event(f,'P',s->lba))) return e;
    }
    f->pending_count=0; return 0;
}
static int event(struct fake *f, char kind, uint64_t lba) {
    if (f->events>=FAKE_TRACE_LIMIT) abort();
    f->trace[f->events]=(struct fake_event){lba,kind}; f->events++;
    if (f->cut_at && f->events==f->cut_at) { if (kind=='W' && drain(f,true)) abort(); f->cut=true; return -FS_EIO; }
    return 0;
}
static int read_dev(struct blkdev *d, uint64_t lba, uint32_t n, void *buf) {
    struct fake *f=d->ctx; int e=blkdev_range(d,lba,n); if (e) return e;
    if (f->cut) return -FS_EIO;
    for (unsigned i=0;i<n;i++) {
        f->reads++; if (lba+i==f->fail_read) return -FS_EIO;
        bool hit=false;
        for (unsigned j=f->pending_count;j>0;j--) if (f->pending[j-1].lba==lba+i) { memcpy((uint8_t *)buf+i*512,f->pending[j-1].data,512); hit=true; break; }
        if (!hit && (e=rd(f,lba+i,(uint8_t *)buf+i*512))) return e;
    }
    return 0;
}
static int write_dev(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf) {
    struct fake *f=d->ctx; int e=blkdev_range(d,lba,n); if (e) return e;
    if (f->cut) return -FS_EIO;
    for (unsigned i=0;i<n;i++) {
        f->writes++; if (lba+i==f->fail_write) return -FS_EIO;
        const uint8_t *b=(const uint8_t *)buf+i*512;
        if (lba+i==f->torn_lba) { if ((e=wr(f,lba+i,b,f->torn_bytes))) return e; f->cut=true; return -FS_EIO; }
        if (f->reorder) {
            /* Device cache coalesces repeated writes to a sector, preserving
             * the latest value even when different sectors are reordered. */
            unsigned j=0; for (;j<f->pending_count;j++) if (f->pending[j].lba==lba+i) break;
            if (j==FAKE_LIMIT) abort(); if (j==f->pending_count) f->pending_count++;
            f->pending[j].lba=lba+i; memcpy(f->pending[j].data,b,512);
        } else if ((e=wr(f,lba+i,b,512))) return e;
        if ((e=event(f,'W',lba+i))) return e;
    }
    return 0;
}
static int flush_dev(struct blkdev *d) {
    struct fake *f=d->ctx; if (f->cut || f->fail_flush) return -FS_EIO;
    int e=drain(f,false); if (e) return e;
    f->flushes++; return event(f,'B',0);
}
int fake_open(struct fake *f, const char *path) {
    memset(f,0,sizeof(*f)); f->fd=open(path,O_RDWR); if (f->fd<0) return -FS_EIO;
    struct stat st; if (fstat(f->fd,&st)) { close(f->fd); return -FS_EIO; }
    f->dev=(struct blkdev){.read=read_dev,.write=write_dev,.flush=flush_dev,.capacity=(uint64_t)st.st_size/512,.sector_size=512,.write_cache_state=BLKDEV_CACHE_ENABLED,.ctx=f};
    f->fail_read=f->fail_write=f->torn_lba=UINT64_MAX;
    f->undo=calloc(FAKE_LIMIT,sizeof(*f->undo)); f->pending=calloc(FAKE_LIMIT,sizeof(*f->pending));
    f->trace=calloc(FAKE_TRACE_LIMIT,sizeof(*f->trace));
    if (!f->undo || !f->pending || !f->trace) abort(); return 0;
}
void fake_close(struct fake *f) { close(f->fd); free(f->undo); free(f->pending); free(f->trace); }
void fake_reset(struct fake *f) {
    for (unsigned i=0;i<f->undo_count;i++) if (pwrite(f->fd,f->undo[i].data,512,(off_t)(f->undo[i].lba*512))!=512) abort();
    f->undo_count=f->pending_count=0; f->events=f->writes=f->reads=f->flushes=0;
    f->cut_at=0; f->cut=false; f->fail_flush=false; f->dev.quarantined=false;
    f->fail_read=f->fail_write=f->torn_lba=UINT64_MAX;
}
void fake_power_loss(struct fake *f) { f->pending_count=0; f->cut=false; f->cut_at=0; f->fail_read=f->fail_write=f->torn_lba=UINT64_MAX; f->fail_flush=false; }
