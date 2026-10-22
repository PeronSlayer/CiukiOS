/* NT5 ownership/priority/service semantics on synthetic resident memory.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "src/kernel/ndis/nt_runtime.h"
#include "src/kernel/ndis/nt_services.h"
#include "src/kernel/ndis/user_memory.h"
static struct ndis_nt_runtime r;
static struct ndis_user_host h;
static struct ntimage image;
static uint8_t memory[65536];
static uint64_t now,wall;
static unsigned notices;
static bool wait_signal;
static struct mm_pin pin,dma_pins[64];
static bool dma_live[64];static uint32_t next_dma_ticket;
int mm_pin(struct mm_pin *p)
{ for(unsigned i=0;i<64;i++) if(!dma_live[i]) { p->ticket=next_dma_ticket++;dma_pins[i]=*p;dma_live[i]=true;return 0; }return -1; }
int mm_unpin(struct mm_pin *p,uint32_t generation)
{ if(generation!=7) return -1;
  for(unsigned i=0;i<64;i++) if(dma_live[i] && !memcmp(p,&dma_pins[i],sizeof(*p))) { dma_live[i]=false;return 0; }return -1; }
static struct ndis_nt_services services;
static struct ndis_user_export veneers[NDIS_NT_EXPORTS];
static uint8_t veneer_page[4096];static unsigned veneer_count,irp_completions;
static uint64_t call(const char *name,const uint32_t *args,unsigned count)
{
    for(unsigned i=0;i<veneer_count;i++) if(!strcmp(veneers[i].symbol->name,name)) {
        uint32_t offset=veneers[i].address-0x500000;
        memcpy(memory+0x6000,args,count*4);
        return ndis_nt_service(&services,pe_u32(veneer_page+offset+6),0x206000);
    }
    assert(0);return 0;
}
static uint32_t port(void *ctx,uint32_t address,unsigned width,bool write,uint32_t value,bool *ok)
{ (void)ctx;(void)value;*ok=address==0x3000 && width==2;return write?0:0x1234; }
static bool mmio(void *ctx,uint32_t address,unsigned width,uint32_t value)
{ (void)ctx;return address==0x400000 && width==4 && value==123; }
static int stall(void *ctx,uint32_t us) { (void)ctx;return us<=50?0:-1; }
static void irp_done(void *ctx,int32_t status,uint32_t bytes)
{ assert(ctx==&services && status==0 && bytes==19);irp_completions++; }
bool mm_pin_valid(const struct mm_pin *p,uint32_t gen)
{ if(gen!=7) return false;if(!memcmp(p,&pin,sizeof(pin))) return true;
  for(unsigned i=0;i<64;i++) if(dma_live[i] && !memcmp(p,&dma_pins[i],sizeof(*p))) return true;return false; }
static bool owned(void *c,uint32_t gen) { (void)c;return gen==7; }
static bool mask(void *c,bool on) { (void)c;(void)on;return true; }
static bool fence(void *c) { (void)c;return false; }
static void simple(void *c) { (void)c; }
static void retain(void *c) { (void)c;ndis_nt_stop(&r); }
static void detach(void *c,uint32_t gen) { (void)c;(void)gen; }
static void record(void *c,const char *s,uint32_t v,uint32_t e) { (void)c;(void)s;(void)v;(void)e; }
static int rd(void *c,uint32_t address,void *out,uint32_t bytes)
{ (void)c;if(address<0x200000 || address-0x200000>sizeof(memory) || bytes>sizeof(memory)-(address-0x200000)) return 0;
  memcpy(out,memory+(address-0x200000),bytes);return 1; }
static int wr(void *c,uint32_t address,const void *in,uint32_t bytes)
{ (void)c;if(address<0x200000 || address-0x200000>sizeof(memory) || bytes>sizeof(memory)-(address-0x200000)) return 0;
  memcpy(memory+(address-0x200000),in,bytes);return 1; }
static uint64_t clock_now(void *c) { (void)c;return now; }
static uint64_t clock_wall(void *c) { (void)c;return wall; }
static int queue(void *c,uint32_t fn,unsigned argc,const uint32_t *args,uint32_t *token)
{ (void)c;assert(argc==4 && args);return ndis_host_enqueue(&h,NDIS_HOST_DPC,fn,args[0],now/10,token); }
static bool cancel(void *c,uint32_t token) { (void)c;return ndis_host_cancel_queued(&h,7,token); }
static void wake(void *c) { (void)c;notices++; }
static bool wait(void *c,bool (*pred)(void *),void *ctx,uint64_t due)
{
    (void)c;if(pred(ctx)) return true;
    if(wait_signal) { int32_t old;assert(ndis_nt_event_set(&r,0x200100,1,&old));return pred(ctx); }
    now=due;return false;
}
static void setup(void)
{
    memset(memory,0,sizeof(memory));memset(dma_live,0,sizeof(dma_live));next_dma_ticket=100;now=10000;wall=116444736000000000ull;notices=0;wait_signal=false;
    pin=(struct mm_pin){.generation=7,.first_pfn=128,.pages=16,.ticket=42};
    image=(struct ntimage){.loaded=true,.platform={.user_host=true},.pe={.base=0x100000,.size=4096,
        .section_count=1,.sections={{.span=4096,.flags=0x60000000}}}};
    h=(struct ndis_user_host){.image=&image,.generation=7,.state=NDIS_HOST_LIVE,.dirql=5,.virtual_if=true,
        .ops={.owned=owned,.mask=mask,.fence=fence,.detach=detach,.retain=retain,.cancel=simple,.record=record,.notify=simple}};
    struct ndis_nt_ops ops={0,rd,wr,clock_now,clock_wall,queue,cancel,wait,wake};
    assert(ndis_nt_init(&r,&h,&ops,0x200000,sizeof(memory),&pin,0));h.nt=&r;
    uint32_t token;struct ndis_host_event e;assert(ndis_host_enqueue(&h,NDIS_HOST_PASSIVE,0x100000,0,now/10,&token));
    assert(ndis_host_take(&h,now/10,&e));
}
static void service_tests(void)
{
    setup();services=(struct ndis_nt_services){.runtime=&r,.port=port,.mmio=mmio,.stall=stall};
    assert(ndis_nt_veneers(veneer_page,sizeof(veneer_page),0x500000,0x501000,veneers,32,&veneer_count) && veneer_count==32);
    for(unsigned i=0;i<veneer_count;i++) {
        assert(veneers[i].address<0xc0000000 && veneers[i].symbol->implemented && !veneers[i].symbol->ordinal);
        if(veneers[i].symbol->kind==NDIS_DATA) assert(veneers[i].address==0x501000 && veneers[i].bytes==12);
        else assert(veneer_page[veneers[i].address-0x500000]==0x53);
        for(unsigned j=0;j<i;j++) assert(strcmp(veneers[i].symbol->name,veneers[j].symbol->name));
    }
    uint32_t args[6]={0};assert(call("KeQueryTimeIncrement",args,0)==10000 && call("KeGetCurrentIrql",args,0)==0);
    args[0]=0x203000;assert(call("KeInitializeSpinLock",args,1)==0);
    assert(call("KfAcquireSpinLock",args,1)==0 && h.irql==2);args[1]=0;
    assert(call("KfReleaseSpinLock",args,2)==0 && h.irql==0);
    args[0]=0xffffffeb;args[1]=UINT32_MAX;args[2]=7;args[3]=0;
    assert(call("_alldiv",args,4)==(uint64_t)(int64_t)-3);
    assert(call("_allmul",args,4)==(uint64_t)(int64_t)-147);
    args[0]=0x203008;args[1]=0x203100;assert(call("InterlockedPushEntrySList",args,2)==0);
    assert(call("InterlockedPopEntrySList",args,1)==0x203100);
    assert(call("InterlockedPopEntrySList",args,1)==0);
    memcpy(memory+0x3100,"abc",4);args[0]=0x203200;args[1]=0x203100;args[2]=8;
    assert(call("strncpy",args,3)==0x203200 && !memcmp(memory+0x3200,"abc\0\0\0\0\0",8));
    args[0]=0x3000;assert(call("READ_PORT_USHORT",args,1)==0x1234);
    args[0]=0x400000;args[1]=123;assert(call("WRITE_REGISTER_ULONG",args,2)==0);
    args[0]=50;assert(call("KeStallExecutionProcessor",args,1)==0);
    uint32_t status[2]={0,19};memcpy(memory+0x3300,status,8);irp_completions=0;
    assert(ndis_nt_irp_register(&services,0x203400,0x203300,64,&services,irp_done));
    args[0]=0x203400;args[1]=0;assert(call("IofCompleteRequest",args,2)==0 && irp_completions==1);
    uint16_t path[]={'\\','x'};services.file[0].present=true;services.file[0].bytes=4;memcpy(services.file[0].name,path,4);
    memcpy(memory+0x3500,path,4);struct ndis_nt_string name={4,4,0x203500};memcpy(memory+0x3510,&name,8);
    uint32_t attributes[6]={24,0,0x203510,0,0,0};memcpy(memory+0x3520,attributes,24);
    args[0]=0x203540;args[1]=0x80000000;args[2]=0x203520;args[3]=0x203550;args[4]=1;args[5]=0;
    assert(call("ZwOpenFile",args,6)==0 && pe_u32(memory+0x3540)==1 && pe_u32(memory+0x3554)==1);
    args[0]=1;assert(call("ZwClose",args,1)==0 && call("ZwClose",args,1)==0xc0000008u);
    args[0]=0x203400;args[1]=0;assert(call("IofCompleteRequest",args,2)==NDIS_NT_INVALID && image.quarantined);
}
int main(int argc,char **argv)
{
    setup();uint8_t old;uint32_t a,b;
    assert(ndis_nt_lock_init(&r,0x200100));
    assert(ndis_nt_lock(&r,0x200100,false,&old) && old==0 && h.irql==2);
    assert(!ndis_host_complete(&h,7,h.active[0].token,now/10)); /* held lock cannot escape callback */
    assert(ndis_nt_unlock(&r,0x200100,false,old) && h.irql==0);
    assert(ndis_nt_raise(&r,2,&old));assert(ndis_nt_lock(&r,0x200100,true,&old));
    assert(ndis_nt_unlock(&r,0x200100,true,0));assert(ndis_nt_lower(&r,0));
    a=ndis_nt_pool(&r,0,4000,0x1234);b=ndis_nt_pool(&r,1,200,0x5678);
    assert(a==0x200000 && b==0x201000); /* small pool never crosses pages */
    uint32_t mdl=ndis_nt_mdl(&r,a+20,3800);assert(mdl && ndis_nt_map_mdl(&r,mdl,0,1,0)==a+20);
    memset(memory+(mdl-0x200000)+28,0xff,4); /* forged PFN never changes private ownership */
    assert(ndis_nt_map_mdl(&r,mdl,0,1,0)==a+20 && !ndis_nt_map_mdl(&r,mdl,0,0,0));
    assert(!ndis_nt_dma_reserve(&r,64,1514));
    assert(ndis_nt_dma_reserve(&r,4,4096));struct ndis_nt_segment segments[8];unsigned count=8;
    assert(ndis_nt_dma_start(&r,1,mdl,MM_PIN_TO_DEVICE,UINT32_MAX,1,1024,1024,segments,&count) && count==4);
    assert(segments[0].physical==128*4096+20 && segments[0].bytes==1004);
    count=8;assert(!ndis_nt_dma_start(&r,2,mdl,MM_PIN_TO_DEVICE,UINT32_MAX,1,0,4096,segments,&count));
    assert(!ndis_nt_dma_release(&r) && !ndis_nt_dma_complete(&r,1,8,r.dma[1].pin.ticket));
    assert(ndis_nt_dma_complete(&r,1,7,r.dma[1].pin.ticket) && !ndis_nt_dma_complete(&r,1,7,r.dma[1].pin.ticket) && ndis_nt_dma_release(&r));
    assert(ndis_nt_free_mdl(&r,mdl) && ndis_nt_free(&r,a) && ndis_nt_free(&r,b));
    assert(!ndis_nt_pool(&r,0,65537,1));
    assert(ndis_nt_dpc_init(&r,0x200200,0x100010,33));
    assert(ndis_nt_dpc_insert(&r,0x200200,1,2));assert(!ndis_nt_dpc_insert(&r,0x200200,1,2));
    assert(ndis_nt_dpc_remove(&r,0x200200));assert(!ndis_nt_dpc_remove(&r,0x200200));
    assert(ndis_nt_event_init(&r,0x200100,false,0,1));int32_t previous;int64_t timeout=0;
    assert(ndis_nt_wait(&r,0x200100,&timeout)==NDIS_NT_TIMEOUT);
    assert(ndis_nt_event_set(&r,0x200100,1,&previous) && previous==0);
    assert(ndis_nt_wait(&r,0x200100,&timeout)==NDIS_NT_SUCCESS);
    assert(ndis_nt_wait(&r,0x200100,&timeout)==NDIS_NT_TIMEOUT);
    wait_signal=true;timeout=-123;assert(ndis_nt_wait(&r,0x200100,&timeout)==NDIS_NT_SUCCESS);
    assert(ndis_nt_event_init(&r,0x200100,true,1,1));
    assert(ndis_nt_wait(&r,0x200100,&timeout)==0 && ndis_nt_wait(&r,0x200100,&timeout)==0);
    assert(ndis_nt_event_reset(&r,0x200100,&previous) && previous==1);
    assert(ndis_nt_timer_init(&r,0x200300));bool armed;
    assert(ndis_nt_timer_set(&r,0x200300,-10,2,0x200200,&armed) && !armed);
    now+=9;ndis_nt_poll(&r);assert(!h.count);now++;ndis_nt_poll(&r);assert(h.count==1);
    assert(ndis_nt_timer_cancel(&r,0x200300,&armed) && armed);
    assert(ndis_nt_dpc_remove(&r,0x200200));
    assert(ndis_nt_timer_set(&r,0x200300,(int64_t)wall+100,0,0x200200,&armed));
    now+=99;ndis_nt_poll(&r);assert(!h.count);now++;ndis_nt_poll(&r);assert(h.count==1);
    struct ndis_nt_string d={2,8,0x200500},s={4,4,0x200510};
    memcpy(memory+0x400,&d,8);memcpy(memory+0x410,&s,8);memcpy(memory+0x510,"A\0B\0",4);
    assert(ndis_nt_unicode_append(&r,0x200400,0x200410,true)==0);
    memcpy(&d,memory+0x400,8);assert(d.length==6 && !memcmp(memory+0x502,"A\0B\0",4));
    assert(ndis_nt_unicode_append(&r,0x200400,0x200410,true)==NDIS_NT_SMALL);
    assert(notices);setup();assert(ndis_nt_lock_init(&r,0x200100) && ndis_nt_lock(&r,0x200100,false,&old));
    assert(!ndis_nt_lock(&r,0x200100,false,&old) && image.quarantined && !r.lock[0].held && !h.irql);
    setup();a=ndis_nt_pool(&r,0,16,1);assert(ndis_nt_free(&r,a));assert(!ndis_nt_free(&r,a) && image.quarantined);
    setup();assert(ndis_nt_raise(&r,2,&old));assert(!ndis_nt_pool(&r,1,8,1) && image.quarantined);
    setup();assert(ndis_nt_raise(&r,2,&old));assert(ndis_nt_wait(&r,0x200100,0)==NDIS_NT_INVALID && image.quarantined);
    service_tests();
    if(argc==2) {
        FILE *out=fopen(argv[1],"wb");assert(out);
        assert(fwrite(veneer_page,1,31*32,out)==31*32 && !fclose(out));
    }
    puts("PASS NT priorities/locks, bounded pools, owned MDLs, DPC cancellation, waits, timers, UTF16 and quarantine");fflush(stdout);return 0;
}
