/* Capability-scoped, bounded native FAT inspection for Setup.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/setup_mode.h>

static bool chain_end(const struct fat_volume *v,uint32_t c)
{ return c >= (v->type==12 ? 0xff8u : v->type==16 ? 0xfff8u : 0x0ffffff8u); }

static int setup_media_live(struct px_node *n)
{
    struct vfs *v=n->space->vfs;
    struct blkdev *dev=n->volume->dev;
    for(unsigned depth=0;depth<8;depth++) {
        if(dev->quarantined)return -EIO;
        if(dev->media_check) { int e=px_error(VFS_IO(v,dev->media_check(dev))); if(e)return e; }
        if(!dev->request_parent)return 0;
        if(dev->media_generation!=dev->request_parent->media_generation)return -EIO;
        dev=dev->request_parent;
    }
    return -EIO;
}

int file_setup_fat(struct process *p,struct file_description *d,uint32_t op,struct ciuki_setup_fat *q)
{
    if (!p || !p->setup_capability || !setup_mode()) return -EPERM;
    if (!d || !q) return -EBADF;
    if (op==CIUKI_SETUP_FAT_ATTRIB && (!q->mount_generation || !q->chain_generation)) return -EINVAL;
    if (q->version!=CIUKI_SETUP_FAT_VERSION || q->size!=sizeof(*q) ||
        (op!=CIUKI_SETUP_FAT_QUERY && op!=CIUKI_SETUP_FAT_ATTRIB)) return -EINVAL;
    struct px_node *n=d->node;
    if (!n || !n->volume || (n->kind!=PX_FILE && n->kind!=PX_DIRECTORY)) return -ENOTSUP;
    struct vfs *v=n->space->vfs;
    vfs_lock(v,0);
    uint32_t turn=0;
    int e=op==CIUKI_SETUP_FAT_ATTRIB ? px_error(vfs_turn_take_locked(v,vfs_drive_mask(n->drive),&turn)) : 0;
    if (!e) vfs_reserve_locked(v,vfs_drive_mask(n->drive));
    if (!e) e=px_ro_check_locked(n);
    if (!e && !vfs_identity_live(v,d->identity)) e=-EIO;
    if (!e) e=setup_media_live(n);
    uint32_t generation=n->generation; uint64_t chain=n->entry.chain_generation;
    if (!e && !n->linked) e=-ENOENT;
    if (!e && ((q->mount_generation && q->mount_generation!=n->generation) ||
               (q->chain_generation && q->chain_generation!=n->entry.chain_generation))) e=-EAGAIN;
    if (!e && op==CIUKI_SETUP_FAT_ATTRIB) {
        if (q->attributes>63 || ((q->attributes^n->entry.attr)&(FAT_ATTR_DIR|FAT_ATTR_VOLUME))) e=-EINVAL;
        else if (px_write_denied(n) || n->volume->readonly) e=-EROFS;
        else {
            e=px_error(px_fat_transaction_locked(n,n->parent,0,(struct fat_transaction_args){
                .op=FAT_TX_SET_METADATA,.entry=&n->entry,.attr=(uint8_t)q->attributes}));
            if (!e) px_changed_locked(n);
        }
    }
    uint64_t lba=0;
    if (!e && op==CIUKI_SETUP_FAT_QUERY && q->cluster_index!=CIUKI_SETUP_INDEX_NONE) {
        struct fat_volume *vol=n->volume;
        uint64_t count=((uint64_t)n->entry.size+vol->spc*512u-1)/(vol->spc*512u);
        uint32_t index=q->cluster_index;
        if (n->kind!=PX_FILE) e=-EISDIR;
        else if (!q->mount_generation || !q->chain_generation) e=-EINVAL;
        else if (index>count || index>vol->clusters) e=-EINVAL;
        else {
            struct fat_cursor *c=&d->fat_cursor;
            if (c->generation!=n->entry.chain_generation || c->first!=n->entry.first || c->index>index)
                *c=(struct fat_cursor){.generation=n->entry.chain_generation,.first=n->entry.first,.cluster=n->entry.first};
            /* At most 128 FAT links per call. Sequential callers reuse the
             * retained description's cursor; random seeks are bounded too. */
            if (index-c->index>128) e=-E2BIG;
            while (!e && c->index<index && c->cluster && !chain_end(vol,c->cluster)) {
                uint32_t next;
                if (c->cluster<2 || c->cluster>=vol->clusters+2) { e=-EIO; break; }
                e=px_error(VFS_IO(v,fat_get_cluster(vol,c->cluster,&next)));
                if (!e) { c->cluster=next; c->index++; }
            }
            if (!e && (!c->cluster || chain_end(vol,c->cluster))) e=index==count ? -ENOENT : -EIO;
            else if (!e && (index==count || c->cluster<2 || c->cluster>=vol->clusters+2)) e=-EIO;
            else if (!e) {
                lba=vol->start+vol->data_sector+(uint64_t)(c->cluster-2)*vol->spc;
                const struct blkdev *dev=vol->dev;
                for (unsigned depth=0;dev->request_parent && depth<8;depth++) {
                    if (lba>UINT64_MAX-dev->request_offset) { e=-EOVERFLOW; break; }
                    lba+=dev->request_offset; dev=dev->request_parent;
                }
                if (!e && dev->request_parent) e=-EIO;
                if (!e && (lba>=dev->capacity || vol->spc>dev->capacity-lba)) e=-EIO;
            }
            if (e && e!=-ENOENT) *c=(struct fat_cursor){0};
        }
    }
    if (!e && (generation!=n->generation || chain!=n->entry.chain_generation)) e=-EAGAIN;
    if (!e) e=px_ro_check_locked(n);
    if (!e && !vfs_identity_live(v,d->identity)) e=-EIO;
    if (!e) e=setup_media_live(n);
    if (!e) {
        uint32_t index=q->cluster_index;
        *q=(struct ciuki_setup_fat){.version=CIUKI_SETUP_FAT_VERSION,.size=sizeof(*q),
            .mount_generation=n->generation,.attributes=n->entry.attr,.cluster_index=index,
            .cluster_sectors=n->volume->spc,.file_size=n->entry.size,
            .chain_generation=n->entry.chain_generation,.lba=lba};
        ksnprintf(q->name,sizeof(q->name),"%s",n->entry.name);
        ksnprintf(q->alias,sizeof(q->alias),"%s",n->entry.alias);
    }
    vfs_turn_drop_locked(v,turn);
    vfs_unlock(v);
    return e;
}
