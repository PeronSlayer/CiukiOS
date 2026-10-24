/* On-disk rules researched in fatgen103; see tests/host/fs/VALIDATION.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "fat.h"
#include <ciuki/hardware_check.h>
#include <ciuki/blackbox.h>
#include <ciuki/bootlog.h>
#pragma weak blackbox_checkpoint
#include "../include/ciuki/fat_native.h"
bool fat_native_ready(void) { return true; }
#define FAT_TX_SECTORS CACHE_RESERVE_MAX
#define FAT_TX_CLUSTERS FAT_TRANSACTION_CLUSTERS
/* f1-89 Amendment 1 item 3: a transaction is built from ordinary 4 KiB
 * pages, never from contiguous frames. Every transaction has the head page
 * below; a mutation adds an entries page, two image pages (16 sectors),
 * their two base and two snapshot pages and a cluster-claim page (nine
 * pages in all); a lookup adds read-copy pages only as it reads. */
struct fat_tx_meta { struct fat_entry original, visible, replacement, unlink_entry; };
#define FAT_TX_READ_PAGES 4u
#define FAT_TX_PAGE_SECTORS 8u
struct fat_transaction {
    struct fat_transaction *next;
    struct fat_volume *source, view;
    struct fat_transaction_guard guard;
    struct fat_entry entry;
    struct fat_entry *target;
    struct fat_entry *replacement_target;
    struct fat_cursor cursor;
    struct fat_tx_meta *meta;
    uint8_t *image_pages[FAT_TX_SECTORS/FAT_TX_PAGE_SECTORS];
    /* Shared (non-fill) images keep the bytes they were built from (base)
     * and receive the pinned bytes at reservation (snap): a data
     * transaction that waited without the turn merges its own nibbles onto
     * what others published meanwhile (f1-89 Amendment 1 item 4). */
    uint8_t *base_pages[FAT_TX_SECTORS/FAT_TX_PAGE_SECTORS];
    uint8_t *snap_pages[FAT_TX_SECTORS/FAT_TX_PAGE_SECTORS];
    uint16_t based;
    uint8_t *read_pages[FAT_TX_READ_PAGES];
    uint32_t *clusters;
    uint64_t sequence, dependency, durable_group;
    uint32_t generation, count, allocated;
    uint64_t sectors[FAT_TX_SECTORS];
    int error;
    bool read_only, visible_pending;
    bool target_initialized;
    void (*on_visible)(void *,const struct fat_entry *);
    void *visible_ctx;
    void (*on_unlinked)(void *,bool);
    struct fat_entry unlink_entry;
    enum fat_transaction_op op;
    bool unlink_pending, unlink_replacement, unlink_notified, rename_notified;
    /* Read-only lookups: every sector they read, with the bytes they saw
     * (copies live in images/snapshots, unused by read-only work). A
     * repeated read sees the same copy. Retirement compares these bytes
     * with the cache under the guard: unrelated publications (file data,
     * other directories, other FAT entries) do not restart a reader. */
    uint32_t read_count;
    bool read_overflow;
    /* LOG_WRITE: DATA into owned reserved clusters only (see transaction
     * run). Independent of every other publication's metadata. */
    bool data_only;
    uint64_t read_sectors[FAT_TX_READ_PAGES*FAT_TX_PAGE_SECTORS];
    /* f1-89 Amendment 1 item 1: a C2 group larger than the 16-sector step
     * is published in chunks that share one cache group (open_group) and
     * are made durable once, by the group's real barrier. fill marks data
     * sectors (new or exclusively owned) that need no read before reserve. */
    uint64_t open_group, last_published;
    bool undurable;
    uint16_t fill;
    /* f1-89 Amendment 1 item 4: a data operation (its own chain and entry,
     * claimed clusters) gives the writer turn up while it waits for device
     * I/O and rebases on resume. free_synced: the view's free count at the
     * last exchange with the volume; publication adds only the difference. */
    bool suspendable;
    uint32_t free_synced, nosuspend;
};
_Static_assert(sizeof(struct fat_transaction)<=4096, "transaction head is one page");
_Static_assert(sizeof(struct fat_tx_meta)<=4096, "transaction entries page");
_Static_assert(FAT_TX_CLUSTERS*sizeof(uint32_t)<=4096, "cluster claims page");
static uint8_t *tx_image(struct fat_transaction *t, unsigned i) {
    return t->image_pages[i/FAT_TX_PAGE_SECTORS]+(size_t)(i%FAT_TX_PAGE_SECTORS)*512;
}
static uint8_t *tx_base(struct fat_transaction *t, unsigned i) {
    return t->base_pages[i/FAT_TX_PAGE_SECTORS]+(size_t)(i%FAT_TX_PAGE_SECTORS)*512;
}
static uint8_t *tx_snap(struct fat_transaction *t, unsigned i) {
    return t->snap_pages[i/FAT_TX_PAGE_SECTORS]+(size_t)(i%FAT_TX_PAGE_SECTORS)*512;
}
static uint8_t *read_copy(struct fat_transaction *t, unsigned i) {
    return t->read_pages[i/FAT_TX_PAGE_SECTORS]+(size_t)(i%FAT_TX_PAGE_SECTORS)*512;
}
static void transaction_free(struct fat_transaction *t) {
    if (!t) return;
    fs_page_free(t->meta); fs_page_free(t->clusters);
    for (unsigned i=0;i<FAT_TX_SECTORS/FAT_TX_PAGE_SECTORS;i++) {
        fs_page_free(t->image_pages[i]); fs_page_free(t->base_pages[i]); fs_page_free(t->snap_pages[i]);
    }
    for (unsigned i=0;i<FAT_TX_READ_PAGES;i++) fs_page_free(t->read_pages[i]);
    fs_page_free(t);
}
static struct fat_transaction *transaction_alloc(bool mutation) {
    struct fat_transaction *t=fs_page_alloc();
    if (!t || !mutation) return t;
    t->meta=fs_page_alloc(); t->clusters=fs_page_alloc();
    for (unsigned i=0;i<FAT_TX_SECTORS/FAT_TX_PAGE_SECTORS;i++) {
        t->image_pages[i]=fs_page_alloc(); t->base_pages[i]=fs_page_alloc(); t->snap_pages[i]=fs_page_alloc();
    }
    bool complete=t->meta && t->clusters;
    for (unsigned i=0;i<FAT_TX_SECTORS/FAT_TX_PAGE_SECTORS;i++)
        complete&=t->image_pages[i] && t->base_pages[i] && t->snap_pages[i];
    if (!complete) { transaction_free(t); return 0; }
    return t;
}
static int transaction_publish(struct fat_volume *, bool durable);
static bool transaction_suspend(struct fat_transaction *);
static void transaction_resume(struct fat_transaction *);
static int transaction_enter(struct fat_transaction *t) {
    int e=t->guard.enter(t->guard.ctx); if (e) return e;
    if ((!t->data_only && t->source->mutation_sequence!=t->sequence) ||
        t->source->dev->media_generation!=t->generation || !t->source->mounted || t->source->dev->quarantined) e=-FS_EAGAIN;
    if (!e && t->source->readonly && !t->read_only) e=-FS_EROFS;
    if (e) t->guard.leave(t->guard.ctx);
    return e;
}
static int transaction_enter_wait(struct fat_transaction *t) {
    int e;
    while ((e=transaction_enter(t))==-FS_EBUSY) {
        if ((e=fs_request_check())) break;
        if (t->guard.wait) { if ((e=t->guard.wait(t->guard.ctx))) break; }
        else fs_service();
    }
    return e;
}
/* The transaction list and in-flight sets are only walked or changed under
 * the namespace turn (claim, publication guard or retirement) or by a legacy
 * caller retaining the old mutation guard, which claim excludes. */
static bool cluster_reserved(struct fat_volume *v, uint32_t c) {
    struct fat_transaction *self=v->transaction;
    struct fat_volume *source=self ? self->source : v;
    for (struct fat_transaction *t=source->transactions;t;t=t->next)
        if (t!=self) for (unsigned i=0;i<t->allocated;i++) if (t->clusters[i]==c) return true;
    return false;
}
static int transaction_claim(struct fat_transaction *t) {
    if (!t->guard.claim) return transaction_enter_wait(t);
    int e;
    while ((e=t->guard.claim(t->guard.ctx))==-FS_EBUSY) {
        if ((e=fs_request_check())) break;
        if (t->guard.wait) { if ((e=t->guard.wait(t->guard.ctx))) break; }
        else fs_service();
    }
    return e;
}
static void transaction_unclaim(struct fat_transaction *t) {
    if (t->guard.claim) t->guard.unclaim(t->guard.ctx); else t->guard.leave(t->guard.ctx);
}
static int transaction_cluster(struct fat_volume *v, uint32_t c) {
    struct fat_transaction *t=v->transaction;
    if (!t) return cluster_reserved(v,c) ? -FS_EBUSY : 0;
    int e=transaction_claim(t); if (e) return e;
    if (cluster_reserved(v,c)) e=-FS_EBUSY;
    else {
        unsigned i=0; while (i<t->allocated && t->clusters[i]!=c) i++;
        if (i==t->allocated) {
            if (i==FAT_TX_CLUSTERS) e=-FS_ENOSPC;
            else t->clusters[t->allocated++]=c;
        }
    }
    transaction_unclaim(t); return e;
}
static uint32_t mask(struct fat_volume *v) { return v->type==12 ? 0xfff : v->type==16 ? 0xffff : 0xfffffff; }
static uint32_t cleanbit(struct fat_volume *v) { return v->type==16 ? 0x8000 : v->type==32 ? 0x8000000 : 0; }
static uint32_t errorbit(struct fat_volume *v) { return cleanbit(v)>>1; }
static bool valid(struct fat_volume *v, uint32_t c) { return c>=2 && c<v->clusters+2; }
static bool eoc(struct fat_volume *v, uint32_t c) { return c>=(mask(v)-7); }
extern void klog(const char *, ...) __attribute__((weak));
static void context(struct fat_volume *v, const char *file) {
    unsigned i=0;
    if (file) for (;i+1<sizeof(v->operation_file) && file[i];i++)
        v->operation_file[i]=(uint8_t)file[i]<=32 ? '_' : file[i];
    v->operation_file[i]=0;
}
const char *fat_readonly_reason(const struct fat_volume *v) {
    if (v->readonly_reason) return v->readonly_reason;
    uint32_t r=v->ro_reasons;
    return r&FAT_RO_CORRUPT ? "backup_bpb" : r&FAT_RO_COPIES ? "fat_copies" :
        r&FAT_RO_DMF ? "dmf_read_only" : r&FAT_RO_WRITE_PROTECTED ? "write_protected" :
        r&FAT_RO_IO_FLAG ? "io_flag" : r&FAT_RO_LOST ? "lost_clusters" :
        r&FAT_RO_WRITE_ERROR ? "write_error" : r&FAT_RO_ACTIVE_ONLY ? "active_fat_only" :
        r&FAT_RO_DURABILITY ? "durability" : r&FAT_RO_DIRTY ? "dirty" : "requested";
}
void fat_revoke_write(struct fat_volume *v, const char *reason, uint32_t cluster) {
    v->readonly=true;
    if (v->readonly_reason) return;
    v->readonly_reason=reason;
    memcpy(v->readonly_file,v->operation_file,sizeof(v->readonly_file));
    v->readonly_cluster=cluster;
    v->diagnostic=reason;
    if (klog) klog("[storage] readonly reason=%s file=%s cluster=%u",
        reason,v->readonly_file[0] ? v->readonly_file : "volume",cluster);
    if (blackbox_checkpoint) blackbox_checkpoint();
}
static int corrupt(struct fat_volume *v, const char *reason, uint32_t cluster) {
    v->ro_reasons|=FAT_RO_CORRUPT;
    fat_revoke_write(v,reason,cluster);
    return -FS_EUCLEAN;
}
struct fat_prepare_cache {
    uint64_t start[3];
    unsigned count[3];
    uint8_t data[3][BLKDEV_READ_BYTES];
};
static uint32_t used_fat_sectors(struct fat_volume *v)
{ return (uint32_t)(((uint64_t)(v->clusters+2)*v->type+4095)/4096); }
static struct fat_prepare_cache *alloc_windows(struct fat_volume *v) {
    struct fat_prepare_cache *p=fs_contig_alloc(sizeof(*p));
    if (!p) {
        v->diagnostic="FAT read windows unavailable";
        if (klog) klog("[storage] mount_memory kind=windows needed_bytes=%u available_bytes=0 error=%d",
            (unsigned)((sizeof(*p)+4095)&~4095u),-FS_ENOMEM);
    }
    return p;
}
static int window_sector(struct fat_volume *v, uint64_t s, const uint8_t **out)
{
    struct fat_prepare_cache *p=v->prepare_cache;
    unsigned slot=2, count=1;
    uint64_t base=s;
    if (v->type && s>=v->reserved && s<v->reserved+(uint64_t)v->fats*v->fat_sectors) {
        slot=(unsigned)((s-v->reserved)/v->fat_sectors);
        uint64_t first=v->reserved+(uint64_t)slot*v->fat_sectors;
        uint64_t end=first+used_fat_sectors(v);
        if (s<end) {
            base=first+((s-first)/BLKDEV_READ_SECTORS)*BLKDEV_READ_SECTORS;
            count=(unsigned)(end-base);
            if (count>BLKDEV_READ_SECTORS) count=BLKDEV_READ_SECTORS;
        }
    }
    if (s<p->start[slot] || s-p->start[slot]>=p->count[slot]) {
        p->count[slot]=0;
        if (hardware_check_io_cancelled && hardware_check_io_cancelled()) return -FS_EAGAIN;
        if (file_read_cancelled && file_read_cancelled()) return -4;
        int e=(v->preparing || v->dev->logical) ? blkdev_read(v->dev,v->start+base,count,p->data[slot]) :
            cache_read_run(v->cache,v->dev,v->start+base,count,p->data[slot]);
        if (e) return e;
        p->start[slot]=base; p->count[slot]=count;
    }
    *out=p->data[slot]+(size_t)(s-p->start[slot])*512;
    return 0;
}
/* f1-93: a logical provider's device call. The attaching VFS may give the
 * caller's volume guard up around it (fat_volume.io_window); the frontier is
 * taken before, under the guard. Unpublished mounts and views call directly. */
struct device_call { struct blkdev *dev; enum blk_operation op; uint64_t lba, frontier; uint32_t count; void *data; };
static int device_call(void *arg) {
    struct device_call *c=arg;
    return c->op==BLK_READ ? blkdev_read(c->dev,c->lba,c->count,c->data) :
        blkdev_request(c->dev,c->op,c->lba,c->count,c->data,BLK_BACKGROUND,c->frontier);
}
static int logical_device(struct fat_volume *v, enum blk_operation op, uint64_t lba, uint32_t count, void *data) {
    struct device_call c={v->dev,op,lba,v->publication_frontier,count,data};
    if (v->io_window && !v->preparing && !v->transaction) return v->io_window(v->io_window_ctx,v,op==BLK_WRITE,device_call,&c);
    return device_call(&c);
}
/* readsec's read-ahead: to the end of the FAT copy, cluster or root
 * directory, at most one 64 KiB run. */
static unsigned readahead_count(const struct fat_volume *v, uint64_t s) {
    uint64_t end=s+1;
    if (v->type && v->fat_sectors && s>=v->reserved && s<v->reserved+(uint64_t)v->fats*v->fat_sectors)
        end=v->reserved+((s-v->reserved)/v->fat_sectors+1)*v->fat_sectors;
    else if (v->spc && s>=v->data_sector)
        end=s+v->spc-(s-v->data_sector)%v->spc;
    else if (v->root_entries && s>=v->root_sector && s<v->data_sector)
        end=v->data_sector;
    if (end>v->sectors) end=v->sectors;
    return (unsigned)(end-s>BLKDEV_READ_SECTORS ? BLKDEV_READ_SECTORS : end-s);
}
static int readsec_device(struct fat_volume *, uint64_t, void *);
static int readsec(struct fat_volume *v, uint64_t s, void *b) {
    int interrupted=fs_request_check(); if (interrupted) return interrupted;
    struct fat_transaction *t=v->transaction;
    if (t && t->read_only) {
        for (unsigned i=0;i<t->read_count;i++)
            if (t->read_sectors[i]==s) { memcpy(b,read_copy(t,i),512); return 0; }
        int e=readsec_device(v,s,b);
        if (!e) {
            unsigned page=t->read_count/FAT_TX_PAGE_SECTORS;
            if (t->read_count<FAT_TX_READ_PAGES*FAT_TX_PAGE_SECTORS && !t->read_pages[page]) t->read_pages[page]=fs_page_alloc();
            if (t->read_count<FAT_TX_READ_PAGES*FAT_TX_PAGE_SECTORS && t->read_pages[page]) {
                t->read_sectors[t->read_count]=s; memcpy(read_copy(t,t->read_count++),b,512);
            } else t->read_overflow=true; /* validated by the sequence only */
        }
        return e;
    }
    if (t) for (unsigned i=0;i<t->count;i++)
        if (t->sectors[i]==s) { memcpy(b,tx_image(t,i),512); return 0; }
    /* A cold read gives the writer turn up (item 4) when every buffered
     * image is owned data (fill): no shared sector image can go stale. */
    if (t && t->suspendable && !t->nosuspend && t->fill==(uint16_t)((1u<<t->count)-1u) &&
        cache_peek(v->cache,v->dev,v->start+s,b) && transaction_suspend(t)) {
        /* The read made without the turn only warms the cache: another
         * writer may publish this sector before we resume, so the bytes we
         * use are read again under the turn (normally a cache hit). */
        int e=readsec_device(v,s,b);
        transaction_resume(t);
        if (!e) e=t->error;
        if (!e) e=readsec_device(v,s,b);
        return e;
    }
    return readsec_device(v,s,b);
}
static int readsec_device(struct fat_volume *v, uint64_t s, void *b) {
    if (v->cache_only) {
        if (s>=v->sectors) return corrupt(v,"sector_bounds",0);
        if (!cache_peek(v->cache,v->dev,v->start+s,b)) return 0;
        if (v->cache_miss==UINT64_MAX) v->cache_miss=s;
        return -FS_EAGAIN;
    }
    /* Four 64 KiB cached runs * 128 us bulk <=512 us. Batching also
     * keeps a 1 us cached FAT decode below 10% checkpoint overhead. */
    static unsigned work;
    if (!(__atomic_add_fetch(&work,1,__ATOMIC_RELAXED) & 3u)) fs_service();
    if (hardware_check_io_cancelled && hardware_check_io_cancelled()) return -FS_EAGAIN;
    if (file_read_cancelled && file_read_cancelled()) return -4; /* EINTR, before issuance */
    if (s>=v->sectors) return corrupt(v,"sector_bounds",0);
    int e;
    if (v->prepare_cache) {
        const uint8_t *p;
        e=window_sector(v,s,&p); if (!e) memcpy(b,p,512);
    } else if (v->preparing) e=blkdev_read(v->dev,v->start+s,1,b);
    else if (v->dev->logical) e=logical_device(v,BLK_READ,v->start+s,1,b);
    else e=cache_read_ahead(v->cache,v->dev,v->start+s,readahead_count(v,s),b);
    if (e && (e==-FS_EQUARANTINED || v->dev->quarantined)) {
        v->ro_reasons|=FAT_RO_WRITE_ERROR;
        fat_revoke_write(v,"device_quarantined",0);
    }
    return e ? (e==-FS_EQUARANTINED ? -FS_EIO : e) : 0;
}
static int write_error(struct fat_volume *v, int e) {
    if (v->transaction && (e==-FS_EAGAIN || e==-FS_ENOSPC || e==-VFS_ECANCELLED || e==-VFS_ETIMEOUT)) return e;
    if (e) {
        v->ro_reasons|=FAT_RO_WRITE_ERROR;
        if (e==-FS_EROFS) v->ro_reasons|=FAT_RO_WRITE_PROTECTED;
        fat_revoke_write(v,e==-FS_EROFS ? "write_protected" : "write_error",0);
        const struct vfs_request *r=fs_request_current();
        if (r && r->cleanup_status && !*r->cleanup_status) *r->cleanup_status=e;
    }
    return e;
}
static int writable(struct fat_volume *v) {
    int e=v->dev->logical ? 0 : cache_error(v->cache,v->dev); if (e) return write_error(v,e);
    if (!v->mounted || v->readonly) return -FS_EROFS;
    if (!v->dev->write) return write_error(v,-FS_EROFS);
    if (!blkdev_durable(v->dev)) return write_error(v,-FS_EIO);
    return 0;
}
static int barrier(struct fat_volume *v);
static int publication_stage(struct fat_volume *v, enum fat_publication_stage stage) {
    if (v->publication_pending && v->publication_stage!=stage) {
        int e=barrier(v); if (e) return e;
    }
    v->publication_stage=stage; return 0;
}
static int publication_begin(struct fat_volume *v) {
    if (v->publication_pending) return 0;
    if (v->publication_count==UINT64_MAX ||
        (v->publication_count && v->publication_generation!=v->dev->media_generation)) return write_error(v,-FS_EIO);
    v->publication_generation=v->dev->media_generation;
    int e=0;
    if (!v->preparing && !v->dev->logical) e=cache_group_begin(v->cache,v->dev,&v->publication_frontier);
    else v->publication_frontier=v->publication_count+1;
    if (e) return write_error(v,e);
    v->publication_group=++v->publication_count; v->publication_pending=true;
    if (v->publication_observer) v->publication_observer(v->publication_ctx,v->publication_group,v->publication_stage,'G',0);
    return 0;
}
/* fx-17b: background free count. One byte per active-FAT sector holds the
 * free entries counted in it; a dirty bit per sector says that sector must
 * be (re)counted. Every publication of an active-FAT sector sets its bit
 * after the new bytes are in the cache; a step clears the bit before it
 * reads, so a count is never older than the last publication it saw. */
