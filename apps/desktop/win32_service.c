/* SPDX-License-Identifier: MIT */
#include "win32_adapter.h"
#include <sys/wait.h>
/* A server that dies soon after its start is failing to start: the kernel
 * refused its registration (q3crash 2026-10-25: the dead desktop's orphaned
 * w32srv held it; 323 restarts in 35 s) or its store is unusable. An exit
 * with a nonzero code within DESK_W32_RAPID_NS, or more than three rapid
 * deaths of any kind, defers the next spawn 250 ms, 500 ms, 1 s, 2 s, 4 s,
 * then 5 s. A crash (signal) of a server that has been serving keeps the
 * immediate restart of f5-09b; a long life resets the count. */
#define DESK_W32_RAPID_NS UINT64_C(2000000000)
#define DESK_W32_BACKOFF_MIN_NS UINT64_C(250000000)
#define DESK_W32_BACKOFF_MAX_NS UINT64_C(5000000000)
void desk_w32_supervise(struct desk_w32_supervisor *s, const struct desk_w32_supervisor_ops *o, uint64_t now)
{
    if (s->stopping || s->pid || now<s->retry_ns) return;
    int pid=o->spawn(o->ctx);
    if (pid>0) {
        s->pid=(uint32_t)pid; s->start_ns=now;
        if (s->starts++ && o->record) o->record(o->ctx,"[win32] w32srv restart",pid,now-s->death_ns);
    } else {
        s->retry_ns=now+UINT64_C(100000000); /* bounded retries, input remains live */
        if (o->record) o->record(o->ctx,"[win32] w32srv unavailable",pid,0);
    }
}
int desk_w32_service_exited(struct desk_w32_supervisor *s, const struct desk_w32_supervisor_ops *o,
                           uint32_t pid, int status, uint64_t now)
{
    if (!s->pid || pid!=s->pid) return 0;
    s->pid=0; s->retry_ns=now; s->death_ns=now;
    int rapid=now-s->start_ns<DESK_W32_RAPID_NS, refused=WIFEXITED(status) && WEXITSTATUS(status);
    s->rapid_exits=rapid ? s->rapid_exits+1 : 0;
    if (rapid && (refused || s->rapid_exits>3)) {
        unsigned step=s->rapid_exits>6 ? 6 : s->rapid_exits;   /* 250 ms << 5 = 8 s, capped to 5 s */
        uint64_t delay=DESK_W32_BACKOFF_MIN_NS<<(step-1);
        if (delay>DESK_W32_BACKOFF_MAX_NS) delay=DESK_W32_BACKOFF_MAX_NS;
        s->retry_ns=now+delay;
        if (o && o->record) o->record(o->ctx,"[win32] w32srv restart deferred",(int)pid,delay);
    }
    return 1;
}
