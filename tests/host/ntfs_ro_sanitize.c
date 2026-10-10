/* Production parser under ASan/UBSan, read-only bounded block adapter.
 * SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <ciuki/ntfs_ro.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct disk { FILE *f; uint64_t sectors; unsigned calls, sector_size; };
static int read_blocks(void *opaque, uint64_t lba, uint32_t count, void *buf)
{
    struct disk *d=opaque;
    assert(count && lba<=d->sectors && count<=d->sectors-lba);
    d->calls++;
    if (fseeko(d->f,(off_t)(lba*d->sector_size),SEEK_SET)) return -1;
    return fread(buf,d->sector_size,count,d->f)==count ? 0 : -1;
}
struct walk { struct ntfs_ro *v; uint64_t directories[64]; unsigned count; };
static int emit(void *opaque, const struct ntfs_ro_entry *e)
{
    struct walk *w=opaque;
    if (e->name[0]=='$' || !strcmp(e->name,".") || !strcmp(e->name,"..")) return 0;
    struct ntfs_ro_node n;
    int err=ntfs_ro_open(w->v,e->reference,&n);
    if (err==-NTFS_RO_EOPNOTSUPP || err==-NTFS_RO_EIO) return 0;
    assert(!err);
    if (n.directory) {
        assert(w->count<64); w->directories[w->count++]=n.reference;
        return 0;
    }
    unsigned char bytes[1024]; size_t got;
    assert(!ntfs_ro_pread(w->v,&n,0,bytes,sizeof(bytes),&got));
    assert(got<=sizeof(bytes));
    if (n.size) assert(!ntfs_ro_pread(w->v,&n,n.size-1,bytes,sizeof(bytes),&got) && got==1);
    assert(!ntfs_ro_pread(w->v,&n,UINT64_MAX,bytes,sizeof(bytes),&got) && got==0);
    return 0;
}
/* emit must not reenter a context. Store a bounded listing first. */
struct entries { struct ntfs_ro_entry *items; unsigned count; };
static int collect(void *opaque, const struct ntfs_ro_entry *e)
{
    struct entries *a=opaque; assert(a->count<12000);
    a->items[a->count++]=*e; return 0;
}
int main(int argc,char **argv)
{
    assert(argc==2);
    struct stat st; assert(!stat(argv[1],&st));
    struct disk disk={.f=fopen(argv[1],"rb")}; assert(disk.f);
    unsigned char boot[512]; assert(fread(boot,1,512,disk.f)==512);
    disk.sector_size=boot[11] | (unsigned)boot[12]<<8;
    for (unsigned i=0;i<8;i++) disk.sectors |= (uint64_t)boot[40+i]<<(8*i);
    assert(disk.sector_size>=512 && disk.sectors<=(uint64_t)st.st_size/disk.sector_size);
    size_t size=ntfs_ro_workspace_size();
    unsigned char *mem=malloc(size+64); assert(mem); memset(mem,0xa5,size+64);
    struct ntfs_ro *v=0; enum ntfs_ro_reason reason;
    int err=ntfs_ro_mount(mem,size,read_blocks,&disk,disk.sector_size,disk.sectors,&v,&reason);
    if (!err) {
        struct ntfs_ro_node root; assert(!ntfs_ro_root(v,&root));
        struct walk w={.v=v,.directories={root.reference},.count=1};
        struct entries a={calloc(12000,sizeof(struct ntfs_ro_entry)),0}; assert(a.items);
        for (unsigned i=0;i<w.count;i++) {
            struct ntfs_ro_node n; assert(!ntfs_ro_open(v,w.directories[i],&n));
            a.count=0; int list_error=ntfs_ro_list(v,&n,collect,&a);
            if (list_error==-NTFS_RO_EIO) continue;
            assert(!list_error);
            for (unsigned j=0;j<a.count;j++) assert(!emit(&w,&a.items[j]));
        }
        free(a.items); ntfs_ro_unmount(v);
        assert(ntfs_ro_root(v,&root)==-NTFS_RO_EINVAL);
    } else assert(!v);
    for (size_t i=size;i<size+64;i++) assert(mem[i]==0xa5);
    free(mem); assert(!fclose(disk.f));
    printf("ntfs sanitize: error=%d reason=%u reads=%u\n",err,(unsigned)reason,disk.calls);
    return 0;
}
