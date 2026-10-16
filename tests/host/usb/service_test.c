/* Native usb_init/service lifecycle against the QEMU-style register model.
 * Replace hardware and scheduling only; compile the native core unchanged.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main usb_model_main
#include "usb_test.c"
#undef main
#include <setjmp.h>
#include <ciuki/task.h>
#define CIUKI_CPU_H
#define P2V(p) model_page(p)

static struct task worker, input_worker;
static void (*worker_fn)(void *);
static void *worker_arg;
static void (*input_fn)(void *);
static void *input_arg;
static unsigned created, started, allocations;
static bool create_failed, input_create_failed, dispatched, input_dispatched, no_pci;
static uint64_t input_stop_at;
static bool phased_replay;
static uint64_t phase_us, phase_fixed_deadline;
static unsigned phase_frames, phase_ticks, phase_waits;
static bool completion_replay;
static bool completion_wait(struct kwait *,kwait_cond_fn,void *,uint64_t);
static uint32_t config_address, flags=0x200;
static void (*restore_hook)(void);
static jmp_buf service_done;
volatile uint64_t g_ticks;

static void sync_model(void)
{
    if(usb_count()) {
        const struct usb_host *h=usb_get(0);
        m.sleep_host=(struct usb_host *)h;m.host.dma=h->dma;m.host.dma_phys=h->dma_phys;m.host.claims[2]=h->claims[2];
    }
}
static void *model_page(uint32_t phys) { assert(phys==0x100000);return m.dma; }
uint32_t pmm_alloc_contig(uint32_t pages) { assert(pages==USB_DMA_BYTES/4096);allocations++;return 0x100000; }
void pmm_free(uint32_t phys) { (void)phys;assert(!"unexpected DMA release"); }
void *vmm_map_mmio(uint32_t phys,uint32_t bytes,bool uncached)
{ (void)phys;(void)bytes;(void)uncached;assert(!"UHCI needs no MMIO mapping");return NULL; }
unsigned pci_count(void) { return no_pci ? 0:1; }
const struct pci_func *pci_get(unsigned i)
{
    static const struct pci_func p={.vendor=0x8086,.device=0x7020,.class_code=0x0c,
        .subclass=3,.irq_line=11,.irq_pin=1,.bar={[4]=0xc001}};
    assert(!i);return &p;
}
static uint32_t irq_save(void) { uint32_t f=flags;flags=0;return f; }
static void irq_restore(uint32_t f) { flags=f;if((f&0x200) && restore_hook)restore_hook(); }
static uint32_t port_read(uint16_t port,unsigned width)
{
    sync_model();
    if(port>=0xcfc && port<=0xcff) return config_read(&m,(config_address&0xfcu)+port-0xcfc,width);
    assert(port>=0xc000 && port<0xc020);return read_reg(&m,port-0xc000,width);
}
static void port_write(uint16_t port,uint32_t value,unsigned width)
{
    if(port==0xcf8) { config_address=value;return; }
    sync_model();
    if(port>=0xcfc && port<=0xcff) config_write(&m,(config_address&0xfcu)+port-0xcfc,value,width);
    else { assert(port>=0xc000 && port<0xc020);write_reg(&m,port-0xc000,value,width); }
}
static uint8_t inb(uint16_t port) { return (uint8_t)port_read(port,1); }
static uint16_t cpu_inw(uint16_t port) { return (uint16_t)port_read(port,2); }
static uint32_t inl(uint16_t port) { return port_read(port,4); }
static void outb(uint16_t port,uint8_t value) { port_write(port,value,1); }
static void cpu_outw(uint16_t port,uint16_t value) { port_write(port,value,2); }
static void outl(uint16_t port,uint32_t value) { port_write(port,value,4); }
struct task *task_create_kernel(const char *name,void (*fn)(void *),void *arg,enum task_prio prio)
{
    assert(prio==P_DEVICE && fn);created++;
    if(!strcmp(name,"usb-hid")) {
        input_worker.state=T_BLOCKED;input_worker.prio=prio;input_fn=fn;input_arg=arg;
        return input_create_failed ? NULL:&input_worker;
    }
    assert(!strcmp(name,"usb-host") && arg);
    if(create_failed) return NULL;
    snprintf(worker.name,sizeof(worker.name),"%s",name);
    worker.state=T_BLOCKED;worker.prio=prio;worker_fn=fn;worker_arg=arg;return &worker;
}
void task_start(struct task *t) { assert((t==&worker || t==&input_worker) && t->state==T_BLOCKED);t->state=T_READY;started++; }
void task_sleep_ms(uint32_t ms)
{
    assert(flags&0x200);
    sync_model();
    if(input_dispatched && m.now>=input_stop_at) longjmp(service_done,1);
    /* Leave only at the service's outer sleep, with the transfer lock free. */
    if(dispatched && !usb_get(0)->busy && m.now>=2100) longjmp(service_done,1);
    sleep_ms(&m,ms);g_ticks=m.now;
}
uint64_t deadline_after_ms(uint32_t ms) { return g_ticks+ms; }
static void phase_event(void)
{
    /* PIT and USB are independent clocks: PIT at n ms, USB at n+0.5 ms.
     * This runs events inside the actual native sleep, without scheduling
     * the clocks together or rounding each USB notification up to a tick. */
    uint64_t pit=(g_ticks+1)*1000, frame=phase_us/1000*1000+500;
    if(frame<=phase_us)frame+=1000;
    assert(!pthread_mutex_trylock(&m.host.transfer_lock));
    fs_lock_drop(&m.host.transfer_lock);
    if(frame<pit) {
        phase_us=frame;phase_frames++;
        m.regs[0]|=1u<<16;assert(usb_host_irq(&m.host));
    } else {
        phase_us=pit;phase_ticks++;m.now=++g_ticks;
    }
}
bool kwait_wait_until(struct kwait *q,kwait_cond_fn condition,void *arg,uint64_t deadline)
{
    if(completion_replay)return completion_wait(q,condition,arg,deadline);
    if(phased_replay) {
        assert(q==&m.host.changed && arg==&m.host);
        if(phase_fixed_deadline)assert(deadline==phase_fixed_deadline);
        phase_waits++;
        while(!condition(arg) && g_ticks<deadline)phase_event();
        return condition(arg);
    }
    (void)q;
    if(!condition(arg))task_sleep_ms((uint32_t)(deadline-g_ticks));
    return condition(arg);
}

