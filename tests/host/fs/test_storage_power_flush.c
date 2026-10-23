/* Sparse register/sector model: no disk image or mkfs subprocess. Reuse the
 * existing FAT32 fixture and run the production mount/VFS/cache barrier.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main storage_layout_fixture_main
#include "test_mount_budget.c"
#undef main
#include <ciuki/storage.h>
static uint64_t power_now(void *ctx) { return *(uint64_t *)ctx; }
static void power_pause(void *ctx) { *(uint64_t *)ctx += 1000; }
static unsigned timeout_flushes;
static int timeout_flush(struct blkdev *dev)
{
    (void)dev;
    const struct vfs_request *request = fs_request_current();
    assert(request && request->deadline == 6000);
    *(uint64_t *)request->arg = request->deadline;
    timeout_flushes++;
    return fs_request_check();
}
static void test_flush(unsigned drive)
{
    timeout_flushes=0;
    static struct media m;
    static struct storage s;
    init(&m,1,8);
    assert(!storage_setup(&s,65536));
    struct fat_volume *v = &s.volumes[drive].fat;
    assert(!fat_mount(v,&s.cache,&m.dev,0,m.dev.capacity,false));
    assert(!fat_enable_write(v));
    assert(!vfs_attach(&s.vfs,drive,v));
    uint8_t sector[512];
    assert(!cache_read(&s.cache,&m.dev,m.data,sector));
    assert(!cache_write(&s.cache,&m.dev,m.data,sector));
    uint64_t us = 0;
    struct vfs_request request = {.arg=&us,.deadline=100000,.now=power_now,.pause=power_pause};
    unsigned writes = m.writes;
    assert(!storage_sync_until(&s,&request));
    assert(m.writes > writes && v->mounted && !s.stopped);
    assert(!fs_request_current());
    /* Busy namespace admission observes the deadline and issues no I/O. */
    writes = m.writes;
    fs_lock_take(&s.vfs.lock);
    us = 0; request.deadline = 3000;
    assert(storage_sync_until(&s,&request) == -VFS_ETIMEOUT && us == 3000);
    fs_lock_drop(&s.vfs.lock);
    assert(m.writes == writes && !fs_request_current() && !s.stopped);
    assert(v->mounted && !v->readonly);
    assert(storage_sync_until(&s,NULL) == -FS_EINVAL);
    /* The request also reaches the issued physical flush, rather than
     * merely checking a clock around an unbounded calldown. */
    us = 4000; request.deadline = 6000; m.dev.flush = timeout_flush;
    assert(storage_sync_until(&s,&request) == -VFS_ETIMEOUT);
    assert(timeout_flushes == 1 && !fs_request_current() && v->mounted && !s.stopped);
    struct cache_completion completion;
    assert(!cache_completion(&s.cache,&m.dev,&completion));
    assert(completion.incomplete); /* uncertain durability stays explicit */
    storage_destroy(&s);
    assert(!blk_request_pool_host_cleanup());
    assert(!fs_host_contig_live);
    puts("PASS bounded suspend flush: production durable barrier, retained mounts, deadline admission and transport propagation, no post-expiry I/O, zero live workspaces");
}

int main(void)
{
    test_flush(0); test_flush(1); test_flush(3);
    return 0;
}
