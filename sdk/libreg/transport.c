/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include "transport.h"
#include "../private/registry_protocol.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static uint32_t rd32(const void *v){const uint8_t *p=v;return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t rd64(const void *v){const uint8_t *p=v;return rd32(p)|(uint64_t)rd32(p+4)<<32;}
static void wr32(void *v,uint32_t n){uint8_t *p=v;for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(i*8));}
static void wr64(void *v,uint64_t n){wr32(v,(uint32_t)n);wr32((uint8_t *)v+4,(uint32_t)(n>>32));}
static void header(uint8_t *b,const struct reg_remote *r,uint32_t offset,uint32_t n,uint64_t generation)
{memset(b,0,32);wr32(b,1);wr32(b+4,r->root);wr32(b+8,offset);wr32(b+12,n);wr64(b+16,generation);}
#if defined(__CIUKIOS__) && !defined(__linux__)
#include <ciuki/runtime.h>
#include <ciuki/security.h>
#include <ciuki/channel.h>
#include <unistd.h>
#include <time.h>
static uint64_t monotonic(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+(uint32_t)t.tv_nsec/1000000;}
void reg_remote_close(struct reg_remote *r){if(r->fd>=0)close(r->fd);r->fd=-1;r->endpoint=r->service_generation=r->peer_pid=0;}
static int connect_registry(struct reg_remote *r)
{
    struct ciuki_security_link in={.version=1,.size=64,.service=r->service?r->service:CIUKI_SECURITY_SERVICE_REGISTRY},out={0};
    struct ciuki_security_args a={.version=1,.size=64,.operation=CIUKI_SECURITY_SERVICE_CONNECT,
        .spawn_address=CU_PTR(&in),.output_address=CU_PTR(&out),.output_bytes=64};
    if(ciuki_security(&a))return -1;
    if(out.endpoint_fd<0||!out.endpoint_generation||!out.service_generation||out.server_role!=(in.service==CIUKI_SECURITY_SERVICE_ACCOUNTS?CIUKI_SECURITY_ROLE_ACCOUNT:CIUKI_SECURITY_ROLE_REGISTRY)){
        if(out.endpoint_fd>=0)close(out.endpoint_fd);return reg_fail(EIO);}
    r->fd=out.endpoint_fd;r->endpoint=out.endpoint_generation;r->service_generation=out.service_generation;r->peer_pid=out.peer_pid;r->serial=0;return 0;
}
int reg_registry_rpc(struct reg_remote *r,unsigned op,const void *in,size_t n,void *out,size_t cap,size_t *used)
{
    if(!used||n>RB_BODY_MAX||(!in&&n)||(!out&&cap))return reg_fail(EINVAL);
    *used=0;if(r->fd<0&&connect_registry(r))return -1;
    if(r->serial==UINT64_MAX){reg_remote_close(r);return reg_fail(EOVERFLOW);}
    uint64_t id=++r->serial,deadline=monotonic()+5000;size_t at=0;
    do{
        struct ciuki_message m={0};uint8_t *h=m.data;size_t take=n-at;if(take>192)take=192;
        wr32(h,0x31435343);wr32(h+4,1);wr32(h+8,64);wr32(h+12,r->service?r->service:4);wr32(h+16,op);wr32(h+20,1);
        wr64(h+24,id);wr32(h+32,r->endpoint);wr32(h+36,(uint32_t)take);wr32(h+52,(uint32_t)n);wr32(h+56,(uint32_t)at);
        /* Body expected generation is root-specific; the envelope must carry it too. */
        if((!r->service||r->service==4)&&n>=32&&op<RB_WIN32)wr64(h+40,rd64((const uint8_t *)in+16));
        if(r->service==2&&n==312)wr64(h+40,rd64((const uint8_t *)in+40));
        if(take)memcpy(h+64,(const uint8_t *)in+at,take);m.length=(uint32_t)(64+take);
        while(ciuki_channel_send(r->fd,&m,1)<0){
            if(errno!=EAGAIN||monotonic()>=deadline)goto transport_error;
            struct timespec pause={.tv_nsec=1000000};nanosleep(&pause,NULL);
        }
        at+=take;
    }while(at<n);
    at=0;uint32_t total=0;uint64_t generation=0;
    do{
        struct ciuki_message m={0};int got;
        while((got=ciuki_channel_recv(r->fd,&m,1))<0&&errno==EAGAIN&&monotonic()<deadline){
            struct timespec pause={.tv_nsec=1000000};nanosleep(&pause,NULL);
        }
        if(got!=1)goto transport_error;
        if(m.fd_count){for(unsigned i=0;i<m.fd_count;i++)close(m.fds[i]);goto malformed;}
        const uint8_t *h=m.data;uint32_t count=rd32(h+36),length=rd32(h+52);
        if(m.sender_pid!=r->peer_pid||m.length<64||rd32(h)!=0x31435343||rd32(h+4)!=1||rd32(h+8)!=64||
           rd32(h+12)!=(r->service?r->service:4)||rd32(h+16)!=op||rd32(h+20)!=2||rd64(h+24)!=id||rd32(h+32)!=r->endpoint||
           count>192||m.length!=64+count||length>RB_BODY_MAX||length>cap||rd32(h+56)!=at||rd32(h+60))goto malformed;
        int32_t status=(int32_t)rd32(h+48);
        if(status){if(status>=0||at||length||count)goto malformed;return reg_fail(-status);}
        if(!at){total=length;generation=rd64(h+40);}else if(total!=length||generation!=rd64(h+40))goto malformed;
        if(count>total-at||(!count&&total))goto malformed;
        if(count)memcpy((uint8_t *)out+at,h+64,count);at+=count;
    }while(at<total);
    *used=at;return 0;
malformed:errno=EINVAL;
transport_error:{int e=errno==EAGAIN?ETIMEDOUT:errno;if(!e)e=ESRCH;reg_remote_close(r);return reg_fail(e);}
}
#else
#ifndef CIUKI_REG_TRANSPORT_FAKE
void reg_remote_close(struct reg_remote *r){r->fd=-1;}
int reg_registry_rpc(struct reg_remote *r,unsigned op,const void *in,size_t n,void *out,size_t cap,size_t *used)
{(void)r;(void)op;(void)in;(void)n;(void)out;(void)cap;(void)used;return reg_fail(EOPNOTSUPP);}
#endif
#endif
static const char *native_name(unsigned root){return root==RB_SYSTEM?"System":root==RB_PROGRAMS?"Programs":"Users";}
/* The 8 KiB page buffers live on the heap: the SDK target limits frames to
 * 4 KiB (-Wframe-larger-than=4096). */
