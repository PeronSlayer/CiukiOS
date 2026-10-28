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
#include <ciuki/desktop.h>
#include <ciuki/files.h>
#include <ciuki/process.h>
#include <ciuki/signal.h>
#include "path.h"
#include <ciuki/security.h> /* sec-user-05 */

static bool upper_is(const char *s, const char *word, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (path_upper((uint8_t)s[i]) != (uint8_t)word[i]) return false;
    return true;
}
/* sec-user-05: two queue roots. The machine queue C:\WINDOWS\SPOOL\ and a
 * profile session's own queue C:\WINDOWS\PROFILES\<root>\SETTINGS\SPOOL\
 * (apps/libprint/print.c); returns the root's length in `full`, 0 if none. */
static size_t spool_queue(const char *full)
{
    static const char machine[] = "C:\\WINDOWS\\SPOOL\\", profiles[] = "C:\\WINDOWS\\PROFILES\\",
                      queue[] = "\\SETTINGS\\SPOOL\\";
    size_t length = strlen(full);
    if (length >= sizeof(machine) - 1 && upper_is(full, machine, sizeof(machine) - 1)) return sizeof(machine) - 1;
    if (length < sizeof(profiles) - 1 || !upper_is(full, profiles, sizeof(profiles) - 1)) return 0;
    size_t at = sizeof(profiles) - 1, start = at;
    while (at < length && full[at] != '\\') at++;
    if (at == start || at - start > 63 || length - at < sizeof(queue) - 1 ||
        !upper_is(full + at, queue, sizeof(queue) - 1)) return 0;
    return at + sizeof(queue) - 1;
}
/* <queue>PRINTERS or <queue>Jnnn\JOB or ...\CTL. */
bool spool_publication(const char *full)
{
    size_t n = spool_queue(full), length = strlen(full);
    if (!n) return false;
    const char *leaf = full + n;
    size_t rest = length - n;
    if (rest == 8 && upper_is(leaf, "PRINTERS", 8)) return true;
    if (rest != 8 || path_upper((uint8_t)leaf[0]) != 'J' || leaf[4] != '\\') return false;
    for (unsigned i = 1; i < 4; i++) if (leaf[i] < '0' || leaf[i] > '9') return false;
    return upper_is(leaf + 5, "JOB", 3) || upper_is(leaf + 5, "CTL", 3);
}
#pragma weak security_credential
/* Rings the worker recorded in that queue's worker.lock\owner. A profile
 * queue's record is user data: only a worker with the publisher's uid and
 * session is signalled. */
void spool_ring_from(struct vfs *vfs, const char *full, struct process *publisher)
{
    size_t n = spool_queue(full);
    if (!n || n > 120) return;
    char owner[160];
    memcpy(owner, full, n);
    memcpy(owner + n, "worker.lock\\owner", sizeof("worker.lock\\owner"));
    struct vfs_table *t = desktop_buffer_alloc(sizeof(*t),MM_IO_REQUEST);
    if (!t) return;
    uint32_t pid = 0; size_t got = 0;
    if (!vfs_table_init(vfs, t, 2)) {
        int fd = vfs_open(t, owner, VFS_READ, VFS_DENY_NONE, 0);
        if (fd >= 0) {
            if (vfs_read(t, fd, &pid, sizeof(pid), &got) || got != sizeof(pid)) pid = 0;
            (void)vfs_close(t, fd);
        }
        vfs_table_destroy(t);
    }
    desktop_buffer_free(t,sizeof(*t),MM_IO_REQUEST);
    struct process *p = pid ? proc_find(pid) : 0;
    if (!p || p->state != PROC_LIVE) return;
    if (n != sizeof("C:\\WINDOWS\\SPOOL\\") - 1) {
        const struct ciuki_credential *a = security_credential && publisher ? security_credential(publisher) : 0,
                                      *b = security_credential ? security_credential(p) : 0;
        if (!a || !b || a->uid != b->uid || a->session_id != b->session_id) return;
    }
    proc_signal_post(p, 0, SIGCHLD, 0);
}
void spool_ring(struct vfs *vfs) { spool_ring_from(vfs, "C:\\WINDOWS\\SPOOL\\PRINTERS", 0); }
