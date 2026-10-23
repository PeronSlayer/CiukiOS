/* The production SDK wrapper is extracted by the test runner.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <ciuki/abi.h>
#include "../../apps/desktop/protocol.h"
static unsigned calls;
static int mock_call(int fd,uintptr_t request,uintptr_t output)
{
    const struct ciuki_accel_request *q=(const void *)request;
    assert(fd==7 && q->size==308 && q->operation==CIUKI_ACCEL_QUERY_ENGINE && q->generation==42);
    assert(q->colour==1 && q->background==sizeof(struct ciuki_display_engine_info)); calls++;
    struct ciuki_display_engine_info *out=(void *)output;
    memset(out,0,sizeof(*out)); out->size=sizeof(*out); out->version=1; strcpy(out->driver_id,"roundtrip");
    return 0;
}
#define CU_PTR(p) ((uintptr_t)(p))
#define CU_CALL(n,a,b,c,d,e,f) mock_call(a,b,c)
#include "display_query_wrapper.inc"
int main(void)
{
    struct { unsigned guard; struct ciuki_display_engine_info info; unsigned tail; } out;
    memset(&out,0,sizeof(out)); out.guard=0xabc123; out.tail=0xdef456;
    out.info.version=1; out.info.size=sizeof(out.info);
    assert(!ciuki_display_engine_info(7,42,&out.info) && calls==1 && out.guard==0xabc123 && out.tail==0xdef456);
    assert(ciuki_display_engine_info(7,42,NULL)==-EINVAL && calls==1);
    struct ciuki_message m; struct desk_packet p;
    desk_message(&m,DESK_GFX_QUERY_ENGINE,12); m.data[0]=DESK_SHELL_VERSION; desk_put32(m.data+8,1);
    assert(desk_parse(&m,1,NULL,&p) && p.opcode==DESK_GFX_QUERY_ENGINE);
    desk_put32(m.data+8,2); assert(!desk_parse(&m,1,NULL,&p));
    desk_message(&m,DESK_GFX_INFO_ENGINE,256); m.data[0]=DESK_SHELL_VERSION; desk_put32(m.data+4,77); desk_put32(m.data+12,1024);
    out.info.kind=CIUKI_ENGINE_NATIVE; out.info.capabilities=3; out.info.software_operations=15;
    out.info.ready=out.info.native_used=out.info.cpu_ready=1; out.info.generation=42;
    strcpy(out.info.display_name,"Round trip adapter"); strcpy(out.info.reason,"none");
    out.info.native[0]=0x100000001ull; out.info.software[3]=9;
    memcpy(m.data+24,&out.info,sizeof(out.info));
    assert(desk_parse(&m,0,NULL,&p) && p.serial==77 && p.width==1024 && !memcmp(&p.engine,&out.info,sizeof(out.info)));
    assert(p.accelerator.operations==0x100000001ull && p.accelerator.fallbacks==9);
    m.length=255; assert(!desk_parse(&m,0,NULL,&p)); m.length=256;
    m.data[24]=2; assert(!desk_parse(&m,0,NULL,&p));
    desk_message(&m,DESK_GFX_INFO,64); m.data[0]=DESK_SHELL_VERSION; desk_put32(m.data+24,1); desk_put32(m.data+28,3);
    assert(desk_parse(&m,0,NULL,&p) && p.accelerator.driver==1 && sizeof(p.accelerator)==40);
    printf("display query: SDK request, bounded canaries, v1 generic payload, 64-bit counters, malformed version/size, legacy 64-byte packet PASS\n");
    return 0;
}
