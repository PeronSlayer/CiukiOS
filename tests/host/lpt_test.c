/* Independent cable/register/8237 model; production LPT, native and INT17.
 * No emulator or physical qualification. SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#define CIUKI_CPU_H
#include <ciuki/kernel.h>
#include <ciuki/lpt.h>
#include <ciuki/task.h>
#include <ciuki/mm.h>
#include <ciuki/firmware.h>
static uint8_t ram[2*1024*1024];
#define P2V(p) ((void *)(ram+(p)))
#define V2P(p) ((uint32_t)((uint8_t *)(p)-ram))
static uint32_t irq_flags=0x200;
static uint32_t irq_save(void) { uint32_t f=irq_flags; irq_flags=0; return f; }
static void irq_restore(uint32_t f) { irq_flags=f; }
volatile uint64_t g_ticks;
struct ciuki_boot_info g_boot;
struct task *g_current;
struct model {
    uint16_t base, high;
    uint8_t data, control, ecr, fifo[32], epp_data, epp_address, external;
    unsigned caps, depth, head, used, ack, negotiation, outputs;
    bool busy, no_ack, stuck_fifo, epp_timeout, epp_sticky, epp_cycle_timeout, reject_negotiation, absent;
} hw[8];
static unsigned reads,writes,allocations,dma_count,dma_byte,dma_mask=15;
static bool bounce_allocated,dma_running,stop_fail,fail_alloc,delay_fail,dma_stall,fail_pin;
static unsigned dma_pins;
static bool irq_masked[16];
void pic_mask(unsigned irq) { assert(irq==5 || irq==7); irq_masked[irq]=true; }
static void (*sleep_hook)(void);
static struct resource claims[64];
static unsigned nclaims;
static struct model *model_port(uint16_t port, unsigned *r)
{
    for (unsigned i=0;i<8;i++) if (hw[i].base) {
        if (port>=hw[i].base && port<hw[i].base+8) { *r=port-hw[i].base; return &hw[i]; }
        if (hw[i].high && port>=hw[i].high && port<hw[i].high+3) { *r=port-hw[i].high+8; return &hw[i]; }
    }
    assert(!"unassigned I/O"); return 0;
}
static void check_claim(uint16_t port)
{
    for (unsigned i=0;i<nclaims;i++) if (claims[i].type==RES_PORT &&
        claims[i].start<=port && port<claims[i].end && claims[i].state!=RS_RELEASED &&
        claims[i].state!=RS_QUARANTINED) return;
    assert(!"I/O before claim or after revocation");
}
static uint8_t inb(uint16_t port)
{
    reads++;
    if (port<0x90) {
        assert(port!=0x0f && port!=8); /* write-only mask / destructive shared TC */
        if (port<8 && (port&1)) {
            uint8_t b=(uint8_t)(dma_count>>(dma_byte++%2*8));
            return b;
        }
        return 0;
    }
    check_claim(port);
    unsigned r; struct model *m=model_port(port,&r);
    if (m->absent) return 0xff;
    switch (r) {
    case 0: return m->control&0x20 && (m->caps&2) ? m->external : m->data;
    case 1:
        if (m->epp_timeout) return 0xd9;
        if (m->negotiation==1) return m->reject_negotiation ? 0xd8 : 0x38;
        if (m->negotiation==2) return 0xf8;
        if (m->negotiation==3) return 0xd8;
        if (m->negotiation==4) { m->negotiation=5; return 0x98; }
        if (m->negotiation==5) { m->negotiation=0; return 0xd8; }
        if (m->busy) return 0x58;
        if (m->ack) { m->ack--; return 0x98; }
        return 0xd8;
    case 2: return m->control & (m->caps&2 ? 0xff : 0xdf);
    case 3: if (m->epp_cycle_timeout) m->epp_timeout=true; return m->epp_address;
    case 4: if (m->epp_cycle_timeout) m->epp_timeout=true; return m->epp_data;
    case 8:
        assert(m->used); { uint8_t b=m->fifo[m->head++%32]; m->used--; return b; }
    case 10:
        if (m->ecr>>5==3 && m->used && !m->stuck_fifo) { m->used--; m->outputs++; }
        return (uint8_t)((m->ecr&~3u)|(m->used==m->depth ? 2 : 0)|(!m->used ? 1 : 0));
    default: assert(!"bad read"); return 0;
    }
}
static void outb(uint16_t port,uint8_t b)
{
    writes++;
    if (port<0x90) {
        if (port==0x0a) { if (b&4) { dma_mask|=1u<<(b&3); dma_running=false; } else { dma_mask&=~(1u<<b); dma_running=true; } }
        else if (port==0x0c) dma_byte=0;
        else if (port<8 && (port&1)) {
            if (!(dma_byte++%2)) dma_count=(dma_count&0xff00)|b; else dma_count=(dma_count&255)|((unsigned)b<<8);
        }
        return;
    }
    check_claim(port);
    unsigned r; struct model *m=model_port(port,&r);
    if (m->absent) return;
    switch (r) {
    case 0: m->data=b; return;
    case 1: if (!m->epp_sticky) m->epp_timeout=false; return;
    case 2:
        /* Real control has no guest IRQ enable, except restoring saved state. */
        assert(!(b&0x10));
        if (b==0x26 && m->negotiation==3) m->negotiation=2; /* ECP forward request */
        else if (b==0x26 && m->negotiation==2) { m->negotiation=0; m->ack=1; }
        else if (b==0x24 && m->control==0x26) m->negotiation=2;
        else if (b==6 && (m->data==1 || m->data==0x10 || m->data==0x40) && !m->negotiation)
            m->negotiation=1;
        else if (b==4 && m->negotiation==1) m->negotiation=2;
        else if (b==2 && m->negotiation==2) m->negotiation=3;
        else if ((b&~0x20u)==0x0c && m->negotiation>=2 && m->negotiation<4) m->negotiation=4;
        if ((m->control&1) && !(b&1) && !m->negotiation) {
            m->outputs++; if (!m->no_ack) m->ack=1;
        }
        m->control=b; return;
    case 3: m->epp_address=b; m->outputs++; if (m->epp_cycle_timeout) m->epp_timeout=true; return;
    case 4: m->epp_data=b; m->outputs++; if (m->epp_cycle_timeout) m->epp_timeout=true; return;
    case 8: assert(m->used<m->depth); m->fifo[(m->head+m->used++)%32]=b; return;
    case 10:
        if (stop_fail && (m->ecr&8)) b|=8;
        m->ecr=b;
        if ((b>>5)<2) { m->head=m->used=0; }
        return;
    default: assert(!"bad write");
    }
}
void task_sleep_ms(uint32_t ms)
{
    assert(irq_flags&0x200); g_ticks+=ms;
    if (dma_running && !dma_stall) { assert(irq_masked[7]); dma_count=0xffff; dma_running=false; }
    if (sleep_hook) { void (*h)(void)=sleep_hook; sleep_hook=0; h(); }
}
int udelay(uint32_t us) { assert(us<=5); return delay_fail ? -ENOSYS : 0; }
uint64_t deadline_after_ms(uint32_t ms) { return g_ticks+ms; }
bool deadline_passed(uint64_t d) { return g_ticks>=d; }
void *kzalloc(size_t n) { if (fail_alloc) return 0; allocations++; return calloc(1,n); }
void kfree(void *p) { if (p) { assert(allocations); allocations--; free(p); } }
void klog(const char *fmt,...) { (void)fmt; }
uint32_t pmm_alloc_dma_run(uint32_t n,uint32_t a)
{ assert(n==1 && a==1); if (fail_alloc) return 0; assert(!bounce_allocated); bounce_allocated=true; return 0x100000; }
void pmm_free(uint32_t p)
{ assert(p==0x100000 && bounce_allocated && !dma_running && !stop_fail && !dma_pins); bounce_allocated=false; }
int mm_pin(struct mm_pin *p)
{
    if (fail_pin) return -ENOMEM;
    assert(p->pages==1 && p->address_mask==0xffffff && p->boundary==65536 && !dma_pins);
    p->ticket=1; dma_pins++; return 0;
}
int mm_unpin(struct mm_pin *p,uint32_t g)
{ assert(p->ticket==1 && p->generation==g && dma_pins && !dma_running); p->ticket=0; dma_pins--; return 0; }
struct task *task_create_kernel(const char *n,void (*f)(void *),void *a,enum task_prio p)
{ (void)n; (void)f; (void)a; (void)p; return 0; }
unsigned pci_count(void) { return 0; }
const struct pci_func *pci_get(unsigned i) { (void)i; return 0; }
const struct resource *registry_get(unsigned i) { assert(i<nclaims); return &claims[i]; }
bool registry_valid(int h,gen_t g)
{ return h>=0 && (unsigned)h<nclaims && claims[h].generation==g && claims[h].state!=RS_RELEASED && claims[h].state!=RS_QUARANTINED; }
int registry_claim(enum res_type t,uint32_t a,uint32_t b,const char *o,bool share)
{
    assert(!share);
    for (unsigned i=0;i<nclaims;i++) if (claims[i].state!=RS_RELEASED &&
        claims[i].type==t && a<claims[i].end && claims[i].start<b) return -EBUSY;
    assert(nclaims<64); unsigned i=nclaims++;
    claims[i]=(struct resource){t,a,b,o,i+1,RS_CLAIMED,false}; return (int)i;
}
int registry_activate(int h,gen_t g)
{ assert(registry_valid(h,g) && claims[h].state==RS_CLAIMED); claims[h].state=RS_ACTIVE; return 0; }
int registry_quiesce(int h,gen_t g,registry_idle_proof_fn proof)
{ assert(claims[h].state==RS_ACTIVE); claims[h].state=RS_QUIESCING; assert(proof(h,g)); return 0; }
int registry_release(int h,gen_t g)
{ assert(registry_valid(h,g) && claims[h].state==RS_QUIESCING); claims[h].state=RS_RELEASED; return 0; }
int registry_quarantine(int h,gen_t g)
{ assert(claims[h].generation==g); claims[h].state=RS_QUARANTINED; return 0; }
void registry_snapshot(struct registry_stats *s) { memset(s,0,sizeof(*s)); s->live=nclaims; }
static unsigned fw_pending;
static bool fw_visible;
static struct ciuki_firmware_record fw_record;
int firmware_inventory_page(struct ciuki_firmware_page *p)
{
    *p=(struct ciuki_firmware_page){.version=CIUKI_FIRMWARE_VERSION,.size=sizeof(*p),.generation=1};
    if (fw_pending) { fw_pending--; p->flags=CIUKI_FIRMWARE_PENDING; }
    else if (fw_visible) { p->count=p->total=1; p->records[0]=fw_record; }
    return 0;
}
#include "../../src/kernel/drivers/lpt.c"
#include <ciuki/files.h>
#include <ciuki/signal.h>
static struct process user_process;
static struct proc_thread user_thread;
static struct uaddr user_memory;
static struct file_description *user_description;
static uint8_t user_bytes[4096];
static unsigned user_pins;
static bool user_fault,user_signal;
struct process *proc_current(void) { return &user_process; }
struct proc_thread *proc_thread_for(const struct task *t) { (void)t; return &user_thread; }
bool proc_signal_caught(struct proc_thread *t) { assert(t==&user_thread); return user_signal; }
struct file_description *file_fd(const struct process *p,int32_t fd)
{ assert(p==&user_process); return fd==5 ? user_description : 0; }
int ua_pin(struct uaddr *u,struct ua_pin *pin,uint32_t base,uint32_t bytes,bool write)
{
    assert(u==&user_memory); (void)pin; (void)write;
    if (user_fault || base<0x1000 || base>0x2000 || bytes>0x2000-base) return -EFAULT;
    user_pins++; return 0;
}
void ua_unpin(struct uaddr *u,struct ua_pin *pin)
{ assert(u==&user_memory && user_pins); (void)pin; user_pins--; }
int copy_from_user(void *dst,uint32_t src,uint32_t bytes)
{
    if (user_fault || src<0x1000 || src>0x2000 || bytes>0x2000-src) return -EFAULT;
    memcpy(dst,user_bytes+src-0x1000,bytes); return 0;
}
int copy_to_user(uint32_t dst,const void *src,uint32_t bytes)
{
    if (user_fault || dst<0x1000 || dst>0x2000 || bytes>0x2000-dst) return -EFAULT;
    memcpy(user_bytes+dst-0x1000,src,bytes); return 0;
}
static void user_retain(struct proc_object *o) { ((struct file_description *)o)->references++; }
static void user_release(struct proc_object *o) { assert(((struct file_description *)o)->references>1); ((struct file_description *)o)->references--; }
#include "../../src/kernel/drivers/lpt_native.c"
#include "../../src/kernel/vm/dosvm_internal.h"
static uint8_t guest_memory[2][0x1000];
static struct dosvm guests[2];
static dosvm_port_handler guest_handlers[3];
int dosvm_write(struct dosvm *v,uint32_t a,const void *p,unsigned n)
{ assert(v>=guests && v<guests+2 && a+n<=0x1000); memcpy(guest_memory[v-guests]+a,p,n); return 0; }
int dosvm_read(struct dosvm *v,uint32_t a,void *p,unsigned n)
{ assert(v>=guests && v<guests+2 && a+n<=0x1000); memcpy(p,guest_memory[v-guests]+a,n); return 0; }
int dosvm_port_register(struct dosvm_key k,uint16_t first,uint16_t last,dosvm_port_handler fn,void *arg)
{ (void)last; assert(k.id && arg); unsigned slot=first==0x378 ? 0 : first==0x278 ? 1 : 2; guest_handlers[slot]=fn; return 0; }
#include "../../src/kernel/vm/dosvm_lpt.c"
static void reset(void)
{
    memset(lpts,0,sizeof(lpts)); memset(hw,0,sizeof(hw)); memset(claims,0,sizeof(claims));
    memset(guests,0,sizeof(guests)); memset(guest_memory,0,sizeof(guest_memory));
    memset(irq_masked,0,sizeof(irq_masked));
    assert(!allocations && !user_pins);
    nclaims=reads=writes=0; g_ticks=0; dma_mask=15; dma_byte=dma_count=0;
    bounce_allocated=dma_running=stop_fail=fail_alloc=delay_fail=user_fault=user_signal=dma_stall=fail_pin=false;
    dma_pins=fw_pending=0; fw_visible=false;
    sleep_hook=0;
}
static int setup(unsigned i,unsigned base,unsigned high,unsigned caps,unsigned dma)
{
    hw[i]=(struct model){.base=base,.high=high,.data=0x7a,.control=0x0c,.ecr=0x15,
        .caps=caps,.depth=16,.external=0x33};
    struct lpt_resource r={i+1,base,high ? 8 : 3,high,7,dma,CIUKI_LPT_PNP,caps};
    return lpt_admit(&r,&lpt_real);
}
static uint32_t acquire(unsigned i)
{ uint32_t g; assert(!lpt_open(i,0,0,false,&g) && g); return g; }
static int configure(unsigned i,uint32_t g,unsigned mode,bool reverse,bool dma)
{
    struct ciuki_lpt_config c={1,sizeof(c),mode,reverse,5,dma,0,0};
    return lpt_control(i,g,LPT_SET_CONFIG,&c,sizeof(c),true);
}
static int transfer(unsigned i,uint32_t g,void *p,unsigned n,bool write)
{ return lpt_transfer(i,g,p,n,write,false,0,0); }
static bool cancel(void *arg) { return *(bool *)arg; }
static void close_sleep(void) { lpt_close(0,lpts[0].status.generation); }
static jmp_buf killed_operation;
static void kill_native_sleep(void)
{
    assert(user_thread.operation && user_thread.cleanup);
    void *o=user_thread.operation; void (*cleanup)(void *)=user_thread.cleanup;
    user_thread.operation=0; user_thread.cleanup=0; cleanup(o);
    longjmp(killed_operation,1);
}
static void test_spp(void)
{
    reset(); assert(setup(0,0x378,0,1,CIUKI_LPT_NO_DMA)==0);
    assert(lpts[0].status.capabilities==1 && hw[0].data==0x7a && hw[0].control==0x0c);
    uint32_t g=acquire(0),other; uint8_t bytes[]={0x61,0x62};
    assert(lpt_open(0,0,0,false,&other)==-EBUSY);
    assert(transfer(0,g,bytes,2,true)==2 && hw[0].outputs==2);
    hw[0].busy=true; unsigned before=hw[0].outputs;
    assert(lpt_transfer(0,g,bytes,1,true,true,0,0)==-EAGAIN && hw[0].outputs==before);
    assert(configure(0,g,0,false,false)==0);
    assert(transfer(0,g,bytes,1,true)==-ETIMEDOUT && g_ticks==5);
    bool cancelled=true;
    assert(lpt_transfer(0,g,bytes,1,true,false,cancel,&cancelled)==-EINTR);
    sleep_hook=close_sleep;
    assert(transfer(0,g,bytes,1,true)==-ENODEV && !lpts[0].status.generation);
    assert(hw[0].data==0x7a && hw[0].control==0x0c);
    other=acquire(0); assert(g!=other && transfer(0,g,bytes,1,true)==-ENODEV); lpt_close(0,other);
    reset(); setup(0,0x378,0,1,CIUKI_LPT_NO_DMA); g=acquire(0); configure(0,g,0,false,false);
    hw[0].no_ack=true; assert(transfer(0,g,bytes,1,true)==-ETIMEDOUT);
    lpt_close(0,g);
    puts("PASS SPP BUSY/ACK, whole-operation deadline, nonblock, cancel, close/generation");
}
static void test_modes(void)
{
    reset(); setup(0,0x378,0x920,15,CIUKI_LPT_NO_DMA);
    assert(lpts[0].status.capabilities==15 && lpts[0].status.fifo_depth==16 && hw[0].ecr==0x15);
    uint32_t g=acquire(0); uint8_t b=0x45;
    assert(!configure(0,g,2,false,false));
    assert(transfer(0,g,&b,1,true)==1 && hw[0].epp_data==b);
    struct ciuki_lpt_config address={1,sizeof(address),2,0,5,0,1,0};
    assert(!lpt_control(0,g,LPT_SET_CONFIG,&address,sizeof(address),true));
    assert(transfer(0,g,&b,1,true)==1 && hw[0].epp_address==b);
    hw[0].epp_cycle_timeout=true;
    assert(transfer(0,g,&b,1,true)==-ETIMEDOUT && !hw[0].epp_timeout);
    hw[0].epp_cycle_timeout=false;
    hw[0].epp_timeout=true; hw[0].epp_sticky=true;
    assert(transfer(0,g,&b,1,true)==-EIO); hw[0].epp_sticky=false;
    assert(lpt_epp_clear(&lpts[0]));
    assert(!configure(0,g,3,false,false));
    assert(transfer(0,g,&b,1,true)==1);
    assert(!configure(0,g,0,false,false)); /* termination before new mode */
    assert(!configure(0,g,1,true,false));
    assert(transfer(0,g,&b,1,false)==1 && b==0x33);
    assert(!configure(0,g,3,true,false));
    hw[0].fifo[0]=0x6a; hw[0].head=0; hw[0].used=1; hw[0].stuck_fifo=true;
    assert(transfer(0,g,&b,1,false)==1 && b==0x6a);
    assert(!configure(0,g,3,false,false));
    hw[0].stuck_fifo=true;
    uint8_t full[32]={0};
    assert(transfer(0,g,full,sizeof(full),true)==-ETIMEDOUT && hw[0].used==16);
    assert(lpts[0].status.flags&CIUKI_LPT_QUARANTINED);
    lpt_close(0,g); assert(lpt_open(0,0,0,false,&g)==-ENODEV);
    reset(); setup(0,0x378,0x920,15,CIUKI_LPT_NO_DMA); g=acquire(0);
    hw[0].reject_negotiation=true;
    assert(configure(0,g,3,false,false)==-ETIMEDOUT);
    assert(lpts[0].config.mode==0 && !(hw[0].ecr&0xe8));
    lpt_close(0,g);
    puts("PASS PS/2 reverse, EPP cycles/timeout, ECP negotiation/FIFO/reverse/quarantine");
}
static void test_dma(void)
{
    reset(); setup(0,0x378,0x920,15,1); uint32_t g=acquire(0);
    assert(lpts[0].status.capabilities==31 && !configure(0,g,3,false,true));
    assert(irq_masked[7] && claims[lpts[0].claims[3]].type==RES_IRQ);
    uint8_t data[]={0x45,0x46,0x47};
    assert(transfer(0,g,data,3,true)==3 && bounce_allocated && !dma_running);
    assert(!memcmp(ram+0x100000,data,3) && (dma_mask&2) && !(hw[0].ecr&8));
    lpt_close(0,g); assert(!bounce_allocated);
    reset(); registry_claim(RES_IRQ,7,8,"peer",false);
    setup(0,0x378,0x920,15,1); g=acquire(0);
    assert(lpts[0].status.capabilities==15 && !irq_masked[7] &&
        lpts[0].status.dma_channel==CIUKI_LPT_NO_DMA && lpts[0].claims[2]<0);
    assert(configure(0,g,3,false,true)==-EINVAL);
    lpt_close(0,g);
    reset(); setup(0,0x378,0x920,15,1); g=acquire(0); configure(0,g,3,false,true);
    fail_alloc=true; assert(transfer(0,g,data,3,true)==-ENOMEM && !bounce_allocated); fail_alloc=false;
    fail_pin=true; assert(transfer(0,g,data,3,true)==-ENOMEM && !bounce_allocated); fail_pin=false;
    dma_stall=true; assert(transfer(0,g,data,3,true)==-ETIMEDOUT && !dma_running);
    lpt_close(0,g); assert(!bounce_allocated && !dma_pins);
    reset(); setup(0,0x378,0x920,15,1); g=acquire(0); configure(0,g,3,false,true);
    sleep_hook=close_sleep;
    assert(transfer(0,g,data,3,true)==-ENODEV && !bounce_allocated && !dma_pins);
    reset(); setup(0,0x378,0x920,15,1); g=acquire(0); configure(0,g,3,false,true);
    stop_fail=true;
    assert(transfer(0,g,data,3,true)==-EIO && bounce_allocated);
    lpt_close(0,g); assert(bounce_allocated && (lpts[0].status.flags&CIUKI_LPT_QUARANTINED));
    puts("PASS ECP DMA single transfer, mask/count/enable stop proof and retained failed-stop bounce");
}
static void test_discovery(void)
{
    reset();
    hw[1]=(struct model){.base=0x278,.data=0x7a,.control=0x0c,.caps=3,.external=0x33};
    uint16_t bases[]={0,0x278,0xffff,0x8888}; lpt_bda_discover(bases);
    assert(!lpts[0].present && lpts[1].present && !lpts[2].present);
    assert(lpt_pnp_assigned("PNP0400",0x278,3,0,0,CIUKI_LPT_NO_DMA)==1);
    assert(lpts[1].resource.origin==CIUKI_LPT_PNP);
    assert(lpt_pnp_assigned("PNP0500",0x1234,3,0,0,CIUKI_LPT_NO_DMA)==-EINVAL);
    reset(); hw[0]=(struct model){.base=0x378,.high=0x920,.data=0x7a,.control=0x0c,
        .caps=15,.depth=16,.ecr=0x15,.external=0x33};
    uint16_t legacy[]={0x378,0,0}; lpt_bda_discover(legacy);
    uint32_t opened=acquire(0); unsigned touched=writes;
    assert(lpt_pnp_assigned("PNP0401",0x378,8,0x920,7,1)==0 && writes==touched);
    lpt_close(0,opened); opened=acquire(0);
    assert(lpts[0].resource.origin==CIUKI_LPT_PNP && lpts[0].status.capabilities==27);
    lpt_close(0,opened);
    reset();
    unsigned before=writes;
    registry_claim(RES_PORT,0x920,0x923,"other",false);
    assert(setup(0,0x378,0x920,15,1)==0 && writes==before && !lpts[0].status.flags);
    reset(); hw[0]=(struct model){.base=0x1000,.high=0x2000,.data=0x7a,.control=0x0c,
        .caps=15,.depth=16,.ecr=0x15,.external=0x33};
    struct pci_func f={.vendor=0x9710,.device=0x9805,.class_code=7,.command=1,.bar={0x1001,0x2001}};
    lpt_pci_discover(&f);
    assert(lpts[0].status.base==0x1000 && lpts[0].status.base_hi==0x2000 && lpts[0].status.capabilities==15);
    before=writes; f.vendor=0x9999; lpt_pci_discover(&f); assert(writes==before);
    reset(); hw[0]=(struct model){.base=0x378,.high=0x920,.data=0x7a,.control=0x0c,
        .caps=15,.depth=16,.ecr=0x15,.external=0x33};
    fw_record=(struct ciuki_firmware_record){.state=CIUKI_FIRMWARE_ROUTING_UNQUALIFIED,.resource_count=4};
    memcpy(fw_record.hid,"PNP0401",8);
    fw_record.resources[0]=(struct ciuki_firmware_resource){CIUKI_RESOURCE_IO,0,0x378,0x380};
    fw_record.resources[1]=(struct ciuki_firmware_resource){CIUKI_RESOURCE_IO,0,0x920,0x923};
    fw_record.resources[2]=(struct ciuki_firmware_resource){CIUKI_RESOURCE_IRQ,0,7,8};
    fw_record.resources[3]=(struct ciuki_firmware_resource){CIUKI_RESOURCE_DMA,0,1,2};
    fw_visible=true; fw_pending=2; lpt_firmware(0);
    assert(g_ticks==2 && lpts[0].status.dma_channel==1 && lpts[0].status.capabilities==27 && !allocations);
    before=writes; claims[lpts[0].claims[0]].state=RS_QUARANTINED;
    uint32_t g; assert(lpt_open(0,0,0,false,&g)==-ENODEV && writes==before);
    puts("PASS BDA three words/EBDA exclusion, assigned PnP upgrade, independent claims, exact PCI BAR pairs");
}
static void test_native(void)
{
    reset(); setup(0,0x378,0,1,CIUKI_LPT_NO_DMA);
    struct px_node node={.kind=PX_LPT,.drive=0};
    static struct file_description d;
    d=(struct file_description){.node=&node,.references=1,.flags=O_RDWR};
    d.object.retain=user_retain; d.object.release=user_release;
    user_description=&d; user_process.memory=&user_memory; user_process.state=PROC_LIVE;
    user_thread.process=&user_process;
    assert(lpt_device_open(&d,O_RDWR|O_APPEND)==-EINVAL);
    assert(!lpt_device_open(&d,O_RDWR));
    user_bytes[0]='x'; user_fault=true; unsigned before=writes;
    assert(lpt_rw_user(&d,0x1000,1,true)==-EFAULT && writes==before); user_fault=false;
    assert(lpt_rw_user(&d,0x1000,1,true)==1);
    struct ciuki_lpt_config c={1,sizeof(c),0,0,5,0,0,0};
    memcpy(user_bytes,&c,sizeof(c));
    assert(!lpt_devctl(&user_process,5,LPT_SET_CONFIG,0x1000,sizeof(c)));
    c.reserved=1; memcpy(user_bytes,&c,sizeof(c));
    assert(lpt_devctl(&user_process,5,LPT_SET_CONFIG,0x1000,sizeof(c))==-EINVAL);
    d.flags=O_RDONLY; c.reserved=0; memcpy(user_bytes,&c,sizeof(c));
    assert(lpt_devctl(&user_process,5,LPT_SET_CONFIG,0x1000,sizeof(c))==-EACCES);
    user_signal=true; d.flags=O_WRONLY;
    before=writes; assert(lpt_rw_user(&d,0x1000,1,true)==-EINTR && writes==before); user_signal=false;
    hw[0].busy=true; sleep_hook=kill_native_sleep;
    if (!setjmp(killed_operation)) { (void)lpt_rw_user(&d,0x1000,1,true); assert(!"killed operation resumed"); }
    assert(!lpts[0].busy && !lpts[0].status.generation);
    lpt_device_close(&d);
    assert(!user_pins && d.references==1 && !allocations);
    puts("PASS native checked buffers, access rights, devctl ABI and zero pin/operation/reference leaks");
}
static void test_int17(void)
{
    reset(); setup(0,0x378,0,3,CIUKI_LPT_NO_DMA);
    for (unsigned i=0;i<2;i++) {
        guests[i].info.key=(struct dosvm_key){i+1,10+i}; guests[i].info.state=DOSVM_RUNNABLE;
        uint16_t initial_equipment=0x823; /* four COM ports and one floppy */
        memcpy(guest_memory[i]+0x410,&initial_equipment,2);
        assert(!dosvm_lpt_init(&guests[i]));
    }
    uint16_t base,equipment; memcpy(&base,guest_memory[0]+0x408,2); memcpy(&equipment,guest_memory[0]+0x410,2);
    assert(base==0x378 && equipment==0x4823 && !guest_memory[0][0x40e]);
    struct v86_frame f={0}; f.tf.eax=0xaabb0078; f.tf.edx=0; f.tf.eflags=0x247;
    assert(!dosvm_lpt_bios(&guests[0],&f) && f.tf.eax==0xaabb9078 && f.tf.eflags==0x247);
    f.tf.eax=0x0200;
    assert(!dosvm_lpt_bios(&guests[1],&f) && (f.tf.eax&0x100));
    uint32_t g; assert(lpt_open(0,0,0,false,&g)==-EBUSY);
    f.tf.eax=0x0100; assert(!dosvm_lpt_bios(&guests[0],&f) && !(f.tf.eax&0x100));
    dosvm_lpt_stop(&guests[0]);
    assert(!dosvm_lpt_lease(&guests[1],0,0,true));
    uint32_t value=0x30;
    assert(!guest_handlers[0](&guests[1],0x37a,1,true,&value,&guests[1]) && !(hw[0].control&0x10));
    unsigned before=writes;
    value=0x55; guest_handlers[0](&guests[0],0x378,1,true,&value,&guests[0]); assert(writes==before);
    value=0x7a; guest_handlers[0](&guests[1],0x378,2,true,&value,&guests[1]); assert(writes==before);
    f.tf.eax=0x0000; assert(!dosvm_lpt_bios(&guests[1],&f) && (f.tf.eax&0x100));
    dosvm_lpt_stop(&guests[1]); g=acquire(0); lpt_close(0,g);
    puts("PASS INT17 write/init/status, FLAGS/AL, virtual BDA, two-VM/native exclusion, raw IRQ/width filtering and stop");
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);
    test_spp(); test_modes(); test_dma(); test_discovery(); test_native(); test_int17();
    printf("PASS LPT metadata=%zu VM_state=%zu native_operation=%zu bytes; no global limit increase\n",
           sizeof(lpts),sizeof(((struct dosvm *)0)->lpt),sizeof(struct lpt_operation));
    return 0;
}
