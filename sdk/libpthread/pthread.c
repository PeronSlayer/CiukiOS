/* SPDX-License-Identifier: MIT */
#include <ciuki/runtime.h>
#include <ciuki/channel.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define DEAD UINT32_MAX
struct once_frame { pthread_once_t *control; struct once_frame *previous; };
struct thread_private {
    void *values[CIUKI_THREAD_KEYS_MAX];
    uint32_t generations[CIUKI_THREAD_KEYS_MAX];
    struct once_frame *once;
    int allocated;
};
static struct thread_private main_private;
struct key_slot { uint32_t generation; int used; void (*destroy)(void *); };
static struct key_slot keys[CIUKI_THREAD_KEYS_MAX];
static pthread_mutex_t key_lock=PTHREAD_MUTEX_INITIALIZER;
static struct thread_private *private(void) { return (void *)(uintptr_t)ciuki_tcb()->pthread_private; }
static int error(uint32_t r) { return ciuki_error(r); }
static void fatal(void) __attribute__((noreturn));
static void fatal(void) { abort(); }
void __ciuki_once_exit_check(void) { if(private()->once)fatal(); }
void __ciuki_pthread_main(void) { ciuki_tcb()->pthread_private=CU_PTR(&main_private); }
pthread_t pthread_self(void) { return ciuki_tcb()->tid; }
int pthread_equal(pthread_t a,pthread_t b) { return a==b; }
static int attr_valid(const pthread_attr_t *a) {
    return a && !a->reserved[0] && !a->reserved[1] && a->detached<=PTHREAD_CREATE_DETACHED &&
        a->stack_bytes>=CIUKI_THREAD_STACK_MIN && a->stack_bytes<=CIUKI_THREAD_STACK_MAX && !(a->stack_bytes%CIUKI_PAGE_SIZE);
}
int pthread_attr_init(pthread_attr_t *a) { if(!a)return EINVAL; *a=(pthread_attr_t){CIUKI_THREAD_STACK_DEFAULT,0,{0,0}}; return 0; }
int pthread_attr_destroy(pthread_attr_t *a) { if(!attr_valid(a))return EINVAL; a->reserved[0]=DEAD;return 0; }
int pthread_attr_setstacksize(pthread_attr_t *a,size_t n) {
    if(!attr_valid(a))return EINVAL;
    if(n<CIUKI_THREAD_STACK_MIN || n>CIUKI_THREAD_STACK_MAX || n%CIUKI_PAGE_SIZE)return ENOTSUP;
    a->stack_bytes=n;return 0;
}
int pthread_attr_getstacksize(const pthread_attr_t *a,size_t *n) { if(!attr_valid(a)||!n)return EINVAL;*n=a->stack_bytes;return 0; }
int pthread_attr_setdetachstate(pthread_attr_t *a,int d) { if(!attr_valid(a))return EINVAL;if(d!=PTHREAD_CREATE_JOINABLE&&d!=PTHREAD_CREATE_DETACHED)return ENOTSUP;a->detached=d;return 0; }
int pthread_attr_getdetachstate(const pthread_attr_t *a,int *d) { if(!attr_valid(a)||!d)return EINVAL;*d=a->detached;return 0; }
int pthread_mutexattr_init(pthread_mutexattr_t *a) { if(!a)return EINVAL;*a=(pthread_mutexattr_t){PTHREAD_MUTEX_NORMAL,0};return 0; }
static int ma_valid(const pthread_mutexattr_t *a) { return a&&!a->reserved&&a->type<=PTHREAD_MUTEX_RECURSIVE; }
int pthread_mutexattr_destroy(pthread_mutexattr_t *a) { if(!ma_valid(a))return EINVAL;a->reserved=DEAD;return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t *a,int type) { if(!ma_valid(a))return EINVAL;if(type!=PTHREAD_MUTEX_NORMAL&&type!=PTHREAD_MUTEX_RECURSIVE)return ENOTSUP;a->type=type;return 0; }
int pthread_mutexattr_gettype(const pthread_mutexattr_t *a,int *type) { if(!ma_valid(a)||!type)return EINVAL;*type=a->type;return 0; }
static int mutex_valid(const pthread_mutex_t *m) { return m && m->words[2]<=PTHREAD_MUTEX_RECURSIVE && !m->words[4]&&!m->words[5]&&!m->words[6]&&!m->words[7]; }
int pthread_mutex_init(pthread_mutex_t *m,const pthread_mutexattr_t *a) { if(!m||(a&&!ma_valid(a)))return EINVAL;memset(m,0,sizeof(*m));if(a)m->words[2]=a->type;return 0; }
int pthread_mutex_destroy(pthread_mutex_t *m) {
    if(!mutex_valid(m))return EINVAL;
    if(__atomic_load_n(&m->words[0],__ATOMIC_ACQUIRE)||__atomic_load_n(&m->words[3],__ATOMIC_ACQUIRE))return EBUSY;
    m->words[4]=DEAD;return 0;
}
static int acquire(pthread_mutex_t *m,int try) {
    if(!mutex_valid(m))return EINVAL;
    uint32_t self=pthread_self();
    for(;;) {
        uint32_t owner=0;
        if(__atomic_compare_exchange_n(&m->words[0],&owner,self,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) { m->words[1]=1;return 0; }
        if(owner==self) {
            if(try&&m->words[2]==PTHREAD_MUTEX_NORMAL)return EBUSY;
            if(m->words[2]==PTHREAD_MUTEX_NORMAL)return EDEADLK;
            if(m->words[1]==UINT32_MAX)return EAGAIN;
            ++m->words[1];return 0;
        }
        if(try)return EBUSY;
        __atomic_add_fetch(&m->words[3],1,__ATOMIC_RELAXED);
        int e=error(CU_CALL(WAIT_WORD,CU_PTR(&m->words[0]),owner,0,CLOCK_MONOTONIC,0,0));
        __atomic_sub_fetch(&m->words[3],1,__ATOMIC_RELAXED);
        if(e&&e!=EAGAIN&&e!=EINTR)return e;
    }
}
int pthread_mutex_lock(pthread_mutex_t *m) { return acquire(m,0); }
int pthread_mutex_trylock(pthread_mutex_t *m) { return acquire(m,1); }
int pthread_mutex_unlock(pthread_mutex_t *m) {
    if(!mutex_valid(m))return EINVAL;
    if(__atomic_load_n(&m->words[0],__ATOMIC_RELAXED)!=pthread_self())return EPERM;
    if(--m->words[1])return 0;
    __atomic_store_n(&m->words[0],0,__ATOMIC_RELEASE);
    /* Always wake: avoids waiter-count visibility races on a future SMP port. */
    return error(CU_CALL(WAKE_WORD,CU_PTR(&m->words[0]),1,0,0,0,0));
}
static int ca_valid(const pthread_condattr_t *a) { return a&&!a->reserved&&(a->clock_id==CLOCK_REALTIME||a->clock_id==CLOCK_MONOTONIC); }
int pthread_condattr_init(pthread_condattr_t *a) { if(!a)return EINVAL;*a=(pthread_condattr_t){CLOCK_REALTIME,0};return 0; }
int pthread_condattr_destroy(pthread_condattr_t *a) { if(!ca_valid(a))return EINVAL;a->reserved=DEAD;return 0; }
int pthread_condattr_setclock(pthread_condattr_t *a,clockid_t c) { if(!ca_valid(a))return EINVAL;if(c!=CLOCK_REALTIME&&c!=CLOCK_MONOTONIC)return ENOTSUP;a->clock_id=c;return 0; }
int pthread_condattr_getclock(const pthread_condattr_t *a,clockid_t *c) { if(!ca_valid(a)||!c)return EINVAL;*c=a->clock_id;return 0; }
static int cond_valid(const pthread_cond_t *c) { return c&&!c->words[3]&&(c->words[1]==CLOCK_REALTIME||c->words[1]==CLOCK_MONOTONIC); }
int pthread_cond_init(pthread_cond_t *c,const pthread_condattr_t *a) { if(!c||(a&&!ca_valid(a)))return EINVAL;memset(c,0,sizeof(*c));if(a)c->words[1]=a->clock_id;return 0; }
int pthread_cond_destroy(pthread_cond_t *c) { if(!cond_valid(c))return EINVAL;if(__atomic_load_n(&c->words[2],__ATOMIC_ACQUIRE))return EBUSY;c->words[3]=DEAD;return 0; }
static int cond_wait(pthread_cond_t *c,pthread_mutex_t *m,const struct timespec *deadline) {
    if(!cond_valid(c)||!mutex_valid(m))return EINVAL;
    if(__atomic_load_n(&m->words[0],__ATOMIC_RELAXED)!=pthread_self())return EPERM;
    if(m->words[1]!=1 || (deadline&&(deadline->tv_nsec<0||deadline->tv_nsec>=1000000000||deadline->reserved)))return EINVAL;
    uint32_t seq=__atomic_load_n(&c->words[0],__ATOMIC_ACQUIRE);
    __atomic_add_fetch(&c->words[2],1,__ATOMIC_RELAXED);
    int e=pthread_mutex_unlock(m);
    if(!e) {
        do { e=error(CU_CALL(WAIT_WORD,CU_PTR(&c->words[0]),seq,CU_PTR(deadline),c->words[1],0,0)); } while(e==EINTR);
        if(e==EAGAIN)e=0;
        int relock=pthread_mutex_lock(m);if(relock)e=relock;
    }
    __atomic_sub_fetch(&c->words[2],1,__ATOMIC_RELEASE);return e;
}
int pthread_cond_wait(pthread_cond_t *c,pthread_mutex_t *m) { return cond_wait(c,m,NULL); }
int pthread_cond_timedwait(pthread_cond_t *c,pthread_mutex_t *m,const struct timespec *t) { if(!t)return EINVAL;return cond_wait(c,m,t); }
static int notify(pthread_cond_t *c,uint32_t n) { if(!cond_valid(c))return EINVAL;__atomic_add_fetch(&c->words[0],1,__ATOMIC_RELEASE);return error(CU_CALL(WAKE_WORD,CU_PTR(&c->words[0]),n,0,0,0,0)); }
int pthread_cond_signal(pthread_cond_t *c) { return notify(c,1); }
int pthread_cond_broadcast(pthread_cond_t *c) { return notify(c,UINT32_MAX); }
int pthread_once(pthread_once_t *once,void (*fn)(void)) {
    if(!once||!fn)return EINVAL;
    struct thread_private *p=private();
    for(struct once_frame *f=p->once;f;f=f->previous) if(f->control==once)fatal();
    for(;;) {
        uint32_t state=__atomic_load_n(once,__ATOMIC_ACQUIRE);
        if(state==2)return 0;if(state>2)return EINVAL;
        if(!state&&__atomic_compare_exchange_n(once,&state,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {
            struct once_frame f={once,p->once};p->once=&f;fn();p->once=f.previous;
            __atomic_store_n(once,2,__ATOMIC_RELEASE);
            return error(CU_CALL(WAKE_WORD,CU_PTR(once),UINT32_MAX,0,0,0,0));
        }
        int e=error(CU_CALL(WAIT_WORD,CU_PTR(once),1,0,CLOCK_MONOTONIC,0,0));
        if(e&&e!=EAGAIN&&e!=EINTR)return e;
    }
}
int pthread_key_create(pthread_key_t *key,void (*d)(void *)) {
    if(!key)return EINVAL;int e=pthread_mutex_lock(&key_lock);if(e)return e;
    e=EAGAIN;
    for(uint32_t i=0;i<CIUKI_THREAD_KEYS_MAX;++i) if(!keys[i].used&&keys[i].generation<UINT32_MAX/CIUKI_THREAD_KEYS_MAX-1) {
        ++keys[i].generation;keys[i].used=1;keys[i].destroy=d;
        *key=keys[i].generation*CIUKI_THREAD_KEYS_MAX+i;e=0;break;
    }
    int u=pthread_mutex_unlock(&key_lock);return e?e:u;
}
static int key_valid(pthread_key_t key) { uint32_t i=key%CIUKI_THREAD_KEYS_MAX;return keys[i].used&&keys[i].generation==key/CIUKI_THREAD_KEYS_MAX; }
int pthread_key_delete(pthread_key_t key) {
    int e=pthread_mutex_lock(&key_lock);if(e)return e;
    if(!key_valid(key))e=EINVAL;else keys[key%CIUKI_THREAD_KEYS_MAX].used=0;
    int u=pthread_mutex_unlock(&key_lock);return e?e:u;
}
void *pthread_getspecific(pthread_key_t key) {
    if(pthread_mutex_lock(&key_lock))return NULL;
    uint32_t i=key%CIUKI_THREAD_KEYS_MAX;struct thread_private *p=private();
    void *v=key_valid(key)&&p->generations[i]==keys[i].generation?p->values[i]:NULL;
    if(pthread_mutex_unlock(&key_lock))fatal();return v;
}
int pthread_setspecific(pthread_key_t key,const void *v) {
    int e=pthread_mutex_lock(&key_lock);if(e)return e;
    if(!key_valid(key))e=EINVAL;else {
        uint32_t i=key%CIUKI_THREAD_KEYS_MAX;private()->generations[i]=keys[i].generation;private()->values[i]=(void *)v;
    }
    int u=pthread_mutex_unlock(&key_lock);return e?e:u;
}
void pthread_exit(void *result) {
    struct thread_private *p=private();if(p->once)fatal();
    for(unsigned pass=0;pass<CIUKI_THREAD_DESTRUCTOR_ITERATIONS;++pass) {
        int any=0;
        for(unsigned i=0;i<CIUKI_THREAD_KEYS_MAX;++i) {
            if(pthread_mutex_lock(&key_lock))fatal();
            void (*d)(void *)=keys[i].used&&p->generations[i]==keys[i].generation?keys[i].destroy:NULL;
            void *v=p->values[i];p->values[i]=NULL;
            if(pthread_mutex_unlock(&key_lock))fatal();
            if(v&&d) { any=1;d(v); }
        }
        if(!any)break;
    }
    struct _reent *r=__getreent();_reclaim_reent(r);
    if(p->allocated) { free(p);free(r); }
    CU_CALL(THREAD_EXIT,CU_PTR(result),0,0,0,0,0);__builtin_trap();
}
struct start_args { void *(*fn)(void *);void *arg; };
static void thread_return(void) { pthread_exit(NULL); }
static void thread_start(void *arg) {
    struct ciuki_tcb *t=ciuki_tcb();
    _Static_assert(sizeof(struct _reent)+sizeof(struct ciuki_tcb)<=CIUKI_TLS_SIZE,"bootstrap reent fits TLS");
    struct _reent *bootstrap=(void *)((char *)t+sizeof(*t));
    _REENT_INIT_PTR(bootstrap);t->reent=CU_PTR(bootstrap);
    struct _reent *r=malloc(sizeof(*r));struct thread_private *p=calloc(1,sizeof(*p));
    if(!r||!p)fatal();_REENT_INIT_PTR(r);t->reent=CU_PTR(r);p->allocated=1;t->pthread_private=CU_PTR(p);
    struct start_args start=*(struct start_args *)arg;free(arg);
    pthread_exit(start.fn(start.arg));
}
int pthread_create(pthread_t *tid,const pthread_attr_t *attr,void *(*fn)(void *),void *arg) {
    if(!tid||!fn||(attr&&!attr_valid(attr)))return EINVAL;
    /* Preserve errno: allocation is the only libc operation in this positive-error API. */
    int saved=errno;struct start_args *a=malloc(sizeof(*a));errno=saved;
    if(!a)return EAGAIN;*a=(struct start_args){fn,arg};
    struct ciuki_thread_args wire={sizeof(wire),CU_PTR(thread_start),CU_PTR(a),CU_PTR(thread_return),attr?attr->stack_bytes:0,attr?attr->detached:0,{0,0}};
    uint32_t r=CU_CALL(THREAD_CREATE,CU_PTR(&wire),0,0,0,0,0);int e=error(r);
    if(e) { free(a);errno=saved;return e==EFAULT||e==EINVAL?EINVAL:e==ENOMEM||e==EAGAIN?EAGAIN:e; }
    *tid=r;return 0;
}
int pthread_join(pthread_t t,void **result) { uint32_t value,r;do { r=CU_CALL(THREAD_JOIN,t,CU_PTR(&value),0,0,0,0); } while(error(r)==EINTR);int e=error(r);if(!e&&result)*result=(void *)(uintptr_t)value;return e; }
int pthread_detach(pthread_t t) { return error(CU_CALL(THREAD_DETACH,t,0,0,0,0,0)); }
int pthread_sigmask(int how,const sigset_t *set,sigset_t *old) { return error(CU_CALL(SIGPROCMASK,how,CU_PTR(set),CU_PTR(old),0,0,0)); }
int pthread_kill(pthread_t t,int sig) { return error(CU_CALL(THREAD_KILL,t,sig,0,0,0,0)); }
