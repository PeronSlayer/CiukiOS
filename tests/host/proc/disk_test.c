/* f9-02 Amendment R: production native syscall/descriptor/capability/lease
 * boundary after f9-01 integration. Reuse the process fixture, retaining
 * the real storage, disk node, VFS and FAT implementations.
 * SPDX-License-Identifier: GPL-2.0-only */
#define FD_TABLE_STACK_TEST
#define NATIVE_DISK_TEST
#include "files_test.c"

struct ciuki_boot_info g_boot;
void klog(const char *fmt, ...) { (void)fmt; }

static struct storage *storage;
static struct fake destination;
static uint8_t prefix[2048 * 512];
static bool watch;
static unsigned commands;
static void disk_transfer(void)
{
    if (!watch) return;
    /* Assert the logical hold too: VFS_IO releases physical mutexes while
     * a logical volume reservation can remain. Cold reads must have neither. */
    CHECK(!(storage->vfs.reserved | storage->vfs.mutating));
    commands++;
}
static int target_read(struct blkdev *d, uint64_t lba, uint32_t n, void *out)
{
    int e = blkdev_range(d, lba, n); if (e) return e;
    disk_transfer();
    if (lba < 2048) {
        uint32_t k = n < 2048 - lba ? n : (uint32_t)(2048 - lba);
        memcpy(out, prefix + lba * 512, (size_t)k * 512);
        out = (uint8_t *)out + (size_t)k * 512; lba += k; n -= k;
    }
    return n ? destination.dev.read(&destination.dev, lba - 2048, n, out) : 0;
}
static int target_write(struct blkdev *d, uint64_t lba, uint32_t n, const void *in)
{
    int e = blkdev_range(d, lba, n); if (e) return e;
    disk_transfer();
    if (lba < 2048) {
        uint32_t k = n < 2048 - lba ? n : (uint32_t)(2048 - lba);
        memcpy(prefix + lba * 512, in, (size_t)k * 512);
        in = (const uint8_t *)in + (size_t)k * 512; lba += k; n -= k;
    }
    return n ? destination.dev.write(&destination.dev, lba - 2048, n, in) : 0;
}
static int target_flush(struct blkdev *d)
{ (void)d; disk_transfer(); return destination.dev.flush(&destination.dev); }

