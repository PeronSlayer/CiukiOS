/* One process fd table for files, streams and the f2-05 object family.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/desktop.h>
#include <ciuki/supervisor.h>
#include <ciuki/signal.h>
#include <ciuki/audio.h>
#include <ciuki/com.h>
#pragma weak com_device_close
#include <ciuki/lpt.h>
#pragma weak lpt_device_close
#pragma weak audio_device_close
#pragma weak px_fat_transaction_locked
#pragma weak vfs_file_wait_locked
#pragma weak vfs_turn_take_locked
#pragma weak vfs_turn_drop_locked
uint32_t net_socket_count(void) __attribute__((weak));
uint32_t *net_socket_flags(struct proc_object *) __attribute__((weak));
void net_fd_changed(struct process *, unsigned) __attribute__((weak));
#define NET_OBJECTS (net_socket_count ? net_socket_count() : 0)

static uint32_t file_descriptions;
uint32_t file_description_count(void) { return __atomic_load_n(&file_descriptions,__ATOMIC_RELAXED); }
static int image_open(void *cwd, const char *path, struct ciuki_file *out);
static int image_open_whole(void *cwd, const char *path, struct ciuki_file *out);
static void reserved_ref(struct proc_object *o) { (void)o; }
static struct proc_object reserved_slot = { reserved_ref, reserved_ref, false };
static void file_retain(struct proc_object *object)
{
    __atomic_add_fetch(&((struct file_description *)object)->references,1,__ATOMIC_RELAXED);
}
static void file_release(struct proc_object *object)
{
    file_description_close((struct file_description *)object);
}

int file_open_flags(uint32_t flags, uint32_t mode)
{
    const uint32_t known = O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC;
    if ((flags & ~known) || (flags & O_ACCMODE) == O_ACCMODE ||
        ((flags & O_EXCL) && !(flags & O_CREAT)) ||
        ((flags & O_TRUNC) && (flags & O_ACCMODE) == O_RDONLY) ||
        ((flags & O_DIRECTORY) && (flags & (O_CREAT | O_TRUNC | O_ACCMODE))) ||
        ((flags & O_CREAT) && (mode & ~0777u))) return -EINVAL;
    return 0;
}
int file_fd_slot(const struct process *p, unsigned start)
{
    if (p && p->fds)
        for (unsigned i = start; i < CIUKI_OPEN_MAX; i++) if (!p->fds[i].object) return (int)i;
    return -EMFILE;
}

int file_fd_reserve(struct process *p)
{
    int fd = file_fd_slot(p, 0);
    if (fd >= 0) {
        p->fds[fd] = (struct proc_fd){ &reserved_slot, FD_CLOEXEC };
        if (net_fd_changed) net_fd_changed(p, (unsigned)fd);
    }
    return fd;
}
int file_fd_publish(struct process *p, int fd, struct proc_object *object, uint32_t flags)
{
    if (p->state == PROC_STOPPING || p->fds[fd].object != &reserved_slot) return -EBADF;
    p->fds[fd] = (struct proc_fd){ object, flags };
    if (net_fd_changed) net_fd_changed(p, (unsigned)fd);
    return fd;
}
void file_fd_cancel(struct process *p, int fd)
{
    if (p->fds[fd].object == &reserved_slot) {
        p->fds[fd] = (struct proc_fd){0};
        if (net_fd_changed) net_fd_changed(p, (unsigned)fd);
    }
}
struct proc_object *file_fd_object(const struct process *p, int32_t fd)
{
    if (!p || !p->fds || fd < 0 || fd >= CIUKI_OPEN_MAX || p->fds[fd].object == &reserved_slot) return 0;
    return p->fds[fd].object;
}
struct file_description *file_fd(const struct process *p, int32_t fd)
{
    struct proc_object *o = file_fd_object(p, fd);
    return o && o->release == file_release ? (struct file_description *)o : 0;
}
struct file_description *file_description_new(uint32_t flags)
{
    if (file_descriptions + desktop_objects.descriptions + NET_OBJECTS >= CIUKI_OPEN_DESCRIPTION_MAX) return 0;
    struct file_description *d = kzalloc(sizeof(*d));
    if (!d) return 0;
    d->object = (struct proc_object){ file_retain, file_release, true };
    d->references = 1; d->stream = 1; d->flags = flags & (O_ACCMODE | O_APPEND | O_NONBLOCK);
    __atomic_add_fetch(&file_descriptions,1,__ATOMIC_RELAXED);
    return d;
}
static bool win32_protected_file(const struct file_description *d)
{
    if (!d->win32) return false;
    for (const struct px_node *n=d->node;n && n->parent && n!=n->parent;n=n->parent)
        if (n->drive==2 && n->parent->mountpoint && path_equal(n->entry.name,"System")) return true;
    return false;
}
int file_description_close(struct file_description *d)
{
    if (__atomic_sub_fetch(&d->references,1,__ATOMIC_ACQ_REL)) return 0;
    int err = 0;
    if (audio_device_close) audio_device_close(d);
    if (com_device_close) com_device_close(d);
    if (lpt_device_close) lpt_device_close(d);
    if (d->node) {
        struct px_namespace *s = d->node->space;
        vfs_lock(s->vfs,0);
        /* f1-93b: the final close's hold was untagged (op=metadata site=vfs,
         * desktop#29 1214 ms on the T23 b453bc92 trial). */
        vfs_tag_locked(s->vfs,"close"); vfs_site_locked(s->vfs,"native");
        if (d->node->volume || d->node->ro) vfs_reserve_file_locked(s->vfs,vfs_drive_mask(d->node->drive),d->identity);
        struct file_description **at = &s->descriptions;
        while (*at && *at != d) at = &(*at)->next;
        if (*at) *at = d->next;
        vfs_description_unlock_locked(s->vfs,&d->lock_description);
        if (d->delete_on_close && !win32_protected_file(d)) d->node->delete_pending=true;
        if (d->node->generation==s->vfs->generation[d->node->drive] && d->node->volume==s->vfs->volumes[d->node->drive])
            err=px_delete_finish_locked(d->node);
        vfs_identity_put(s->vfs,d->identity);
        { int drop=px_drop_locked(d->node); if (!err) err=drop; }
        vfs_unlock(s->vfs);
    }
    __atomic_sub_fetch(&file_descriptions,1,__ATOMIC_RELAXED); kfree(d);
    return err;
}
int file_mapping_identity(struct file_description *d, uint64_t index, struct vfs_page_identity **out)
{
    if (!d || !d->node || d->node->kind!=PX_FILE || !d->identity) return -EINVAL;
    return px_error(vfs_page_get(d->node->space->vfs,d->identity,index,out));
}
int file_close(struct process *p, int32_t fd)
{
    struct proc_object *o = file_fd_object(p, fd);
    if (!o) return -EBADF;
    struct file_description *d = file_fd(p, fd);
    p->fds[fd] = (struct proc_fd){ 0 }; /* remove before a possibly blocking final release */
    if (net_fd_changed) net_fd_changed(p, (unsigned)fd);
    if (d) return file_description_close(d) ? -EIO : 0;
    o->release(o); return 0;
}
int file_dup(struct process *p, int32_t fd, int32_t target, bool exact, uint32_t flags)
{
    struct proc_object *o = file_fd_object(p, fd);
    if (!o) return -EBADF;
    if (target < 0 || target >= CIUKI_OPEN_MAX) return exact ? -EBADF : -EINVAL;
    if (exact && target == fd) return fd;
    int slot = exact ? target : file_fd_slot(p, (unsigned)target);
    if (slot < 0) return slot;
    o->retain(o);
    /* An in-flight open has reserved this number, but has not published an
     * object. dup2 is D-class and waits without holding a namespace lock;
     * preserve the validated source across close/reuse while waiting. */
    while (p->fds[slot].object == &reserved_slot) {
        if (p->state == PROC_STOPPING) { o->release(o); return -EBADF; }
        task_sleep_ms(1);
    }
    struct proc_object *previous = p->fds[slot].object;
    p->fds[slot] = (struct proc_fd){ o, flags };
    if (net_fd_changed) net_fd_changed(p, (unsigned)slot);
    /* Atomic replacement is published before release can yield. dup2's
     * closed error set intentionally discards a final-close I/O error. */
    if (previous) previous->release(previous);
    return slot;
}
int file_fcntl(struct process *p, int32_t fd, uint32_t cmd, uint32_t arg)
{
    /* fx-11: rejecting the access mode here made every socket's
     * F_GETFL|O_NONBLOCK fail EINVAL (NetSurf: no connect on the T23). */
    if (cmd == F_SETFL) { int bits = file_setfl_bits(arg); if (bits < 0) return bits; arg = (uint32_t)bits; }
    if (cmd == F_SETFD && (arg & ~FD_CLOEXEC)) return -EINVAL;
    struct proc_object *o = file_fd_object(p, fd);
    if (!o) return -EBADF;
    if (cmd == F_GETFD) return (int)p->fds[fd].flags;
    if (cmd == F_SETFD) { p->fds[fd].flags = arg; return 0; }
    if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC)
        return file_dup(p, fd, (int32_t)arg, false, cmd == F_DUPFD_CLOEXEC ? FD_CLOEXEC : 0);
    struct file_description *d = file_fd(p, fd);
    struct desktop_description *desktop = d ? 0 : desktop_fd(p, fd);
    uint32_t *socket_status = net_socket_flags ? net_socket_flags(o) : 0;
    if (socket_status && cmd == F_SETFL && (arg & O_APPEND)) return -EINVAL;
    uint32_t *status = d ? &d->flags : desktop ? &desktop->flags : socket_status;
    if (!status) return -EINVAL;
    if (cmd == F_GETFL) return (int)*status;
    if (cmd == F_SETFL && d && (d->node->kind == PX_AUDIO || d->node->kind == PX_MIXER || d->node->kind == PX_NET || d->node->kind == PX_MEDIA || d->node->kind == PX_COM || d->node->kind == PX_LPT || d->node->kind == PX_DISK) && (arg & O_APPEND)) return -EINVAL;
    if (cmd == F_SETFL) { *status = (*status & O_ACCMODE) | arg; return 0; }
    return -EINVAL;
}

