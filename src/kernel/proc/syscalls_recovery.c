/* G14 system call 99, CIUKI_SYS_RECOVERY (docs/design/startup-recovery.md
 * section 3; abi.h): the boot status and Automatic Skip Driver record,
 * selective startup, and ScanDisk. EBX=operation, ECX=record, EDX=size.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/security_storage.h>
#include <ciuki/abi.h>
#include <ciuki/process.h>
#include <ciuki/uaddr.h>
#include <ciuki/bootstat.h>
#include <ciuki/storage.h>
#include "../fs/fs_port.h"

/* Storage results are negative FS_* codes (fs_port.h). */
static int fs_errno(int e)
{
    return e == -FS_ENOENT ? -ENODEV : e == -FS_EBUSY ? -EBUSY : e == -FS_EINVAL ? -EINVAL :
        e == -FS_EROFS ? -EROFS : e == -FS_EIO ? -EIO : e == -FS_ENOMEM ? -ENOMEM : e;
}

int32_t recovery_syscall_subject(struct process *p, const struct security_subject *subject, struct trap_frame *tf)
{
    if (!security_process_live(p) || !security_subject_live(subject)) return -EACCES;
    if (subject!=p->subject && !security_device_system(p)) return -EPERM;
    bool authority = security_storage_admin(subject);
    uint32_t op = tf->ebx;
    if (op == CIUKI_RECOVERY_SCANDISK || op == CIUKI_RECOVERY_REPORT) {
        struct ciuki_scandisk r;
        if (tf->edx != sizeof(r)) return -EINVAL;
        if (copy_from_user(&r, tf->ecx, sizeof(r))) return -EFAULT;
        if (r.version != CIUKI_SCANDISK_VERSION || r.size != sizeof(r) || r.drive >= 26) return -EINVAL;
        int e;
        if (op == CIUKI_RECOVERY_REPORT) e = storage_scandisk_report(r.drive, &r);
        else {
            if (!authority) return -EPERM;
            struct storage *storage=storage_get();
            if (!storage || !storage->ready) return -ENODEV;
            vfs_lock(&storage->vfs,0);
            e=security_storage_broker(subject,storage,r.drive);
            vfs_unlock(&storage->vfs);
            if (e) return e;
            if (r.flags & ~(uint32_t)(CIUKI_SCANDISK_FIX | CIUKI_SCANDISK_THOROUGH |
                                      CIUKI_SCANDISK_FREE_LOST | CIUKI_SCANDISK_NO_COPY)) return -EINVAL;
            bool schedule = false;
            e = storage_scandisk(r.drive, &r, &schedule);
            if (e == -FS_ENOENT || e == -FS_EBUSY || e == -FS_EINVAL) return fs_errno(e);
            if (schedule && !bootstat_schedule_scan(r.flags & CIUKI_SCANDISK_THOROUGH))
                r.flags |= CIUKI_SCANDISK_SCHEDULED;
            e = 0; /* the check's own outcome is r.result */
        }
        if (e) return fs_errno(e);
        return copy_to_user(tf->ecx, &r, sizeof(r)) ? -EFAULT : 0;
    }
    struct ciuki_recovery_status s;
    if (tf->edx != sizeof(s)) return -EINVAL;
    if (op == CIUKI_RECOVERY_STATUS) {
        bootstat_status(&s);
        if (authority) s.flags |= CIUKI_RECOVERY_AUTHORITY;
        return copy_to_user(tf->ecx, &s, sizeof(s)) ? -EFAULT : 0;
    }
    if (op != CIUKI_RECOVERY_SET && op != CIUKI_RECOVERY_ENABLE) return -EINVAL;
    if (copy_from_user(&s, tf->ecx, sizeof(s))) return -EFAULT;
    if (s.version != CIUKI_RECOVERY_VERSION || s.size != sizeof(s)) return -EINVAL;
    if (!authority) return -EPERM;
    int e = op == CIUKI_RECOVERY_SET ? bootstat_configure(s.disabled, s.request) : bootstat_enable(s.skipped);
    return e == -FS_EINVAL ? -EINVAL : fs_errno(e);
}

int32_t recovery_syscall(struct trap_frame *tf)
{
    struct process *p=proc_current();
    return recovery_syscall_subject(p,p ? p->subject : 0,tf);
}