static int remote_load(struct reg_remote *r,struct rt **tree,uint8_t *out)
{
    uint8_t in[32];size_t used=0;header(in,r,0,0,0);
    if(reg_registry_rpc(r,RB_NATIVE_READ,in,32,out,RB_BODY_MAX,&used))return -1;
    if(used!=32||rd32(out)!=1||rd32(out+4)!=r->root||rd32(out+8)||rd64(out+24))return reg_fail(EINVAL);
    uint32_t total=rd32(out+12);uint64_t generation=rd64(out+16);
    if(total>RB_ROOT_MAX||generation==UINT64_MAX)return reg_fail(EIO);
    char *text=malloc((size_t)total+1);if(!text)return -1;
    uint32_t at=0;int error=0;
    while(at<total){header(in,r,at,total,generation);
        if(reg_registry_rpc(r,RB_NATIVE_PAGE,in,32,out,RB_BODY_MAX,&used)){error=errno;break;}
        size_t take=total-at;if(take>RB_PAGE)take=RB_PAGE;
        if(used!=32+take||rd32(out)!=1||rd32(out+4)!=r->root||rd32(out+8)!=at||rd32(out+12)!=total||
           rd64(out+16)!=generation||rd64(out+24)){error=EAGAIN;break;}
        memcpy(text+at,out+32,take);at+=(uint32_t)take;
    }
    text[total]=0;struct rt *next=error?NULL:reg_parse(native_name(r->root),text,total);free(text);
    if(!next)return reg_fail(error?error:EIO);
    if(next->generation!=generation){reg_tree_free(next);return reg_fail(EIO);}
    reg_tree_free(*tree);*tree=next;return 0;
}
int reg_remote_load(struct reg_remote *r,struct rt **tree)
{
    uint8_t *out=malloc(RB_BODY_MAX);if(!out)return -1;
    int e=remote_load(r,tree,out);free(out);return e;
}
static int remote_commit(struct reg_remote *r,struct rt *tree,uint64_t generation,uint8_t *in)
{
    size_t n=0;char *text=reg_serialise(tree,NULL,&n);if(!text)return -1;
    uint8_t out[32];size_t used=0;int error=0;
    header(in,r,0,(uint32_t)n,generation);
    if(reg_registry_rpc(r,RB_NATIVE_BEGIN,in,32,out,sizeof(out),&used)){error=errno;goto done;}
    for(size_t at=0;at<n;){size_t take=n-at;if(take>RB_PAGE)take=RB_PAGE;
        header(in,r,(uint32_t)at,(uint32_t)n,generation);memcpy(in+32,text+at,take);
        if(reg_registry_rpc(r,RB_NATIVE_PART,in,32+take,out,sizeof(out),&used)){error=errno;break;}at+=take;
    }
    if(!error){header(in,r,0,(uint32_t)n,generation);
        if(reg_registry_rpc(r,RB_NATIVE_COMMIT,in,32,out,sizeof(out),&used))error=errno;
    }
    if(error){int saved=error;header(in,r,0,0,0);(void)reg_registry_rpc(r,RB_NATIVE_ABORT,in,32,out,sizeof(out),&used);error=saved;}
done:free(text);return error?reg_fail(error):0;
}
int reg_remote_commit(struct reg_remote *r,struct rt *tree,uint64_t generation)
{
    uint8_t *in=malloc(RB_BODY_MAX);if(!in)return -1;
    int e=remote_commit(r,tree,generation,in);free(in);return e;
}
int reg_remote_win32(struct reg_remote *r,const void *input,size_t n,void *output,size_t cap,size_t *used)
{
    if(!input||!output||!used||n>72000)return reg_fail(EINVAL);*used=0;
    uint8_t *body=malloc(RB_BODY_MAX);if(!body)return -1;uint8_t reply[32];size_t bytes=0;int error=0;
    memset(body,0,32);wr32(body,1);wr32(body+12,(uint32_t)n);
    if(reg_registry_rpc(r,RB_WIN32_BEGIN,body,32,reply,32,&bytes))error=errno;
    for(size_t at=0;!error&&at<n;){size_t take=n-at;if(take>RB_PAGE)take=RB_PAGE;
        memset(body,0,32);wr32(body,1);wr32(body+8,(uint32_t)at);wr32(body+12,(uint32_t)n);memcpy(body+32,(const uint8_t *)input+at,take);
        if(reg_registry_rpc(r,RB_WIN32_PART,body,32+take,reply,32,&bytes))error=errno;at+=take;
    }
    if(!error){memset(body,0,32);wr32(body,1);wr32(body+12,(uint32_t)n);
        if(reg_registry_rpc(r,RB_WIN32_COMMIT,body,32,reply,32,&bytes))error=errno;
    }
    uint32_t total=!error&&bytes==32?rd32(reply+12):0;
    if(!error&&(bytes!=32||rd32(reply)!=1||total>cap||total>72000))error=EINVAL;
    for(uint32_t at=0;!error&&at<total;){uint32_t take=total-at;if(take>RB_PAGE)take=RB_PAGE;
        memset(reply,0,32);wr32(reply,1);wr32(reply+8,at);wr32(reply+12,total);
        if(reg_registry_rpc(r,RB_WIN32_READ,reply,32,body,RB_BODY_MAX,&bytes))error=errno;
        else if(bytes!=32+take||rd32(body)!=1||rd32(body+8)!=at||rd32(body+12)!=total)error=EINVAL;
        if(!error)memcpy((uint8_t *)output+at,body+32,take);at+=take;
    }
    free(body);if(error){reg_remote_close(r);return reg_fail(error);}*used=total;return 0;
}
