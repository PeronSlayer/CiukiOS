/* f2-02's pre-signal harness injects an interruption flag directly. Keep
 * that fixture's adapter while signal_test exercises the real eligibility.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/signal.h>
bool proc_signal_caught(struct proc_thread *t)
{
    return t && t->interrupted && !t->in_handler;
}
enum signal_wait_decision proc_signal_wait(struct proc_thread *t, enum signal_wait_class kind,
                                          bool committed, bool issued, uint32_t progress)
{
    /* Old fixture only reaches the pre-issue read/sleep cancellation points.
     * signal_test uses the production helper for the complete decision table. */
    if (kind != SIGNAL_WAIT_I || committed || issued || progress) __builtin_trap();
    return proc_signal_caught(t) ? SIGNAL_WAIT_EINTR : SIGNAL_WAIT_CONTINUE;
}
void proc_signal_child(struct process *p, uint32_t pid)
{
    (void)pid;
    p->pending |= CIUKI_SIGBIT(SIGCHLD);
}
