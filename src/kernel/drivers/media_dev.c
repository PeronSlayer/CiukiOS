/* Copied removable-media control. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/storage.h>
#include <ciuki/usb.h>

/* fx-17b: the floppy platform is optional (no FDC, host tests). */
bool floppy_drive_present(unsigned drive) __attribute__((weak));
int floppy_mount(unsigned drive) __attribute__((weak));
/* fx-21b: absent from storage-only host builds without the USB core. */
uint32_t usb_unrecognized_devices(void) __attribute__((weak));
static bool floppy_drive(unsigned d) { return d<2 && floppy_drive_present && floppy_drive_present(d); }
/* f5-12B2: CD audio devctls (MEDIA_CD_*) live in media_cd.c. */
int media_cd_devctl(struct process *p, struct storage *s, uint32_t op, uint32_t address, uint32_t bytes);
/* fx-26: Windows 98 broadcasts WM_DEVICECHANGE (DBT_DEVICEARRIVAL,
 * DBT_DEVICEREMOVECOMPLETE) when a volume arrives or leaves; here every
 * change of the drive namespace (vfs.c/mount.c: a volume published or
 * withdrawn) bumps one serial and wakes poll(), and /dev/media is readable
 * until its description has read the state again. */
extern void net_input_changed(void) __attribute__((weak));
static uint32_t media_serial = 1;
void vfs_namespace_changed(void)
{
    __atomic_fetch_add(&media_serial, 1u, __ATOMIC_RELEASE);
    if (net_input_changed) net_input_changed();
}
int media_poll_events(struct file_description *f)
{
    return !f->observed_valid || f->observed != __atomic_load_n(&media_serial, __ATOMIC_ACQUIRE) ? POLLIN : 0;
}
/* G6: Win32 SetVolumeLabel / FAT 1.03 label rules: printable ASCII, upper
 * case, no * ? . , ; : / \ | + = < > [ ] ", at most 11 bytes, space padded. */
static int media_label_encode(const char *in, char out[11])
{
    unsigned n=0;
    for (;n<12 && in[n];n++) {
        unsigned char c=(unsigned char)in[n];
        if (n==11 || c<32 || c>126) return -EINVAL;
        for (const char *bad="*?.,;:/\\|+=<>[]\"";*bad;bad++) if (c==(unsigned char)*bad) return -EINVAL;
        out[n]=(char)(c>='a' && c<='z' ? c-32 : c);
    }
    if (n==12) return -EINVAL;
    for (unsigned k=n;k<11;k++) out[k]=' ';
    return 0;
}
/* The label of drive r->drive in vfs: GET fills label/flags, SET (padded
 * non-null) publishes the new label first. Takes the namespace lock. */
