/* Checked native COM character I/O; no namespace lock across device waits.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/security_device.h>
#include <ciuki/com.h>
#include <ciuki/files.h>
#include <ciuki/signal.h>
#include <ciuki/uaddr.h>

int com_device_open(struct file_description *d, uint32_t flags)
{
    if (flags&(O_CREAT|O_EXCL|O_TRUNC|O_DIRECTORY|O_APPEND)) return -EINVAL;
    struct process *owner=proc_current();
    return com_lease_open(d->node->drive,CIUKI_COM_OWNER_NATIVE,owner ? owner->pid : 0,&d->com_generation,0);
}
void com_device_close(struct file_description *d)
{
    if (d->node && d->node->kind==PX_COM && d->com_generation) {
        com_close(d->node->drive,d->com_generation); d->com_generation=0;
    }
}
static bool interrupted(void)
{
    struct proc_thread *t=proc_thread_for(g_current);
    return t && (t->process->state==PROC_STOPPING || proc_signal_caught(t));
}
struct com_operation {
    struct file_description *description;
    struct process *process;
    struct ua_pin pin;
    bool pinned;
    uint8_t copied[CIUKI_COM_QUEUE];
};
static void operation_free(void *arg)
{
    struct com_operation *o=arg;
    if (o->pinned) ua_unpin(o->process->memory,&o->pin);
    o->description->object.release(&o->description->object); kfree(o);
}
int com_rw_user(struct file_description *d, uint32_t address, uint32_t bytes, bool write)
{
    if (bytes>CIUKI_IO_MAX) return -EINVAL;
    if (write ? (d->flags&O_ACCMODE)==O_RDONLY : (d->flags&O_ACCMODE)==O_WRONLY) return -EBADF;
    if (!security_device_session(proc_current()) && !security_device_system(proc_current())) return -EACCES;
    if (!bytes) return 0;
    struct com_operation *o=kzalloc(sizeof(*o)); if (!o) return -ENOMEM;
    o->description=d; o->process=proc_current(); d->object.retain(&d->object);
    struct proc_thread *t=proc_thread_for(g_current);
    if (t) { t->operation=o; t->cleanup=operation_free; }
    int e=ua_pin(o->process->memory,&o->pin,address,bytes,!write); o->pinned=!e;
    unsigned count=bytes>sizeof(o->copied) ? sizeof(o->copied) : bytes;
    if (!e && write) e=copy_from_user(o->copied,address,count);
    struct ciuki_com_line line={.version=1,.size=sizeof(line)};
    if (!e) e=com_control(d->node->drive,d->com_generation,COM_GET_LINE,&line,sizeof(line),false);
    int n=0;
    if (!e && interrupted()) e=-EINTR;
    if (!e) {
        n=com_transfer(d->node->drive,d->com_generation,o->copied,count,write);
        if (n==-EAGAIN && !(d->flags&O_NONBLOCK)) {
            e=com_wait(d->node->drive,d->com_generation,write,
                write ? line.write_timeout_ms : line.read_timeout_ms,interrupted);
            if (!e) n=com_transfer(d->node->drive,d->com_generation,o->copied,count,write);
        }
        if (!e && n<0) e=n;
        if (!e && !write && n>0) e=copy_to_user(address,o->copied,(unsigned)n);
    }
    if (t) { t->operation=0; t->cleanup=0; }
    operation_free(o); return e ? e : n;
}
int com_devctl(struct process *p, int32_t fd, uint32_t op, uint32_t address, uint32_t bytes)
{
    struct file_description *d=file_fd(p,fd);
    if (!d || d->node->kind!=PX_COM) return -EBADF;
    if (!security_device_session(p) && !security_device_system(p)) return -EACCES;
    if (op==COM_SET_MOUSE && !security_device_admin(p)) return -EPERM;
    union { struct ciuki_com_status status; struct ciuki_com_line line; struct ciuki_com_modem modem;
        struct ciuki_com_owner owner; struct ciuki_com_mouse mouse; } record;
    unsigned size=op==COM_GET_LINE || op==COM_SET_LINE ? sizeof(record.line) :
        op==COM_GET_STATUS ? sizeof(record.status) : op==COM_SET_MODEM ? sizeof(record.modem) :
        op==COM_GET_OWNER ? sizeof(record.owner) : op==COM_SET_MOUSE ? sizeof(record.mouse) : 0;
    if (!size) return -ENOTTY;
    if (bytes!=size) return -EINVAL;
    int e=copy_from_user(&record,address,bytes); if (e) return e;
    if (!security_process_live(p) || (op==COM_SET_MOUSE && !security_device_admin(p))) return -EPERM;
    e=com_control(d->node->drive,d->com_generation,op,&record,bytes,(d->flags&O_ACCMODE)!=O_RDONLY);
    if (!e && (op==COM_GET_LINE || op==COM_GET_STATUS || op==COM_GET_OWNER)) e=copy_to_user(address,&record,bytes);
    return e;
}
