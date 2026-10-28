/* Direct retained REGISTRY link. Included by server.c, no external symbols.
 * Private transport, not a public Win32 header. SPDX-License-Identifier: MIT */
#include "../../private/registry_protocol.h"
static uint32_t registry_fd=UINT32_MAX,registry_generation,registry_peer;
static uint64_t registry_serial;
static uint32_t br32(const void *v){const uint8_t *p=v;return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t br64(const void *v){const uint8_t *p=v;return br32(p)|(uint64_t)br32(p+4)<<32;}
static void bw32(void *v,uint32_t n){uint8_t *p=v;for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(i*8));}
static void bw64(void *v,uint64_t n){bw32(v,(uint32_t)n);bw32((uint8_t *)v+4,(uint32_t)(n>>32));}
static uint32_t broker_errno(int32_t e){return e==-13||e==-1?5:e==-11?170:e==-12?8:e==-22?87:e==-95?120:e==-5?1016:1062;}
static void broker_close(void){if(registry_fd!=UINT32_MAX)w32r_gate(CIUKI_SYS_CLOSE,registry_fd,0,0);registry_fd=UINT32_MAX;registry_generation=registry_peer=0;}
static uint32_t broker_rpc(unsigned op,const void *in,uint32_t n,void *out,uint32_t cap,uint32_t *used)
{
    *used=0;if(n>RB_BODY_MAX)return 87;
    if(registry_fd==UINT32_MAX){
        struct ciuki_security_link q={.version=1,.size=64,.service=4},r={0};
        struct ciuki_security_args a={.version=1,.size=64,.operation=CIUKI_SECURITY_SERVICE_CONNECT,
            .spawn_address=(uint32_t)(uintptr_t)&q,.output_address=(uint32_t)(uintptr_t)&r,.output_bytes=64};
#ifdef W32_HOST_MODEL
        extern int32_t w32r_host_security(struct ciuki_security_args *,const struct ciuki_security_link *,struct ciuki_security_link *) __attribute__((weak));
        int32_t e=w32r_host_security?w32r_host_security(&a,&q,&r):-38;
#else
        int32_t e=(int32_t)w32r_gate(CIUKI_SYS_SECURITY,(uintptr_t)&a,64,0);
#endif
        if(e<0)return broker_errno(e);
        if(r.endpoint_fd<0||r.server_role!=CIUKI_SECURITY_ROLE_REGISTRY||!r.endpoint_generation){
            if(r.endpoint_fd>=0)w32r_gate(CIUKI_SYS_CLOSE,(uint32_t)r.endpoint_fd,0,0);return 1062;}
        registry_fd=(uint32_t)r.endpoint_fd;registry_generation=r.endpoint_generation;registry_peer=r.peer_pid;registry_serial=0;
    }
    uint64_t id=++registry_serial;if(!id){broker_close();return 1062;}
    uint32_t at=0;
    do{
        struct ciuki_message m;w32_fill(&m,0,sizeof(m));uint8_t *h=m.data;uint32_t take=n-at;if(take>192)take=192;
        bw32(h,0x31435343);bw32(h+4,1);bw32(h+8,64);bw32(h+12,4);bw32(h+16,op);bw32(h+20,1);bw64(h+24,id);
        bw32(h+32,registry_generation);bw32(h+36,take);bw32(h+52,n);bw32(h+56,at);
        if(take)w32_copy(h+64,(const uint8_t *)in+at,take);m.length=64+take;
        int32_t e=(int32_t)w32r_gate(CIUKI_SYS_CHANNEL_SEND,registry_fd,(uintptr_t)&m,0);
        if(e<0){broker_close();return broker_errno(e);}at+=take;
    }while(at<n);
    at=0;uint32_t total=0;
    do{
        struct ciuki_message m;w32_fill(&m,0,sizeof(m));
        int32_t e=(int32_t)w32r_gate(CIUKI_SYS_CHANNEL_RECV,registry_fd,(uintptr_t)&m,0);
        if(e!=1){broker_close();return e<0?broker_errno(e):1062;}
        if(m.fd_count){for(unsigned i=0;i<m.fd_count;i++)w32r_gate(CIUKI_SYS_CLOSE,(uint32_t)m.fds[i],0,0);broker_close();return 13;}
        const uint8_t *h=m.data;uint32_t take=br32(h+36),length=br32(h+52);
        if(m.sender_pid!=registry_peer||m.length<64||br32(h)!=0x31435343||br32(h+4)!=1||br32(h+8)!=64||br32(h+12)!=4||
           br32(h+16)!=op||br32(h+20)!=2||br64(h+24)!=id||br32(h+32)!=registry_generation||take>192||
           m.length!=64+take||length>RB_BODY_MAX||length>cap||br32(h+56)!=at||br32(h+60)){
            broker_close();return 13;
        }
        int32_t status=(int32_t)br32(h+48);if(status){if(status>=0||length||take||at){broker_close();return 13;}return broker_errno(status);}
        if(!at)total=length;else if(total!=length){broker_close();return 13;}
        if(take>total-at||(!take&&total)){broker_close();return 13;}
        if(take)w32_copy((uint8_t *)out+at,h+64,take);at+=take;
    }while(at<total);
    *used=at;return 0;
}
static void broker_header(uint8_t *p,uint32_t offset,uint32_t total){w32_fill(p,0,32);bw32(p,1);bw32(p+8,offset);bw32(p+12,total);}
static uint32_t broker_exchange(const void *in,uint32_t n,void *out,uint32_t cap,uint32_t *used)
{
    void *heap=NtCurrentTeb()->ProcessEnvironmentBlock->ProcessHeap;
    uint8_t *body=RtlAllocateHeap(heap,0,RB_BODY_MAX);if(!body)return 8;uint8_t reply[32];uint32_t bytes=0;
    broker_header(body,0,n);uint32_t e=broker_rpc(RB_WIN32_BEGIN,body,32,reply,32,&bytes);
    for(uint32_t at=0;!e&&at<n;){uint32_t take=n-at;if(take>RB_PAGE)take=RB_PAGE;
        broker_header(body,at,n);w32_copy(body+32,(const uint8_t *)in+at,take);
        e=broker_rpc(RB_WIN32_PART,body,32+take,reply,32,&bytes);at+=take;
    }
    if(!e){broker_header(body,0,n);e=broker_rpc(RB_WIN32_COMMIT,body,32,reply,32,&bytes);}
    uint32_t total=!e&&bytes==32?br32(reply+12):0;
    if(!e&&(bytes!=32||br32(reply)!=1||total<sizeof(struct w32r_reply)||total>cap))e=13;
    for(uint32_t at=0;!e&&at<total;){uint32_t take=total-at;if(take>RB_PAGE)take=RB_PAGE;
        broker_header(reply,at,total);e=broker_rpc(RB_WIN32_READ,reply,32,body,RB_BODY_MAX,&bytes);
        if(!e&&(bytes!=32+take||br32(body)!=1||br32(body+8)!=at||br32(body+12)!=total))e=13;
        if(!e)w32_copy((uint8_t *)out+at,body+32,take);at+=take;
    }
    if(e)broker_close();else *used=total;RtlFreeHeap(heap,0,body);return e;
}
