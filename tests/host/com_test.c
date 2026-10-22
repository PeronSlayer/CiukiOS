/* Banked UART hardware model, production COM service. No guest evidence.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CIUKI_CPU_H
#include <ciuki/kernel.h>
#include <ciuki/com.h>
#include <ciuki/task.h>
#include <ciuki/work.h>
#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
static uint32_t flags=0x200;
static uint32_t irq_save(void) { uint32_t old=flags; flags=0; return old; }
static void irq_restore(uint32_t f) { flags=f; }
struct ciuki_boot_info g_boot;
static bool logger_present, fail_work, fail_alloc, cancel_wait;
static unsigned logger_calls, reads, writes, allocations;
static irq_handler_t handlers[16];
struct model {
    unsigned base, chip, head, used, tx_used, depth_override;
    uint8_t regs[8], divisor[2], efr, fcr, error, modem, rx[128], tx[65536];
    bool absent, stuck, clone64, bad_divisor, timed_out;
} models[8];
static unsigned capacity(struct model *m)
{
    if (!(m->fcr&1)) return 1;
    if (m->depth_override) return m->depth_override;
    switch (m->chip) {
    case CIUKI_COM_16550A: return 16;
    case CIUKI_COM_16650: return 32;
    case CIUKI_COM_16750: return m->fcr&0x20 ? 64 : 16;
    default: return 1;
    }
}
static void receive(struct model *m, uint8_t b)
{
    if (m->used>=capacity(m)) { m->error|=2; return; }
    m->rx[(m->head+m->used)%128]=b; m->used++;
}
static struct model *find_model(uint16_t port, unsigned *r)
{
    for (unsigned i=0;i<8;i++) if (models[i].base && port>=models[i].base && port<models[i].base+8) {
        *r=port-models[i].base; return &models[i];
    }
    assert(!"unexpected port I/O"); return 0;
}
static unsigned fifo_trigger(struct model *m)
{
    static const unsigned regular[]={1,4,8,14}, enhanced[]={8,16,24,28}, large[]={1,16,32,56};
    if (!(m->fcr&1) || capacity(m)==1) return 1;
    unsigned k=m->fcr>>6;
    return m->chip==CIUKI_COM_16650 ? enhanced[k] : capacity(m)==64 ? large[k] : regular[k];
}
static uint8_t inb(uint16_t port)
{
    unsigned r; struct model *m=find_model(port,&r); reads++;
    if (m->absent) return 0xff;
    if (r==2) {
        if (m->regs[3]==0xbf && m->chip==CIUKI_COM_16650) return m->efr;
        unsigned fifo=0;
        if (m->fcr&1) {
            if (m->chip==CIUKI_COM_16550) fifo=0x80;
            if (m->chip>=CIUKI_COM_16550A) fifo=0xc0;
            if (m->chip==CIUKI_COM_16750 && (m->fcr&0x20)) fifo|=0x20;
        }
        unsigned cause=1;
        if ((m->regs[1]&4) && m->error) cause=6;
        else if ((m->regs[1]&1) && m->used>=fifo_trigger(m)) cause=4;
        else if ((m->regs[1]&1) && m->used && m->timed_out) cause=12;
        else if (m->regs[1]&2) cause=2;
        else if ((m->regs[1]&8) && (m->modem&15)) cause=0;
        return (uint8_t)(fifo|cause);
    }
    if ((m->regs[3]&0x80) && r<2) return m->divisor[r];
    if (r==0) {
        assert(m->used); uint8_t b=m->rx[m->head]; m->head=(m->head+1)%128; m->used--; return b;
    }
    if (r==5) { uint8_t l=(m->stuck ? 0 : 0x60)|m->error|(m->used ? 1 : 0); m->error=0; return l; }
    if (r==6) {
        if (m->regs[4]&0x10) return (uint8_t)(((m->regs[4]&2)<<3)|((m->regs[4]&1)<<5)|
            ((m->regs[4]&4)<<4)|((m->regs[4]&8)<<4));
        uint8_t v=m->modem; m->modem&=0xf0; return v;
    }
    if (r==7 && m->chip==CIUKI_COM_8250) return 0xff;
    return m->regs[r];
}
static void outb(uint16_t port, uint8_t b)
{
    unsigned r; struct model *m=find_model(port,&r); writes++;
    if (m->absent) return;
    if (r==2) {
        if (m->regs[3]==0xbf && m->chip==CIUKI_COM_16650) { m->efr=b; return; }
        if (b&2) { m->head=m->used=0; m->error=0; }
        if (m->chip==CIUKI_COM_16750 && !(m->regs[3]&0x80) && !m->clone64) b=(b&~0x20)|(m->fcr&0x20);
        m->fcr=b&~6u; return;
    }
    if ((m->regs[3]&0x80) && r<2) { if (!m->bad_divisor) m->divisor[r]=b; return; }
    if (r==0) {
        if (m->regs[4]&0x10) receive(m,b);
        else { assert(m->tx_used<sizeof(m->tx)); m->tx[m->tx_used++]=b; }
        return;
    }
    if (r==7 && m->chip==CIUKI_COM_8250) return;
    m->regs[r]=b;
}
static void pause_io(void *c) { (void)c; }
static uint8_t fake_read(void *c,uint16_t p) { (void)c; return inb(p); }
static void fake_write(void *c,uint16_t p,uint8_t b) { (void)c; outb(p,b); }
static const struct com_io io={fake_read,fake_write,pause_io,0};
void task_sleep_ms(uint32_t ms) { (void)ms; }
void *kzalloc(size_t n) { if (fail_alloc) return 0; allocations++; return calloc(1,n); }
void kfree(void *p) { if (p) { assert(allocations); allocations--; free(p); } }
void klog(const char *fmt,...) { (void)fmt; }
#ifndef COM_WITH_LOGGER
bool serial_present(void) { return logger_present; }
unsigned serial_fifo_size(void) { return 16; }
#endif
void irq_set_handler(unsigned irq,irq_handler_t h) { assert(irq<16); handlers[irq]=h; }
void pic_unmask(unsigned irq) { assert(irq<16); }
void kwait_init(struct kwait *w) { memset(w,0,sizeof(*w)); }
void kwait_wake_irq(struct kwait *w,bool io_wake) { (void)w; (void)io_wake; }
static kwork_fn work[256]; static void *args[256]; static unsigned work_head, work_used;
static void (*wait_hook)(void);
bool kwork_queue(kwork_fn fn,void *arg) {
    if (fail_work) return false;
    assert(work_used<256); unsigned i=(work_head+work_used++)%256; work[i]=fn; args[i]=arg; return true;
}
static void drain_work(void) {
    unsigned passes=0;
    while (work_used) { assert(++passes<1000); unsigned i=work_head; work_head=(work_head+1)%256; work_used--; work[i](args[i]); }
}
uint64_t deadline_after_ms(uint32_t ms) { return ms; }
bool kwait_wait_until(struct kwait *w,kwait_cond_fn cond,void *arg,uint64_t deadline)
{ (void)w; assert(deadline); if (wait_hook) wait_hook(); return cond(arg); }
static struct resource claims[64]; static unsigned claim_count;
static irq_chain_handler_t chain[8]; static const char *chain_names[8]; static unsigned chain_count;
unsigned registry_count(void) { return claim_count; }
const struct resource *registry_get(unsigned i) { assert(i<claim_count); return &claims[i]; }
int registry_claim(enum res_type t,uint32_t s,uint32_t e,const char *owner,bool share)
{
    for (unsigned i=0;i<claim_count;i++) {
        struct resource *r=&claims[i];
        if (r->state!=RS_RELEASED && r->type==t && s<r->end && r->start<e && !(share && r->shareable)) return -EBUSY;
    }
    assert(claim_count<64); unsigned i=claim_count++;
    claims[i]=(struct resource){t,s,e,owner,i+1,RS_CLAIMED,share}; return (int)i;
}
int registry_activate(int h,gen_t g) { assert(claims[h].generation==g); claims[h].state=RS_ACTIVE; return 0; }
bool registry_valid(int h,gen_t g) { return h>=0 && (unsigned)h<claim_count && claims[h].generation==g && claims[h].state!=RS_RELEASED && claims[h].state!=RS_QUARANTINED; }
int registry_release(int h,gen_t g) { assert(claims[h].generation==g); claims[h].state=RS_RELEASED; return 0; }
int registry_quarantine(int h,gen_t g) { assert(claims[h].generation==g); claims[h].state=RS_QUARANTINED; return 0; }
void registry_snapshot(struct registry_stats *s) { memset(s,0,sizeof(*s)); }
int irq_chain_add(unsigned irq,irq_chain_handler_t fn,const char *owner)
{ assert(irq<16 && chain_count<8); chain[chain_count]=fn; chain_names[chain_count++]=owner; return 0; }
unsigned pci_count(void) { return 0; }
const struct pci_func *pci_get(unsigned i) { (void)i; return 0; }
bool pci_irq_routed(const struct pci_func *f) { return f->irq_pin==1 && f->irq_line==11; }
#include "../../src/kernel/drivers/com.c"
#include <ciuki/files.h>
#include <ciuki/signal.h>
struct task *g_current;
static struct process user_process;
static struct proc_thread user_thread;
static struct uaddr user_memory;
static struct file_description *user_description;
static uint8_t user_bytes[4096];
static unsigned user_pins;
static bool user_fault, user_signal;
struct process *proc_current(void) { return &user_process; }
struct proc_thread *proc_thread_for(const struct task *t) { (void)t; return &user_thread; }
bool proc_signal_caught(struct proc_thread *t) { assert(t==&user_thread); return user_signal; }
struct file_description *file_fd(const struct process *p,int32_t fd) { assert(p==&user_process); return fd==5 ? user_description : 0; }
int ua_pin(struct uaddr *u,struct ua_pin *pin,uint32_t base,uint32_t bytes,bool write)
{
    assert(u==&user_memory); (void)pin; (void)write;
    if (user_fault || base<0x1000 || base>0x2000 || bytes>0x2000-base) return -EFAULT;
    user_pins++; return 0;
}
void ua_unpin(struct uaddr *u,struct ua_pin *pin) { assert(u==&user_memory && user_pins); (void)pin; user_pins--; }
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
#include "../../src/kernel/drivers/com_native.c"
static void reset(void)
{
    drain_work();
    for (unsigned i=0;i<CIUKI_COM_MAX;i++) if (ports[i].queues) com_close(i,ports[i].status.generation);
    assert(!allocations); memset(ports,0,sizeof(ports)); memset(models,0,sizeof(models));
    memset(irq_owned,0,sizeof(irq_owned)); memset(legacy_attached,0,sizeof(legacy_attached));
    memset(log_handler,0,sizeof(log_handler)); memset(handlers,0,sizeof(handlers));
    memset(&g_boot,0,sizeof(g_boot)); claim_count=chain_count=logger_calls=reads=writes=0;
    logger_present=fail_work=fail_alloc=cancel_wait=false;
}
static void make_model(unsigned i,unsigned base,unsigned chip)
{
    models[i]=(struct model){.base=base,.chip=chip,.regs={0,0,0,3,0,0,0,0x37},.divisor={3,0}};
}
static int admit(unsigned model,unsigned number,unsigned irq,unsigned origin)
{
    struct com_resource r={number,models[model].base,irq,origin,1843200,1,true};
    return com_admit(&r,&io);
}
static void interrupt(unsigned irq)
{
    for (unsigned i=0;i<8;i++) models[i].timed_out=true;
    struct trap_frame f={.vector=0x20+irq}; assert(handlers[irq]);
    unsigned before=reads+writes; handlers[irq](&f);
    assert(reads+writes-before<=170); /* 1 us/access host budget model */
    drain_work();
}
static void logger(struct trap_frame *tf) { assert(tf); logger_calls++; }
static bool cancelled(void) { return cancel_wait; }
static void generations(void)
{
    for (unsigned chip=CIUKI_COM_8250;chip<=CIUKI_COM_16750;chip++) {
        reset(); make_model(0,0x2f8,chip);
        struct model original=models[0]; assert(admit(0,2,3,CIUKI_COM_BDA)==1);
        assert(ports[1].status.flags==CIUKI_COM_READY && ports[1].status.chip==chip);
        assert(ports[1].status.fifo_depth==(chip<=CIUKI_COM_16550 ? 1u : chip==CIUKI_COM_16550A ? 16u : chip==CIUKI_COM_16650 ? 32u : 64u));
        assert(!memcmp(original.regs,models[0].regs,8) && !memcmp(original.divisor,models[0].divisor,2));
        assert(!models[0].fcr && !models[0].efr && !models[0].tx_used);
        uint32_t g=0; assert(!com_open(1,&g) && g); assert(com_open(1,&g)==-EBUSY);
        struct ciuki_com_line l=default_line();
        for (unsigned trigger=0;trigger<4;trigger++) {
            l.rx_trigger=trigger;
            assert(!com_control(1,g,COM_SET_LINE,&l,sizeof(l),true));
            unsigned n=capacity(&models[0])>1 ? fifo_trigger(&models[0]) : 1;
            models[0].timed_out=false;
            for (unsigned k=0;k+1<n;k++) receive(&models[0],(uint8_t)(k+17));
            assert((inb((uint16_t)(models[0].base+2))&15)==1);
            receive(&models[0],(uint8_t)(n-1+17));
            assert((inb((uint16_t)(models[0].base+2))&15)==4);
            interrupt(3); uint8_t data[64]; assert(com_transfer(1,g,data,sizeof(data),false)==(int)n);
            for (unsigned k=0;k<n;k++) assert(data[k]==k+17);
        }
        uint8_t data[1024]; memset(data,0xa7,sizeof(data));
        assert(com_transfer(1,g,data,sizeof(data),true)==1024);
        assert(com_transfer(1,g,data,1,true)==-EAGAIN);
        for (unsigned k=0;ports[1].status.tx_queued;k++) { assert(k<=1024); interrupt(3); }
        assert(models[0].tx_used==1024 && !memcmp(models[0].tx,data,1024));
        models[0].error=0x1e; interrupt(3); assert(ports[1].status.errors==0x1e);
        for (unsigned k=0;k<1100;k++) { receive(&models[0],0x33); interrupt(3); }
        assert(ports[1].status.rx_queued==1024 && ports[1].status.dropped==76);
        com_close(1,g); assert(com_transfer(1,g,data,1,false)==-ENODEV);
        for (unsigned k=0;k<20;k++) { uint32_t next; assert(!com_open(1,&next) && next!=g); com_close(1,next); }
    }
    puts("PASS COM six generations, restoration, triggers, IRQ RX/TX, line errors, overflow, open/close and stale generations");
}
static void arbitration(void)
{
    for (unsigned irq=3;irq<=4;irq++) {
        reset(); unsigned base=irq==3 ? 0x2f8 : 0x3f8;
        make_model(0,base,CIUKI_COM_16550A); make_model(1,base-0x10,CIUKI_COM_16650);
        logger_present=true; g_boot.uart_base=base;
        assert(registry_claim(RES_PORT,base,base+8,"uart",false)>=0);
        assert(registry_claim(RES_IRQ,irq,irq+1,"uart",false)>=0);
        assert(com_legacy_attach(irq,logger));
        assert(admit(0,1,irq,CIUKI_COM_BDA)==0 && !reads && !writes);
        uint32_t g; assert(com_open(0,&g)==-EBUSY && !reads && !writes);
        assert(admit(1,2,irq,CIUKI_COM_BDA)==1 && !com_open(1,&g));
        uint8_t c=0x42; receive(&models[1],c); assert(com_transfer(1,g,&c,1,true)==1);
        interrupt(irq); assert(logger_calls==1 && models[1].used==0);
        interrupt(irq); assert(models[1].tx_used==1 && models[1].tx[0]==c);
        assert(!models[0].tx_used && !models[0].fcr && models[0].regs[3]==3);
        com_close(1,g);
    }
    reset(); make_model(0,0x3f8,CIUKI_COM_16550A); make_model(1,0x3e8,CIUKI_COM_16550A);
    assert(admit(0,1,4,CIUKI_COM_BDA)==0 && admit(1,3,4,CIUKI_COM_BDA)==2);
    uint32_t a,b; assert(!com_open(0,&a) && !com_open(2,&b));
    receive(&models[0],11); receive(&models[1],22); interrupt(4);
    uint8_t c; assert(com_transfer(0,a,&c,1,false)==1 && c==11);
    assert(com_transfer(2,b,&c,1,false)==1 && c==22);
    puts("PASS COM aggregate IRQ3/4 with logger and two native ports, logger registers/bytes untouched");
}
static void faults_and_abi(void)
{
    reset(); make_model(0,0x2f8,CIUKI_COM_16550A); assert(admit(0,2,3,CIUKI_COM_PNP)==1);
    uint32_t g; fail_alloc=true; assert(com_open(1,&g)==-ENOMEM); fail_alloc=false; assert(!com_open(1,&g));
    assert(com_wait(1,g,false,0,cancelled)==-EAGAIN);
    assert(com_wait(1,g,false,1,cancelled)==-ETIMEDOUT);
    cancel_wait=true; assert(com_wait(1,g,false,60000,cancelled)==-EINTR); cancel_wait=false;
    struct ciuki_com_line l=default_line(); l.baud=12345;
    assert(com_control(1,g,COM_SET_LINE,&l,sizeof(l),true)==-EINVAL);
    l=default_line(); assert(com_control(1,g,COM_SET_LINE,&l,sizeof(l),false)==-EACCES);
    assert(com_control(1,g,COM_SET_LINE,&l,sizeof(l)-1,true)==-EINVAL);
    l.version=2; assert(com_control(1,g,COM_SET_LINE,&l,sizeof(l),true)==-EINVAL); l.version=1;
    for (unsigned parity=0;parity<=4;parity++) for (unsigned bits=5;bits<=8;bits++) {
        l.parity=parity; l.data_bits=bits; l.stop_bits=2;
        assert(!com_control(1,g,COM_SET_LINE,&l,sizeof(l),true));
        unsigned expected=bits-5 | 4 | (parity ? 8 | ((parity-1)<<4) : 0);
        assert(models[0].regs[3]==expected);
    }
    struct ciuki_com_modem modem={1,sizeof(modem),CIUKI_COM_DTR|CIUKI_COM_BREAK,0};
    assert(!com_control(1,g,COM_SET_MODEM,&modem,sizeof(modem),true));
    assert(models[0].regs[4]==9 && (models[0].regs[3]&0x40));
    models[0].modem=0x93; interrupt(3);
    struct ciuki_com_status s={.version=1,.size=sizeof(s)};
    assert(!com_control(1,g,COM_GET_STATUS,&s,sizeof(s),false) && s.modem==0x93);
    assert(com_control(1,g,0,0,0,true)==-ENOTTY);
    uint8_t c=1; assert(com_transfer(1,g,&c,1,true)==1);
    assert(com_control(1,g,COM_SET_LINE,&l,sizeof(l),true)==-EBUSY); interrupt(3);
    models[0].stuck=true; com_close(1,g); assert(com_open(1,&g)==-ENODEV);
    reset(); make_model(0,0x2f8,CIUKI_COM_16550A); models[0].fcr=0xc1;
    assert(admit(0,2,3,CIUKI_COM_BDA)==1 && !ports[1].status.flags && models[0].fcr==0xc1);
    reset(); make_model(0,0x2f8,CIUKI_COM_16750); models[0].clone64=true;
    assert(admit(0,2,3,CIUKI_COM_BDA)==1 && ports[1].status.chip==CIUKI_COM_16550A && ports[1].status.fifo_depth==16);
    reset(); make_model(0,0x2f8,CIUKI_COM_16650); models[0].depth_override=16;
    assert(admit(0,2,3,CIUKI_COM_BDA)==1 && ports[1].status.chip==CIUKI_COM_16550A && ports[1].status.fifo_depth==16);
    reset(); make_model(0,0x2f8,CIUKI_COM_16550A); models[0].absent=true;
    assert(admit(0,2,3,CIUKI_COM_BDA)==1 && !ports[1].status.flags);
    reset(); make_model(0,0x2f8,CIUKI_COM_16550A); models[0].bad_divisor=true;
    assert(admit(0,2,3,CIUKI_COM_BDA)==1 && !ports[1].status.flags && models[0].divisor[0]==3 && models[0].regs[3]==3);
    reset(); make_model(0,0x2f8,CIUKI_COM_16550A);
    assert(registry_claim(RES_IRQ,3,4,"other-device",false)>=0);
    assert(admit(0,2,3,CIUKI_COM_BDA)==1 && !ports[1].status.flags && !reads && !writes);
    reset(); make_model(0,0x2f8,CIUKI_COM_16650); models[0].efr=0xd0;
    assert(admit(0,2,3,CIUKI_COM_BDA)==1 && models[0].efr==0xd0 && !com_open(1,&g) && !models[0].efr);
    com_close(1,g);
    reset(); make_model(0,0x2f8,CIUKI_COM_16550A); assert(admit(0,2,3,CIUKI_COM_BDA)==1 && !com_open(1,&g));
    fail_work=true; receive(&models[0],1); interrupt(3); assert(ports[1].status.flags&CIUKI_COM_QUARANTINED); fail_work=false;
    com_close(1,g);
    reset(); make_model(0,0x2f8,CIUKI_COM_16550A); assert(admit(0,2,3,CIUKI_COM_BDA)==1 && !com_open(1,&g));
    registry_quarantine(ports[1].claim,ports[1].claim_generation);
    unsigned before=writes;
    assert(com_transfer(1,g,&c,1,true)==-ENODEV && writes==before);
    struct ciuki_com_status revoked; assert(com_snapshot(1,&revoked,0) && (revoked.flags&CIUKI_COM_QUARANTINED));
    com_close(1,g); assert(com_open(1,&g)==-ENODEV);
    puts("PASS COM ABI validation, modem/break, rights, timeouts/cancellation, faults, clone fallback and quarantine");
}
static void aggregate_budget(void)
{
    reset(); uint32_t generations[8]; uint8_t data[32]; memset(data,0x65,sizeof(data));
    for (unsigned i=0;i<8;i++) {
        make_model(i,0x1000+i*8,CIUKI_COM_16650);
        assert(admit(i,i+1,4,CIUKI_COM_PNP)==(int)i && !com_open(i,&generations[i]));
        assert(com_transfer(i,generations[i],data,sizeof(data),true)==sizeof(data));
        for (unsigned k=0;k<32;k++) receive(&models[i],(uint8_t)k);
    }
    interrupt(4);
    for (unsigned i=0;i<8;i++) assert(ports[i].status.rx_queued==32 && ports[i].status.transmitted==16);
    interrupt(4);
    for (unsigned i=0;i<8;i++) assert(ports[i].status.transmitted==32);
    receive(&models[0],0x44);
    struct trap_frame tf={.vector=0x24}; handlers[4](&tf);
    assert(ports[0].deferred); com_close(0,generations[0]);
    uint32_t next; assert(com_open(0,&next)==-EBUSY); drain_work();
    assert(!com_open(0,&next) && next!=generations[0] && !ports[0].status.rx_queued);
    puts("PASS COM eight-port aggregate <=170 register accesses per hard IRQ, full-duplex progress and stale deferred-close isolation");
}
static void discovery(void)
{
    reset(); make_model(0,0x2f8,CIUKI_COM_16550A);
    assert(admit(0,2,3,CIUKI_COM_BDA)==1);
    unsigned prior=writes; assert(com_pnp_assigned("PNP0501",0x2f8,8,4)==1);
    assert(writes==prior && ports[1].status.irq==4 && ports[1].status.number==2);
    uint32_t assigned; assert(!com_open(1,&assigned)); receive(&models[0],0x43); interrupt(4);
    uint8_t assigned_byte; assert(com_transfer(1,assigned,&assigned_byte,1,false)==1 && assigned_byte==0x43);
    reset(); make_model(0,0x2f8,CIUKI_COM_16450); uint16_t bda[]={0,0x2f8,0xffff,0x2f8};
    com_bda_discover(bda); assert(!ports[0].present && ports[1].present && !ports[2].present && !ports[3].present);
    unsigned before=writes;
    assert(com_pnp_assigned("PNP0501",0x2f8,8,3)==1 && writes==before);
    assert(com_pnp_assigned("PNP0501",0x2fc,8,3)==-EBUSY && writes==before);
    assert(com_pnp_assigned("PNP0400",0x378,8,7)==-EINVAL);
    struct com_resource r={0,0x1234,0,CIUKI_COM_BDA,1843200,1,false};
    assert(com_admit(&r,&io)==0 && !ports[0].status.flags && writes==before);
    r.base=0xfffc; assert(com_admit(&r,&io)==-EINVAL); r.base=0x3f8; r.stride=4;
    assert(com_admit(&r,&io)==-EINVAL); r.stride=1; r.clock=0; assert(com_admit(&r,&io)==-EINVAL);
    make_model(1,0x1234,CIUKI_COM_16550A);
    assert(com_pnp_assigned("PNP0501",0x1234,8,3)==0 && (ports[0].status.flags&CIUKI_COM_READY) && ports[0].status.number==1);
    reset(); for (unsigned i=0;i<4;i++) make_model(i,0x1000+i*8,CIUKI_COM_16550A);
    struct pci_func pci={.vendor=0x1415,.device=0x9501,.class_code=7,.irq_line=11,.irq_pin=1,.command=1,.bar={0x1001}};
    com_pci_discover(&pci); assert(chain_count==4);
    for (unsigned i=0;i<4;i++) assert(ports[i].status.flags==CIUKI_COM_READY && ports[i].resource.origin==CIUKI_COM_PCI);
    uint32_t g; assert(!com_open(2,&g)); receive(&models[2],0x55);
    for (unsigned i=0;i<chain_count;i++) (void)chain[i](11,chain_names[i]); drain_work();
    uint8_t c; assert(com_transfer(2,g,&c,1,false)==1 && c==0x55);
    reset(); pci.irq_pin=0; com_pci_discover(&pci); assert(!writes && ports[0].present && !ports[0].status.flags);
    reset(); pci.irq_pin=1; pci.sub_vendor=0x131f; com_pci_discover(&pci); assert(!writes && !ports[0].present);
    for (unsigned id=2;id<=4;id++) {
        reset(); unsigned count=1u<<(id-2);
        for (unsigned i=0;i<count;i++) make_model(i,0x1000+i*8,CIUKI_COM_16550A);
        pci=(struct pci_func){.vendor=0x1b36,.device=id,.class_code=7,.subclass=id==2 ? 0 : 2,
            .irq_line=11,.irq_pin=1,.command=1,.bar={0x1001}};
        com_pci_discover(&pci); assert(chain_count==count);
        for (unsigned i=0;i<count;i++) assert(ports[i].status.flags==CIUKI_COM_READY);
    }
    printf("PASS COM BDA/PnP/PCI resource validation, qualified shared PCI, metadata=%zu open_queues=%u bytes\n",sizeof(ports),2*CIUKI_COM_QUEUE);
}
static void wait_receive(void) { receive(&models[0],0x77); interrupt(3); }
static void native(void)
{
    reset(); make_model(0,0x2f8,CIUKI_COM_16550A); assert(admit(0,2,3,CIUKI_COM_BDA)==1);
    struct px_node node={.kind=PX_COM,.drive=1};
    struct file_description d={.node=&node,.flags=O_RDWR,.references=1,.object={user_retain,user_release,false}};
    user_description=&d; user_process.memory=&user_memory; user_thread.process=&user_process;
    assert(com_device_open(&d,O_RDWR|O_APPEND)==-EINVAL && !d.com_generation);
    assert(!com_device_open(&d,O_RDWR));
    assert(com_rw_user(&d,0x1000,1,false)==-EAGAIN);
    user_fault=true; unsigned before=writes;
    assert(com_rw_user(&d,0x1000,1,true)==-EFAULT && writes==before);
    assert(com_devctl(&user_process,5,COM_SET_LINE,0x1000,sizeof(struct ciuki_com_line))==-EFAULT && writes==before);
    user_fault=false;
    struct ciuki_com_line line=default_line(); line.read_timeout_ms=30;
    memcpy(user_bytes,&line,sizeof(line));
    assert(!com_devctl(&user_process,5,COM_SET_LINE,0x1000,sizeof(line)));
    assert(com_rw_user(&d,0x1000,1,false)==-ETIMEDOUT);
    wait_hook=wait_receive; assert(com_rw_user(&d,0x1000,1,false)==1 && user_bytes[0]==0x77); wait_hook=0;
    user_signal=true; assert(com_rw_user(&d,0x1000,1,true)==-EINTR); user_signal=false;
    d.flags=O_RDONLY; assert(com_rw_user(&d,0x1000,1,true)==-EBADF);
    memcpy(user_bytes,&line,sizeof(line)); assert(com_devctl(&user_process,5,COM_SET_LINE,0x1000,sizeof(line))==-EACCES);
    d.flags=O_WRONLY; assert(com_rw_user(&d,0x1000,1,false)==-EBADF); d.flags=O_RDWR|O_NONBLOCK;
    memset(user_bytes,0x61,sizeof(user_bytes)); assert(com_rw_user(&d,0x1000,1500,true)==1024);
    assert(com_rw_user(&d,0x1000,1,true)==-EAGAIN);
    while (ports[1].status.tx_queued) interrupt(3);
    struct ciuki_com_status s={.version=1,.size=sizeof(s)}; memcpy(user_bytes,&s,sizeof(s));
    assert(!com_devctl(&user_process,5,COM_GET_STATUS,0x1000,sizeof(s)));
    memcpy(&s,user_bytes,sizeof(s)); assert(s.transmitted==1024 && s.generation==d.com_generation);
    assert(com_devctl(&user_process,6,COM_GET_STATUS,0x1000,sizeof(s))==-EBADF);
    assert(!user_pins && d.references==1 && !user_thread.cleanup && allocations==1);
    struct ciuki_com_modem outputs={1,sizeof(outputs),CIUKI_COM_BREAK,0};
    assert(!com_control(1,d.com_generation,COM_SET_MODEM,&outputs,sizeof(outputs),true));
    com_device_close(&d); assert(!d.com_generation && !allocations); com_device_close(&d);
    assert(!com_device_open(&d,O_RDWR) && models[0].regs[3]==3); com_device_close(&d);
    puts("PASS COM native checked buffers/pins, short I/O, rights, devctl, finite waits, signal cancellation and cleanup");
}
int main(void) { setvbuf(stdout,0,_IONBF,0); generations(); arbitration(); faults_and_abi(); aggregate_budget(); discovery(); native(); reset(); return 0; }
