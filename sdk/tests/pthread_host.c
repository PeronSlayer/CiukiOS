/* Host-only deterministic syscall model. SPDX-License-Identifier: MIT */
#include <ciuki/runtime.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static struct ciuki_tcb tcb;
static struct _reent reent;
__FILE __sf[3];
static unsigned char heap[32768];
static unsigned heap_used;
static unsigned wait_calls,wake_calls,last_count;
static uint32_t *last_word;
static int scenario,created_error,destructors,reclaims;
static void *exit_jump[5];
static uint32_t exit_value;
struct ciuki_tcb *__ciuki_host_tcb(void) { return &tcb; }
struct _reent *__getreent(void) { return &reent; }
int *__errno(void) { return &reent._errno; }
void _reclaim_reent(struct _reent *r) { (void)r;++reclaims; }
void *memset(void *p,int v,size_t n) { unsigned char *b=p;while(n--)*b++=v;return p; }
void *memcpy(void *p,const void *s,size_t n) { unsigned char *b=p;const unsigned char *a=s;while(n--)*b++=*a++;return p; }
void *malloc(size_t n) { n=(n+7)&~7u;if(n>sizeof(heap)-heap_used)return NULL;void *p=heap+heap_used;heap_used+=n;return p; }
void *calloc(size_t n,size_t s) { if(s&&n>sizeof(heap)/s)return NULL;void *p=malloc(n*s);if(p)memset(p,0,n*s);return p; }
void free(void *p) { (void)p; }
void abort(void) { __asm__ volatile("syscall"::"a"(60),"D"(127):"memory","rcx","r11");__builtin_trap(); }
uint32_t ciuki_syscall(uint32_t call,uint32_t b,uint32_t c,uint32_t d,uint32_t s,uint32_t i,uint32_t p) {
    (void)d;(void)s;(void)i;(void)p;
    switch(call) {
    case CIUKI_SYS_WAIT_WORD:
        last_word=(void *)(uintptr_t)b;++wait_calls;
        if(wait_calls==1)return -EINTR;
        if(scenario==1) { *last_word=0;return 0; }
        if(scenario==2)return -ETIMEDOUT;
        if(scenario==3) { *last_word=2;return -EAGAIN; }
        return -EFAULT;
    case CIUKI_SYS_WAKE_WORD:++wake_calls;last_word=(void *)(uintptr_t)b;last_count=c;return 0;
    case CIUKI_SYS_THREAD_CREATE:return created_error?(uint32_t)-created_error:12;
    case CIUKI_SYS_THREAD_EXIT:exit_value=b;__builtin_longjmp(exit_jump,1);
    case CIUKI_SYS_THREAD_JOIN:return -ESRCH;
    case CIUKI_SYS_THREAD_DETACH:return -EINVAL;
    case CIUKI_SYS_SIGPROCMASK:return -EINVAL;
    case CIUKI_SYS_THREAD_KILL:return -ESRCH;
    default:return -ENOSYS;
    }
}
#define CHECK(x) do { if(!(x))return __LINE__%100+1; } while(0)
static unsigned once_runs;
static void initializer(void) { ++once_runs; }
static pthread_key_t destructor_key;
static void destroy(void *v) { ++destructors;if(pthread_setspecific(destructor_key,v))abort(); }
static void *unused_start(void *p) { return p; }
extern uint64_t __udivdi3(uint64_t,uint64_t),__umoddi3(uint64_t,uint64_t);
extern int64_t __divdi3(int64_t,int64_t),__moddi3(int64_t,int64_t);
int ciuki_pthread_host_tests(void) {
    uint64_t random=UINT64_MAX;
    for(unsigned j=0;j<1000;++j) {
        random=random*UINT64_C(6364136223846793005)+1;uint64_t denominator=(random>>3)|1;
        CHECK(__udivdi3(random,denominator)==random/denominator);CHECK(__umoddi3(random,denominator)==random%denominator);
        int64_t a=(int64_t)(random>>1),b=(int64_t)(denominator>>1)+1;
        CHECK(__divdi3(-a,b)==-a/b);CHECK(__moddi3(-a,b)==-a%b);
    }
    CHECK(__udivdi3(UINT64_MAX,1)==UINT64_MAX);CHECK(__divdi3(INT64_MIN,1)==INT64_MIN);
    CHECK(ciuki_error((uint32_t)-EINTR)==EINTR);CHECK(ciuki_error(0xb0000000u)==0);
    CHECK(ciuki_error((uint32_t)-CIUKI_SYSCALL_ERROR_MAX)==CIUKI_SYSCALL_ERROR_MAX);
    CHECK(ciuki_error((uint32_t)-CIUKI_SYSCALL_ERROR_MAX-1)==0);
    tcb.self=(uintptr_t)&tcb;tcb.tid=7;__ciuki_pthread_main();errno=E2BIG;
    pthread_mutex_t m=PTHREAD_MUTEX_INITIALIZER;
    CHECK(pthread_mutex_unlock(&m)==EPERM);
    CHECK(!pthread_mutex_lock(&m));CHECK(pthread_mutex_lock(&m)==EDEADLK);CHECK(pthread_mutex_trylock(&m)==EBUSY);
    CHECK(pthread_mutex_destroy(&m)==EBUSY);CHECK(!pthread_mutex_unlock(&m));
    m.words[0]=8;m.words[1]=1;scenario=1;wait_calls=0;
    CHECK(!pthread_mutex_lock(&m));CHECK(wait_calls==2&&m.words[0]==7);CHECK(errno==E2BIG);CHECK(!pthread_mutex_unlock(&m));
    pthread_mutexattr_t ma;CHECK(!pthread_mutexattr_init(&ma));CHECK(!pthread_mutexattr_settype(&ma,PTHREAD_MUTEX_RECURSIVE));
    CHECK(!pthread_mutex_init(&m,&ma));CHECK(!pthread_mutex_lock(&m));CHECK(!pthread_mutex_lock(&m));CHECK(m.words[1]==2);
    CHECK(!pthread_mutex_unlock(&m)&&m.words[0]==7);m.words[1]=UINT32_MAX;CHECK(pthread_mutex_lock(&m)==EAGAIN);
    m.words[1]=1;CHECK(!pthread_mutex_unlock(&m));CHECK(!pthread_mutex_destroy(&m));CHECK(pthread_mutex_lock(&m)==EINVAL);
    CHECK(!pthread_mutex_init(&m,NULL));CHECK(!pthread_mutex_lock(&m));
    pthread_cond_t cv=PTHREAD_COND_INITIALIZER;struct timespec deadline={1,0,0};scenario=2;wait_calls=0;
    CHECK(pthread_cond_timedwait(&cv,&m,&deadline)==ETIMEDOUT);CHECK(wait_calls==2&&m.words[0]==7&&cv.words[2]==0);
    cv.words[0]=UINT32_MAX;CHECK(!pthread_cond_signal(&cv)&&cv.words[0]==0);CHECK(last_word==&cv.words[0]&&last_count==1);
    CHECK(!pthread_cond_broadcast(&cv)&&last_count==UINT32_MAX);
    cv.words[2]=1;CHECK(pthread_cond_destroy(&cv)==EBUSY);cv.words[2]=0;CHECK(!pthread_cond_destroy(&cv));
    CHECK(pthread_cond_signal(&cv)==EINVAL);CHECK(!pthread_mutex_unlock(&m));
    pthread_once_t once=PTHREAD_ONCE_INIT;CHECK(!pthread_once(&once,initializer));CHECK(!pthread_once(&once,initializer)&&once_runs==1);
    once=1;scenario=3;wait_calls=0;CHECK(!pthread_once(&once,initializer)&&wait_calls==2&&once_runs==1);
    pthread_key_t all[CIUKI_THREAD_KEYS_MAX],extra;
    for(unsigned j=0;j<CIUKI_THREAD_KEYS_MAX;++j)CHECK(!pthread_key_create(&all[j],NULL));
    CHECK(pthread_key_create(&extra,NULL)==EAGAIN);CHECK(!pthread_setspecific(all[0],(void *)123));CHECK(pthread_getspecific(all[0])==(void *)123);
    CHECK(!pthread_key_delete(all[0]));CHECK(pthread_setspecific(all[0],(void *)1)==EINVAL);CHECK(pthread_getspecific(all[0])==NULL);
    CHECK(!pthread_key_create(&extra,NULL)&&extra!=all[0]);CHECK(pthread_getspecific(extra)==NULL);CHECK(!pthread_key_delete(extra));
    for(unsigned j=1;j<CIUKI_THREAD_KEYS_MAX;++j)CHECK(!pthread_key_delete(all[j]));
    CHECK(!pthread_key_create(&destructor_key,destroy));CHECK(!pthread_setspecific(destructor_key,(void *)1));
    pthread_t thread;created_error=ENOMEM;CHECK(pthread_create(&thread,NULL,unused_start,NULL)==EAGAIN&&errno==E2BIG);
    created_error=EFAULT;CHECK(pthread_create(&thread,NULL,unused_start,NULL)==EINVAL&&errno==E2BIG);
    CHECK(pthread_join(9,NULL)==ESRCH);CHECK(pthread_detach(9)==EINVAL);CHECK(pthread_sigmask(-1,NULL,NULL)==EINVAL);CHECK(pthread_kill(9,SIGUSR1)==ESRCH);
    if(!__builtin_setjmp(exit_jump))pthread_exit((void *)37);
    CHECK(exit_value==37&&destructors==CIUKI_THREAD_DESTRUCTOR_ITERATIONS&&reclaims==1);
    CHECK(wake_calls>0);return 0;
}
