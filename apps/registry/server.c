/* Framing is bounded independently of kernel framing; no operation starts
 * until its whole logical request has validated. SPDX-License-Identifier: MIT */
#include "server.h"
#include "../accounts/kernel_sys.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static void clear(struct rb_client *c){if(c->body){memset(c->body,0,c->total);free(c->body);}c->body=NULL;c->received=c->total=c->op=0;c->request=c->expected=c->deadline=0;}
static void close_client(struct rb_server *s,struct rb_client *c)
{if(c->lease){rb_disconnect(&s->broker,c->slot);rb_kernel_retire(&s->kernel,c->lease);}clear(c);memset(c,0,sizeof(*c));c->fd=-1;}
int rb_server_init(struct rb_server *s,const struct rb_storage *storage,struct wr_server *win32,
                   int (*account)(void *,uint32_t,struct rb_identity *,int *),void *context)
{
    if(!s||!account)return -EINVAL;memset(s,0,sizeof(*s));struct rb_authority a;
    int e=rb_kernel_init(&s->kernel,&a);if(e)return e;s->kernel.account=account;s->kernel.context=context;
    for(unsigned i=0;i<RB_LINK_MAX;i++)s->clients[i].fd=-1;
    return rb_init(&s->broker,&a,storage,win32);
}
static int answer(struct rb_server *s,struct rb_client *c,int status,const uint8_t *body,size_t n)
{
    if(status)n=0;size_t at=0;
    do{
        if(rb_kernel_live(&s->kernel,c->lease))return -ESRCH;
        struct ciuki_message m={0};uint8_t *p=m.data;size_t take=n-at;if(take>192)take=192;
        rb_put32(p,0x31435343);rb_put32(p+4,1);rb_put32(p+8,64);rb_put32(p+12,4);
        rb_put32(p+16,c->op);rb_put32(p+20,2);rb_put64(p+24,c->request);rb_put32(p+32,c->endpoint);
        rb_put32(p+36,(uint32_t)take);rb_put32(p+48,(uint32_t)status);rb_put32(p+52,(uint32_t)n);rb_put32(p+56,(uint32_t)at);
        if(n>=32&&c->op<RB_WIN32)rb_put64(p+40,rb_get64(body+16));
        if(take)memcpy(p+64,body+at,take);m.length=(uint32_t)(64+take);
        int e=acct_sys_channel_send(c->fd,&m);if(e)return e;at+=take;
    }while(at<n);return 0;
}
static int frame(struct rb_server *s,struct rb_client *c,const struct ciuki_message *m)
{
    if(m->length<64||m->fd_count)return -EINVAL;const uint8_t *p=m->data;
    uint32_t bytes=rb_get32(p+36),total=rb_get32(p+52),offset=rb_get32(p+56),op=rb_get32(p+16);uint64_t id=rb_get64(p+24),expected=rb_get64(p+40);
    if(rb_get32(p)!=0x31435343||rb_get32(p+4)!=1||rb_get32(p+8)!=64||rb_get32(p+12)!=4||rb_get32(p+20)!=1||
       !id||id<=c->last||rb_get32(p+32)!=c->endpoint||bytes>192||m->length!=64+bytes||rb_get32(p+48)||rb_get32(p+60)||
       total>RB_BODY_MAX||offset>total||bytes>total-offset||(!bytes&&total))return -EINVAL;
    if(!c->body){
        if(offset)return -EINVAL;c->body=malloc(total?total:1);if(!c->body)return -ENOMEM;
        c->op=op;c->request=id;c->total=total;c->expected=expected;c->deadline=acct_sys_monotonic_ms()+5000;
    }
    if(op!=c->op||id!=c->request||total!=c->total||expected!=c->expected||offset!=c->received)return -EINVAL;
    if(bytes)memcpy(c->body+offset,p+64,bytes);c->received+=bytes;
    if(c->received<c->total)return 0;
    uint8_t *reply=malloc(RB_BODY_MAX);if(!reply)return -ENOMEM;size_t n=0;
    int e=0;if(c->op<RB_WIN32&&c->total>=32&&rb_get64(c->body+16)!=expected)e=-EINVAL;
    if(!e)e=rb_exchange(&s->broker,c->slot,c->op,c->body,c->total,reply,RB_BODY_MAX,&n);
    int sent=answer(s,c,e,reply,n);memset(reply,0,RB_BODY_MAX);free(reply);c->last=c->request;clear(c);
    if(s->broker.links[c->slot].stage||s->broker.links[c->slot].reply)c->deadline=acct_sys_monotonic_ms()+5000;
    return sent;
}
void rb_server_step(struct rb_server *s,unsigned idle_ms)
{
    for(unsigned i=0;i<RB_LINK_MAX;i++)if(s->clients[i].lease&&
        (rb_kernel_live(&s->kernel,s->clients[i].lease)||
         (s->clients[i].deadline&&acct_sys_monotonic_ms()>=s->clients[i].deadline)))close_client(s,s->clients+i);
    for(unsigned i=0;i<RB_LINK_MAX;i++)if(!s->clients[i].lease){
        uintptr_t lease=0;int fd=-1;uint32_t generation=0;int e=rb_kernel_accept(&s->kernel,&lease,&fd,&generation);if(e)break;
        unsigned slot;e=rb_connect(&s->broker,lease,&slot);if(e){rb_kernel_retire(&s->kernel,lease);continue;}
        s->clients[i]=(struct rb_client){.lease=lease,.fd=fd,.endpoint=generation,.slot=slot};
    }
    int fds[RB_LINK_MAX];unsigned count=0;
    for(unsigned i=0;i<RB_LINK_MAX;i++)if(s->clients[i].lease){
        struct rb_client *c=s->clients+i;
        for(unsigned burst=0;burst<64&&c->lease;burst++){
            struct ciuki_message m={0};int r=acct_sys_channel_recv(c->fd,&m,0);if(r==-EAGAIN)break;
            if(r<=0){close_client(s,c);break;}
            if(m.fd_count){for(unsigned j=0;j<m.fd_count;j++)acct_sys_close(m.fds[j]);close_client(s,c);break;}
            if(frame(s,c,&m)){close_client(s,c);break;}
        }
        if(c->lease)fds[count++]=c->fd;
    }
    acct_sys_wait_readable(fds,count,idle_ms);
}
void rb_server_destroy(struct rb_server *s){for(unsigned i=0;i<RB_LINK_MAX;i++)close_client(s,s->clients+i);rb_destroy(&s->broker);}
