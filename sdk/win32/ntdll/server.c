/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/server.h>
#include <w32/ntdll.h>
#include <ciuki/abi.h>
/* Uses the frozen builtin gate, not a Linux host syscall or kernel pointer. */
static uint32_t serial;
static uint32_t endpoint=UINT32_MAX,connected;
static uint32_t w32r_gate(uint32_t op,uintptr_t a,uintptr_t b,uintptr_t c) {
#ifdef W32_HOST_MODEL
    extern uint32_t w32r_host_gate(uint32_t,uintptr_t,uintptr_t,uintptr_t) __attribute__((weak));
    return w32r_host_gate?w32r_host_gate(op,a,b,c):(uint32_t)-38;
#else
    uint32_t result;
    __asm__ volatile("pushl %%ebp; xorl %%ebp,%%ebp; int $0x80; popl %%ebp"
        : "=a"(result) : "a"(op),"b"(a),"c"(b),"d"(c),"S"(0),"D"(0) : "memory","cc");
    return result;
#endif
}
static void disconnect(void)
{
    if(endpoint!=UINT32_MAX)w32r_gate(CIUKI_SYS_CLOSE,endpoint,0,0);
    endpoint=UINT32_MAX;connected=0;
}
static uint32_t packet(uint32_t fd,const struct w32r_packet *in,struct w32r_packet *out,int *unsent)
{
    struct ciuki_message m;w32_fill(&m,0,sizeof(m));m.length=256;w32_copy(m.data,in,256);
    uint32_t r=w32r_gate(CIUKI_SYS_CHANNEL_SEND,fd,(uintptr_t)&m,0);
    if((int32_t)r<0){if(unsent)*unsent=1;return 1062;}
    r=w32r_gate(CIUKI_SYS_CHANNEL_RECV,fd,(uintptr_t)&m,0);
    if(r!=1||m.length!=256||m.fd_count)return 1062;
    w32_copy(out,m.data,256);
    if(out->version!=1||out->id!=in->id||out->kind!=in->kind)return 13;
    return out->error;
}
static uint32_t exchange(const void *input,uint32_t n,void *output,uint32_t cap,uint32_t *used,int *unsent)
{
    uint32_t id=++serial;if(!id)id=++serial;
    struct w32r_packet in,out;uint32_t at=0,total=0;
    if(n>W32R_MAX)return 87;
    while(at<n){
        w32_fill(&in,0,sizeof(in));in.version=1;in.id=id;in.kind=at?W32R_WRITE:W32R_BEGIN;in.total=n;in.offset=at;
        uint32_t chunk=n-at;if(chunk>232)chunk=232;w32_copy(in.bytes,(const uint8_t *)input+at,chunk);
        uint32_t e=packet(endpoint,&in,&out,at?NULL:unsent);if(e)return e;
        at+=chunk;if(out.offset!=at)return 13;total=out.total;
    }
    if(total<sizeof(struct w32r_reply)||total>cap)return 13;
    at=0;
    while(at<total){
        w32_fill(&in,0,sizeof(in));in.version=1;in.id=id;in.kind=W32R_READ;in.total=total;in.offset=at;
        uint32_t e=packet(endpoint,&in,&out,NULL);if(e)return e;
        uint32_t chunk=total-at;if(chunk>232)chunk=232;
        if(out.total!=total||out.offset!=at+chunk)return 13;w32_copy((uint8_t *)output+at,out.bytes,chunk);at+=chunk;
    }
    *used=total;return 0;
}

uint32_t w32_server_exchange(const void *in,uint32_t n,void *out,uint32_t cap,uint32_t *used) {
    if(!in || !out || !used || n<sizeof(struct w32r_request)+2 || n>W32R_MAX)return 87;
    *used=0;
    RTL_CRITICAL_SECTION *lock=NtCurrentTeb()->ProcessEnvironmentBlock->FastPebLock;
    RtlEnterCriticalSection(lock);
    uint32_t e=0;
    for(unsigned attempt=0;attempt<2;attempt++){
        if(!connected){
            struct w32r_service a={.version=1,.size=40,.operation=0x200,.action=2,.service=2};
            e=w32r_gate(CIUKI_SYS_W32,(uintptr_t)&a,sizeof(a),0);
            if(e||a.error||!a.result){e=a.error?a.error:1062;break;}
            /* The kernel removes the tracked FD on service exit. Retain a
             * private CLOEXEC reference so unrelated FD reuse cannot redirect
             * an RPC; its old channel instead reports the closed peer. */
            endpoint=w32r_gate(CIUKI_SYS_FCNTL,a.endpoint,F_DUPFD_CLOEXEC,0);
            if((int32_t)endpoint<0){endpoint=UINT32_MAX;e=8;break;}
            struct {struct w32r_request r;char empty[2];} hello={.r={.version=1,.op=W32R_CLIENT,.path_bytes=1,.name_bytes=1}};
            struct w32r_reply reply;uint32_t count=0;
            e=exchange(&hello,sizeof(hello.r)+2,&reply,sizeof(reply),&count,NULL);
            if(!e)e=count==sizeof(reply)&&reply.version==1?reply.error:13;
            if(e){disconnect();break;}
            connected=1;
        }
        int unsent=0;e=exchange(in,n,out,cap,used,&unsent);
        if(!e)break;
        disconnect();
        /* Only a failed first send proves no mutation reached the server.
         * A lost reply or a partially written request is never replayed.
         * The retained reference detects restart even when the kernel's
         * tracked descriptor has already been reused by another operation. */
        if(!unsent)break;
    }
    RtlLeaveCriticalSection(lock);return e;
}
