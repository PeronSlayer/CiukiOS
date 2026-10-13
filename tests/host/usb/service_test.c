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

static struct task worker;
static void (*worker_fn)(void *);
static void *worker_arg;
static unsigned created, started, allocations;
static bool create_failed, dispatched, no_pci;
static uint32_t config_address, flags=0x200;
static jmp_buf service_done;
volatile uint64_t g_ticks;

static void sync_model(void)
{
    if(usb_count()) {
        const struct usb_host *h=usb_get(0);
        m.host.dma=h->dma;m.host.dma_phys=h->dma_phys;m.host.claims[2]=h->claims[2];
    }
}
static void *model_page(uint32_t phys) { assert(phys==0x100000);return m.dma; }
uint32_t pmm_alloc_contig(uint32_t pages) { assert(pages==2);allocations++;return 0x100000; }
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
static void irq_restore(uint32_t f) { flags=f; }
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
    assert(!strcmp(name,"usb-host") && prio==P_DEVICE && fn);created++;
    if(create_failed) return NULL;
    worker.state=T_BLOCKED;worker.prio=prio;worker_fn=fn;worker_arg=arg;return &worker;
}
void task_start(struct task *t) { assert(t==&worker && t->state==T_BLOCKED);t->state=T_READY;started++; }
void task_sleep_ms(uint32_t ms)
{
    assert(flags&0x200);
    sync_model();
    /* Leave only at the service's outer sleep, with the transfer lock free. */
    if(dispatched && !usb_get(0)->busy && m.now>=2100) longjmp(service_done,1);
    sleep_ms(&m,ms);g_ticks=m.now;
}

#undef CIUKI_USB_HOST
#include "../../../src/kernel/usb/usb_core.c"

int main(int argc,char **argv)
{
    assert(argc==2);setup(false,false);
    bool empty=!strcmp(argv[1],"empty");
    if(empty) m.dev[0].present=m.dev[1].present=false;
    create_failed=!strcmp(argv[1],"create-failed");
    no_pci=!strcmp(argv[1],"none");
    bool policy=false;
    if(!strcmp(argv[1],"safe")) {g_boot.flags=CBI_F_SAFE_MODE;policy=true;}
    if(!strcmp(argv[1],"forced")) {g_boot.flags=CBI_F_INPUT_FORCED;policy=true;}
    if(!strcmp(argv[1],"firmware")) {g_boot.input_policy=CBI_INPUT_FIRMWARE;policy=true;}
    unsigned before=writes,c=claims;
    usb_init();
    if(policy || no_pci) {
        assert(!usb_count() && !created && !started && !allocations && writes==before && claims==c);
    } else if(create_failed) {
        assert(created==1 && !started && usb_count()==1 && usb_get(0)->quarantined);
        assert(!usb_get(0)->ready && !(m.config[1]&4) && !strcmp(usb_get(0)->reason,"service_thread_failed"));
    } else {
        assert(created==1 && started==1 && worker.state==T_READY);
        assert(usb_get(0)->ready && !usb_get(0)->device_count);
        assert(!service_entries && service_queues==1 && !first_scans);
        if(!strcmp(argv[1],"csc-cleared")) m.regs[0x10/4]&=~0x00020002u;
        if(!setjmp(service_done)) { dispatched=true;worker.state=T_RUNNING;worker_fn(worker_arg);assert(!"service returned"); }
        const struct usb_host *h=usb_get(0);
        assert(h->ready && !h->quarantined && h->device_count==(empty?0u:2u) && !h->busy);
        assert(first_scans==1 && cadence_records>=2 && service_entries==1);
        if(empty) {
            assert(!connect_records && disconnect_records==2 && !descriptor_starts && !descriptor_results && !retired_packets);
            assert(!m.root_resets[0] && !m.root_resets[1] && !m.root_enables[0] && !m.root_enables[1]);
        } else {
            assert(h->devices[0].hid[0].active && h->devices[1].hid[0].active);
            assert(m.root_resets[0]==1 && m.root_resets[1]==1 && m.root_enables[0]==1 && m.root_enables[1]==1);
            assert(connect_records==2 && descriptor_starts==2 && descriptor_results==2 && !descriptor_failures && retired_packets);
        }
        unsigned scanned=h->device_count,old_started=started,old_created=created;
        usb_init();assert(h->device_count==scanned && started==old_started && created==old_created);
    }
    printf("PASS USB native lifecycle %s (production init/service, blocked-until-start scheduler, QEMU PORTSC)\n",argv[1]);
    return 0;
}
