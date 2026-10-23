/* Rows 29..51: copied metadata, stable user ranges, retained descriptions and
 * cwd snapshots, then resource reservation and the F1 commit boundary.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/files.h>
#include <ciuki/clock.h>
#include <ciuki/signal.h>
#include <ciuki/audio.h>
#include <ciuki/net.h>
#include <ciuki/version.h>
#pragma weak audio_write_user
#pragma weak audio_devctl
#pragma weak audio_refill_reader
#pragma weak net_devctl
bool net_socket_object(struct proc_object *) __attribute__((weak));
int32_t net_socket_rw(struct process *, int, uint32_t, uint32_t, bool) __attribute__((weak));
int media_devctl(struct process *, int32_t, uint32_t, uint32_t, uint32_t);
#pragma weak media_devctl
int file_setup_fat(struct process *, struct file_description *, uint32_t, struct ciuki_setup_fat *) __attribute__((weak));
int file_setup_clock(struct process *, const struct ciuki_setup_clock *) __attribute__((weak));

#ifndef CIUKI_BUILD_ID
#define CIUKI_BUILD_ID "unknown"
#endif

static int file_uname(uint32_t address)
{
    struct ciuki_utsname out = { .sysname = "CiukiOS", .nodename = "ciuki",
        .release = CIUKI_BUILD_ID, .version = CIUKI_VERSION, .machine = "i686",
        .abi_version = CIUKI_ABI_VERSION };
    struct clock_seed seed;
    file_clock_snapshot(&seed);
    out.realtime_source = seed.source;
    return copy_to_user(address, &out, sizeof(out));
}

struct file_operation {
    struct process *process;
    struct px_node *cwd;
    struct file_description *description;
    struct ua_pin pin;
    char *path[2];
    void *buffer;
    size_t buffer_bytes; /* nonzero for a page-backed contiguous I/O buffer */
    bool pinned, reading;
    struct vfs_request request;
    struct fs_request_scope scope;
    bool scoped;
};
static void operation_free(void *arg)
{
    struct file_operation *op = arg;
    if (op->scoped) fs_request_leave(&op->scope);
    if (op->pinned) ua_unpin(op->process->memory, &op->pin);
    if (op->description) op->description->object.release(&op->description->object);
    px_release(op->cwd);
    kfree(op->path[0]); kfree(op->path[1]);
    if (op->buffer) {
        if (op->buffer_bytes) fs_contig_free(op->buffer,op->buffer_bytes);
        else fs_page_free(op->buffer);
    }
    kfree(op);
}
/* F1 readsec calls this only before issuing a new synchronous read. The
 * outstanding command has already drained when the next call reaches here. */
bool file_read_cancelled(void)
{
    struct proc_thread *t = proc_thread_for(g_current);
    if (!t || t->cleanup != operation_free) return false;
    struct file_operation *op = t->operation;
    return op && op->reading && (t->process->state == PROC_STOPPING ||
        proc_signal_wait(t, SIGNAL_WAIT_I, false, false, 0) == SIGNAL_WAIT_EINTR);
}
static int path_copy(struct file_operation *op, unsigned which, uint32_t address)
{
    char *path = op->path[which] = kmalloc(CIUKI_PATH_MAX);
    if (!path) return -ENOMEM;
    for (unsigned i = 0; i < CIUKI_PATH_MAX; i++) {
        if (i > UINT32_MAX - address) return -EFAULT;
        int err = copy_from_user(path + i, address + i, 1);
        if (err) return err;
        if (!path[i]) return px_validate(path);
    }
    return -ENAMETOOLONG;
}
static int output_pin(struct file_operation *op, uint32_t address, uint32_t bytes, bool write)
{
    int err = ua_pin(op->process->memory, &op->pin, address, bytes, write);
    op->pinned = !err;
    return err;
}
static bool caught(void)
{
    struct proc_thread *t = proc_thread_for(g_current);
    return t && (t->process->state == PROC_STOPPING || proc_signal_caught(t));
}
static uint64_t file_now(void *arg) { (void)arg; return (uint64_t)fs_now_ms()*1000u; }
static bool file_cancelled(void *arg) { (void)arg; return caught(); }
static void file_pause(void *arg) { (void)arg; task_sleep_ms(1); }
static const char *file_tag(uint32_t nr) {
    switch (nr) {
    case CIUKI_SYS_READ: case CIUKI_SYS_PREAD: return "read";
    case CIUKI_SYS_WRITE: case CIUKI_SYS_PWRITE: return "write";
    case CIUKI_SYS_GETDENTS: return "readdir";
    case CIUKI_SYS_FSYNC: return "commit";
    case CIUKI_SYS_FTRUNCATE: return "truncate";
    case CIUKI_SYS_FSTAT: case CIUKI_SYS_STAT: return "stat";
    case CIUKI_SYS_MKDIR: return "mkdir";
    case CIUKI_SYS_RMDIR: return "rmdir";
    case CIUKI_SYS_UNLINK: return "unlink";
    case CIUKI_SYS_RENAME: return "rename";
    case CIUKI_SYS_CHDIR: return "chdir";
    case CIUKI_SYS_GETCWD: return "getcwd";
    case CIUKI_SYS_LSEEK64: return "seek";
    case CIUKI_SYS_OPEN: return "open";
    default: return "metadata";
    }
}
static int64_t scalar64(uint32_t lo, uint32_t hi)
{
    uint64_t bits = lo | (uint64_t)hi << 32;
    int64_t value; memcpy(&value, &bits, sizeof(value)); return value;
}

