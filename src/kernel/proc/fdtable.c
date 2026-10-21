/* One process fd table for files, streams and the f2-05 object family.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/desktop.h>
#include <ciuki/supervisor.h>
#include <ciuki/signal.h>
#include <ciuki/audio.h>
#pragma weak audio_device_close
uint32_t net_socket_count(void) __attribute__((weak));
uint32_t *net_socket_flags(struct proc_object *) __attribute__((weak));
void net_fd_changed(struct process *, unsigned) __attribute__((weak));
#define NET_OBJECTS (net_socket_count ? net_socket_count() : 0)

static uint32_t file_descriptions;
uint32_t file_description_count(void) { return __atomic_load_n(&file_descriptions,__ATOMIC_RELAXED); }
static int image_open(void *cwd, const char *path, struct ciuki_file *out);
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
    if (d->node) {
        struct px_namespace *s = d->node->space;
        vfs_lock(s->vfs,0);
        if (d->node->volume || d->node->ro) vfs_reserve_locked(s->vfs,vfs_drive_mask(d->node->drive));
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
    if ((cmd == F_SETFD && (arg & ~FD_CLOEXEC)) ||
        (cmd == F_SETFL && (arg & ~(O_APPEND | O_NONBLOCK)))) return -EINVAL;
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
    if (cmd == F_SETFL && d && (d->node->kind == PX_AUDIO || d->node->kind == PX_MIXER || d->node->kind == PX_NET || d->node->kind == PX_MEDIA) && (arg & O_APPEND)) return -EINVAL;
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
    if (d->node->kind == PX_NULL || d->node->kind == PX_CONSOLE || d->node->kind == PX_AUDIO || d->node->kind == PX_MIXER || d->node->kind == PX_NET || d->node->kind == PX_MEDIA) return -ESPIPE;
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
    if (write && d->node->ro) return -EROFS;
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
        if (reserve) vfs_reserve_locked(v,vfs_drive_mask(n->drive));
        int sync=w32_file_sync(n);
        if (reserve) { vfs_unlock(v); vfs_lock(v,0); }
        if (sync) return sync;
    }
    struct fat_entry before = n->entry;
    struct read_range_check read_check={d,0};
    int err = n->ro ? VFS_IO(d->node->space->vfs,fs_ro_read(n->ro,n->ro_id,position,buffer,bytes,done)) :
        write ? VFS_IO(d->node->space->vfs,fat_write_cursor(n->volume, &n->entry, &d->fat_cursor, position, buffer, bytes, done)) :
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
    if (d->node->ro) return -EROFS;
    if ((d->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (d->node->kind != PX_FILE) return d->node->kind == PX_NULL || d->node->kind == PX_CONSOLE ? -EINVAL : -EISDIR;
    if (length > INT64_MAX) return -EINVAL;
    if (length > UINT32_MAX) return -EFBIG;
    struct fat_entry before = d->node->entry;
    uint64_t start=length<before.size ? length : before.size;
    int locked=px_description_range_locked(d,start,length<before.size ? before.size-length : length-before.size,true);
    if (locked) return locked;
    int err = VFS_IO(d->node->space->vfs,fat_truncate(d->node->volume, &d->node->entry, length));
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
    return px_error(VFS_IO(d->node->space->vfs,fat_commit(d->node->volume)));
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
    static const struct ciuki_file_ops executable_ops = { image_open };
    proc_set_file_ops(&executable_ops);
    return files_stdio(p);
}

/* Executable bytes are immutable through close, which cannot block. Snapshot
 * under the shared namespace lock, using page backing rather than a large
 * kernel heap allocation or a read-only open masquerading as immutability. */