int file_open(struct process *p, const char *path, uint32_t flags, uint32_t mode)
{
    int err = file_open_flags(flags, mode);
    if (!err) err = px_validate(path);
    if (err) return err;
    struct px_node *cwd = p->cwd;
    if (!cwd) return -ENOENT;
    int slot = file_fd_slot(p, 0);
    if (slot < 0) return slot;
    if (file_descriptions + desktop_objects.descriptions + NET_OBJECTS == CIUKI_OPEN_DESCRIPTION_MAX) return -ENFILE;
    struct file_description *d = file_description_new(flags);
    if (!d) return -ENOMEM;
    p->fds[slot] = (struct proc_fd){ &reserved_slot, FD_CLOEXEC };
    px_retain(cwd);
    vfs_lock(cwd->space->vfs,0);
    struct proc_thread *t = proc_thread_for(g_current);
    err = t && (t->process->state == PROC_STOPPING || proc_signal_caught(t)) ? -EINTR : px_open_locked(cwd->space, cwd, path, flags, d);
    px_release(cwd);
    if (!err && d->node->kind==PX_FILE && d->node->volume)
        err=px_error(vfs_identity_get(cwd->space->vfs,d->node->volume,&d->node->entry,&d->identity));
    if (!err) p->fds[slot] = (struct proc_fd){ &d->object, flags & O_CLOEXEC ? FD_CLOEXEC : 0 };
    else p->fds[slot] = (struct proc_fd){ 0 };
    vfs_unlock(cwd->space->vfs);
    if (err) file_description_close(d);
    return err ? err : slot;
}

