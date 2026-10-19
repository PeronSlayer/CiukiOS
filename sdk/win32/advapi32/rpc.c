/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#include <w32/ntdll.h>
#include <ciuki/abi.h>
/* Uses the frozen builtin gate, not a Linux host syscall or kernel pointer. */
extern uint32_t w32r_gate(uint32_t,uint32_t,uint32_t,uint32_t);
static uint32_t serial;
void w32r_pause(void){w32r_gate(CIUKI_SYS_SLEEP_MS,1,0,0);}
void w32r_last_error(uint32_t e){NtCurrentTeb()->LastErrorValue=e;}
static uint32_t packet(uint32_t endpoint,const struct w32r_packet *in,struct w32r_packet *out)
{
    struct ciuki_message m;w32_fill(&m,0,sizeof(m));m.length=256;w32_copy(m.data,in,256);
    uint32_t r=w32r_gate(CIUKI_SYS_CHANNEL_SEND,endpoint,(uint32_t)(uintptr_t)&m,0);
    if((int32_t)r<0)return 1062;
    r=w32r_gate(CIUKI_SYS_CHANNEL_RECV,endpoint,(uint32_t)(uintptr_t)&m,0);
    if(r!=1||m.length!=256||m.fd_count)return 1062;
    w32_copy(out,m.data,256);
    if(out->version!=1||out->id!=in->id||out->kind!=in->kind)return 13;
    return out->error;
}
uint32_t w32r_exchange(const void *input,uint32_t n,void *output,uint32_t cap,uint32_t *used)
{
    struct w32r_service a={.version=1,.size=40,.operation=0x200,.action=2,.service=2};
    uint32_t status=w32r_gate(CIUKI_SYS_W32,(uint32_t)(uintptr_t)&a,sizeof(a),0);
    if(status||a.error||!a.result)return a.error?a.error:1062;
    uint32_t id=++serial;if(!id)id=++serial;
    struct w32r_packet in,out;uint32_t at=0,total=0;
    if(n>W32R_MAX)return 87;
    while(at<n){
        w32_fill(&in,0,sizeof(in));in.version=1;in.id=id;in.kind=at?W32R_WRITE:W32R_BEGIN;in.total=n;in.offset=at;
        uint32_t chunk=n-at;if(chunk>232)chunk=232;w32_copy(in.bytes,(const uint8_t *)input+at,chunk);
        uint32_t e=packet(a.endpoint,&in,&out);if(e)return e;
        at+=chunk;if(out.offset!=at)return 13;total=out.total;
    }
    if(total<sizeof(struct w32r_reply)||total>cap)return 13;
    at=0;
    while(at<total){
        w32_fill(&in,0,sizeof(in));in.version=1;in.id=id;in.kind=W32R_READ;in.total=total;in.offset=at;
        uint32_t e=packet(a.endpoint,&in,&out);if(e)return e;
        uint32_t chunk=total-at;if(chunk>232)chunk=232;
        if(out.total!=total||out.offset!=at+chunk)return 13;w32_copy((uint8_t *)output+at,out.bytes,chunk);at+=chunk;
    }
    *used=total;return 0;
}
