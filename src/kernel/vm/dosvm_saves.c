/* Idle title operations; the same reservation excludes launch and removal.
 * sec-k05: always the calling user's own store (DOSVM SAVE_QUERY/RESET/
 * EXPORT are own-session); administrators change another user's data only
 * through explicit file management. The table runs as the caller's subject.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/abi.h>
#include <ciuki/kernel.h>
#include <ciuki/storage.h>
#include "dos_save_store.h"
#include "dos_identity.h"
#include <ciuki/sha256.h>
int dosvm_game_remove_reserve(unsigned);
void dosvm_game_remove_release(unsigned);
struct save_manager {
    struct vfs_table table;
    struct dos_save save;
    struct dos_save_file file;
    int base;
    uint32_t sectors;
    uint8_t buffer[4096];
    /* Page-backed, not on the kernel stack. */
    char root[DOS_IDENTITY_PATH],destination[DOS_IDENTITY_PATH],path[DOS_SAVE_PATH],folder[DOS_SAVE_PATH];
};
static int manager_base_read(struct blkdev *d,uint64_t lba,uint32_t count,void *out)
{
    struct save_manager *m=d->ctx; int e=blkdev_range(d,lba,count); if (e) return e;
    memset(out,0,(size_t)count*512u);
    if (lba>=m->sectors) return 0;
    if (count>m->sectors-lba) count=m->sectors-(uint32_t)lba;
    uint64_t pos; size_t got=0;
    e=vfs_seek(&m->table,m->base,(int64_t)(lba*512u),VFS_SEEK_SET,&pos);
    if (!e) e=vfs_read(&m->table,m->base,out,(size_t)count*512u,&got);
    return e ? e : got==(size_t)count*512u ? 0 : -FS_EIO;
}
static int manager_merged_read(struct blkdev *d,uint64_t lba,uint32_t count,void *out)
{
    struct save_manager *m=d->ctx; int e=blkdev_range(d,lba,count); if (e) return e;
    for (unsigned i=0;!e && i<count;i++) {
        uint8_t *sector=(uint8_t *)out+i*512u;
        e=dos_save_read(&m->save,(uint32_t)lba+i,sector);
        if (e==-FS_ENOENT) e=manager_base_read(d,lba+i,1,sector);
    }
    return e;
}
static int manager_export(struct save_manager *m,unsigned game,struct ciuki_dosvm_command *c,
                           struct storage *storage,const struct dos_identity *owner)
{
    struct fat_entry entry;
    int e=vfs_stat(&m->table,"C:\\SYSTEM\\DOS\\UNIFIED.IMG",&entry); if (e) return e;
    if ((entry.attr&FAT_ATTR_DIR) || !entry.size || entry.size%512u || entry.size/512u>DOS_SAVE_SECTORS) return -FS_EIO;
    m->sectors=entry.size/512u;
    m->base=vfs_open(&m->table,"C:\\SYSTEM\\DOS\\UNIFIED.IMG",VFS_READ,VFS_DENY_WRITE,0);
    if (m->base<0) return m->base;
    struct dos_save_identity id={.game=game,.kernel=CIUKI_DOSVM_UNIFIED};
    struct sha256_ctx hash; sha256_init(&hash);
    for (uint32_t at=0;!e && at<entry.size;) {
        size_t n=entry.size-at,got=0; if (n>sizeof(m->buffer)) n=sizeof(m->buffer);
        e=vfs_read(&m->table,m->base,m->buffer,n,&got);
        if (!e && got!=n) e=-FS_EIO;
        if (!e) { sha256_update(&hash,m->buffer,n); at+=(uint32_t)n; fs_service(); }
    }
    uint8_t digest[32]; sha256_final(&hash,digest); sha256_hex(digest,id.base);
    /* Export never archives a stale overlay or creates an empty replacement.
     * A changed installed base must first be resolved by the launch path. */
    ksnprintf(m->path,sizeof(m->path),"%s\\G%uK%u%s.COW",m->root,game,id.kernel,id.base);
    if (!e) e=dos_identity_root(owner,DOS_ROOT_EXPORT,m->destination,sizeof(m->destination));
    if (!e) e=vfs_stat(&m->table,m->path,&entry);
    if (!e) {
        m->file=(struct dos_save_file){.table=&m->table,.writable=false};
        m->file.fd=vfs_open(&m->table,m->path,VFS_READ,VFS_DENY_WRITE,0);
        if (m->file.fd<0) e=m->file.fd;
        else {
            struct dos_save_io io; dos_save_file_io(&m->file,&io);
            e=dos_save_open(&m->save,&io,&id,entry.size);
            if (e==DOS_SAVE_MISMATCH) e=-FS_EIO;
            if (!e) {
                struct blkdev base={.read=manager_base_read,.capacity=DOS_SAVE_SECTORS,.sector_size=512,
                    .logical=true,.ctx=m};
                struct blkdev merged=base; merged.read=manager_merged_read;
                e=dos_identity_prepare(owner,&m->table,DOS_ROOT_EXPORT);
                if (!e) e=dos_save_store_export(&m->table,&merged,&base,&storage->cache,game,m->destination,m->folder,&c->changed);
                /* The user sees a folder in their own Documents, not a
                 * profile spelling (name holds 31 characters). */
                if (!e) e=dos_identity_export_name(owner,m->folder,c->name);
            }
            dos_save_release(&m->save);
            int ce=vfs_close(&m->table,m->file.fd); if (!e) e=ce;
        }
    }
    int ce=vfs_close(&m->table,m->base); return e ? e : ce;
}
int dosvm_game_data(unsigned game,unsigned op,struct ciuki_dosvm_command *c,const struct dos_identity *owner)
{
    if (!game || game>CIUKI_DOSVM_GAME_COUNT || op<CIUKI_DOSVM_SAVE_QUERY || op>CIUKI_DOSVM_SAVE_EXPORT || !owner) return -EINVAL;
    int e=dosvm_game_remove_reserve(game); if (e && op!=CIUKI_DOSVM_SAVE_QUERY) return e;
    bool reserved=!e;
    struct storage *storage=storage_get();
    if (!storage || !storage->ready || storage->stopped) { e=-FS_EIO; goto release; }
    struct save_manager *m=fs_contig_alloc(sizeof(*m));
    if (!m) { e=-FS_ENOMEM; goto release; }
    e=dos_identity_root(owner,DOS_ROOT_STORE,m->root,sizeof(m->root));
    if (!e) e=vfs_table_init(&storage->vfs,&m->table,2);
    if (!e) {
        e=dos_identity_bind(owner,&m->table);
        /* A user without any store yet has no data: zero, not an error. */
        if (!e && op==CIUKI_DOSVM_SAVE_QUERY) {
            e=dos_save_store_query(&m->table,m->root,game,&c->changed); c->reason=DOS_SAVE_QUOTA;
            if (e==-FS_ENOENT) { c->changed=0; e=0; }
        } else if (!e && op==CIUKI_DOSVM_SAVE_RESET) {
            e=dos_save_store_reset(&m->table,m->root,game);
            if (e==-FS_ENOENT) e=0;
        } else if (!e) e=manager_export(m,game,c,storage,owner);
        vfs_table_destroy(&m->table);
    }
    fs_contig_free(m,sizeof(*m));
release:
    if (reserved) dosvm_game_remove_release(game);
    return e;
}
