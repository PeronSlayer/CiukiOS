/* The native print spool worker's doorbell (apps/libprint/print.h,
 * cp_service_notify) for jobs that the Win32 file service publishes: the
 * Windows spooler starts a job when its producer schedules it
 * (AddJob/ScheduleJob), it does not poll. Native producers ring from
 * libprint and the DOS LPT capture from dosvm_print.c; Win32 programs
 * publish `job`, `ctl` and `printers` records by MoveFileA (gdi32
 * printing.c), so the Win32 MoveFile service rings after such a rename.
 * SIGCHLD to the PID in C:\WINDOWS\SPOOL\worker.lock\owner: its default
 * action is to ignore, so a record left by an earlier boot is harmless.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/process.h>
#include <ciuki/signal.h>
#include "path.h"

static bool upper_is(const char *s, const char *word, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (path_upper((uint8_t)s[i]) != (uint8_t)word[i]) return false;
    return true;
}
/* C:\WINDOWS\SPOOL\PRINTERS or C:\WINDOWS\SPOOL\Jnnn\JOB or ...\CTL. */
bool spool_publication(const char *full)
{
    static const char root[] = "C:\\WINDOWS\\SPOOL\\";
    size_t n = sizeof(root) - 1, length = strlen(full);
    if (length < n || !upper_is(full, root, n)) return false;
    const char *leaf = full + n;
    size_t rest = length - n;
    if (rest == 8 && upper_is(leaf, "PRINTERS", 8)) return true;
    if (rest != 8 || path_upper((uint8_t)leaf[0]) != 'J' || leaf[4] != '\\') return false;
    for (unsigned i = 1; i < 4; i++) if (leaf[i] < '0' || leaf[i] > '9') return false;
    return upper_is(leaf + 5, "JOB", 3) || upper_is(leaf + 5, "CTL", 3);
}
void spool_ring(struct vfs *vfs)
{
    struct vfs_table *t = kzalloc(sizeof(*t));
    if (!t) return;
    uint32_t pid = 0; size_t n = 0;
    if (!vfs_table_init(vfs, t, 2)) {
        int fd = vfs_open(t, "C:\\WINDOWS\\SPOOL\\worker.lock\\owner", VFS_READ, VFS_DENY_NONE, 0);
        if (fd >= 0) {
            if (vfs_read(t, fd, &pid, sizeof(pid), &n) || n != sizeof(pid)) pid = 0;
            (void)vfs_close(t, fd);
        }
        vfs_table_destroy(t);
    }
    kfree(t);
    struct process *p = pid ? proc_find(pid) : 0;
    if (p && p->state == PROC_LIVE) proc_signal_post(p, 0, SIGCHLD, 0);
}