struct fat_census {
    uint32_t sectors, cursor, started_ms;
    uint8_t *free;     /* entries free per sector, valid once counted */
    uint32_t *dirty;   /* sectors still to count */
};
static void census_mark(struct fat_volume *v, uint64_t s) {
    struct fat_census *c=v->census;
    if (!c) return;
    uint64_t base=v->reserved+(uint64_t)v->active_fat*v->fat_sectors;
    if (s<base || s-base>=c->sectors) return;
    uint32_t i=(uint32_t)(s-base);
    __atomic_or_fetch(&c->dirty[i/32],1u<<(i%32),__ATOMIC_RELEASE);
}
static int writesec(struct fat_volume *v, uint64_t s, const void *b) {
    int interrupted=fs_request_check(); if (interrupted) return write_error(v,interrupted);
    if (v->transaction) {
        struct fat_transaction *t=v->transaction;
        if (t->error) return t->error;
        if (s>=v->sectors) return -FS_EIO;
        unsigned i=0; while (i<t->count && t->sectors[i]!=s) i++;
        unsigned limit=2*v->cache->dirty_budget/512;
        if (limit>FAT_TX_SECTORS) limit=FAT_TX_SECTORS;
        if (i==limit) {
            /* Not a C2 boundary: publish the chunk, no durability wait. */
            int e=transaction_publish(v,false); if (e) return e; i=0;
        }
        bool fill=v->publication_stage==FAT_P_DATA && s>=v->data_sector;
        if (i==t->count) {
            t->sectors[t->count++]=s;
            if (fill) t->fill|=(uint16_t)(1u<<i); else t->fill&=(uint16_t)~(1u<<i);
            /* The resident bytes this image was built from: the caller read
             * them under the turn, and we have not published over them. */
            if (!cache_peek(v->cache,v->dev,v->start+s,tx_base(t,i))) t->based|=(uint16_t)(1u<<i);
            else t->based&=(uint16_t)~(1u<<i);
        } else if (!fill) t->fill&=(uint16_t)~(1u<<i);
        memcpy(tx_image(t,i),b,512); v->publication_pending=true;
        /* The block queue applies its measured-service command cap. At most
         * 8 KiB of cached images are published in one short CPU-only turn. */
        return 0;
    }
    static unsigned work;
    if (!(__atomic_add_fetch(&work,1,__ATOMIC_RELAXED) & 3u)) fs_service();
    if (s>=v->sectors) return corrupt(v,"sector_bounds",0);
    if (v->mutation_sequence==UINT64_MAX) return write_error(v,-FS_EIO);
    int e=publication_begin(v); if (e) return e;
    if (v->prepare_cache) for (unsigned i=0;i<3;i++)
        if (s>=v->prepare_cache->start[i] && s-v->prepare_cache->start[i]<v->prepare_cache->count[i])
            v->prepare_cache->count[i]=0;
    e=v->preparing ? blkdev_request(v->dev,BLK_WRITE,v->start+s,1,(void *)b,BLK_BACKGROUND,v->publication_frontier) :
        v->dev->logical ? logical_device(v,BLK_WRITE,v->start+s,1,(void *)b) :
        cache_write(v->cache,v->dev,v->start+s,b);
    if (!e) { v->mutation_sequence++; census_mark(v,s); }
    if (e && v->publication_observer) v->publication_observer(v->publication_ctx,v->publication_group,v->publication_stage,'E',e);
    return write_error(v,e);
}
static int barrier(struct fat_volume *v) {
    if (v->transaction) return transaction_publish(v,true);
    int e=fs_request_check();
    if (e) { v->completion.error=e; v->completion.incomplete=true; }
    else if (!v->preparing && !v->dev->logical) e=cache_barrier_record(v->cache,v->dev,&v->completion);
    else {
        e=!blkdev_durable(v->dev) ? -FS_EROFS :
            v->dev->write_cache_state==BLKDEV_CACHE_DISABLED ? 0 :
            v->preparing ? blkdev_request(v->dev,BLK_FLUSH,0,0,0,BLK_BACKGROUND,v->publication_frontier) :
            logical_device(v,BLK_FLUSH,0,0,0);
        v->completion=(struct cache_completion){.group=v->publication_group,
            .generation=v->dev->media_generation,.error=e,.incomplete=e!=0};
    }
    if (v->publication_pending) {
        if (!e) { v->publication_durable=v->publication_count; v->publication_pending=false; }
        if (v->publication_observer) v->publication_observer(v->publication_ctx,v->publication_group,v->publication_stage,e ? 'E' : 'B',e);
    }
    return write_error(v,e);
}
static void identity_changed(struct fat_volume *v, const struct fat_entry *before,
                             const struct fat_entry *after, enum fat_identity_change why) {
    if (v->transaction) {
        struct fat_transaction *t=v->transaction;
        if (why==FAT_ID_UNLINK && t->unlink_notified) { t->unlink_notified=false; return; }
        if (why==FAT_ID_RENAME && t->rename_notified) { t->rename_notified=false; return; }
        int e=transaction_enter_wait(t);
        if (e) { t->error=e; return; }
        if (t->source->identity_changed) t->source->identity_changed(t->source->identity_ctx,t->source,before,after,why);
        if (why==FAT_ID_UNLINK) {
            bool replacement=before==&t->meta->replacement;
            struct fat_entry *target=replacement ? t->replacement_target : t->target;
            if (target) target->index=UINT32_MAX;
            if (!replacement) t->meta->original.index=UINT32_MAX;
            if (t->on_unlinked) t->on_unlinked(t->visible_ctx,replacement);
        }
        t->guard.leave(t->guard.ctx); return;
    }
    if (v->identity_changed) v->identity_changed(v->identity_ctx,v,before,after,why);
}
static uint32_t offset(struct fat_volume *v, uint32_t c) { return v->type==12 ? c+c/2 : c*(v->type/8); }
static int fat_bytes(struct fat_volume *v, unsigned copy, uint32_t off, uint8_t *p, unsigned n, bool wr) {
    uint8_t b[512];
    while (n) {
        uint32_t s=v->reserved+copy*v->fat_sectors+off/512;
        unsigned take=512-off%512; if (take>n) take=n;
        /* A write stores bytes computed from an earlier read (a FAT12 pair
         * shares a nibble with its neighbour): no turn suspension between. */
        if (wr && v->transaction) v->transaction->nosuspend++;
        int e=readsec(v,s,b);
        if (wr && v->transaction) v->transaction->nosuspend--;
        if (e) return e;
        if (wr) { memcpy(b+off%512,p,take); if ((e=writesec(v,s,b))) return e; }
        else memcpy(p,b+off%512,take);
        off+=take; p+=take; n-=take;
    }
    return 0;
}
/* Decode within resident 64 KiB runs. The census and ownership walk do not
 * copy a sector or acquire the shared cache mutex for each FAT entry. FAT12
 * entries spanning sectors retain the byte decoder. */
static int cache_charge(struct fat_volume *v, unsigned units);
static int get_copy(struct fat_volume *v, unsigned copy, uint32_t c, uint32_t *out) {
    /* Cached entry decode: 128 * 1 us = 128 us. This also covers callers
     * whose outer loops skip allocated clusters or follow fragmented links.
     * The volume reservation retains FAT ordering across fs_service.
     * f1-93b: a cache-only section (the caller holds the volume guard) is
     * bounded by work instead and never reaches a preemption point. */
    static unsigned work;
    if (v->cache_only) { int e=cache_charge(v,2); if (e) return e; }
    else if (!(__atomic_add_fetch(&work,1,__ATOMIC_RELAXED) & 127u)) fs_service();
    uint32_t off=offset(v,c), x;
    if (v->prepare_cache && (v->type!=12 || off%512!=511)) {
        const uint8_t *p;struct fat_prepare_cache *w=v->prepare_cache;
        uint64_t sector=v->reserved+(uint64_t)copy*v->fat_sectors+off/512;
        if (sector>=w->start[copy] && sector-w->start[copy]<w->count[copy])
            p=w->data[copy]+(size_t)(sector-w->start[copy])*512;
        else { int e=window_sector(v,sector,&p);if (e) return e; }
        x=v->type==32 ? fs_rd32(p+off%512) : fs_rd16(p+off%512);
    } else {
        uint8_t b[4]; int e=fat_bytes(v,copy,off,b,v->type==32 ? 4 : 2,false);
        if (e) return e;
        x=v->type==32 ? fs_rd32(b) : fs_rd16(b);
    }
    if (v->type==12 && (c&1)) x>>=4;
    *out=x&mask(v); return 0;
}
int fat_get_cluster(struct fat_volume *v, uint32_t c, uint32_t *out) {
    if (c>=v->clusters+2) return corrupt(v,"fat_bounds",c);
    return get_copy(v,v->active_fat,c,out);
}
static int set_cluster(struct fat_volume *v, uint32_t c, uint32_t value) {
    uint8_t b[4]; unsigned n=v->type==32 ? 4 : 2;
    for (unsigned f=0;f<v->fats;f++) {
        int e=fat_bytes(v,f,offset(v,c),b,n,false); if (e) return write_error(v,e);
        uint32_t x=n==4 ? fs_rd32(b) : fs_rd16(b);
        if (v->type==12) x=(c&1) ? (x&15)|(value<<4) : (x&0xf000)|value;
        else if (v->type==32) x=(x&0xf0000000)|value; else x=value;
        if (n==4) fs_wr32(b,x); else fs_wr16(b,(uint16_t)x);
        if ((e=fat_bytes(v,f,offset(v,c),b,n,true))) return e;
    }
    return 0;
}
/* FAT[1] flags: Microsoft FAT specification section 4.2 (contract f1-26).
 * Complete each copy's sector write and barrier before touching the next.
 * A cut between copies remains detectable by the full-sector mount scan.
 * Preserve the hard-error flag and FAT32's reserved upper nibble. */
static int set_clean(struct fat_volume *v, bool clean) {
    if (!cleanbit(v)) return 0;
    int stage_error=publication_stage(v,FAT_P_FLAGS); if (stage_error) return stage_error;
    unsigned n=v->type/8;
    for (unsigned f=0;f<v->fats;f++) {
        uint8_t b[4]; int e=fat_bytes(v,f,offset(v,1),b,n,false);
        if (e) return write_error(v,e);
        uint32_t x=n==4 ? fs_rd32(b) : fs_rd16(b);
        x=clean ? x|cleanbit(v)|errorbit(v) : x&~cleanbit(v);
        if (n==4) fs_wr32(b,x); else fs_wr16(b,(uint16_t)x);
        if ((e=fat_bytes(v,f,offset(v,1),b,n,true)) || (e=barrier(v))) return e;
    }
    return 0;
}
static uint32_t cluster_sector(struct fat_volume *v, uint32_t c) { return v->data_sector+(c-2)*v->spc; }
/* hops links after c (c already validated). */
static int chain_from(struct fat_volume *v, uint32_t c, uint32_t hops, uint32_t *out) {
    for (uint32_t i=0;i<hops;i++) {
        uint32_t n; int e=fat_get_cluster(v,c,&n); if (e) return e;
        if (eoc(v,n)) return -FS_ENOENT;
        if (!valid(v,n)) return corrupt(v,"chain_link",n);
        c=n;
    }
    *out=c; return 0;
}
static int chain_at(struct fat_volume *v, uint32_t first, uint32_t index, uint32_t *out) {
    if (index>=v->clusters || !valid(v,first)) return corrupt(v,"chain_bounds",first);
    return chain_from(v,first,index,out);
}
/* f3-51c chain generations. Issued under the volume reservation that also
 * serializes every lookup and FAT mutation of this volume; a remount starts
 * again from zero, but every description and identity of the former mount
 * already fails with EIO before it could follow a cursor. */
static uint64_t chain_stamp(struct fat_volume *v) {
    /* f1-89: unique and monotonic without a guard. Lookups decode entries
     * outside the publication guard; one guard step per decoded entry was
     * both a reader restart source and a hold per directory slot. */
    struct fat_volume *source=v->transaction ? v->transaction->source : v;
    return __atomic_add_fetch(&source->chain_stamp,1,__ATOMIC_RELAXED);
}
static bool cursor_valid(struct fat_volume *v, const struct fat_entry *file, const struct fat_cursor *c) {
    return c && c->first && c->first==file->first && c->generation==file->chain_generation &&
        c->index<v->clusters && valid(v,c->cluster) && (c->index || c->cluster==c->first);
}
/* Resolve cluster index of a file from the nearest valid pair at or before
 * it: the open file's cursor, the entry's write cursor, else the first
 * cluster (MS-DOS FNDCLUS: from sf_lstclus when the target is not before
 * sf_cluspos, otherwise from sf_firclus). Same bounds/corruption rules. */
static int file_cluster(struct fat_volume *v, const struct fat_entry *file,
                        const struct fat_cursor *cursor, uint32_t index, uint32_t *out) {
    if (index>=v->clusters || !valid(v,file->first)) return corrupt(v,"chain_bounds",file->first);
    uint32_t from=file->first, at=0;
    const struct fat_cursor *hints[2]={cursor,&file->write_cursor};
    for (unsigned i=0;i<2;i++)
        if (cursor_valid(v,file,hints[i]) && hints[i]->index<=index && hints[i]->index>=at) {
            from=hints[i]->cluster; at=hints[i]->index;
        }
    return chain_at(v,from,index-at,out);
}
static void cursor_set(struct fat_cursor *cursor, const struct fat_entry *file, uint32_t index, uint32_t cluster) {
    if (!cursor) return;
    /* fx-16: the leading run belongs to the chain generation, not to the position. */
    uint32_t run0=cursor->generation==file->chain_generation && cursor->first==file->first ? cursor->run0 : 0;
    *cursor=(struct fat_cursor){.generation=file->chain_generation,.first=file->first,.index=index,.cluster=cluster,.run0=run0};
}
/* fx-16: a read seek resolves its cluster in O(1) inside the file's leading
 * contiguous run and learns that run from the walks it already makes.
 * file_cluster follows MS-DOS FNDCLUS: from the open file's cursor when the
 * target is not before it, otherwise from the first cluster. DOOM reads its
 * lumps with lseek+read in WAD order, not file order, so about half of the
 * reads walked the chain from the start: an 11-12 MB DOOM.WAD on a 4 KiB
 * cluster FAT32 is ~3000 links, 1 us each on the T23 (get_copy's own cost
 * note), synchronously inside the guest's INT 21h/IFS call. T23 trial
 * e831e685 BOOT.LOG vm_perf_vector n=6: INT FEh 619 calls, 736 ms in 5 s,
 * while the SB16 IRQ waited up to 69-178 ms (no virtual interrupt is served
 * during a monitor service). Linux keeps per-inode contiguous runs for the
 * same reason (fs/fat/cache.c: fat_cache_lookup, nr_contig). Every chain
 * mutation stamps a new chain_generation (f3-51c), which retires run0. */
static int read_cluster(struct fat_volume *v, const struct fat_entry *file,
                        struct fat_cursor *cursor, uint32_t index, uint32_t *out) {
    if (!cursor || !valid(v,file->first) || index>=v->clusters) return file_cluster(v,file,cursor,index,out);
    if (cursor->generation!=file->chain_generation || cursor->first!=file->first)
        *cursor=(struct fat_cursor){.generation=file->chain_generation,.first=file->first,.index=0,.cluster=file->first};
    uint32_t run=cursor->run0 ? cursor->run0 : 1;
    if (index<run) { *out=file->first+index; return 0; }
    /* Nearest valid pair at or before the target, the run's end included. */
    uint32_t from=file->first+run-1, at=run-1;
    const struct fat_cursor *hints[2]={cursor,&file->write_cursor};
    for (unsigned i=0;i<2;i++)
        if (cursor_valid(v,file,hints[i]) && hints[i]->index<=index && hints[i]->index>at) {
            from=hints[i]->cluster; at=hints[i]->index;
        }
    bool extending=at==run-1;
    uint32_t c=from;
    for (uint32_t i=at;i<index;i++) {
        uint32_t n; int e=fat_get_cluster(v,c,&n); if (e) return e;
        if (eoc(v,n)) return -FS_ENOENT;
        if (!valid(v,n)) return corrupt(v,"chain_link",n);
        if (extending && n==c+1) run=i+2; else extending=false;
        c=n;
    }
    cursor->run0=run;
    *out=c; return 0;
}
static int dir_sector(struct fat_volume *v, uint32_t dir, uint32_t index, uint32_t *sec) {
    if (!dir && v->type!=32) {
        if (index>=v->root_entries) return -FS_ENOENT;
        *sec=v->root_sector+index/16; return 0;
    }
    uint32_t c; int e=chain_at(v,dir ? dir : v->root,index/(16u*v->spc),&c); if (e) return e;
    *sec=cluster_sector(v,c)+(index/16)%v->spc; return 0;
}
static int dir_read(struct fat_volume *v, uint32_t dir, uint32_t i, uint8_t out[32]) {
    uint32_t s; uint8_t b[512]; int e=dir_sector(v,dir,i,&s); if (e) return e;
    if ((e=readsec(v,s,b))) return e; memcpy(out,b+(i%16)*32,32); return 0;
}
static int dir_write(struct fat_volume *v, uint32_t dir, uint32_t i, const uint8_t in[32]) {
    uint32_t s; uint8_t b[512]; int e=dir_sector(v,dir,i,&s); if (e) return e;
    if ((e=readsec(v,s,b))) return write_error(v,e);
    memcpy(b+(i%16)*32,in,32); return writesec(v,s,b);
}
/* f1-93b: one directory walk (a step, a lookup, an alias scan). It keeps
 * the cluster of the last slot range and the last sector read, so a scan
 * costs one FAT decode per cluster and one sector read per 16 slots instead
 * of a chain walk from the start and a sector read for every slot. Only
 * readers use it: nothing the walk itself writes can stale it. */
struct dir_position { uint32_t index, cluster; };
struct dir_walk {
    struct fat_dir_hint *hint;
    uint32_t first;
    /* at[0]: the cluster of the last slot read; at[1]: the one before it,
     * where an entry (LFN slots) that a section did not finish began. */
    struct dir_position at[2];
    unsigned located; /* valid entries of at[] */
    bool buffered;
    uint64_t sector;
    uint8_t data[512];
};
static uint32_t dir_first(struct fat_volume *v, uint32_t dir) { return dir ? dir : v->root; }
static bool fixed_root(struct fat_volume *v, uint32_t dir) { return !dir && v->type!=32; }
static void walk_begin(struct dir_walk *w, struct fat_volume *v, uint32_t dir, struct fat_dir_hint *hint) {
    w->hint=hint; w->located=0; w->buffered=false; w->first=fixed_root(v,dir) ? 0 : dir_first(v,dir);
    if (hint && hint->valid && !fixed_root(v,dir) && hint->first==w->first &&
        hint->sequence==v->mutation_sequence && hint->generation==v->dev->media_generation &&
        hint->index<v->clusters && valid(v,hint->cluster)) {
        w->at[0]=(struct dir_position){hint->index,hint->cluster}; w->located=1;
    }
}
static void walk_end(struct dir_walk *w, struct fat_volume *v) {
    if (!w->hint || !w->located) return;
    const struct dir_position *p=&w->at[w->located-1];
    *w->hint=(struct fat_dir_hint){.sequence=v->mutation_sequence,.generation=v->dev->media_generation,
        .first=w->first,.index=p->index,.cluster=p->cluster,.valid=true};
}
static int walk_sector(struct fat_volume *v, struct dir_walk *w, uint32_t dir, uint32_t index, uint32_t *sec) {
    if (fixed_root(v,dir)) return dir_sector(v,dir,index,sec);
    uint32_t ci=index/(16u*v->spc), c;
    if (ci>=v->clusters) return corrupt(v,"chain_bounds",w->first);
    const struct dir_position *from=0;
    for (unsigned i=0;i<w->located;i++) if (w->at[i].index<=ci && (!from || w->at[i].index>from->index)) from=&w->at[i];
    int e=from ? chain_from(v,from->cluster,ci-from->index,&c) : chain_at(v,w->first,ci,&c);
    if (e) return e;
    if (!w->located || w->at[0].index!=ci) {
        if (w->located) { w->at[1]=w->at[0]; w->located=2; } else w->located=1;
        w->at[0]=(struct dir_position){ci,c};
    }
    *sec=cluster_sector(v,c)+(index/16)%v->spc; return 0;
}
static int walk_read(struct fat_volume *v, struct dir_walk *w, uint32_t dir, uint32_t i, uint8_t out[32]) {
    uint32_t s; int e=walk_sector(v,w,dir,i,&s); if (e) return e;
    if (!w->buffered || w->sector!=s) {
        w->buffered=false;
        if (v->cache_only && v->cache_progress && (e=cache_charge(v,2))) return e;
        if ((e=readsec(v,s,w->data))) return e;
        w->sector=s; w->buffered=true;
        if (v->cache_only) v->cache_progress=true;
    }
    memcpy(out,w->data+(i%16)*32,32); return 0;
}
static uint8_t checksum(const uint8_t *s) {
    uint8_t x=0; for (unsigned i=0;i<11;i++) x=(uint8_t)((x>>1)|((x&1)<<7)) + s[i]; return x;
}
static const uint8_t lfn_offsets[13]={1,3,5,7,9,14,16,18,20,22,24,28,30};
static int alias_decode(const uint8_t *b, char out[40]) {
    uint16_t u[12]; unsigned n=0;
    for (unsigned i=0;i<11;i++) {
        if (i==8 && b[8]!=' ') u[n++]='.';
        if (b[i]==' ') continue;
        uint16_t c=path_from_cp437(i==0 && b[i]==5 ? 0xe5 : b[i]);
        if (c>='A' && c<='Z' && (b[12]&(i<8 ? 8 : 16))) c+=32;
        u[n++]=c;
    }
    return path_from_ucs2(u,n,out,40);
}
static void decode_entry(struct fat_volume *v, const uint8_t b[32], struct fat_entry *e) {
    e->chain_generation=chain_stamp(v); /* f3-51c: a fresh copy never adopts an older cursor */
    e->attr=b[11]; e->first=fs_rd16(b+26); if (v->type==32) e->first|=(uint32_t)fs_rd16(b+20)<<16;
    e->size=fs_rd32(b+28); e->times.create_tenths=b[13]; e->times.create_time=fs_rd16(b+14);
    e->times.create_date=fs_rd16(b+16); e->times.access_date=fs_rd16(b+18);
    e->times.write_time=fs_rd16(b+22); e->times.write_date=fs_rd16(b+24);
}
static void encode_entry(struct fat_volume *v, uint8_t b[32], const struct fat_entry *e) {
    b[11]=e->attr; b[13]=e->times.create_tenths; fs_wr16(b+14,e->times.create_time);
    fs_wr16(b+16,e->times.create_date); fs_wr16(b+18,e->times.access_date);
    fs_wr16(b+20,v->type==32 ? (uint16_t)(e->first>>16) : 0);
    fs_wr16(b+22,e->times.write_time); fs_wr16(b+24,e->times.write_date);
    fs_wr16(b+26,(uint16_t)e->first); fs_wr32(b+28,e->size);
}
/* *settled: the first slot of the entry being decoded (every slot before
 * it is decided), where a cache-only section that stopped resumes. */
