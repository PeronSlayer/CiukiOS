/* C3 source: real VFS/FAT/cache/BOT/UHCI over the existing peripheral model.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main f183_usb_fixture_main
#include "storage_test.c"
#undef main
static struct vfs_table copy_source;
static int copy_handle;
void f183_usb_source_open(void) {
    setup("build/host/fs/fat16.img",false); use_uhci(); mount_device();
    struct storage *s=storage_get(); assert(!storage_enable_write(s,4));
    assert(!vfs_table_init(&s->vfs,&copy_source,4));
    copy_handle=vfs_open(&copy_source,"E:/SOURCE.BIN",VFS_CREATE|VFS_TRUNCATE|VFS_READ|VFS_WRITE,VFS_DENY_NONE,0);
    assert(copy_handle>=0);
    uint8_t *buffer=malloc(65536); assert(buffer); memset(buffer,0x58,65536);
    for (unsigned i=0;i<160;i++) {
        size_t done; assert(!vfs_write(&copy_source,copy_handle,buffer,65536,&done) && done==65536);
    }
    assert(!vfs_commit(&copy_source,copy_handle)); free(buffer);
    cache_invalidate(&s->cache,&s->volumes[4].io,false);
}
int f183_usb_source_read(uint64_t offset,uint32_t bytes,void *buffer) {
    uint64_t position; size_t done;
    assert(bytes<=65536);
    int e=vfs_seek(&copy_source,copy_handle,(int64_t)offset,VFS_SEEK_SET,&position);
    if (!e) e=vfs_read(&copy_source,copy_handle,buffer,bytes,&done);
    return e ? e : done==bytes ? 0 : -FS_EIO;
}
void f183_usb_source_close(void) {
    assert(!vfs_close(&copy_source,copy_handle)); vfs_table_destroy(&copy_source);
    printf("C3 USB source engine=production-UHCI-BOT FAT16 read_commands=%u command_cap_sectors=128 reset=%u\n",
        m.read_commands,m.resets);
    storage_destroy(storage_get()); fs_lock_destroy(&m.h.transfer_lock);
    assert(!munmap(m.image,m.bytes));
}
