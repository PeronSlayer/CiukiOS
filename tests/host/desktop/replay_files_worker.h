/* SPDX-License-Identifier: MIT
 * Schedule Files' real pthread on the replay's virtual-clock baton. */
#include <pthread.h>
int rp_files_worker_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
int rp_files_worker_wait(pthread_cond_t *,pthread_mutex_t *);
int rp_files_worker_signal(pthread_cond_t *);
int rp_files_worker_join(pthread_t,void **);
#define pthread_create rp_files_worker_create
#define pthread_cond_wait rp_files_worker_wait
#define pthread_cond_signal rp_files_worker_signal
#define pthread_join rp_files_worker_join
