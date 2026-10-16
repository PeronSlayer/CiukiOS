/* Boot-page orchestration and the real file-backed crash fixture. */
#include <stdio.h>
#include <assert.h>
#include <ciuki/kernel.h>
#include <ciuki/storage.h>
#include <ciuki/hardware_check.h>
int hardware_check_ata(struct storage *,struct hw_check_result *);
int hardware_check_repair(struct storage *,struct hw_check_result *);
static uint64_t clock_ms;
static bool cancelled;
bool hardware_check_io_cancelled(void) { return cancelled; }
static unsigned next_item, lines, variant;
static uint64_t now(void *ctx) { (void)ctx; return clock_ms; }
static void step(void *ctx,unsigned i,uint64_t deadline,struct hw_check_result *r)
{
    (void)ctx; assert(i==next_item++); assert(deadline==clock_ms+HW_CHECK_LIMIT_MS);
    r->status=variant==1 ? HW_SKIP : HW_PASS;
    ksnprintf(r->detail,sizeof(r->detail),"fixture=%u",i);
    clock_ms+=variant==2 && i==4 ? 15001 : 50;
}
static void line(void *ctx,const char *s)
{ (void)ctx; assert(s && *s); lines++; }
static uint8_t disk_data[8192*512];
static unsigned writes, reads;
static int disk_read(struct blkdev *b,uint64_t lba,uint32_t n,void *buf)
{ assert(!blkdev_range(b,lba,n)); memcpy(buf,disk_data+lba*512,n*512); reads++; return 0; }
static int disk_write(struct blkdev *b,uint64_t lba,uint32_t n,const void *buf)
{ assert(!blkdev_range(b,lba,n)); memcpy(disk_data+lba*512,buf,n*512); writes++; return 0; }
static int disk_flush(struct blkdev *b) { (void)b; return 0; }
static struct storage storage;
static struct fat_volume vol;
static struct vfs_table table;
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);
    for (variant=0;variant<3;variant++) {
        next_item=lines=0;
        struct hw_check_result results[HW_CHECK_ITEMS];
        struct hw_check_ops ops={0,now,step,line}; hardware_check_model(&ops,results);
        assert(next_item==HW_CHECK_ITEMS && lines==3*HW_CHECK_ITEMS+2);
        for (unsigned i=0;i<HW_CHECK_ITEMS;i++) {
            assert(results[i].status==(variant==1 ? HW_SKIP : variant==2 && i==4 ? HW_FAIL : HW_PASS));
            char expected[32];ksnprintf(expected,sizeof(expected),"fixture=%u",i);
            assert(!strcmp(results[i].detail,expected)); /* retain details on deadline */
        }
    }
    puts("PASS hardware page: ordered items, absent devices, deadline failure, retained summaries");
    uint8_t *b=disk_data;
    b[0]=0xeb; b[1]=0x3c; b[2]=0x90;
    fs_wr16(b+11,512); b[13]=1; fs_wr16(b+14,1); b[16]=2;
    fs_wr16(b+17,16); fs_wr16(b+19,8192); b[21]=0xf8; fs_wr16(b+22,32); fs_wr16(b+510,0xaa55);
    for (unsigned c=0;c<2;c++) { fs_wr16(b+(1+c*32)*512,0xfff8); fs_wr16(b+(1+c*32)*512+2,0xffff); }
    struct blkdev disk={.read=disk_read,.write=disk_write,.flush=disk_flush,.capacity=8192,
        .sector_size=512,.write_cache_state=BLKDEV_CACHE_ENABLED};
    assert(!cache_init(&storage.cache,256*1024));
    assert(!fat_mount(&vol,&storage.cache,&disk,0,disk.capacity,true));
    vfs_init(&storage.vfs); assert(!vfs_attach(&storage.vfs,2,&vol)); storage.ready=true;
    assert(!vfs_table_init(&storage.vfs,&table,2));
    assert(!vfs_mkdir(&table,"C:/SYSTEM",0)); assert(!vfs_mkdir(&table,"C:/SYSTEM/LOGS",0));
    struct hw_check_result result;
    unsigned initial_writes=writes;
    cancelled=true;
    assert(hardware_check_ata(&storage,&result)==-FS_EAGAIN && result.status==HW_FAIL);
    assert(writes==initial_writes); cancelled=false;
    puts("PASS diagnostic deadline: cancelled task issues no scratch or disk write");
    assert(!hardware_check_ata(&storage,&result)); assert(result.status==HW_PASS);
    printf("PASS ATA scratch: %s (device reads=%u writes=%u)\n",result.detail,reads,writes);
    int e=hardware_check_repair(&storage,&result);
    printf("%s mount repair: %s\n",e ? "FAIL" : "PASS",result.detail);
    assert(!e && result.status==HW_PASS);
    struct fat_entry entry;
    assert(vfs_stat(&table,"C:/SYSTEM/LOGS/HWCHK.TMP",&entry)==-FS_ENOENT);
    int fd=vfs_open(&table,"C:/SYSTEM/LOGS/HWCHK.TMP",VFS_WRITE|VFS_CREATE,VFS_DENY_NONE,0);
    assert(fd>=0); assert(!vfs_close(&table,fd));
    unsigned prior=writes;
    assert(hardware_check_ata(&storage,&result)==-FS_EEXIST && result.status==HW_FAIL);
    assert(writes==prior);
    assert(!vfs_delete(&table,"C:/SYSTEM/LOGS/HWCHK.TMP"));
    puts("PASS scratch ownership: exclusive create, no overwrite, cleanup, live volume still writable");
    vfs_table_destroy(&table); assert(!vfs_detach(&storage.vfs,2)); vfs_destroy(&storage.vfs);
    cache_destroy(&storage.cache); assert(!fs_host_contig_live); return 0;
}
