/* f9-02 raw whole-disk nodes /dev/ata0..3: copy records and the calling
 * process's identity into the lease code (fs/disklease.c), which decides.
 * The caller is the process issuing THIS request, so a descriptor that was
 * inherited, duplicated or passed keeps no authority of its opener.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/storage.h>

static struct storage_caller caller(const struct process *p)
{
    return (struct storage_caller){ p ? p->pid : 0, p && p->setup_capability };
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
int disk_devctl(struct process *p, int32_t fd, uint32_t op, uint32_t address, uint32_t bytes)
{
    if (!file_fd_object(p, fd)) return -EBADF;
    struct file_description *d = file_fd(p, fd);
    if (!d || d->node->kind != PX_DISK) return -ENOTTY;
    if (op < DISK_GET_INFO || op > DISK_RECHECK) return -ENOTTY;
    union {
        struct ciuki_disk_info info;
        struct ciuki_disk_request request;
        struct ciuki_disk_lease lease;
        struct ciuki_disk_recheck recheck;
    } record;
    if (bytes < sizeof(record.request) || bytes > sizeof(record)) return -EINVAL;
    int err = copy_from_user(&record, address, bytes);
    if (err) return err;
    unsigned slot = d->node->drive;
    if (!disk_node_present(slot)) return -ENXIO;
    struct storage_caller c = caller(p);
    err = storage_disk_control(storage_get(), slot, &c, op, &record, bytes);
    return err ? err : copy_to_user(address, &record, bytes);
}
/* Only a process with the capability can have acquired a lease, so other
 * exits (every exit of an ordinary boot) do no work here. */
void disk_process_exit(struct process *p)
{
    if (p && p->setup_capability) storage_disk_exit(storage_get(), p->pid);
}
/* Namespace lock held by the caller (posixpath.c, fdtable.c). */
bool disk_write_denied(unsigned drive)
{
    struct process *p = proc_current();
    return storage_disk_write_denied(storage_get(), drive, p ? p->pid : 0);
}
