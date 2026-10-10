/* SPDX-License-Identifier: MIT */
#include <ciuki/runtime.h>
#include <ciuki/spawn.h>
#include <ciuki/surface.h>
#include <ciuki/channel.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>

static int32_t result(struct _reent *r,uint32_t value) {
    int e=ciuki_error(value);if(e) { r->_errno=e;return -1; }return value;
}
#define RET(v) result(__getreent(),(v))
static uint32_t high(int64_t n) { return (uint64_t)n>>32; }
int _open_r(struct _reent *r,const char *path,int flags,int mode) { return result(r,CU_CALL(OPEN,CU_PTR(path),flags,mode,0,0,0)); }
int open(const char *path,int flags,...) { int mode=0;if(flags&O_CREAT) { va_list ap;va_start(ap,flags);mode=va_arg(ap,int);va_end(ap); }return _open_r(__getreent(),path,flags,mode); }
int creat(const char *p,mode_t m) { return open(p,O_CREAT|O_WRONLY|O_TRUNC,m); }
_ssize_t _read_r(struct _reent *r,int fd,void *buf,size_t n) { return result(r,CU_CALL(READ,fd,CU_PTR(buf),n,0,0,0)); }
_ssize_t _write_r(struct _reent *r,int fd,const void *buf,size_t n) { return result(r,CU_CALL(WRITE,fd,CU_PTR(buf),n,0,0,0)); }
ssize_t read(int f,void *b,size_t n) { return _read_r(__getreent(),f,b,n); }
ssize_t write(int f,const void *b,size_t n) { return _write_r(__getreent(),f,b,n); }
ssize_t pread(int f,void *b,size_t n,off_t off) { return RET(CU_CALL(PREAD,f,CU_PTR(b),n,(uint32_t)off,high(off),0)); }
ssize_t pwrite(int f,const void *b,size_t n,off_t off) { return RET(CU_CALL(PWRITE,f,CU_PTR(b),n,(uint32_t)off,high(off),0)); }
_off_t _lseek_r(struct _reent *r,int fd,_off_t off,int whence) { int64_t out;return result(r,CU_CALL(LSEEK64,fd,(uint32_t)off,high(off),whence,CU_PTR(&out),0))<0?-1:out; }
off_t lseek(int fd,off_t off,int whence) { return _lseek_r(__getreent(),fd,off,whence); }
int _close_r(struct _reent *r,int fd) { return result(r,CU_CALL(CLOSE,fd,0,0,0,0,0)); }
int close(int fd) { return _close_r(__getreent(),fd); }
int _fstat_r(struct _reent *r,int fd,struct stat *s) { return result(r,CU_CALL(FSTAT,fd,CU_PTR(s),0,0,0,0)); }
int fstat(int fd,struct stat *s) { return _fstat_r(__getreent(),fd,s); }
int _stat_r(struct _reent *r,const char *p,struct stat *s) { return result(r,CU_CALL(STAT,CU_PTR(p),CU_PTR(s),0,0,0,0)); }
int stat(const char *p,struct stat *s) { return _stat_r(__getreent(),p,s); }
int _isatty_r(struct _reent *r,int fd) { struct stat s;if(_fstat_r(r,fd,&s)<0)return 0;r->_errno=ENOTTY;return 0; }
int isatty(int f) { return _isatty_r(__getreent(),f); }
int mkdir(const char *p,mode_t m) { return RET(CU_CALL(MKDIR,CU_PTR(p),m,0,0,0,0)); }
int rmdir(const char *p) { return RET(CU_CALL(RMDIR,CU_PTR(p),0,0,0,0,0)); }
int _rename_r(struct _reent *r,const char *a,const char *b) { return result(r,CU_CALL(RENAME,CU_PTR(a),CU_PTR(b),0,0,0,0)); }
int rename(const char *a,const char *b) { return _rename_r(__getreent(),a,b); }
int _unlink_r(struct _reent *r,const char *p) { return result(r,CU_CALL(UNLINK,CU_PTR(p),0,0,0,0,0)); }
int unlink(const char *p) { return _unlink_r(__getreent(),p); }
int remove(const char *p) { struct stat s;if(stat(p,&s)<0)return -1;return S_ISDIR(s.st_mode)?rmdir(p):unlink(p); }
int dup(int f) { return RET(CU_CALL(DUP,f,0,0,0,0,0)); }
int dup2(int a,int b) { return RET(CU_CALL(DUP2,a,b,0,0,0,0)); }
int _fcntl_r(struct _reent *r,int f,int cmd,int arg) { return result(r,CU_CALL(FCNTL,f,cmd,arg,0,0,0)); }
int _mkdir_r(struct _reent *r,const char *p,int mode) { return result(r,CU_CALL(MKDIR,CU_PTR(p),mode,0,0,0,0)); }
int _wait_r(struct _reent *r,int *status) { return result(r,CU_CALL(WAITPID,-1,CU_PTR(status),0,0,0,0)); }
int fcntl(int f,int cmd,...) { uint32_t arg=0;if(cmd==F_DUPFD||cmd==F_DUPFD_CLOEXEC||cmd==F_SETFD||cmd==F_SETFL) { va_list ap;va_start(ap,cmd);arg=va_arg(ap,int);va_end(ap); }return RET(CU_CALL(FCNTL,f,cmd,arg,0,0,0)); }
int fsync(int f) { return RET(CU_CALL(FSYNC,f,0,0,0,0,0)); }
int ftruncate(int f,off_t n) { return RET(CU_CALL(FTRUNCATE,f,(uint32_t)n,high(n),0,0,0)); }
char *getcwd(char *p,size_t n) { if(!p||!n) { errno=EINVAL;return NULL; }return RET(CU_CALL(GETCWD,CU_PTR(p),n,0,0,0,0))<0?NULL:p; }
int chdir(const char *p) { return RET(CU_CALL(CHDIR,CU_PTR(p),0,0,0,0,0)); }
int _getpid_r(struct _reent *r) { (void)r;return CU_CALL(GETPID,0,0,0,0,0,0); }
pid_t getpid(void) { return CU_CALL(GETPID,0,0,0,0,0,0); }
pid_t getppid(void) { return CU_CALL(GETPPID,0,0,0,0,0,0); }
pid_t waitpid(pid_t p,int *s,int o) { return RET(CU_CALL(WAITPID,p,CU_PTR(s),o,0,0,0)); }
pid_t wait(int *s) { return waitpid(-1,s,0); }
ciuki_pid_t ciuki_spawn(const char *path,char *const argv[],char *const envp[],const struct ciuki_spawn_fd *fds,uint32_t count,uint32_t flags) {
    struct ciuki_spawn_args args={sizeof(args),CU_PTR(path),CU_PTR(argv),CU_PTR(envp),CU_PTR(fds),count,flags,0};
    return RET(CU_CALL(SPAWN,CU_PTR(&args),0,0,0,0,0));
}
void *mmap(void *hint,size_t len,int prot,int flags,int fd,off_t offset) {
    struct ciuki_mmap_args a={sizeof(a),CU_PTR(hint),len,prot,flags,fd,offset};
    uint32_t r=CU_CALL(MMAP,CU_PTR(&a),0,0,0,0,0);int e=ciuki_error(r);if(e)errno=e;
    return e?MAP_FAILED:(void *)(uintptr_t)r;
}
int munmap(void *p,size_t n) { return RET(CU_CALL(MUNMAP,CU_PTR(p),n,0,0,0,0)); }
int mprotect(void *p,size_t n,int prot) { return RET(CU_CALL(MPROTECT,CU_PTR(p),n,prot,0,0,0)); }
static uint32_t heap_origin,heap_high_water;
uint32_t ciuki_heap_high_water(void) { return __atomic_load_n(&heap_high_water,__ATOMIC_RELAXED); }
static void account_heap(uint32_t old,uint32_t next) {
    if(!heap_origin)heap_origin=old;
    if(next>=heap_origin&&next-heap_origin>heap_high_water)__atomic_store_n(&heap_high_water,next-heap_origin,__ATOMIC_RELAXED);
}
extern void __malloc_lock(struct _reent *),__malloc_unlock(struct _reent *);
void *_sbrk_r(struct _reent *r,ptrdiff_t inc) {
    __malloc_lock(r);
    uint32_t old=CU_CALL(BRK,0,0,0,0,0,0);int e=ciuki_error(old);
    int64_t next=(int64_t)old+inc;
    if(!e&&(next<=0||next>UINT32_MAX))e=ENOMEM;
    if(!e&&inc) { uint32_t changed=CU_CALL(BRK,(uint32_t)next,0,0,0,0,0);e=ciuki_error(changed); }
    if(!e)account_heap(old,(uint32_t)next);
    __malloc_unlock(r);if(e) { r->_errno=e;return (void *)-1; }return (void *)(uintptr_t)old;
}
void *sbrk(ptrdiff_t n) { return _sbrk_r(__getreent(),n); }
int brk(void *p) { struct _reent *r=__getreent();__malloc_lock(r);uint32_t old=CU_CALL(BRK,0,0,0,0,0,0);uint32_t out=ciuki_error(old)?old:CU_CALL(BRK,CU_PTR(p),0,0,0,0,0);if(!ciuki_error(out))account_heap(old,out);__malloc_unlock(r);int e=ciuki_error(out);if(e) { r->_errno=e;return -1; }return 0; }
int uname(struct utsname *u) { return RET(CU_CALL(UNAME,CU_PTR(u),0,0,0,0,0)); }
int getpagesize(void) { return CIUKI_PAGE_SIZE; }
long sysconf(int n) {
    switch(n) { case _SC_PAGESIZE:return CIUKI_SC_PAGESIZE_VALUE;case _SC_OPEN_MAX:return CIUKI_SC_OPEN_MAX_VALUE;
    case _SC_ARG_MAX:return CIUKI_SC_ARG_MAX_VALUE;case _SC_THREAD_KEYS_MAX:return CIUKI_SC_THREAD_KEYS_MAX_VALUE;
    case _SC_THREAD_STACK_MIN:return CIUKI_SC_THREAD_STACK_MIN_VALUE;default:errno=EINVAL;return -1; }
}
int ciuki_surface_create(uint32_t w,uint32_t h,uint32_t f) { return RET(CU_CALL(SURFACE_CREATE,w,h,f,0,0,0)); }
void *ciuki_surface_map(int f,int prot) { uint32_t r=CU_CALL(SURFACE_MAP,f,prot,0,0,0,0);int e=ciuki_error(r);if(e)errno=e;return e?MAP_FAILED:(void *)(uintptr_t)r; }
int ciuki_surface_info(int f,struct ciuki_surface_info *i) { return RET(CU_CALL(SURFACE_INFO,f,CU_PTR(i),0,0,0,0)); }
int ciuki_present(int d,int s,const struct ciuki_rect *r) { return RET(CU_CALL(PRESENT,d,s,CU_PTR(r),0,0,0)); }
int ciuki_display_info(int d,struct ciuki_display_info *i) { return RET(CU_CALL(DISPLAY_INFO,d,CU_PTR(i),0,0,0,0)); }
int ciuki_input_read(int d,struct ciuki_input_event *e,uint32_t n) { return RET(CU_CALL(INPUT_READ,d,CU_PTR(e),n,0,0,0)); }
int ciuki_channel_pair(int fds[2]) { return RET(CU_CALL(CHANNEL_PAIR,CU_PTR(fds),0,0,0,0,0)); }
int ciuki_channel_send(int f,const struct ciuki_message *m,uint32_t flags) { return RET(CU_CALL(CHANNEL_SEND,f,CU_PTR(m),flags,0,0,0)); }
int ciuki_channel_recv(int f,struct ciuki_message *m,uint32_t flags) { return RET(CU_CALL(CHANNEL_RECV,f,CU_PTR(m),flags,0,0,0)); }
int ciuki_wait_word(uint32_t *w,uint32_t e,const struct ciuki_timespec *d,int clock) { return RET(CU_CALL(WAIT_WORD,CU_PTR(w),e,CU_PTR(d),clock,0,0)); }
int ciuki_wake_word(uint32_t *w,uint32_t n) { return RET(CU_CALL(WAKE_WORD,CU_PTR(w),n,0,0,0,0)); }

