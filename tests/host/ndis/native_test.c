/* Native integration with fake registry/hardware/task operations, not a device
 * qualification. SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "src/kernel/ndis/native.h"
#include <ciuki/clock.h>
#define CIUKI_CPU_H
static uint32_t config_selected;
static uint8_t config[256]={0x86,0x80,0x0e,0x10};
static unsigned config_writes;static uint8_t scb_mask,scb_cause;
static uint32_t read_eflags(void) { return 0x200; }
static void outb(uint16_t p,uint8_t v)
{ if(p==0x3003) { scb_mask=v;return; }assert(p>=0xcfc && p<=0xcff);config[(config_selected&0xfc)+(p-0xcfc)]=v;config_writes++; }
static void cpu_outw(uint16_t p,uint16_t v)
{ assert(p==0xcfc);memcpy(config+(config_selected&0xfc),&v,2);config_writes++; }
static void outl(uint16_t p,uint32_t v) { assert(p==0xcf8);config_selected=v; }
static uint8_t inb(uint16_t p) { assert(p==0x3003 || p==0x3001);return p==0x3003?scb_mask:scb_cause; }
static uint16_t cpu_inw(uint16_t p)
{ assert(p==0xcfc || p==0xcfe);uint16_t value;memcpy(&value,config+(config_selected&0xfc)+(p-0xcfc),2);return value; }
static uint32_t inl(uint16_t p)
{ assert(p==0xcfc);uint32_t value;memcpy(&value,config+(config_selected&0xfc),4);return value; }
static uint32_t irq_save(void) { return 0x200; }
static void irq_restore(uint32_t f) { assert(f==0x200); }
#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
static struct ndis_native n,n2;
static struct ndis_channel_shared shared __attribute__((aligned(4096)));
static struct task task,task2;
static unsigned killed;static bool mask_success=true;
struct task *g_current;
volatile bool g_need_resched;
static unsigned masks,fences,wakes,worker_wakes,tx_done,detached,quarantines;
static bool qualified,claim_live=true,device_pending;
static uint64_t now;
volatile uint64_t g_ticks;
static struct pci_func pci={.vendor=0x8086,.device=0x100e,.irq_line=11,.irq_pin=1};
static struct resource irq={.type=RES_IRQ,.start=11,.end=12,.generation=3,.state=RS_ACTIVE};
static const char owner[]="fixture-ndis";
static irq_chain_handler_t irq_handler;
static bool masker(struct ndis_native *p,bool on) { assert(p==&n || p==&n2);(void)on;masks++;return mask_success; }
static int source(struct ndis_native *p) { assert(p==&n || p==&n2);return device_pending; }
static bool stop(struct ndis_native *p) { assert(p==&n || p==&n2);fences++;return false; }
static const struct ndis_qualification q={.name="synthetic",.evidence="host model only",.qualified=true,
    .shared_mask=true,.runtime_cpu=true,.runtime_irq=true,.runtime_fence=true,.config_write={[2]=1},.bar_bytes={[1]=32},.mask=masker,.cause=source,.stop=stop};
bool ndis_profiles_available(void) { return qualified; }
const struct ndis_qualification *ndis_qualified_profile(const struct ntimage *i,const struct pci_func *p)
{ return qualified && i && p && !i->quarantined?&q:0; }
const struct pci_func *pci_get(unsigned i) { assert(i==0);return &pci; }
bool pci_irq_routed(const struct pci_func *p) { return p==&pci; }
bool registry_valid(int h,gen_t g) { return claim_live && h==2 && g==3; }
const struct resource *registry_get(unsigned i) { assert(i==2);return &irq; }
int registry_quarantine(int h,gen_t g) { assert(h==2 && g==3 && fences);claim_live=false;quarantines++;return 0; }
int irq_chain_add(unsigned i,irq_chain_handler_t fn,const char *o) { assert(i==11 && o==owner);irq_handler=fn;return 0; }
int irq_chain_remove(unsigned i,irq_chain_handler_t fn,const char *o)
{ assert(i==11 && o==owner && fn==irq_handler);detached++;irq_handler=0;return 0; }
bool irq_chain_pending(unsigned i,int h,gen_t g,uint32_t t) { assert(i==11 && h==2 && g==3 && t && masks);return true; }
bool irq_chain_resolve_pending(unsigned i,int h,gen_t g,uint32_t t,bool recognized)
{ assert(i==11 && h==2 && g==3 && t);(void)recognized;return true; }
bool ndis_pci_disable(const struct pci_func *p,uint16_t *a,uint16_t *b)
{ assert(p==&n.pci || p==&n2.pci);*a=7;*b=0x400;return true; }
bool ndis_user_mapping_owned(void *c,const struct ndis_host_map *m) { (void)c;(void)m;return true; }
int ndis_user_mapping_apply(void *c,const struct ndis_host_map *m,bool on) { (void)c;(void)m;(void)on;return 0; }
uint32_t as_lookup(const struct aspace *a,uint32_t va,uint32_t *pte)
{ assert((a==&task.as || a==&task2.as) && va>=0x300000);*pte=3;return V2P(&shared)+(va-0x300000); }
bool ledger_page_pinned(uint32_t p) { (void)p;return true; }
int ndis_user_attach(struct task *t,struct ndis_user_host *h)
{ assert(t==&task && t->state==T_BLOCKED);t->ndis_user_host=h;return 1; }
void task_start(struct task *t)
{ assert((t==&task || t==&task2) && t->ndis_user_host);if(t->state==T_BLOCKED) { wakes++;t->state=T_READY; } }
bool task_alive(const struct task *t) { return t==&task || t==&task2; }
void task_kill(struct task *t,int code)
{ assert(t==&task2 && !t->ndis_user_host && code==-1 && n2.retained && fences);t->state=T_ZOMBIE;killed++; }
uint64_t ndis_execution_now(void) { return now; }
bool as_range_ok(const struct aspace *a,uint32_t p,uint32_t b,bool write)
{ (void)a;(void)p;(void)b;(void)write;assert(0);return false; }
int copy_user(void *d,const void *s,uint32_t b) { (void)d;(void)s;(void)b;assert(0);return -1; }
void file_clock_snapshot(struct clock_seed *s) { *s=(struct clock_seed){0}; }
void kwait_init(struct kwait *q) { memset(q,0,sizeof(*q)); }
bool kwait_wait_until(struct kwait *q,kwait_cond_fn p,void *a,uint64_t d)
{ (void)q;(void)p;(void)a;(void)d;assert(0);return false; }
void kwait_wake_irq(struct kwait *q,bool io) { (void)q;(void)io; }
int udelay(uint32_t us) { (void)us;assert(0);return -1; }
void klog(const char *fmt,...) { assert(fmt); }
static void worker(void *ctx) { (void)ctx;worker_wakes++; }
static void tx(void *ctx,uint32_t token,int status) { (void)ctx;assert(token && status==-1);tx_done++; }
#include "src/kernel/ndis/native.c"
int main(void)
{
    task.state=task2.state=T_BLOCKED;task.user=task2.user=true;
    struct ndis_flag_table sites={.sealed=true};
    struct ntimage image={.loaded=true,.platform={.user_host=true,.flag_table=&sites},.pe={.base=0x100000,
        .size=8192,.section_count=1,.sections={{.rva=4096,.span=4096,.flags=0x60000000}}}};
    irq.owner=owner;n.memory.generation=7;n.shared_address=0x300000;n.isr=0x101000;n.dpc=0x101010;
    n.wake_worker=worker;n.complete_tx=tx;
    assert(!ndis_native_bind(&n,&task,&image,0,2,3,owner,7,0x200000,0x290000,4096,&shared,true) && !masks);
    qualified=true;assert(ndis_native_bind(&n,&task,&image,0,2,3,owner,7,0x200000,0x290000,4096,&shared,true));
    assert(n.host.state==NDIS_HOST_LIVE && n.irq_attached && irq_handler && masks);
    uint8_t bytes[8];assert(ndis_native_pci_read(&n,1,bytes,7)==7 && !memcmp(bytes,config+1,7));
    assert(ndis_native_pci_intx_mask(&n,true) && (config[5]&4));
    config[6]=8;assert(ndis_native_pci_intx_cause(&n)==1 && config[6]==8);
    assert(ndis_native_pci_intx_mask(&n,false) && !(config[5]&4) && config[6]==8);
    assert(!ndis_native_no_idle_proof(&n));config_writes=0;
    n.pci.device=0x1229;n.pci.bar[1]=0x3001;n.port_count=1;n.port_claim[0]=2;n.port_generation[0]=3;
    irq.type=RES_PORT;irq.start=0x3000;irq.end=0x3020;scb_mask=0xa0;scb_cause=0x40;
    assert(ndis_native_e100_mask(&n,true) && scb_mask==0xa1);
    assert(ndis_native_e100_cause(&n)==1 && scb_cause==0x40);
    assert(ndis_native_e100_mask(&n,false) && scb_mask==0xa0 && scb_cause==0x40);
    n.pci.device=0x100e;n.pci.bar[1]=0;n.port_count=0;irq.type=RES_IRQ;irq.start=11;irq.end=12;
    uint8_t value=0x55;assert(!ndis_native_pci_write(&n,4,&value,1) && !config_writes);
    assert(ndis_native_pci_write(&n,0x40,&value,1)==1 && config_writes==1 && config[0x40]==value);
    assert(!ndis_native_pci_write(&n,0x41,&value,1) && config_writes==1);
    uint32_t args[2]={11,22},entry_token,result;struct ndis_host_event entry;
    assert(ndis_native_enqueue(&n,NDIS_HOST_LIFECYCLE,0x101100,2,args,0,&entry_token));
    assert(ndis_host_take(&n.host,0,&entry));ndis_native_callback_result(&n,&entry,0x1234);
    assert(ndis_host_complete(&n.host,7,entry_token,0));
    assert(ndis_native_take_result(&n,7,entry_token,&result) && result==0x1234);
    assert(!ndis_native_take_result(&n,7,entry_token,&result));
    assert(!irq_handler(11,owner) && n.host.irq_pending && n.irq_delivery);
    ndis_native_poll(&n);assert(n.isr_callback && n.host.count==1);
    struct ndis_host_event event;assert(ndis_host_take(&n.host,0,&event) && event.kind==NDIS_HOST_ISR);
    shared.irq_result[0]=shared.irq_result[1]=1;
    ndis_native_callback_result(&n,&event,0);assert(n.dpc_callback && n.host.count==1);
    assert(ndis_host_complete(&n.host,7,event.token,0));
    assert(ndis_host_take(&n.host,0,&event) && event.kind==NDIS_HOST_DPC);
    ndis_native_callback_result(&n,&event,0);assert(!n.host.irq_pending);
    assert(ndis_host_complete(&n.host,7,event.token,0));
    uint8_t frame[60]={0};uint32_t token;assert(ndis_channel_send(&n.channel,frame,60,&token)==1);
    ndis_host_stop(&n.host,"synthetic",14,0x101000,0);
    assert(n.retained && n.memory.retained && detached && quarantines && fences && tx_done==1 && worker_wakes && wakes);
    assert(!ndis_host_live(&n.host) && !irq_handler && image.quarantined && !n.channel.live);
    ndis_native_poll(&n);ndis_host_stop(&n.host,"again",0,0,0);assert(tx_done==1);
    struct ntimage image2=image;image2.quarantined=false;image2.entered=false;claim_live=true;mask_success=false;
    n2.memory.generation=7;n2.shared_address=0x300000;n2.wake_worker=worker;n2.complete_tx=tx;
    unsigned before_wakes=wakes;
    assert(!ndis_native_bind(&n2,&task2,&image2,0,2,3,owner,7,0x200000,0x290000,4096,&shared,true));
    assert(n2.retained && killed==1 && wakes==before_wakes); /* no raw pre-attach wake, even for initial credits */
    assert(task2.state==T_ZOMBIE && !task2.ndis_user_host && adapters[1]==&n2);
    puts("PASS native admission gate, registry IRQ->ISR->DPC->rearm, fence, TX cancellation and quarantine");fflush(stdout);return 0;
}
