/* Production BOT/UHCI IRQ interleavings, including the T23 partial READ.
 * Reuse only the register/peripheral model, never copy driver algorithms. */
#define main storage_fixture_main
#include "storage_test.c"
#undef main
static unsigned races;
static void irq_loss_reads(const char *path)
{
    setup(path,false);use_uhci();m.h.irq_attached=true;attach();
    m.h.claims[1]=1;m.interrupt_mask=0x0d;m.native_gate=0x2000;
    m.h.io.config_read=irq_config_read;m.h.io.config_write=irq_config_write;
    irq_lease_lost=true;
    unsigned interrupts=m.h.interrupts;
    fs_lock_take(&m.h.transfer_lock);usb_hid_service(&m.h);fs_lock_drop(&m.h.transfer_lock);
    assert(!m.h.irq_attached && m.h.ready && !m.h.quarantined);
    uint8_t *buf=malloc(BLKDEV_READ_BYTES);assert(buf);
    assert(!block()->read(block(),0,128,buf) && !memcmp(buf,m.image,BLKDEV_READ_BYTES));
    assert(!block()->quarantined && !m.h.quarantined && m.h.interrupts==interrupts);
    assert(!m.interrupt_mask && !(m.native_gate&0x20bf));
    assert(strstr(records,"[usb] irq_lost controller=") && !strstr(records,"lease_invalid"));
    free(buf);cleanup();
    puts("PASS F0-39 USB IRQ loss: production BOT/UHCI 128-sector read survives in polling mode with identical data and no new interrupts");
}
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
/* Same root listing/stat workload through production VFS/FAT/cache/BOT.
 * Two LFN slots plus one short entry per file: 64 visible entries, 13 cold
 * directory sectors, a normal FAT16 fixed root (32 sectors in this fixture). */
static void metadata_commands(const char *path) {
    setup(path,false); use_uhci(); m.h.irq_attached=true; attach();
    unsigned reserved=fs_rd16(m.image+14), fats=m.image[16], fat_sectors=fs_rd16(m.image+22);
    unsigned root_sector=reserved+fats*fat_sectors, root_entries=fs_rd16(m.image+17);
    assert(root_entries>=64*3+1);
    uint8_t *root=m.image+(size_t)root_sector*512; memset(root,0,root_entries*32);
    static const unsigned offsets[13]={1,3,5,7,9,14,16,18,20,22,24,28,30};
    for (unsigned i=0;i<64;i++) {
        char alias[12],name[40]; snprintf(alias,sizeof(alias),"FILE%04uTXT",i);
        snprintf(name,sizeof(name),"Entry %02u for AutoPlay.txt",i);
        unsigned length=(unsigned)strlen(name); uint8_t sum=0;
        for (unsigned j=0;j<11;j++) sum=(uint8_t)(((sum&1)<<7)+(sum>>1)+(uint8_t)alias[j]);
        for (unsigned slot=0;slot<2;slot++) {
            uint8_t *b=root+(i*3+slot)*32; memset(b,0xff,32);
            unsigned seq=2-slot; b[0]=(uint8_t)(seq|(slot ? 0 : 0x40)); b[11]=15;b[12]=0;b[13]=sum;fs_wr16(b+26,0);
            for (unsigned j=0;j<13;j++) {
                unsigned index=(seq-1)*13+j;
                fs_wr16(b+offsets[j],index<length ? (uint8_t)name[index] : index==length ? 0 : 0xffff);
            }
        }
        uint8_t *b=root+(i*3+2)*32;memset(b,0,32);memcpy(b,alias,11);b[11]=FAT_ATTR_ARCHIVE;
    }
    struct block_cache cache; struct fat_volume vol;
    static struct vfs v; static struct vfs_table table;
    assert(!cache_init(&cache,131072));
    assert(!fat_prepare_removable(&vol,&cache,block(),0,block()->capacity));
    vol.preparing=false; vfs_init(&v); assert(!vfs_attach(&v,4,&vol)); assert(!vfs_table_init(&v,&table,4));
    m.read_commands=0; unsigned before=m.read_commands,entries=0;
    struct vfs_find find; struct fat_entry entry;
    int e=vfs_find_first(&table,"E:/*",0,&find,&entry);
    while (!e) { entries++; e=vfs_find_next(&table,&find,&entry); }
    assert(e==-FS_ENOENT && entries==64);
    for (unsigned i=0;i<64;i++) {
        char name[64];snprintf(name,sizeof(name),"E:/Entry %02u for AutoPlay.txt",i);
        assert(!vfs_stat(&table,name,&entry));
    }
    unsigned commands=m.read_commands-before;
    printf("F1-78 metadata: root entries=%u stats=64 READ(10) commands=%u\n",entries,commands);
    assert(commands==1); /* baseline replay: 13 commands, at least 8x fewer */
    vfs_table_destroy(&table);vfs_destroy(&v);cache_destroy(&cache);cleanup();
}
int main(int argc,char **argv)
{
    assert(argc==2);
    irq_loss_reads(argv[1]);
    metadata_commands(argv[1]);
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
