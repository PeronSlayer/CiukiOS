/* One bounded DNS worker, no NetSurf calls and no pointers into fetch state.
 * SPDX-License-Identifier: MIT */
#include "dns.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
struct slot { uint64_t token;char name[254];uint32_t address;int error;bool running,done,cancelled; };
struct ciuki_dns { pthread_mutex_t lock;pthread_cond_t wake;bool stop;uint64_t next;struct slot slots[8]; };
static void release(struct ciuki_dns *d)
{ pthread_cond_destroy(&d->wake);pthread_mutex_destroy(&d->lock);free(d); }
static void *worker(void *arg)
{
    struct ciuki_dns *d=arg;
    pthread_mutex_lock(&d->lock);
    for(;;){
        struct slot *s=NULL;
        if(d->stop){pthread_mutex_unlock(&d->lock);release(d);return NULL;}
        for(unsigned i=0;i<8;i++)if(d->slots[i].token && !d->slots[i].done && !d->slots[i].running){s=&d->slots[i];break;}
        if(!s){pthread_cond_wait(&d->wake,&d->lock);continue;}
        s->running=true;char name[254];memcpy(name,s->name,sizeof(name));
        pthread_mutex_unlock(&d->lock);
        struct addrinfo hints={0},*answer=NULL;hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;
        int r=getaddrinfo(name,NULL,&hints,&answer);
        int error=r==EAI_SYSTEM?errno:r==EAI_MEMORY?ENOMEM:r==EAI_AGAIN?ETIMEDOUT:r?EHOSTUNREACH:0;
        uint32_t address=answer?((struct sockaddr_in *)answer->ai_addr)->sin_addr.s_addr:0;
        if(!r && !answer)error=EHOSTUNREACH;
        freeaddrinfo(answer);
        pthread_mutex_lock(&d->lock);
        if(s->cancelled)memset(s,0,sizeof(*s));
        else{s->address=address;s->error=error;s->done=true;}
    }
}
struct ciuki_dns *ciuki_dns_create(void)
{
    struct ciuki_dns *d=calloc(1,sizeof(*d));if(!d)return NULL;
    if(pthread_mutex_init(&d->lock,NULL)){free(d);return NULL;}
    if(pthread_cond_init(&d->wake,NULL)){pthread_mutex_destroy(&d->lock);free(d);return NULL;}
    pthread_attr_t attr;
    if(pthread_attr_init(&attr)){release(d);return NULL;}
    int e=pthread_attr_setstacksize(&attr,128*1024);
    if(!e)e=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    pthread_t thread;if(!e)e=pthread_create(&thread,&attr,worker,d);
    pthread_attr_destroy(&attr);
    if(e){release(d);return NULL;}
    return d;
}
uint64_t ciuki_dns_submit(struct ciuki_dns *d,const char *name)
{
    size_t n=strlen(name);if(!n||n>253)return 0;
    uint64_t token=0;pthread_mutex_lock(&d->lock);
    for(unsigned i=0;i<8 && !d->stop;i++)if(!d->slots[i].token){
        if(d->next==UINT64_MAX)break;
        token=++d->next;d->slots[i].token=token;memcpy(d->slots[i].name,name,n+1);pthread_cond_signal(&d->wake);break;
    }
    pthread_mutex_unlock(&d->lock);return token;
}
int ciuki_dns_result(struct ciuki_dns *d,uint64_t token,uint32_t *address,int *error)
{
    int result=0;pthread_mutex_lock(&d->lock);
    for(unsigned i=0;i<8;i++)if(token && d->slots[i].token==token && d->slots[i].done){
        *address=d->slots[i].address;*error=d->slots[i].error;memset(&d->slots[i],0,sizeof(d->slots[i]));result=1;break;
    }
    pthread_mutex_unlock(&d->lock);return result;
}
void ciuki_dns_cancel(struct ciuki_dns *d,uint64_t token)
{
    pthread_mutex_lock(&d->lock);
    for(unsigned i=0;i<8;i++)if(token && d->slots[i].token==token){
        if(d->slots[i].running && !d->slots[i].done)d->slots[i].cancelled=true;
        else memset(&d->slots[i],0,sizeof(d->slots[i]));break;
    }
    pthread_mutex_unlock(&d->lock);
}
void ciuki_dns_destroy(struct ciuki_dns *d)
{ if(!d)return;pthread_mutex_lock(&d->lock);d->stop=true;pthread_cond_signal(&d->wake);pthread_mutex_unlock(&d->lock); }