static __attribute__((noinline)) int next_entry(struct fat_volume *v, uint32_t dir, uint32_t *cursor,
                                                struct fat_entry *entry, bool labels, struct dir_walk *w, uint32_t *settled) {
    uint16_t name[260]; unsigned expected=0,total=0; uint8_t sum=0; uint32_t start=0;
    uint64_t limit=dir || v->type==32 ? (uint64_t)v->clusters*v->spc*16 : v->root_entries;
    if (limit>UINT32_MAX) limit=UINT32_MAX;
    *settled=*cursor;
    while (*cursor<limit) {
        if (!total) *settled=*cursor;
        uint8_t b[32]; uint32_t i=(*cursor)++; int e=walk_read(v,w,dir,i,b); if (e) return e;
        if (!b[0]) { if (total) v->lfn_orphans++; return -FS_ENOENT; }
        if (b[0]==0xe5) { if (total) v->lfn_orphans++; expected=total=0; continue; }
        if (b[11]==15) {
            unsigned ord=b[0]&31;
            if (b[0]&64) { if (total) v->lfn_orphans++; total=expected=ord; sum=b[13]; start=i; memset(name,0xff,sizeof(name)); }
            if (!ord || ord>20 || ord!=expected || b[0]&(0x80|0x20) || b[12] || fs_rd16(b+26) || b[13]!=sum) { v->lfn_invalid++; expected=total=0; continue; }
            for (unsigned k=0;k<13;k++) name[(ord-1)*13+k]=fs_rd16(b+lfn_offsets[k]);
            expected--; continue;
        }
        if (b[11]&FAT_ATTR_VOLUME) {
            if (total) v->lfn_orphans++; expected=total=0;
            if (!labels) continue;
            memset(entry,0,sizeof(*entry)); entry->parent=dir; entry->index=i; entry->lfn_index=i;
            uint16_t label[11]; unsigned length=11;
            while (length && b[length-1]==' ') length--;
            for (unsigned j=0;j<length;j++) label[j]=path_from_cp437(b[j]);
            if (path_from_ucs2(label,length,entry->alias,sizeof(entry->alias))) return -FS_EILSEQ;
            memcpy(entry->name,entry->alias,strlen(entry->alias)+1); decode_entry(v,b,entry); return 0;
        }
        memset(entry,0,sizeof(*entry)); entry->parent=dir; entry->index=i; entry->lfn_index=i;
        if ((e=alias_decode(b,entry->alias))) return corrupt(v,"directory_alias",dir);
        memcpy(entry->name,entry->alias,strlen(entry->alias)+1);
        if (total && !expected && sum==checksum(b)) {
            unsigned n=0; while (n<total*13 && name[n] && name[n]!=0xffff) n++;
            bool ok=n>0 && n<=255;
            if (n<total*13) { if (name[n]!=0) ok=false; for (unsigned k=n+1;k<total*13;k++) if (name[k]!=0xffff) ok=false; }
            if (ok && !path_from_ucs2(name,n,entry->name,sizeof(entry->name)) && !path_validate_name(entry->name,name,&n)) { entry->lfn_index=start; entry->lfn_count=(uint8_t)total; }
            else { v->lfn_invalid++; memcpy(entry->name,entry->alias,strlen(entry->alias)+1); }
        } else if (total) {
            if (expected) v->lfn_orphans++; else v->lfn_bad_checksum++;
        }
        decode_entry(v,b,entry); return 0;
    }
    if (!dir && v->type!=32) return -FS_ENOENT;
    /* A directory may fill the entire available chain without a 00 slot.
     * Reaching the walk bound is not proof of a cycle: verify its last link.
     * The cursor cannot represent larger directories; that is a capability
     * limit, not evidence of damaged allocation. */
    if (limit==UINT32_MAX) return -FS_EOPNOTSUPP;
    uint32_t c,n,sector;
    int e=walk_sector(v,w,dir,(uint32_t)(limit-1),&sector); /* the walk's last cluster */
    if (e) return e;
    c=w->at[0].cluster;
    if ((e=fat_get_cluster(v,c,&n))) return e;
    return eoc(v,n) ? -FS_ENOENT : corrupt(v,"directory_limit",c);
}
int fat_next(struct fat_volume *v,uint32_t dir,uint32_t *cursor,struct fat_entry *entry) {
    struct dir_walk w; uint32_t settled; walk_begin(&w,v,dir,0);
    return next_entry(v,dir,cursor,entry,false,&w,&settled);
}
int fat_next_dos(struct fat_volume *v,uint32_t dir,uint32_t *cursor,struct fat_entry *entry) {
    struct dir_walk w; uint32_t settled; walk_begin(&w,v,dir,0);
    return next_entry(v,dir,cursor,entry,true,&w,&settled);
}
int fat_lookup(struct fat_volume *v, uint32_t dir, const char *name, struct fat_entry *e) {
    context(v,name);
    struct dir_walk w; walk_begin(&w,v,dir,0);
    uint32_t i=0, settled; int err;
    while (!(err=next_entry(v,dir,&i,e,false,&w,&settled))) if (path_equal(name,e->name) || path_equal(name,e->alias)) return 0;
    return err;
}
static int update(struct fat_volume *v, const struct fat_entry *e) {
    int stage_error=publication_stage(v,FAT_P_DIRECTORY); if (stage_error) return stage_error;
    if (e->index==UINT32_MAX && !(e->attr&FAT_ATTR_DIR)) return 0; /* open-unlinked */
    uint8_t b[32]; int err=dir_read(v,e->parent,e->index,b); if (err) return write_error(v,err);
    encode_entry(v,b,e); if ((err=dir_write(v,e->parent,e->index,b))) return err;
    if (v->transaction) { v->transaction->meta->visible=*e; v->transaction->visible_pending=true; }
    return barrier(v);
}
static int hints(struct fat_volume *v) {
    if (v->type!=32) return 0;
    int stage_error=publication_stage(v,FAT_P_FSINFO); if (stage_error) return stage_error;
    uint8_t b[512]; int e=readsec(v,v->fsinfo,b); if (e) return write_error(v,e);
    /* fx-17b: while the count is unknown FSInfo keeps saying so
     * (FSI_Free_Count 0xFFFFFFFF), never the provisional bound. */
    bool unknown=v->free_unknown || (v->transaction && v->transaction->source->free_unknown);
    fs_wr32(b+488,unknown ? 0xffffffffu : v->free_clusters); fs_wr32(b+492,v->next_free);
    if ((e=writesec(v,v->fsinfo,b))) return e;
    return barrier(v);
}
static int release_chain(struct fat_volume *v, uint32_t c) {
    int stage_error=publication_stage(v,FAT_P_RECLAIM); if (stage_error) return stage_error;
    for (uint32_t i=0;c && i<v->clusters;i++) {
        if (!valid(v,c)) return corrupt(v,"chain_bounds",c);
        uint32_t n; int e=fat_get_cluster(v,c,&n); if (e) return write_error(v,e);
        if (!eoc(v,n) && !valid(v,n)) return corrupt(v,"chain_link",n);
        if ((e=set_cluster(v,c,0))) return e;
        v->free_clusters++; if (c<v->next_free) v->next_free=c;
        if (eoc(v,n)) return barrier(v);
        c=n;
    }
    return c ? corrupt(v,"chain_cycle",c) : 0;
}
static bool split12(struct fat_volume *v, uint32_t c) { return v->type==12 && offset(v,c)%512==511; }
static int allocate(struct fat_volume *v, uint32_t n, bool directory, uint32_t *first) {
    int stage_error=publication_stage(v,FAT_P_CHAIN); if (stage_error) return stage_error;
    *first=0; if (n>v->free_clusters) return -FS_ENOSPC;
    uint32_t tail=0, found=0, c=v->next_free;
    for (uint32_t scanned=0;scanned<v->clusters && found<n;scanned++) {
        if (!valid(v,c)) c=2;
        uint32_t val; int e=fat_get_cluster(v,c,&val); if (e) return write_error(v,e);
        if (!val && !(directory && split12(v,c))) {
            if (v->transaction && v->transaction->allocated==FAT_TX_CLUSTERS) {
                /* allocate() consumes each claim immediately. Once this
                 * detached prefix is durable the FAT itself excludes reuse;
                 * release its in-memory claims before the next bounded set. */
                if ((e=barrier(v))) return e;
                v->transaction->allocated=0;
            }
            e=transaction_cluster(v,c); if (e==-FS_EBUSY) { c++; continue; } if (e) return e;
            if ((e=set_cluster(v,c,mask(v)))) return e;
            if (tail && (e=set_cluster(v,tail,c))) return e;
            if (!*first) *first=c;
            tail=c; found++; v->free_clusters--;
        }
        c++;
    }
    v->next_free=valid(v,c) ? c : 2;
    if (found!=n) { int e=release_chain(v,*first); *first=0; return e ? e : -FS_ENOSPC; }
    return barrier(v);
}
static int zero_chain(struct fat_volume *v, uint32_t c) {
    int stage_error=publication_stage(v,FAT_P_DATA); if (stage_error) return stage_error;
    uint8_t b[512]; memset(b,0,sizeof(b));
    for (uint32_t i=0;i<v->clusters;i++) {
        for (unsigned s=0;s<v->spc;s++) { int e=writesec(v,cluster_sector(v,c)+s,b); if (e) return e; }
        uint32_t n; int e=fat_get_cluster(v,c,&n); if (e) return write_error(v,e);
        if (eoc(v,n)) return barrier(v); if (!valid(v,n)) return corrupt(v,"chain_link",n); c=n;
    }
    return corrupt(v,"chain_cycle",c);
}
static int grow_directory(struct fat_volume *v, uint32_t dir) {
    if (!dir && v->type!=32) return -FS_ENOSPC;
    uint32_t c=dir ? dir : v->root;
    for (uint32_t i=0;i<v->clusters;i++) {
        uint32_t n; int e=fat_get_cluster(v,c,&n); if (e) return e;
        if (eoc(v,n)) {
            /* There is no atomic two-sector update of a live FAT12 link.
             * New directories avoid such clusters. Imported split tails need
             * a future directory COW/recovery design, not an unsafe update. */
            if (split12(v,c)) return -FS_EOPNOTSUPP;
            uint32_t newc; if ((e=allocate(v,1,true,&newc))) return e;
            if ((e=zero_chain(v,newc))) return e;
            if ((e=publication_stage(v,FAT_P_LINK)) || (e=set_cluster(v,c,newc))) return e;
            return barrier(v);
        }
        if (!valid(v,n)) return corrupt(v,"chain_link",n); c=n;
    }
    return corrupt(v,"chain_cycle",c);
}
static int slots(struct fat_volume *v, uint32_t dir, unsigned need, uint32_t *start) {
    uint32_t run=0, first_zero=0; bool end=false;
    uint64_t limit=dir || v->type==32 ? (uint64_t)v->clusters*v->spc*16 : v->root_entries;
    if (limit>UINT32_MAX) limit=UINT32_MAX;
    for (uint32_t i=0;(uint64_t)i<limit;i++) {
        uint8_t b[32]; int e=dir_read(v,dir,i,b);
        if (e==-FS_ENOENT) { if ((e=grow_directory(v,dir))) return e; if ((e=dir_read(v,dir,i,b))) return e; }
        else if (e) return e;
        if (!b[0] && !end) { end=true; first_zero=i; }
        if (end || b[0]==0xe5) {
            if (!run) *start=i;
            if (++run==need) {
                if (end) {
                    /* Bytes after the first 00 entry are unspecified on a
                     * foreign volume. Clear every newly exposed slot and a
                     * new terminator DURABLY while the old terminator still
                     * hides them. Otherwise a crash can expose stale owners. */
                    if ((e=publication_stage(v,FAT_P_DIRECTORY))) return e;
                    uint8_t zero[32]; memset(zero,0,sizeof(zero));
                    for (uint32_t j=first_zero+1;j<=i;j++)
                        if ((e=dir_write(v,dir,j,zero))) return e;
                    uint32_t sector;
                    e=dir_sector(v,dir,i+1,&sector);
                    if (!e) { if ((e=dir_write(v,dir,i+1,zero))) return e; }
                    else if (e!=-FS_ENOENT) return e;
                    if ((e=barrier(v))) return e;
                }
                return 0;
            }
        } else run=0;
    }
    return -FS_ENOSPC;
}
static bool short_ok(uint16_t c) {
    if (c<=32 || c=='.' || c=='"' || c=='*' || c=='+' || c==',' || c=='/' || c==':' || c==';' || c=='<' || c=='=' || c=='>' || c=='?' || c=='[' || c=='\\' || c==']' || c=='|') return false;
    return true;
}
static __attribute__((noinline)) int alias_exists(struct fat_volume *v, uint32_t dir, const uint8_t alias[11], const struct fat_entry *ignore) {
    uint8_t b[32]; memcpy(b,alias,11); b[12]=0; char text[40]; int e=alias_decode(b,text); if (e) return e;
    struct fat_entry *entry=&v->alias_scratch; uint32_t i=0, settled;
    struct dir_walk w; walk_begin(&w,v,dir,0);
    while (!(e=next_entry(v,dir,&i,entry,false,&w,&settled))) {
        if (ignore && entry->index==ignore->index && dir==ignore->parent) continue;
        if (path_equal(text,entry->alias) || path_equal(text,entry->name)) return 1;
    }
    return e==-FS_ENOENT ? 0 : e;
}
static int make_alias(struct fat_volume *v, uint32_t dir, const uint16_t *u, unsigned n, uint8_t alias[11], const struct fat_entry *ignore) {
    uint8_t basis[11]; memset(basis,' ',11); unsigned begin=0,last=n;
    while (begin<n && (u[begin]==' ' || u[begin]=='.')) begin++;
    for (unsigned i=begin;i<n;i++) if (u[i]=='.') last=i;
    bool lossy=begin!=0; unsigned b=0,x=0; bool dot=false;
    for (unsigned i=begin;i<n;i++) {
        if (u[i]=='.') { if (i!=last) lossy=true; dot=true; continue; }
        if (u[i]==' ') { lossy=true; continue; }
        uint16_t upper=path_upper(u[i]); int cp=path_cp437(upper);
        /* A case counterpart absent in CP437 retains the representable glyph.
         * A UCS-2 glyph absent in CP437 is rejected by the F1 contract. */
        if (cp<0) cp=path_cp437(u[i]); if (cp<0) return cp;
        if (!short_ok(upper)) { cp='_'; lossy=true; }
        if (i<last) { if (!dot && b<8) basis[b++]=(uint8_t)cp; else lossy=true; }
        else { if (x<3) basis[8+x++]=(uint8_t)cp; else lossy=true; }
    }
    if (!b) { basis[0]='_'; b=1; lossy=true; }
    if (basis[0]==0xe5) basis[0]=5;
    memcpy(alias,basis,11);
    int e=alias_exists(v,dir,alias,ignore); if (e<0) return e;
    if (!lossy && !e) return 0;
    for (unsigned tail=1;tail<=999999;tail++) {
        char digits[6]; unsigned k=0,t=tail; do { digits[k++]=(char)('0'+t%10); t/=10; } while (t);
        memcpy(alias,basis,11); unsigned keep=b; if (keep>7-k) keep=7-k;
        memset(alias+keep,' ',8-keep); alias[keep++]='~';
        while (k) alias[keep++]=(uint8_t)digits[--k];
        e=alias_exists(v,dir,alias,ignore); if (e<0) return e; if (!e) return 0;
    }
    return -FS_ENOSPC;
}
static int publish(struct fat_volume *v, struct fat_entry *e, const uint16_t *u, unsigned n, const uint8_t alias[11]) {
    int stage_error=publication_stage(v,FAT_P_DIRECTORY); if (stage_error) return stage_error;
    /* LFN entries own no clusters. Make them durable before the sole owning
     * short entry, including when entries span multiple sectors. */
    uint8_t b[32]; uint8_t sum=checksum(alias);
    for (unsigned j=0;j<e->lfn_count;j++) {
        unsigned ord=e->lfn_count-j; memset(b,0,sizeof(b)); b[0]=(uint8_t)(ord|(j==0 ? 64 : 0)); b[11]=15; b[13]=sum;
        for (unsigned k=0;k<13;k++) { unsigned pos=(ord-1)*13+k; fs_wr16(b+lfn_offsets[k],pos<n ? u[pos] : pos==n ? 0 : 0xffff); }
        int err=dir_write(v,e->parent,e->lfn_index+j,b); if (err) return err;
    }
    int err=barrier(v); if (err) return err;
    memset(b,0,sizeof(b)); memcpy(b,alias,11); encode_entry(v,b,e);
    if (v->transaction) {
        alias_decode(b,e->alias); path_from_ucs2(u,n,e->name,sizeof(e->name));
        v->transaction->meta->visible=*e; v->transaction->visible_pending=true;
    }
    if ((err=dir_write(v,e->parent,e->index,b))) return err;
    if ((err=barrier(v))) return err;
    alias_decode(b,e->alias); path_from_ucs2(u,n,e->name,sizeof(e->name)); return 0;
}
static int unlink_entry(struct fat_volume *v, const struct fat_entry *e) {
    int stage_error=publication_stage(v,FAT_P_DELETE); if (stage_error) return stage_error;
    uint8_t b[32]; int err=dir_read(v,e->parent,e->index,b); if (err) return err;
    if (v->transaction) {
        v->transaction->meta->unlink_entry=*e; v->transaction->unlink_pending=true;
        v->transaction->unlink_replacement=e==&v->transaction->meta->replacement;
    }
    b[0]=0xe5; if ((err=dir_write(v,e->parent,e->index,b)) || (err=barrier(v))) return err;
    identity_changed(v,e,0,FAT_ID_UNLINK);
    for (unsigned i=0;i<e->lfn_count;i++) {
        if ((err=dir_read(v,e->parent,e->lfn_index+i,b))) return err;
        b[0]=0xe5; if ((err=dir_write(v,e->parent,e->lfn_index+i,b))) return err;
    }
    return barrier(v);
}
int fat_create(struct fat_volume *v, uint32_t dir, const char *name, uint8_t attr, struct fat_entry *out) {
    context(v,name);
    int e=writable(v); if (e) return e;
    if (attr&~0x37u) return -FS_EINVAL;
    uint16_t u[256]; unsigned n; if ((e=path_validate_name(name,u,&n))) return e;
    e=fat_lookup(v,dir,name,out); if (!e) return -FS_EEXIST; if (e!=-FS_ENOENT) return e;
    uint8_t alias[11]; if ((e=make_alias(v,dir,u,n,alias,0))) return e;
    memset(out,0,sizeof(*out)); out->parent=dir; out->attr=attr; out->chain_generation=chain_stamp(v);
    /* Always preserve exact case in LFN; even a short spelling may mix case. */
    out->lfn_count=(uint8_t)((n+12)/13);
    if ((e=slots(v,dir,out->lfn_count+1,&out->lfn_index))) return e;
    out->index=out->lfn_index+out->lfn_count;
    fs_timestamp(&out->times.create_date,&out->times.create_time,&out->times.create_tenths);
    out->times.write_date=out->times.access_date=out->times.create_date;
    out->times.write_time=out->times.create_time;
    if (attr&FAT_ATTR_DIR) {
        if ((e=allocate(v,1,true,&out->first)) || (e=zero_chain(v,out->first))) return e;
        uint8_t b[32]; struct fat_entry dot=*out; dot.size=0;
        memset(b,0,sizeof(b)); memset(b,' ',11); b[0]='.'; encode_entry(v,b,&dot);
        if ((e=dir_write(v,out->first,0,b))) return e;
        b[1]='.'; dot.first=(dir==v->root || !dir) ? 0 : dir; encode_entry(v,b,&dot);
        if ((e=dir_write(v,out->first,1,b)) || (e=barrier(v))) return e;
    }
    if ((e=publish(v,out,u,n,alias))) return e;
    return hints(v);
}
static int read_span(struct fat_volume *v, const struct fat_entry *file, struct fat_cursor *cursor,
                     uint64_t pos, void *buf, size_t size, size_t *done) {
    context(v,file->name);
    *done=0; if (!buf && size) return -FS_EINVAL;
    if (file->attr&FAT_ATTR_DIR) return -FS_EISDIR;
    if (pos>=file->size || !size) return 0;
    if (size>file->size-pos) size=(size_t)(file->size-pos);
    uint32_t c; uint32_t cluster_bytes=(uint32_t)v->spc*512;
    uint32_t index=(uint32_t)pos/cluster_bytes;
    int e=read_cluster(v,file,cursor,index,&c); if (e) return e;
    unsigned off=(uint32_t)pos%cluster_bytes; uint32_t walked=0;
    while (size) {
        if (off%512 || size<512) {
            uint8_t b[512]; if ((e=readsec(v,cluster_sector(v,c)+off/512,b))) return e;
            size_t n=512-off%512; if (n>size) n=size;
            memcpy((uint8_t *)buf+*done,b+off%512,n); *done+=n; size-=n; off+=(unsigned)n;
        } else {
            uint64_t sector=cluster_sector(v,c)+off/512;
            unsigned limit=(unsigned)(size/512);
            if (limit>BLKDEV_READ_SECTORS) limit=BLKDEV_READ_SECTORS;
            unsigned n=0;
            /* Only adjacent cluster numbers are physically contiguous. Do
             * not read a fragmented successor or bytes beyond the EOF. */
            while (n<limit) {
                unsigned take=(cluster_bytes-off)/512;
                if (take>limit-n) take=limit-n;
                n+=take; off+=take*512;
                if (n==limit) break;
                uint32_t next;
                if ((e=fat_get_cluster(v,c,&next))) return e;
                if (!valid(v,next) || walked+1>=v->clusters) return corrupt(v,"read_chain",next);
                if (next!=c+1) break;
                walked++; index++; c=next; off=0;
            }
            if (file_read_cancelled && file_read_cancelled()) return -4;
            if (sector>=v->sectors || n>v->sectors-sector) return corrupt(v,"read_bounds",c);
            e=v->dev->logical ? logical_device(v,BLK_READ,v->start+sector,n,(uint8_t *)buf+*done) :
                cache_read_run(v->cache,v->dev,v->start+sector,n,(uint8_t *)buf+*done);
            if (e) {
                if (e==-FS_EQUARANTINED || v->dev->quarantined) {
                    v->ro_reasons|=FAT_RO_WRITE_ERROR;
                    fat_revoke_write(v,"device_quarantined",c);
                }
                return e==-FS_EQUARANTINED ? -FS_EIO : e;
            }
            *done+=n*512; size-=n*512;
        }
        if (off==cluster_bytes && size) {
            uint32_t next; if ((e=fat_get_cluster(v,c,&next))) return e;
            if (!valid(v,next) || ++walked>=v->clusters) return corrupt(v,"read_chain",next);
            index++; c=next; off=0;
        }
    }
    cursor_set(cursor,file,index,c); /* the cluster holding the last byte read */
    return 0;
}
int fat_read_cursor(struct fat_volume *v, const struct fat_entry *file, struct fat_cursor *cursor,
                    uint64_t pos, void *buf, size_t size, size_t *done) {
    int e=read_span(v,file,cursor,pos,buf,size,done);
    if (e && cursor) *cursor=(struct fat_cursor){0}; /* cancellation, I/O or corruption */
    return e;
}
int fat_read(struct fat_volume *v, const struct fat_entry *file, uint64_t pos, void *buf, size_t size, size_t *done) {
    return fat_read_cursor(v,file,0,pos,buf,size,done);
}
/* Plan only one contiguous run, bounded by file EOF, FAT links, volume and
 * generation. Metadata lookups retain readsec's bounded existing read-ahead;
 * data is queued, never read speculatively in the demand caller. */
