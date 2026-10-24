/* Desktop requests of the system tools (protocol v2, `sysapp` clients). Used by the program
 * mains only; the view code never touches the channel. SPDX-License-Identifier: MIT */
#ifndef CIUKI_SYSREQ_H
#define CIUKI_SYSREQ_H
#include "../libdeskui/deskui.h"
#include <errno.h>
#include <string.h>
/* The desktop's client table size (DESK_CLIENTS in apps/desktop/desktop.h, which clients do not include). */
#define SYSREQ_CLIENTS 16u
/* DESK_LAUNCH by catalogue name without argument. Returns the serial (>0) or -errno. */
static inline int sysreq_launch(struct du_window *w, uint32_t *serial, const char *program)
{
    struct ciuki_message m; unsigned n = (unsigned)strlen(program);
    if (!n || n > DESK_TITLE_MAX) return -EINVAL;
    desk_message(&m, DESK_LAUNCH, 12 + n);
    *serial = *serial + 1 ? *serial + 1 : 1;
    desk_put32(m.data + 4, *serial); desk_put32(m.data + 8, n); memcpy(m.data + 12, program, n);
    return du_window_queue(w, &m) ? -EAGAIN : (int)*serial;
}
/* Existing v2 LAUNCH envelope, authenticated by the desktop as schedtask's
 * explicit Run request. No message number or wire layout is added. */
static inline int sysreq_task_message(struct ciuki_message *m, uint32_t serial, const char *task)
{
    size_t n = task ? strlen(task) : 0;
    if (!m || !serial || !n || n >= 32) return -EINVAL;
    for (size_t i = 0; i < n; i++) if ((unsigned char)task[i] < 32 || task[i] == 127) return -EINVAL;
    desk_message(m, DESK_LAUNCH, 29 + (unsigned)n); m->data[0] = DESK_SHELL_VERSION;
    desk_put32(m->data + 4, serial); desk_put32(m->data + 8, 9); memcpy(m->data + 12, "schedtask", 9);
    desk_put32(m->data + 21, (uint32_t)(4 + n)); memcpy(m->data + 25, "run:", 4); memcpy(m->data + 29, task, n);
    return 0;
}
/* DESK_WINDOWS_QUERY. Returns the serial (>0) or -errno. */
static inline int sysreq_windows(struct du_window *w, uint32_t *serial)
{
    struct ciuki_message m;
    desk_message(&m, DESK_WINDOWS_QUERY, 8);
    *serial = *serial + 1 ? *serial + 1 : 1;
    desk_put32(m.data + 4, *serial);
    return du_window_queue(w, &m) ? -EAGAIN : (int)*serial;
}
#endif
