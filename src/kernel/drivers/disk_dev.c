/* f9-02 raw whole-disk nodes /dev/ata0..3: copy records and the calling
 * process's identity into the lease code (fs/disklease.c), which decides.
 * The caller is the process issuing THIS request, so a descriptor that was
 * inherited, duplicated or passed keeps no authority of its opener.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/storage.h>
#include <ciuki/security_device.h>

static struct storage_caller caller(const struct process *p)
{
    return (struct storage_caller){ .pid = p ? p->pid : 0,
        .setup_capability = security_device_setup(p),
        .administrator = security_device_admin(p), .system = security_device_system(p) };
}
/* storage_add_disk records the slot's device once at boot and never clears
 * it; ATAPI and empty slots stay NULL. Safe without the namespace lock. */
bool disk_node_present(unsigned slot)
{
    struct storage *s = storage_get();
    return slot < STORAGE_DISKS && s->ready && __atomic_load_n(&s->raw[slot].dev, __ATOMIC_ACQUIRE);
}
int disk_node_io(struct process *p, unsigned slot, bool write, uint64_t offset, void *buffer, uint32_t bytes)
{
    if (!disk_node_present(slot)) return -ENXIO;
    struct storage_caller c = caller(p);
    return storage_disk_io(storage_get(), slot, &c, write, offset, buffer, bytes);
}
int disk_node_sync(struct process *p, unsigned slot)
{
    if (!disk_node_present(slot)) return -ENXIO;
    struct storage_caller c = caller(p);
    return storage_disk_flush(storage_get(), slot, &c);
}
struct disk_control_operation {
    struct process *process;
    struct file_description *description;
    struct ua_pin pin;
    bool pinned;
};
static void disk_control_free(void *arg)
{
    struct disk_control_operation *o = arg;
    if (o->pinned) ua_unpin(o->process->memory, &o->pin);
    o->description->object.release(&o->description->object);
    kfree(o);
}
int disk_devctl(struct process *p, int32_t fd, uint32_t op, uint32_t address, uint32_t bytes)
{
    if (!file_fd_object(p, fd)) return -EBADF;
    struct file_description *d = file_fd(p, fd);
    if (!d || d->node->kind != PX_DISK) return -ENOTTY;
    if (op < DISK_GET_INFO || op > DISK_PARTITION_ACQUIRE) return -ENOTTY;
    union {
        struct ciuki_disk_info info;
        struct ciuki_disk_request request;
        struct ciuki_disk_lease lease;
        struct ciuki_disk_recheck recheck;
    } record;
    if (bytes < sizeof(record.request) || bytes > sizeof(record)) return -EINVAL;
    unsigned slot = d->node->drive;
    if (!disk_node_present(slot)) return -ENXIO;
    struct disk_control_operation *o = kzalloc(sizeof(*o));
    if (!o) return -ENOMEM;
    o->process = p; o->description = d;
    d->object.retain(&d->object);
    struct proc_thread *thread = proc_thread_for(g_current);
    if (thread) { thread->cleanup = disk_control_free; thread->operation = o; }
    int err = copy_from_user(&record, address, bytes);
    /* All requests return a record. Reject an unwritable output before any
     * lease/table state changes and retain its backing across blocking I/O. */
    if (!err) { err = ua_pin(p->memory, &o->pin, address, bytes, true); o->pinned = !err; }
    struct storage_caller c = caller(p);
    if (!err) err = storage_disk_control(storage_get(), slot, &c, op, &record, bytes);
    if (!err) err = copy_to_user(address, &record, bytes);
    if (thread) { thread->cleanup = 0; thread->operation = 0; }
    disk_control_free(o);
    return err;
}
/* Also revoke an ordinary nonboot partition editor's lease at exit. */
void disk_process_exit(struct process *p)
{
    if (p) storage_disk_exit(storage_get(), p->pid);
}
/* Namespace lock held by the caller (posixpath.c, fdtable.c). */
bool disk_write_denied(unsigned drive)
{
    struct process *p = proc_current();
    return storage_disk_write_denied(storage_get(), drive, security_device_setup(p) ? p->pid : 0);
}
