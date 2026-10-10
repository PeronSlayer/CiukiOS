/* f2-02's pre-signal harness injects an interruption flag directly. Keep
 * that fixture's adapter while signal_test exercises the real eligibility.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/signal.h>
bool proc_signal_caught(struct proc_thread *t)
{
    return t && t->interrupted && !t->in_handler;
}
void proc_signal_child(struct process *p, uint32_t pid)
{
    (void)pid;
    p->pending |= CIUKI_SIGBIT(SIGCHLD);
}
