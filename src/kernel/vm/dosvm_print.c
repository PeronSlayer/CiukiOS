/* DOS RAW capture into the common native spool queue.
 * Contract and research: docs/design/printing-dos.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/process.h>
#include <ciuki/signal.h>
#include "dosvm_internal.h"
/* Private on-disk protocol shared with the native spool producer. */
#include "../../../apps/libprint/print.h"

_Static_assert(sizeof(struct cp_job)==424,"native spool record");
static void print_path(char *out,unsigned size,unsigned slot,const char *leaf)
{ ksnprintf(out,size,"C:\\WINDOWS\\SPOOL\\j%03u%s",slot,leaf); }
static int print_open(struct dosvm *v)
{
    struct dosvm_print_capture *p=&v->print_capture;
    int e=vfs_mkdir(&v->disk_table,"C:\\WINDOWS\\SPOOL",0);
    if (e && e!=-FS_EEXIST) return px_error(e);
    char path[64];
    if (!p->slot) {
        for (unsigned id=1;id<=CP_JOBS;id++) {
            print_path(path,sizeof(path),id,"");
            e=vfs_mkdir(&v->disk_table,path,0);
            if (!e) { p->slot=id; break; }
            if (e!=-FS_EEXIST) return px_error(e);
        }
        if (!p->slot) return -ENOSPC;
    }
    print_path(path,sizeof(path),p->slot,"\\data");
    int fd=vfs_open(&v->disk_table,path,VFS_WRITE|VFS_CREATE,VFS_DENY_WRITE,0);
    if (fd<0) return px_error(fd);
    uint64_t at;
    e=vfs_seek(&v->disk_table,fd,p->bytes,VFS_SEEK_SET,&at);
    if (e) { vfs_close(&v->disk_table,fd); return px_error(e); }
    p->fd=fd; p->opened=true; return 0;
}
/* Require a complete fixed record before using its header; an in-progress
 * temporary file may already have a valid header while its body is short. */
static int print_config_header(struct dosvm *v,const char *path,uint32_t header[3])
{
    int fd=vfs_open(&v->disk_table,path,VFS_READ,VFS_DENY_NONE,0);
    if (fd<0) return px_error(fd);
    uint8_t buffer[64]; size_t at=0; int e=0;
    while (at<sizeof(struct cp_config)) {
        size_t want=sizeof(struct cp_config)-at,n=0;
        if (want>sizeof(buffer)) want=sizeof(buffer);
        e=vfs_read(&v->disk_table,fd,buffer,want,&n);
        if (e) break;
        if (!n || n>want) { e=-FS_EIO; break; }
        if (at<3*sizeof(uint32_t)) {
            size_t take=3*sizeof(uint32_t)-at;
            if (take>n) take=n;
            memcpy((uint8_t *)header+at,buffer,take);
        }
        at+=n;
    }
    int closed=vfs_close(&v->disk_table,fd);
    return px_error(e ? e : closed);
}
static int print_default_printer(struct dosvm *v,uint32_t *printer)
{
    uint32_t header[3];
    int e=print_config_header(v,"C:\\WINDOWS\\SPOOL\\printers",header);
    if (e==-FS_ENOENT) {
        e=print_config_header(v,"C:\\WINDOWS\\SPOOL\\printers.bak",header);
        if (e==-FS_ENOENT) { *printer=0; return 0; }
        if (e) return e;
        /* A complete backup proves replacement of an existing config.
         * Only then may a complete temporary config supersede it. */
        uint32_t replacement[3];
        if (!print_config_header(v,"C:\\WINDOWS\\SPOOL\\printers.tmp",replacement))
            memcpy(header,replacement,sizeof(header));
    } else if (e) return e;
    if (header[0]!=CP_CONFIG_MAGIC || header[1]!=1 || header[2]>=CP_PRINTERS)
        return -EINVAL;
    *printer=header[2]; return 0;
}
static int print_publish(struct dosvm *v)
{
    struct dosvm_print_capture *p=&v->print_capture;
    struct cp_job job={.magic=CP_JOB_MAGIC,.version=1,.id=p->slot,
        .bytes=p->bytes,.page={612,792,0,36,36,36,36,1,0,0}};
    int e=print_default_printer(v,&job.printer);
    if (e) return e;
    size_t n=0;
    ksnprintf(job.title,sizeof(job.title),"DOS LPT1 VM %u",v->info.key.id);
    char temp[64],path[64];
    print_path(temp,sizeof(temp),p->slot,"\\job.tmp");
    print_path(path,sizeof(path),p->slot,"\\job");
    int fd=vfs_open(&v->disk_table,temp,VFS_WRITE|VFS_CREATE|VFS_TRUNCATE,VFS_DENY_WRITE,0);
    if (fd<0) return px_error(fd);
    e=vfs_write(&v->disk_table,fd,&job,sizeof(job),&n);
    if (!e && n!=sizeof(job)) e=-FS_EIO;
    if (!e) e=vfs_commit(&v->disk_table,fd);
    int closed=vfs_close(&v->disk_table,fd);
    if (!e) e=closed;
    if (!e) e=vfs_rename(&v->disk_table,temp,path);
    if (e) return px_error(e);
    p->slot=p->bytes=0; p->finish=p->failed=false;
    return 0;
}
/* The spool worker's doorbell (apps/libprint/print.h, cp_service_notify):
 * Windows 98's print spooler VxD hands captured DOS output to the 32-bit
 * spooler; here the published job rings the worker, which otherwise sleeps.
 * SIGCHLD to the PID in worker.lock\owner; its default action is to
 * ignore, so a record left by an earlier boot cannot harm another process. */
