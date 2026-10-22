/* F3-64 production VM provider, cache and resident accounting. Namespace
 * persistence/physical crash cuts run against real FAT/VFS separately.
 * SPDX-License-Identifier: GPL-2.0-only */
static struct { struct dosvm *vm; uint8_t bytes[16384]; uint32_t size; } vm_save_disk;
static int vm_save_read(void *arg,uint32_t at,void *out,unsigned n)
{
    (void)arg; CHECK(vm_save_disk.vm->disk_busy && at+n<=vm_save_disk.size);
    memcpy(out,vm_save_disk.bytes+at,n); return 0;
}
static int vm_save_write(void *arg,uint32_t at,const void *in,unsigned n)
{
    (void)arg; CHECK(vm_save_disk.vm->disk_busy && at+n<=sizeof(vm_save_disk.bytes));
    memcpy(vm_save_disk.bytes+at,in,n); if (vm_save_disk.size<at+n) vm_save_disk.size=at+n; return 0;
}
static int vm_save_flush(void *arg) { (void)arg; CHECK(vm_save_disk.vm->disk_busy); return 0; }
static int vm_save_resize(void *arg,uint32_t n) { (void)arg; CHECK(vm_save_disk.vm->disk_busy && n<=vm_save_disk.size); vm_save_disk.size=n; return 0; }
static void vm_save_attach(struct dosvm *v)
{
    vm_save_disk.vm=v; v->persistent=true; v->game=1; v->save_file.writable=true;
    v->save=(struct dos_save){.alloc=save_index_alloc,.release=save_index_release,.memory=v,.quota=8192};
    struct dos_save_io io={vm_save_read,vm_save_write,vm_save_flush,vm_save_resize,0};
    struct dos_save_identity id={.game=1,.kernel=2}; memset(id.base,'a',64);
    disk_gate_take(v); CHECK(!dos_save_open(&v->save,&io,&id,vm_save_disk.size)); disk_gate_drop(v);
    v->sector_count=v->save.rows;
    for (unsigned i=0;i<v->read_capacity;i++) v->read_cache[i].count=0;
}
static void test_persistent_provider(void)
{
    empty(); memset(&vm_save_disk,0,sizeof(vm_save_disk));
    uint8_t first[512],second[512],got[512]; memset(first,0x64,512); memset(second,0x65,512);
    struct dosvm *v=create(); unsigned resident=v->info.resident;
    vm_save_attach(v); CHECK(!dosvm_disk_read(v,77,got) && memcmp(first,got,512));
    CHECK(!dosvm_disk_write(v,77,first) && !dosvm_disk_read(v,77,got) && !memcmp(first,got,512));
    CHECK(v->info.resident==resident+PAGE_SIZE && v->sector_count==1);
    struct dosvm_ledger ledger; dosvm_snapshot(&ledger);
    CHECK(ledger.overlay_rows==1 && v->save.index_pages==1 && ledger.pages==vm_owned_pages(v));
    unsigned owners[MM_OWNERS],pages; owners_count(owners,&pages);
    CHECK(ledger.pages==pages && owners[MM_VM_PRIVATE]>=v->read_pages+v->save.index_pages);
    struct blkdev dev; dosvm_disk_provider_init(v,&dev); CHECK(!dev.flush(&dev));
    uint32_t committed=v->save.committed;
    CHECK(!dosvm_disk_write(v,77,second)); v->save_discarding=true;
    CHECK(!dev.flush(&dev) && v->save.committed==committed && v->save.pending==1);
    destroy(v,true); CHECK(v->info.data_loss); empty();
    v=create(); vm_save_attach(v);
    CHECK(v->save.recovered_bytes==1024 && !dosvm_disk_read(v,77,got) && !memcmp(first,got,512));
    CHECK(!dosvm_disk_write(v,77,second)); destroy(v,false); empty();
    v=create(); vm_save_attach(v); CHECK(v->save.sealed);
    CHECK(!dosvm_disk_read(v,77,got) && !memcmp(second,got,512));
    unsigned writes=0; int e;
    while (!(e=dosvm_disk_write(v,78,first))) writes++;
    CHECK(writes && e==-FS_ENOSPC && v->save.full && v->save.readonly);
    uint32_t end=v->save.end;
    CHECK(dosvm_disk_write(v,79,first)==-FS_EROFS && v->save.end==end);
    CHECK(!dosvm_disk_read(v,77,got) && !memcmp(second,got,512));
    destroy(v,false); CHECK(v->save.sealed && !v->info.data_loss); empty();
    puts("PASS persistent VM provider: cache merge/invalidation, committed restart, forced rollback, quota/read-only/seal, private index pages and zero teardown ledger");
}
