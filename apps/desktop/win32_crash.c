/* Eight bounded text files; no dump or user buffers. SPDX-License-Identifier: MIT
 * sec-sysvc: a crash report belongs to the user whose program faulted
 * (contract A1-10, "private persistent crash reports remain bound to their
 * owner"): the desktop writes it through the SYSTEM log service into the
 * user's own logs, CRASHn.TXT for native programs and WCRASHn.TXT for Win32
 * ones (desk_w32_crash_log). desk_w32_crash_store keeps the directory form for
 * an older kernel and the host fixtures. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include "win32_crash.h"
#include "fault_text.h"
#if defined(__CIUKIOS__) && !defined(__linux__)
#include <ciuki/recovery.h>
#endif

/* Fixed format and bounded names keep the entire text below 2 KiB. */
static int crash_text(const struct ciuki_w32_fault *f,unsigned long long sequence,char *out,size_t cap)
{
    int n;
    if(f->kind==W32_REPORT_NATIVE_FAULT){
        /* G13: native program; same first line, so rotation and the log viewer treat both alike. */
        char text[640];
        if(desk_native_fault_text(f,text,sizeof(text))<0)return -EIO;
        n=snprintf(out,cap,"CiukiOS crash sequence=%llu\nExecutable: %.63s\nPID: %u TID: %u\nNative fault\n%s\n",sequence,f->exe,f->pid,f->tid,text);
        return n<0||(size_t)n>=cap?-EIO:n;
    }
    n=snprintf(out,cap,"CiukiOS crash sequence=%llu\nExecutable: %.63s\nPID: %u TID: %u\nException: %08x\nModule: %.63s + %08x\nEIP: %08x\n"
        "EAX=%08x EBX=%08x ECX=%08x EDX=%08x\nESI=%08x EDI=%08x EBP=%08x ESP=%08x EFLAGS=%08x\nStack:",
        sequence,f->exe,f->pid,f->tid,f->code,f->module,f->offset,f->eip,f->registers[0],f->registers[1],f->registers[2],f->registers[3],f->registers[4],f->registers[5],f->registers[6],f->registers[7],f->registers[8]);
    if(n<0||(size_t)n>=cap)return -EIO;
    for(unsigned i=0;i<8;i++){
        int k=f->valid&(1u<<i)?snprintf(out+n,cap-(size_t)n," %08x",f->stack[i]):snprintf(out+n,cap-(size_t)n," ????????");
        if(k<0||(size_t)k>=cap-(size_t)n)return -EIO;
        n+=k;
    }
    if((size_t)n+1>=cap)return -EIO;
    out[n++]='\n'; out[n]=0;
    return n;
}
int desk_w32_crash_write(const struct desk_crash_io *io,const char *prefix,const struct ciuki_w32_fault *f)
{
    if(!io||!prefix||!f||(f->kind!=W32_REPORT_CRASH&&f->kind!=W32_REPORT_NATIVE_FAULT))return -EINVAL;
    unsigned slot=0;unsigned long long oldest=~0ull,maximum=0;
    char name[32];
    for(unsigned i=0;i<8;i++){
        snprintf(name,sizeof(name),"%s%u.TXT",prefix,i);
        char head[64];size_t got=0;unsigned long long sequence=0;
        if(!io->head(io->ctx,name,head,sizeof(head)-1,&got)){head[got]=0;if(sscanf(head,"CiukiOS crash sequence=%llu",&sequence)!=1)sequence=0;}
        if(sequence<oldest){oldest=sequence;slot=i;}if(sequence>maximum)maximum=sequence;
    }
    char text[2048];
    int n=crash_text(f,maximum+1,text,sizeof(text));
    if(n<0)return n;
    snprintf(name,sizeof(name),"%s%u.TXT",prefix,slot);
    return io->replace(io->ctx,name,text,(size_t)n);
}

/* ---- the directory form ---- */
static int dir_head(void *ctx,const char *name,char *out,size_t cap,size_t *got)
{
    char path[300];int n=snprintf(path,sizeof(path),"%s/%s",(const char *)ctx,name);
    if(n<0||n>=(int)sizeof(path))return -ENAMETOOLONG;
    FILE *in=fopen(path,"rb");if(!in)return -errno;
    *got=fread(out,1,cap,in);fclose(in);return 0;
}
static int dir_replace(void *ctx,const char *name,const char *text,size_t n)
{
    char path[300];int k=snprintf(path,sizeof(path),"%s/%s",(const char *)ctx,name);
    if(k<0||k>=(int)sizeof(path))return -ENAMETOOLONG;
    FILE *out=fopen(path,"wb");if(!out)return -errno;
    int error=0;
    if(fwrite(text,1,n,out)!=n)error=EIO;
    if(fflush(out)||fsync(fileno(out)))error=EIO;
    if(fclose(out))error=EIO;
    return -error;
}
int desk_w32_crash_store_as(const char *directory,const char *prefix,const struct ciuki_w32_fault *f)
{
    if(!directory||!prefix||!f||(f->kind!=W32_REPORT_CRASH&&f->kind!=W32_REPORT_NATIVE_FAULT))return -EINVAL;
    char path[300];int n=snprintf(path,sizeof(path),"%s/%s0.TXT",directory,prefix);if(n<0||n>=(int)sizeof(path))return -ENAMETOOLONG;
    if(mkdir(directory,0777)&&errno!=EEXIST)return -errno;
    struct desk_crash_io io={(void *)directory,dir_head,dir_replace};
    return desk_w32_crash_write(&io,prefix,f);
}
int desk_w32_crash_store(const char *directory,const struct ciuki_w32_fault *f)
{ return desk_w32_crash_store_as(directory,"CRASH",f); }

/* ---- the user's own logs (sec-sysvc) ---- */
#if defined(__CIUKIOS__) && !defined(__linux__)
static int log_head(void *ctx,const char *name,char *out,size_t cap,size_t *got)
{
    (void)ctx;uint32_t total=0;
    return ciuki_system_log_read(CIUKI_SYSTEM_LOG_USER,name,0,out,cap,got,&total)?-errno:0;
}
static int log_replace(void *ctx,const char *name,const char *text,size_t n)
{ (void)ctx; return ciuki_system_log_write(CIUKI_SYSTEM_LOG_USER,name,1,text,n,0)?-errno:0; }
int desk_w32_crash_log(const struct ciuki_w32_fault *f)
{
    struct desk_crash_io io={NULL,log_head,log_replace};
    return desk_w32_crash_write(&io,f&&f->kind==W32_REPORT_NATIVE_FAULT?"CRASH":"WCRASH",f);
}
#else
int desk_w32_crash_log(const struct ciuki_w32_fault *f) { (void)f; return -ENOSYS; }
#endif