#undef CIUKI_USB_HOST
#include "../../../src/kernel/usb/usb_core.c"

/* Run the production HID task with independent PIT n ms / HC n+0.5 ms
 * clocks. Only the register model and wait/scheduler boundaries are fake. */
static uint64_t completion_us, completion_end, inject_us, ack_us[2], serviced_us[2];
static unsigned timer_wakes, notification_wakes, idle_wakes;
static bool deliver_irq, injected, input_notified;
static void completion_wake(struct kwait *q)
{
    if(q==&input_changed)input_notified=true;
    else assert(q==&hosts[0].changed);
}
static void completion_ack(struct model *model,struct peripheral *p)
{
    unsigned i=(unsigned)(p-model->dev);assert(i<2 && !ack_us[i]);
    ack_us[i]=completion_us;
}
static void completion_config_write(void *arg,unsigned off,uint32_t value,unsigned width)
{ sync_model();config_write(arg,off,value,width); }
static bool completion_wait(struct kwait *q,kwait_cond_fn condition,void *arg,uint64_t deadline)
{
    struct usb_host *h=&hosts[0];
    assert(q==&input_changed && condition==hid_pending && !arg && deadline==g_ticks+1);
    for(unsigned i=0;i<2;i++) if(ack_us[i] && !serviced_us[i] && h->devices[i].hid[0].pipe_state==2)
        serviced_us[i]=completion_us;
    if(completion_us>=completion_end)longjmp(service_done,1);
    while(!condition(arg) && g_ticks<deadline) {
        uint64_t pit=(g_ticks+1)*1000, frame=completion_us/1000*1000+500;
        if(frame<=completion_us)frame+=1000;
        if(frame<pit) {
            completion_us=frame;
            if(inject_us && !injected && completion_us>=inject_us) {
                m.dev[0].report[2]=0x45;m.dev[0].report_len=8;
                m.dev[1].report[1]=3;m.dev[1].report[2]=5;m.dev[1].report_len=4;
                injected=true;
            }
            /* The model HC frame advances independently of the PIT tick. */
            sleep_ms(&m,1);
            unsigned status=read_reg(&m,m.ohci ? 0x0c:2,m.ohci ? 4:2);
            bool asserted=m.ohci ? (read_reg(&m,0x10,4)&(1u<<31)) &&
                (status&read_reg(&m,0x10,4)&0x7f) : (status&0x1f)!=0;
            if(deliver_irq && h->irq_attached && asserted) {
                uint32_t saved=irq_save();assert(usb_host_irq(h));irq_restore(saved);
            }
        } else {completion_us=pit;g_ticks++;}
    }
    if(condition(arg)) {
        assert(input_notified);input_notified=false;notification_wakes++;
    } else timer_wakes++;
    return condition(arg);
}
static void completion_workers(const char *mode)
{
    bool ohci=strstr(mode,"ohci")!=NULL, poll=strstr(mode,"poll")!=NULL;
    setup(ohci,false);route_ready=!poll;deliver_irq=strstr(mode,"-irq")!=NULL;
    host_count=1;
    struct pci_func p=*pci_get(0);p.prog_if=ohci ? 0x10:0;
    p.bar[ohci ? 0:4]=ohci ? 0xf0000000:0xc001;
    struct usb_io callbacks=io(&m);
    callbacks.config_write=completion_config_write;
    assert(!usb_attach(&hosts[0],&p,&callbacks,m.dma,0x100000));sync_model();
    struct usb_host *h=&hosts[0];usb_step(h);
    /* Settle retirement and arm both real HID TDs before measuring idle. */
    for(unsigned n=0;n<20;n++) {sleep_ms(&m,1);usb_hid_service(h);}
    assert(h->device_count==2 && h->devices[0].hid[0].pipe_state==1 && h->devices[1].hid[0].pipe_state==1);
    if(!ohci) for(unsigned i=0;i<2;i++) {
        unsigned at=USB_HID_BASE+i*USB_HID_MAX*USB_HID_STRIDE+16;
        assert(!!(*(uint32_t *)(m.dma+at+4)&(1u<<24))==!poll);
    }
    write_reg(&m,ohci ? 0x0c:2,ohci ? 0x7f:0x1f,ohci ? 4:2);
    g_ticks=m.now;completion_us=g_ticks*1000;completion_end=completion_us+1000000;
    h->irq_pending=input_pending=false;wake_hook=completion_wake;completion_replay=true;
    unsigned interrupts=h->interrupts;
    if(!setjmp(service_done))input_service(NULL);
    idle_wakes=timer_wakes+notification_wakes;
    assert(idle_wakes==1000 && !notification_wakes && h->interrupts==interrupts);
    unsigned idle_interrupts=h->interrupts-interrupts;
    timer_wakes=notification_wakes=0;
    inject_us=completion_us+500;completion_end=completion_us+20000;report_ack_hook=completion_ack;
    if(!setjmp(service_done))input_service(NULL);
    unsigned max_latency=0;
    for(unsigned i=0;i<2;i++) {
        assert(ack_us[i] && serviced_us[i]>=ack_us[i]);
        unsigned latency=(unsigned)(serviced_us[i]-ack_us[i]);
        assert(latency<=1000 && (!deliver_irq || !latency));
        if(latency>max_latency)max_latency=latency;
    }
    assert(held[0x58] && last_x==3 && last_y==5); /* real queue delivery after retirement */
    assert(deliver_irq ? notification_wakes && h->interrupts>interrupts : !notification_wakes && h->interrupts==interrupts);
    assert(h->ready && !h->quarantined);
    printf("PASS USB workers %s: idle_wakes_per_second=%u idle_interrupts=%u completion_to_service_max_us=%u notification_wakes=%u timer_wakes=%u reports_delivered=2\n",
           mode,idle_wakes,idle_interrupts,max_latency,notification_wakes,timer_wakes);
}