int fat_read_stream(struct fat_volume *v, const struct fat_entry *file, uint64_t pos,
                    void *buf, size_t size, size_t *done, struct cache_stream *stream,
                    struct fat_cursor *cursor) {
    if (!v->cache->adaptive || v->dev->logical) return fat_read_cursor(v,file,cursor,pos,buf,size,done);
    cache_stream_begin(v->cache,stream,v->dev,pos);
    int e=fat_read_cursor(v,file,cursor,pos,buf,size,done);
    uint32_t window=cache_stream_end(stream,pos,*done,!e);
    uint64_t end=pos+*done;
    if (e || !window || end>=file->size || fs_request_check()) return e;
    /* A partially read sector was already demanded. Prefetch only complete
     * sectors, including no bytes beyond the logical EOF. */
    uint64_t first=(end+511)&~(uint64_t)511;
    if (first>=file->size || file->size-first<512) return e;
    uint32_t cb=(uint32_t)v->spc*512, c;
    /* The demand read left the cursor on its last cluster: zero or one link. */
    if (file_cluster(v,file,cursor,(uint32_t)(first/cb),&c)) return e;
    uint32_t off=(uint32_t)(first%cb), count=0, walked=0;
    uint32_t limit=(uint32_t)((file->size-first)/512);
    if (limit>window/512) limit=window/512;
    uint64_t sector=cluster_sector(v,c)+off/512;
    while (count<limit) {
        uint32_t n=(cb-off)/512; if (n>limit-count) n=limit-count;
        count+=n; off+=n*512;
        if (count==limit) break;
        uint32_t next;
        if (fat_get_cluster(v,c,&next) || !valid(v,next) || ++walked>=v->clusters || next!=c+1) break;
        c=next; off=0;
    }
    if (sector<v->sectors && count<=v->sectors-sector)
        cache_stream_queue(stream,v->start+sector,count);
    return e;
}
/* Full-file COW gives one atomic owning-entry switch, including FAT12 packed
 * entries and shrinking. No private data cache, buffers or heap allocation. */
static int replace(struct fat_volume *v, struct fat_entry *file, uint32_t size, uint64_t pos, const void *data, size_t amount) {
    uint32_t cb=(uint32_t)v->spc*512, count=size/cb+(size%cb!=0), first=0;
    int e=allocate(v,count,false,&first); if (e) return e;
    if ((e=publication_stage(v,FAT_P_DATA))) return e;
    uint32_t c=first, oldc=file->first; uint64_t at=0;
    for (uint32_t i=0;i<count;i++) {
        for (unsigned s=0;s<v->spc;s++,at+=512) {
            uint8_t b[512]; memset(b,0,sizeof(b));
            if (at<size && at<file->size) {
                if (!valid(v,oldc)) { e=corrupt(v,"replace_chain",oldc); goto abandon; }
                if ((e=readsec(v,cluster_sector(v,oldc)+s,b))) goto abandon;
                uint64_t keep=file->size<size ? file->size : size;
                if (at+512>keep) memset(b+(size_t)(keep-at),0,(size_t)(at+512-keep));
            }
            uint64_t lo=at>pos ? at : pos, hi=at+512<pos+amount ? at+512 : pos+amount;
            if (lo<hi) memcpy(b+(size_t)(lo-at),(const uint8_t *)data+(size_t)(lo-pos),(size_t)(hi-lo));
            if ((e=writesec(v,cluster_sector(v,c)+s,b))) return e;
        }
        if (i+1<count) {
            uint32_t next;
            if ((e=fat_get_cluster(v,c,&next))) goto abandon;
            if (!valid(v,next)) { e=corrupt(v,"replace_destination",next); goto abandon; } c=next;
            if (at<file->size) {
                if ((e=fat_get_cluster(v,oldc,&next))) goto abandon;
                if (!valid(v,next)) { e=corrupt(v,"replace_source",next); goto abandon; } oldc=next;
            }
        }
    }
    if ((e=barrier(v))) return e;
    struct fat_entry changed=*file; changed.first=first; changed.size=size; changed.attr|=FAT_ATTR_ARCHIVE;
    /* f3-51c: relocation; every cursor of the former chain is now foreign. */
    changed.chain_generation=chain_stamp(v); changed.write_cursor=(struct fat_cursor){0};
    uint8_t tenths; fs_timestamp(&changed.times.write_date,&changed.times.write_time,&tenths);
    if ((e=update(v,&changed))) return e;
    uint32_t old=file->first; *file=changed;
    if ((e=release_chain(v,old))) return e;
    return hints(v);
abandon:
    /* I/O failures revoke writing; no speculative cleanup after an error. */
    return write_error(v,e);
}
/* Select without reserving: the caller holds the namespace lock through all
 * passes. Thus data can be durable BEFORE any new allocation is published.
 * Avoid FAT12 links whose later live extension would require two sectors. */
static int write_free(struct fat_volume *v, uint32_t *cursor, uint32_t *scanned, uint32_t *out) {
    while (*scanned<v->clusters) {
        uint32_t c=*cursor;
        if (!valid(v,c)) c=2;
        *cursor=c+1; (*scanned)++;
        uint32_t n; int e=fat_get_cluster(v,c,&n); if (e) return e;
        if (!n && !split12(v,c)) {
            e=transaction_cluster(v,c); if (e==-FS_EBUSY) continue;
            if (e) return e;
            *out=c; return 0;
        }
    }
    return -FS_ENOSPC;
}
static int write_data(struct fat_volume *v, uint32_t c, uint64_t base,
                      uint64_t begin, uint64_t end, uint64_t pos,
                      const void *data, size_t size, bool fresh) {
    for (uint64_t at=begin&~511ull;at<end;at+=512) {
        uint8_t b[512];
        uint64_t lo=begin>at ? begin : at, hi=end<at+512 ? end : at+512;
        int e;
        if (!fresh && (lo!=at || hi!=at+512)) {
            if ((e=readsec(v,cluster_sector(v,c)+(uint32_t)((at-base)/512),b))) return e;
        } else memset(b,0,sizeof(b));
        memset(b+(size_t)(lo-at),0,(size_t)(hi-lo));
        lo=lo>pos ? lo : pos; hi=hi<pos+size ? hi : pos+size;
        if (lo<hi) memcpy(b+(size_t)(lo-at),(const uint8_t *)data+(size_t)(lo-pos),(size_t)(hi-lo));
        if ((e=writesec(v,cluster_sector(v,c)+(uint32_t)((at-base)/512),b))) return e;
    }
    return 0;
}
static int write_span(struct fat_volume *, struct fat_entry *, struct fat_cursor *, uint64_t, const void *, size_t, uint32_t, size_t *, bool);
int fat_write_cursor(struct fat_volume *v, struct fat_entry *file, struct fat_cursor *cursor,
                     uint64_t pos, const void *data, size_t size, size_t *done) {
    context(v,file->name);
    *done=0; int e=writable(v); if (e) return e;
    if (file->attr&FAT_ATTR_DIR) return -FS_EISDIR;
    if (file->attr&FAT_ATTR_RO) return -FS_EACCES;
    if (!data && size) return -FS_EINVAL;
    if (pos>UINT32_MAX || size>UINT32_MAX-pos) return -FS_EFBIG;
    if (!size) return 0;
    e=write_span(v,file,cursor,pos,data,size,(uint32_t)(pos+size),done,true);
    if (e && cursor) *cursor=(struct fat_cursor){0};
    return e;
}
int fat_write(struct fat_volume *v, struct fat_entry *file, uint64_t pos, const void *data, size_t size, size_t *done) {
    return fat_write_cursor(v,file,0,pos,data,size,done);
}
static int extend(struct fat_volume *v, struct fat_entry *file, uint32_t size, bool initialize) {
    context(v,file->name);
    int e=writable(v); if (e) return e;
    if (file->attr&FAT_ATTR_DIR) return -FS_EISDIR;
    if (file->attr&FAT_ATTR_RO) return -FS_EACCES;
    if (size<=file->size) return size==file->size ? 0 : -FS_EINVAL;
    size_t done;
    return write_span(v,file,0,file->size,0,0,size,&done,initialize);
}
int fat_extend(struct fat_volume *v, struct fat_entry *file, uint32_t size) {
    return extend(v,file,size,true);
}
/* Privileged pagefile reservation: only FAT/length metadata is durable.
 * Its exclusive owner never reads a slot before writing it in this boot.
 * Ordinary append/extension retains zero initialization. */
int fat_reserve(struct fat_volume *v, struct fat_entry *file, uint32_t size) {
    return extend(v,file,size,false);
}
static int write_span(struct fat_volume *v, struct fat_entry *file, struct fat_cursor *cursor,
                      uint64_t pos, const void *data, size_t size, uint32_t end, size_t *done, bool initialize) {
    int e;
    identity_changed(v,file,file,FAT_ID_WRITE);
    if (initialize && (e=publication_stage(v,FAT_P_DATA))) return e;
    uint32_t cb=(uint32_t)v->spc*512;
    uint32_t count=file->size/cb+(file->size%cb!=0), tail=0;
    uint32_t need=end/cb+(end%cb!=0), extra=0;
    if (end>file->size) {
        /* Reuse an owned excess tail left by an interrupted size publish. */
        if (file->first) {
            if (!count) count=1;
            if ((e=file_cluster(v,file,cursor,count-1,&tail))) return e;
            for (;;) {
                uint32_t next; if ((e=fat_get_cluster(v,tail,&next))) return e;
                if (eoc(v,next)) break;
                if (!valid(v,next) || count>=v->clusters) return corrupt(v,"write_chain",next);
                count++; tail=next;
            }
        }
        if (need>count) extra=need-count;
    }
    if (extra>v->free_clusters) return -FS_ENOSPC;
    if (extra && tail && split12(v,tail)) return -FS_EOPNOTSUPP;
    uint32_t free_cursor=v->next_free, scanned=0, c=0;
    uint32_t selected[FAT_TX_CLUSTERS];
    if (v->transaction && extra>FAT_TX_CLUSTERS) return -FS_ENOSPC;
    /* Preflight ENOSPC, including excluded FAT12 split entries, before data. */
    for (uint32_t i=0;i<extra;i++) {
        if ((e=write_free(v,&free_cursor,&scanned,&c))) return e;
        if (v->transaction) selected[i]=c;
    }
    uint64_t begin=pos<file->size ? pos : file->size;
    uint32_t index=(uint32_t)(begin/cb), last=0, last_index=0;
    if (initialize && index<count) {
        if ((e=file_cluster(v,file,cursor,index,&c))) return e;
        while (index<count && begin<end) {
            uint64_t base=(uint64_t)index*cb, stop=base+cb;
            if (stop>end) stop=end;
            if ((e=write_data(v,c,base,begin,stop,pos,data,size,false))) return e;
            last=c; last_index=index; begin=stop; index++;
            if (index<count && begin<end) {
                uint32_t next; if ((e=fat_get_cluster(v,c,&next))) return e;
                if (!valid(v,next)) return corrupt(v,"write_chain",next);
                c=next;
            }
        }
    }
    free_cursor=v->next_free; scanned=0;
    for (uint32_t i=0;initialize && i<extra;i++) {
        if (v->transaction) c=selected[i];
        else if ((e=write_free(v,&free_cursor,&scanned,&c))) return write_error(v,e);
        uint64_t base=(uint64_t)(count+i)*cb;
        uint64_t stop=base+cb; if (stop>end) stop=end;
        /* Slack beyond EOF is not visible. A later extension initializes it
         * then; do not prewrite a 64 KiB cluster for a 4 KiB append. */
        if ((e=write_data(v,c,base,base,stop,pos,data,size,true))) return e;
        last=c; last_index=count+i;
    }
    if (initialize && (e=barrier(v))) return e; /* data before any allocation links */
    if ((e=publication_stage(v,FAT_P_CHAIN))) return e;
    uint32_t first=0, previous=0;
    free_cursor=v->next_free; scanned=0;
    for (uint32_t i=0;i<extra;i++) {
        if (v->transaction) { c=selected[i]; free_cursor=c+1; }
        else if ((e=write_free(v,&free_cursor,&scanned,&c))) return write_error(v,e);
        if ((e=set_cluster(v,c,mask(v)))) return e;
        if (previous && (e=set_cluster(v,previous,c))) return e;
        if (!first) first=c;
        previous=c;
        if (!initialize) { last=c; last_index=count+i; }
        v->free_clusters--;
    }
    if (extra) {
        v->next_free=valid(v,free_cursor) ? free_cursor : 2;
        if ((e=barrier(v))) return e; /* complete detached chain */
        /* f3-51c: the FAT_P_LINK group mutates this file's chain. Stamp the
         * caller's entry first, so even a failed group retires its cursors. */
        file->chain_generation=chain_stamp(v);
        if (tail && ((e=publication_stage(v,FAT_P_LINK)) || (e=set_cluster(v,tail,first)) || (e=barrier(v)))) return e;
    }
    struct fat_entry changed=*file;
    if (!changed.first) changed.first=first;
    if (end>changed.size) changed.size=end;
    changed.attr|=FAT_ATTR_ARCHIVE;
    changed.write_cursor=(struct fat_cursor){0};
    if (last) cursor_set(&changed.write_cursor,&changed,last_index,last);
    uint8_t tenths; fs_timestamp(&changed.times.write_date,&changed.times.write_time,&tenths);
    if ((e=update(v,&changed))) return e; /* chain before directory length */
    *file=changed; *done=size;
    if (cursor) *cursor=changed.write_cursor; /* the last cluster written */
    return extra ? hints(v) : 0;
}
int fat_truncate(struct fat_volume *v, struct fat_entry *file, uint64_t size) {
    context(v,file->name);
    int e=writable(v); if (e) return e;
    if (file->attr&FAT_ATTR_DIR) return -FS_EISDIR; if (file->attr&FAT_ATTR_RO) return -FS_EACCES;
    if (size>UINT32_MAX) return -FS_EFBIG;
    /* A zero-length reserved file may still own its entire crash tail. */
    if (size==file->size && (size || !file->first)) return 0;
    identity_changed(v,file,file,FAT_ID_TRUNCATE);
    return replace(v,file,(uint32_t)size,0,0,0);
}
int fat_write_reserved_cursor(struct fat_volume *v, const struct fat_entry *file, struct fat_cursor *cursor, uint32_t capacity,
                       uint64_t pos, const void *data, size_t size, size_t *done) {
    context(v,file->name);
    *done=0; int e=writable(v); if (e) return e;
    if (file->attr&(FAT_ATTR_RO|FAT_ATTR_DIR)) return -FS_EACCES;
    if ((!data && size) || pos>capacity || size>capacity-pos || (pos&511) || (size&511)) return -FS_EINVAL;
    if (!size) return 0;
    identity_changed(v,file,file,FAT_ID_WRITE);
    if ((e=publication_stage(v,FAT_P_DATA))) return e;
    uint32_t cb=(uint32_t)v->spc*512, c;
    uint32_t index=(uint32_t)pos/cb;
    if ((e=file_cluster(v,file,cursor,index,&c))) return e;
    unsigned off=(uint32_t)pos%cb;
    while (*done<size) {
        if ((e=writesec(v,cluster_sector(v,c)+off/512,(const uint8_t *)data+*done))) return e;
        *done+=512; off+=512;
        if (off==cb && *done<size) {
            uint32_t next; if ((e=fat_get_cluster(v,c,&next))) return e;
            if (!valid(v,next)) return corrupt(v,"reserved_chain",next);
            c=next; index++; off=0;
        }
    }
    /* Only data durability here. FAT, FSInfo and directory sectors are not
     * dirtied. All storage waits occur with IF enabled. */
    e=barrier(v);
    if (!e) cursor_set(cursor,file,index,c);
    else if (cursor) *cursor=(struct fat_cursor){0};
    return e;
}
int fat_write_reserved(struct fat_volume *v, const struct fat_entry *file, uint32_t capacity,
                       uint64_t pos, const void *data, size_t size, size_t *done) {
    return fat_write_reserved_cursor(v,file,0,capacity,pos,data,size,done);
}
int fat_publish_reserved(struct fat_volume *v, struct fat_entry *file, bool finish) {
    context(v,file->name);
    int e=writable(v); if (e) return e;
    if ((e=barrier(v))) return e;
    uint8_t tenths;
    fs_timestamp(&file->times.write_date,&file->times.write_time,&tenths);
    file->attr|=FAT_ATTR_ARCHIVE;
    if (!finish) return update(v,file);
    uint32_t tail=file->first, last=0;
    if (file->size) {
        uint32_t cb=(uint32_t)v->spc*512;
        if ((e=chain_at(v,file->first,(file->size-1)/cb,&last))) return e;
        if ((e=fat_get_cluster(v,last,&tail))) return e;
        if (eoc(v,tail)) tail=0;
        else if (!valid(v,tail)) return corrupt(v,"reserved_tail",tail);
    } else file->first=0;
    /* f3-51c: cutting the reserved tail mutates this file's chain. */
    if (tail) { file->chain_generation=chain_stamp(v); file->write_cursor=(struct fat_cursor){0}; }
    /* Publish the smaller length before unlinking the reserved tail. A
     * crash between these barriers leaves owned excess, never a short chain. */
    if ((e=update(v,file))) return e;
    if (last && tail) {
        if ((e=publication_stage(v,FAT_P_LINK)) || (e=set_cluster(v,last,mask(v))) || (e=barrier(v))) return e;
    }
    if (tail && (e=release_chain(v,tail))) return e;
    return tail ? hints(v) : 0;
}
int fat_remove(struct fat_volume *v, struct fat_entry *file, bool directory) {
    context(v,file->name);
    int e=writable(v); if (e) return e;
    if (!!(file->attr&FAT_ATTR_DIR)!=directory) return directory ? -FS_ENOTDIR : -FS_EISDIR;
    if (file->attr&FAT_ATTR_RO) return -FS_EACCES;
    if (directory) {
        uint32_t i=0; struct fat_entry child;
        while (!(e=fat_next(v,file->first,&i,&child))) if (!path_equal(child.alias,".") && !path_equal(child.alias,"..")) return -FS_ENOTEMPTY;
        if (e!=-FS_ENOENT) return e;
    }
    if (file->index!=UINT32_MAX && (e=unlink_entry(v,file))) return e;
    file->chain_generation=chain_stamp(v); /* f3-51c: the chain is being released */
    if ((e=release_chain(v,file->first))) return e;
    return hints(v);
}
int fat_rename(struct fat_volume *v, struct fat_entry *file, uint32_t parent, const char *name) {
    context(v,file->name);
    int e=writable(v); if (e) return e;
    if (file->attr&FAT_ATTR_RO) return -FS_EACCES;
    uint16_t u[256]; unsigned n; if ((e=path_validate_name(name,u,&n))) return e;
    struct fat_entry dest; e=fat_lookup(v,parent,name,&dest);
    if (!e && !(parent==file->parent && dest.index==file->index)) return -FS_EEXIST;
    if (e && e!=-FS_ENOENT) return e;
    uint8_t alias[11]; if ((e=make_alias(v,parent,u,n,alias,file))) return e;
    dest=*file; dest.parent=parent; dest.lfn_count=(uint8_t)((n+12)/13);
    /* An exact short spelling needs no new LFN slot. Imported BOOT.LOG
     * commonly follows '..' with no deleted slot before its owner. */
    uint8_t short_entry[32]={0}; char short_name[40];
    memcpy(short_entry,alias,11);
    if (!alias_decode(short_entry,short_name) && !strncmp(short_name,name,sizeof(short_name))) dest.lfn_count=0;
    if (parent==file->parent) {
        if ((e=publication_stage(v,FAT_P_RENAME))) return e;
        /* Keep the owning short entry at the SAME index. Extend only into
         * contiguous deleted entries before it; never create two owners. */
        if (dest.index<dest.lfn_count) return -FS_ENOSPC;
        dest.lfn_index=dest.index-dest.lfn_count;
        for (uint32_t i=dest.lfn_index;i<file->lfn_index;i++) {
            uint8_t b[32]; if ((e=dir_read(v,parent,i,b))) return e;
            if (b[0]!=0xe5) return -FS_ENOSPC;
        }
        for (uint32_t i=file->lfn_index;i<dest.lfn_index;i++) {
            uint8_t b[32]; if ((e=dir_read(v,parent,i,b))) return e; b[0]=0xe5;
            if ((e=dir_write(v,parent,i,b))) return e;
        }
    } else {
        if ((e=slots(v,parent,dest.lfn_count+1,&dest.lfn_index))) return e;
        dest.index=dest.lfn_index+dest.lfn_count;
        if ((e=unlink_entry(v,file))) return e;
        if (file->attr&FAT_ATTR_DIR) {
            if ((e=publication_stage(v,FAT_P_RENAME))) return e;
            uint8_t b[32]; if ((e=dir_read(v,file->first,1,b))) return e;
            uint32_t first=(parent==v->root || !parent) ? 0 : parent;
            fs_wr16(b+26,(uint16_t)first); fs_wr16(b+20,v->type==32 ? (uint16_t)(first>>16) : 0);
            if ((e=dir_write(v,file->first,1,b)) || (e=barrier(v))) return e;
        }
    }
    if ((e=publish(v,&dest,u,n,alias))) return e;
    identity_changed(v,file,&dest,FAT_ID_RENAME);
    *file=dest; return hints(v);
}
/* Native replacement reserves alias and destination capacity before either
 * name disappears. Transaction callers pin both namespace identities across
 * the durable tombstone/new-owner steps. On failure mount recovery reclaims
 * detached chains; no intermediate step creates two owners. */
