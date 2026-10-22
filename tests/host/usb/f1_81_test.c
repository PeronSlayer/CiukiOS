/* Production VFS/cache/BOT/UHCI latency, with a 1 ms HC frame clock.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main storage_fixture_main
#include "storage_test.c"
#undef main

static void transport_latency(const char *path, unsigned mode, bool baseline)
{
    bool irq=mode==1;
    setup(path,false);use_uhci();m.h.irq_attached=mode!=0;
    m.interrupt_mask=irq ? 0x0d : 0;m.native_gate=irq ? 0x2000 : 0;
    uint64_t begin=m.now;
    mount_device();
    uint64_t mount_ms=m.now-begin;
    unsigned mount_commands=m.read_commands;
    uint8_t *buf=malloc(BLKDEV_READ_BYTES);assert(buf);
    unsigned before=m.read_commands;
    uint64_t interrupts=m.h.interrupts;
    begin=m.now;
    assert(!block()->read(block(),0,1,buf) && !memcmp(buf,m.image,512));
    uint64_t block_ms=m.now-begin, single_ms=m.now-m.command_at+1;
    assert(m.read_commands==before+1);
    if (irq && !baseline) assert(m.h.interrupts-interrupts==5); /* TUR + three READ phases */
    begin=m.now;
    assert(!block()->read(block(),0,128,buf) && !memcmp(buf,m.image,BLKDEV_READ_BYTES));
    uint64_t sequential_ms=m.now-begin;
    assert(m.read_commands==before+2);
    assert(block()->read(block(),0,129,buf)==-FS_EINVAL && m.read_commands==before+2);
    assert(!m.resets && !m.clear_in && !m.clear_out && !m.writes);
    assert(irq ? m.h.interrupts>0 : m.h.interrupts==0);
    printf("F1-81 %s: mount READs=%u ms=%llu; single READs=1 ms=%llu block_ms=%llu; 128-sector READs=1 ms=%llu KB/s=%llu\n",
           mode==2 ? "lost IRQ" : irq ? "IRQ" : "poll",mount_commands,(unsigned long long)mount_ms,
           (unsigned long long)single_ms,(unsigned long long)block_ms,(unsigned long long)sequential_ms,
           (unsigned long long)(BLKDEV_READ_BYTES*1000ull/sequential_ms/1024));
    if (!baseline) {
        assert(single_ms<=3);assert(mount_ms<=60);
        assert(BLKDEV_READ_BYTES*1000ull/sequential_ms>=700u*1024);
    }
    free(buf);cleanup();
}

static unsigned deferred, writeback_waits;
static volatile uint32_t *pending_qh;
static bool delay_writeback(void *cookie,uint32_t address,volatile uint32_t *qh,uint32_t next)
{
    (void)cookie;
    if (!deferred && (next&1) && m.cbw[15]==0x28 && m.stage==2) {
        assert(address>=m.h.dma_phys+USB_BULK_TD);
        volatile uint32_t *td=uhci_dma(NULL,address,16);
        assert(!(td[1]&(1u<<23)) && (td[1]&(1u<<24)));
        pending_qh=qh;deferred++;return false;
    }
    qh[1]=next;return true;
}
static void finish_writeback(void)
{
    if (!pending_qh) return;
    assert(!(pending_qh[1]&1) && m.stage==2); /* no CSW or TD reuse yet */
    if (++writeback_waits==2) { pending_qh[1]=1;pending_qh=NULL; }
}
static void completion_proof(const char *path)
{
    setup(path,false);use_uhci();m.h.irq_attached=true;
    m.interrupt_mask=0x0d;m.native_gate=0x2000;attach();
    qh_advance_hook=delay_writeback;sleep_hook=finish_writeback;
    uint8_t data[512];
    assert(!block()->read(block(),0,1,data) && !memcmp(data,m.image,sizeof(data)));
    assert(deferred==1 && writeback_waits==2 && !pending_qh && !m.resets);
    qh_advance_hook=NULL;sleep_hook=NULL;cleanup();
    puts("PASS F1-81: final TD status/IOC before QH writeback cannot release data or submit CSW");
}

static void directory_latency(const char *path, bool irq, bool baseline)
{
    setup(path,false);use_uhci();m.h.irq_attached=irq;
    m.interrupt_mask=irq ? 0x0d : 0;m.native_gate=irq ? 0x2000 : 0;
    attach();
    unsigned root_sector=fs_rd16(m.image+14)+m.image[16]*fs_rd16(m.image+22);
    unsigned root_entries=fs_rd16(m.image+17);
    assert(root_entries>=301);
    uint8_t *root=m.image+(size_t)root_sector*512;memset(root,0,root_entries*32);
    for (unsigned i=0;i<300;i++) {
        char alias[12];snprintf(alias,sizeof(alias),"FILE%04uTXT",i);
        memcpy(root+i*32,alias,11);root[i*32+11]=FAT_ATTR_ARCHIVE;
    }
    struct block_cache cache;struct fat_volume vol;
    static struct vfs v;static struct vfs_table table;
    assert(!cache_init(&cache,131072));
    assert(!fat_prepare_removable(&vol,&cache,block(),0,block()->capacity));
    vol.preparing=false;vfs_init(&v);assert(!vfs_attach(&v,4,&vol));
    assert(!vfs_table_init(&v,&table,4));
    unsigned before=m.read_commands,entries=0;
    uint64_t begin=m.now;
    struct vfs_find find;struct fat_entry entry;
    int e=vfs_find_first(&table,"E:/*",0,&find,&entry);
    while (!e) { entries++;e=vfs_find_next(&table,&find,&entry); }
    assert(e==-FS_ENOENT && entries==300);
    uint64_t listing_ms=m.now-begin;
    unsigned commands=m.read_commands-before;
    for (unsigned i=0;i<300;i++) {
        char name[32];snprintf(name,sizeof(name),"E:/FILE%04u.TXT",i);
        assert(!vfs_stat(&table,name,&entry));
    }
    assert(m.read_commands-before==commands && commands==1);
    printf("F1-81 %s: cold directory entries=%u READs=%u ms=%llu; cached stats=300 READs=0 ms=%llu\n",
           irq ? "IRQ" : "poll",entries,commands,(unsigned long long)listing_ms,
           (unsigned long long)(m.now-begin-listing_ms));
    if (!baseline) assert(listing_ms<=40);
    vfs_table_destroy(&table);vfs_destroy(&v);cache_destroy(&cache);cleanup();
}

int main(int argc,char **argv)
{
    assert(argc==2 || (argc==3 && !strcmp(argv[2],"baseline")));
    for (unsigned irq=0;irq<2;irq++) {
        transport_latency(argv[1],irq,argc==3);
        directory_latency(argv[1],irq,argc==3);
    }
    transport_latency(argv[1],2,argc==3);
    if (argc==2) completion_proof(argv[1]);
    return 0;
}
