/* Copied removable-media control. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/storage.h>
#include <ciuki/usb.h>
int media_devctl(struct process *p,int32_t fd,uint32_t op,uint32_t address,uint32_t bytes)
{
    if (!file_fd_object(p,fd)) return -EBADF;
    struct file_description *f=file_fd(p,fd);
    if (!f || f->node->kind!=PX_MEDIA) return -ENOTTY;
    struct storage *s=storage_get();
    if (op==MEDIA_GET_VOLUME) {
        struct ciuki_media_volume v;
        if (bytes!=sizeof(v)) return -EINVAL;
        int e=copy_from_user(&v,address,bytes); if (e) return e;
        if (v.version!=CIUKI_MEDIA_VERSION) return -EOPNOTSUPP;
        if (v.size!=sizeof(v) || v.drive<3 || v.drive>=26 || v.flags || v.generation ||
            v.reserved[0] || v.reserved[1] || v.reserved[2]) return -EINVAL;
        unsigned d=v.drive;
        memset(&v,0,sizeof(v)); v.version=CIUKI_MEDIA_VERSION; v.size=sizeof(v); v.drive=d;
        if (s && s->ready) {
            fs_lock_take(&s->vfs.lock);
            struct storage_volume *fat=&s->volumes[d];
            if (fat->present && fat->removable && s->vfs.volumes[d] && !fat->part.parent->quarantined) {
                v.flags=CIUKI_MEDIA_REMOVABLE; v.generation=s->vfs.generation[d];
                if (fat->fat.volume_serial) ksnprintf(v.id,sizeof(v.id),"fat-%08x-%llu",fat->fat.volume_serial,(unsigned long long)fat->fat.sectors);
                memcpy(v.label,fat->fat.volume_label,sizeof(fat->fat.volume_label));
            }
            struct fs_ro_volume *ro=s->vfs.ro_volumes[d];
            if (ro && ro->ops==&iso9660_ops && ro->mounted && !ro->dev->quarantined && ro->ops->generation(ro)==ro->mount_generation) {
                v.flags=CIUKI_MEDIA_OPTICAL; v.generation=s->vfs.generation[d];
                ksnprintf(v.id,sizeof(v.id),"iso-%016llx",(unsigned long long)ro->volume_identity);
                memcpy(v.label,ro->volume_label,sizeof(v.label));
            }
            fs_lock_drop(&s->vfs.lock);
        }
        return copy_to_user(address,&v,bytes);
    }
    if (op==MEDIA_GET_STATE) {
        struct ciuki_media_state state;
        if (bytes!=sizeof(state) && bytes!=sizeof(struct ciuki_media_state_v1)) return -EINVAL;
        int e=copy_from_user(&state,address,bytes); if (e) return e;
        uint32_t version=state.version;
        if (version!=CIUKI_MEDIA_VERSION && version!=CIUKI_MEDIA_VERSION_1) return -EOPNOTSUPP;
        if (state.size!=bytes || state.reserved ||
            bytes!=(version==CIUKI_MEDIA_VERSION ? sizeof(state) : sizeof(struct ciuki_media_state_v1))) return -EINVAL;
        memset(&state,0,sizeof(state)); state.version=version; state.size=bytes;
        if (s && s->ready) {
            fs_lock_take(&s->vfs.lock);
            for (unsigned d=4;d<26;d++) {
                struct storage_volume *v=&s->volumes[d];
                if (v->present && v->removable && s->vfs.volumes[d] && !v->part.parent->quarantined) {
                    state.removable_mask|=1u<<d; state.generation[d]=v->part.parent->media_generation;
                    state.type[d]=CIUKI_MEDIA_USB;
                }
            }
            for (unsigned d=3;d<26;d++) {
                struct fs_ro_volume *ro=s->vfs.ro_volumes[d];
                if (!ro || !ro->mounted || ro->dev->quarantined || ro->mount_generation!=ro->dev->media_generation) continue;
                for (unsigned slot=0;slot<STORAGE_DISKS;slot++) if (ro==&s->optical[slot]) {
                    state.removable_mask|=1u<<d; state.generation[d]=ro->dev->media_generation;
                    state.type[d]=CIUKI_MEDIA_OPTICAL;
                }
            }
            fs_lock_drop(&s->vfs.lock);
        }
        return copy_to_user(address,&state,bytes);
    }
    if (op==MEDIA_SAFE_REMOVE) {
        struct ciuki_media_remove r;
        if (bytes!=sizeof(r)) return -EINVAL;
        int e=copy_from_user(&r,address,bytes); if (e) return e;
        if (r.version!=CIUKI_MEDIA_VERSION && r.version!=CIUKI_MEDIA_VERSION_1) return -EOPNOTSUPP;
        if (r.size!=sizeof(r) || r.drive<3 || r.drive>=26 || !r.generation) return -EINVAL;
        e=optical_eject(s,r.drive,r.generation);
        if (e!=-FS_ENOENT) return px_error(e);
        return px_error(usb_storage_eject(s,r.drive,r.generation));
    }
    return -ENOTTY;
}