struct ciuki_DIR { int fd;struct dirent record; };
DIR *fdopendir(int fd) { struct stat s;if(fstat(fd,&s)<0)return NULL;if(!S_ISDIR(s.st_mode)) { errno=ENOTDIR;return NULL; }DIR *d=malloc(sizeof(*d));if(d)d->fd=fd;return d; }
DIR *opendir(const char *p) { int f=open(p,O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(f<0)return NULL;DIR *d=fdopendir(f);if(!d) { int e=errno;close(f);errno=e; }return d; }
struct dirent *readdir(DIR *d) {
    if(!d) { errno=EINVAL;return NULL; }
    int n=RET(CU_CALL(GETDENTS,d->fd,CU_PTR(&d->record),sizeof(d->record),0,0,0));
    if(n<=0)return NULL;
    if(n!=(int)sizeof(d->record)||d->record.d_reclen!=sizeof(d->record)||d->record.d_namlen>CIUKI_NAME_MAX||d->record.d_name[d->record.d_namlen]) { errno=EIO;return NULL; }
    return &d->record;
}
void rewinddir(DIR *d) { if(!d)errno=EINVAL;else (void)lseek(d->fd,0,SEEK_SET); }
int closedir(DIR *d) { if(!d) { errno=EINVAL;return -1; }int r=close(d->fd);free(d);return r; }
int dirfd(DIR *d) { if(!d) { errno=EINVAL;return -1; }return d->fd; }
static pthread_mutex_t temp_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned temp_counter;
int mkstemp(char *name) {
    size_t n=strlen(name);if(n<6||strcmp(name+n-6,"XXXXXX")) { errno=EINVAL;return -1; }
    if(pthread_mutex_lock(&temp_lock)) { errno=EIO;return -1; }
    int fd=-1;for(unsigned attempt=0;attempt<10000;++attempt) {
        unsigned k=(++temp_counter+(unsigned)getpid()*2654435761u);
        for(unsigned i=0;i<6;++i) { name[n-1-i]="abcdefghijklmnopqrstuvwxyz0123456789"[k%36];k/=36; }
        fd=open(name,O_CREAT|O_EXCL|O_RDWR,0600);if(fd>=0||errno!=EEXIST)break;
    }
    if(pthread_mutex_unlock(&temp_lock)) { if(fd>=0)close(fd);errno=EIO;return -1; }return fd;
}
FILE *tmpfile(void) {
    char name[]="/tmp/ciuki-XXXXXX";int fd=mkstemp(name);if(fd<0)return NULL;
    if(unlink(name)<0) { int e=errno;close(fd);errno=e;return NULL; }
    FILE *f=fdopen(fd,"w+");if(!f) { int e=errno;close(fd);errno=e; }return f;
}
char *tmpnam(char *out) {
    static char buffer[L_tmpnam];if(!out)out=buffer;
    if(pthread_mutex_lock(&temp_lock)) { errno=EIO;return NULL; }
    unsigned counter=++temp_counter;int e=pthread_mutex_unlock(&temp_lock);if(e) { errno=e;return NULL; }
    snprintf(out,L_tmpnam,"/tmp/c%lx-%lx",(unsigned long)getpid(),(unsigned long)counter);return out;
}
