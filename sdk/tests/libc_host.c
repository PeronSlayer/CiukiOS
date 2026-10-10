/* Native x86-64 reference of pinned newlib/SDK and unchanged smoke main.
 * Only the syscall boundary is modeled; this is host evidence, not guest evidence.
 * SPDX-License-Identifier: MIT */
#include <ciuki/runtime.h>
#include <ciuki/raw.h>
#include <string.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <unistd.h>

static uint32_t linux_call(uint32_t nr,uint32_t b,uint32_t c,uint32_t d) {
    unsigned long r;__asm__ volatile("syscall":"=a"(r):"a"(nr),"D"((unsigned long)b),"S"((unsigned long)c),"d"((unsigned long)d):"memory","rcx","r11");return r;
}
static void host_exit(unsigned status) __attribute__((noreturn));
static void host_exit(unsigned status) { linux_call(60,status,0,0);__builtin_trap(); }
static void output(const void *b,unsigned n) { if(linux_call(1,1,CU_PTR(b),n)!=n)host_exit(125); }
static union { struct ciuki_tcb tcb;unsigned char page[CIUKI_TLS_SIZE]; } tcbs[3];
static unsigned current;
static int fail_uname,read_only;
static void select_tcb(unsigned n) { current=n; }
struct ciuki_tcb *__ciuki_host_tcb(void) { return &tcbs[current].tcb; }
struct _reent *__wrap___getreent(void) { return (void *)(uintptr_t)tcbs[current].tcb.reent; }
void __ciuki_sigreturn(void) { __builtin_trap(); }
void __real___ciuki_start(uint32_t *);
int ciuki_libc_host_tests(uintptr_t *stack_pointer) {
    const char *mode=stack_pointer[0]>1?(const char *)stack_pointer[2]:"";
    read_only=!strcmp(mode,"original");fail_uname=read_only||!strcmp(mode,"fail-uname");
    tcbs[0].tcb=(struct ciuki_tcb){.self=CU_PTR(&tcbs[0].tcb),.tid=1};select_tcb(0);
    static char arg[]="libc_smoke";
    struct __attribute__((packed,aligned(4))) { uint32_t argc;char *argv[2];char *env[1];uint32_t aux[4]; } stack={1,{arg,NULL},{NULL},{AT_CIUKI_TLS,CU_PTR(&tcbs[0].tcb),AT_NULL,0}};
    __real___ciuki_start(&stack.argc);host_exit(123);
}
uint32_t __wrap_ciuki_raw_probe_report(uint32_t b,uint32_t n,uint32_t d,uint32_t s,uint32_t i,uint32_t p) {
    (void)d;(void)s;(void)i;(void)p;if(!n||n>CIUKI_PROBE_REPORT_MAX)return -EINVAL;
    for(unsigned j=0;j<n;++j)if(((char *)(uintptr_t)b)[j]<32||((char *)(uintptr_t)b)[j]>126)return -EINVAL;
    output((void *)(uintptr_t)b,n);output("\n",1);return 0;
}
static unsigned char heap[1024*1024] __attribute__((aligned(4096)));
static uint32_t heap_end;
struct host_file { char name[128];unsigned char data[8192];unsigned size; };
struct host_fd { int used,file;uint64_t position;unsigned flags,cookie; };
static struct host_file files[8];
static struct host_fd fds[128]={{1,-1,0,O_RDONLY,0},{1,-2,0,O_WRONLY,0},{1,-2,0,O_WRONLY,0}};
static int fd_slot(void) { for(int f=0;f<128;++f)if(!fds[f].used)return f;return -EMFILE; }
static uint64_t mono,cpu;
static struct ciuki_thread_args worker;
static uint32_t worker_tid,worker_value;
static int worker_pending,signalled;
static jmp_buf worker_exit;
static void run_worker(void) {
    if(!worker_pending)return;
    worker_pending=0;unsigned parent=current;
    tcbs[1].tcb=(struct ciuki_tcb){.self=CU_PTR(&tcbs[1].tcb),.tid=worker_tid};select_tcb(1);
    if(!setjmp(worker_exit))((void (*)(void *))(uintptr_t)worker.entry)((void *)(uintptr_t)worker.argument);
    select_tcb(parent);
}
static struct sigaction actions[SIGCHLD+1];
static int64_t scalar(uint32_t lo,uint32_t hi) { return (int64_t)(lo|((uint64_t)hi<<32)); }
uint32_t __wrap_ciuki_syscall(uint32_t nr,uint32_t b,uint32_t c,uint32_t d,uint32_t s,uint32_t i,uint32_t p) {
    (void)p;
    switch(nr) {
    case CIUKI_SYS_EXIT:host_exit(b);
    case CIUKI_SYS_GETPID:return 7;
    case CIUKI_SYS_BRK:
        if(!heap_end)heap_end=CU_PTR(heap);
        if(b) { if(b<CU_PTR(heap)||b>CU_PTR(heap)+sizeof(heap))return -ENOMEM;heap_end=b; }
        return heap_end;
    case CIUKI_SYS_WAKE_WORD:return 0;
    case CIUKI_SYS_WAIT_WORD:return *(uint32_t *)(uintptr_t)b==c?-EINTR:-EAGAIN;
    case CIUKI_SYS_THREAD_CREATE:worker=*(struct ciuki_thread_args *)(uintptr_t)b;worker_tid++;if(worker_tid<2)worker_tid=2;worker_pending=1;return worker_tid;
    case CIUKI_SYS_THREAD_EXIT:worker_value=b;longjmp(worker_exit,1);
    case CIUKI_SYS_THREAD_JOIN:run_worker();if(c)*(uint32_t *)(uintptr_t)c=worker_value;return 0;
    case CIUKI_SYS_SIGACTION:
        if(b>SIGCHLD)return -EINVAL;
        if(d)*(struct sigaction *)(uintptr_t)d=actions[b];if(c)actions[b]=*(struct sigaction *)(uintptr_t)c;return 0;
    case CIUKI_SYS_THREAD_KILL:
        if(b!=1||c!=SIGUSR1)return -ESRCH;
        signalled=1;unsigned previous=current;select_tcb(0);actions[c].sa_handler(c);select_tcb(previous);return 0;
    case CIUKI_SYS_CLOCK_GETTIME: {
        if(b>CLOCK_PROCESS_CPUTIME_ID)return -EINVAL;
        uint64_t ticks=b==CLOCK_PROCESS_CPUTIME_ID?cpu:mono;
        *(struct ciuki_timespec *)(uintptr_t)c=(struct ciuki_timespec){.tv_sec=(int64_t)(ticks/1000)+(b==CLOCK_REALTIME?1700000000:0),.tv_nsec=(ticks%1000)*1000000};
        ++mono;++cpu;return 0;
    }
    case CIUKI_SYS_NANOSLEEP: {
        struct ciuki_timespec *r=(void *)(uintptr_t)b,*rem=(void *)(uintptr_t)c;
        uint64_t ticks=(uint64_t)r->tv_sec*1000+(r->tv_nsec+999999)/1000000,start=mono;
        signalled=0;run_worker();
        if(signalled&&mono-start<ticks) { if(rem)*rem=(struct ciuki_timespec){.tv_nsec=(ticks-(mono-start))*1000000};return -EINTR; }
        if(mono-start<ticks)mono=start+ticks;if(rem)*rem=(struct ciuki_timespec){0};return 0;
    }
    case CIUKI_SYS_UNAME: {
        if(fail_uname)return -ENOSYS;
        struct utsname *u=(void *)(uintptr_t)b;memset(u,0,sizeof(*u));strcpy(u->sysname,"CiukiOS");return 0;
    }
    case CIUKI_SYS_OPEN: {
        if(read_only&&(c&O_CREAT))return -EROFS;
        const char *name=(void *)(uintptr_t)b;int file=-1,fd=fd_slot();if(fd<0)return fd;
        if(!strcmp(name,"/tmp"))file=-3;
        else {
            for(int j=0;j<8;++j)if(!strcmp(files[j].name,name))file=j;
            if(file>=0&&(c&O_EXCL))return -EEXIST;
            if(file<0&&!(c&O_CREAT))return -ENOENT;
            if(file<0)for(int j=0;j<8;++j)if(!files[j].name[0]) { file=j;strcpy(files[j].name,name);files[j].size=0;break; }
            if(file<0)return -ENOSPC;
        }
        fds[fd]=(struct host_fd){1,file,0,c,0};return fd;
    }
    case CIUKI_SYS_STAT: {
        const char *name=(void *)(uintptr_t)b;
        for(unsigned j=0;j<8;++j)if(!strcmp(files[j].name,name)) { struct stat *st=(void *)(uintptr_t)c;memset(st,0,sizeof(*st));st->st_mode=S_IFREG|0666;st->st_size=files[j].size;return 0; }
        return -ENOENT;
    }
    case CIUKI_SYS_RENAME:
        for(unsigned j=0;j<8;++j)if(!strcmp(files[j].name,(void *)(uintptr_t)b)) { strcpy(files[j].name,(void *)(uintptr_t)c);return 0; }return -ENOENT;
    case CIUKI_SYS_UNLINK:
        for(unsigned j=0;j<8;++j)if(!strcmp(files[j].name,(void *)(uintptr_t)b)) { files[j].name[0]=0;return 0; }return -ENOENT;
    default:break;
    }
    if(b>=128||!fds[b].used)return -EBADF;
    struct host_fd *fd=&fds[b];struct host_file *file=fd->file<0?NULL:&files[fd->file];
    switch(nr) {
    case CIUKI_SYS_FSTAT: {
        struct stat *st=(void *)(uintptr_t)c;memset(st,0,sizeof(*st));st->st_mode=file?S_IFREG|0666:fd->file==-3?S_IFDIR|0777:S_IFCHR|0666;st->st_blksize=4096;st->st_size=file?file->size:0;return 0;
    }
    case CIUKI_SYS_CLOSE:fd->used=0;return 0;
    case CIUKI_SYS_DUP: { int n=fd_slot();if(n>=0)fds[n]=*fd;return n; }
    case CIUKI_SYS_FSYNC:return 0;
    case CIUKI_SYS_FCNTL:return c==F_GETFL?fd->flags&~(O_CREAT|O_EXCL):0;
    case CIUKI_SYS_LSEEK64: {
        if(fd->file==-3) { fd->cookie=0;*(int64_t *)(uintptr_t)i=0;return 0; }
        if(!file)return -ESPIPE;
        int64_t off=scalar(c,d)+(s==SEEK_CUR?(int64_t)fd->position:s==SEEK_END?file->size:0);
        if(off<0)return -EINVAL;fd->position=off;*(int64_t *)(uintptr_t)i=off;return 0;
    }
    case CIUKI_SYS_FTRUNCATE:
        if(!file||scalar(c,d)>8192)return -EFBIG;
        if(c>file->size)memset(file->data+file->size,0,c-file->size);file->size=c;return 0;
    case CIUKI_SYS_READ:case CIUKI_SYS_WRITE:case CIUKI_SYS_PREAD:case CIUKI_SYS_PWRITE: {
        int wr=nr==CIUKI_SYS_WRITE||nr==CIUKI_SYS_PWRITE,positioned=nr==CIUKI_SYS_PREAD||nr==CIUKI_SYS_PWRITE;
        if(fd->file==-1)return wr?d:0;
        if(fd->file==-2) { output((void *)(uintptr_t)c,d);return d; }
        if(!file)return -EISDIR;
        uint64_t off=positioned?(uint64_t)scalar(s,i):fd->position;if(off>8192||d>8192-off)return -EFBIG;
        unsigned count=d;if(!wr&&off+count>file->size)count=off>=file->size?0:file->size-off;
        if(wr) { memcpy(file->data+off,(void *)(uintptr_t)c,count);if(off+count>file->size)file->size=off+count; }
        else memcpy((void *)(uintptr_t)c,file->data+off,count);
        if(!positioned)fd->position=off+count;return count;
    }
    case CIUKI_SYS_GETDENTS: {
        if(fd->cookie>=2)return 0;struct ciuki_dirent *ent=(void *)(uintptr_t)c;memset(ent,0,sizeof(*ent));
        ent->d_reclen=sizeof(*ent);ent->d_type=DT_DIR;strcpy(ent->d_name,fd->cookie?"..":".");ent->d_namlen=strlen(ent->d_name);ent->d_off=++fd->cookie;return sizeof(*ent);
    }
    default:return -ENOSYS;
    }
}