/* f9-02 /dev/ataN: pread/pwrite of whole sectors, one request of at most
 * 64 KiB (128 sectors) per call, authority checked on each call. No
 * namespace lock is held across the request. */
static int disk_byte_io(struct file_operation *op, const struct trap_frame *tf, bool write, bool positioned)
{
    struct file_description *d = op->description;
    uint32_t mode = d->flags & O_ACCMODE;
    if (write ? mode == O_RDONLY : mode == O_WRONLY) return -EBADF;
    if (!positioned) return -ESPIPE;
    int64_t offset = scalar64(tf->esi, tf->edi);
    if (offset < 0) return -EINVAL;
    if (!tf->edx) return 0;
    uint32_t bytes = tf->edx < CIUKI_DISK_IO_MAX ? tf->edx : CIUKI_DISK_IO_MAX;
    int err = output_pin(op, tf->ecx, bytes, !write);
    if (err) return err;
    op->buffer_bytes = bytes;
    op->buffer = fs_contig_alloc(bytes);
    if (!op->buffer) return -ENOMEM;
    if (write && (err = copy_from_user(op->buffer, tf->ecx, bytes))) return err;
    int done = disk_node_io ? disk_node_io(op->process, d->node->drive, write, (uint64_t)offset, op->buffer, bytes) : -ENODEV;
    if (done > 0 && !write && (err = copy_to_user(tf->ecx, op->buffer, (uint32_t)done))) return err;
    return done;
}

