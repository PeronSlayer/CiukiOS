/* ISA FDC boot binding; all register work goes through the tested engine.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/task.h>
#include <ciuki/arch.h>
#include <ciuki/firmware.h>
#include <ciuki/storage.h>
#include "floppy.h"
static struct fdc controller;
static struct mm_pin buffer_pin;
static bool initialized;
static bool native_mounted[2];
static uint8_t real_read(void *arg, uint16_t port) { (void)arg; return inb(port); }
static void real_write(void *arg, uint16_t port, uint8_t b) { (void)arg; outb(port,b); }
static uint64_t real_now(void *arg)
{ (void)arg; uint32_t f=irq_save(); uint64_t tick=g_ticks; irq_restore(f); return tick; }
static void real_pause(void *arg, unsigned ms) { (void)arg; task_sleep_ms(ms); }
static uint32_t real_exclude(void *arg) { (void)arg; return irq_save(); }
static void real_restore(void *arg, uint32_t f) { (void)arg; irq_restore(f); }
static bool real_cancelled(void *arg) { (void)arg; return fs_request_check()!=0; }
static void physical_irq(struct trap_frame *f) { (void)f; fdc_irq(&controller); }
static void real_irq(void *arg, bool enable)
{
    (void)arg;
    if (enable) { irq_set_handler(6,physical_irq); pic_unmask(6); }
    else { pic_mask(6); irq_set_handler(6,0); }
}
static bool id(const struct ciuki_firmware_record *r)
{
    if (!strncmp(r->hid,"PNP0700",8)) return true;
    for (unsigned i=0;i<r->cid_count && i<4;i++) if (!strncmp(r->cid[i],"PNP0700",8)) return true;
    return false;
}
static void pnp_resources(struct fdc_evidence *e, const struct ciuki_firmware_record *r)
{
    bool main=false, dir=false, irq=false, dma=false;
    for (unsigned i=0;i<r->resource_count && i<8;i++) {
        const struct ciuki_firmware_resource *s=&r->resources[i];
        if (s->flags || s->end<=s->start) continue;
        if (s->type==CIUKI_RESOURCE_IO) {
            if (s->start<=0x3f2 && s->end>=0x3f6) main=true;
            if (s->start<=0x3f7 && s->end>=0x3f8) dir=true;
        }
        if (s->type==CIUKI_RESOURCE_IRQ && s->start==6 && s->end==7) irq=true;
        if (s->type==CIUKI_RESOURCE_DMA && s->start==2 && s->end==3) dma=true;
    }
    e->pnp=true; e->routed=main && dir && irq && dma && !r->flags && r->state==CIUKI_FIRMWARE_DISCOVERED;
}
static void attach_thread(void *arg)
{
    (void)arg;
    uint16_t equipment; memcpy(&equipment,P2V(0x410),2);
    uint8_t types=fs_floppy_cmos_types();
    struct fdc_evidence ev; int discovery=fdc_discover(&ev,equipment,types);
    struct ciuki_firmware_page page;
    bool found=false;
    /* Firmware inventory runs on its own worker. No AML is evaluated here. */
    for (unsigned wait=0;wait<3000;wait++) {
        memset(&page,0,sizeof(page)); page.version=CIUKI_FIRMWARE_VERSION; page.size=sizeof(page);
        int e=firmware_inventory_page(&page);
        if (e || !(page.flags&CIUKI_FIRMWARE_PENDING)) break;
        task_sleep_ms(1);
    }
    for (unsigned pages=0;pages<16;pages++) {
        for (unsigned i=0;i<page.count && i<4;i++) if (id(&page.records[i])) {
            if (found) ev.routed=false; /* ambiguous controllers are not probed */
            else pnp_resources(&ev,&page.records[i]);
            found=true;
        }
        if (!page.next) break;
        page.cursor=page.next;
        if (firmware_inventory_page(&page)) { ev.routed=false; break; }
    }
    if ((!found && discovery) || !ev.routed || (inb(0x4d0)&0x40) || (!types) || (types>>4)>5 || (types&15)>5) {
        controller.evidence=ev;
        controller.reason=found ? "fdc_firmware_resources_unqualified" : "fdc_not_present"; return;
    }
    controller.evidence=ev;
    uint32_t physical=pmm_alloc_dma_run(1,1);
    if (!physical) { controller.reason="fdc_dma_memory_unavailable"; return; }
    mm_claim(physical,1,MM_DMA);
    buffer_pin=(struct mm_pin){.generation=1,.first_pfn=physical/PAGE_SIZE,.pages=1,
        .address_mask=0xffffff,.alignment=PAGE_SIZE,.boundary=65536,.direction=MM_PIN_BIDIRECTIONAL};
    int e=mm_pin ? mm_pin(&buffer_pin) : -ENOMEM;
    if (e) controller.reason="fdc_pin_failed";
    if (!e) {
        const struct fdc_io io={real_read,real_write,real_now,real_pause,real_exclude,real_restore,
            real_irq,real_cancelled,0};
        e=fdc_attach(&controller,&io,&ev,P2V(physical),physical,&buffer_pin);
    }
    if (e && !controller.quarantined) {
        bool retired=!buffer_pin.ticket || (mm_unpin && !mm_unpin(&buffer_pin,buffer_pin.generation));
        if (retired) mm_free(physical,MM_DMA);
        else { controller.quarantined=true; controller.reason="fdc_pin_retirement_failed"; }
    }
    klog("[floppy] result=%d ready=%u pnp=%u drives=%02x reason=%s",e,controller.ready,ev.pnp,types,
         controller.reason ? controller.reason : "fdc_pin_failed");
    if (e) return;
    for (unsigned i=0;i<2;i++) if (controller.drive[i].present) (void)floppy_mount(i);
    /* A single task observes DIR for leased drives. Guest port polling only
     * reads cached state. Media loss expires native cache/handles before
     * releasing the native lease; a DOS lease stays assigned until VM reset
     * explicitly revalidates the replacement medium. */
    for (;;) {
        task_sleep_ms(1000);
        fdc_idle(&controller);
        for (unsigned i=0;i<2;i++) {
            struct fdc_drive *d=&controller.drive[i];
            if (!d->owner || controller.quarantined) continue;
            struct fdc_lease lease={&controller,d->owner,d->owner_generation,d->serial,i};
            int r=fdc_check(&lease);
            if (r==-FS_EAGAIN && lease.owner==FDC_NATIVE && native_mounted[i]) {
                if (!floppy_unmount(i,true)) {
                    d->block.quarantined=false;
                    (void)floppy_mount(i);
                }
            }
        }
    }
}
void floppy_init(void)
{
    if (initialized || (g_boot.flags&CBI_F_SAFE_MODE)) return;
    initialized=true;
    if (!task_create_kernel("floppy",attach_thread,0,P_DEVICE)) controller.reason="fdc_worker_unavailable";
}
struct fdc *floppy_controller(void) { return controller.ready ? &controller : 0; }
const struct fdc *floppy_inventory(void) { return &controller; }
int floppy_native_open(unsigned drive, struct blkdev **out)
{ return controller.ready ? fdc_native_open(&controller,drive,out) : -FS_ENOENT; }
int floppy_native_close(unsigned drive)
{ return drive<2 && native_mounted[drive] ? -FS_EBUSY : fdc_native_close(&controller,drive); }
int floppy_mount(unsigned drive)
{
    struct blkdev *b; int e=floppy_native_open(drive,&b);
    if (e) return e;
    e=storage_add_floppy(storage_get(),b,drive);
    if (e) (void)floppy_native_close(drive);
    else { native_mounted[drive]=true; (void)storage_activate_removable(storage_get(),b); }
    return e;
}
int floppy_unmount(unsigned drive, bool surprise)
{
    if (drive>=2 || !native_mounted[drive] || controller.drive[drive].owner!=FDC_NATIVE) return -FS_EINVAL;
    int e=storage_remove_removable(storage_get(),&controller.drive[drive].block,surprise);
    if (!e) e=fdc_native_close(&controller,drive);
    if (!e) native_mounted[drive]=false;
    return e;
}
