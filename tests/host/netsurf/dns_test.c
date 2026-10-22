/* Resolver queue, generations, cancellation and asynchronous shutdown.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "sdk/netsurf/port/dns.h"
#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static bool allow,entered,released,fail_thread,fail_alloc;
static void *allocation;
static unsigned notifications;
static void notify(void *ctx)
{ assert(ctx==&notifications);pthread_mutex_lock(&gate);notifications++;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate); }
void *__real_calloc(size_t,size_t);
void *__wrap_calloc(size_t n,size_t size)
{
    if(fail_alloc)return NULL;
    void *p=__real_calloc(n,size);allocation=p;return p;
}
void __real_free(void *);
void __wrap_free(void *p)
{
    pthread_mutex_lock(&gate);
    if(p && p==allocation){released=true;pthread_cond_signal(&changed);}
    pthread_mutex_unlock(&gate);__real_free(p);
}
int __real_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
int __wrap_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void *),void *ctx)
{ if(fail_thread)return EAGAIN;return __real_pthread_create(t,a,fn,ctx); }
int __wrap_getaddrinfo(const char *name,const char *service,const struct addrinfo *hints,struct addrinfo **out)
{
    (void)service;(void)hints;*out=NULL;
    pthread_mutex_lock(&gate);entered=true;pthread_cond_signal(&changed);
    while(!allow)pthread_cond_wait(&changed,&gate);pthread_mutex_unlock(&gate);
    if(!strcmp(name,"missing.example"))return EAI_NONAME;
    if(!strcmp(name,"offline.example")){errno=ENETDOWN;return EAI_SYSTEM;}
    struct addrinfo *a=malloc(sizeof(*a));struct sockaddr_in *s=malloc(sizeof(*s));assert(a && s);
    *s=(struct sockaddr_in){.sin_family=AF_INET,.sin_addr.s_addr=0x12345678};
    *a=(struct addrinfo){.ai_family=AF_INET,.ai_addr=(void *)s,.ai_addrlen=sizeof(*s)};*out=a;return 0;
}
void __wrap_freeaddrinfo(struct addrinfo *a) { if(a){free(a->ai_addr);free(a);} }
static void wait_for(bool *flag)
{
    struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_sec+=3;
    pthread_mutex_lock(&gate);while(!*flag)assert(!pthread_cond_timedwait(&changed,&gate,&until));pthread_mutex_unlock(&gate);
}
static void unblock(void)
{ pthread_mutex_lock(&gate);allow=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate); }
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    fail_alloc=true;assert(!ciuki_dns_create());fail_alloc=false;
    fail_thread=true;assert(!ciuki_dns_create());fail_thread=false;released=false;
    struct ciuki_dns *d=ciuki_dns_create();assert(d);
    ciuki_dns_notify(d,notify,&notifications);
    uint64_t tokens[8];for(unsigned i=0;i<8;i++){tokens[i]=ciuki_dns_submit(d,i==2?"missing.example":i==3?"offline.example":"fixture.example");assert(tokens[i]);}
    wait_for(&entered);assert(!ciuki_dns_submit(d,"full.example"));
    ciuki_dns_cancel(d,tokens[0]);ciuki_dns_cancel(d,tokens[1]);
    uint64_t replacement=ciuki_dns_submit(d,"replacement.example");assert(replacement>tokens[7]);
    unblock();uint32_t address;int error;
    assert(!ciuki_dns_result(d,tokens[0],&address,&error));assert(!ciuki_dns_result(d,tokens[1],&address,&error));
    for(unsigned i=2;i<=8;i++){
        uint64_t token=i==8?replacement:tokens[i];
        for(;;){
            pthread_mutex_lock(&gate);unsigned seen=notifications;pthread_mutex_unlock(&gate);
            if(ciuki_dns_result(d,token,&address,&error))break;
            struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_sec+=3;
            pthread_mutex_lock(&gate);
            while(notifications==seen)assert(!pthread_cond_timedwait(&changed,&gate,&until));
            pthread_mutex_unlock(&gate);
        }
        assert(error==(i==2?EHOSTUNREACH:i==3?ENETDOWN:0));if(!error)assert(address==0x12345678);
    }
    assert(notifications==7);ciuki_dns_destroy(d);wait_for(&released);
    pthread_mutex_lock(&gate);allow=false;entered=false;released=false;pthread_mutex_unlock(&gate);
    d=ciuki_dns_create();assert(d && ciuki_dns_submit(d,"slow.example"));wait_for(&entered);
    struct timespec before,after;clock_gettime(CLOCK_MONOTONIC,&before);ciuki_dns_destroy(d);clock_gettime(CLOCK_MONOTONIC,&after);
    assert((after.tv_sec-before.tv_sec)*1000000000LL+after.tv_nsec-before.tv_nsec<100000000);
    unblock();wait_for(&released);
    puts("PASS DNS bounded queue/generation/cancelled late answers/failures/OOM/nonblocking shutdown/final release");
    return 0;
}
