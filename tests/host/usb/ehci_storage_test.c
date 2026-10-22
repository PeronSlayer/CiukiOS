/* EHCI schedule walker -> production BOT/block/cache/FAT/VFS.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main usb_storage_regression_main
#define USB_STORAGE_MODEL_PACKET_MAX 512u
#include "storage_test.c"
#undef main
#include "ehci_model.h"
static struct ehci_model e;
static int disk_packet(struct ehci_model *model, unsigned address, unsigned ep,
                       unsigned pid, unsigned toggle, uint8_t *buf, unsigned n)
{
    assert(address == device()->address); m.now = model->now;
    return packet(device(), ep, pid, toggle, buf, n, false);
}
static void use_ehci(void)
{
    ehci_model_init(&e, &m.h); e.transaction=disk_packet;
    m.h.dma=m.dma; m.h.dma_phys=0x100000; m.h.ehci_caplen=0x20;
    m.h.pci.prog_if=0x20; m.h.owned=true; m.h.ready=false;
    m.h.ops=&usb_ehci_ops; m.h.io=ehci_io(&e); m.h.ports=2;
    e.config[0x40/4]=1|(1u<<24); /* ownership itself is tested in ehci_test */
    assert(!m.h.ops->start(&m.h)); m.h.ready=true; e.regs[0x64/4]=5;
    device()->high_speed=true;
    device()->storage.in_packet=device()->storage.out_packet=512;
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    assert(argc==2);
    for (unsigned short_in=0;short_in<2;short_in++) {
        setup(argv[1],false); use_ehci();
        if(short_in) m.inquiry_bytes=5;
        attach(); assert(device()->storage.online && m.inquiries);
        assert(block()->capacity==m.bytes/512 && device()->storage.in_packet==512);
        usb_storage_service(&m.h); assert(storage_get()->vfs.volumes[4]);
        uint8_t data[512], before[512];
        assert(!block()->read(block(),1,1,before));
        memset(data,0xa7,sizeof(data)); assert(!block()->write(block(),1,1,data));
        memset(data,0,sizeof(data)); assert(!block()->read(block(),1,1,data));
        for(unsigned i=0;i<sizeof(data);i++) assert(data[i]==0xa7);
        assert(!block()->write(block(),1,1,before)); assert(!block()->flush(block()));
        assert(e.packets && e.completions && e.doorbells && !m.h.quarantined);
        usb_storage_detach(device());
        assert(usb_ehci_stop(&m.h)); cleanup();
    }
    puts("PASS EHCI real BOT bind, short INQUIRY, 512-byte bulk, FAT mount, block read/write/flush and DMA stop (ASan+UBSan)");
    return 0;
}
