/* Independent VFS/parallel-cable fixture for production DOS RAW capture.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/lpt.h>
#include <ciuki/process.h>
#include <ciuki/signal.h>
#include "../../src/kernel/vm/dosvm_internal.h"
#include "../../apps/libprint/print.h"
volatile uint64_t g_ticks;
static struct dosvm vm;
static unsigned directory, active, position, fail_write, short_write, fail_commit, fail_rename;
static bool spool_dir, config_present, backup_present, temp_present;
static uint32_t config[3]={CP_CONFIG_MAGIC,1,2};
static uint32_t backup_config[3]={CP_CONFIG_MAGIC,1,3},temp_config[3]={CP_CONFIG_MAGIC,1,4};
static size_t temp_bytes;
/* The spool worker's doorbell: worker.lock\owner names its PID. */
static uint32_t owner_pid;
static struct process worker;
static unsigned rings, ring_signal;
static struct { bool reserved, published; unsigned size; uint8_t data[8192]; struct cp_job job; } slots[CP_JOBS+1];
static unsigned file_slot[16], file_kind[16], file_at[16];
static bool opened_fd[16];
static uint8_t memory[4096];
static dosvm_port_handler ports[3];
int ksnprintf(char *b,size_t n,const char *f,...)
{ va_list a; va_start(a,f); int r=vsnprintf(b,n,f,a); va_end(a); return r; }
int px_error(int e) { return e; }
static unsigned path_slot(const char *p)
{ unsigned n=0; assert(sscanf(p,"C:\\WINDOWS\\SPOOL\\j%u",&n)==1 && n && n<=CP_JOBS); return n; }
int vfs_mkdir(struct vfs_table *t,const char *path,uint8_t attr)
{
    assert(t==&vm.disk_table && !attr);
    if (!strcmp(path,"C:\\WINDOWS\\SPOOL")) { if (spool_dir) return -FS_EEXIST; spool_dir=true; return 0; }
    directory=path_slot(path);
    if (slots[directory].reserved) return -FS_EEXIST;
    slots[directory].reserved=true; return 0;
}
int vfs_open(struct vfs_table *t,const char *path,unsigned flags,enum vfs_share share,uint8_t attr)
{
    assert(t==&vm.disk_table && !attr); (void)share;
    if (!strcmp(path,"C:\\WINDOWS\\SPOOL\\worker.lock\\owner")) {
        assert(flags==VFS_READ);
        if (!owner_pid) return -FS_ENOENT;
        for (unsigned fd=1;fd<16;fd++) if (!opened_fd[fd]) {
            opened_fd[fd]=true; file_at[fd]=0; file_slot[fd]=0; file_kind[fd]=5; active++; return (int)fd;
        }
        return -FS_ENOSPC;
    }
    unsigned cfg=!strcmp(path,"C:\\WINDOWS\\SPOOL\\printers") ? 2 :
        !strcmp(path,"C:\\WINDOWS\\SPOOL\\printers.bak") ? 3 :
        !strcmp(path,"C:\\WINDOWS\\SPOOL\\printers.tmp") ? 4 : 0;
    if ((cfg==2 && !config_present) || (cfg==3 && !backup_present) || (cfg==4 && !temp_present)) return -FS_ENOENT;
    for (unsigned fd=1;fd<16;fd++) if (!opened_fd[fd]) {
        opened_fd[fd]=true; file_at[fd]=0; file_slot[fd]=cfg ? 0 : path_slot(path);
        file_kind[fd]=cfg ? cfg : strstr(path,"job.tmp") ? 1 : 0;
        if (flags&VFS_TRUNCATE) memset(&slots[file_slot[fd]].job,0,sizeof(struct cp_job));
        active++; return (int)fd;
    }
    return -FS_ENOSPC;
}
int vfs_seek(struct vfs_table *t,int fd,int64_t at,enum vfs_whence whence,uint64_t *out)
{ assert(t==&vm.disk_table && opened_fd[fd] && whence==VFS_SEEK_SET && at>=0); file_at[fd]=(unsigned)at; *out=(uint64_t)at; return 0; }
int vfs_read(struct vfs_table *t,int fd,void *p,size_t n,size_t *done)
{
    assert(t==&vm.disk_table && opened_fd[fd] && file_kind[fd]>=2);
    if (file_kind[fd]==5) {
        assert(!file_at[fd] && n==sizeof(owner_pid));
        memcpy(p,&owner_pid,n); file_at[fd]+=(unsigned)n; *done=n; return 0;
    }
    size_t length=file_kind[fd]==4 ? temp_bytes : sizeof(struct cp_config);
    if (n>length-file_at[fd]) n=length-file_at[fd];
    memset(p,0,n);
    const uint32_t *header=file_kind[fd]==2 ? config : file_kind[fd]==3 ? backup_config : temp_config;
    if (file_at[fd]<sizeof(config)) {
        size_t take=sizeof(config)-file_at[fd]; if (take>n) take=n;
        memcpy(p,(const uint8_t *)header+file_at[fd],take);
    }
    file_at[fd]+=(unsigned)n; *done=n; return 0;
}
int vfs_write(struct vfs_table *t,int fd,const void *p,size_t n,size_t *done)
{
    assert(t==&vm.disk_table && opened_fd[fd]);
    *done=0;
    if (fail_write) { fail_write--; return -FS_EIO; }
    if (short_write) { short_write--; n=n>1 ? n/2 : 0; }
    unsigned id=file_slot[fd];
    if (file_kind[fd]==1) { assert(n<=sizeof(struct cp_job)); memcpy(&slots[id].job,p,n); }
    else { assert(file_at[fd]+n<=sizeof(slots[id].data)); memcpy(slots[id].data+file_at[fd],p,n); slots[id].size=file_at[fd]+(unsigned)n; }
    file_at[fd]+=(unsigned)n; *done=n; position+=(unsigned)n; return 0;
}
int vfs_commit(struct vfs_table *t,int fd)
{ assert(t==&vm.disk_table && opened_fd[fd]); if (fail_commit) { fail_commit--; return -FS_EIO; } return 0; }
int vfs_close(struct vfs_table *t,int fd)
{ assert(t==&vm.disk_table && opened_fd[fd] && active); opened_fd[fd]=false; active--; return 0; }
int vfs_rename(struct vfs_table *t,const char *from,const char *to)
{
    assert(t==&vm.disk_table && strstr(from,"job.tmp") && strstr(to,"\\job"));
    if (fail_rename) { fail_rename--; return -FS_EIO; }
    unsigned id=path_slot(from); assert(!slots[id].published && slots[id].job.bytes==slots[id].size);
    slots[id].published=true; return 0;
}
int dosvm_write(struct dosvm *v,uint32_t at,const void *p,unsigned n)
{ assert(v==&vm && at+n<=sizeof(memory)); memcpy(memory+at,p,n); return 0; }
int dosvm_read(struct dosvm *v,uint32_t at,void *p,unsigned n)
{ assert(v==&vm && at+n<=sizeof(memory)); memcpy(p,memory+at,n); return 0; }
int dosvm_port_register(struct dosvm_key k,uint16_t first,uint16_t last,dosvm_port_handler fn,void *arg)
{ (void)last; assert(k.id==1 && arg==&vm); ports[first==0x378 ? 0 : first==0x278 ? 1 : 2]=fn; return 0; }
bool lpt_snapshot(unsigned n,struct ciuki_lpt_status *s,const char **why)
{ (void)n; (void)s; (void)why; return false; }
int lpt_open(unsigned n,uint32_t id,uint32_t gen,bool raw,uint32_t *out)
{ (void)n; (void)id; (void)gen; (void)raw; (void)out; assert(!"capture requested hardware lease"); return -ENODEV; }
void lpt_close(unsigned n,uint32_t gen) { (void)n; (void)gen; assert(!"unexpected physical close"); }
int lpt_raw_io(unsigned n,uint32_t gen,unsigned reg,bool write,uint8_t *p)
{ (void)n; (void)gen; (void)reg; (void)write; (void)p; return -ENODEV; }
int lpt_transfer(unsigned n,uint32_t gen,void *p,unsigned bytes,bool write,bool nonblock,bool (*cancel)(void *),void *arg)
{ (void)n; (void)gen; (void)p; (void)bytes; (void)write; (void)nonblock; (void)cancel; (void)arg; return -ENODEV; }
int lpt_control(unsigned n,uint32_t gen,uint32_t op,void *p,unsigned bytes,bool write)
{ (void)n; (void)gen; (void)op; (void)p; (void)bytes; (void)write; return -ENODEV; }
struct process *proc_find(uint32_t pid) { return pid==worker.pid ? &worker : 0; }
void proc_signal_post(struct process *p,struct proc_thread *t,uint32_t signo,uint32_t sender)
{ assert(p==&worker && !t && !sender); rings++; ring_signal=signo; }
#include "../../src/kernel/vm/dosvm_print.c"
#include "../../src/kernel/vm/dosvm_lpt.c"
static void reset(void)
{
    assert(!active); memset(&vm,0,sizeof(vm)); memset(slots,0,sizeof(slots)); memset(opened_fd,0,sizeof(opened_fd));
    vm.info.key=(struct dosvm_key){1,10}; vm.info.state=DOSVM_RUNNABLE; vm.table_ready=true;
    g_ticks=position=fail_write=short_write=fail_commit=fail_rename=rings=ring_signal=owner_pid=0;
    worker=(struct process){0}; worker.pid=43; worker.state=PROC_LIVE;
    spool_dir=config_present=backup_present=temp_present=false; temp_bytes=sizeof(struct cp_config);
    assert(!dosvm_lpt_init(&vm));
}
static unsigned bios(unsigned fn,unsigned byte)
{ struct v86_frame f={0}; f.tf.eax=0xaabb0000|(fn<<8)|byte; f.tf.eflags=0x247; assert(!dosvm_lpt_bios(&vm,&f)); assert((f.tf.eax&0xffff00ff)==(0xaabb0000|byte) && f.tf.eflags==0x247); return (f.tf.eax>>8)&255; }
static uint32_t port(unsigned reg,bool write,uint32_t b,unsigned width)
{ assert(!ports[0](&vm,(uint16_t)(0x378+reg),width,write,&b,&vm)); return b; }
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);
    reset(); uint16_t b; memcpy(&b,memory+0x408,2); assert(b==0x378);
    assert(bios(0,'A')==0x90 && bios(0,12)==0x90); dosvm_print_service(&vm);
    assert(slots[1].published && slots[1].size==2 && !memcmp(slots[1].data,"A\f",2));
    assert(slots[1].job.magic==CP_JOB_MAGIC && !slots[1].job.offset && !slots[1].job.completed && slots[1].job.printer==0 && slots[1].job.page.width==612);
    config_present=true; assert(bios(0,'B')==0x90); g_ticks=4999; dosvm_print_service(&vm); assert(!slots[2].reserved);
    g_ticks=5000; dosvm_print_service(&vm); assert(slots[2].published && slots[2].job.printer==2 && !active);
    puts("PASS BIOS shared RAW queue, BDA/AL/FLAGS, FF, idle deadline and configured default printer");
    reset(); backup_present=true; assert(!dosvm_print_byte(&vm,'b') && !dosvm_print_flush(&vm,true));
    assert(slots[1].published && slots[1].job.printer==3 && !active);
    temp_present=true; assert(!dosvm_print_byte(&vm,'t') && !dosvm_print_flush(&vm,true));
    assert(slots[2].published && slots[2].job.printer==4 && !active);
    temp_bytes=sizeof(config); assert(!dosvm_print_byte(&vm,'s') && !dosvm_print_flush(&vm,true));
    assert(slots[3].published && slots[3].job.printer==3 && !active);
    backup_present=false; temp_bytes=sizeof(struct cp_config);
    assert(!dosvm_print_byte(&vm,'u') && !dosvm_print_flush(&vm,true));
    assert(slots[4].published && slots[4].job.printer==0 && !active);
    puts("PASS default config replacement: complete backup, complete proved temporary, partial temporary rejection, unproved temporary ignored");
    reset(); port(0,true,'C',1); assert(!vm.print_capture.used); port(2,true,13,1); port(2,true,13,1);
    assert(vm.print_capture.used==1 && vm.print_capture.buffer[0]=='C'); assert(port(1,false,0,1)==0x98 && port(1,false,0,1)==0xd8);
    assert(port(0,false,0,1)=='C' && port(2,false,0,1)==13);
    port(0,true,'D',2); assert(vm.print_capture.used==1);
    port(2,true,12,1); port(0,true,12,1); port(2,true,13,1); dosvm_print_service(&vm);
    assert(slots[1].published && !memcmp(slots[1].data,"C\f",2));
    puts("PASS SPP byte latch, single strobe, ACK, status/register reads and unsplit wide cycles");
    reset(); for (unsigned i=0;i<512;i++) assert(!dosvm_print_byte(&vm,'x'));
    assert(dosvm_print_byte(&vm,'y')==-EAGAIN && bios(2,0)==1);
    g_ticks=5000; short_write=1; assert(dosvm_print_flush(&vm,false)==-EIO && vm.print_capture.used==256 && vm.print_capture.bytes==256);
    unsigned before=position; dosvm_print_service(&vm); assert(position==before);
    assert(!dosvm_print_flush(&vm,false) && vm.print_capture.bytes==512 && !vm.print_capture.used);
    assert(!dosvm_print_byte(&vm,'y')); fail_commit=1;
    assert(dosvm_print_flush(&vm,true)==-FS_EIO && !slots[1].published && vm.print_capture.opened);
    fail_rename=1; assert(dosvm_print_flush(&vm,true)==-FS_EIO && !slots[1].published);
    assert(dosvm_print_byte(&vm,'z')==-EAGAIN && !dosvm_print_flush(&vm,true));
    assert(slots[1].published && slots[1].size==513 && slots[1].data[512]=='y' && !active);
    for (unsigned i=0;i<512;i++) assert(slots[1].data[i]=='x');
    puts("PASS backpressure, short-write continuation, commit/publication failure retain and exact retry");
    reset(); assert(!dosvm_print_byte(&vm,'Z')); vm.info.state=DOSVM_STOPPING;
    assert(dosvm_print_byte(&vm,'Q')==-ENODEV); fail_write=1; assert(dosvm_print_flush(&vm,true)==-FS_EIO);
    assert(vm.print_capture.used==1 && !slots[1].published && !dosvm_print_flush(&vm,true));
    assert(slots[1].published && slots[1].size==1 && slots[1].data[0]=='Z' && !active);
    reset(); assert(!dosvm_print_byte(&vm,'M') && !dosvm_print_flush(&vm,false));
    short_write=1; assert(dosvm_print_flush(&vm,true)==-EIO && !slots[1].published && !active);
    assert(!dosvm_print_flush(&vm,true) && slots[1].published && slots[1].job.bytes==1 && !active);
    reset(); assert(!dosvm_print_byte(&vm,'R')); g_ticks=5000; fail_write=1; dosvm_print_service(&vm);
    assert(vm.print_capture.failed && vm.print_capture.used==1 && !slots[1].published);
    g_ticks=5999; dosvm_print_service(&vm); assert(!slots[1].published && vm.print_capture.used==1);
    g_ticks=6000; dosvm_print_service(&vm); assert(slots[1].published && slots[1].data[0]=='R' && !active);
    reset(); for (unsigned i=1;i<=CP_JOBS;i++) slots[i].reserved=true;
    assert(!dosvm_print_byte(&vm,'S') && dosvm_print_flush(&vm,true)==-ENOSPC && vm.print_capture.used==1);
    slots[7].reserved=false; assert(!dosvm_print_flush(&vm,true) && slots[7].published && !active);
    puts("PASS stopping VM retains last byte, recoverable queue-full, no hardware lease or handle leaks");
    /* Doorbell: every publication rings the worker once (SIGCHLD); no worker,
     * a dead owner or a failed publication rings nothing. */
    reset(); assert(!dosvm_print_byte(&vm,'1') && !dosvm_print_flush(&vm,true) && slots[1].published && !rings && !active);
    owner_pid=43; assert(!dosvm_print_byte(&vm,'2') && !dosvm_print_flush(&vm,true) && slots[2].published);
    assert(rings==1 && ring_signal==SIGCHLD && !active);
    assert(!dosvm_print_byte(&vm,'3')); fail_rename=1; assert(dosvm_print_flush(&vm,true)==-FS_EIO && rings==1);
    assert(!dosvm_print_flush(&vm,true) && slots[3].published && rings==2);
    worker.state=PROC_STOPPING; assert(!dosvm_print_byte(&vm,'4') && !dosvm_print_flush(&vm,true) && slots[4].published && rings==2);
    owner_pid=44; worker.state=PROC_LIVE; assert(!dosvm_print_byte(&vm,'5') && !dosvm_print_flush(&vm,true) && rings==2 && !active);
    puts("PASS published DOS job rings the spool worker once; no owner, dead or unknown owner and failed publication ring nothing");
    return 0;
}