static int byte_io(struct file_operation *op, const struct trap_frame *tf)
{
    bool write = tf->eax == CIUKI_SYS_WRITE || tf->eax == CIUKI_SYS_PWRITE;
    bool positioned = tf->eax == CIUKI_SYS_PREAD || tf->eax == CIUKI_SYS_PWRITE;
    struct file_description *d = op->description;
    if (d->node->kind == PX_DISK) return disk_byte_io(op, tf, write, positioned);
    vfs_lock(d->node->space->vfs,d->node->ro ? vfs_drive_mask(d->node->drive) : 0);
    vfs_tag_locked(d->node->space->vfs,write ? "write" : "read");
    vfs_site_locked(d->node->space->vfs,"native");
    int media_error=d->node->volume && !write ?
        (d->node->drive<26 && d->node->volume->mounted && !d->node->volume->dev->quarantined &&
         d->node->space->vfs->volumes[d->node->drive]==d->node->volume &&
         d->node->space->vfs->generation[d->node->drive]==d->node->generation ? 0 : -EIO) : px_ro_check_locked(d->node);
    if (!media_error && write && d->node->ro) media_error=-EROFS;
    vfs_unlock(d->node->space->vfs);
    if (media_error) return media_error;
    uint32_t mode = d->flags & O_ACCMODE;
    if (write ? mode == O_RDONLY : mode == O_WRONLY) return -EBADF;
    if (d->node->kind == PX_AUDIO || d->node->kind == PX_MIXER || d->node->kind == PX_NET || d->node->kind == PX_MEDIA) {
        if (positioned) return -ESPIPE;
        if (d->node->kind == PX_MIXER || !write) return -EBADF;
        return -EBADF; /* audio write dispatch owns its own cancellable operation */
    }
    if (d->node->kind != PX_FILE && d->node->kind != PX_NULL && d->node->kind != PX_CONSOLE) return -EISDIR;
    if (positioned && d->node->kind != PX_FILE) return -ESPIPE;
    int64_t offset = scalar64(tf->esi, tf->edi);
    if (positioned && offset < 0) return -EINVAL;
    if (!tf->edx) return 0;
    int err = output_pin(op, tf->ecx, tf->edx, !write);
    if (err) return err;
    /* One bounded run per syscall. A larger request returns short, releasing
     * the namespace lock before userspace retries the next run. This keeps
     * a multi-MiB cache write from holding the lock for its entire payload. */
    uint32_t limit = d->node->kind==PX_FILE ? BLKDEV_READ_BYTES : PAGE_SIZE;
    uint32_t bytes = tf->edx < limit ? tf->edx : limit;
    if (d->node->kind == PX_CONSOLE && bytes > 1024) bytes = 1024;
    if (d->node->kind==PX_FILE) {
        op->buffer_bytes=bytes;
        op->buffer=fs_contig_alloc(bytes);
    } else op->buffer=fs_page_alloc();
    if (!op->buffer) return -ENOMEM;
    if (write && (err = copy_from_user(op->buffer, tf->ecx, bytes))) return err;
    struct vfs *v = d->node->space->vfs;
    vfs_lock(v,(write || d->node->ro) && (d->node->volume || d->node->ro) ? vfs_drive_mask(d->node->drive) : 0);
    vfs_tag_locked(v,write ? "write" : "read"); vfs_site_locked(v,"native");
    if (caught()) err = -EINTR;
    uint64_t position = positioned ? (uint64_t)offset : write && (d->flags & O_APPEND) ? d->node->entry.size : d->position;
    /* Check the ENTIRE request before a permitted short transfer. */
    if (!err && write && d->node->kind == PX_FILE &&
        (position > UINT32_MAX || tf->edx > UINT32_MAX - position)) err = -EFBIG;
    size_t done = 0;
    uint64_t saved_position = d->position;
    op->reading = !write;
    if (!err) err = file_io_locked(d, op->buffer, bytes, write, positioned, (uint64_t)offset, &done);
    if (!write && d->node->kind==PX_FILE && !d->node->ro)
        saved_position=positioned ? d->position : d->position-done;
    op->reading = false;
    if (!write && caught()) {
        /* F1 reads are synchronous. Keep pins until completion; no byte has
         * reached the caller yet, so discard a successfully drained result. */
        d->position = saved_position;
        err = err && err != -EINTR && err != -VFS_ECANCELLED ? -EIO : -EINTR;
        done = 0;
    }
    if (!write && done) {
        int copy = copy_to_user(tf->ecx, op->buffer, (uint32_t)done);
        if (copy) { d->position = saved_position; done = 0; err = copy; }
    }
    if (write && d->node->kind == PX_NULL && !err) done = tf->edx;
    vfs_unlock(v);
    return done ? (int)done : err;
}