static int media_label_locked_apply(struct vfs *vfs, struct ciuki_media_label *r, const char *padded)
{
    unsigned d=r->drive;
    if (d>=26) return -EINVAL;
    vfs_lock(vfs,0);
    struct fat_volume *v=vfs->volumes[d];
    struct fs_ro_volume *ro=vfs->ro_volumes[d];
    bool readonly=!v || v->readonly || (disk_write_denied && disk_write_denied(d));
    int e=v && v->mounted ? 0 : ro ? -EROFS : -ENOENT;
    if (!e && padded) {
        if (readonly) e=-EROFS;
        else {
            uint32_t turn=0;
            e=px_error(vfs_turn_take_locked(vfs,vfs_drive_mask(d),&turn));
            if (!e) {
                vfs_reserve_locked(vfs,vfs_drive_mask(d));
                vfs_tag_locked(vfs,"label"); vfs_site_locked(vfs,"native");
                e=v==vfs->volumes[d] && v->mounted ? px_error(vfs_fat_transaction_locked(vfs,v,
                    (struct fat_transaction_args){.op=FAT_TX_SET_LABEL,.name=padded})) : -EIO;
            }
            vfs_turn_drop_locked(vfs,turn);
        }
    }
    if (!e || (e==-EROFS && !padded)) {
        e=0;
        memset(r->label,0,sizeof(r->label));
        if (v) memcpy(r->label,v->volume_label,11);
        else if (ro) memcpy(r->label,ro->volume_label,11);
        r->flags=readonly ? CIUKI_MEDIA_LABEL_READ_ONLY : 0;
    }
    vfs_unlock(vfs);
    return e;
}
static int media_label(struct storage *s,uint32_t op,uint32_t address,uint32_t bytes)
{
    struct ciuki_media_label r;
    if (bytes!=sizeof(r)) return -EINVAL;
    int e=copy_from_user(&r,address,sizeof(r)); if (e) return e;
    if (r.version!=CIUKI_MEDIA_VERSION) return -EOPNOTSUPP;
    if (r.size!=sizeof(r) || r.drive>=26 || r.reserved || r.flags) return -EINVAL;
    char padded[11];
    if (op==MEDIA_SET_LABEL) {
        bool terminated=false; for (unsigned k=0;k<sizeof(r.label);k++) if (!r.label[k]) terminated=true;
        if (!terminated) return -EINVAL;
        if ((e=media_label_encode(r.label,padded))) return e;
    }
    if (!s || !s->ready) return -ENOENT;
    e=media_label_locked_apply(&s->vfs,&r,op==MEDIA_SET_LABEL ? padded : 0);
    return e ? e : op==MEDIA_SET_LABEL ? 0 : copy_to_user(address,&r,sizeof(r));
}
struct floppy_image_operation {
    struct process *process;
    struct ua_pin pin;
    uint32_t address;
    bool pinned;
};
static void floppy_image_free(void *arg)
{
    struct floppy_image_operation *o=arg;
    if (o->pinned) ua_unpin(o->process->memory,&o->pin);
    kfree(o);
}
static int floppy_user_copy(void *ctx,uint32_t offset,void *buffer,uint32_t bytes,bool output)
{
    struct floppy_image_operation *o=ctx;
    return output ? copy_to_user(o->address+offset,buffer,bytes) : copy_from_user(buffer,o->address+offset,bytes);
}
static int floppy_image_call(struct process *p, struct storage *s, uint32_t op, uint32_t address, uint32_t bytes)
{
    struct ciuki_floppy_image r;
    if (bytes!=sizeof(r)) return -EINVAL;
    int e=copy_from_user(&r,address,sizeof(r)); if (e) return e;
    if (r.version!=CIUKI_MEDIA_VERSION || r.size!=sizeof(r) || r.drive>=2 || !r.generation ||
        !r.sectors || r.sectors>5760 || !r.buffer || r.flags || r.reserved) return -EINVAL;
    uint32_t length=r.sectors*512;
    struct floppy_image_operation *o=kzalloc(sizeof(*o)); if (!o) return -ENOMEM;
    o->process=p; o->address=r.buffer;
    struct proc_thread *thread=proc_thread_for(g_current);
    if (thread) { thread->cleanup=floppy_image_free; thread->operation=o; }
    if (!e) { e=ua_pin(p->memory,&o->pin,r.buffer,length,op==MEDIA_FLOPPY_READ); o->pinned=!e; }
    if (!e) e=px_error(storage_floppy_copy(s,&r,floppy_user_copy,o,op==MEDIA_FLOPPY_WRITE));
    if (thread) { thread->cleanup=0; thread->operation=0; }
    floppy_image_free(o); return e;
}
int media_devctl(struct process *p,int32_t fd,uint32_t op,uint32_t address,uint32_t bytes)
{
    if (!file_fd_object(p,fd)) return -EBADF;
    struct file_description *f=file_fd(p,fd);
    if (!f || f->node->kind!=PX_MEDIA) return -ENOTTY;
    struct storage *s=storage_get();
    if (op==MEDIA_FLOPPY_READ || op==MEDIA_FLOPPY_WRITE) return floppy_image_call(p,s,op,address,bytes);
    if (op>=MEDIA_CD_STATE && op<=MEDIA_CD_POSITION) return media_cd_devctl(p,s,op,address,bytes);
    if (op==MEDIA_GET_LABEL || op==MEDIA_SET_LABEL) return media_label(s,op,address,bytes);
    if (op==MEDIA_GET_CONVERT || op==MEDIA_CONVERT) {
        struct ciuki_media_convert r;
        if (bytes!=sizeof(r)) return -EINVAL;
        int e=copy_from_user(&r,address,sizeof(r)); if (e) return e;
        if (r.version!=CIUKI_MEDIA_VERSION) return -EOPNOTSUPP;
        e=px_error(storage_convert(s,&r,op==MEDIA_CONVERT));
        return e ? e : copy_to_user(address,&r,sizeof(r));
    }
    if (op==MEDIA_DEFRAG_ANALYZE || op==MEDIA_DEFRAG_STEP) {
        struct ciuki_media_defrag r;
        if (bytes!=sizeof(r)) return -EINVAL;
        int e=copy_from_user(&r,address,sizeof(r)); if (e) return e;
        if (r.version!=CIUKI_MEDIA_VERSION) return -EOPNOTSUPP;
        e=px_error(storage_defrag(s,&r,op==MEDIA_DEFRAG_STEP));
        return e ? e : copy_to_user(address,&r,sizeof(r));
    }
    if (op==MEDIA_GET_FORMAT || op==MEDIA_FORMAT) {
        struct ciuki_media_format r;
        if (bytes!=sizeof(r)) return -EINVAL;
        int e=copy_from_user(&r,address,sizeof(r)); if (e) return e;
        if (r.version!=CIUKI_MEDIA_VERSION) return -EOPNOTSUPP;
        if (r.size!=sizeof(r) || r.drive>=26 || !r.generation || r.reserved) return -EINVAL;
        e=px_error(storage_format_removable(s,&r,op==MEDIA_FORMAT));
        return e ? e : copy_to_user(address,&r,sizeof(r));
    }
    /* Namespace publication selects immutable mount identity/label fields.
     * Queries never invoke filesystem/transport callbacks or reserve a drive. */
    if (op==MEDIA_GET_BLOCKERS) {
        /* Static copied output avoids a large device-worker stack frame.
         * No wait between clearing/filling this record and copying it out. */
        static struct ciuki_media_blockers out;
        struct ciuki_media_remove r;
        if (bytes!=sizeof(out)) return -EINVAL;
        int e=copy_from_user(&r,address,sizeof(r)); if (e) return e;
        if (r.version!=CIUKI_MEDIA_VERSION) return -EOPNOTSUPP;
        if (r.size!=sizeof(out) || r.drive<3 || r.drive>=26 || !r.generation) return -EINVAL;
        if (!s || !s->ready || s->stopped) return -ENODEV;
        vfs_lock(&s->vfs,0);
        struct storage_volume *fat=&s->volumes[r.drive];
        struct fs_ro_volume *ro=s->vfs.ro_volumes[r.drive];
        struct blkdev *parent=fat->present && fat->removable && s->vfs.volumes[r.drive]==&fat->fat ? fat->part.parent : 0;
        bool optical=false;
        for (unsigned slot=0;slot<STORAGE_DISKS;slot++) if (ro==&s->optical[slot]) optical=true;
        if (!optical) ro=0;
        struct blkdev *dev=parent ? parent : ro ? ro->dev : 0;
        if (!dev || dev->quarantined || dev->media_generation!=r.generation) {
            vfs_unlock(&s->vfs); return -EIO;
        }
        memset(&out,0,sizeof(out));
        out.version=r.version; out.size=sizeof(out); out.drive=r.drive; out.generation=r.generation;
        for (unsigned slot=0;slot<CIUKI_PROCESS_MAX;slot++) {
            struct process *owner=proc_at_slot(slot);
            if (!owner || !owner->fds) continue;
            for (unsigned fdno=0;fdno<CIUKI_OPEN_MAX;fdno++) {
                struct file_description *f=file_fd(owner,(int32_t)fdno);
                if (!f || !f->node || f->node->space->vfs!=&s->vfs) continue;
                struct px_node *n=f->node;
                bool match=ro && n->ro==ro && n->linked && n->generation==s->vfs.generation[r.drive];
                if (parent) for (unsigned d=4;d<26;d++) {
                    struct storage_volume *v=&s->volumes[d];
                    if (v->present && v->removable && v->part.parent==parent && n->volume==&v->fat) match=true;
                }
                if (!match) continue;
                struct ciuki_media_program *program=&out.programs[out.count++];
                program->pid=owner->pid;
                ksnprintf(program->name,sizeof(program->name),"%s",owner->name);
                break;
            }
        }
        int copied=copy_to_user(address,&out,sizeof(out));
        vfs_unlock(&s->vfs);
        return copied;
    }
    if (op==MEDIA_FLOPPY_LOAD) {
        /* fx-17b: Retry on "Please insert a disk into drive A:". Windows 98
         * reads a diskette on access; there is no insertion notification. */
        struct ciuki_media_remove r;
        if (bytes!=sizeof(r)) return -EINVAL;
        int e=copy_from_user(&r,address,sizeof(r)); if (e) return e;
        if (r.version!=CIUKI_MEDIA_VERSION) return -EOPNOTSUPP;
        if (r.size!=sizeof(r) || r.drive>=2 || r.generation) return -EINVAL;
        if (!floppy_drive(r.drive) || !floppy_mount) return -ENODEV;
        e=floppy_mount(r.drive);
        return !e ? 0 : e==-FS_EBUSY ? -EBUSY : -ENXIO;
    }
    if (op==MEDIA_GET_SPACE) {
        struct ciuki_media_space sp;
        if (bytes!=sizeof(sp)) return -EINVAL;
        int e=copy_from_user(&sp,address,bytes); if (e) return e;
        if (sp.version!=CIUKI_MEDIA_VERSION) return -EOPNOTSUPP;
        if (sp.size!=sizeof(sp) || sp.drive>=26 || sp.flags || sp.cluster_bytes || sp.total_clusters ||
            sp.free_clusters || sp.reserved) return -EINVAL;
        unsigned d=sp.drive; e=-ENOENT;
        if (s && s->ready) {
            vfs_lock(&s->vfs,0);
            struct fat_volume *v=s->vfs.volumes[d];
            if (v && v->mounted) {
                bool known; sp.free_clusters=fat_free_estimate(v,&known);
                sp.flags=known ? 0 : CIUKI_MEDIA_SPACE_COUNTING;
                sp.cluster_bytes=(uint32_t)v->spc*512; sp.total_clusters=v->clusters; e=0;
            }
            vfs_unlock(&s->vfs);
        }
        return e ? e : copy_to_user(address,&sp,bytes);
    }
    if (op==MEDIA_GET_VOLUME) {
        struct ciuki_media_volume v;
        if (bytes!=sizeof(v)) return -EINVAL;
        int e=copy_from_user(&v,address,bytes); if (e) return e;
        if (v.version!=CIUKI_MEDIA_VERSION) return -EOPNOTSUPP;
        if (v.size!=sizeof(v) || v.drive==2 || v.drive>=26 || (v.drive<2 && !floppy_drive(v.drive)) || v.flags || v.generation ||
            v.reserved[0] || v.reserved[1] || v.reserved[2]) return -EINVAL;
        unsigned d=v.drive;
        memset(&v,0,sizeof(v)); v.version=CIUKI_MEDIA_VERSION; v.size=sizeof(v); v.drive=d;
        if (s && s->ready) {
            vfs_lock(&s->vfs,0);
            struct storage_volume *fat=&s->volumes[d];
            if (fat->present && fat->removable && s->vfs.volumes[d] && !fat->part.parent->quarantined) {
                v.flags=CIUKI_MEDIA_REMOVABLE; v.generation=s->vfs.generation[d];
                if (fat->fat.volume_serial) ksnprintf(v.id,sizeof(v.id),"fat-%08x-%llu",fat->fat.volume_serial,(unsigned long long)fat->fat.sectors);
                memcpy(v.label,fat->fat.volume_label,sizeof(fat->fat.volume_label));
            }
            struct fs_ro_volume *ro=s->vfs.ro_volumes[d];
            if (ro && ro->ops==&iso9660_ops && !ro->dev->quarantined && ro->dev->media_generation==ro->mount_generation) {
                v.flags=CIUKI_MEDIA_OPTICAL; v.generation=s->vfs.generation[d];
                ksnprintf(v.id,sizeof(v.id),"iso-%016llx",(unsigned long long)ro->volume_identity);
                memcpy(v.label,ro->volume_label,sizeof(v.label));
            }
            vfs_unlock(&s->vfs);
        }
        return copy_to_user(address,&v,bytes);
    }
    if (op==MEDIA_GET_STATE) {
        struct ciuki_media_state state;
        if (bytes!=sizeof(state) && bytes!=sizeof(struct ciuki_media_state_v1)) return -EINVAL;
        int e=copy_from_user(&state,address,bytes); if (e) return e;
        uint32_t version=state.version;
        if (version!=CIUKI_MEDIA_VERSION && version!=CIUKI_MEDIA_VERSION_1) return -EOPNOTSUPP;
        if (state.size!=bytes || state.usb_unrecognized ||
            bytes!=(version==CIUKI_MEDIA_VERSION ? sizeof(state) : sizeof(struct ciuki_media_state_v1))) return -EINVAL;
        memset(&state,0,sizeof(state)); state.version=version; state.size=bytes;
        /* fx-26: the serial is taken before the state, so a change made
         * while this snapshot runs leaves the description readable. */
        f->observed=__atomic_load_n(&media_serial,__ATOMIC_ACQUIRE); f->observed_valid=true;
        /* fx-21b: v1 keeps its reserved word zero. */
        if (version==CIUKI_MEDIA_VERSION && usb_unrecognized_devices) state.usb_unrecognized=usb_unrecognized_devices();
        if (s && s->ready) {
            vfs_lock(&s->vfs,0);
            for (unsigned d=4;d<26;d++) {
                struct storage_volume *v=&s->volumes[d];
                if (v->present && v->removable && (s->vfs.volumes[d] || v->unformatted) && !v->part.parent->quarantined) {
                    state.removable_mask|=1u<<d; state.generation[d]=v->part.parent->media_generation;
                    state.type[d]=CIUKI_MEDIA_USB;
                } else if (v->safely_removed && v->part.parent) {
                    state.generation[d]=v->part.parent->media_generation;
                    state.type[d]=CIUKI_MEDIA_SAFE_REMOVED;
                }
            }
            for (unsigned d=0;d<2;d++) if (floppy_drive(d)) {
                /* fx-17b: A:/B: always listed; the bit only with a diskette. */
                struct storage_volume *v=&s->volumes[d];
                state.type[d]=CIUKI_MEDIA_FLOPPY;
                if (v->present && (s->vfs.volumes[d] || v->unformatted) && v->part.parent && !v->part.parent->quarantined) {
                    state.removable_mask|=1u<<d; state.generation[d]=v->part.parent->media_generation;
                }
            }
            for (unsigned d=3;d<26;d++) {
                struct fs_ro_volume *ro=s->vfs.ro_volumes[d];
                if (!ro || ro->dev->quarantined || ro->mount_generation!=ro->dev->media_generation) continue;
                for (unsigned slot=0;slot<STORAGE_DISKS;slot++) if (ro==&s->optical[slot]) {
                    state.removable_mask|=1u<<d; state.generation[d]=ro->dev->media_generation;
                    state.type[d]=CIUKI_MEDIA_OPTICAL;
                }
            }
            vfs_unlock(&s->vfs);
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