static void activate(struct process *p, struct proc_thread *t)
{ client = p; client_thread = t; g_current = t->task; g_current->state = T_RUNNING; }
static int control_fd(int fd, uint32_t op, void *record, uint32_t size)
{
    CHECK(!ua_write(client->memory, USER, record, size));
    int e = call(CIUKI_SYS_DEVCTL, fd, op, USER, size, 0);
    if (!e) CHECK(!ua_read(client->memory, record, USER, size));
    return e;
}
static struct ciuki_disk_lease state(int fd)
{
    struct ciuki_disk_lease l = { .version=CIUKI_DISK_VERSION, .size=sizeof(l) };
    CHECK(!control_fd(fd, DISK_LEASE_STATE, &l, sizeof(l)));
    return l;
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    ram = calloc(HOST_PAGES, PAGE_SIZE); CHECK(ram);
    controller.state = T_RUNNING; g_current = &controller; proc_init();
    storage = storage_get(); CHECK(!storage_setup(storage, 0));
    CHECK(!fake_open(&disk[0], argv[1]));
    CHECK(!fat_mount(&volumes[0], &storage->cache, &disk[0].dev, 0, disk[0].dev.capacity, false));
    CHECK(!vfs_attach(&storage->vfs, 2, &volumes[0])); /* the read-only CD source */
    CHECK(!fake_open(&destination, argv[2])); destination.assert_io_unlocked = true;
    struct blkdev target = { .read=target_read, .write=target_write, .flush=target_flush,
        .capacity=destination.dev.capacity + 2048, .sector_size=512,
        .write_cache_state=BLKDEV_CACHE_ENABLED };
    prefix[446] = 0x80; prefix[450] = 0x0c;
    fs_wr32(prefix + 454, 2048); fs_wr32(prefix + 458, (uint32_t)destination.dev.capacity);
    fs_wr16(prefix + 510, 0xaa55);
    storage->raw[0].dev = &target;
    CHECK(!files_attach(&storage->vfs, &space));
    struct process *owner;
    struct proc_thread *owner_thread = make_process(proc_supervisor(), &owner);
    activate(owner, owner_thread);
    owner->cwd = space->root; px_retain(owner->cwd);
    owner->cwd_retain = px_retain; owner->cwd_release = px_release;
    /* All four slot names use the generic storage map; absent/ATAPI slots
     * have no raw device. No model, controller or display name is special. */
    for (unsigned slot = 1; slot < 4; slot++) {
        char path[] = "/dev/ata0"; path[8] += (char)slot;
        CHECK(file_open(owner, path, O_RDONLY, 0) == -ENXIO);
        storage->raw[slot].dev = &target;
        int f = file_open(owner, path, O_RDONLY, 0); CHECK(f >= 0);
        CHECK(!file_close(owner, f)); storage->raw[slot].dev = 0;
    }
    int fd = file_open(owner, "/dev/ata0", O_RDWR, 0); CHECK(fd >= 0);
    CHECK(file_open(owner, "/dev/ata0", O_WRONLY, 0) == -EACCES);
    CHECK(file_open(owner, "/dev/ata0", O_RDWR | O_APPEND, 0) == -EINVAL);
    CHECK(call(CIUKI_SYS_READ, fd, USER, 512, 0, 0) == -ESPIPE);
    CHECK(call(CIUKI_SYS_WRITE, fd, USER, 512, 0, 0) == -ESPIPE);
    CHECK(call(CIUKI_SYS_LSEEK64, fd, 0, 0, SEEK_SET, USER) == -ESPIPE);
    CHECK(call(CIUKI_SYS_PREAD, fd, USER, 511, 0, 0) == -EINVAL);
    CHECK(call(CIUKI_SYS_PREAD, fd, USER, 512, 1, 0) == -EINVAL);
    CHECK(call(CIUKI_SYS_PREAD, fd, USER, 512, 0, 0) == 512);
    CHECK(call(CIUKI_SYS_PWRITE, fd, USER, 512, 4096, 0) == -EPERM);
    CHECK(call(CIUKI_SYS_FSYNC, fd, 0, 0, 0, 0) == -EPERM);
    struct ciuki_disk_info info = { .version=CIUKI_DISK_VERSION, .size=sizeof(info) };
    CHECK(control_fd(fd, DISK_LEASE_ACQUIRE, &info, sizeof(info)) == -EPERM);
    owner->setup_capability = true;
    CHECK(call(CIUKI_SYS_PWRITE, fd, USER, 512, 4096, 0) == -EACCES);
    /* Readable RX records must fail before a lease is acquired. */
    CHECK(!ua_write(owner->memory, CIUKI_IMAGE_BASE + 128, &info, sizeof(info)));
    CHECK(call(CIUKI_SYS_DEVCTL, fd, DISK_LEASE_ACQUIRE, CIUKI_IMAGE_BASE + 128, sizeof(info), 0) == -EFAULT);
    CHECK(state(fd).state == CIUKI_DISK_LEASE_NONE);
    CHECK(!control_fd(fd, DISK_LEASE_ACQUIRE, &info, sizeof(info)));
    int duplicate = call(CIUKI_SYS_DUP, fd, 0, 0, 0, 0); CHECK(duplicate >= 0);
    struct ciuki_mmap_args mapping = { .size=sizeof(mapping), .length=2 * CIUKI_DISK_IO_MAX,
        .prot=PROT_READ | PROT_WRITE, .flags=MAP_PRIVATE | MAP_ANONYMOUS, .fd=-1 };
    uint32_t user = (uint32_t)ua_mmap(owner->memory, &mapping);
    CHECK(user >= CIUKI_MMAP_BASE && user < CIUKI_MMAP_LIMIT);
    watch = true; commands = 0;
    CHECK(call(CIUKI_SYS_PWRITE, duplicate, user, CIUKI_DISK_IO_MAX + 512, 4096, 0) == CIUKI_DISK_IO_MAX);
    CHECK(call(CIUKI_SYS_FSYNC, duplicate, 0, 0, 0, 0) == 0);
    CHECK(call(CIUKI_SYS_PREAD, duplicate, user, CIUKI_DISK_IO_MAX + 512, 4096, 0) == CIUKI_DISK_IO_MAX);
    watch = false; CHECK(commands == 3);
    struct ciuki_disk_lease pub = { .version=CIUKI_DISK_VERSION, .size=sizeof(pub) };
    CHECK(!control_fd(fd, DISK_PUBLISH, &pub, sizeof(pub)) && pub.drive == 3);
    put_path(USER, "/mnt/d/Lease.bin");
    int volume_fd = call(CIUKI_SYS_OPEN, USER, O_CREAT | O_RDWR, 0666, 0, 0); CHECK(volume_fd >= 0);
    uint8_t payload[512]; memset(payload, 0x92, sizeof(payload));
    CHECK(!ua_write(owner->memory, user, payload, sizeof(payload)));
    CHECK(call(CIUKI_SYS_PWRITE, volume_fd, user, sizeof(payload), 0, 0) == sizeof(payload));
    CHECK(!call(CIUKI_SYS_FSYNC, volume_fd, 0, 0, 0, 0));
    CHECK(call(CIUKI_SYS_PWRITE, fd, user, 512, 2048 * 512, 0) == -EBUSY);
    /* A copied writable description grants neither raw nor volume authority. */
    struct process *peer;
    struct proc_thread *peer_thread = make_process(owner, &peer);
    /* make_process has already installed stdio. Rebuild the inherited table
     * once, as the spawn path does, rather than retaining stdio twice. */
    for (int i = 0; i < CIUKI_OPEN_MAX; i++)
        if (peer->fds[i].object) CHECK(!file_close(peer, i));
    struct ciuki_spawn_fd inherited[] = { {fd, 8}, {volume_fd, 9} };
    CHECK(!proc_inherit(peer, owner, inherited, 2) && !peer->setup_capability);
    activate(peer, peer_thread);
    CHECK(call(CIUKI_SYS_PREAD, 8, USER, 512, 0, 0) == 512);
    CHECK(call(CIUKI_SYS_PWRITE, 8, USER, 512, 4096, 0) == -EPERM);
    CHECK(call(CIUKI_SYS_FSYNC, 8, 0, 0, 0, 0) == -EPERM);
    struct ciuki_disk_request request = { CIUKI_DISK_VERSION, sizeof(request), 0, 0 };
    CHECK(control_fd(8, DISK_FLUSH, &request, sizeof(request)) == -EPERM);
    CHECK(control_fd(8, DISK_LEASE_RELEASE, &request, sizeof(request)) == -EPERM);
    CHECK(control_fd(8, DISK_PUBLISH, &pub, sizeof(pub)) == -EPERM);
    CHECK(call(CIUKI_SYS_PWRITE, 9, USER, 512, 0, 0) == -EROFS);
    CHECK(call(CIUKI_SYS_FTRUNCATE, 9, 0, 0, 0, 0) == -EROFS);
    CHECK(file_open(peer, "/mnt/d/New.bin", O_CREAT | O_RDWR, 0666) == -EROFS);
    peer->setup_capability = true; /* capability alone is still not the lease */
    CHECK(call(CIUKI_SYS_PWRITE, 8, USER, 512, 4096, 0) == -EACCES);
    CHECK(call(CIUKI_SYS_PWRITE, 9, USER, 512, 0, 0) == -EROFS);
    peer->setup_capability = false;
    cache_invalidate(&storage->cache, &storage->volumes[3].io, false);
    watch = true; commands = 0;
    CHECK(call(CIUKI_SYS_PREAD, 9, USER, 512, 0, 0) == 512);
    watch = false; CHECK(commands);
    uint8_t copy[512]; CHECK(!ua_read(peer->memory, copy, USER, sizeof(copy)));
    CHECK(!memcmp(copy, payload, sizeof(copy)));
    /* The media/generation gate retained by the merge still rejects stale I/O. */
    storage->vfs.generation[3]++;
    commands = 0; watch = true;
    CHECK(call(CIUKI_SYS_PREAD, 9, USER, 512, 0, 0) == -EIO);
    watch = false; CHECK(!commands); storage->vfs.generation[3]--;
    activate(owner, owner_thread);
    owner->setup_capability = false; /* current flag, even with the holder PID */
    CHECK(call(CIUKI_SYS_PWRITE, volume_fd, user, 512, 0, 0) == -EROFS);
    owner->setup_capability = true;
    stop_collect(owner); activate(peer, peer_thread);
    CHECK(state(8).state == CIUKI_DISK_LEASE_REVOKED);
    CHECK(call(CIUKI_SYS_PWRITE, 9, USER, 512, 0, 0) == -EROFS);
    CHECK(call(CIUKI_SYS_PREAD, 9, USER, 512, 0, 0) == 512);
    all_closed(); stop_collect(peer);
    CHECK(!file_description_count());
    storage_destroy(storage); fake_close(&destination); fake_close(&disk[0]);
    CHECK(!pages_used && heap_blocks == 1 && !fs_host_contig_live);
    free(ram);
    printf("PASS native disk boundary: generic slots, aligned bounded raw syscalls without holds, writable ioctl pins, "
           "dup/inheritance, capability/lease checks, published volume cold read without holds, stale media, death revocation; %u checks\n", checks);
    return 0;
}
