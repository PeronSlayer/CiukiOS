/* Rows 29..51: copied metadata, stable user ranges, retained descriptions and
 * cwd snapshots, then resource reservation and the F1 commit boundary.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/files.h>
#include <ciuki/clock.h>
#include <ciuki/signal.h>

struct file_operation {
    struct process *process;
    struct px_node *cwd;
    struct file_description *description;
    struct ua_pin pin;
    char *path[2];
    void *buffer;
    bool pinned, reading;
};
static void operation_free(void *arg)
{
    struct file_operation *op = arg;
    if (op->pinned) ua_unpin(op->process->memory, &op->pin);
    if (op->description) op->description->object.release(&op->description->object);
    px_release(op->cwd);
    kfree(op->path[0]); kfree(op->path[1]);
    if (op->buffer) fs_page_free(op->buffer);
    kfree(op);
}
/* F1 readsec calls this only before issuing a new synchronous read. The
 * outstanding command has already drained when the next call reaches here. */
bool file_read_cancelled(void)
{
    struct proc_thread *t = proc_thread_for(g_current);
    if (!t || t->cleanup != operation_free) return false;
    struct file_operation *op = t->operation;
    return op && op->reading && (t->process->state == PROC_STOPPING || proc_signal_caught(t));
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
static int64_t scalar64(uint32_t lo, uint32_t hi)
{
    uint64_t bits = lo | (uint64_t)hi << 32;
    int64_t value; memcpy(&value, &bits, sizeof(value)); return value;
}

static int byte_io(struct file_operation *op, const struct trap_frame *tf)
{
    bool write = tf->eax == CIUKI_SYS_WRITE || tf->eax == CIUKI_SYS_PWRITE;
    bool positioned = tf->eax == CIUKI_SYS_PREAD || tf->eax == CIUKI_SYS_PWRITE;
    struct file_description *d = op->description;
    uint32_t mode = d->flags & O_ACCMODE;
    if (write ? mode == O_RDONLY : mode == O_WRONLY) return -EBADF;
    if (d->node->kind != PX_FILE && d->node->kind != PX_NULL && d->node->kind != PX_CONSOLE) return -EISDIR;
    if (positioned && d->node->kind != PX_FILE) return -ESPIPE;
    int64_t offset = scalar64(tf->esi, tf->edi);
    if (positioned && offset < 0) return -EINVAL;
    if (!tf->edx) return 0;
    int err = output_pin(op, tf->ecx, tf->edx, !write);
    if (err) return err;
    op->buffer = fs_page_alloc();
    if (!op->buffer) return -ENOMEM;
    uint32_t bytes = tf->edx < PAGE_SIZE ? tf->edx : PAGE_SIZE;
    if (d->node->kind == PX_CONSOLE && bytes > 1024) bytes = 1024;
    if (write && (err = copy_from_user(op->buffer, tf->ecx, bytes))) return err;
    struct vfs *v = d->node->space->vfs;
    fs_lock_take(&v->lock);
    if (caught()) err = -EINTR;
    uint64_t position = positioned ? (uint64_t)offset : write && (d->flags & O_APPEND) ? d->node->entry.size : d->position;
    /* Check the ENTIRE request before a permitted short transfer. */
    if (!err && write && d->node->kind == PX_FILE &&
        (position > UINT32_MAX || tf->edx > UINT32_MAX - position)) err = -EFBIG;
    size_t done = 0;
    uint64_t saved_position = d->position;
    op->reading = !write;
    if (!err) err = file_io_locked(d, op->buffer, bytes, write, positioned, (uint64_t)offset, &done);
    op->reading = false;
    if (!write && caught()) {
        /* F1 reads are synchronous. Keep pins until completion; no byte has
         * reached the caller yet, so discard a successfully drained result. */
        d->position = saved_position;
        err = err && err != -EINTR ? -EIO : -EINTR;
        done = 0;
    }
    if (!write && done) {
        int copy = copy_to_user(tf->ecx, op->buffer, (uint32_t)done);
        if (copy) { d->position = saved_position; done = 0; err = copy; }
    }
    if (write && d->node->kind == PX_NULL && !err) done = tf->edx;
    fs_lock_drop(&v->lock);
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
    if (tf->eax == CIUKI_SYS_LSEEK64 && (d->node->kind == PX_NULL || d->node->kind == PX_CONSOLE)) return -ESPIPE;
    if (tf->eax == CIUKI_SYS_LSEEK64)
        err = output_pin(op, tf->edi, sizeof(int64_t), true);
    else if (tf->eax == CIUKI_SYS_FSTAT)
        err = output_pin(op, tf->ecx, sizeof(struct ciuki_stat), true);
    else if (tf->eax == CIUKI_SYS_GETDENTS) {
        err = output_pin(op, tf->ecx, tf->edx, true);
        if (!err) { op->buffer = fs_page_alloc(); if (!op->buffer) err = -ENOMEM; }
    }
    if (err) return err;
    fs_lock_take(&d->node->space->vfs->lock);
    if (tf->eax != CIUKI_SYS_LSEEK64 && caught()) err = -EINTR;
    if (!err) switch (tf->eax) {
    case CIUKI_SYS_LSEEK64: {
        int64_t position;
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
    fs_lock_drop(&d->node->space->vfs->lock);
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
    fs_lock_take(&s->vfs->lock);
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
    fs_lock_drop(&s->vfs->lock);
    return err;
}

static int file_dispatch(struct process *p, struct trap_frame *tf)
{
    uint32_t nr = tf->eax;
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
    if (fd_call_needed) {
        if (!file_fd_object(p, (int32_t)tf->ebx)) return -EBADF;
        d = file_fd(p, (int32_t)tf->ebx);
        if (!d) return nr == CIUKI_SYS_FSTAT ? -EOPNOTSUPP : nr == CIUKI_SYS_GETDENTS ? -ENOTDIR :
            nr == CIUKI_SYS_LSEEK64 || nr == CIUKI_SYS_PREAD || nr == CIUKI_SYS_PWRITE ? -ESPIPE : -EBADF;
    }
    struct file_operation *op = kzalloc(sizeof(*op));
    if (!op) return -ENOMEM;
    op->process = p; op->cwd = p->cwd; px_retain(op->cwd);
    op->description = d; if (d) d->object.retain(&d->object);
    struct proc_thread *thread = proc_thread_for(g_current);
    if (thread) { thread->cleanup = operation_free; thread->operation = op; }
    int err;
    if (fd_call_needed) err = fd_call(op, tf);
    else if (nr == CIUKI_SYS_GETCWD) {
        err = !tf->ecx ? -EINVAL : !op->cwd ? -ENOENT : 0;
        if (!err) { op->path[0] = kmalloc(CIUKI_PATH_MAX); if (!op->path[0]) err = -ENOMEM; }
        if (!err) {
            fs_lock_take(&op->cwd->space->vfs->lock);
            err = px_getcwd_locked(op->cwd, op->path[0]);
            fs_lock_drop(&op->cwd->space->vfs->lock);
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
    if (tf->eax < CIUKI_SYS_OPEN || tf->eax > CIUKI_SYS_NANOSLEEP) return false;
    struct process *p = proc_current();
    tf->eax = (uint32_t)(p ? file_dispatch(p, tf) : -ENOSYS);
    return true;
}