static int fd_call(struct file_operation *op, const struct trap_frame *tf)
{
    struct file_description *d = op->description;
    if (tf->eax == CIUKI_SYS_READ || tf->eax == CIUKI_SYS_WRITE ||
        tf->eax == CIUKI_SYS_PREAD || tf->eax == CIUKI_SYS_PWRITE) return byte_io(op, tf);
    int err = 0;
    if (tf->eax == CIUKI_SYS_GETDENTS && d->node->kind != PX_DIRECTORY &&
        d->node->kind != PX_MNT && d->node->kind != PX_DEV) return -ENOTDIR;
    if (tf->eax == CIUKI_SYS_LSEEK64 && (d->node->kind == PX_NULL || d->node->kind == PX_CONSOLE || d->node->kind == PX_AUDIO || d->node->kind == PX_MIXER || d->node->kind == PX_NET || d->node->kind == PX_MEDIA || d->node->kind == PX_DISK)) return -ESPIPE;
    if (tf->eax == CIUKI_SYS_LSEEK64)
        err = output_pin(op, tf->edi, sizeof(int64_t), true);
    else if (tf->eax == CIUKI_SYS_FSTAT)
        err = output_pin(op, tf->ecx, sizeof(struct ciuki_stat), true);
    else if (tf->eax == CIUKI_SYS_GETDENTS) {
        err = output_pin(op, tf->ecx, tf->edx, true);
        if (!err) { op->buffer = fs_page_alloc(); if (!op->buffer) err = -ENOMEM; }
    }
    if (err) return err;
    vfs_lock(d->node->space->vfs,(d->node->volume || d->node->ro) ? vfs_drive_mask(d->node->drive) : 0);
    if (tf->eax != CIUKI_SYS_LSEEK64 && caught()) err = -EINTR;
    if (!err) switch (tf->eax) {
    case CIUKI_SYS_LSEEK64: {
        int64_t position;
        vfs_tag_locked(d->node->space->vfs,"seek"); vfs_site_locked(d->node->space->vfs,"native"); /* f1-93b */
        err = file_seek_locked(d, scalar64(tf->ecx, tf->edx), tf->esi, &position);
        if (!err) err = copy_to_user(tf->edi, &position, sizeof(position));
        break;
    }
    case CIUKI_SYS_FSTAT: {
        struct ciuki_stat out;
        err = px_stat_locked(d->node, &out);
        if (!err) err = copy_to_user(tf->ecx, &out, sizeof(out));
        break;
    }
    case CIUKI_SYS_GETDENTS: {
        uint32_t done = 0;
        while (tf->edx - done >= sizeof(struct ciuki_dirent)) {
            uint64_t old_position = d->position; uint32_t old_cursor = d->cursor;
            op->reading = true;
            err = px_getdents_locked(d, op->buffer);
            op->reading = false;
            if (caught()) {
                /* A drained record has not reached userspace yet. Preserve
                 * its cookie just as read preserves an interrupted offset. */
                d->position = old_position; d->cursor = old_cursor;
                err = err < 0 && err != -EINTR ? -EIO : -EINTR;
                break;
            }
            if (err <= 0) break;
            int copy = copy_to_user(tf->ecx + done, op->buffer, sizeof(struct ciuki_dirent));
            if (copy) { d->position = old_position; d->cursor = old_cursor; err = copy; break; }
            done += sizeof(struct ciuki_dirent);
            if (caught()) { err = -EINTR; break; }
        }
        if (done) err = (int)done;
        break;
    }
    case CIUKI_SYS_FSYNC: err = file_sync_locked(d); break;
    case CIUKI_SYS_FTRUNCATE: err = file_truncate_locked(d, (uint64_t)scalar64(tf->ecx, tf->edx)); break;
    default: err = -ENOSYS; break;
    }
    vfs_unlock(d->node->space->vfs);
    return err;
}

static int pathname_call(struct file_operation *op, const struct trap_frame *tf)
{
    if (!op->cwd) return -ENOENT;
    int err = path_copy(op, 0, tf->ebx);
    if (!err && tf->eax == CIUKI_SYS_RENAME) err = path_copy(op, 1, tf->ecx);
    if (!err && tf->eax == CIUKI_SYS_STAT) err = output_pin(op, tf->ecx, sizeof(struct ciuki_stat), true);
    if (err) return err;
    struct px_namespace *s = op->cwd->space;
    if (tf->eax == CIUKI_SYS_OPEN) {
        /* The table is shared, while cwd is the retained entry snapshot. */
        struct process view = *op->process; view.cwd = op->cwd;
        return file_open(&view, op->path[0], tf->ecx, tf->edx);
    }
    vfs_lock(s->vfs,0);
    if (caught()) err = -EINTR;
    if (!err) switch (tf->eax) {
    case CIUKI_SYS_MKDIR: err = px_mkdir_locked(s, op->cwd, op->path[0], tf->ecx); break;
    case CIUKI_SYS_RMDIR: case CIUKI_SYS_UNLINK:
        err = px_remove_locked(s, op->cwd, op->path[0], tf->eax == CIUKI_SYS_RMDIR); break;
    case CIUKI_SYS_RENAME: err = px_rename_locked(s, op->cwd, op->path[0], op->path[1]); break;
    case CIUKI_SYS_CHDIR: case CIUKI_SYS_STAT: {
        struct px_path result;
        err = px_resolve_locked(s, op->cwd, op->path[0], false, &result);
        if (!err && tf->eax == CIUKI_SYS_CHDIR) {
            if (result.node->kind != PX_DIRECTORY && result.node->kind != PX_MNT && result.node->kind != PX_DEV) err = -ENOTDIR;
            else {
                px_retain(result.node); px_release(op->process->cwd);
                op->process->cwd = result.node;
                op->process->cwd_retain = px_retain; op->process->cwd_release = px_release;
            }
        } else if (!err) {
            struct ciuki_stat out;
            err = px_stat_locked(result.node, &out);
            if (!err) err = copy_to_user(tf->ecx, &out, sizeof(out));
        }
        break;
    }
    default: err = -ENOSYS; break;
    }
    vfs_unlock(s->vfs);
    return err;
}

