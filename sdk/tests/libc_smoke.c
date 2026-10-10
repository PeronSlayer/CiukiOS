/* Standalone F2 libc payload; guest assertions are reported through frozen call 3.
 * SPDX-License-Identifier: MIT */
#include <ciuki/runtime.h>
#include <ciuki/raw.h>
#include <ciuki/channel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
static unsigned checks,failures,heap_peak;
static int callback_order;
static jmp_buf jump;
static void report(const char *name) {
    heap_peak=ciuki_heap_high_water();
    char record[CIUKI_PROBE_REPORT_MAX+1];
    /* Controller supplies run/sequence/probe/event and task identity. */
    int n=snprintf(record,sizeof(record),"case=%s pid=%ld tid=%lu expected=0 observed=%u checks=%u failures=%u heap_peak=%u reent_bytes=%u tcb_bytes=%u order=%d",name,(long)getpid(),(unsigned long)pthread_self(),failures,checks,failures,heap_peak,(unsigned)sizeof(struct _reent),(unsigned)sizeof(struct ciuki_tcb),callback_order);
    if(n<=0||n>CIUKI_PROBE_REPORT_MAX||ciuki_error(ciuki_raw_probe_report(CU_PTR(record),n,0,0,0,0)))_exit(126);
}
static void failed_check(const char *expression,unsigned line,int error) {
    char name[81],record[CIUKI_PROBE_REPORT_MAX+1];unsigned i=0;
    for(;expression[i]&&i<sizeof(name)-1;++i) {
        char c=expression[i];name[i]=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')?c:'_';
    }
    name[i]=0;
    int n=snprintf(record,sizeof(record),"case=%s expected=1 observed=0 line=%u errno=%d",name,line,error);
    if(n<=0||n>CIUKI_PROBE_REPORT_MAX||ciuki_error(ciuki_raw_probe_report(CU_PTR(record),n,0,0,0,0)))_exit(126);
}
#define CHECK(x) do { __atomic_add_fetch(&checks,1,__ATOMIC_RELAXED);if(!(x)) { int error=errno;__atomic_add_fetch(&failures,1,__ATOMIC_RELAXED);failed_check(#x,__LINE__,error);errno=error; } } while(0)
__attribute__((constructor)) static void construct(void) { callback_order=1; }
static void smoke_atexit(void) { CHECK(callback_order==2);callback_order=3;report("atexit"); }
__attribute__((destructor)) static void destruct(void) { CHECK(callback_order==3);callback_order=4;report("destructor");if(failures)_exit(1); }
static int64_t ns(const struct timespec *t) { return t->tv_sec*1000000000LL+t->tv_nsec; }
static void clock_report(const char *name, const char *a, int64_t first, const char *b, int64_t second, const char *c, int64_t third) {
    char record[CIUKI_PROBE_REPORT_MAX+1];
    int n=snprintf(record,sizeof(record),"case=%s %s=%lld %s=%lld %s=%lld",name,
                   a,(long long)first,b,(long long)second,c,(long long)third);
    if(n<=0||n>CIUKI_PROBE_REPORT_MAX||ciuki_error(ciuki_raw_probe_report(CU_PTR(record),n,0,0,0,0)))_exit(126);
}
static uint32_t signal_start;
static volatile sig_atomic_t caught;
static void catch_signal(int signo) { if(signo==SIGUSR1)++caught; }
static void *interrupt_sleep(void *arg) {
    while(!__atomic_load_n(&signal_start,__ATOMIC_ACQUIRE)) {
        int e=ciuki_error(CU_CALL(WAIT_WORD,CU_PTR(&signal_start),0,0,CLOCK_MONOTONIC,0,0));
        if(e&&e!=EAGAIN&&e!=EINTR) return (void *)(uintptr_t)e;
    }
    struct timespec ten={0,10000000,0};if(nanosleep(&ten,NULL))return (void *)(uintptr_t)errno;
    return (void *)(uintptr_t)pthread_kill((pthread_t)(uintptr_t)arg,SIGUSR1);
}
static void *thread_errno(void *arg) { (void)arg;errno=ERANGE;CHECK(errno==ERANGE);return (void *)(uintptr_t)errno; }
int main(void) {
    CHECK(callback_order==1);callback_order=2;CHECK(!atexit(smoke_atexit));
    char text[128];CHECK(snprintf(text,sizeof(text),"%lld %.2f",(long long)0x100000001,1.25)>0);CHECK(!strcmp(text,"4294967297 1.25"));
    CHECK(printf("CiukiOS libc smoke: %s\n",text)>0);CHECK(fprintf(stderr,"CiukiOS stderr smoke\n")>0);CHECK(!fflush(NULL));
    void *p=malloc(37);CHECK(p&&((uintptr_t)p%8)==0);if(p) { memset(p,0x5a,37);p=realloc(p,4096);CHECK(p&&((unsigned char *)p)[0]==0x5a);free(p); }
    void *aligned=NULL;CHECK(!posix_memalign(&aligned,64,256));CHECK(aligned&&((uintptr_t)aligned%64)==0);free(aligned);
    p=aligned_alloc(64,256);CHECK(p&&((uintptr_t)p%64)==0);free(p);
    CHECK(strtoll("-4294967297",NULL,10)==-4294967297LL);CHECK(strtod("1.25",NULL)==1.25);
    CHECK(fabs(sin(0.5)-0.479425538604203)<1e-12);CHECK(fabs(sqrt(2.0)*sqrt(2.0)-2.0)<1e-12);CHECK(log(1.0)==0.0);
    int jumped=setjmp(jump);if(!jumped)longjmp(jump,7);CHECK(jumped==7);
    errno=E2BIG;pthread_t thread;void *value=NULL;int e=pthread_create(&thread,NULL,thread_errno,NULL);CHECK(!e);if(!e)CHECK(!pthread_join(thread,&value)&&value==(void *)(uintptr_t)ERANGE);CHECK(errno==E2BIG);
    CHECK(!setenv("CIUKI_SMOKE","works",1));CHECK(getenv("CIUKI_SMOKE")&&!strcmp(getenv("CIUKI_SMOKE"),"works"));CHECK(!unsetenv("CIUKI_SMOKE"));CHECK(getenv("CIUKI_SMOKE")==NULL);
    static char assignment[]="CIUKI_SMOKE=putenv";CHECK(!putenv(assignment));CHECK(!strcmp(getenv("CIUKI_SMOKE"),"putenv"));CHECK(!unsetenv("CIUKI_SMOKE"));
    FILE *f=tmpfile();CHECK(f!=NULL);if(f) { CHECK(fwrite("abc",1,3,f)==3);CHECK(!fflush(f));CHECK(!fseeko(f,0,SEEK_SET));CHECK(ftello(f)==0);CHECK(fread(text,1,3,f)==3&&!memcmp(text,"abc",3));CHECK(!fclose(f)); }
    char name[]="/tmp/smoke-XXXXXX";int fd=mkstemp(name);CHECK(fd>=0);if(fd>=0) { CHECK(write(fd,"data",4)==4);CHECK(!fsync(fd));
        struct stat st;CHECK(!fstat(fd,&st)&&st.st_size==4);CHECK(pread(fd,text,4,0)==4&&!memcmp(text,"data",4));
        CHECK(pwrite(fd,"D",1,0)==1);CHECK(lseek(fd,0,SEEK_CUR)==4);int duplicate=dup(fd);CHECK(duplicate>=0);if(duplicate>=0)CHECK(!close(duplicate));
        char renamed[64];snprintf(renamed,sizeof(renamed),"%s-moved",name);CHECK(!rename(name,renamed));CHECK(!stat(renamed,&st)&&st.st_size==4);
        CHECK(!ftruncate(fd,8192));CHECK(pread(fd,text,1,8191)==1&&text[0]==0);CHECK(lseek(fd,0x100000001LL,SEEK_SET)==0x100000001LL);CHECK(!close(fd));CHECK(!unlink(renamed)); }
    DIR *d=opendir("/tmp");CHECK(d!=NULL);if(d) { struct dirent *first=readdir(d);CHECK(first!=NULL);rewinddir(d);CHECK(readdir(d)!=NULL);CHECK(!closedir(d)); }
    struct timespec previous,now;CHECK(!clock_gettime(CLOCK_MONOTONIC,&previous));unsigned decreases=0;
    for(unsigned i=0;i<10000;++i) { CHECK(!clock_gettime(CLOCK_MONOTONIC,&now));if(now.tv_sec<previous.tv_sec||(now.tv_sec==previous.tv_sec&&now.tv_nsec<previous.tv_nsec))++decreases;previous=now; }CHECK(!decreases);
    CHECK(!clock_gettime(CLOCK_REALTIME,&now));CHECK(!clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&now));CHECK(clock()>=0);CHECK(time(NULL)>=0);
    struct timespec sleep20_start;CHECK(!clock_gettime(CLOCK_MONOTONIC,&sleep20_start));
    struct timespec request={0,20000000,0};CHECK(!nanosleep(&request,NULL));CHECK(!clock_gettime(CLOCK_MONOTONIC,&now));CHECK((now.tv_sec-previous.tv_sec)*1000000000LL+now.tv_nsec-previous.tv_nsec>=20000000);
    clock_report("clock-monotonic","reads",10000,"decreases",decreases,"sleep_ns",ns(&now)-ns(&sleep20_start));
    CHECK(clock_gettime(-1,&now)==-1&&errno==EINVAL);CHECK(!clock_getres(CLOCK_MONOTONIC,&now)&&now.tv_nsec>0);
    struct timespec cpu_before,cpu_after,sleep_before,sleep_after;
    CHECK(!clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&cpu_before));CHECK(!clock_gettime(CLOCK_MONOTONIC,&sleep_before));
    do { CHECK(!clock_gettime(CLOCK_MONOTONIC,&sleep_after)); } while(ns(&sleep_after)-ns(&sleep_before)<20000000);
    CHECK(!clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&cpu_after));CHECK(ns(&cpu_after)>ns(&cpu_before));
    int64_t busy_ns=ns(&cpu_after)-ns(&cpu_before);
    request.tv_nsec=100000000;CHECK(!nanosleep(&request,NULL));CHECK(!clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&cpu_before));
    CHECK(ns(&cpu_before)-ns(&cpu_after)<=2000000);
    clock_report("clock-cpu","busy_ns",busy_ns,"sleep_cpu_ns",ns(&cpu_before)-ns(&cpu_after),"requested_sleep_ns",100000000);
    CHECK(signal(SIGUSR1,catch_signal)!=SIG_ERR);e=pthread_create(&thread,NULL,interrupt_sleep,(void *)(uintptr_t)pthread_self());CHECK(!e);
    if(!e) {
        __atomic_store_n(&signal_start,1,__ATOMIC_RELEASE);CHECK(ciuki_wake_word(&signal_start,1)>=0);
        request.tv_nsec=20000000;struct timespec remaining={0,0,0};int sleep_result=nanosleep(&request,&remaining);int sleep_errno=errno;
        CHECK(sleep_result==-1&&sleep_errno==EINTR&&ns(&remaining)>=0&&ns(&remaining)<=20000000);
        CHECK(!pthread_join(thread,&value)&&value==NULL);CHECK(caught==1);
        clock_report("sleep-interrupt","result",sleep_result,"errno",sleep_errno,"remainder_ns",ns(&remaining));
    }
    CHECK(fork()==-1&&errno==ENOSYS);CHECK(vfork()==-1&&errno==ENOSYS);CHECK(_Fork()==-1&&errno==ENOSYS);CHECK(execv("/bin/x",NULL)==-1&&errno==ENOSYS);
    CHECK(system(NULL)==0);CHECK(system("x")==-1&&errno==ENOSYS);CHECK(popen("x","r")==NULL&&errno==ENOSYS);
    CHECK(isatty(1)==0&&errno==ENOTTY);CHECK(isatty(-1)==0&&errno==EBADF);CHECK(setlocale(LC_ALL,"C")!=NULL);CHECK(setlocale(LC_ALL,"ciuki-invalid")==NULL);
    CHECK(sysconf(_SC_PAGESIZE)==CIUKI_PAGE_SIZE);CHECK(sysconf(-1)==-1&&errno==EINVAL);
    struct utsname u;CHECK(!uname(&u)&&!strcmp(u.sysname,"CiukiOS"));
    report("libc-smoke");return failures?1:0;
}
