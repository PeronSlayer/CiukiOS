/* Production BOT/UHCI IRQ interleavings, including the T23 partial READ.
 * Reuse only the register/peripheral model, never copy driver algorithms. */
#define main storage_fixture_main
#include "storage_test.c"
#undef main
static unsigned races;
static void read_recovery(const char *path)
{
    for(unsigned kind=1;kind<=3;kind++) for(unsigned failures=1;failures<=2;failures++) {
        setup(path,false);use_uhci();m.h.irq_attached=true;attach();
        m.read_fault_kind=kind;m.read_fault_commands=failures;
        uint8_t *buf=malloc(BLKDEV_READ_BYTES);assert(buf);
        int e=block()->read(block(),0,128,buf);
        assert(m.read_commands==2 && m.resets>=1 && m.clear_in==m.resets && m.clear_out==m.resets);
        assert(strstr(records,"retry addr=1 cmd=28 attempt=2"));
        if(failures==1) assert(!e && !block()->quarantined && !memcmp(buf,m.image,BLKDEV_READ_BYTES));
        else assert(e==-FS_EIO && block()->quarantined);
        free(buf);cleanup();
    }
    setup(path,false);use_uhci();m.h.irq_attached=true;attach();
    m.read_fault_kind=m.read_fault_commands=1;m.reset_fail=true;
    uint8_t *buf=malloc(BLKDEV_READ_BYTES);assert(buf);
    assert(block()->read(block(),0,128,buf)==-FS_EIO && block()->quarantined);
    assert(m.read_commands==1 && m.resets==1 && !m.clear_in && !m.clear_out);
    free(buf);cleanup();
    setup(path,false);use_uhci();m.h.irq_attached=true;attach();m.read_csw_stalls=2;
    uint8_t stalled_buf[1024];
    assert(!block()->read(block(),0,2,stalled_buf) && m.read_commands==2 && m.resets==1);
    assert(m.clear_in==2 && m.clear_out==1 && !block()->quarantined);
    cleanup();
    setup(path,false);use_uhci();m.h.irq_attached=true;attach();m.data_stall=m.clear_fail=true;
    assert(!block()->read(block(),0,2,stalled_buf) && m.read_commands==2 && m.resets==1);
    assert(m.clear_in==1 && m.clear_out==1 && !block()->quarantined);
    cleanup();
    for(unsigned short_bytes=64;short_bytes<=96;short_bytes+=32) {
        setup(path,false);use_uhci();m.h.irq_attached=true;attach();m.read_limit=short_bytes;
        uint8_t buf[1024];
        assert(block()->read(block(),0,2,buf)==-FS_EIO && block()->quarantined);
        assert(m.read_commands==2 && device()->storage.trace.csw_available);
        assert(device()->storage.trace.data_actual==short_bytes && m.in_toggle==device()->storage.in_toggle);
        assert(strstr(records,"reason=short_command"));cleanup();
    }
    setup(path,false);use_uhci();m.h.irq_attached=true;attach();m.data_crc=true;
    uint8_t crc_buf[1024];assert(block()->read(block(),0,2,crc_buf)==-FS_EIO && block()->quarantined);
    assert(m.read_commands==1 && !strstr(records,"retry addr=1 cmd=28"));cleanup();
    puts("PASS F1-74 IRQ recovery: partial-data/missing-CSW/invalid-CSW/persistent-CSW-STALL retry once after reset/clear-IN/clear-OUT; repeat/reset failure quarantines; short/ZLP preserve CSW/toggles; CRC never replayed");
}
static void complete_between_observations(void)
{
    if(races || m.cbw[15]!=0x28 || m.read_commands!=101 || m.stage!=1 || m.at!=64512) return;
    volatile uint32_t *qh=uhci_dma(NULL,m.h.dma_phys+USB_UHCI_BULK_QH,8);
    if(qh[1]&1) return;
    uint32_t address=qh[1]&~15u;
    volatile uint32_t *td=uhci_dma(NULL,address,16);
    if(!(td[1]&(1u<<23))) return;
    assert(uhci_transaction(NULL,address,m.h.dma_phys+USB_UHCI_BULK_QH,td));
    qh[1]=td[0]; /* HC writeback after the prefix walk's active snapshot */
    m.hc_status|=1;assert(usb_host_irq(&m.h));races++;
}
int main(int argc,char **argv)
{
    assert(argc==2);
    setup(argv[1],false);use_uhci();m.h.irq_attached=true;irq_wakes=races=0;
    attach();port_read_hook=complete_between_observations;
    uint8_t *data=malloc(BLKDEV_READ_BYTES);assert(data);
    for(unsigned i=0;i<101;i++) {
        int e=block()->read(block(),0,128,data);
        if(e) fprintf(stderr,"F1-74 read=%u result=%d stage=%s bytes=%u csw=%u retired=%d races=%u\n",
            i+1,e,device()->storage.trace.stage,device()->storage.trace.data_actual,
            device()->storage.trace.csw_available,device()->storage.trace.packet.retired,races);
        assert(!e && !memcmp(data,m.image,BLKDEV_READ_BYTES));
    }
    port_read_hook=NULL;
    assert(races==1 && irq_wakes && m.h.interrupts && !m.resets && !block()->quarantined);
    assert(device()->storage.trace.csw_available && !device()->packet_trace.retired);
    free(data);cleanup();
    puts("PASS F1-74 IRQ: 101 x 128-sector READs, completion between prefix/stop observations, retired data before CSW, no false timeout/reset");
    read_recovery(argv[1]);
    return 0;
}