int fat_rename_replace(struct fat_volume *v, struct fat_entry *file,
                       struct fat_entry *replacement, uint32_t parent,
                       const char *name, bool retain_replacement) {
    int e=writable(v); if (e) return e;
    if (file->attr&FAT_ATTR_RO || (replacement && (replacement->attr&FAT_ATTR_RO))) return -FS_EACCES;
    uint16_t u[256]; unsigned n;
    if ((e=path_validate_name(name,u,&n))) return e;
    struct fat_entry existing; e=fat_lookup(v,parent,name,&existing);
    if (!e && !(existing.parent==file->parent && existing.index==file->index) &&
        !(replacement && existing.parent==replacement->parent && existing.index==replacement->index)) return -FS_EEXIST;
    if (e && e!=-FS_ENOENT) return e;
    if (replacement) {
        if (!!(file->attr&FAT_ATTR_DIR)!=!!(replacement->attr&FAT_ATTR_DIR))
            return file->attr&FAT_ATTR_DIR ? -FS_ENOTDIR : -FS_EISDIR;
        if (replacement->attr&FAT_ATTR_DIR) {
            uint32_t cursor=0;
            while (!(e=fat_next(v,replacement->first,&cursor,&existing)))
                if (!path_equal(existing.name,".") && !path_equal(existing.name,"..")) return -FS_ENOTEMPTY;
            if (e!=-FS_ENOENT) return e;
        }
    }
    uint8_t alias[11]; if ((e=make_alias(v,parent,u,n,alias,file))) return e;
    struct fat_entry dest=*file; dest.parent=parent; dest.lfn_count=(uint8_t)((n+12)/13);
    if ((e=slots(v,parent,dest.lfn_count+1,&dest.lfn_index))) return e;
    dest.index=dest.lfn_index+dest.lfn_count;
    if (replacement) {
        if ((e=unlink_entry(v,replacement))) return e;
        replacement->index=UINT32_MAX;
        if (!retain_replacement) {
            uint32_t chain=replacement->first;
            replacement->first=0; replacement->size=0;
            replacement->chain_generation=chain_stamp(v); replacement->write_cursor=(struct fat_cursor){0};
            if ((e=release_chain(v,chain))) return e;
        }
    }
    if ((e=unlink_entry(v,file))) return e;
    if ((file->attr&FAT_ATTR_DIR) && parent!=file->parent) {
        if ((e=publication_stage(v,FAT_P_RENAME))) return e;
        uint8_t b[32]; if ((e=dir_read(v,file->first,1,b))) return e;
        uint32_t first=(parent==v->root || !parent) ? 0 : parent;
        fs_wr16(b+26,(uint16_t)first); fs_wr16(b+20,v->type==32 ? (uint16_t)(first>>16) : 0);
        if ((e=dir_write(v,file->first,1,b)) || (e=barrier(v))) return e;
    }
    if ((e=publish(v,&dest,u,n,alias))) return e;
    memcpy(dest.name,name,strlen(name)+1);
    uint8_t raw[32]={0}; memcpy(raw,alias,11); alias_decode(raw,dest.alias);
    identity_changed(v,file,&dest,FAT_ID_RENAME);
    *file=dest;
    return hints(v);
}
int fat_set_metadata(struct fat_volume *v, struct fat_entry *file, uint8_t attr, const struct fat_times *times) {
    context(v,file->name);
    int e=writable(v); if (e) return e;
    if ((attr&~0x37u) || ((attr^file->attr)&FAT_ATTR_DIR)) return -FS_EINVAL;
    struct fat_entry changed=*file; changed.attr=attr; if (times) changed.times=*times;
    e=update(v,&changed); if (!e) *file=changed; return e;
}
/* G6: the volume label (Microsoft FAT 1.03: BS_VolLab at boot sector offset
 * 43, or 71 on FAT32, and the root-directory entry with ATTR_VOLUME_ID, the
 * one Windows reads). label is 11 bytes, space padded, already validated
 * and upper case; all spaces removes the label. Both copies are written
 * in one publication; the FAT32 backup boot sector follows the primary. */
int fat_set_label(struct fat_volume *v, const char label[11]) {
    int e=writable(v); if (e) return e;
    bool empty=true; for (unsigned k=0;k<11;k++) if (label[k]!=' ') empty=false;
    if ((e=publication_stage(v,FAT_P_DIRECTORY))) return e;
    uint8_t b[32]; uint32_t found=UINT32_MAX;
    uint64_t limit=v->type==32 ? (uint64_t)v->clusters*v->spc*16 : v->root_entries;
    if (limit>UINT32_MAX) limit=UINT32_MAX;
    struct dir_walk walk; walk_begin(&walk,v,0,0);
    for (uint32_t i=0;i<(uint32_t)limit;i++) {
        if (!(i&127u)) fs_service();
        e=walk_read(v,&walk,0,i,b);
        if (e==-FS_ENOENT || (!e && !b[0])) break;
        if (e) return write_error(v,e);
        if (b[0]!=0xe5 && b[11]!=15 && (b[11]&(FAT_ATTR_VOLUME|FAT_ATTR_DIR))==FAT_ATTR_VOLUME) { found=i; break; }
    }
    uint8_t tenths; uint16_t date,time; fs_timestamp(&date,&time,&tenths);
    if (found!=UINT32_MAX) {
        if ((e=dir_read(v,0,found,b))) return write_error(v,e);
        if (empty) b[0]=0xe5; else memcpy(b,label,11);
        fs_wr16(b+22,time); fs_wr16(b+24,date);
        if ((e=dir_write(v,0,found,b))) return e;
    } else if (!empty) {
        uint32_t slot; if ((e=slots(v,0,1,&slot))) return e;
        memset(b,0,sizeof(b)); memcpy(b,label,11); b[11]=FAT_ATTR_VOLUME;
        fs_wr16(b+22,time); fs_wr16(b+24,date);
        if ((e=dir_write(v,0,slot,b))) return e;
    }
    unsigned ext=v->type==32 ? 64 : 36;
    uint8_t sector[512];
    for (unsigned copy=0;copy<2;copy++) {
        uint32_t at=copy ? (v->type==32 ? v->backup : 0) : 0;
        if (copy && (!at || at>=v->reserved)) break;
        if ((e=readsec(v,at,sector))) return write_error(v,e);
        if (sector[ext+2]!=0x29) break; /* no extended BPB: no BS_VolLab */
        memcpy(sector+ext+7,empty ? "NO NAME    " : label,11);
        if ((e=writesec(v,at,sector))) return e;
    }
    for (unsigned k=0;k<11;k++) v->volume_label[k]=label[k];
    v->volume_label[11]=0;
    for (unsigned k=11;k && v->volume_label[k-1]==' ';k--) v->volume_label[k-1]=0;
    return barrier(v);
}
/* One ownership bit per cluster, at most 1 MiB of ordinary PMM pages.
 * A failed allocation falls back to the existing reserved cache workspace;
 * every pass revisits all chains, so cross-links spanning passes remain
 * detectable. No reclamation is issued until ALL validation passes finish. */
#define SCAN_PAGES 256u
/* Enough frames for every path representable by the public path contract
 * (one-character components plus separators), including the root frame. */
#define SCAN_DEPTH ((FS_PATH_CHARS+1u)/2u+1u)
struct fat_scan_state {
    void *pages[SCAN_PAGES];
    uint32_t page_count, begin, end, log_eof[3];
    bool workspace;
};
static bool scan_owned(struct fat_volume *v, struct fat_scan_state *s, uint32_t c, bool set) {
    if (c<s->begin || c>=s->end) return false;
    uint32_t off=(c-s->begin)/8;
    uint8_t *b=s->workspace ? cache_workspace_byte(v->cache,off) :
        (uint8_t *)s->pages[off/4096]+off%4096;
    uint8_t bit=(uint8_t)(1u<<(c%8)); bool old=(*b&bit)!=0;
    if (set) *b|=bit;
    return old;
}
static void scan_release(struct fat_volume *v, struct fat_scan_state *s) {
    if (s->workspace) cache_workspace_end(v->cache);
    for (unsigned i=0;i<s->page_count;i++) { fs_page_free(s->pages[i]); fs_service(); }
}
static int scan_memory(struct fat_volume *v, struct fat_scan_state *s) {
    v->scan_needed_bytes=(v->scan_extent+7)/8;
    uint32_t pages=(v->scan_needed_bytes+4095)/4096;
    if (pages>SCAN_PAGES) pages=SCAN_PAGES;
    for (;s->page_count<pages;s->page_count++) {
        fs_service(); /* <=18 us allocation/zero per private scan page */
        s->pages[s->page_count]=fs_normal_page_alloc();
        if (!s->pages[s->page_count]) break;
    }
    if (s->page_count==pages) { v->scan_available_bytes=pages*4096; return 0; }
    for (unsigned i=0;i<s->page_count;i++) { fs_page_free(s->pages[i]); fs_service(); }
    s->page_count=0;
    v->scan_available_bytes=v->cache->workspace_pages*4096;
    int e=cache_workspace_begin(v->cache,v->scan_available_bytes);
    if (!e && v->scan_available_bytes) s->workspace=true;
    else { if (!e) cache_workspace_end(v->cache); v->scan_available_bytes=0; }
    if (klog) klog("[storage] scan_memory needed_bytes=%u available_bytes=%u budget_bytes=%u fallback=passes error=%d",
        v->scan_needed_bytes,v->scan_available_bytes,SCAN_PAGES*4096,s->workspace ? 0 : e ? e : -FS_ENOMEM);
    if (!s->workspace) { v->diagnostic="ownership bitmap unavailable"; return e ? e : -FS_ENOMEM; }
    return 0;
}
static void scan_reset(struct fat_volume *v, struct fat_scan_state *s, uint32_t begin) {
    s->begin=begin;
    uint32_t count=v->scan_available_bytes*8;
    s->end=v->scan_extent-begin<count ? v->scan_extent : begin+count;
    uint32_t pages=((s->end-begin+7)/8+4095)/4096;
    for (unsigned i=0;i<pages;i++) {
        void *p=s->workspace ? v->cache->workspace[i] : s->pages[i];
        memset(p,0,4096);
        fs_service(); /* 8 us per completed clear */
    }
    memset(s->log_eof,0,sizeof(s->log_eof));
}
static int scan_pair(struct fat_volume *v, uint32_t c, uint32_t *a, uint32_t *b) {
    int e=get_copy(v,v->active_fat,c,a); if (e) return e;
    *b=*a;
    if (v->fats==2 && !(v->ro_reasons&FAT_RO_ACTIVE_ONLY)) return get_copy(v,1,c,b);
    return 0;
}
static bool eof_detach(struct fat_volume *v, uint32_t c, uint32_t a, uint32_t b) {
    return !split12(v,c) && ((eoc(v,a) && (eoc(v,b)||valid(v,b))) || (eoc(v,b) && valid(v,a)));
}
static int scan_chain(struct fat_volume *v, struct fat_scan_state *s, uint32_t first, uint32_t size, bool dir) {
    if (!first) return size || dir ? corrupt(v,"missing_chain",first) : 0;
    uint32_t cb=(uint32_t)v->spc*512, needed=size/cb+(size%cb!=0);
    uint32_t c=first, anchor=first, power=1, distance=0;
    for (uint32_t length=0;length<v->clusters;length++) {
        if (!(length&127)) fs_service();
        if (!valid(v,c) || scan_owned(v,s,c,true)) return corrupt(v,"chain_owner",c);
        uint32_t n; int e=fat_get_cluster(v,c,&n); if (e) return e;
        if (c>=s->begin && c<s->end) {
            uint32_t other;
            if ((e=scan_pair(v,c,&n,&other))) return e;
            if (n!=other) {
                /* The file length identifies EOF during this traversal;
                 * no second per-cluster bitmap is needed. Every other
                 * owned disagreement, including excess-tail links, gates writes. */
                if (!dir && length+1==needed && eof_detach(v,c,n,other)) v->ro_reasons|=FAT_RO_DIRTY;
                else { v->ro_reasons|=FAT_RO_COPIES;fat_revoke_write(v,"fat_copies",c); }
            }
        }
        if (eoc(v,n)) {
            if ((uint64_t)(length+1)*cb<size) return corrupt(v,"short_chain",c);
            if (!dir && length+1>needed) v->ro_reasons|=FAT_RO_DIRTY;
            return 0;
        }
        if (!valid(v,n)) return corrupt(v,"chain_link",n);
        /* Brent cycle detection also bounds a cycle outside this pass. */
        if (n==anchor) return corrupt(v,"chain_cycle",n);
        if (++distance==power) { anchor=n;distance=0;power*=2; }
        c=n;
    }
    return corrupt(v,"chain_cycle",c);
}
static void find_logs(struct fat_volume *v) {
    struct fat_entry entry;
    v->log_directory=UINT32_MAX;
    int e=fat_lookup(v,v->type==32 ? v->root : 0,"SYSTEM",&entry);
    if (e || !(entry.attr&FAT_ATTR_DIR)) return;
    e=fat_lookup(v,entry.first,"LOGS",&entry);
    if (!e && (entry.attr&FAT_ATTR_DIR)) v->log_directory=entry.first;
}
static bool boot_log(struct fat_volume *v, uint32_t dir, const struct fat_entry *file) {
    return dir==v->log_directory &&
        (path_equal(file->alias,"BOOT.LOG") || path_equal(file->alias,"BOOT1.LOG") || path_equal(file->alias,"BOOT2.LOG"));
}
/* Reservations are zero-initialized before ownership is published. The log
 * writer emits text followed by zero padding, with a possible zero gap before
 * the runtime ring. Recover only within the already proved owned chain and
 * the fixed reservation budget; never search free clusters or another mirror.
 * Plain text has no checksum: a text-like torn sector cannot be distinguished
 * from a short record. A malformed sector stops recovery before that sector. */
static int recover_log_size(struct fat_volume *v, struct fat_entry *file) {
    if (!file->first || file->size>=BOOTLOG_LIMIT) return 0;
    /* Start in the last published sector, even for an aligned EOF: a cut
     * during finalization may already have detached its following cluster. */
    uint32_t cb=(uint32_t)v->spc*512, pos=(file->size ? file->size-1 : 0)&~511u, c;
    int e=chain_at(v,file->first,pos/cb,&c); if (e) return e;
    for (uint32_t walked=0;walked<v->clusters;walked++) {
        for (unsigned off=pos%cb;off<cb && pos<BOOTLOG_LIMIT;off+=512,pos+=512) {
            fs_service();
            uint8_t sector[512];
            if ((e=readsec(v,cluster_sector(v,c)+off/512,sector))) return e;
            unsigned length=0;
            while (length<512 && sector[length]) {
                uint8_t ch=sector[length];
                if ((ch<32 && ch!='\t' && ch!='\r' && ch!='\n') || ch==127) return 0;
                length++;
            }
            for (unsigned i=length;i<512;i++) if (sector[i]) return 0;
            if (length && pos+length>file->size) file->size=pos+length;
        }
        if (pos>=BOOTLOG_LIMIT) return 0;
        uint32_t next;
        if ((e=fat_get_cluster(v,c,&next))) return e;
        if (eoc(v,next)) return 0;
        if (!valid(v,next)) return corrupt(v,"log_recovery_chain",next);
        c=next;
    }
    return corrupt(v,"log_recovery_cycle",c);
}
/* Called only after every ownership pass has finished. Publish/detach before
 * freeing, so another power cut restarts the same repair. Directory discovery
 * reuses scan_tree's bounded stack instead of a byte-per-cluster head map. */
