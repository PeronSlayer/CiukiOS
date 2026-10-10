/* One process fd table for files, streams and the f2-05 object family.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/desktop.h>
#include <ciuki/supervisor.h>
#include <ciuki/signal.h>

static uint32_t file_descriptions;
uint32_t file_description_count(void) { return file_descriptions; }
static int image_open(void *cwd, const char *path, struct ciuki_file *out);
static void reserved_ref(struct proc_object *o) { (void)o; }
static struct proc_object reserved_slot = { reserved_ref, reserved_ref, false };
static void file_retain(struct proc_object *object)
{
    ((struct file_description *)object)->references++;
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
    if (file_descriptions + desktop_objects.descriptions >= CIUKI_OPEN_DESCRIPTION_MAX) return 0;
    struct file_description *d = kzalloc(sizeof(*d));
    if (!d) return 0;
    d->object = (struct proc_object){ file_retain, file_release, true };
    d->references = 1; d->stream = 1; d->flags = flags & (O_ACCMODE | O_APPEND | O_NONBLOCK);
    file_descriptions++;
    return d;
}
int file_description_close(struct file_description *d)
{
    if (--d->references) return 0;
    int err = 0;
    if (d->node) {
        struct px_namespace *s = d->node->space;
        fs_lock_take(&s->vfs->lock);
        struct file_description **at = &s->descriptions;
        while (*at && *at != d) at = &(*at)->next;
        if (*at) *at = d->next;
        err = px_drop_locked(d->node);
        fs_lock_drop(&s->vfs->lock);
    }
    file_descriptions--; kfree(d);
    return err;
}
int file_close(struct process *p, int32_t fd)
{
    struct proc_object *o = file_fd_object(p, fd);
    if (!o) return -EBADF;
    struct file_description *d = file_fd(p, fd);
    p->fds[fd] = (struct proc_fd){ 0 }; /* remove before a possibly blocking final release */
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
    uint32_t *status = d ? &d->flags : desktop ? &desktop->flags : 0;
    if (!status) return -EINVAL;
    if (cmd == F_GETFL) return (int)*status;
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
    if (file_descriptions + desktop_objects.descriptions == CIUKI_OPEN_DESCRIPTION_MAX) return -ENFILE;
    struct file_description *d = file_description_new(flags);
    if (!d) return -ENOMEM;
    p->fds[slot] = (struct proc_fd){ &reserved_slot, FD_CLOEXEC };
    px_retain(cwd);
    fs_lock_take(&cwd->space->vfs->lock);
    struct proc_thread *t = proc_thread_for(g_current);
    err = t && (t->process->state == PROC_STOPPING || proc_signal_caught(t)) ? -EINTR : px_open_locked(cwd->space, cwd, path, flags, d);
    px_release(cwd);
    if (!err) p->fds[slot] = (struct proc_fd){ &d->object, flags & O_CLOEXEC ? FD_CLOEXEC : 0 };
    else p->fds[slot] = (struct proc_fd){ 0 };
    fs_lock_drop(&cwd->space->vfs->lock);
    if (err) file_description_close(d);
    return err ? err : slot;
}

int file_seek_locked(struct file_description *d, int64_t off, uint32_t whence, int64_t *out)
{
    if (whence > SEEK_END) return -EINVAL;
    if (d->node->kind == PX_NULL || d->node->kind == PX_CONSOLE) return -ESPIPE;
    if (d->node->kind != PX_FILE) {
        if (off || whence != SEEK_SET) return -EINVAL;
        d->position = 0; d->cursor = 0; *out = 0; return 0;
    }
    uint64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? d->position : d->node->entry.size;
    uint64_t magnitude = off < 0 ? (uint64_t)(-(off + 1)) + 1 : (uint64_t)off;
    if (off < 0 && magnitude > base) return -EINVAL;
    if (off >= 0 && magnitude > (uint64_t)INT64_MAX - base) return -EOVERFLOW;
    d->position = off < 0 ? base - magnitude : base + magnitude;
    *out = (int64_t)d->position;
    return 0;
}
int file_io_locked(struct file_description *d, void *buffer, size_t bytes, bool write,
                    bool positioned, uint64_t offset, size_t *done)
{
    *done = 0;
    if (bytes > CIUKI_IO_MAX) return -EINVAL;
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
    struct fat_entry before = n->entry;
    int err = write ? fat_write(n->volume, &n->entry, position, buffer, bytes, done) :
                      fat_read(n->volume, &n->entry, position, buffer, bytes, done);
    /* F1 COW publishes the owning entry before freeing the old chain and
     * FSInfo. A failure in that suffix cannot erase completed user bytes. */
    if (write && err && (n->entry.first != before.first || n->entry.size != before.size)) *done = bytes;
    if (!positioned && (!err || *done)) d->position = position + *done;
    if (write && memcmp(&before, &n->entry, sizeof(before))) px_changed_locked(n);
    err = px_error(err);
    /* FAT RO attributes were checked at open. A later legacy attribute
     * change remains effective without exposing EACCES outside the ABI set. */
    return err == -EACCES ? -EROFS : err;
}
int file_truncate_locked(struct file_description *d, uint64_t length)
{
    if ((d->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (d->node->kind != PX_FILE) return d->node->kind == PX_NULL || d->node->kind == PX_CONSOLE ? -EINVAL : -EISDIR;
    if (length > INT64_MAX) return -EINVAL;
    if (length > UINT32_MAX) return -EFBIG;
    struct fat_entry before = d->node->entry;
    int err = fat_truncate(d->node->volume, &d->node->entry, length);
    if (memcmp(&before, &d->node->entry, sizeof(before))) px_changed_locked(d->node);
    return err == -FS_EACCES ? -EROFS : px_error(err);
}
int file_sync_locked(struct file_description *d)
{
    if (!d->node->volume) return -EINVAL;
    return px_error(fat_commit(d->node->volume));
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
    fs_lock_take(&s->vfs->lock);
    struct px_path r;
    int err = px_resolve_locked(s, cwd, path, false, &r);
    if (!err && r.node->kind != PX_FILE) err = -EACCES;
    if (!err && r.node->entry.size > CIUKI_ELF_BYTES_MAX) err = -ENOEXEC;
    if (!err) err = px_share_locked(r.node, VFS_READ, 0);
    if (!err) image->bytes = r.node->entry.size;
    for (uint32_t offset = 0; !err && offset < image->bytes; offset += PAGE_SIZE) {
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
        uint32_t bytes = image->bytes - offset;
        if (bytes > PAGE_SIZE) bytes = PAGE_SIZE;
        size_t done = 0;
        err = px_error(fat_read(r.node->volume, &r.node->entry, offset, P2V(page), bytes, &done));
        if (!err && done != bytes) err = -EIO;
    }
    fs_lock_drop(&s->vfs->lock);
    if (err) { image_close(image); return err; }
    *out = (struct ciuki_file){ image, image->bytes, image_read, image_close };
    return 0;
}
