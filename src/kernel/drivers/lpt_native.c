/* Checked, cancellable /dev/lptN I/O. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/lpt.h>
#include <ciuki/files.h>
#include <ciuki/signal.h>
#include <ciuki/uaddr.h>

int lpt_device_open(struct file_description *d, uint32_t flags)
{
    if (flags&(O_CREAT|O_EXCL|O_TRUNC|O_DIRECTORY|O_APPEND)) return -EINVAL;
    return lpt_open(d->node->drive,0,0,false,&d->lpt_generation);
}
void lpt_device_close(struct file_description *d)
{
    if (d->node && d->node->kind==PX_LPT && d->lpt_generation) {
        lpt_close(d->node->drive,d->lpt_generation); d->lpt_generation=0;
    }
}
static bool lpt_interrupted(void *unused)
{
    (void)unused;
    struct proc_thread *t=proc_thread_for(g_current);
    return t && (t->process->state==PROC_STOPPING || proc_signal_caught(t));
}
struct lpt_operation {
    struct file_description *description;
    struct process *process;
    struct ua_pin pin;
    bool pinned, active;
    uint8_t copied[CIUKI_LPT_CHUNK];
};
static void lpt_operation_free(void *arg)
{
    struct lpt_operation *o=arg;
    if (o->active) lpt_abort(o->description->node->drive,o->description->lpt_generation);
    if (o->pinned) ua_unpin(o->process->memory,&o->pin);
    o->description->object.release(&o->description->object); kfree(o);
}
int lpt_rw_user(struct file_description *d, uint32_t address, uint32_t bytes, bool write)
{
    if (bytes>CIUKI_IO_MAX) return -EINVAL;
    if (write ? (d->flags&O_ACCMODE)==O_RDONLY : (d->flags&O_ACCMODE)==O_WRONLY) return -EBADF;
    if (!bytes) return 0;
    struct lpt_operation *o=kzalloc(sizeof(*o)); if (!o) return -ENOMEM;
    o->description=d; o->process=proc_current(); d->object.retain(&d->object);
    struct proc_thread *t=proc_thread_for(g_current);
    if (t) { t->operation=o; t->cleanup=lpt_operation_free; }
    unsigned n=bytes>sizeof(o->copied) ? sizeof(o->copied) : bytes;
    int e=ua_pin(o->process->memory,&o->pin,address,n,!write); o->pinned=!e;
    if (!e && write) e=copy_from_user(o->copied,address,n);
    int done=0;
    if (!e) {
        o->active=true;
        done=lpt_transfer(d->node->drive,d->lpt_generation,o->copied,n,write,
                          !!(d->flags&O_NONBLOCK),lpt_interrupted,0);
        o->active=false;
        if (done<0) e=done;
        else if (!write && done) e=copy_to_user(address,o->copied,(unsigned)done);
    }
    if (t) { t->operation=0; t->cleanup=0; }
    lpt_operation_free(o); return e ? e : done;
}
int lpt_devctl(struct process *p, int32_t fd, uint32_t op, uint32_t address, uint32_t bytes)
{
    struct file_description *d=file_fd(p,fd);
    if (!d || d->node->kind!=PX_LPT) return -EBADF;
    union { struct ciuki_lpt_status status; struct ciuki_lpt_config config; struct ciuki_lpt_init init; } record;
    unsigned size=op==LPT_GET_CONFIG || op==LPT_SET_CONFIG ? sizeof(record.config) :
        op==LPT_GET_STATUS ? sizeof(record.status) : op==LPT_INIT ? sizeof(record.init) : 0;
    if (!size) return -ENOTTY;
    if (bytes!=size) return -EINVAL;
    int e=copy_from_user(&record,address,bytes); if (e) return e;
    /* Configuration can sleep in negotiation. Install the same retained
     * operation cleanup used by data I/O so task death cannot strand busy. */
    struct lpt_operation *o=kzalloc(sizeof(*o)); if (!o) return -ENOMEM;
    o->description=d; o->process=p; d->object.retain(&d->object);
    struct proc_thread *t=proc_thread_for(g_current);
    if (t) { t->operation=o; t->cleanup=lpt_operation_free; }
    struct ua_pin pin;
    bool output=op==LPT_GET_CONFIG || op==LPT_GET_STATUS;
    if (output) { e=ua_pin(p->memory,&pin,address,bytes,true); if (!e) { o->pin=pin; o->pinned=true; } }
    if (!e) {
        o->active=true;
        e=lpt_control(d->node->drive,d->lpt_generation,op,&record,bytes,(d->flags&O_ACCMODE)!=O_RDONLY);
        o->active=false;
        if (!e && output) e=copy_to_user(address,&record,bytes);
    }
    if (t) { t->operation=0; t->cleanup=0; }
    lpt_operation_free(o); return e;
}
