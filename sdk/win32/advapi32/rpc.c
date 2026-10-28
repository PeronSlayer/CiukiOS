/* Shared per-process channel transport lives in ntdll.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#include <w32/ntdll.h>
#include <ciuki/abi.h>
extern uint32_t w32r_gate(uint32_t,uint32_t,uint32_t,uint32_t);
void w32r_pause(void){w32r_gate(CIUKI_SYS_SLEEP_MS,1,0,0);}
void w32r_last_error(uint32_t e){NtCurrentTeb()->LastErrorValue=e;}
uint32_t w32r_exchange(const void *in,uint32_t n,void *out,uint32_t cap,uint32_t *used) {
    return w32_server_exchange(in,n,out,cap,used);
}
void *w32r_alloc(uint32_t n) {
    void *heap=NtCurrentTeb()->ProcessEnvironmentBlock->ProcessHeap;
    return heap?RtlAllocateHeap(heap,0,n?n:1):0;
}
void w32r_free(void *p) {
    if(p)RtlFreeHeap(NtCurrentTeb()->ProcessEnvironmentBlock->ProcessHeap,0,p);
}
/* --- sec-user-05: identity transports (identity.c) --- */
#include <ciuki/w32_transport.h>
int w32r_release(void *p) {
    return p && RtlFreeHeap(NtCurrentTeb()->ProcessEnvironmentBlock->ProcessHeap,0,p);
}
uint32_t w32r_object(struct ciuki_w32_object_args *a,void *buffer)
{
    a->header=(struct ciuki_w32_header){1,sizeof(*a),W32_OBJECT,0};
    if(buffer)a->name=(uint32_t)(uintptr_t)buffer;
    int32_t e=(int32_t)w32r_gate(CIUKI_SYS_W32,(uint32_t)(uintptr_t)a,sizeof(*a),0);
    if(e<0)return e==-12?8u:e==-14?998u:87u;
    return a->error;
}
/* ntdll keeps the process's one REGISTRY link; RB_MACHINE_ID never opens a
 * second one (the broker serves at most RB_LINK_MAX links). */
extern uint32_t w32_registry_machine(uint32_t out[8]);
uint32_t w32r_machine(uint32_t sid[3])
{
    uint32_t r[8];uint32_t e=w32_registry_machine(r);
    if(e)return e;
    if(r[0]!=1)return 13;
    sid[0]=r[1];sid[1]=r[2];sid[2]=r[3];return 0;
}
/* ACCOUNTS (service 2) over a private link closed before returning: the
 * frozen CSC1 framing (64-byte header, at most 192 body bytes per message,
 * 8192 per logical body), sender and endpoint generation checked on every
 * reply. A service status is returned as -errno; a broken link is -EPIPE. */
static uint32_t accounts_serial;
static uint32_t le32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
int32_t w32r_accounts(uint32_t op,const void *in,uint32_t n,void *out,uint32_t cap,uint32_t *used)
{
    *used=0;
    if(n>CIUKI_SECURITY_SERVICE_BODY_MAX||(!in&&n)||(!out&&cap))return -22;
    struct ciuki_security_link q,r;w32_fill(&q,0,sizeof(q));w32_fill(&r,0,sizeof(r));
    q.version=1;q.size=sizeof(q);q.service=CIUKI_SECURITY_SERVICE_ACCOUNTS;
    struct ciuki_security_args a;w32_fill(&a,0,sizeof(a));
    a.version=CIUKI_SECURITY_VERSION;a.size=sizeof(a);a.operation=CIUKI_SECURITY_SERVICE_CONNECT;
    a.spawn_address=(uint32_t)(uintptr_t)&q;a.output_address=(uint32_t)(uintptr_t)&r;a.output_bytes=sizeof(r);
    int32_t e=(int32_t)w32r_gate(CIUKI_SYS_SECURITY,(uint32_t)(uintptr_t)&a,sizeof(a),0);
    if(e<0)return e;
    if(r.endpoint_fd<0)return -32;
    uint32_t fd=(uint32_t)r.endpoint_fd,id=__atomic_add_fetch(&accounts_serial,1,__ATOMIC_RELAXED);
    int32_t status=-32;
    if(r.server_role!=CIUKI_SECURITY_ROLE_ACCOUNT||!r.endpoint_generation)goto done;
    uint32_t at=0;
    do{
        struct ciuki_message m;w32_fill(&m,0,sizeof(m));uint8_t *h=m.data;uint32_t take=n-at;
        if(take>CIUKI_SECURITY_SERVICE_FRAGMENT_MAX)take=CIUKI_SECURITY_SERVICE_FRAGMENT_MAX;
        w32_put32(h,0x31435343u);w32_put32(h+4,1);w32_put32(h+8,64);w32_put32(h+12,CIUKI_SECURITY_SERVICE_ACCOUNTS);
        w32_put32(h+16,op);w32_put32(h+20,1);w32_put32(h+24,id);w32_put32(h+32,r.endpoint_generation);
        w32_put32(h+36,take);w32_put32(h+52,n);w32_put32(h+56,at);
        if(take)w32_copy(h+64,(const uint8_t *)in+at,take);
        m.length=64+take;
        if((int32_t)w32r_gate(CIUKI_SYS_CHANNEL_SEND,fd,(uint32_t)(uintptr_t)&m,0)<0)goto done;
        at+=take;
    }while(at<n);
    uint32_t total=0;at=0;
    do{
        struct ciuki_message m;w32_fill(&m,0,sizeof(m));
        if((int32_t)w32r_gate(CIUKI_SYS_CHANNEL_RECV,fd,(uint32_t)(uintptr_t)&m,0)!=1)goto done;
        if(m.fd_count){for(unsigned i=0;i<m.fd_count&&i<4;i++)w32r_gate(CIUKI_SYS_CLOSE,(uint32_t)m.fds[i],0,0);goto done;}
        const uint8_t *h=m.data;uint32_t take=le32(h+36),length=le32(h+52);
        if(m.sender_pid!=r.peer_pid||m.length<64||le32(h)!=0x31435343u||le32(h+4)!=1||le32(h+8)!=64||
           le32(h+12)!=CIUKI_SECURITY_SERVICE_ACCOUNTS||le32(h+16)!=op||le32(h+20)!=2||le32(h+24)!=id||le32(h+28)||
           le32(h+32)!=r.endpoint_generation||take>CIUKI_SECURITY_SERVICE_FRAGMENT_MAX||m.length!=64+take||
           length>CIUKI_SECURITY_SERVICE_BODY_MAX||length>cap||le32(h+56)!=at||le32(h+60))goto done;
        int32_t s=(int32_t)le32(h+48);
        if(s){status=s<0&&!at&&!length&&!take?s:-32;goto done;}
        if(!at)total=length;else if(total!=length)goto done;
        if(take>total-at||(!take&&total))goto done;
        if(take)w32_copy((uint8_t *)out+at,h+64,take);
        at+=take;
    }while(at<total);
    *used=at;status=0;
done:
    w32r_gate(CIUKI_SYS_CLOSE,fd,0,0);
    return status;
}
/* --- end sec-user-05 --- */