static int repair_file_tail(struct fat_volume *v, uint32_t dir, struct fat_entry *file) {
    uint32_t tail=file->first, last=0, a=0, b=0;int e;
    bool log=boot_log(v,dir,file), excess=true;
    if (file->size) {
        if ((e=chain_at(v,file->first,(file->size-1)/((uint32_t)v->spc*512),&last)) ||
            (e=scan_pair(v,last,&a,&b))) return e;
        tail=a;
        /* Imported FAT12 split tails cannot be detached atomically. */
        if (split12(v,last)) return 0;
        if (eoc(v,tail) && a==b) {
            if (!log || !(file->size%((uint32_t)v->spc*512))) return 0;
            excess=false;
        }
    } else if (!tail) return 0;
    uint32_t published=file->size;
    if (log) {
        if ((e=recover_log_size(v,file))) return e;
        /* If recovery moves EOF beyond an interrupted detach, restore the
         * proved active link in both copies before publishing the larger size.
         * Otherwise a second cut could expose a short chain in that mirror. */
        if (file->size>published && a!=b &&
            ((e=publication_stage(v,FAT_P_LINK)) || (e=set_cluster(v,last,a)) || (e=barrier(v)))) return e;
    }
    if (!excess && file->size==published) return 0;
    if ((e=fat_publish_reserved(v,file,true))) return e;
    if (a!=b) {
        /* If the active copy was already EOC, publish_reserved had no
         * active tail to detach. Normalize the other mirror before reclaim. */
        if (eoc(v,a) && ((e=publication_stage(v,FAT_P_LINK)) || (e=set_cluster(v,last,mask(v))) || (e=barrier(v)))) return e;
        v->repaired_copies++;
    }
    if (log) {
        v->repaired_logs++;
        if (klog) klog("[storage] log_repair file=%s action=%s recovered_bytes=%u size=%u",
            file->alias,file->size>published ? "extended" : "finalized",file->size-published,file->size);
    }
    return 0;
}
static int scan_tree(struct fat_volume *v, struct fat_scan_state *s, bool repair) {
    struct { uint32_t dir, cursor; } stack[SCAN_DEPTH];
    unsigned depth=1; int e;
    stack[0].dir=v->type==32 ? v->root : 0;stack[0].cursor=0;
    if (!repair && v->type==32 && (e=scan_chain(v,s,v->root,0,true))) return e;
    while (depth) {
        fs_service();
        struct fat_entry file;
        uint32_t dir=stack[depth-1].dir;
        e=fat_next(v,dir,&stack[depth-1].cursor,&file);
        if (e==-FS_ENOENT) { depth--;continue; }
        if (e) return e;
        if (path_equal(file.alias,".") || path_equal(file.alias,"..")) continue;
        context(v,file.name);
        if (file.attr&0xc0) return corrupt(v,"directory_attributes",file.first);
        if (!repair && (e=scan_chain(v,s,file.first,file.size,(file.attr&FAT_ATTR_DIR)!=0))) return e;
        if (file.attr&FAT_ATTR_DIR) {
            if (depth==SCAN_DEPTH) return corrupt(v,"directory_depth_limit",file.first);
            stack[depth].dir=file.first;stack[depth++].cursor=0;
        } else if (repair) {
            if ((e=repair_file_tail(v,dir,&file))) return e;
        } else if (file.size && dir==v->log_directory) {
            const char *names[]={"BOOT.LOG","BOOT1.LOG","BOOT2.LOG"};
            for (unsigned i=0;i<3;i++) if (path_equal(file.alias,names[i]))
                if ((e=chain_at(v,file.first,(file.size-1)/((uint32_t)v->spc*512),&s->log_eof[i]))) return e;
        }
    }
    return 0;
}
static bool log_detach(struct fat_volume *v, struct fat_scan_state *s, uint32_t c, uint32_t a, uint32_t b) {
    for (unsigned i=0;i<3;i++) if (s->log_eof[i]==c)
        return eof_detach(v,c,a,b);
    return false;
}
static int scan_census(struct fat_volume *v, bool repair, bool *agree) {
    /* After the read-only proof, only file EOF detach can change this
     * unpublished/read-only volume. Its allocation never extends the FAT. */
    uint32_t end=repair ? v->scan_extent : v->clusters+2;
    v->free_clusters=v->clusters-(end-2);v->next_free=end<v->clusters+2 ? end : 2;
    if (!repair) v->scan_extent=2;
    for (uint32_t c=0;c<end;c++) {
        if (!(c&127)) fs_service();
        uint32_t a,b;int e=scan_pair(v,c,&a,&b); if (e) return e;
        if (c==1 && cleanbit(v)) {
            if (!(b&cleanbit(v))) v->ro_reasons|=FAT_RO_DIRTY;
            if (!(b&errorbit(v))) v->ro_reasons|=FAT_RO_IO_FLAG;
            a|=cleanbit(v)|errorbit(v);b|=cleanbit(v)|errorbit(v);
        }
        if (a!=b) { *agree=false; if (c<2) v->ro_reasons|=FAT_RO_COPIES; }
        if (c<2) continue;
        if (a || b) { if (!repair) v->scan_extent=c+1; }
        if (!a && !b) { if (!v->free_clusters || c<v->next_free) v->next_free=c;v->free_clusters++; }
        /* Obvious malformed links are cheap to reject even on clean media. */
        if (a && !eoc(v,a) && a!=mask(v)-8 && (!valid(v,a) || a==c)) *agree=false;
    }
    return 0;
}
static int scan_check(struct fat_volume *v, struct fat_scan_state *s) {
    for (uint32_t c=s->begin<2 ? 2 : s->begin;c<s->end;c++) {
        if (!(c&127)) fs_service();
        uint32_t a,b;int e=scan_pair(v,c,&a,&b); if (e) return e;
        bool owned=scan_owned(v,s,c,false), diff=a!=b;
        /* Owned mirror differences were checked with the published file
         * length in scan_chain; this pass only counts unowned allocation. */
        if (!owned && (a || diff) && (a!=mask(v)-8 || diff)) v->lost_clusters++;
    }
    return 0;
}
static uint32_t repair_value(struct fat_volume *v, struct fat_scan_state *s, uint32_t c, uint32_t a, uint32_t b) {
    if (scan_owned(v,s,c,false)) return a!=b && log_detach(v,s,c,a,b) ? mask(v) : a;
    return a==mask(v)-8 || b==mask(v)-8 ? mask(v)-8 : 0;
}
static int scan_reclaim(struct fat_volume *v, struct fat_scan_state *s) {
    int stage_error=publication_stage(v,FAT_P_RECLAIM); if (stage_error) return stage_error;
    /* FAT12's packed entries overlap; its small volumes retain the existing
     * preserving writer. FAT16/32 coalesce all changes in each FAT sector. */
    uint8_t a[512],b[512];uint32_t sector=UINT32_MAX;bool dirty_a=false,dirty_b=false;
    for (uint32_t c=s->begin<2 ? 2 : s->begin;c<=s->end;c++) {
        uint32_t sec=c==s->end ? UINT32_MAX : offset(v,c)/512;
        if (sector!=sec && sector!=UINT32_MAX) {
            int e=0;
            if (dirty_a) e=writesec(v,v->reserved+sector,a);
            if (!e && dirty_b) e=writesec(v,v->reserved+v->fat_sectors+sector,b);
            if (e) return e;
            dirty_a=dirty_b=false;
        }
        if (c==s->end) break;
        if (!(c&127)) fs_service();
        uint32_t av,bv;int e=scan_pair(v,c,&av,&bv); if (e) return e;
        uint32_t n=repair_value(v,s,c,av,bv);
        if (v->type!=12 && sector!=sec) {
            if ((e=readsec(v,v->reserved+sec,a))) return e;
            if (v->fats==2 && (e=readsec(v,v->reserved+v->fat_sectors+sec,b))) return e;
            sector=sec;
        }
        if (av==n && bv==n) continue;
        bool owned=scan_owned(v,s,c,false);
        if (!owned && n==0) {
            v->repaired_lost++;
            if (av || bv) { v->free_clusters++;if (c<v->next_free) v->next_free=c; }
        }
        if (av!=bv) v->repaired_copies++;
        if (v->type==12) { if ((e=set_cluster(v,c,n))) return e;continue; }
        unsigned off=offset(v,c)%512;
        if (v->type==32) {
            if (av!=n) { fs_wr32(a+off,(fs_rd32(a+off)&0xf0000000)|n);dirty_a=true; }
            if (v->fats==2 && bv!=n) { fs_wr32(b+off,(fs_rd32(b+off)&0xf0000000)|n);dirty_b=true; }
        } else {
            if (av!=n) { fs_wr16(a+off,(uint16_t)n);dirty_a=true; }
            if (v->fats==2 && bv!=n) { fs_wr16(b+off,(uint16_t)n);dirty_b=true; }
        }
    }
    return 0;
}
static int scan_volume(struct fat_volume *v, bool repair, bool explicit_check) {
    v->scan_complete=false;v->scan_fast=false;v->scan_passes=0;
    v->ro_reasons&=~(FAT_RO_LOST|FAT_RO_COPIES);v->lost_clusters=0;
    bool agree=true;int e=scan_census(v,repair,&agree);if (e) return e;
    if (!repair && !explicit_check && cleanbit(v) && agree &&
        !(v->ro_reasons&(FAT_RO_DIRTY|FAT_RO_IO_FLAG|FAT_RO_CORRUPT|FAT_RO_ACTIVE_ONLY))) {
        v->scan_fast=true;v->scan_complete=true;return 0;
    }
    find_logs(v);
    struct fat_scan_state s={0};
    if ((e=scan_memory(v,&s))) return e;
    for (uint32_t begin=0;begin<v->scan_extent;begin=s.end) {
        scan_reset(v,&s,begin);v->scan_passes++;
        if ((e=scan_tree(v,&s,false)) || (e=scan_check(v,&s))) goto out;
    }
    if (v->lost_clusters) v->ro_reasons|=FAT_RO_LOST;
    v->scan_complete=true;
    if (repair && !(v->ro_reasons&(FAT_RO_COPIES|FAT_RO_CORRUPT))) {
        if ((e=scan_tree(v,&s,true))) goto out;
        if (v->scan_passes==1) e=scan_reclaim(v,&s);
        else for (uint32_t begin=0;!e && begin<v->scan_extent;begin=s.end) {
            scan_reset(v,&s,begin);
            if (!(e=scan_tree(v,&s,false))) e=scan_reclaim(v,&s);
        }
        if (e || (e=barrier(v)) || (e=hints(v))) goto out;
        v->lost_clusters=0;v->ro_reasons&=~FAT_RO_LOST;
    }
out:
    scan_release(v,&s);return e;
}
/* Explicit diagnostics always check ownership, including clean volumes. */
int fat_scan(struct fat_volume *v) {
    uint32_t began=fs_now_ms();
    struct fat_prepare_cache *p=0;
    if (!v->prepare_cache) {
        p=alloc_windows(v);if (!p) return -FS_ENOMEM;v->prepare_cache=p;
    }
    int e=scan_volume(v,false,true);
    v->scan_ms=fs_now_ms()-began;
    if (!e) v->scan_deferred=false;
    if (p) { v->prepare_cache=0;fs_contig_free(p,sizeof(*p)); }
    return e;
}
static int reject(struct fat_volume *v, const char *why) { v->diagnostic=why; v->mounted=false; return -FS_EINVAL; }
static int mount_volume(struct fat_volume *v, struct block_cache *cache, struct blkdev *dev, uint64_t start, uint64_t sectors, bool wr, bool preparing, struct fat_prepare_cache *private) {
    memset(v,0,sizeof(*v)); v->cache=cache; v->dev=dev; v->start=start; v->sectors=sectors;
    v->preparing=preparing; v->prepare_cache=private;
    if (!dev || dev->sector_size!=512 || !sectors || start>=dev->capacity || sectors>dev->capacity-start) return reject(v,"device/partition bounds");
    uint8_t b[512], other[512]; int e=readsec(v,0,b); if (e) return e;
    if (fs_rd16(b+510)!=0xaa55 || fs_rd16(b+11)!=512 || (b[0]!=0xe9 && !(b[0]==0xeb && b[2]==0x90))) return reject(v,"BPB signature/sector size/jump");
    v->spc=b[13]; v->reserved=fs_rd16(b+14); v->fats=b[16]; v->root_entries=fs_rd16(b+17); v->media=b[21];
    uint32_t total=fs_rd16(b+19); if (!total) total=fs_rd32(b+32);
    uint32_t fat16=fs_rd16(b+22); v->fat_sectors=fat16 ? fat16 : fs_rd32(b+36);
    if (!v->spc || (v->spc&(v->spc-1)) || v->spc>128 || !v->reserved || !v->fats || v->fats>2 || !v->fat_sectors || !total || total>sectors || (v->media!=0xf0 && v->media<0xf8)) return reject(v,"BPB geometry");
    uint64_t roots=((uint64_t)v->root_entries*32+511)/512;
    uint64_t data=(uint64_t)v->reserved+(uint64_t)v->fats*v->fat_sectors+roots;
    if (data>=total) return reject(v,"BPB extent overflow");
    v->sectors=total; v->root_sector=v->reserved+v->fats*v->fat_sectors; v->data_sector=(uint32_t)data;
    v->clusters=(total-v->data_sector)/v->spc;
    v->type=v->clusters<4085 ? 12 : v->clusters<65525 ? 16 : 32;
    unsigned ext=v->type==32 ? 64 : 36;
    if (b[ext+2]==0x29) {
        v->volume_serial=fs_rd32(b+ext+3);
        memcpy(v->volume_label,b+ext+7,11);
        for (unsigned i=0;i<11;i++) if ((uint8_t)v->volume_label[i]<32 || (uint8_t)v->volume_label[i]>126) v->volume_label[i]='?';
        for (unsigned i=11;i && v->volume_label[i-1]==' ';i--) v->volume_label[i-1]=0;
        if (!strncmp(v->volume_label,"NO NAME",8)) v->volume_label[0]=0;
    }
    if (!v->clusters || v->clusters>=0x0ffffff5 || ((uint64_t)(v->clusters+2)*v->type+7)/8>(uint64_t)v->fat_sectors*512) return reject(v,"FAT capacity/cluster count");
    if ((v->type==32 && (fat16 || v->root_entries || fs_rd16(b+19))) || (v->type!=32 && (!fat16 || !v->root_entries || (v->root_entries%16)))) return reject(v,"cluster type contradicts BPB layout");
    /* Resource Kit Appendix B: DMF distribution disks are read-only. CHS
     * describes this format only; it never gates generic FAT12 USB/images.
     * See docs/validation/f1-115b-native-fat12.md. */
    if (v->type==12 && total==3360 && fs_rd16(b+24)==21 && fs_rd16(b+26)==2)
        v->ro_reasons|=FAT_RO_DMF;
    if (v->type==32) {
        v->root=fs_rd32(b+44); v->fsinfo=fs_rd16(b+48); v->backup=fs_rd16(b+50);
        uint16_t flags=fs_rd16(b+40);
        if (!valid(v,v->root) || fs_rd16(b+42) || !v->fsinfo || v->fsinfo>=v->reserved || (v->backup!=0 && v->backup!=0xffff && (v->backup>=v->reserved || v->backup==v->fsinfo)) || (flags&0xff70)) return reject(v,"FAT32 root/version/reserved geometry");
        if (flags&0x80) {
            v->active_fat=(uint8_t)(flags&15); if (v->active_fat>=v->fats) return reject(v,"active FAT index");
            v->ro_reasons|=FAT_RO_ACTIVE_ONLY;
        } else if (flags&15) return reject(v,"mirrored FAT active bits");
        if ((e=readsec(v,v->fsinfo,other))) return e;
        if (fs_rd32(other)!=0x41615252 || fs_rd32(other+484)!=0x61417272 || fs_rd32(other+508)!=0xaa550000) return reject(v,"FSInfo signatures");
        uint32_t free=fs_rd32(other+488), next=fs_rd32(other+492);
        v->free_clusters=free<=v->clusters ? free : 0;
        v->free_unknown=free>v->clusters; /* 0xFFFFFFFF: unknown (FAT spec FSI_Free_Count) */
        v->next_free=valid(v,next) ? next : 2;
        if (v->backup && v->backup!=0xffff) {
            if ((e=readsec(v,v->backup,other))) return e;
            if (memcmp(b+11,other+11,79) || fs_rd16(other+510)!=0xaa55) v->ro_reasons|=FAT_RO_CORRUPT;
        }
    }
    uint32_t zero=0,one=0;
    for (unsigned copy=0;copy<(preparing && !private ? v->fats : 1u);copy++) {
        uint32_t z,o;
        unsigned f=preparing && !private ? copy : v->active_fat;
        if ((e=get_copy(v,f,0,&z)) || (e=get_copy(v,f,1,&o))) return e;
        if (z!=((mask(v)&~255u)|v->media) || (v->type==12 ? !eoc(v,o) : (o|(cleanbit(v)|errorbit(v)))!=mask(v))) return reject(v,"FAT reserved entries/media/flags");
        if (copy && (z!=zero || o!=one)) v->ro_reasons|=FAT_RO_COPIES;
        zero=z;one=o;
        if (cleanbit(v) && !(one&cleanbit(v))) v->ro_reasons|=FAT_RO_DIRTY;
        if (errorbit(v) && !(one&errorbit(v))) v->ro_reasons|=FAT_RO_IO_FLAG;
    }
    if (!wr) v->ro_reasons|=FAT_RO_REQUEST;
    if (!dev->write) v->ro_reasons|=FAT_RO_WRITE_PROTECTED;
    if (!blkdev_durable(dev)) v->ro_reasons|=FAT_RO_DURABILITY;
    v->readonly=v->ro_reasons!=0; v->mounted=true;
    /* Only the removable header path has no windows. No full FAT read or
     * ownership proof is needed to publish a deliberately read-only drive. */
    v->scan_deferred=preparing && !private;
    if (v->scan_deferred && v->type!=32) v->free_unknown=true; /* FAT12/16 keep no free-count hint */
    uint32_t scan_begin=fs_now_ms();
    e=v->scan_deferred ? 0 : scan_volume(v,false,false);
    if (!v->scan_deferred) v->scan_ms=fs_now_ms()-scan_begin;
    if (e && e!=-FS_EUCLEAN) { v->mounted=false; return e; }
    if (v->ro_reasons) v->readonly=true;
    if (v->ro_reasons&~(FAT_RO_REQUEST|FAT_RO_DIRTY|FAT_RO_LOST|FAT_RO_IO_FLAG)) {
        context(v,0);fat_revoke_write(v,fat_readonly_reason(v),0);
    }
    if (!v->readonly_reason) v->diagnostic=v->readonly ? "read-only; inspect ro_reasons" : "validated writable mount";
    if (!v->readonly || (wr && !(v->ro_reasons&~(FAT_RO_DIRTY|FAT_RO_LOST|FAT_RO_IO_FLAG)))) return fat_enable_write(v);
    return 0;
}
static int mount_with_windows(struct fat_volume *v, struct block_cache *cache, struct blkdev *dev,
                              uint64_t start, uint64_t sectors, bool wr, bool preparing) {
    uint32_t began=fs_now_ms();memset(v,0,sizeof(*v));
    struct fat_prepare_cache *private=alloc_windows(v);
    if (!private) return -FS_ENOMEM;
    int e=mount_volume(v,cache,dev,start,sectors,wr,preparing,private);
    v->prepare_cache=0;
    fs_contig_free(private,sizeof(*private));
    v->mount_ms=fs_now_ms()-began;
    return e;
}
int fat_mount(struct fat_volume *v, struct block_cache *cache, struct blkdev *dev, uint64_t start, uint64_t sectors, bool wr) {
    return mount_with_windows(v,cache,dev,start,sectors,wr,false);
}
int fat_prepare(struct fat_volume *v, struct block_cache *cache, struct blkdev *dev, uint64_t start, uint64_t sectors) {
    return mount_with_windows(v,cache,dev,start,sectors,false,true);
}
int fat_prepare_removable(struct fat_volume *v, struct block_cache *cache, struct blkdev *dev, uint64_t start, uint64_t sectors) {
    uint32_t began=fs_now_ms();
    int e=mount_volume(v,cache,dev,start,sectors,false,true,0);
    v->mount_ms=fs_now_ms()-began;
    return e;
}
static int repair_volume(struct fat_volume *v) {
    return scan_volume(v,true,false);
}
static int enable_write(struct fat_volume *v) {
    if (!v || !v->mounted) return -FS_EINVAL;
    context(v,0);
    int e=(v->preparing || v->dev->logical) ? (v->dev->quarantined ? -FS_EIO : 0) : cache_error(v->cache,v->dev); if (e) return write_error(v,e);
    if (!v->readonly && v->writable_session) return 0;
    if (!blkdev_durable(v->dev)) {
        v->ro_reasons|=FAT_RO_DURABILITY;fat_revoke_write(v,"durability",0);return -FS_EROFS;
    }
    if (v->ro_reasons&~(FAT_RO_REQUEST|FAT_RO_DURABILITY|FAT_RO_DIRTY|FAT_RO_LOST|FAT_RO_IO_FLAG)) return -FS_EROFS;
    if (v->scan_deferred) {
        /* Full ownership even on clean media. No dirty flag/repair write
         * may precede this read-only proof; FSInfo is advisory only. */
        if ((e=fat_scan(v))) return e;
    }
    if (!v->scan_complete || v->ro_reasons&~(FAT_RO_REQUEST|FAT_RO_DURABILITY|FAT_RO_DIRTY|FAT_RO_LOST|FAT_RO_IO_FLAG)) return -FS_EROFS;
    bool recover=(v->ro_reasons&(FAT_RO_DIRTY|FAT_RO_LOST|FAT_RO_IO_FLAG))!=0;
    if (recover) {
        /* Mark the session dirty before any repair. A second power cut
         * re-enters this same bounded scan; no repair requires a clean flag. */
        if ((e=set_clean(v,false))) return e;
        v->readonly=false;
        e=repair_volume(v);
        if (e) { v->readonly=true;return e; }
        if ((e=set_clean(v,true))) return e;
        v->ro_reasons&=~(FAT_RO_DIRTY|FAT_RO_LOST|FAT_RO_IO_FLAG); v->dirty_recovered=true;
    }
    /* A permitted durability/dirty upgrade starts a new writable session;
     * its later first failure must not inherit the former closed gate. */
    v->readonly_reason=0;v->readonly_file[0]=0;v->readonly_cluster=0;
    v->ro_reasons=0; v->readonly=false; v->writable_session=true;
    if ((e=set_clean(v,false))) return e;
    v->diagnostic=v->dirty_recovered ? "dirty_recovered" : "read gate passed; writable";
    return 0;
}
int fat_enable_write(struct fat_volume *v) {
    if (!v || !v->mounted) return -FS_EINVAL;
    if (!v->readonly && v->writable_session) return 0;
    uint32_t began=fs_now_ms();struct fat_prepare_cache *p=0;
    bool recover=(v->ro_reasons&(FAT_RO_DIRTY|FAT_RO_LOST|FAT_RO_IO_FLAG))!=0;
    if (!v->prepare_cache) {
        p=alloc_windows(v);if (!p) return -FS_ENOMEM;v->prepare_cache=p;
    }
    int e=enable_write(v);
    if (p) { v->prepare_cache=0;fs_contig_free(p,sizeof(*p)); }
    v->repair_ms=fs_now_ms()-began;
    if (klog && recover) klog("[storage] repaired logs=%u lost=%u copies=%u dirty=%s elapsed_ms=%u mount_ms=%u error=%d",
        v->repaired_logs,v->repaired_lost,v->repaired_copies,e ? "retained" : "cleared",v->repair_ms,v->mount_ms,e);
    return e;
}
/* fx-17: active-FAT free count for a removable volume without a trusted
 * FSInfo hint (FAT12/16 always; FAT32 with FSI_Free_Count 0xFFFFFFFF or out
 * of range). The FAT spec: the count "must be computed" when unknown. */
