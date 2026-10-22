/* Authored hostile host fixtures; no vendor code. SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "src/kernel/ndis/userhost.h"
#include "src/kernel/arch/vga_io.h"
static struct ntimage image;
static struct ndis_user_host h;
static unsigned order, fenced, retained, cancelled, detached, maps, records, notifications;
static bool ownership, masker, map_ok;
static int cause;
static bool owned(void *c,uint32_t g) { (void)c;return ownership && g==77; }
static bool mapping(void *c,const struct ndis_host_map *m)
{ (void)c; return map_ok && m->physical==0xf1000000 && m->bytes==4096; }
static int map(void *c,const struct ndis_host_map *m,bool install)
{ (void)c;(void)m; if(install) maps++;else { assert(retained); maps--; }return 0; }
static bool mask(void *c,bool masked) { (void)c; (void)masked;order++;return masker; }
static int pending(void *c) { (void)c;return cause; }
static void notify(void *c) { (void)c;notifications++; }
static bool fence(void *c) { (void)c;assert(h.state==NDIS_HOST_FENCING && !h.generation); fenced=++order;return false; }
static void detach(void *c,uint32_t g) { (void)c;assert(g==77 && fenced); detached=++order; }
static void retain(void *c) { (void)c;assert(detached>fenced);retained=++order; }
static void cancel(void *c) { (void)c;assert(retained && h.state==NDIS_HOST_QUARANTINED); cancelled=++order; }
static void record(void *c,const char *why,uint32_t vector,uint32_t offset)
{ (void)c;(void)vector;(void)offset;assert(why);records++; }
static struct ndis_host_ops ops(void)
{ return (struct ndis_host_ops){0,owned,mapping,map,mask,pending,notify,fence,detach,retain,cancel,record}; }
static void setup(void)
{
    h=(struct ndis_user_host){0};
    image=(struct ntimage){.loaded=true,.platform={.user_host=true},
        .pe={.base=0x100000,.size=8192,.section_count=1,
        .sections={{.rva=4096,.span=4096,.flags=0x60000020}}}};
    order=fenced=retained=cancelled=detached=maps=records=notifications=0;
    ownership=masker=map_ok=true;cause=0;
    struct ndis_host_ops o=ops();
    assert(!ndis_user_host_available());
    assert(!ndis_host_init(&h,&image,77,&o,true,false,5));
    assert(ndis_host_init(&h,&image,77,&o,true,true,5));
}
static void ports(void)
{
    setup(); uint8_t bitmap[ARCH_IO_BYTES];
    h.ports[0]=(struct arch_io_range){0xc001,0xc011}; h.port_count=1;
    ndis_host_ports(&h,bitmap);
    for(unsigned i=0;i<65536;i++) assert(!!(bitmap[i/8]&(1u<<(i%8)))==!(i>=0xc001 && i<0xc011));
    assert(bitmap[8192]==0xff);
    ownership=false; ndis_host_ports(&h,bitmap);
    for(unsigned i=0;i<ARCH_IO_BYTES;i++) assert(bitmap[i]==0xff);
    struct arch_io_range bad[]={{65535,65537}};
    assert(!arch_io_bitmap(bitmap,bad,1));
    for(unsigned i=0;i<ARCH_IO_BYTES;i++) assert(bitmap[i]==0xff);
    struct arch_io_range last={65535,65536};
    assert(arch_io_bitmap(bitmap,&last,1));assert(bitmap[8191]==0x7f && bitmap[8192]==0xff);
    assert(arch_io_bitmap(bitmap,0,0));arch_vga_bitmap(bitmap,true);
    assert(bitmap[0xc000/8]==0xff && !bitmap[0x3b0/8]);
    assert(arch_io_bitmap(bitmap,0,0));assert(bitmap[0x3b0/8]==0xff);
}
static void memory_stop(void)
{
    setup();struct ndis_host_map m={.va=0x10000,.physical=0xf1000000,.bytes=4096,
        .generation=77,.kind=NDIS_HOST_BAR,.uncached=true};
    m.bytes=4095;assert(!ndis_host_mapping(&h,&m));m.bytes=4096;
    m.physical++;assert(!ndis_host_mapping(&h,&m));m.physical--;
    m.generation++;assert(!ndis_host_mapping(&h,&m));m.generation--;
    m.uncached=false;assert(!ndis_host_mapping(&h,&m));m.uncached=true;
    m.va=0xbffff000;m.bytes=8192;assert(!ndis_host_mapping(&h,&m));m.va=0x10000;m.bytes=4096;
    map_ok=false;assert(!ndis_host_mapping(&h,&m));map_ok=true;
    assert(ndis_host_mapping(&h,&m));assert(maps==1);
    assert(!ndis_host_mapping(&h,&m));
    ndis_host_stop(&h,"host-fault",14,0x101011,10);
    assert(fenced<detached && detached<retained && retained<cancelled && !maps);
    assert(!h.generation && h.retired_generation==77 && h.fault_offset==0x1011);
    assert(image.quarantined && !h.idle_proved && records==1);
    ndis_host_stop(&h,"duplicate",0,0,11);assert(records==1);
    struct ndis_user_host replacement={0};struct ndis_host_ops o=ops();
    assert(!ndis_host_init(&replacement,&image,77,&o,false,false,5));
    assert(!ndis_host_mapping(&h,&m));
}
static void deadlines(void)
{
    const uint64_t limits[]={5000,50000,500000,10000000};
    for(unsigned kind=0;kind<4;kind++) for(unsigned running=0;running<2;running++) {
        setup();uint32_t token;struct ndis_host_event e;
        assert(ndis_host_enqueue(&h,(enum ndis_host_kind)kind,0x101000,0,100,&token));
        if(running) assert(ndis_host_take(&h,100,&e));
        ndis_host_watchdog(&h,99+limits[kind]);assert(ndis_host_live(&h));
        ndis_host_watchdog(&h,100+limits[kind]);assert(h.state==NDIS_HOST_QUARANTINED && cancelled);
        assert(!ndis_host_complete(&h,77,token,101+limits[kind]));
    }
    setup();uint32_t a,b;struct ndis_host_event e;
    assert(ndis_host_enqueue(&h,NDIS_HOST_DPC,0x101000,0,0,&a));
    assert(ndis_host_take(&h,0,&e) && h.irql==2);
    assert(ndis_host_enqueue(&h,NDIS_HOST_ISR,0x101001,0,49000,&b));
    h.synchronized=true;assert(!ndis_host_take(&h,49000,&e));h.synchronized=false;
    h.virtual_if=false;assert(!ndis_host_take(&h,49000,&e));h.virtual_if=true;
    assert(ndis_host_take(&h,49000,&e) && e.deadline==50000 && h.irql==5);
    assert(!ndis_host_complete(&h,77,a,49001));
    assert(ndis_host_complete(&h,77,b,49001) && h.irql==2);
    assert(ndis_host_complete(&h,77,a,49002) && !h.depth && !h.irql);
    setup();ownership=false;ndis_host_watchdog(&h,0);assert(cancelled);
    h.syscall_active=true;ndis_host_watchdog(&h,1000);assert(h.cancellation_stalled && records==2);
    ndis_host_watchdog(&h,1001);assert(records==2);
    setup();h.next_token=UINT32_MAX;assert(!ndis_host_enqueue(&h,NDIS_HOST_ISR,0x101000,0,0,&a));
}
static void interrupts(void)
{
    setup();assert(ndis_host_interrupt(&h,100));unsigned token=h.irq_token;
    assert(h.irq_masked && h.irq_pending && h.irq_deadline==5100);
    assert(ndis_host_interrupt(&h,200) && h.irq_token==token && h.irq_deadline==5100);
    assert(!ndis_host_rearm(&h,78,token,200));assert(!ndis_host_rearm(&h,77,token+1,200));
    cause=1;assert(ndis_host_rearm(&h,77,token,200) && h.irq_masked && h.irq_deadline==5100);
    cause=0;assert(ndis_host_rearm(&h,77,token,200) && !h.irq_masked && !h.irq_pending);
    assert(!ndis_host_rearm(&h,77,token,200));
    assert(ndis_host_interrupt(&h,300) && h.irq_token!=token);
    cause=-1;assert(!ndis_host_rearm(&h,77,h.irq_token,301) && cancelled);
    setup();masker=false;assert(!ndis_host_interrupt(&h,0) && cancelled);
    setup();assert(ndis_host_interrupt(&h,0));ndis_host_watchdog(&h,5000);assert(cancelled);
}
static void flags(void)
{
    setup();struct trap_frame tf={.vector=13,.cs=0x1b,.eip=0x101000,.eflags=0x202};
    assert(ndis_host_gp(&h,&tf,0xfa)==1 && !h.virtual_if && (tf.eflags&0x200));
    assert(!(ndis_host_flags(&h,tf.eflags)&0x200));
    assert(ndis_host_gp(&h,&tf,0xf4)==2 && !h.virtual_if && (tf.eflags&0x200));
    assert(ndis_host_gp(&h,&tf,0xfb)==1 && h.virtual_if && !(tf.eflags&0x3000));
    assert(ndis_host_restore_flags(&h,2) && !h.virtual_if);
    assert(!ndis_host_restore_flags(&h,0x3202) && !h.virtual_if);
    assert(ndis_host_restore_flags(&h,0x202) && h.virtual_if);
    assert(!ndis_host_gp(&h,&tf,0x9d)); /* POPFD does not trap; never fake it */
    tf.err=1;assert(!ndis_host_gp(&h,&tf,0xfa));tf.err=0;
    tf.cs=8;assert(!ndis_host_gp(&h,&tf,0xfa));
}
static void dispatch(void)
{
    setup();h.dispatcher=0x800000;h.stack_bytes=4096;
    for(unsigned i=0;i<NDIS_HOST_DEPTH;i++) h.dispatch_stack[i]=0x900000+i*8192;
    struct trap_frame tf={.cs=0x1b,.user_ss=0x23,.eip=0x700000,.user_esp=0x710000,.eflags=0x202};
    struct trap_frame idle=tf;uint32_t outer,inner;
    assert(ndis_host_enqueue(&h,NDIS_HOST_DPC,0x101000,33,0,&outer));
    assert(ndis_host_deliver(&h,&tf,0));
    assert(tf.eip==h.dispatcher && tf.user_esp==0x900000 && tf.eax==0x101000 &&
           tf.ebx==33 && tf.ecx==outer && tf.edx==77 && h.depth==1);
    tf.eip=0x101011;tf.user_esp-=100;struct trap_frame interrupted=tf;
    assert(ndis_host_enqueue(&h,NDIS_HOST_ISR,0x101022,44,49000,&inner));
    h.virtual_if=false;assert(!ndis_host_deliver(&h,&tf,49000));h.virtual_if=true;
    assert(ndis_host_deliver(&h,&tf,49000) && h.depth==2 && h.active[1].deadline==50000);
    assert(tf.user_esp==0x902000 && tf.ecx==inner);
    assert(!ndis_host_return_event(&h,&tf,77,outer,49001));
    tf.eip=0xc1000000;tf.user_esp=UINT32_MAX; /* not a trusted return context */
    assert(ndis_host_return_event(&h,&tf,77,inner,49001));
    assert(!memcmp(&tf,&interrupted,sizeof(tf)) && h.depth==1 && h.active[0].deadline==50000);
    assert(!ndis_host_return_event(&h,&tf,77,inner,49002));
    assert(ndis_host_return_event(&h,&tf,77,outer,49002));
    assert(!memcmp(&tf,&idle,sizeof(tf)) && !h.depth && !h.continuation_valid[0]);
}
int main(void)
{
    ports();memory_stop();deadlines();interrupts();flags();dispatch();
    puts("PASS ND14 host permissions, deadlines, nested scheduling, flags, rearm, fence-before-cancel");
    fflush(stdout);return 0;
}
