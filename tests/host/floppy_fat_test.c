/* f1-115/f1-115b integration: actual FAT/VFS over FDC registers + DMA2.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "floppy_fixture.h"
#include <ciuki/storage.h>
void klog(const char *fmt, ...) { (void)fmt; }
int main(int argc,char **argv)
{
    assert(argc==2);
    struct floppy_fixture *m=calloc(1,sizeof(*m));assert(m);assert(!fixture_open(m,4));
    FILE *image=fopen(argv[1],"r+b");assert(image);assert(fread(m->disk,1,m->bytes,image)==m->bytes);
    struct blkdev *b;assert(!fdc_native_open(&m->fdc,0,&b));
    struct storage *s=calloc(1,sizeof(*s));assert(s);assert(!storage_setup(s,65536));
    int e=storage_add_floppy(s,b,0);
    if(e){fprintf(stderr,"FAT12 mount returned %d (requires f1-115b FAT source)\n",e);return 1;}
    assert(s->volumes[0].fat.type==12 && s->volumes[0].present && !s->volumes[2].present);
    assert(!storage_enable_write(s,0));
    struct vfs_table *t=calloc(1,sizeof(*t));assert(t);assert(!vfs_table_init(&s->vfs,t,0));
    int h=vfs_open(t,"A:/ROUNDTRIP.TXT",VFS_READ|VFS_WRITE|VFS_CREATE,VFS_DENY_NONE,0);assert(h>=0);
    uint8_t data[1700],got[1700];for(unsigned i=0;i<sizeof(data);i++)data[i]=(uint8_t)(i*19);
    size_t done;assert(!vfs_write(t,h,data,sizeof(data),&done) && done==sizeof(data));
    assert(!vfs_commit(t,h));uint64_t at;assert(!vfs_seek(t,h,0,VFS_SEEK_SET,&at));
    assert(!vfs_read(t,h,got,sizeof(got),&done) && done==sizeof(data) && !memcmp(got,data,sizeof(data)));
    assert(!vfs_close(t,h));assert(!vfs_rename(t,"A:/ROUNDTRIP.TXT","A:/RENAMED.TXT"));
    assert(!vfs_delete(t,"A:/RENAMED.TXT"));
    h=vfs_open(t,"A:/KEEP.TXT",VFS_READ|VFS_WRITE|VFS_CREATE,VFS_DENY_NONE,0);assert(h>=0);
    assert(!vfs_write(t,h,data,sizeof(data),&done));assert(!vfs_commit(t,h));assert(!vfs_close(t,h));
    vfs_table_destroy(t);free(t);
    assert(!storage_remove_removable(s,b,false));assert(!fdc_native_close(&m->fdc,0));
    assert(!fdc_native_open(&m->fdc,0,&b));assert(!storage_add_floppy(s,b,1));
    t=calloc(1,sizeof(*t));assert(t);assert(!vfs_table_init(&s->vfs,t,1));
    h=vfs_open(t,"B:/KEEP.TXT",VFS_READ,VFS_DENY_NONE,0);assert(h>=0);
    assert(!vfs_read(t,h,got,sizeof(got),&done) && done==sizeof(data) && !memcmp(got,data,sizeof(data)));
    m->changed=true;uint32_t generation=b->media_generation;assert(b->media_check(b)==-FS_EAGAIN && b->media_generation==generation+1);
    assert(!storage_remove_removable(s,b,true));
    assert(vfs_read(t,h,got,1,&done)!=0);
    assert(!fdc_native_close(&m->fdc,0));
    b->quarantined=false;assert(!fdc_native_open(&m->fdc,0,&b));
    assert(!storage_add_floppy(s,b,1));
    /* Reusing B: and its retained volume record cannot revive an old open. */
    assert(vfs_read(t,h,got,1,&done)!=0);
    int fresh=vfs_open(t,"B:/KEEP.TXT",VFS_READ,VFS_DENY_NONE,0);assert(fresh>=0);
    assert(!vfs_read(t,fresh,got,sizeof(got),&done) && done==sizeof(data) && !memcmp(got,data,sizeof(data)));
    assert(!vfs_close(t,fresh));(void)vfs_close(t,h);
    vfs_table_destroy(t);free(t);assert(!storage_remove_removable(s,b,false));
    storage_destroy(s);free(s);assert(!fdc_native_close(&m->fdc,0));
    rewind(image);assert(fwrite(m->disk,1,m->bytes,image)==m->bytes);assert(!fclose(image));
    fixture_close(m);free(m);
    assert(!blk_request_pool_host_cleanup());
    puts("floppy FAT12: PASS (FDC/DMA round trip, A:/B:, rename/delete, remount, stale handle)");
    fflush(stdout);return 0;
}