#define FAT_REMOVABLE_CENSUS_BYTES (512u*1024u)
static int census_begin(struct fat_volume *v) {
    struct fat_census *c=fs_contig_alloc(sizeof(*c));
    if (!c) return -FS_ENOMEM;
    memset(c,0,sizeof(*c)); c->sectors=(uint32_t)(((uint64_t)(v->clusters+2)*4+511)/512);
    if (c->sectors>v->fat_sectors) c->sectors=v->fat_sectors;
    c->free=fs_contig_alloc(c->sectors); c->dirty=fs_contig_alloc(((size_t)c->sectors+31)/32*4);
    if (!c->free || !c->dirty) {
        if (c->free) fs_contig_free(c->free,c->sectors);
        if (c->dirty) fs_contig_free(c->dirty,((size_t)c->sectors+31)/32*4);
        fs_contig_free(c,sizeof(*c)); return -FS_ENOMEM;
    }
    memset(c->free,0,c->sectors); memset(c->dirty,0xff,((size_t)c->sectors+31)/32*4);
    if (c->sectors%32) c->dirty[c->sectors/32]=(1u<<(c->sectors%32))-1u; /* no padding bits */
    c->started_ms=fs_now_ms(); v->census=c;
    return 0;
}
void fat_census_end(struct fat_volume *v) {
    struct fat_census *c=v ? v->census : 0;
    if (!c) return;
    v->census=0;
    fs_contig_free(c->free,c->sectors); fs_contig_free(c->dirty,((size_t)c->sectors+31)/32*4);
    fs_contig_free(c,sizeof(*c));
}
static bool census_take(struct fat_census *c, uint32_t i) {
    uint32_t bit=1u<<(i%32);
    if (!(__atomic_load_n(&c->dirty[i/32],__ATOMIC_ACQUIRE)&bit)) return false;
    __atomic_and_fetch(&c->dirty[i/32],~bit,__ATOMIC_ACQ_REL);
    return true;
}
int fat_census_step(struct fat_volume *v, unsigned budget) {
    struct fat_census *c=v ? v->census : 0;
    if (!c || !v->mounted) return 1;
    if (v->dev->quarantined) return -FS_EIO;
    context(v,0);
    uint64_t base=v->reserved+(uint64_t)v->active_fat*v->fat_sectors;
    uint8_t b[512]; unsigned done=0, scanned=0;
    while (done<budget && scanned<c->sectors) {
        uint32_t i=c->cursor; c->cursor=(c->cursor+1)%c->sectors; scanned++;
        if (!census_take(c,i)) continue;
        int e=readsec(v,base+i,b);
        if (e) { census_mark(v,base+i); return e; }
        unsigned n=0;
        for (unsigned k=0;k<128;k++) {
            uint32_t cl=i*128u+k;
            if (cl>=2 && cl<v->clusters+2 && !(fs_rd32(b+k*4)&0x0fffffffu)) n++;
        }
        c->free[i]=(uint8_t)n; done++;
    }
    for (uint32_t w=0;w<(c->sectors+31)/32;w++) if (__atomic_load_n(&c->dirty[w],__ATOMIC_ACQUIRE)) return 0;
    /* Nothing dirty: every count is at least as new as every publication,
     * and the caller's drive reservation keeps it that way while we sum. */
    uint64_t free=0;
    for (uint32_t i=0;i<c->sectors;i++) free+=c->free[i];
    uint32_t ms=fs_now_ms()-c->started_ms;
    v->free_clusters=(uint32_t)free; v->free_unknown=false;
    if (klog) klog("[storage] free_count clusters=%u free=%u elapsed_ms=%u source=background",v->clusters,(uint32_t)free,ms);
    fat_census_end(v);
    return 1;
}
uint32_t fat_free_estimate(const struct fat_volume *v, bool *known) {
    const struct fat_census *c=v->census;
    if (known) *known=!v->free_unknown;
    if (!v->free_unknown) return v->free_clusters;
    if (!c) return 0;
    uint64_t free=0;
    for (uint32_t i=0;i<c->sectors;i++) {
        bool pending=(__atomic_load_n(&c->dirty[i/32],__ATOMIC_ACQUIRE)>>(i%32))&1u;
        free+=pending ? 128u : c->free[i];
    }
    return free>v->clusters ? v->clusters : (uint32_t)free;
}

static int census_free(struct fat_volume *v) {
    uint32_t free=0, first=0;
    for (uint32_t c=2;c<v->clusters+2;c++) {
        if (!(c&127)) fs_service();
        uint32_t n; int e=fat_get_cluster(v,c,&n); if (e) return e;
        if (!n) { free++; if (!first) first=c; }
    }
    v->free_clusters=free; v->next_free=first ? first : 2; v->free_unknown=false;
    return 0;
}
/* fx-17b: a volume left read-only (write-protected, durability, metadata
 * reasons) still reports its free space: small FATs are counted now, a
 * large FAT32 in the background like a writable one. */
int fat_census_start(struct fat_volume *v) {
    if (!v || !v->mounted || !v->free_unknown || v->census) return 0;
    context(v,0);
    if (v->type==32 && (uint64_t)(v->clusters+2)*4>FAT_REMOVABLE_CENSUS_BYTES) {
        int e=census_begin(v); if (e) return e;
        v->free_clusters=v->clusters; return 0;
    }
    return census_free(v);
}
/* fx-17: removable FAT is made writable the way Windows 98 VFAT and Linux
 * vfat mount it: no ownership scan at insertion (F1-74's deferred proof is
 * superseded for removable media, see vfs-storage-contract.md "fx-17").
 * The allocator only takes clusters whose active-FAT entry is zero, so a
 * medium left dirty (lost clusters at worst under this driver's publication
 * order) stays safe to write; its dirty/error indication is preserved for an
 * explicit check. Write protection, unqualified durability, divergent or
 * corrupt metadata and DMF disks keep the volume read-only. */
int fat_enable_write_removable(struct fat_volume *v) {
    if (!v || !v->mounted) return -FS_EINVAL;
    if (!v->readonly && v->writable_session) return 0;
    if (!v->scan_deferred) return fat_enable_write(v);
    context(v,0);
    int e=v->dev->logical ? 0 : cache_error(v->cache,v->dev); if (e) return write_error(v,e);
    if (!v->dev->write) {
        v->ro_reasons|=FAT_RO_WRITE_PROTECTED; fat_revoke_write(v,"write_protected",0); return -FS_EROFS;
    }
    if (!blkdev_durable(v->dev)) {
        v->ro_reasons|=FAT_RO_DURABILITY; fat_revoke_write(v,"durability",0); return -FS_EROFS;
    }
    if (v->ro_reasons&~(FAT_RO_REQUEST|FAT_RO_DIRTY|FAT_RO_IO_FLAG)) return -FS_EROFS;
    /* Without a trusted free count, count the active FAT now when that is
     * cheap (FAT12/16 and small FAT32: at most FAT_REMOVABLE_CENSUS_BYTES).
     * A large FAT32 whose FSInfo says "unknown" (not written by Windows or
     * mkfs.fat) stays read-only until the explicit full upgrade: counting
     * megabytes of FAT under the drive reservation would stall its readers. */
    uint32_t began=fs_now_ms();
    if (v->free_unknown && v->type==32 && (uint64_t)(v->clusters+2)*4>FAT_REMOVABLE_CENSUS_BYTES) {
        /* fx-17b: a large FAT32 whose FSInfo says "unknown" is writable at
         * once; the storage writer counts its FAT in the background. Until
         * then allocation scans from the FSInfo next-free hint and the free
         * count is only a bound (Windows 98 keeps FSI_Free_Count unknown
         * until it recomputes it; FAT spec: 0xFFFFFFFF "must be computed"). */
        if ((e=census_begin(v))) { fat_revoke_write(v,"free_count_unknown",0); return e; }
        v->free_clusters=v->clusters;
    } else if (v->free_unknown && (e=census_free(v))) { v->repair_ms=fs_now_ms()-began; return e; }
    bool dirty=(v->ro_reasons&(FAT_RO_DIRTY|FAT_RO_IO_FLAG))!=0;
    v->readonly_reason=0;v->readonly_file[0]=0;v->readonly_cluster=0;
    v->ro_reasons=0; v->readonly=false; v->writable_session=true;
    v->removable_trusted=true; v->dirty_preserved=dirty;
    /* A clean medium is marked in use, so removal without Safely Remove is
     * detectable at the next insertion (FAT spec ClnShutBitMask). */
    if (!dirty && (e=set_clean(v,false))) { v->repair_ms=fs_now_ms()-began; return e; }
    v->repair_ms=fs_now_ms()-began;
    v->diagnostic=dirty ? "removable writable; dirty indication kept" : "removable writable";
    return 0;
}
int fat_commit(struct fat_volume *v) {
    if (!v->mounted) return -FS_EINVAL;
    int e=v->dev->logical ? 0 : cache_error(v->cache,v->dev); if (e) return write_error(v,e);
    if (v->readonly) return v->writable_session ? -FS_EROFS : 0;
    return barrier(v);
}
int fat_unmount(struct fat_volume *v) {
    int e=fat_commit(v); if (e) return e;
    fat_census_end(v);
    if (v->writable_session && !v->readonly && cleanbit(v) && !v->dirty_preserved) {
        if ((e=set_clean(v,true))) return e;
    }
    v->mounted=false; return 0;
}

/* Reserved publication is the only transaction path into the shared cache.
 * In particular, neither cache_group_begin nor cache_write runs under the
 * caller's short namespace turn. Each completed image remains pinned until
 * its durable barrier, including repeated versions of one FAT sector. */
/* Namespace held (from resume). Other writers may have published while this
 * transaction waited: adopt their sequence and counters, keep our pending
 * free-count change. Our next step reads every shared sector afresh. */
static void transaction_rebase(struct fat_transaction *t) {
    struct fat_volume *v=&t->view;
    int64_t pending=(int64_t)v->free_clusters-(int64_t)t->free_synced;
    t->sequence=t->source->mutation_sequence;
    v->free_clusters=(uint32_t)((int64_t)t->source->free_clusters+pending);
    t->free_synced=t->source->free_clusters;
    /* next_free stays ours: a hint our own allocations advanced; the
     * allocator re-checks the FAT and the in-flight claims anyway. */
    /* Our target can only change behind our identity claim through a
     * legacy (unclaimed) caller: stop before publishing anything more. */
    if (t->target_initialized && t->target && t->meta) {
        const struct fat_entry *old=&t->meta->original, *now=t->target;
        if (now->parent!=old->parent || now->index!=old->index || now->first!=old->first ||
            now->size!=old->size || now->chain_generation!=old->chain_generation) t->error=-FS_EAGAIN;
    }
}
static bool transaction_suspend(struct fat_transaction *t) {
    if (!t->suspendable || !t->guard.suspend || !t->guard.resume) return false;
    t->guard.suspend(t->guard.ctx); return true;
}
static void transaction_resume(struct fat_transaction *t) {
    t->guard.resume(t->guard.ctx); transaction_rebase(t); t->guard.leave(t->guard.ctx);
}
static bool transaction_try_resume(struct fat_transaction *t) {
    if (!t->guard.try_resume(t->guard.ctx)) return false;
    transaction_rebase(t); t->guard.leave(t->guard.ctx); return true;
}
/* Pinned bytes (snap) may include other writers' entries published while
 * this transaction waited without the turn. Keep every nibble we changed
 * relative to base (our own entries and claimed clusters only), take the
 * rest from snap. The FAT32 FSInfo hints are regenerated from the rebased
 * view. Fill (owned data) sectors are exclusive and need no merge. */
static void transaction_merge(struct fat_volume *v) {
    struct fat_transaction *t=v->transaction;
    for (unsigned i=0;i<t->count;i++) if (!(t->fill&(1u<<i))) {
        uint8_t *img=tx_image(t,i), *base=tx_base(t,i); const uint8_t *cur=tx_snap(t,i);
        for (unsigned k=0;k<512;k++) {
            uint8_t m=img[k]^base[k];
            uint8_t ours=(uint8_t)((m&0xf0 ? 0xf0 : 0)|(m&0x0f ? 0x0f : 0));
            img[k]=(uint8_t)((img[k]&ours)|(cur[k]&(uint8_t)~ours));
        }
        memcpy(base,cur,512);
        if (v->type==32 && t->sectors[i]==v->fsinfo) { fs_wr32(img+488,t->source->free_unknown ? 0xffffffffu : v->free_clusters); fs_wr32(img+492,v->next_free); }
    }
}
static int transaction_durable(struct fat_volume *v) {
    struct fat_transaction *t=v->transaction;
    int e=cache_barrier_record(v->cache,v->dev,&v->completion);
    if (!e) {
        t->dependency=t->last_published; t->durable_group=v->publication_group;
        t->undurable=false; t->open_group=0;
    } else {
        /* Cancellation after publication can have uncertain durable
         * metadata. Retain the old write gate until mount recovery. */
        if (e==-FS_EROFS) (void)write_error(v,e);
        else {
            v->ro_reasons|=FAT_RO_WRITE_ERROR;
            fat_revoke_write(v,"transaction_durability",0);
        }
    }
    if (v->publication_observer) v->publication_observer(v->publication_ctx,v->publication_group,v->publication_stage,e ? 'E' : 'B',e);
    return e;
}
/* durable: a real barrier() (a C2 boundary) publishes what is buffered,
 * then waits for durability outside the guard. !durable: the 16-sector
 * overflow inside one C2 group publishes only, in the group's open cache
 * group, so write-back merges the group again. */
static int transaction_publish(struct fat_volume *v, bool durable) {
    struct fat_transaction *t=v->transaction;
    if (t->error) return t->error;
    if (!t->count) {
        if (!durable || !t->undurable) return 0;
        bool suspended=transaction_suspend(t);
        int e=transaction_durable(v);
        if (suspended) transaction_resume(t);
        if (!e) v->publication_pending=false;
        return write_error(v,e);
    }
    struct cache_reserve_sector set[FAT_TX_SECTORS];
    struct cache_image images[FAT_TX_SECTORS];
    /* A data transaction whose every shared image has its base waits for
     * slots, credits, readers or another writer's barrier without the turn,
     * then merges its own nibbles onto what is pinned (f1-89 A1 item 4).
     * merging: all bases valid, so every pinned snapshot is merged (a no-op
     * when nothing changed). suspending: the turn may be given up. */
    uint16_t all=(uint16_t)((1u<<t->count)-1u);
    bool merging=(uint16_t)(t->based|t->fill)==all;
    bool suspending=merging && t->suspendable && t->guard.suspend && t->guard.resume && t->guard.try_resume;
    unsigned contended=0; /* resume attempts lost to another writer */
    for (unsigned i=0;i<t->count;i++) {
        bool fill=(t->fill&(1u<<i))!=0;
        set[i]=(struct cache_reserve_sector){v->start+t->sectors[i],merging && !fill ? tx_snap(t,i) : 0,fill ? tx_image(t,i) : 0};
        images[i]=(struct cache_image){i,tx_image(t,i)};
    }
retry:
    ; struct cache_reservation reservation;
    if (suspending) transaction_suspend(t);
    int e=cache_reserve(v->cache,v->dev,set,t->count,&reservation);
    if (suspending) {
        /* Never wait for the turn while holding pins (the holder may need
         * them): give the reservation up, wait holding nothing, retry. After
         * a few losses reserve under the turn; pin holders never wait for it. */
        if (!e && !transaction_try_resume(t)) {
            cache_release(&reservation); transaction_resume(t);
            if (t->error) return t->error;
            if (++contended>=4) suspending=false;
            goto retry;
        }
        if (e) transaction_resume(t);
    }
    if (!e && t->error) { cache_release(&reservation); return t->error; }
    if (e) return e;
    if (merging) transaction_merge(v);
    reservation.group_hint=t->open_group;
    for (;;) {
        e=transaction_enter(t);
        if (e==-FS_EBUSY) {
            cache_release(&reservation);
            if ((e=fs_request_check())) return e;
            if (suspending) transaction_suspend(t);
            if (t->guard.wait) e=t->guard.wait(t->guard.ctx);
            else fs_service();
            if (suspending) transaction_resume(t);
            if (e) return e;
            if (t->error) return t->error;
            goto retry;
        }
        if (e) break;
        if (t->source->mutation_sequence==UINT64_MAX || t->source->publication_count==UINT64_MAX ||
            (t->source->publication_count && t->source->publication_generation!=t->generation)) {
            t->guard.leave(t->guard.ctx); e=-FS_EIO; break;
        }
        if (t->visible_pending && t->target_initialized && t->target) {
            const struct fat_entry *old=&t->meta->original, *now=t->target;
            if (now->parent!=old->parent || now->index!=old->index || now->first!=old->first ||
                now->size!=old->size || now->chain_generation!=old->chain_generation) {
                t->guard.leave(t->guard.ctx); e=-FS_EAGAIN; break;
            }
        }
        e=cache_publish(&reservation,images,t->count,t->dependency);
        if (!e && t->data_only) {
            /* Owned DATA sectors only: no metadata changed, so neither the
             * sequence nor the (possibly stale) view counters are published. */
            v->publication_group=++t->source->publication_count;
            v->publication_frontier=reservation.group;
            t->source->publication_frontier=reservation.group;
            t->source->publication_generation=t->generation;
            if (v->publication_observer) v->publication_observer(v->publication_ctx,v->publication_group,v->publication_stage,'G',0);
        } else if (!e) {
            t->sequence=++t->source->mutation_sequence;
            for (unsigned k=0;k<t->count;k++) census_mark(t->source,t->sectors[k]);
            /* Publish only this transaction's own change of the free count. */
            int64_t delta=(int64_t)v->free_clusters-(int64_t)t->free_synced;
            t->source->free_clusters=(uint32_t)((int64_t)t->source->free_clusters+delta);
            v->free_clusters=t->source->free_clusters; t->free_synced=v->free_clusters;
            t->source->next_free=v->next_free;
            v->publication_group=++t->source->publication_count;
            v->publication_frontier=reservation.group;
            t->source->publication_frontier=reservation.group;
            t->source->publication_generation=t->generation;
            if (t->visible_pending && t->target) {
                *t->target=t->meta->visible; t->meta->original=t->meta->visible; t->target_initialized=true;
                if ((t->op==FAT_TX_RENAME || t->op==FAT_TX_RENAME_REPLACE) && t->source->identity_changed) {
                    t->source->identity_changed(t->source->identity_ctx,t->source,&t->entry,&t->meta->visible,FAT_ID_RENAME);
                    t->rename_notified=true;
                }
                if (t->on_visible) t->on_visible(t->visible_ctx,&t->meta->visible);
                t->visible_pending=false;
            }
            if (t->unlink_pending) {
                bool replacement=t->unlink_replacement;
                struct fat_entry *target=replacement ? t->replacement_target : t->target;
                if (target) target->index=UINT32_MAX;
                if (!replacement) t->meta->original.index=UINT32_MAX;
                if (t->source->identity_changed) t->source->identity_changed(t->source->identity_ctx,t->source,&t->meta->unlink_entry,0,FAT_ID_UNLINK);
                if (t->on_unlinked) t->on_unlinked(t->visible_ctx,replacement);
                t->unlink_pending=false; t->unlink_notified=true;
            }
            if (v->publication_observer) v->publication_observer(v->publication_ctx,v->publication_group,v->publication_stage,'G',0);
        }
        t->guard.leave(t->guard.ctx);
        if (e!=-FS_EAGAIN) break;
        /* Our slots are pinned: their bytes cannot change while we wait. */
        if (suspending) transaction_suspend(t);
        e=cache_reservation_wait(&reservation);
        if (suspending && !transaction_try_resume(t)) {
            cache_release(&reservation); transaction_resume(t);
            if (!e) e=t->error;
            if (e) return write_error(v,e);
            if (++contended>=4) suspending=false;
            goto retry;
        }
        if (!e && t->error) e=t->error;
        if (e) break;
        if (merging) transaction_merge(v); /* FSInfo hints after a rebase */
    }
    bool suspended=false;
    if (!e) {
        t->open_group=reservation.group; t->last_published=reservation.published; t->undurable=true;
        /* The durability wait runs without the writer turn (item 4); our
         * pins are released before the turn is taken back. */
        if (durable) { suspended=transaction_suspend(t); e=transaction_durable(v); }
    }
    cache_release(&reservation);
    if (suspended) transaction_resume(t);
    if (!e) { t->count=0; t->fill=0; t->based=0; if (durable) v->publication_pending=false; }
    return write_error(v,e);
}
static int lookup_dependency(struct fat_volume *v, const struct fat_transaction_args *a) {
    if (!a->depend || a->depend_index==UINT32_MAX) return 0;
    uint8_t b[32]; int e=dir_read(v,a->depend_parent,a->depend_index,b); if (e) return e;
    uint32_t first=fs_rd16(b+26); if (v->type==32) first|=(uint32_t)fs_rd16(b+20)<<16;
    if (!b[0] || b[0]==0xe5 || b[11]==15 || !(b[11]&FAT_ATTR_DIR) || first!=a->directory) return -FS_EAGAIN;
    return 0;
}
/* Under the retirement guard no publication can run: a lookup whose every
 * read still matches the resident bytes is equivalent to one performed now.
 * A sector no longer resident cannot be compared and counts as changed. */