int file_seek_locked(struct file_description *d, int64_t off, uint32_t whence, int64_t *out)
{
    int live=px_ro_check_locked(d->node); if (live) return live;
    if (!vfs_identity_live(d->node->space->vfs,d->identity)) return -EIO;
    if (whence > SEEK_END) return -EINVAL;
    if (d->node->kind == PX_NULL || d->node->kind == PX_CONSOLE || d->node->kind == PX_AUDIO || d->node->kind == PX_MIXER || d->node->kind == PX_NET || d->node->kind == PX_MEDIA || d->node->kind == PX_COM || d->node->kind == PX_LPT || d->node->kind == PX_DISK) return -ESPIPE;
    if (d->node->kind != PX_FILE) {
        if (off || whence != SEEK_SET) return -EINVAL;
        d->position = 0; d->cursor = 0; *out = 0; return 0;
    }
    uint64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? d->position : d->node->ro ? d->node->ro_size : d->node->entry.size;
    uint64_t magnitude = off < 0 ? (uint64_t)(-(off + 1)) + 1 : (uint64_t)off;
    if (off < 0 && magnitude > base) return -EINVAL;
    if (off >= 0 && magnitude > (uint64_t)INT64_MAX - base) return -EOVERFLOW;
    d->position = off < 0 ? base - magnitude : base + magnitude;
    *out = (int64_t)d->position;
    return 0;
}
extern int w32_file_sync(struct px_node *) __attribute__((weak));
extern bool w32_file_needs_sync(struct px_node *) __attribute__((weak));
extern void w32_file_changed(struct px_node *,uint64_t,const void *,uint32_t) __attribute__((weak));
struct read_range_check { struct file_description *description; int error; };
static int native_write_turn(struct file_description *,uint64_t *,bool,const void *,size_t,size_t *,bool);
static int native_write_transaction(struct file_description *d,uint64_t *position,bool positioned,
                                     const void *data,size_t bytes,size_t *done,bool section) {
    struct px_node *n=d->node; struct vfs *v=n->space->vfs;
    if (!px_fat_transaction_locked || !vfs_file_wait_locked || !vfs_turn_take_locked)
        return VFS_IO(v,fat_write_cursor(n->volume,&n->entry,&d->fat_cursor,*position,data,bytes,done));
    /* f1-89: the writer turn spans every chunk of this write. Busy flags
     * are waited for WITHOUT the turn (their owner may be a data
     * transaction that suspended its turn around device I/O). */
    for (;;) {
        int e=vfs_file_wait_locked(v,&n->writing);
        if (!e) e=vfs_file_wait_locked(v,&d->lock_description.io_active);
        if (!e && d->identity) e=vfs_file_wait_locked(v,&d->identity->writer);
        if (e) return e;
        uint32_t turn; e=vfs_turn_take_locked(v,vfs_drive_mask(n->drive),&turn);
        if (e) return e;
        if (n->writing || d->lock_description.io_active || (d->identity && d->identity->writer)) {
            vfs_turn_drop_locked(v,turn); continue;
        }
        e=native_write_turn(d,position,positioned,data,bytes,done,section);
        vfs_turn_drop_locked(v,turn);
        return e;
    }
}
static int native_write_turn(struct file_description *d,uint64_t *position,bool positioned,
                             const void *data,size_t bytes,size_t *done,bool section) {
    struct px_node *n=d->node; struct vfs *v=n->space->vfs; int e=0;
    if (!vfs_identity_live(v,d->identity) || n->generation!=v->generation[n->drive]) return -FS_EIO;
    if (!positioned) *position=(d->flags&O_APPEND) ? n->entry.size : d->position;
    if (*position>UINT32_MAX || bytes>UINT32_MAX-*position) return -FS_EFBIG;
    if (!section && (e=px_description_range_locked(d,*position,bytes,true))) return e;
    n->writing=true; d->lock_description.io_active=true;
    /* A write slice is one in-flight claim set (256 clusters); a gap
     * before the position is extended in 128 KiB steps first. */
    size_t limit=(size_t)n->volume->spc*512*FAT_TRANSACTION_CLUSTERS, extend=limit<131072 ? limit : 131072;
    while (*position>=(uint64_t)n->entry.size+limit && !e) {
        uint64_t end=(uint64_t)n->entry.size+extend; if (end>*position) end=*position;
        e=px_fat_transaction_locked(n,n->parent,0,(struct fat_transaction_args){.op=FAT_TX_EXTEND,.entry=&n->entry,.capacity=(uint32_t)end});
    }
    while (*done<bytes && !e) {
        size_t step=limit, written=0;
        if (*position+*done>n->entry.size) step-=*position+*done-n->entry.size;
        if (step>bytes-*done) step=bytes-*done;
        e=px_fat_transaction_locked(n,n->parent,0,(struct fat_transaction_args){.op=FAT_TX_WRITE,
            .entry=&n->entry,.cursor=&d->fat_cursor,.position=*position+*done,.data=(const uint8_t *)data+*done,.size=step,.done=&written});
        *done+=written;
    }
    d->lock_description.io_active=false; n->writing=false; fs_event_wake(&v->volume_changed);
    return e;
}
static int read_range(void *arg,uint64_t position,size_t bytes)
{
    struct read_range_check *check=arg;
    return check->error=px_description_range_locked(check->description,position,bytes,false);
}
static int description_io_locked(struct file_description *d, void *buffer, size_t bytes, bool write,
                    bool positioned, uint64_t offset, size_t *done, bool section)
{
    vfs_tag_locked(d->node->space->vfs,write ? "write" : "read");
    vfs_site_locked(d->node->space->vfs,"native");
    *done = 0;
    if (bytes > CIUKI_IO_MAX) return -EINVAL;
    int live=!write && d->node->volume ?
        (d->node->drive<26 && d->node->volume->mounted && !d->node->volume->dev->quarantined &&
         d->node->space->vfs->volumes[d->node->drive]==d->node->volume &&
         d->node->space->vfs->generation[d->node->drive]==d->node->generation ? 0 : -EIO) : px_ro_check_locked(d->node);
    if (live) return live;
    if (!vfs_identity_live(d->node->space->vfs,d->identity)) return -EIO;
    if (write && win32_protected_file(d)) return -EACCES;
    if (write && (d->node->ro || px_write_denied(d->node))) return -EROFS;
    uint32_t mode = d->flags & O_ACCMODE;
    if (write ? mode == O_RDONLY : mode == O_WRONLY) return -EBADF;
    struct px_node *n = d->node;
    if (n->kind != PX_FILE && n->kind != PX_NULL && n->kind != PX_CONSOLE) return -EISDIR;
    if (positioned && n->kind != PX_FILE) return -ESPIPE;
    if (positioned && offset > INT64_MAX) return -EINVAL;
    if (!bytes) return 0;
    if (n->kind == PX_NULL) { *done = write ? bytes : 0; return 0; }
    if (n->kind == PX_CONSOLE) {
        int err = device_write(buffer, (uint32_t)bytes);
        if (err >= 0) { *done = (size_t)err; supervisor_output(g_current, d->stream, buffer, (uint32_t)err); return 0; }
        return err;
    }
    uint64_t position = positioned ? offset : write && (d->flags & O_APPEND) ? n->entry.size : d->position;
    if (write && (position > UINT32_MAX || bytes > UINT32_MAX - position)) return -EFBIG;
    /* DOS record locks are mandatory for native clients as well. Namespace
     * and this volume's reservation already protect the common node list. */
    bool private_read=!write && !n->ro && !n->space->vfs->active;
    int locked=section || private_read ? 0 : px_description_range_locked(d,position,bytes,write); if (locked) return locked;
    if (w32_file_sync) {
        struct vfs *v=n->space->vfs;
        bool reserve=!v->active && w32_file_needs_sync && w32_file_needs_sync(n);
        if (reserve) vfs_reserve_file_locked(v,vfs_drive_mask(n->drive),d->identity);
        int sync=w32_file_sync(n);
        if (reserve) { vfs_unlock(v); vfs_lock(v,0); }
        if (sync) return sync;
    }
    struct fat_entry before = n->entry;
    struct read_range_check read_check={d,0};
    int err = n->ro ? VFS_IO(d->node->space->vfs,fs_ro_read(n->ro,n->ro_id,position,buffer,bytes,done)) :
        write ? native_write_transaction(d,&position,positioned,buffer,bytes,done,section) :
                vfs_read_identity_locked(n->space->vfs,d->identity,n->volume,&n->entry,
                    &d->fat_cursor,&d->lock_description.io_active,&position,&d->position,positioned,
                    section ? 0 : read_range,&read_check,buffer,bytes,done);
    /* A mandatory-lock denial is already in the caller's ABI. Preserve it
     * just as the reserved precheck does, before FAT's attribute mapping. */
    if (read_check.error) return read_check.error;
    if(write && *done && w32_file_changed)w32_file_changed(n,position,buffer,(uint32_t)*done);
    if (!write) proc_launch_volume_read((uint32_t)bytes, *done);
    /* fat_write reports the committed count even if the later FSInfo hint
     * fails. An in-place overwrite can leave the whole entry unchanged;
     * its confirmed bytes still require a ctime notification. */
    if (!positioned && (!err || *done)) d->position = position + *done;
    if (write && (*done || memcmp(&before, &n->entry, sizeof(before)))) px_changed_locked(n);
    if (n->ro && err) px_ro_check_locked(n);
    err = px_error(err);
    /* FAT RO attributes were checked at open. A later legacy attribute
     * change remains effective without exposing EACCES outside the ABI set. */
    return err == -EACCES ? -EROFS : err;
}
int file_io_locked(struct file_description *d,void *buffer,size_t bytes,bool write,
                    bool positioned,uint64_t offset,size_t *done)
{
    return description_io_locked(d,buffer,bytes,write,positioned,offset,done,false);
}
/* Private section endpoint. Win32 byte locks do not apply to mapped views;
 * generation, access, protected ancestry and durability still apply. */