struct file_image {
    uint32_t bytes, pages;
    uint32_t lists[CIUKI_ELF_BYTES_MAX / PAGE_SIZE / (PAGE_SIZE / sizeof(uint32_t))];
};
static void *image_page(struct file_image *image, uint32_t index)
{
    uint32_t list = image->lists[index / (PAGE_SIZE / sizeof(uint32_t))];
    return list ? P2V(((uint32_t *)P2V(list))[index % (PAGE_SIZE / sizeof(uint32_t))]) : 0;
}
static void image_close(void *cookie)
{
    struct file_image *image = cookie;
    for (unsigned i = 0; i < image->pages; i++) pmm_free(V2P(image_page(image, i)));
    for (unsigned i = 0; i < ARRAY_SIZE(image->lists); i++) if (image->lists[i]) pmm_free(image->lists[i]);
    kfree(image);
}
static int image_read(void *cookie, uint32_t offset, void *buffer, uint32_t bytes)
{
    struct file_image *image = cookie;
    if (offset > image->bytes || bytes > image->bytes - offset) return -EIO;
    for (uint32_t done = 0; done < bytes;) {
        uint32_t within = offset % PAGE_SIZE, take = PAGE_SIZE - within;
        if (take > bytes - done) take = bytes - done;
        memcpy((uint8_t *)buffer + done, (uint8_t *)image_page(image, offset / PAGE_SIZE) + within, take);
        offset += take; done += take;
    }
    return 0;
}
static int image_open(void *cwd_arg, const char *path, struct ciuki_file *out)
{
    struct px_node *cwd = cwd_arg;
    if (!cwd) return -ENOENT;
    struct file_image *image = kzalloc(sizeof(*image));
    if (!image) return -ENOMEM;
    struct px_namespace *s = cwd->space;
    vfs_lock(s->vfs,0);
    struct px_path r;
    int err = px_resolve_locked(s, cwd, path, false, &r);
    if (!err && r.node->kind != PX_FILE) err = -EACCES;
    if (!err && (r.node->ro ? r.node->ro_size : r.node->entry.size) > CIUKI_ELF_BYTES_MAX) err = -ENOEXEC;
    if (!err) err = px_share_locked(r.node, VFS_READ, 0);
    if (!err) err = px_range_locked(r.node,0,r.node->ro ? r.node->ro_size : r.node->entry.size);
    if (!err) image->bytes = (uint32_t)(r.node->ro ? r.node->ro_size : r.node->entry.size);
    uint32_t buffer_bytes=image->bytes<BLKDEV_READ_BYTES ? image->bytes : BLKDEV_READ_BYTES;
    void *buffer=!err && buffer_bytes ? fs_contig_alloc(buffer_bytes) : 0;
    if (!err && buffer_bytes && !buffer) err=-ENOMEM;
    struct fat_cursor cursor = {0}; /* f3-51c: sequential image read, linear in the chain */
    for (uint32_t offset = 0; !err && offset < image->bytes; offset += BLKDEV_READ_BYTES) {
        uint32_t bytes = image->bytes - offset;
        if (bytes > BLKDEV_READ_BYTES) bytes = BLKDEV_READ_BYTES;
        size_t done = 0;
        err = px_error(r.node->ro ? VFS_IO(s->vfs,fs_ro_read(r.node->ro,r.node->ro_id,offset,buffer,bytes,&done)) :
                       VFS_IO(s->vfs,fat_read_cursor(r.node->volume, &r.node->entry, &cursor, offset, buffer, bytes, &done)));
        proc_launch_volume_read(bytes, done);
        if (!err && done != bytes) err = -EIO;
        for (uint32_t off=0; !err && off<bytes; off+=PAGE_SIZE) {
            unsigned list_index = image->pages / (PAGE_SIZE / sizeof(uint32_t));
            if (!image->lists[list_index]) {
                image->lists[list_index] = pmm_alloc();
                if (!image->lists[list_index]) { err = -ENOMEM; break; }
                memset(P2V(image->lists[list_index]), 0, PAGE_SIZE);
            }
            uint32_t page = pmm_alloc();
            if (!page) { err = -ENOMEM; break; }
            ((uint32_t *)P2V(image->lists[list_index]))[image->pages % (PAGE_SIZE / sizeof(uint32_t))] = page;
            image->pages++;
            uint32_t take=bytes-off;
            if (take>PAGE_SIZE) take=PAGE_SIZE;
            memcpy(P2V(page),(uint8_t *)buffer+off,take);
        }
    }
    fs_contig_free(buffer,buffer_bytes);
    vfs_unlock(s->vfs);
    if (err) { image_close(image); return err; }
    *out = (struct ciuki_file){ image, image->bytes, image_read, image_close };
    return 0;
}
