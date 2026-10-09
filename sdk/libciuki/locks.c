/* SPDX-License-Identifier: MIT */
#include <ciuki/runtime.h>
#include <sys/lock.h>
#include <stdlib.h>
struct __lock { pthread_mutex_t mutex; };
#define NORMAL(name) struct __lock name={PTHREAD_MUTEX_INITIALIZER}
#define RECURSIVE(name) struct __lock name={{{0,0,PTHREAD_MUTEX_RECURSIVE,0,0,0,0,0}}}
RECURSIVE(__lock___malloc_recursive_mutex);
RECURSIVE(__lock___env_recursive_mutex);
RECURSIVE(__lock___sinit_recursive_mutex);
RECURSIVE(__lock___sfp_recursive_mutex);
RECURSIVE(__lock___atexit_recursive_mutex);
RECURSIVE(__lock___tzset_recursive_mutex);
NORMAL(__lock___tz_mutex);
NORMAL(__lock___at_quick_exit_mutex);
NORMAL(__lock___dd_hash_mutex);
NORMAL(__lock___arc4random_mutex);
static void check(int e) { if(e)abort(); }
void __retarget_lock_init(_LOCK_T *l) { *l=malloc(sizeof(**l));if(!*l)abort();check(pthread_mutex_init(&(*l)->mutex,NULL)); }
void __retarget_lock_init_recursive(_LOCK_T *l) { pthread_mutexattr_t a;check(pthread_mutexattr_init(&a));check(pthread_mutexattr_settype(&a,PTHREAD_MUTEX_RECURSIVE));*l=malloc(sizeof(**l));if(!*l)abort();check(pthread_mutex_init(&(*l)->mutex,&a)); }
void __retarget_lock_close(_LOCK_T l) { check(pthread_mutex_destroy(&l->mutex));free(l); }
void __retarget_lock_close_recursive(_LOCK_T l) { __retarget_lock_close(l); }
void __retarget_lock_acquire(_LOCK_T l) { check(pthread_mutex_lock(&l->mutex)); }
void __retarget_lock_acquire_recursive(_LOCK_T l) { __retarget_lock_acquire(l); }
int __retarget_lock_try_acquire(_LOCK_T l) { return pthread_mutex_trylock(&l->mutex)==0; }
int __retarget_lock_try_acquire_recursive(_LOCK_T l) { return __retarget_lock_try_acquire(l); }
void __retarget_lock_release(_LOCK_T l) { check(pthread_mutex_unlock(&l->mutex)); }
void __retarget_lock_release_recursive(_LOCK_T l) { __retarget_lock_release(l); }