int file_section_io_locked(struct file_description *d,void *buffer,size_t bytes,bool write,
                           bool positioned,uint64_t offset,size_t *done)
{
    return description_io_locked(d,buffer,bytes,write,positioned,offset,done,true);
}
int file_truncate_locked(struct file_description *d, uint64_t length)
{
    vfs_tag_locked(d->node->space->vfs,"truncate");
    vfs_site_locked(d->node->space->vfs,"native");
    if (win32_protected_file(d)) return -EACCES;
    int live=px_ro_check_locked(d->node); if (live) return live;
    if (!vfs_identity_live(d->node->space->vfs,d->identity)) return -EIO;
    if (d->node->ro || px_write_denied(d->node)) return -EROFS;
    if ((d->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (d->node->kind != PX_FILE) return d->node->kind == PX_NULL || d->node->kind == PX_CONSOLE ? -EINVAL : -EISDIR;
    if (length > INT64_MAX) return -EINVAL;
    if (length > UINT32_MAX) return -EFBIG;
    struct fat_entry before = d->node->entry;
    uint64_t start=length<before.size ? length : before.size;
    int locked=px_description_range_locked(d,start,length<before.size ? before.size-length : length-before.size,true);
    if (locked) return locked;
    int err = px_fat_transaction_locked ? px_fat_transaction_locked(d->node,d->node->parent,0,
        (struct fat_transaction_args){.op=FAT_TX_TRUNCATE,.entry=&d->node->entry,.position=length}) :
        VFS_IO(d->node->space->vfs,fat_truncate(d->node->volume, &d->node->entry, length));
    if (memcmp(&before, &d->node->entry, sizeof(before))) px_changed_locked(d->node);
    return err == -FS_EACCES ? -EROFS : px_error(err);
}
int file_sync_locked(struct file_description *d)
{
    vfs_tag_locked(d->node->space->vfs,"commit");
    vfs_site_locked(d->node->space->vfs,"native");
    int live=px_ro_check_locked(d->node); if (live) return live;
    if (!vfs_identity_live(d->node->space->vfs,d->identity)) return -EIO;
    if (d->node->ro) return px_ro_check_locked(d->node);
    if (!d->node->volume) return -EINVAL;
    return px_error(px_fat_transaction_locked ? px_fat_transaction_locked(d->node,d->node->parent,0,
        (struct fat_transaction_args){.op=FAT_TX_COMMIT,.entry=&d->node->entry}) : VFS_IO(d->node->space->vfs,fat_commit(d->node->volume)));
}
int files_stdio(struct process *p)
{
    int fd = file_open(p, "/dev/null", O_RDONLY, 0);
    if (fd < 0) return fd;
    if (fd != 0) { file_dup(p, fd, 0, true, 0); file_close(p, fd); }
    fd = file_open(p, "/dev/console", O_WRONLY, 0);
    if (fd < 0) return fd;
    if (fd != 1) { file_dup(p, fd, 1, true, 0); file_close(p, fd); }
    fd = file_open(p, "/dev/console", O_WRONLY, 0);
    if (fd < 0) return fd;
    file_fd(p, fd)->stream = 2;
    if (fd != 2) { file_dup(p, fd, 2, true, 0); file_close(p, fd); }
    return 0;
}
int files_bootstrap(struct vfs *v)
{
    struct px_namespace *s;
    int err = files_attach(v, &s);
    if (err) return err;
    struct process *p = proc_supervisor();
    if (!p->cwd) { p->cwd = s->root; px_retain(p->cwd); p->cwd_retain = px_retain; p->cwd_release = px_release; }
    if (!p->fds) { p->fds = kzalloc(CIUKI_OPEN_MAX * sizeof(*p->fds)); if (!p->fds) return -ENOMEM; }
    static const struct supervisor_io_ops ops = { files_stdio, px_chdir };
    supervisor_set_io_ops(&ops);
    static const struct ciuki_file_ops executable_ops = { image_open, image_open_whole };
    proc_set_file_ops(&executable_ops);
    return files_stdio(p);
}

/* Executable bytes are immutable through close, which cannot block. Snapshot
 * under the shared namespace lock, using page backing rather than a large
 * kernel heap allocation or a read-only open masquerading as immutability.
 *
 * fx-8: the snapshot is sparse. For an ELF32 executable it holds only the
 * bytes the loader reads (elf.c: the ELF header, the program and section
 * header tables, every PT_LOAD's file bytes); any other file (PE images,
 * malformed headers) is snapshotted whole, as before, so every validation
 * result is unchanged. The built-in programs carry their DWARF sections
 * (NetSurf: 13.5 MB file, 2.6 MB loadable); reading those made the T23
 * spend 3.9 s per NetSurf start (trial 423b803a: 247 ATA commands, about
 * 3 MB/s), now ~0.8 s for the 2.6 MB the child maps. A page the snapshot
 * does not hold reads as -ENOEXEC. */
struct file_image {
    uint32_t bytes, pages;  /* pages: snapshot pages held */
    uint32_t lists[CIUKI_ELF_BYTES_MAX / PAGE_SIZE / (PAGE_SIZE / sizeof(uint32_t))];
};
#define IMAGE_PER_LIST (PAGE_SIZE / sizeof(uint32_t))
static void *image_page(struct file_image *image, uint32_t index)
{
    if (index >= ARRAY_SIZE(image->lists) * IMAGE_PER_LIST) return 0;
    uint32_t list = image->lists[index / IMAGE_PER_LIST];
    uint32_t page = list ? ((uint32_t *)P2V(list))[index % IMAGE_PER_LIST] : 0;
    return page ? P2V(page) : 0;
}
static void image_reset(struct file_image *image)
{
    for (unsigned i = 0; i < ARRAY_SIZE(image->lists); i++) {
        if (!image->lists[i]) continue;
        uint32_t *list = P2V(image->lists[i]);
        for (unsigned k = 0; k < IMAGE_PER_LIST; k++) if (list[k]) pmm_free(list[k]);
        pmm_free(image->lists[i]); image->lists[i] = 0;
    }
    image->pages = 0;
}
static void image_close(void *cookie)
{
    struct file_image *image = cookie;
    image_reset(image);
    kfree(image);
}
static int image_read(void *cookie, uint32_t offset, void *buffer, uint32_t bytes)
{
    struct file_image *image = cookie;
    if (offset > image->bytes || bytes > image->bytes - offset) return -EIO;
    for (uint32_t done = 0; done < bytes;) {
        uint32_t within = offset % PAGE_SIZE, take = PAGE_SIZE - within;
        if (take > bytes - done) take = bytes - done;
        const uint8_t *page = image_page(image, offset / PAGE_SIZE);
        if (!page) return -ENOEXEC;          /* outside the loader's byte set */
        memcpy((uint8_t *)buffer + done, page + within, take);
        offset += take; done += take;
    }
    return 0;
}
/* Store one page of a read run at its file page index. */
static int image_store(struct file_image *image, uint32_t index, const uint8_t *data, uint32_t take)
{
    uint32_t *slot = &image->lists[index / IMAGE_PER_LIST];
    if (!*slot) {
        *slot = pmm_alloc();
        if (!*slot) return -ENOMEM;
        memset(P2V(*slot), 0, PAGE_SIZE);
    }
    uint32_t *entry = &((uint32_t *)P2V(*slot))[index % IMAGE_PER_LIST];
    if (!*entry) {
        *entry = pmm_alloc();
        if (!*entry) return -ENOMEM;
        image->pages++;
    }
    memcpy(P2V(*entry), data, take);
    return 0;
}
/* One positioned read of exactly `bytes` into the run buffer. */
struct image_source {
    int (*read)(struct image_source *, uint32_t offset, uint32_t bytes);
    uint8_t *buffer;
    bool whole;
    struct px_namespace *space;
    struct px_node *node;
    struct vfs_file_identity *id;
    struct fat_cursor cursor;
    bool busy;
};
/* Read the snapshot's missing pages in [first, end): runs of contiguous
 * missing pages, at most BLKDEV_READ_BYTES each (one admission per run). */
static int image_fill(struct file_image *image, struct image_source *src, uint32_t first, uint32_t end)
{
    const uint32_t run_pages = BLKDEV_READ_BYTES / PAGE_SIZE;
    for (uint32_t page = first; page < end;) {
        if (image_page(image, page)) { page++; continue; }
        uint32_t last = page;
        while (last < end && last - page < run_pages && !image_page(image, last)) last++;
        uint32_t offset = page * PAGE_SIZE, bytes = (last - page) * PAGE_SIZE;
        if (bytes > image->bytes - offset) bytes = image->bytes - offset;
        int err = src->read(src, offset, bytes);
        for (uint32_t at = 0; !err && at < bytes; at += PAGE_SIZE)
            err = image_store(image, page + at / PAGE_SIZE, src->buffer + at, bytes - at < PAGE_SIZE ? bytes - at : PAGE_SIZE);
        if (err) return err;
        page = last;
    }
    return 0;
}
static int image_need(struct file_image *image, struct image_source *src, uint32_t offset, uint32_t bytes)
{
    if (offset >= image->bytes || !bytes) return 0;
    if (bytes > image->bytes - offset) bytes = image->bytes - offset;
    return image_fill(image, src, offset / PAGE_SIZE, (offset + bytes - 1) / PAGE_SIZE + 1);
}
/* The bytes elf_validate() and elf_load() read, or the whole file when it is
 * not an ELF32 header those functions could accept. Ranges are read in file
 * order so the FAT cursor walks the cluster chain forward once. */
static int image_snapshot(struct file_image *image, struct image_source *src)
{
    struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version, entry, phoff, shoff, flags;
             uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } h;
    struct { uint32_t type, offset, va, pa, filesz, memsz, flags, align; } ph;
    _Static_assert(sizeof(h) == 52 && sizeof(ph) == 32, "ELF32 header layout");
    uint32_t bytes = image->bytes;
    int err = image_need(image, src, 0, bytes < BLKDEV_READ_BYTES ? bytes : BLKDEV_READ_BYTES);
    if (err) return err;
    bool elf = bytes >= sizeof(h) && !image_read(image, 0, &h, sizeof(h)) &&
        !memcmp(h.ident, "\177ELF", 4) && h.ident[4] == 1 && h.ident[5] == 1 &&
        h.phentsize == sizeof(ph) && h.phnum && h.phnum <= CIUKI_ELF_PHDR_MAX &&
        h.phoff <= bytes && (uint32_t)h.phnum * sizeof(ph) <= bytes - h.phoff &&
        (!h.shoff || (h.shentsize == 40 && h.shoff <= bytes && (uint32_t)h.shnum * 40u <= bytes - h.shoff));
    if (!elf || src->whole) return image_need(image, src, 0, bytes);
    if ((err = image_need(image, src, h.phoff, (uint32_t)h.phnum * sizeof(ph)))) return err;
    struct image_span { uint32_t offset, bytes; } ranges[CIUKI_ELF_PHDR_MAX + 1];
    unsigned count = 0;
    if (h.shoff) ranges[count++] = (struct image_span){ h.shoff, (uint32_t)h.shnum * 40u };
    for (unsigned i = 0; i < h.phnum; i++) {
        if ((err = image_read(image, h.phoff + i * sizeof(ph), &ph, sizeof(ph)))) return err;
        /* Out-of-file ranges are left to elf_validate's own refusal. */
        if (ph.type == 1 && ph.filesz && ph.offset < bytes)
            ranges[count++] = (struct image_span){ ph.offset, ph.filesz };
    }
    for (unsigned i = 1; i < count; i++)
        for (unsigned j = i; j && ranges[j - 1].offset > ranges[j].offset; j--) {
            struct image_span t = ranges[j]; ranges[j] = ranges[j - 1]; ranges[j - 1] = t;
        }
    for (unsigned i = 0; !err && i < count; i++) err = image_need(image, src, ranges[i].offset, ranges[i].bytes);
    return err;
}
struct image_range { struct px_node *node; };
static int image_range_check(void *arg, uint64_t position, size_t bytes)
{
    struct image_range *r = arg;
    return px_range_locked(r->node, position, bytes) ? -EACCES : 0;
}
static int image_fat_read(struct image_source *src, uint32_t offset, uint32_t bytes)
{
    struct px_node *n = src->node;
    struct image_range range = { n };
    uint64_t position = offset; size_t done = 0;
    int err = px_error(vfs_read_identity_locked(src->space->vfs, src->id, n->volume, &n->entry, &src->cursor,
        &src->busy, &position, &position, true, image_range_check, &range, src->buffer, bytes, &done));
    proc_launch_volume_read(bytes, done);
    return !err && done != bytes ? -EIO : err;
}
static int image_ro_read(struct image_source *src, uint32_t offset, uint32_t bytes)
{
    size_t done = 0;
    int err = px_error(VFS_IO(src->space->vfs, fs_ro_read(src->node->ro, src->node->ro_id, offset, src->buffer, bytes, &done)));
    proc_launch_volume_read(bytes, done);
    return !err && done != bytes ? -EIO : err;
}
/* f1-93: FAT executable bytes come through the open-file reader path, one
 * 64 KiB run per admission, with no volume reservation across a device read
 * (the T23 1.0.0 alpha launch held drive C for 2294 ms reading a binary
 * under the reservation, tagged op=lookup). The snapshot stays consistent:
 * the entry and the f3-51c write generation are compared after the last
 * run, and the whole image is read again (at most twice) if the file
 * changed meanwhile. Mandatory record locks are checked for every run.
 * Namespace and the drive's reservation held on entry and return. */
static int image_fat(struct px_namespace *s, struct px_node *n, struct file_image *image, void *buffer, bool whole)
{
    struct vfs *v = s->vfs;
    struct vfs_file_identity *id = 0;
    int err = px_error(vfs_identity_get(v, n->volume, &n->entry, &id));
    if (err) return err;
    px_retain(n);
    uint32_t mask = v->active;
    vfs_unlock(v); vfs_lock(v, 0);
    vfs_tag_locked(v, "read"); vfs_site_locked(v, "native");
    for (unsigned attempt = 0;; attempt++) {
        if (!n->linked) { err = -ENOENT; break; }
        struct fat_entry entry = n->entry;
        if (entry.size > CIUKI_ELF_BYTES_MAX) { err = -ENOEXEC; break; }
        image_reset(image); image->bytes = entry.size;
        uint64_t generation = vfs_change_generation(v, n->volume, entry.parent, entry.index);
        struct image_source src = { .read = image_fat_read, .buffer = buffer, .whole = whole, .space = s, .node = n, .id = id };
        err = image_snapshot(image, &src);
        if (err) break;
        /* Changed while it was read: read it again from the new entry. */
        if (n->linked && n->entry.first == entry.first && n->entry.size == entry.size &&
            n->entry.chain_generation == entry.chain_generation && vfs_identity_live(v, id) &&
            vfs_change_generation(v, n->volume, entry.parent, entry.index) == generation) break;
        if (attempt == 2) { err = -EAGAIN; break; }
    }
    vfs_identity_put(v, id);
    vfs_reserve_locked(v, mask);
    int drop = px_drop_locked(n);
    return err ? err : drop;
}
static int image_snapshot_open(void *cwd_arg, const char *path, struct ciuki_file *out, bool whole)
{
    struct px_node *cwd = cwd_arg;
    if (!cwd) return -ENOENT;
    struct file_image *image = kzalloc(sizeof(*image));
    if (!image) return -ENOMEM;
    struct px_namespace *s = cwd->space;
    vfs_lock(s->vfs,0);
    struct px_path r;
    int err = px_resolve_locked(s, cwd, path, false, &r);
    vfs_tag_locked(s->vfs,"open"); vfs_site_locked(s->vfs,"native");
    if (!err && r.node->kind != PX_FILE) err = -EACCES;
    if (!err && (r.node->ro ? r.node->ro_size : r.node->entry.size) > CIUKI_ELF_BYTES_MAX) err = -ENOEXEC;
    if (!err) err = px_share_locked(r.node, VFS_READ, 0);
    if (!err) err = px_range_locked(r.node,0,r.node->ro ? r.node->ro_size : r.node->entry.size);
    if (!err) image->bytes = (uint32_t)(r.node->ro ? r.node->ro_size : r.node->entry.size);
    /* A FAT image can be read again with a different size: one full run. */
    uint32_t buffer_bytes=err ? 0 : !r.node->ro ? BLKDEV_READ_BYTES :
        image->bytes<BLKDEV_READ_BYTES ? image->bytes : BLKDEV_READ_BYTES;
    void *buffer=!err && buffer_bytes ? fs_contig_alloc(buffer_bytes) : 0;
    if (!err && buffer_bytes && !buffer) err=-ENOMEM;
    if (!err && !r.node->ro && r.node->volume) err = image_fat(s, r.node, image, buffer, whole);
    else if (!err) {
        struct image_source src = { .read = image_ro_read, .buffer = buffer, .whole = whole, .space = s, .node = r.node };
        err = image_snapshot(image, &src);
    }
    fs_contig_free(buffer,buffer_bytes);
    vfs_unlock(s->vfs);
    if (err) { image_close(image); return err; }
    *out = (struct ciuki_file){ image, image->bytes, image_read, image_close };
    return 0;
}
static int image_open(void *cwd, const char *path, struct ciuki_file *out)
{ return image_snapshot_open(cwd, path, out, false); }
static int image_open_whole(void *cwd, const char *path, struct ciuki_file *out)
{ return image_snapshot_open(cwd, path, out, true); }
