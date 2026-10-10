/* SPDX-License-Identifier: MIT */
#ifndef _CIUKI_PTHREAD_H
#define _CIUKI_PTHREAD_H
#include <sys/types.h>
#include <time.h>
typedef ciuki_pthread_t pthread_t;
typedef struct ciuki_pthread_mutex pthread_mutex_t;
typedef struct ciuki_pthread_cond pthread_cond_t;
typedef struct ciuki_pthread_attr pthread_attr_t;
typedef struct ciuki_pthread_mutexattr pthread_mutexattr_t;
typedef struct ciuki_pthread_condattr pthread_condattr_t;
typedef uint32_t pthread_once_t;
typedef uint32_t pthread_key_t;
#define PTHREAD_MUTEX_INITIALIZER CIUKI_PTHREAD_MUTEX_INITIALIZER
#define PTHREAD_COND_INITIALIZER CIUKI_PTHREAD_COND_INITIALIZER
#define PTHREAD_ONCE_INIT CIUKI_PTHREAD_ONCE_INIT
#define PTHREAD_MUTEX_NORMAL CIUKI_PTHREAD_MUTEX_NORMAL
#define PTHREAD_MUTEX_RECURSIVE CIUKI_PTHREAD_MUTEX_RECURSIVE
#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED CIUKI_THREAD_DETACHED
#define PTHREAD_STACK_MIN CIUKI_THREAD_STACK_MIN
#define PTHREAD_KEYS_MAX CIUKI_THREAD_KEYS_MAX
#define PTHREAD_DESTRUCTOR_ITERATIONS CIUKI_THREAD_DESTRUCTOR_ITERATIONS
int pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
void pthread_exit(void *) __attribute__((noreturn));
int pthread_join(pthread_t,void **);
int pthread_detach(pthread_t);
pthread_t pthread_self(void);
int pthread_equal(pthread_t,pthread_t);
int pthread_attr_init(pthread_attr_t *);
int pthread_attr_destroy(pthread_attr_t *);
int pthread_attr_setstacksize(pthread_attr_t *,size_t);
int pthread_attr_getstacksize(const pthread_attr_t *,size_t *);
int pthread_attr_setdetachstate(pthread_attr_t *,int);
int pthread_attr_getdetachstate(const pthread_attr_t *,int *);
int pthread_mutexattr_init(pthread_mutexattr_t *);
int pthread_mutexattr_destroy(pthread_mutexattr_t *);
int pthread_mutexattr_settype(pthread_mutexattr_t *,int);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *,int *);
int pthread_mutex_init(pthread_mutex_t *,const pthread_mutexattr_t *);
int pthread_mutex_destroy(pthread_mutex_t *);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_trylock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);
int pthread_condattr_init(pthread_condattr_t *);
int pthread_condattr_destroy(pthread_condattr_t *);
int pthread_condattr_setclock(pthread_condattr_t *,clockid_t);
int pthread_condattr_getclock(const pthread_condattr_t *,clockid_t *);
int pthread_cond_init(pthread_cond_t *,const pthread_condattr_t *);
int pthread_cond_destroy(pthread_cond_t *);
int pthread_cond_wait(pthread_cond_t *,pthread_mutex_t *);
int pthread_cond_timedwait(pthread_cond_t *,pthread_mutex_t *,const struct timespec *);
int pthread_cond_signal(pthread_cond_t *);
int pthread_cond_broadcast(pthread_cond_t *);
int pthread_once(pthread_once_t *,void (*)(void));
int pthread_key_create(pthread_key_t *,void (*)(void *));
int pthread_key_delete(pthread_key_t);
void *pthread_getspecific(pthread_key_t);
int pthread_setspecific(pthread_key_t,const void *);
int pthread_sigmask(int,const sigset_t *,sigset_t *);
int pthread_kill(pthread_t,int);
#endif