static int file_dispatch(struct process *p, struct trap_frame *tf)
{
    uint32_t nr = tf->eax;
    if (nr == CIUKI_SYS_DEVCTL) {
        struct file_description *d = file_fd(p, (int32_t)tf->ebx);
        if (tf->ecx==CIUKI_SETUP_FAT_QUERY || tf->ecx==CIUKI_SETUP_FAT_ATTRIB) {
            if (!d) return -EBADF;
            if (tf->esi!=sizeof(struct ciuki_setup_fat)) return -EINVAL;
            struct ciuki_setup_fat q;
            int e=copy_from_user(&q,tf->edx,sizeof(q));
            if (e) return e;
            d->object.retain(&d->object);
            e=file_setup_fat ? file_setup_fat(p,d,tf->ecx,&q) : -ENOSYS;
            d->object.release(&d->object);
            return e ? e : copy_to_user(tf->edx,&q,sizeof(q));
        }
        if (tf->ecx==CIUKI_SETUP_CLOCK_SET) {
            if (!d) return -EBADF;
            if (tf->esi!=sizeof(struct ciuki_setup_clock)) return -EINVAL;
            struct ciuki_setup_clock q;
            int e=copy_from_user(&q,tf->edx,sizeof(q));
            return e ? e : file_setup_clock ? file_setup_clock(p,&q) : -ENOSYS;
        }
        if (d && d->node->kind == PX_MEDIA)
            return media_devctl ? media_devctl(p, (int32_t)tf->ebx, tf->ecx, tf->edx, tf->esi) : -ENOTTY;
        if (d && d->node->kind == PX_NET)
            return net_devctl ? net_devctl(p, (int32_t)tf->ebx, tf->ecx, tf->edx, tf->esi) : -ENOTTY;
        if (d && d->node->kind == PX_DISK)
            return disk_devctl ? disk_devctl(p, (int32_t)tf->ebx, tf->ecx, tf->edx, tf->esi) : -ENOTTY;
        return audio_devctl ? audio_devctl(p, (int32_t)tf->ebx, tf->ecx, tf->edx, tf->esi) :
               file_fd_object(p, (int32_t)tf->ebx) ? -ENOTTY : -EBADF;
    }
    if (nr == CIUKI_SYS_UNAME) return file_uname(tf->ebx);
    /* Flags/structure checks precede descriptor and pointer validation. */
    if (nr == CIUKI_SYS_OPEN) { int err = file_open_flags(tf->ecx, tf->edx); if (err) return err; }
    if (nr == CIUKI_SYS_MKDIR && (tf->ecx & ~0777u)) return -EINVAL;
    if ((nr == CIUKI_SYS_READ || nr == CIUKI_SYS_WRITE || nr == CIUKI_SYS_PREAD || nr == CIUKI_SYS_PWRITE) && tf->edx > CIUKI_IO_MAX) return -EINVAL;
    if (nr == CIUKI_SYS_GETDENTS && (tf->edx < CIUKI_GETDENTS_MIN || tf->edx > CIUKI_GETDENTS_MAX)) return -EINVAL;
    if (nr == CIUKI_SYS_LSEEK64 && tf->esi > SEEK_END) return -EINVAL;
    if (nr == CIUKI_SYS_CLOSE) return file_close(p, (int32_t)tf->ebx);
    if (nr == CIUKI_SYS_DUP) return file_dup(p, (int32_t)tf->ebx, 0, false, 0);
    if (nr == CIUKI_SYS_DUP2) return file_dup(p, (int32_t)tf->ebx, (int32_t)tf->ecx, true, 0);
    if (nr == CIUKI_SYS_FCNTL) return file_fcntl(p, (int32_t)tf->ebx, tf->ecx, tf->edx);
    if (nr == CIUKI_SYS_CLOCK_GETTIME) {
        struct ciuki_timespec now;
        int err = file_clock_now(tf->ebx, p, &now);
        return err ? err : copy_to_user(tf->ecx, &now, sizeof(now));
    }
    if (nr == CIUKI_SYS_NANOSLEEP) return file_nanosleep(tf->ebx, tf->ecx);
    bool fd_call_needed = (nr >= CIUKI_SYS_READ && nr <= CIUKI_SYS_FSTAT) ||
                         nr == CIUKI_SYS_GETDENTS || nr == CIUKI_SYS_FSYNC || nr == CIUKI_SYS_FTRUNCATE;
    struct file_description *d = 0;
    if (fd_call_needed && net_socket_object && net_socket_object(file_fd_object(p, (int32_t)tf->ebx))) {
        if (nr == CIUKI_SYS_READ || nr == CIUKI_SYS_WRITE)
            return net_socket_rw ? net_socket_rw(p, (int32_t)tf->ebx, tf->ecx, tf->edx, nr == CIUKI_SYS_WRITE) : -ENOSYS;
        if (nr == CIUKI_SYS_FSTAT) return -EOPNOTSUPP;
        if (nr == CIUKI_SYS_FSYNC || nr == CIUKI_SYS_FTRUNCATE) return -EINVAL;
        return nr == CIUKI_SYS_GETDENTS ? -ENOTDIR : -ESPIPE;
    }
    if (fd_call_needed) {
        if (!file_fd_object(p, (int32_t)tf->ebx)) return -EBADF;
        d = file_fd(p, (int32_t)tf->ebx);
        if (!d) return nr == CIUKI_SYS_FSTAT ? -EOPNOTSUPP : nr == CIUKI_SYS_GETDENTS ? -ENOTDIR :
            nr == CIUKI_SYS_LSEEK64 || nr == CIUKI_SYS_PREAD || nr == CIUKI_SYS_PWRITE ? -ESPIPE : -EBADF;
    }
    if (d && d->node->kind == PX_AUDIO && nr == CIUKI_SYS_WRITE)
        return audio_write_user ? audio_write_user(d, tf->ecx, tf->edx) : -ENODEV;
    /* f9-02: fsync of /dev/ataN is DISK_FLUSH, outside the namespace lock. */
    if (d && d->node->kind == PX_DISK && nr == CIUKI_SYS_FSYNC)
        return disk_node_sync ? disk_node_sync(p, d->node->drive) : -ENODEV;
    if (d && d->node->kind == PX_DISK && nr == CIUKI_SYS_FTRUNCATE) return -EINVAL;
    struct file_operation *op = kzalloc(sizeof(*op));
    if (!op) return -ENOMEM;
    op->process = p; op->cwd = p->cwd; px_retain(op->cwd);
    op->description = d; if (d) d->object.retain(&d->object);
    op->request=(struct vfs_request){.deadline=UINT64_MAX,.now=file_now,
        .cancelled=(nr==CIUKI_SYS_READ || nr==CIUKI_SYS_PREAD) ? file_cancelled : 0,
        .pause=file_pause,.operation=file_tag(nr),.site="native",
        .urgent_reader=(nr==CIUKI_SYS_READ || nr==CIUKI_SYS_PREAD) && audio_refill_reader && audio_refill_reader(p,tf->edx)};
    fs_request_enter(&op->scope,&op->request); op->scoped=true;
    struct proc_thread *thread = proc_thread_for(g_current);
    if (thread) { thread->cleanup = operation_free; thread->operation = op; }
    int err;
    if (fd_call_needed) err = fd_call(op, tf);
    else if (nr == CIUKI_SYS_GETCWD) {
        err = !tf->ecx ? -EINVAL : !op->cwd ? -ENOENT : 0;
        if (!err) { op->path[0] = kmalloc(CIUKI_PATH_MAX); if (!op->path[0]) err = -ENOMEM; }
        if (!err) {
            vfs_lock(op->cwd->space->vfs,0);
            err = px_getcwd_locked(op->cwd, op->path[0]);
            vfs_unlock(op->cwd->space->vfs);
        }
        if (!err) {
            uint32_t bytes = (uint32_t)strlen(op->path[0]) + 1;
            if (bytes > tf->ecx) err = -ERANGE;
            else { err = copy_to_user(tf->ebx, op->path[0], bytes); if (!err) err = (int)bytes; }
        }
    } else err = pathname_call(op, tf);
    if (thread) { thread->cleanup = 0; thread->operation = 0; }
    operation_free(op);
    return err;
}
bool file_syscall(struct trap_frame *tf)
{
    if ((tf->eax < CIUKI_SYS_OPEN || tf->eax > CIUKI_SYS_NANOSLEEP) && tf->eax != CIUKI_SYS_UNAME && tf->eax != CIUKI_SYS_DEVCTL) return false;
    struct process *p = proc_current();
    tf->eax = (uint32_t)(p ? file_dispatch(p, tf) : -ENOSYS);
    return true;
}