static void print_ring(struct dosvm *v)
{
    uint32_t pid=0; size_t n=0;
    int fd=vfs_open(&v->disk_table,"C:\\WINDOWS\\SPOOL\\worker.lock\\owner",VFS_READ,VFS_DENY_NONE,0);
    if (fd<0) return;
    int e=vfs_read(&v->disk_table,fd,&pid,sizeof(pid),&n);
    (void)vfs_close(&v->disk_table,fd);
    if (e || n!=sizeof(pid) || !pid) return;
    struct process *p=proc_find(pid);
    if (p && p->state==PROC_LIVE) proc_signal_post(p,0,SIGCHLD,0);
}
int dosvm_print_byte(struct dosvm *v,uint8_t byte)
{
    struct dosvm_print_capture *p=&v->print_capture;
    if (!p->enabled || v->info.state>=DOSVM_STOPPING) return -ENODEV;
    if (p->busy || p->finish || p->failed || p->used==sizeof(p->buffer)) return -EAGAIN;
    p->buffer[p->used++]=byte; p->last_byte=g_ticks;
    if (byte==12 || p->bytes+p->used==CP_MAX_BYTES) p->finish=true;
    return 0;
}
int dosvm_print_flush(struct dosvm *v,bool final)
{
    struct dosvm_print_capture *p=&v->print_capture;
    if (!p->enabled || (!p->used && !p->slot)) return 0;
    if (!v->table_ready) return -ENODEV;
    if (p->busy) return -EBUSY;
    p->busy=true;
    if (final) p->finish=true;
    int e=0;
    if (!p->opened && p->used) e=print_open(v);
    if (!e && p->used) {
        size_t n=0;
        e=vfs_write(&v->disk_table,p->fd,p->buffer,p->used,&n);
        if (n>p->used) n=0; /* a broken backend must not overrun capture state */
        p->bytes+=(uint32_t)n; p->used-=(unsigned)n;
        memmove(p->buffer,p->buffer+n,p->used);
        if (e) e=px_error(e);
        else if (p->used) e=-EIO;
    }
    if (!e && p->finish) {
        if (p->opened) {
            e=px_error(vfs_commit(&v->disk_table,p->fd));
            if (!e) {
                int closed=vfs_close(&v->disk_table,p->fd);
                /* VFS consumes the handle even if its final flush fails. */
                p->opened=false;
                e=px_error(closed);
            }
        }
        if (!e) e=print_publish(v);
        if (!e) print_ring(v);
    }
    p->failed=e!=0;
    if (e) p->retry_tick=g_ticks;
    p->busy=false;
    return e;
}
void dosvm_print_service(struct dosvm *v)
{
    struct dosvm_print_capture *p=&v->print_capture;
    if (p->failed && g_ticks-p->retry_tick<1000) return;
    bool aged=g_ticks-p->last_byte>=5000;
    if (p->finish || p->used==sizeof(p->buffer) ||
        ((p->used || p->slot) && aged))
        (void)dosvm_print_flush(v,aged);
}
