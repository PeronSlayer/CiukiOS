/* G29c editor on a sparse image only. SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "../../sdk/libdisk/partition_tool.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#undef CLOCKS_PER_SEC
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include <ciuki/abi.h>
#include <fcntl.h>
#pragma clang diagnostic pop
static int image,held;
static unsigned writes,fail_write,flushes,checks;
static uint64_t capacity=262144;
#define CHECK(x) do { checks++; assert(x); } while(0)
const uint8_t disk_stub_mbr_code[440]={1};
const uint8_t disk_canonical_mbr_code[440]={2};
static int op(void *c,unsigned disk,bool write) { (void)c;(void)write; return disk==1 ? image : -ENXIO; }
static void cl(void *c,int h) { (void)c;(void)h; }
static int rd(void *c,int h,void *p,uint32_t n,uint64_t at) { (void)c; ssize_t r=pread(h,p,n,(off_t)at); return r<0 ? -errno : (int)r; }
static int wr(void *c,int h,const void *p,uint32_t n,uint64_t at)
{ (void)c; CHECK(held); if(++writes==fail_write)return -EIO; ssize_t r=pwrite(h,p,n,(off_t)at); return r<0 ? -errno : (int)r; }
static int ctl(void *c,int h,uint32_t op,void *record,uint32_t size)
{
    (void)c; (void)size;
    if(op==DISK_GET_INFO || op==DISK_PARTITION_ACQUIRE) {
        struct ciuki_disk_info *i=record;
        if(op==DISK_PARTITION_ACQUIRE) { CHECK(!held); held=1; }
        memset(i,0,sizeof(*i)); i->version=CIUKI_DISK_VERSION;i->size=sizeof(*i);
        i->flags=CIUKI_DISK_PRESENT|CIUKI_DISK_FLUSH_CACHE; i->sectors=capacity;i->sector_size=512;
        i->write_cache=CIUKI_DISK_CACHE_ENABLED; return 0;
    }
    if(op==DISK_FLUSH) { CHECK(held); flushes++; return fsync(h) ? -errno : 0; }
    if(op==DISK_LEASE_RELEASE) { CHECK(held); held=0; return 0; }
    return -ENOTTY;
}
static const struct disk_backend backend={.open=op,.close=cl,.pread=rd,.pwrite=wr,.devctl=ctl};
const struct disk_backend *disk_native_backend(void) { return &backend; }
static void reload(struct pt_table *t) { CHECK(!pt_read(1,t)); }
int main(int argc,char **argv)
{
    CHECK(argc==2); image=open(argv[1],O_RDWR|O_CREAT|O_TRUNC,0600); CHECK(image>=0); CHECK(!ftruncate(image,262144*512));
    struct pt_table *t=malloc(sizeof(*t)); CHECK(t); disk_set_backend(&backend);
    reload(t); CHECK(!pt_create(t,PT_PRIMARY,2048,32768)); CHECK(!writes);
    CHECK(!pt_commit(t) && !held && writes==1 && flushes==1);
    reload(t); CHECK(pt_create(t,PT_PRIMARY,30000,32768)<0);
    CHECK(!pt_create(t,PT_EXTENDED,65536,131072)); CHECK(!pt_commit(t));
    reload(t); CHECK(t->count==2); CHECK(!pt_create(t,PT_LOGICAL,67584,32768)); CHECK(!pt_commit(t));
    reload(t); CHECK(!pt_create(t,PT_LOGICAL,105000,32768)); CHECK(!pt_commit(t));
    reload(t); CHECK(t->count==3); CHECK(pt_delete(t,2)<0);
    CHECK(!pt_active(t,1)); CHECK(!pt_commit(t));
    reload(t); CHECK(t->sector[0][446]==0x80); CHECK(pt_active(t,2)<0);
    CHECK(!pt_delete(t,5)); CHECK(!pt_commit(t));
    reload(t); CHECK(!pt_delete(t,6)); CHECK(!pt_commit(t));
    reload(t); CHECK(!pt_delete(t,2)); CHECK(!pt_commit(t));
    reload(t); CHECK(t->count==1);
    t->large=false; CHECK(pt_create(t,PT_PRIMARY,40000,4194305)<0);
    CHECK(pt_create(t,PT_PRIMARY,DISK_LBA28-1,32768)<0);
    /* Table changes after preview are detected under the acquired lease. */
    CHECK(!pt_delete(t,1)); uint8_t b=0x33; CHECK(pwrite(image,&b,1,440)==1);
    unsigned before=writes; CHECK(pt_commit(t)==DISK_ECHANGED && writes==before && !held);
    reload(t); CHECK(!pt_delete(t,1)); fail_write=writes+1;
    CHECK(pt_commit(t)==DISK_EIO && !held); fail_write=0;
    capacity=16777216; CHECK(!ftruncate(image,(off_t)(capacity*512))); reload(t);
    CHECK(pt_create(t,PT_PRIMARY,1048576,8388608)<0); /* FAT16 >2 GiB */
    t->large=true; CHECK(!pt_create(t,PT_PRIMARY,1048576,8388608)); CHECK(t->sector[0][466]==0x0c);
    CHECK(!pt_commit(t)); reload(t); CHECK(t->sector[0][466]==0x0c);
    free(t); CHECK(!close(image)); CHECK(!unlink(argv[1]));
    printf("PASS FDISK: primary/extended/logical create/delete, active, bounds/overlap, FAT16 limit, stale-plan and write-error refusal (%u checks)\n",checks);
    return 0;
}