static void phase_early_sleep(void *arg,unsigned ms)
{ assert(arg==&m && ms==1);phase_event(); }
static void phase_native_sleep(void *arg,unsigned ms)
{ assert(arg==&m);native_sleep(&m.host,ms); }
static void phased_delays(void)
{
    route_ready=true;assert(!attach());phased_replay=true;
    g_ticks=m.now=phase_us=0;phase_fixed_deadline=50;
    native_sleep(&m.host,50);
    assert(phase_us==50000 && phase_ticks==50 && phase_frames==50 && phase_waits==51);
    phase_fixed_deadline=0;
    usb_host_take(&m.host);
    /* Also replay an I/O backend that returns at every PIT/USB event:
     * usb_sleep itself must count elapsed time, never wakeup iterations. */
    m.host.io.sleep=phase_early_sleep;
    uint64_t pause_begin=phase_us;
    usb_poll_pause(&m.host,50);
    assert(phase_us-pause_begin==25000); /* only optional retry backoff may end early */
    const unsigned delays[]={50,2,100}; /* reset, address recovery, debounce */
    for(unsigned i=0;i<sizeof(delays)/sizeof(delays[0]);i++) {
        uint64_t begin=phase_us;
        unsigned frames=phase_frames,ticks=phase_ticks;
        usb_sleep(&m.host,delays[i]);
        assert(phase_us-begin==(uint64_t)delays[i]*1000);
        assert(phase_frames-frames==delays[i] && phase_ticks-ticks==delays[i]);
    }
    m.host.io.sleep=phase_native_sleep;
    uint64_t begin=phase_us;
    assert(!m.host.ops->port_reset(&m.host,0));
    assert(phase_us-begin==61000 && m.root_resets[0]==1 && m.root_enables[0]==1);
    assert(m.recovery_at[0]-m.reset_at[0]==50);
    usb_host_drop(&m.host);
    puts("PASS USB phased delays: PIT n ms, USB n+0.5 ms; native 50 ms deadline survives 50 IRQ wakes; reset/address/debounce 50/2/100 ms; production root reset 61 ms; optional retry backoff separate");
}
static void fatal_isolation(const char *mode)
{
    route_ready=true;assert(!attach());
    bool relatched=!strcmp(mode,"fatal-relatched");
    m.host_error=!strcmp(mode,"fatal-persistent");
    m.fatal_reassert=!strcmp(mode,"fatal-reasserted");
    m.gate_stuck=!strcmp(mode,"fatal-gate-stuck");
    m.source_stuck=!strcmp(mode,"fatal-source-stuck");
    m.run_stuck=!strcmp(mode,"fatal-run-stuck");
    m.halt_stuck=!strcmp(mode,"fatal-halt-stuck");
    m.bme_stuck=!strcmp(mode,"fatal-bme-stuck");
    /* No worker may run before source containment, even when every W1C
     * reasserts HCPE. Simulate dispatcher EOI only after this assertion. */
    m.regs[0]|=8u<<16;
    uint64_t tick=m.now;
    uint32_t f=irq_save();
    assert(usb_host_irq(&m.host) && (m.host.irq_status&8));
    assert(m.now==tick && !m.host.quarantined && !m.host.ohci_done_count);
    assert(irq_masked==!relatched); /* before the first EOI, zero worker turns */
    irq_restore(f);
    if(m.fatal_reassert) {
        assert(read_reg(&m,2,2)&0x10);
        assert(leases[m.host.claims[1]].state==RS_QUARANTINED);
        puts("PASS USB fatal reassert-after-every-W1C: IRQ11 masked before EOI in 1 pass, 0 worker turns, no sleeping or clock advance");
    }
    if(!m.source_stuck)assert(!(read_reg(&m,4,2)&15));
    for(unsigned i=0;i<3;i++) {
        m.regs[0]|=0x10u<<16;
        unsigned before=writes;
        assert(usb_host_irq(&m.host) && writes>before && m.host.irq_status==0x18);
    }
    usb_step(&m.host);
    assert(m.host.quarantined && !m.host.ready);
    if(relatched) {
        assert(!m.host.irq_attached && irq_removed==1 && !irq_masked);
        assert(!(config_read(&m,0xc0,2)&0x20bf) && !(read_reg(&m,2,2)&0x1f));
    } else {
        assert(m.host.irq_attached && !irq_removed && irq_masked);
        assert(leases[m.host.claims[1]].state==RS_QUARANTINED);
        /* Even an unexpected dispatch after quarantine cannot abandon an
         * unacknowledged source. The production registry keeps IRQ11 masked. */
        m.regs[0]|=8u<<16;
        unsigned before=writes;
        assert(usb_host_irq(&m.host) && writes>before);
    }
    printf("PASS USB fatal isolation %s: repeated HSE/HCPE acknowledged after fatal snapshot; %s\n",
           mode,relatched ? "gate/source/status/BME verified before detach":"failed readback retains callback and quarantines masked IRQ11");
}
static unsigned interleave_irqs;
static void publish_second_done(void)
{
    assert(flags&0x200);restore_hook=NULL;
    assert(m.host.ohci_done_count==1 && !*(uint32_t *)(m.dma+0x84));
    *(uint32_t *)(m.dma+0x84)=m.host.dma_phys+USB_HID_BASE+16;
    m.regs[0x0c/4]|=2;
    uint32_t f=irq_save();
    assert(usb_host_irq(&m.host));interleave_irqs++;
    assert(!flags && m.host.ohci_done_count==2);
    irq_restore(f);
}
static void schedule_done_irq(struct model *model)
{
    assert(model==&m && !flags && !*(uint32_t *)(m.dma+0x84));
    assert(m.host.ohci_done_count==1);
    wdh_ack_hook=NULL;restore_hook=publish_second_done;
}
static void done_head_interleave(void)
{
    setup(true,false);route_ready=true;assert(!attach());
    *(uint32_t *)(m.dma+USB_TD+8)=0;
    *(uint32_t *)(m.dma+USB_HID_BASE+24)=0;
    *(uint32_t *)(m.dma+0x84)=m.host.dma_phys+USB_TD;
    m.regs[0x0c/4]|=2;
    uint64_t tick=m.now;
    wdh_ack_hook=schedule_done_irq;
    fs_lock_take(&m.host.transfer_lock);
    usb_ohci_service_done(&m.host); /* IRQ is delivered as polling restores IF */
    fs_lock_drop(&m.host.transfer_lock);
    assert(flags==0x200 && interleave_irqs==1 && !m.host.ohci_done_count);
    assert(m.host.ohci_done_batches==2 && m.host.ohci_done_tds==2);
    assert(!*(uint32_t *)(m.dma+0x84) && !(read_reg(&m,0x0c,4)&2));
    assert(m.host.ready && !irq_masked && m.now==tick);
    puts("PASS USB OHCI polling/IRQ WDH interleave: capture and ack with IF=0, IRQ delivered on restore, both done lists consumed exactly once without sleeping");
}
int main(int argc,char **argv)
{
    assert(argc==2);setup(false,false);
    if(!strcmp(argv[1],"done-head-interleave")) {done_head_interleave();return 0;}
    if(!strncmp(argv[1],"completion-",11)) {completion_workers(argv[1]);return 0;}
    if(!strcmp(argv[1],"phased-delays")) {phased_delays();return 0;}
    if(!strncmp(argv[1],"fatal-",6)) {fatal_isolation(argv[1]);return 0;}
    bool diagnostic=!strcmp(argv[1],"diagnostic");
    if(diagnostic) g_boot.flags=CBI_F_DIAG_VERBOSE;
    bool empty=!strcmp(argv[1],"empty");
    if(empty) m.dev[0].present=m.dev[1].present=false;
    create_failed=!strcmp(argv[1],"create-failed");
    input_create_failed=!strcmp(argv[1],"input-create-failed");
    no_pci=!strcmp(argv[1],"none");
    bool policy=false;
    if(!strcmp(argv[1],"safe")) {g_boot.flags=CBI_F_SAFE_MODE;policy=true;}
    if(!strcmp(argv[1],"forced")) {g_boot.flags=CBI_F_INPUT_FORCED;policy=true;}
    if(!strcmp(argv[1],"firmware")) {g_boot.input_policy=CBI_INPUT_FIRMWARE;policy=true;}
    unsigned before=writes,c=claims;
    usb_init();
    if(policy || no_pci) {
        assert(!usb_count() && !created && !started && !allocations && writes==before && claims==c);
    } else if(create_failed || input_create_failed) {
        assert(created==2 && !started && usb_count()==1 && usb_get(0)->quarantined);
        assert(!usb_get(0)->ready && !(m.config[1]&4) && !strcmp(usb_get(0)->reason,"service_thread_failed"));
    } else {
        assert(created==2 && started==2 && worker.state==T_READY);
        assert(worker_arg==usb_get(0) && !strcmp(worker.name,"usb-host"));
        assert(usb_get(0)->ready && !usb_get(0)->device_count);
        assert(!usb_input_ready()); /* a controller alone is not an input device */
        assert(!service_entries && service_queues==1 && !first_scans);
        if(!strcmp(argv[1],"csc-cleared")) m.regs[0x10/4]&=~0x00020002u;
        if(!setjmp(service_done)) { dispatched=true;worker.state=T_RUNNING;worker_fn(worker_arg);assert(!"service returned"); }
        const struct usb_host *h=usb_get(0);
        assert(h->ready && !h->quarantined && h->device_count==(empty?0u:2u) && !h->busy);
        assert(first_scans==1 && service_entries==1);
        assert(diagnostic ? cadence_records>=2 : cadence_records==0);
        if(empty) {
            assert(!usb_input_ready());
            assert(!connect_records && disconnect_records==2 && !descriptor_starts && !descriptor_results && !retired_packets);
            assert(!m.root_resets[0] && !m.root_resets[1] && !m.root_enables[0] && !m.root_enables[1]);
        } else {
            assert(usb_input_ready() && usb_devices==2);
            assert(h->devices[0].hid[0].active && h->devices[1].hid[0].active);
            assert(m.root_resets[0]==1 && m.root_resets[1]==1 && m.root_enables[0]==1 && m.root_enables[1]==1);
            assert(connect_records==2 && descriptor_starts==2 && descriptor_results==2 && !descriptor_failures && retired_packets);
        }
        if(!strcmp(argv[1],"hid-worker")) {
            /* The general owner is asleep/reserved; dispatch only the real
             * input thread. It must neither scan nor borrow the general TD. */
            struct usb_host *host=(struct usb_host *)h;
            uint8_t general[64];memcpy(general,host->dma+USB_TD,sizeof(general));
            unsigned scans=host->scan_count;
            m.dev[0].report[2]=0x45;m.dev[0].report_len=8;
            m.dev[1].report[1]=3;m.dev[1].report[2]=5;m.dev[1].report_len=4;
            usb_host_take(host);fs_lock_drop(&host->transfer_lock);
            m.check_sleep_lock=true;input_stop_at=m.now+40;
            if(!setjmp(service_done)) {
                input_dispatched=true;input_worker.state=T_RUNNING;input_fn(input_arg);
                assert(!"input service returned");
            }
            input_dispatched=false;
            assert(h->busy && h->scan_count==scans && held[0x58] && last_x==3 && last_y==5);
            assert(!memcmp(general,host->dma+USB_TD,sizeof(general)));
            fs_lock_take(&host->transfer_lock);usb_host_drop(host);
        }
        unsigned scanned=h->device_count,old_started=started,old_created=created;
        usb_init();assert(h->device_count==scanned && started==old_started && created==old_created);
    }
    printf("PASS USB native lifecycle %s (production init/service, blocked-until-start scheduler, QEMU PORTSC)\n",argv[1]);
    return 0;
}
