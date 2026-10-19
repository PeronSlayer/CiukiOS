/* SPDX-License-Identifier: MIT */
#include "win32_adapter.h"
void desk_w32_supervise(struct desk_w32_supervisor *s, const struct desk_w32_supervisor_ops *o, uint64_t now)
{
    if (s->stopping || s->pid || now<s->retry_ns) return;
    int pid=o->spawn(o->ctx);
    if (pid>0) {
        s->pid=(uint32_t)pid;
        if (s->starts++ && o->record) o->record(o->ctx,"[win32] w32srv restart",pid,now-s->death_ns);
    } else {
        s->retry_ns=now+UINT64_C(100000000); /* bounded retries, input remains live */
        if (o->record) o->record(o->ctx,"[win32] w32srv unavailable",pid,0);
    }
}
int desk_w32_service_exited(struct desk_w32_supervisor *s, const struct desk_w32_supervisor_ops *o,
                           uint32_t pid, int status, uint64_t now)
{
    (void)o; (void)status;
    if (!s->pid || pid!=s->pid) return 0;
    s->pid=0; s->retry_ns=now; s->death_ns=now;
    return 1;
}