static bool reads_current(struct fat_transaction *t) {
    if (t->read_overflow) return false;
    uint8_t now[512];
    for (unsigned i=0;i<t->read_count;i++) {
        if (cache_peek(t->source->cache,t->source->dev,t->source->start+t->read_sectors[i],now)) return false;
        if (memcmp(now,read_copy(t,i),512)) return false;
    }
    return true;
}
/* f1-93b: the work bound of a cache-only section. Counted only once the
 * step has read its first directory sector, so a section always settles at
 * least the entry it resumed at (at most 21 slots, 3 sectors) before it
 * stops: no livelock, whatever the bound. */
static int cache_charge(struct fat_volume *v, unsigned units) {
    if (!v->cache_progress) return 0;
    v->cache_work+=units;
    if (v->cache_work<=FAT_CACHED_BUDGET) return 0;
    if (v->cache_miss==UINT64_MAX) v->cache_miss=FAT_CACHE_YIELD;
    return -FS_EAGAIN;
}
static __attribute__((noinline)) int dir_cached(struct fat_volume *v, uint32_t dir, const char *name, uint32_t *cursor,
                                                struct fat_entry *e, bool labels, struct fat_dir_hint *hint) {
    struct dir_walk w; walk_begin(&w,v,dir,hint);
    uint32_t at=*cursor, settled=at; int r;
    for (;;) {
        r=next_entry(v,dir,&at,e,name ? false : labels,&w,&settled);
        if (r || !name || path_equal(name,e->name) || path_equal(name,e->alias)) break;
        /* A decoded entry (LFN to UTF-8, name compare) costs as a sector. */
        if ((r=cache_charge(v,2))) { settled=at; break; }
    }
    walk_end(&w,v);
    *cursor=v->cache_miss==UINT64_MAX ? at : settled;
    return r;
}
int fat_dir_cached(struct fat_volume *v, uint32_t dir, const char *name, uint32_t *cursor,
                   struct fat_entry *e, bool labels, struct fat_dir_hint *hint, uint64_t *miss) {
    if (name) context(v,name);
    v->cache_only=true; v->cache_miss=UINT64_MAX; v->cache_work=0; v->cache_progress=false;
    int r=dir_cached(v,dir,name,cursor,e,labels,hint);
    v->cache_only=v->cache_progress=false; *miss=v->cache_miss;
    return *miss==UINT64_MAX ? r : -FS_EAGAIN;
}
/* f1-93b: the FAT link of cluster c from a resident FAT sector, if any (a
 * warm prefetch decision only: no device read, no guard). */
static bool link_resident(const struct fat_volume *v, uint32_t c, uint32_t *next) {
    if (v->type!=16 && v->type!=32) return false;
    uint32_t off=c*(v->type/8u);
    uint64_t s=v->reserved+(uint64_t)v->active_fat*v->fat_sectors+off/512;
    uint8_t b[512];
    if (cache_peek(v->cache,v->dev,v->start+s,b)) return false;
    *next=v->type==32 ? fs_rd32(b+off%512)&0x0fffffffu : fs_rd16(b+off%512);
    return true;
}
int fat_warm(const struct fat_volume *v, uint64_t s) {
    if (!v->dev || v->dev->logical) return 0;
    if (s>=v->sectors) return -FS_EINVAL;
    if (hardware_check_io_cancelled && hardware_check_io_cancelled()) return -FS_EAGAIN;
    if (file_read_cancelled && file_read_cancelled()) return -4; /* EINTR, before issuance */
    unsigned n=readahead_count(v,s);
    if (v->type && v->spc && s>=v->data_sector) {
        /* A directory's next cluster, when it is the adjacent one. */
        uint32_t c=(uint32_t)((s-v->data_sector)/v->spc)+2, next;
        while (n+v->spc<=BLKDEV_READ_SECTORS && s+n+v->spc<=v->sectors && c+1<v->clusters+2 &&
               link_resident(v,c,&next) && next==c+1) { n+=v->spc; c=next; }
    }
    uint8_t b[512];
    int e=cache_read_ahead(v->cache,v->dev,v->start+s,n,b);
    return e==-FS_EQUARANTINED ? -FS_EIO : e;
}
int fat_chain_clusters(struct fat_volume *v, struct fat_chain_walk *w, bool cached, uint64_t *miss) {
    uint8_t b[512]; uint64_t loaded=UINT64_MAX; unsigned visited=0; int e=0;
    uint32_t c=w->cluster;
    *miss=UINT64_MAX;
    if (cached) { v->cache_only=true; v->cache_miss=UINT64_MAX; v->cache_work=0; v->cache_progress=false; }
    while (c) {
        if (!valid(v,c) || w->count>=v->clusters) { e=-FS_EIO; break; }
        /* As get_copy: 128 decodes per checkpoint. f1-93b: a cached section
         * (guard held) is bounded by FAT_CHAIN_CACHED_SECTORS instead and
         * never reaches a preemption point. */
        if (!cached && !(w->count & 127u)) fs_service();
        uint32_t off=offset(v,c), next;
        if (v->type==12 && off%512==511) {
            if ((e=fat_get_cluster(v,c,&next))) break; /* entry spans two sectors */
        } else {
            uint64_t s=v->reserved+(uint64_t)v->active_fat*v->fat_sectors+off/512;
            if (s!=loaded) {
                if (visited==(cached ? FAT_CHAIN_CACHED_SECTORS : FAT_CHAIN_SECTORS)) { e=-FS_EAGAIN; break; } /* yield */
                if ((e=readsec(v,s,b))) break;
                loaded=s; visited++;
            }
            uint32_t x=v->type==32 ? fs_rd32(b+off%512) : fs_rd16(b+off%512);
            if (v->type==12 && (c&1)) x>>=4;
            next=x&mask(v);
        }
        w->count++;
        if (eoc(v,next)) { c=0; break; }
        if (!next) { e=-FS_EIO; break; }
        c=next;
    }
    if (cached) {
        v->cache_only=false;
        if (e==-FS_EAGAIN && v->cache_miss!=UINT64_MAX) *miss=v->cache_miss;
    }
    w->cluster=c;
    return e;
}
int fat_transaction_run(struct fat_volume *source, const struct fat_transaction_args *a,
                         const struct fat_transaction_guard *guard) {
    if (!source || !a || !guard || !guard->enter || !guard->leave || !guard->retire || source->dev->logical) return -FS_EOPNOTSUPP;
    bool mutation=a->op!=FAT_TX_LOOKUP && a->op!=FAT_TX_NEXT && a->op!=FAT_TX_COMMIT;
    struct fat_transaction *t=transaction_alloc(mutation);
    if (!t) return -FS_ENOMEM;
    t->source=source; t->guard=*guard;
    t->op=a->op;
    int e;
    while ((e=guard->enter(guard->ctx))==-FS_EBUSY) {
        if ((e=fs_request_check())) break;
        if (guard->wait) { if ((e=guard->wait(guard->ctx))) break; }
        else fs_service();
    }
    if (e) { transaction_free(t); return e; }
    bool lookup=a->op==FAT_TX_LOOKUP || a->op==FAT_TX_NEXT, data_only=a->op==FAT_TX_LOG_WRITE;
    /* A lookup has read nothing yet: it simply starts from this snapshot. */
    if (a->op!=FAT_TX_COMMIT && !lookup && !data_only && source->mutation_sequence!=a->sequence) {
        guard->leave(guard->ctx); transaction_free(t); return -FS_EAGAIN;
    }
    t->view=*source; t->view.transaction=t; t->view.prepare_cache=0;
    t->view.publication_pending=false;
    t->sequence=source->mutation_sequence; t->generation=source->dev->media_generation;
    t->read_only=a->op==FAT_TX_LOOKUP || a->op==FAT_TX_NEXT || a->op==FAT_TX_COMMIT;
    t->free_synced=source->free_clusters;
    t->suspendable=a->op==FAT_TX_WRITE || a->op==FAT_TX_EXTEND || a->op==FAT_TX_RESERVE ||
        a->op==FAT_TX_TRUNCATE || a->op==FAT_TX_PUBLISH || a->op==FAT_TX_SET_METADATA;
    t->data_only=data_only;
    if (a->entry && a->op!=FAT_TX_CREATE && a->op!=FAT_TX_LOOKUP && a->op!=FAT_TX_NEXT) t->entry=*a->entry;
    t->target=a->entry; if (t->meta) t->meta->original=t->entry;
    t->target_initialized=a->op!=FAT_TX_CREATE && a->op!=FAT_TX_LOOKUP && a->op!=FAT_TX_NEXT;
    t->on_visible=a->visible; t->visible_ctx=a->visible_ctx;
    t->on_unlinked=a->unlinked; t->replacement_target=a->replacement;
    if (a->replacement && t->meta) t->meta->replacement=*a->replacement;
    if (a->cursor) t->cursor=*a->cursor;
    t->next=source->transactions; source->transactions=t;
    guard->leave(guard->ctx);
    struct fat_volume *v=&t->view;
    switch (a->op) {
    case FAT_TX_CREATE: e=fat_create(v,a->directory,a->name,a->attr,&t->entry); break;
    case FAT_TX_LOOKUP: e=lookup_dependency(v,a); if (!e) e=fat_lookup(v,a->directory,a->name,&t->entry); break;
    case FAT_TX_NEXT: e=lookup_dependency(v,a); if (!e) e=fat_next(v,a->directory,a->directory_cursor,&t->entry); break;
    case FAT_TX_WRITE:
        e=fat_write_cursor(v,&t->entry,&t->cursor,a->position,a->data,a->size,a->done); break;
    case FAT_TX_LOG_WRITE:
        e=fat_write_reserved_cursor(v,&t->entry,&t->cursor,a->capacity,a->position,a->data,a->size,a->done); break;
    case FAT_TX_PUBLISH: e=fat_publish_reserved(v,&t->entry,a->flag); break;
    case FAT_TX_EXTEND: e=fat_extend(v,&t->entry,a->capacity); break;
    case FAT_TX_TRUNCATE: e=fat_truncate(v,&t->entry,a->position); break;
    case FAT_TX_RESERVE: e=fat_reserve(v,&t->entry,a->capacity); break;
    case FAT_TX_REMOVE: e=fat_remove(v,&t->entry,a->flag); break;
    case FAT_TX_RENAME: e=fat_rename(v,&t->entry,a->directory,a->name); break;
    case FAT_TX_RENAME_REPLACE: e=fat_rename_replace(v,&t->entry,a->replacement ? &t->meta->replacement : 0,a->directory,a->name,a->flag); break;
    case FAT_TX_SET_METADATA: e=fat_set_metadata(v,&t->entry,a->attr,a->times); break;
    case FAT_TX_SET_LABEL: e=fat_set_label(v,a->name); break;
    case FAT_TX_COMMIT:
        e=cache_error(v->cache,v->dev);
        if (!e) e=v->readonly ? (v->writable_session ? -FS_EROFS : 0) :
            cache_barrier_record(v->cache,v->dev,&v->completion);
        if (e) e=write_error(v,e);
        break;
    default: e=-FS_EINVAL; break;
    }
    if (!e) e=transaction_publish(v,true);
    /* Retirement cannot be cancelled: keep the list/node pins until a
     * namespace turn is available, but retain no cache pins while waiting. */
    guard->retire(guard->ctx);
    {
        bool current=source->dev->media_generation==t->generation &&
            (source->mutation_sequence==t->sequence || data_only || (lookup && source->dev->media_generation==t->generation &&
             !source->dev->quarantined && source->mounted && reads_current(t)));
        if (!current && a->op!=FAT_TX_COMMIT && !(v->ro_reasons&FAT_RO_WRITE_ERROR)) e=-FS_EAGAIN;
        if (source->dev->media_generation!=t->generation) e=-FS_EIO;
        if (source->dev->media_generation==t->generation && t->durable_group>source->publication_durable)
            source->publication_durable=t->durable_group;
        if (!e) {
            if (a->entry && a->op!=FAT_TX_COMMIT && a->op!=FAT_TX_REMOVE) *a->entry=t->entry;
            if (a->replacement && t->meta) *a->replacement=t->meta->replacement;
            if (a->cursor) *a->cursor=t->cursor;
            if (a->op!=FAT_TX_LOOKUP) source->completion=v->completion;
            if (a->op==FAT_TX_SET_LABEL) memcpy(source->volume_label,v->volume_label,sizeof(source->volume_label));
        }
        if (v->readonly && !source->readonly && (current || (v->ro_reasons&FAT_RO_WRITE_ERROR)) && source->dev->media_generation==t->generation) {
            source->ro_reasons|=v->ro_reasons;
            fat_revoke_write(source,v->readonly_reason,v->readonly_cluster);
        }
        struct fat_transaction **link=&source->transactions;
        while (*link && *link!=t) link=&(*link)->next;
        if (*link) *link=t->next;
        guard->leave(guard->ctx);
    }
    transaction_free(t);
    return e;
}

/* G29b: whole-file COW relocation, under the caller's exclusive volume
 * reservation. The old chain remains owned until update() is durable. */
static void defrag_mark(struct fat_volume *v, struct fat_defrag_info *info, uint32_t c, uint8_t bits)
{
    unsigned cell=(unsigned)(((uint64_t)(c-2)*FAT_DEFRAG_CELLS)/v->clusters);
    info->map[cell]|=bits;
}
static int defrag_chain(struct fat_volume *v, uint32_t first, uint32_t *count,
                        bool *fragmented, struct fat_defrag_info *info, uint8_t bits)
{
    *count=0; *fragmented=false;
    if (!first) return 0;
    for (uint32_t c=first;*count<v->clusters;) {
        if (!valid(v,c)) return -FS_EIO;
        if (!(*count&127)) fs_service();
        uint32_t n; int e=fat_get_cluster(v,c,&n); if (e) return e;
        (*count)++;
        if (info) defrag_mark(v,info,c,bits);
        if (eoc(v,n)) return 0;
        if (n!=c+1) *fragmented=true;
        c=n;
    }
    return -FS_EIO;
}
/* Ordinal traversal never accepts a directory slot or cluster from userspace.
 * Rewalking is deliberate: other callers cannot forge a target directory. */
static int defrag_walk(struct fat_volume *v, struct fat_defrag_info *info,
                        uint32_t ordinal, struct fat_entry *target)
{
    struct { uint32_t dir, cursor; } stack[SCAN_DEPTH];
    unsigned depth=1; uint32_t index=0;
    stack[0].dir=0; stack[0].cursor=0;
    while (depth) {
        fs_service();
        struct fat_entry file;
        int e=fat_next(v,stack[depth-1].dir,&stack[depth-1].cursor,&file);
        if (e==-FS_ENOENT) { depth--; continue; }
        if (e) return e;
        if (path_equal(file.alias,".") || path_equal(file.alias,"..")) continue;
        if (!(file.attr&FAT_ATTR_DIR) && target && index++==ordinal) { *target=file; return 0; }
        if (info) {
            uint32_t count; bool fragmented;
            e=defrag_chain(v,file.first,&count,&fragmented,info,
                file.attr&FAT_ATTR_DIR ? FAT_MAP_DIRECTORY : FAT_MAP_USED);
            if (e) return e;
            if (!(file.attr&FAT_ATTR_DIR) && file.size) {
                info->files++;
                if (fragmented) {
                    info->fragmented++;
                    e=defrag_chain(v,file.first,&count,&fragmented,info,FAT_MAP_FRAGMENTED);
                    if (e) return e;
                }
            }
        }
        if (file.attr&FAT_ATTR_DIR) {
            if (depth==SCAN_DEPTH) return -FS_EIO;
            stack[depth].dir=file.first; stack[depth++].cursor=0;
        }
    }
    return target ? 1 : 0;
}
int fat_defrag_analyze(struct fat_volume *v, struct fat_defrag_info *info)
{
    if (!v || !info || !v->mounted || !v->clusters) return -FS_EINVAL;
    memset(info,0,sizeof(*info)); info->clusters=v->clusters;
    for (uint32_t c=2;c<v->clusters+2;c++) {
        if (!(c&127)) fs_service();
        uint32_t n; int e=fat_get_cluster(v,c,&n); if (e) return e;
        if (!n) info->free_clusters++;
        defrag_mark(v,info,c,!n ? FAT_MAP_FREE : n==mask(v)-8 ? FAT_MAP_BAD : FAT_MAP_USED);
    }
    if (v->type==32) {
        uint32_t count; bool fragmented;
        int e=defrag_chain(v,v->root,&count,&fragmented,info,FAT_MAP_DIRECTORY); if (e) return e;
    }
    return defrag_walk(v,info,0,0);
}
int fat_defrag_step(struct fat_volume *v, uint32_t ordinal, uint64_t expected, bool *moved)
{
    if (!v || !moved || !v->mounted || v->transaction || v->transactions) return -FS_EINVAL;
    *moved=false;
    if (expected!=v->mutation_sequence) return -FS_EAGAIN;
    int e=writable(v); if (e) return e;
    /* A cheap clean mount/removable insertion has not proved ownership.
     * Even with the optional early check off, reclaiming a source chain
     * requires that proof (a cross-link must never free another file). */
    if (v->scan_deferred || v->scan_fast || !v->scan_complete) {
        if ((e=fat_scan(v))) return e;
        if (v->ro_reasons&(FAT_RO_CORRUPT|FAT_RO_COPIES|FAT_RO_LOST)) return -FS_EROFS;
        fat_census_end(v); v->free_unknown=false;
    }
    struct fat_entry file;
    e=defrag_walk(v,0,ordinal,&file); if (e) return e;
    if (!file.size || !file.first) return 0;
    uint32_t count; bool fragmented;
    if ((e=defrag_chain(v,file.first,&count,&fragmented,0,0))) return e;
    if (!fragmented) return 0;
    uint32_t bytes=(uint32_t)v->spc*512;
    /* Preallocated/excess-tail files belong to the checker, not this tool. */
    if (count!=file.size/bytes+(file.size%bytes!=0)) return -FS_EIO;
    uint32_t run=0, first=0;
    for (uint32_t c=2;c<v->clusters+2;c++) {
        if (!(c&127)) fs_service();
        uint32_t n; if ((e=fat_get_cluster(v,c,&n))) return e;
        run=n ? 0 : run+1;
        if (run==count) { first=c-count+1; break; }
    }
    if (!first) return 0; /* Fragmentation is still reported after the pass. */
    uint8_t data[512]; uint32_t old=file.first;
    if ((e=publication_stage(v,FAT_P_DATA))) return e;
    for (uint32_t i=0,c=old;i<count;i++) {
        for (unsigned s=0;s<v->spc;s++) {
            if ((e=readsec(v,cluster_sector(v,c)+s,data)) ||
                (e=writesec(v,cluster_sector(v,first+i)+s,data))) return e;
        }
        uint32_t n; if ((e=fat_get_cluster(v,c,&n))) return e; c=n;
    }
    if ((e=barrier(v)) || (e=publication_stage(v,FAT_P_CHAIN))) return e;
    for (uint32_t i=0;i<count;i++)
        if ((e=set_cluster(v,first+i,i+1==count ? mask(v) : first+i+1))) return e;
    if ((e=barrier(v))) return e;
    v->free_clusters-=count;
    file.first=first;
    if ((e=update(v,&file))) return e;
    /* Directory publication has completed. Reclaim cannot damage its data. */
    if ((e=release_chain(v,old)) || (e=hints(v))) return e;
    *moved=true;
    return 0;
}

#include "fat_convert.inc"
