/* File-backed bounded private disk overlay; the base is never opened writable.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/abi.h>
#include <ciuki/sha256.h>
#include "dosvm_internal.h"
/* fx-4b background verifier (dosvm_image.c); weak for the harnesses that
 * link this file without it, which keep the synchronous hash. */
#pragma weak dosvm_image_lookup
#pragma weak dosvm_image_publish

/* f3-06: base names a read-only image of the same private geometry (a
 * local workload disk built by scripts/test/dos_fixtures.py, or the bundled
 * shareware games disk); the canonical C: payload otherwise. Only the
 * overlay below is ever opened writable. */
#define DOSVM_DEFAULT_BASE "C:\\SYSTEM\\DOS\\UNIFIED.IMG"
/* The unified image includes the pinned shareware games and interactive
 * shell. Every desktop window uses it; external test bases are probe-only. */
#define DOSVM_BUNDLED_BASE DOSVM_DEFAULT_BASE
/* A full image, or one cut after its last used sector (zeros follow). */
static bool base_size_valid(uint32_t size)
{
    return !(size % 512u) && size >= DOSVM_BASE_MIN_SECTORS * 512u && size <= DOSVM_SECTORS * 512u;
}
/* The lookup table (20596 bytes on i686) is larger than the kernel heap's
 * biggest class (2040 bytes, core/kheap.c), so a kmalloc'd table failed with
 * ENOMEM on every call: /Programs/dosbox create was refused ("Not enough space",
 * vm-window QEMU 2026-10-12). One static table, never on a kernel stack,
 * behind a sleeping test-and-set gate (no mutex: the VFS calls below take
 * the storage lock). */
static struct vfs_table workload_table;
static bool workload_busy;
static int base_error(const char *path, int error, const char *reason)
{
    klog("[dosvm] disk base=%s error=%d reason=%s", path, error, reason);
    return error;
}
static int base_lookup(char *path, unsigned capacity, bool bundled)
{
    path[0] = 0;
    const char *fallback = bundled ? DOSVM_BUNDLED_BASE : "D:\\CIUKIGAM.IMG";
    struct storage *s = storage_get();
    if (!s || !s->ready || s->stopped) return base_error(fallback, -V86_EIO, "probe");
    for (;;) {
        uint32_t flags = irq_save();
        if (!workload_busy) { workload_busy = true; irq_restore(flags); break; }
        irq_restore(flags);
        task_sleep_ms(1);
    }
    struct vfs_table *t = &workload_table;
    const char *reason = "probe";
    int e = vfs_table_init(&s->vfs, t, 2);
    if (!e) {
        e = -ENOENT;
        struct fat_entry entry;
        /* Optional fixtures: every failed stat, including EIO/EBUSY from a
         * foreign or quarantined volume, is absent. Never carry its error
         * into the next letter or the required bundled base. */
        for (unsigned d = 3; !bundled && d < 26 && e == -ENOENT; d++) {
            ksnprintf(path, capacity, "%c:\\CIUKIGAM.IMG", 'A' + d);
            if (!vfs_stat(t, path, &entry) && entry.size == DOSVM_SECTORS * 512u && !(entry.attr & 0x10)) e = 0;
        }
        /* The bundled base is required for windows without a fixture. Keep
         * its real error, so a damaged boot volume cannot look like absence. */
        if (e == -ENOENT && bundled) {
            reason = "bundled";
            ksnprintf(path, capacity, "%s", DOSVM_BUNDLED_BASE);
            e = vfs_stat(t, path, &entry);
            if (!e && (!base_size_valid(entry.size) || (entry.attr & 0x10))) e = -EINVAL;
        }
        vfs_table_destroy(t);
    } else ksnprintf(path, capacity, "%s", fallback);
    workload_busy = false;
    if (e && (bundled || e != -ENOENT)) base_error(path, e, reason);
    if (e) path[0] = 0;
    return e;
}
int dosvm_workload_base(char path[24]) { return base_lookup(path, 24, false); }
int dosvm_window_base(char path[32]) { return base_lookup(path, 32, true); }
/* f3-46 (contract D1): a cache block is clean (the overlay is written
 * synchronously before the cache is updated), so the global replacement
 * discards it without I/O. One tracker per 64 KiB run; it can outlive the
 * VM while the reclaim queue holds it (close orphans it, release frees). */
struct vm_cache_block {
    struct reclaim_page reclaim; /* phys: the run's first page */
    struct dosvm *vm;
    unsigned slot;
    uint16_t holds;
    bool evict, busy;
};
static const struct reclaim_backing cache_provider;
static void run_free(uint32_t phys)
{
    for (unsigned p = 0; p < DOSVM_READ_PAGES; p++) mm_free(phys + p * PAGE_SIZE, MM_VM_PRIVATE);
}
/* A new 64 KiB run and its tracker, or 0. Never waits for reclaim. */
static uint32_t cache_block_new(struct dosvm *v, unsigned slot)
{
    struct vm_cache_block *t = kmalloc(sizeof(*t));
    if (!t) return 0;
    uint32_t phys = pmm_alloc_contig(DOSVM_READ_PAGES);
    if (!phys) { kfree(t); return 0; }
    mm_claim(phys, DOSVM_READ_PAGES, MM_VM_PRIVATE);
    *t = (struct vm_cache_block){.reclaim = {.phys = phys, .generation = v->info.key.generation,
        .owner = MM_VM_PRIVATE, .owner_id = v->as.pd_phys, .backing = &cache_provider}, .vm = v, .slot = slot};
    v->read_cache[slot].phys = phys; v->read_cache[slot].count = 0; v->read_cache[slot].track = t;
    v->read_pages += DOSVM_READ_PAGES;
    v->info.resident += DOSVM_READ_PAGES * PAGE_SIZE;
    if (reclaim_track) reclaim_track(&t->reclaim);
    return phys;
}
/* Leave the VM: free now, or orphan to a queued page-out (release frees). */
static void cache_block_drop(struct dosvm *v, unsigned slot)
{
    struct vm_cache_block *t = v->read_cache[slot].track;
    uint32_t phys = v->read_cache[slot].phys;
    v->read_cache[slot].phys = v->read_cache[slot].count = 0; v->read_cache[slot].track = 0;
    if (!phys) return;
    v->read_pages -= DOSVM_READ_PAGES;
    v->info.resident -= DOSVM_READ_PAGES * PAGE_SIZE;
    if (t) {
        uint32_t flags = irq_save();
        if (t->holds) { t->vm = 0; irq_restore(flags); return; }
        if (reclaim_untrack) reclaim_untrack(&t->reclaim);
        irq_restore(flags);
        kfree(t);
    }
    run_free(phys);
}
static void read_cache_close(struct dosvm *v)
{
    for (unsigned i = 0; i < v->read_capacity; i++) {
        cache_block_drop(v, i);
        memset(&v->read_cache[i], 0, sizeof(v->read_cache[i]));
        /* F0-44: one block (16 pages) released and published per span;
         * the caller holds no disk gate here (close/open failure paths). */
        kpreempt_point();
    }
    v->read_capacity = v->read_recent = v->read_oldest = 0;
}
static int read_cache_open(struct dosvm *v)
{
    /* Keep the image resident when the admitted commitment permits it.
     * Creation has not allocated the worker yet: reserve its full charge
     * before spending any headroom. Reopen already includes that charge.
     * Never reduce the guest pool or silently enlarge its admission. */
    const unsigned block_bytes = DOSVM_READ_SECTORS * 512u;
    uint32_t worker = v->task ? 0 : KSTACK_SIZE + PAGE_SIZE + sizeof(struct task);
    /* f3-46: the pool's whole quota is committed whether or not resident. */
    uint64_t used = (uint64_t)v->info.resident + worker +
        (uint64_t)(v->memory.pool.pages - v->memory.pool.resident) * PAGE_SIZE;
    uint32_t headroom = used <= v->info.commitment ? v->info.commitment - used : 0;
    unsigned blocks = (v->base_sectors + DOSVM_READ_SECTORS - 1) / DOSVM_READ_SECTORS;
    if (blocks > DOSVM_READ_BLOCKS) blocks = DOSVM_READ_BLOCKS;
    if (blocks * block_bytes > headroom && blocks > DOSVM_READ_FALLBACK_BLOCKS)
        blocks = DOSVM_READ_FALLBACK_BLOCKS;
    if (blocks * block_bytes > headroom) return -ENOMEM;
    v->read_capacity = blocks;
    for (unsigned i = 0; i < blocks; i++) {
        v->read_cache[i].previous = i ? i - 1 : blocks;
        v->read_cache[i].next = i + 1;
        if (!cache_block_new(v, i)) { read_cache_close(v); return -ENOMEM; }
        /* F0-41: allocation published; no disk gate or partial I/O held. */
        kpreempt_point();
    }
    v->read_recent = 0; v->read_oldest = blocks ? blocks - 1 : 0;
    return 0;
}
static void disk_gate_take(struct dosvm *v);
static void disk_gate_drop(struct dosvm *v);
/* f3-51 launch identity: SHA-256 of the whole read-only base, before any
 * guest read. One seek, then sequential reads of one read-cache block
 * (64 KiB) each: a seek cancels the VFS read-ahead stream, and before f3-51c
 * every FAT read walked the cluster chain from the first cluster, so the
 * former 512-byte seek+read loop cost O(sectors x clusters) FAT lookups,
 * about 1.8e8 for the 27 MB games disk: the window CREATE never returned.
 * The open file's FAT cluster cursor (f3-51c) now continues each read from
 * the previous one's last cluster, so this loop is linear in the chain.
 * Block 0 is the buffer; its count stays 0, so it publishes no cached data. */
static int base_hash_read(struct dosvm *v, uint8_t *buffer, uint32_t size)
{
    const unsigned block_bytes = DOSVM_READ_SECTORS * 512u;
    uint64_t position = 0;
    disk_gate_take(v);
    int e = vfs_seek(&v->disk_table, v->base, 0, VFS_SEEK_SET, &position);
    disk_gate_drop(v);
    if (e) return e;
    if (position) return -FS_EIO;
    struct sha256_ctx hash; uint8_t binary[32];
    sha256_init(&hash);
    for (uint32_t at = 0; at < size;) {
        unsigned amount = size - at < block_bytes ? size - at : block_bytes;
        size_t got = 0;
        disk_gate_take(v);
        e = vfs_read(&v->disk_table, v->base, buffer, amount, &got);
        disk_gate_drop(v);
        if (e || got != amount) return e ? e : -FS_EIO;
        sha256_update(&hash, buffer, amount); at += amount;
        kpreempt_point();
        /* fx-4: an owner stopped during a cold hash ends creation now. */
        if (v->create_cancelled && v->create_cancelled(v->create_cancel_arg)) return -EINTR;
    }
    sha256_final(&hash, binary); sha256_hex(binary, v->base_sha256);
    return 0;
}
static int base_hash(struct dosvm *v, uint32_t size)
{
    if (!v->read_capacity || v->read_cache[0].count || !v->read_cache[0].phys) return -EINVAL;
    /* f3-46: block 0 is the hash buffer; the reclaimer must not take it
     * while the gate is dropped between reads. */
    struct vm_cache_block *track = v->read_cache[0].track;
    if (track) track->busy = true;
    int e = base_hash_read(v, P2V(v->read_cache[0].phys), size);
    if (track) track->busy = false;
    return e;
}
/* f3-51c: SHA-256 of game/kernel bases per content version for the session,
 * bounded LRU. vfs_handle_version names the volume attachment and media
 * generation, the owning directory slot, first cluster, size, modification
 * time and the VFS write generation, which advances on every write,
 * truncate, rename from/to and unlink of that slot and on volume expiry (raw
 * writes, removal); a remount is a new attachment. A digest is stored only
 * when the version is unchanged across the whole hash (the base is open
 * deny-write throughout), so item 8's identity guarantee is unchanged: the
 * digest still names exactly the bytes the VM boots. A bounded version
 * watch prevents unrelated 64-slot change-ledger eviction from invalidating
 * a cached digest; it pins no handle, page, volume or write-deny claim. */
#define DOSVM_DIGESTS 16u
/* Every digest pin plus the boot-time image check's own (dosvm_image.c). */
_Static_assert(VFS_VERSION_PINS >= DOSVM_DIGESTS + 1u, "version watches: digests plus the image check");
static struct {
    struct vfs_file_version version;
    struct vfs_version_pin pin;
    char sha256[65];
    bool valid;
    uint32_t used;
} digests[DOSVM_DIGESTS];
static uint32_t digest_clock;
void dosvm_game_digest_invalidate(void)
{
    struct vfs_version_pin pins[DOSVM_DIGESTS];
    uint32_t flags=irq_save();
    for (unsigned i=0;i<DOSVM_DIGESTS;i++) pins[i]=digests[i].pin;
    memset(digests,0,sizeof(digests));
    irq_restore(flags);
    for (unsigned i=0;i<DOSVM_DIGESTS;i++) vfs_version_unpin(&pins[i]);
}
static bool digest_lookup(const struct vfs_file_version *version, char out[65])
{
    bool hit = false;
    uint32_t flags = irq_save();
    for (unsigned i = 0; i < DOSVM_DIGESTS && !hit; i++)
        if (digests[i].valid && vfs_file_version_equal(&digests[i].version, version)) {
            memcpy(out, digests[i].sha256, sizeof(digests[i].sha256));
            digests[i].used = ++digest_clock; hit = true;
        }
    irq_restore(flags);
    return hit;
}
static void digest_store(struct vfs_table *table,int handle,const struct vfs_file_version *version, const char sha256[65])
{
    struct vfs_version_pin pin={0};
    (void)vfs_version_pin(table,handle,version,&pin);
    uint32_t flags = irq_save();
    unsigned slot = DOSVM_DIGESTS;
    for (unsigned i = 0; i < DOSVM_DIGESTS && slot == DOSVM_DIGESTS; i++)
        if (digests[i].valid && vfs_file_version_equal(&digests[i].version, version)) slot = i;
    for (unsigned i = 0; i < DOSVM_DIGESTS && slot == DOSVM_DIGESTS; i++)
        if (!digests[i].valid) slot = i;
    if (slot == DOSVM_DIGESTS) {
        slot = 0; /* least recently used */
        for (unsigned i = 1; i < DOSVM_DIGESTS; i++) if (digests[i].used < digests[slot].used) slot = i;
    }
    struct vfs_version_pin old=digests[slot].pin;
    digests[slot].pin=pin;
    digests[slot].version = *version;
    memcpy(digests[slot].sha256, sha256, sizeof(digests[slot].sha256));
    digests[slot].sha256[64] = 0;
    digests[slot].valid = true; digests[slot].used = ++digest_clock;
    irq_restore(flags);
    vfs_version_unpin(&old);
}
/* fx-4b: the background verifier shares the session cache. */
bool dosvm_game_digest_find(const struct vfs_file_version *version, char out[65])
{ return digest_lookup(version, out); }
void dosvm_game_digest_publish(struct vfs_table *table, int handle, const struct vfs_file_version *version,
                               const char sha256[65])
{ digest_store(table, handle, version, sha256); }
/* Hash once per base content version. fx-4b: for the unified base a valid
 * verified stamp or the in-flight background verification of the same
 * version replaces the hash; otherwise the launch still hashes. */
static int base_identity(struct dosvm *v, uint32_t size, bool unified,
                         struct vfs_file_version *version, bool *versioned_out)
{
    struct vfs_file_version after;
    bool versioned = !vfs_handle_version(&v->disk_table, v->base, version) && version->size == size;
    *versioned_out = versioned;
    v->base_cached = versioned && digest_lookup(version, v->base_sha256);
    v->base_source = "session";
    if (v->base_cached) return 0;
    if (unified && versioned && dosvm_image_lookup) {
        const char *source = 0;
        int e = dosvm_image_lookup(&v->disk_table, version, v->base_sha256, &source,
                                   v->create_cancelled, v->create_cancel_arg);
        if (e == -EINTR) return e;
        if (!e) {
            v->base_cached = true; v->base_source = source;
            digest_store(&v->disk_table, v->base, version, v->base_sha256);
            return 0;
        }
    }
    v->base_source = "hash";
    int e = base_hash(v, size);
    if (e) return e;
    if (versioned && !vfs_handle_version(&v->disk_table, v->base, &after) &&
        vfs_file_version_equal(version, &after))
        digest_store(&v->disk_table, v->base, version, v->base_sha256);
    else *versioned_out = false;
    return 0;
}
static void *save_index_alloc(void *arg)
{
    struct dosvm *v=arg;
    if (v->info.resident>v->info.commitment || v->info.commitment-v->info.resident<PAGE_SIZE) return 0;
    uint32_t phys=pmm_alloc_contig(1); if (!phys) return 0;
    mm_claim(phys,1,MM_VM_PRIVATE); v->info.resident+=PAGE_SIZE;
    return P2V(phys);
}
static void save_index_release(void *arg,void *page)
{
    struct dosvm *v=arg; mm_free(V2P(page),MM_VM_PRIVATE); v->info.resident-=PAGE_SIZE;
}
int dosvm_disk_open(struct dosvm *v, const char *base)
{
    const char *step;
    return dosvm_disk_open_step(v, base, &step);
}
int dosvm_disk_open_step(struct dosvm *v, const char *base, const char **step)
{
    *step = "disk-state";
    if (v->base_open || v->table_ready || v->read_capacity) return -EBUSY;
    if (!base) base = DOSVM_DEFAULT_BASE;
    *step = "disk-base";
    if (!strncmp(base,"C:\\SYSTEM\\DOS\\DOSBOOT.IMG",sizeof("C:\\SYSTEM\\DOS\\DOSBOOT.IMG")) ||
        !strncmp(base,"C:\\SYSTEM\\DOS\\FREEDOS.IMG",sizeof("C:\\SYSTEM\\DOS\\FREEDOS.IMG"))) return -EINVAL;
    unsigned length = 0;
    while (length < 64 && base[length]) length++;
    if (!length || length == 64) return -EINVAL;
    const char *reason = !strncmp(base, DOSVM_BUNDLED_BASE, sizeof(DOSVM_BUNDLED_BASE)) ? "bundled" : "probe";
    *step = "storage";
    struct storage *s = storage_get();
    if (!s || !s->ready || s->stopped) return base_error(base, -V86_EIO, reason);
    int e = vfs_table_init(&s->vfs, &v->disk_table, 2);
    if (e) return base_error(base, e, reason);
    v->table_ready = true;
    /* sec-k05: every base, overlay, title-data, spool and export operation
     * on this table runs as the VM's retained subject (sec-k03 seam). */
    *step = "identity";
    if ((e = dos_identity_bind(&v->identity, &v->disk_table))) return base_error(base, e, "identity");
    *step = "base-stat";
    struct fat_entry entry;
    e = vfs_stat(&v->disk_table, base, &entry);
    if (e) return base_error(base, e, reason);
    *step = "base-size";
    if (!base_size_valid(entry.size) || (entry.attr & 0x10)) return base_error(base, -EINVAL, reason);
    v->base_sectors = entry.size / 512u;
    *step = "base-open";
    v->base = vfs_open(&v->disk_table, base, VFS_READ, VFS_DENY_WRITE, 0);
    if (v->base < 0) return base_error(base, v->base, reason);
    v->base_open = true;
    *step = "read-cache";
    e = read_cache_open(v);
    if (e) return e; /* lifecycle failure unwinds the base/table as before */
    if (v->game || v->kernel) {
        *step = "base-identity";
        bool unified = v->kernel==2 && !strncmp(base,DOSVM_DEFAULT_BASE,sizeof(DOSVM_DEFAULT_BASE));
        struct vfs_file_version version; bool versioned = false;
        e = base_identity(v, entry.size, unified, &version, &versioned);
        if (e) return e;
        if (unified) {
            *step = "base-sidecar";
            char expected[66];
            const char *sidecar="C:\\SYSTEM\\DOS\\UNIFIED.SHA";
            int identity=vfs_open(&v->disk_table,sidecar,VFS_READ,VFS_DENY_WRITE,0);
            if (identity<0) return identity;
            size_t got=0;
            disk_gate_take(v);
            int read_error=vfs_read(&v->disk_table,identity,expected,sizeof(expected),&got);
            disk_gate_drop(v);
            int closed=vfs_close(&v->disk_table,identity);
            if (read_error || got!=65 || expected[64]!='\n' || memcmp(expected,v->base_sha256,64) || closed) return -FS_EIO;
            /* fx-4b: a fresh synchronous hash that matched is stamped. */
            if (!v->base_cached && versioned && dosvm_image_publish)
                dosvm_image_publish(&v->disk_table, &version, v->base_sha256);
        }
    }
    if (v->game && !strncmp(base,DOSVM_DEFAULT_BASE,sizeof(DOSVM_DEFAULT_BASE))) {
        *step = "game-data";
        v->persistent=true;
        v->save.alloc=save_index_alloc; v->save.release=save_index_release; v->save.memory=v;
        v->save_file.table=&v->disk_table;
        struct dos_save_identity identity={.game=v->game,.kernel=v->kernel};
        memcpy(identity.base,v->base_sha256,sizeof(identity.base));
        unsigned archived=0;
        /* sec-k05: this user's title data under PROFILE(uid) (contract 8.4);
         * the machine namespace only for internal VMs and the interim
         * bootstrap session. No other user's store is ever opened. */
        char root[DOS_IDENTITY_PATH];
        e=dos_identity_root(&v->identity,DOS_ROOT_STORE,root,sizeof(root));
        disk_gate_take(v);
        if (!e) e=dos_identity_prepare(&v->identity,&v->disk_table,DOS_ROOT_STORE);
        if (!e) e=dos_save_store_open(&v->save_file,&v->save,&identity,root,v->overlay_path,&archived);
        disk_gate_drop(v);
        if (archived) klog("[dosvm] game-data game=%u action=archive reason=identity-mismatch count=%u base=%s error=%d",
                          v->game,archived,v->base_sha256,e);
        if (e==-FS_EROFS && v->save_file.fd<0 && v->save_file.absent) return 0;
        if (e) return e;
        v->overlay=v->save_file.fd; v->overlay_open=true;
        v->sector_count=v->save.rows;
        if (v->save.recovered_bytes || !v->save.sealed)
            klog("[dosvm] game-data game=%u reason=forced-session-end lost_bytes=%u committed_bytes=%u volatile_loss=unknown",
                 v->game,v->save.recovered_bytes,v->save.committed);
        *step = "none";
        return 0;
    }
    *step = "overlay";
    /* sec-k05: a transient overlay lives with the owner's private data. */
    char scratch[DOS_IDENTITY_PATH];
    e = dos_identity_root(&v->identity, DOS_ROOT_SCRATCH, scratch, sizeof(scratch));
    if (!e) e = dos_identity_prepare(&v->identity, &v->disk_table, DOS_ROOT_SCRATCH);
    if (e) return e;
    int written = ksnprintf(v->overlay_path, sizeof(v->overlay_path), "%s\\VM%08x.COW",
              scratch, v->info.key.generation);
    if (v->game || v->kernel)
        written = ksnprintf(v->overlay_path,sizeof(v->overlay_path),"%s\\G%uK%u%sV%08x.COW",
                  scratch,v->game,v->kernel,v->base_sha256,v->info.key.generation);
    if (written < 0 || (unsigned)written >= sizeof(v->overlay_path)) return -FS_ENAMETOOLONG;
    v->overlay = vfs_open(&v->disk_table, v->overlay_path,
        VFS_READ | VFS_WRITE | VFS_CREATE | VFS_EXCLUSIVE, VFS_DENY_ALL, 0);
    /* A metadata write gate must not hide readable game sectors. A caller
     * may open a read-only DOS disk; guest writes report write protection. */
    if (v->overlay == -FS_EROFS) { *step = "none"; return 0; }
    if (v->overlay < 0) return v->overlay;
    v->overlay_open = v->overlay_created = true;
    *step = "none";
    return 0;
}


static bool disk_gate_free(void *arg) { return !*(bool *)arg; }
static void disk_gate_take(struct dosvm *v)
{
    /* Test-and-set under a critical section: the worker and the controller
     * task may both reach the gate, and the kernel preempts at trap exits. */
    for (;;) {
        uint32_t flags = irq_save();
        if (!v->disk_busy) { v->disk_busy = true; irq_restore(flags); return; }
        irq_restore(flags);
        kwait_wait_until(&v->disk_wait, disk_gate_free, &v->disk_busy, deadline_after_ms(1000));
    }
}
/* f1-83 calldown context supplies the original DOS deadline. */
static int disk_gate_request(struct dosvm *v)
{
    const struct vfs_request *r=fs_request_current();
    if (!r) { disk_gate_take(v); return 0; }
    uint64_t begin=r->now(r->arg); int e;
    for (;;) {
        e=fs_request_check(); if (e) break;
        uint32_t flags=irq_save();
        if (!v->disk_busy) { v->disk_busy=true; irq_restore(flags); break; }
        irq_restore(flags); r->pause(r->arg);
    }
    fs_request_wait(VFS_WAIT_PROVIDER,begin,e); return e;
}
static void disk_gate_drop(struct dosvm *v) { v->disk_busy = false; kwait_wake_all(&v->disk_wait); }
int dosvm_disk_gate_enter(struct dosvm *v) { return disk_gate_request(v); }
void dosvm_disk_gate_leave(struct dosvm *v) { disk_gate_drop(v); }
static void disk_access(struct dosvm *v, uint32_t lba)
{
    uint32_t flags = irq_save();
    struct dosvm_disk_cost *d = &v->perf.interval[v->perf.active_interval].disk;
    unsigned region = lba / DOSVM_READ_SECTORS, byte = region / 8, bit = 1u << (region % 8);
    if (!(d->touched[byte] & bit)) { d->touched[byte] |= bit; d->regions++; }
    irq_restore(flags);
}
static void disk_cache_result(struct dosvm *v, bool hit)
{
    uint32_t flags = irq_save();
    struct dosvm_disk_cost *d = &v->perf.interval[v->perf.active_interval].disk;
    if (hit) { d->hits++; v->perf.disk_hits++; }
    else { d->misses++; v->perf.disk_misses++; }
    irq_restore(flags);
}
static int backing_io(struct dosvm *v, int handle, uint32_t offset, void *data, unsigned bytes, bool write)
{
    uint64_t position;
    size_t done = 0;
    int e = vfs_seek(&v->disk_table, handle, offset, VFS_SEEK_SET, &position);
    if (e) return e;
    if (position != offset) return -V86_EIO;
    uint32_t flags = irq_save();
    struct dosvm_disk_cost *d = &v->perf.interval[v->perf.active_interval].disk;
    if (write) d->writes++;
    else { d->reads++; v->perf.disk_reads++; }
    irq_restore(flags);
    e = write ? vfs_write(&v->disk_table, handle, data, bytes, &done) :
                vfs_read(&v->disk_table, handle, data, bytes, &done);
    if (!write) {
        flags = irq_save();
        d = &v->perf.interval[v->perf.active_interval].disk;
        d->bytes += done; v->perf.disk_bytes += done;
        if (done > v->perf.disk_max) v->perf.disk_max = done;
        irq_restore(flags);
    }
    return e ? e : done == bytes ? 0 : -V86_EIO;
}

static void read_promote(struct dosvm *v, unsigned slot)
{
    if (slot == v->read_recent) return;
    unsigned prev = v->read_cache[slot].previous, next = v->read_cache[slot].next;
    v->read_cache[prev].next = next;
    if (next < v->read_capacity) v->read_cache[next].previous = prev;
    else v->read_oldest = prev;
    v->read_cache[slot].previous = v->read_capacity;
    v->read_cache[slot].next = v->read_recent;
    v->read_cache[v->read_recent].previous = slot;
    v->read_recent = slot;
}
bool ledger_pressure(void) __attribute__((weak));
static void cache_touch(struct dosvm *v, unsigned slot)
{
    /* Access history for the global second chance (no translation maps it). */
    struct mm_page_descriptor *d = pmm_descriptor ? pmm_descriptor(v->read_cache[slot].phys) : 0;
    if (d) d->reserved |= MM_PAGE_TOUCHED;
}
static bool cache_slot_busy(const struct dosvm *v, unsigned slot)
{ return v->read_cache[slot].track && v->read_cache[slot].track->busy; }
/* f3-46: an empty resident block; else regrow a reclaimed block while the
 * system is out of pressure (never waiting for reclaim); else the least
 * recently used resident block; read_capacity when none is usable. */
static unsigned cache_victim(struct dosvm *v)
{
    unsigned reclaimed = v->read_capacity;
    for (unsigned i = 0; i < v->read_capacity; i++) if (!v->read_cache[i].count) {
        if (v->read_cache[i].phys && !cache_slot_busy(v, i)) return i;
        if (!v->read_cache[i].phys && reclaimed == v->read_capacity) reclaimed = i;
    }
    if (reclaimed < v->read_capacity && !(ledger_pressure && ledger_pressure()) && cache_block_new(v, reclaimed)) {
        v->read_regrown++;
        return reclaimed;
    }
    for (unsigned i = v->read_oldest, n = 0; i < v->read_capacity && n < v->read_capacity;
         i = v->read_cache[i].previous, n++)
        if (v->read_cache[i].phys && !cache_slot_busy(v, i)) return i;
    return v->read_capacity;
}
static unsigned read_find(struct dosvm *v, uint32_t lba)
{
    for (unsigned i = 0; i < v->read_capacity; i++)
        if (lba >= v->read_cache[i].lba && lba - v->read_cache[i].lba < v->read_cache[i].count)
            return i;
    return v->read_capacity;
}
static int read_sector_uncached(struct dosvm *v, uint32_t lba, void *sector)
{
    if (v->persistent && v->overlay_open) {
        int e=dos_save_read(&v->save,lba,sector);
        if (e!=-FS_ENOENT) return e;
    }
    for (unsigned i = 0; !v->persistent && i < v->sector_count; i++)
        if (v->sectors[i].lba == lba)
            return backing_io(v, v->overlay, v->sectors[i].offset, sector, 512, false);
    /* Past the end of a cut base image: the free FAT16 area, all zeros. */
    if (lba >= v->base_sectors) { memset(sector, 0, 512); return 0; }
    return backing_io(v, v->base, lba * 512u, sector, 512, false);
}
static int disk_read_locked(struct dosvm *v, uint32_t lba, void *sector)
{
    if (!v->base_open || lba >= DOSVM_SECTORS) return -EINVAL;
    disk_access(v, lba);
    unsigned slot = read_find(v, lba);
    if (slot < v->read_capacity) {
        memcpy(sector, (uint8_t *)P2V(v->read_cache[slot].phys) + (lba - v->read_cache[slot].lba) * 512u, 512);
        read_promote(v, slot); disk_cache_result(v, true); cache_touch(v, slot);
        return 0;
    }
    disk_cache_result(v, false);
    if (!v->read_capacity) return read_sector_uncached(v, lba, sector);
    slot = cache_victim(v);
    if (slot == v->read_capacity) return read_sector_uncached(v, lba, sector);
    cache_touch(v, slot);
    uint32_t start = lba & ~(DOSVM_READ_SECTORS - 1u);
    unsigned count = start < v->base_sectors ? v->base_sectors - start : 0;
    if (count > DOSVM_READ_SECTORS) count = DOSVM_READ_SECTORS;
    v->read_cache[slot].count = 0;
    uint8_t *bytes = P2V(v->read_cache[slot].phys);
    int e = count ? backing_io(v, v->base, start * 512u, bytes, count * 512u, false) : 0;
    if (count < DOSVM_READ_SECTORS) memset(bytes + count * 512u, 0, (DOSVM_READ_SECTORS - count) * 512u);
    if (v->persistent && v->overlay_open) for (unsigned i=0;!e && i<DOSVM_READ_SECTORS;i++) {
        int found=dos_save_read(&v->save,start+i,bytes+i*512u);
        if (found!=-FS_ENOENT) e=found;
    }
    for (unsigned i = 0; !v->persistent && !e && i < v->sector_count; i++) {
        uint32_t offset = v->sectors[i].lba - start;
        if (offset < DOSVM_READ_SECTORS)
            e = backing_io(v, v->overlay, v->sectors[i].offset, bytes + offset * 512u, 512, false);
    }
    /* Read-ahead errors outside the requested sector need not fail it.
     * Never publish either an incomplete base or an incomplete overlay merge. */
    if (e) return read_sector_uncached(v, lba, sector);
    v->read_cache[slot].lba = start; v->read_cache[slot].count = DOSVM_READ_SECTORS;
    read_promote(v, slot);
    memcpy(sector, bytes + (lba - start) * 512u, 512);
    return 0;
}

static int disk_write_locked(struct dosvm *v, uint32_t lba, const void *sector)
{
    if (!v->base_open || lba >= DOSVM_SECTORS) return -EINVAL;
    if (!v->overlay_open) return -FS_EROFS;
    if (v->persistent) {
        disk_access(v,lba);
        int e=dos_save_write(&v->save,lba,sector);
        if (e==-FS_ENOSPC) klog("[dosvm] game-data game=%u reason=game-data-full quota=%u bytes=%u action=read-only",
                              v->game,v->save.quota,v->save.end);
        if (!e) { v->info.dirty=true; v->sector_count=v->save.rows; }
        unsigned slot=read_find(v,lba);
        if (slot<v->read_capacity) v->read_cache[slot].count=0;
        return e;
    }
    unsigned i;
    for (i = 0; i < v->sector_count; i++) if (v->sectors[i].lba == lba) break;
    if (i == DOSVM_OVERLAY_SECTORS) return -ENOSPC;
    disk_access(v, lba);
    uint32_t offset = i == v->sector_count ? i * 512u : v->sectors[i].offset;
    /* A failed issued write may have changed private data; report its loss
     * even when no new sector can be published into the index. */
    v->info.dirty = true;
    int e = backing_io(v, v->overlay, offset, (void *)sector, 512, true);
    unsigned slot = read_find(v, lba);
    if (e) {
        /* Even a failed write may have changed an existing overlay sector. */
        if (slot < v->read_capacity) v->read_cache[slot].count = 0;
        return e;
    }
    if (i == v->sector_count) {
        v->sectors[i].lba = lba;
        v->sectors[i].offset = offset;
        v->sector_count++;
    }
    if (slot < v->read_capacity) {
        memcpy((uint8_t *)P2V(v->read_cache[slot].phys) + (lba - v->read_cache[slot].lba) * 512u, sector, 512);
        read_promote(v, slot);
    }
    return 0;
}

int dosvm_disk_read(struct dosvm *v, uint32_t lba, void *sector)
{
    /* -ENOSYS: a sector outside the file owner's logical volume (f3-61:
     * the MBR of a partitioned kernel=unified base) takes the plain path. */
    if (dosvm_file_disk && v->file_volume) {
        int e=dosvm_file_disk(v,lba,sector,false);
        if (e!=-ENOSYS) return e;
    }
    disk_gate_take(v);
    int e = disk_read_locked(v, lba, sector);
    disk_gate_drop(v);
    /* Never yield inside seek/read or while publishing a read-cache window,
     * nor from the VM worker's INT 13h emulation transaction. */
    if (g_current && g_current != v->task) kpreempt_point();
    return e;
}
int dosvm_disk_write(struct dosvm *v, uint32_t lba, const void *sector)
{
    if (dosvm_file_disk && v->file_volume) {
        int e=dosvm_file_disk(v,lba,(void *)sector,true);
        if (e!=-ENOSYS) return e;
    }
    disk_gate_take(v);
    int e = disk_write_locked(v, lba, sector);
    disk_gate_drop(v);
    return e;
}

/* The native FAT mount reads the existing logical source directly. Its
 * callbacks never invoke guest INT13 or recursively enter the raw/file gate.
 * f3-47 can replace the cache behind these source routines without changing
 * the mount adapter or introducing another resident cache. */
static int provider_read(struct blkdev *dev,uint64_t lba,uint32_t count,void *buffer)
{
    int e=blkdev_range(dev,lba,count); if (e) return e;
    struct dosvm *v=dev->ctx; int admission=disk_gate_request(v); if (admission) return admission;
    for (uint32_t i=0;!e && i<count;i++) e=disk_read_locked(v,(uint32_t)lba+i,(uint8_t *)buffer+i*512u);
    disk_gate_drop(v); return e;
}
static int provider_write(struct blkdev *dev,uint64_t lba,uint32_t count,const void *buffer)
{
    int e=blkdev_range(dev,lba,count); if (e) return e;
    struct dosvm *v=dev->ctx; int admission=disk_gate_request(v); if (admission) return admission;
    for (uint32_t i=0;!e && i<count;i++) e=disk_write_locked(v,(uint32_t)lba+i,(const uint8_t *)buffer+i*512u);
    disk_gate_drop(v); return e;
}
static int provider_flush(struct blkdev *dev)
{
    struct dosvm *v=dev->ctx; int admission=disk_gate_request(v); if (admission) return admission;
    int e=v->overlay_open ? (v->persistent ? (v->save_discarding ? 0 : dos_save_commit(&v->save,false)) : vfs_commit(&v->disk_table,v->overlay)) : 0;
    disk_gate_drop(v); return e;
}
void dosvm_disk_provider_init(struct dosvm *v,struct blkdev *provider)
{
    *provider=(struct blkdev){.read=provider_read,.write=provider_write,.flush=provider_flush,
        .capacity=DOSVM_SECTORS,.sector_size=512,.media_generation=v->info.key.generation,
        .write_cache_state=BLKDEV_CACHE_ENABLED,.logical=true,.ctx=v};
}

int dosvm_disk_close(struct dosvm *v, bool forced)
{
    disk_gate_take(v);
    int error = 0, e;
    if (v->overlay_open) {
        if (v->persistent) {
            if (!forced && v->save_file.writable) error=dos_save_commit(&v->save,true);
            if (forced || error) {
                v->info.data_loss=v->save.pending!=0 || v->save.failed;
                klog("[dosvm] game-data game=%u reason=forced-session-end lost_records=%u committed_bytes=%u error=%d volatile_loss=unknown",
                     v->game,v->save.pending,v->save.committed,error);
            }
        } else if (!forced && v->info.dirty) error = vfs_commit(&v->disk_table, v->overlay);
        e = vfs_close(&v->disk_table, v->overlay);
        if (!error) error = e;
        v->overlay_open = false;
    }
    dos_save_release(&v->save);
    if (v->base_open) {
        e = vfs_close(&v->disk_table, v->base);
        if (!error) error = e;
        v->base_open = false;
    }
    if (v->overlay_created) {
        if (v->info.dirty) klog("[dosvm] private-data vm=%u generation=%u action=discard reason=%s dirty=1 cleanup_error=%d",
            v->info.key.id,v->info.key.generation,forced ? "forced-session-end" : "transient-session-end",error);
        e = vfs_delete(&v->disk_table, v->overlay_path);
        if (e == -FS_ENOENT) e = 0;
        if (!error) error = e;
        if (!e) v->overlay_created = false;
    }
    if (v->table_ready && !v->overlay_created) {
        vfs_table_destroy(&v->disk_table);
        v->table_ready = false;
    }
    v->sector_count = 0;
    disk_gate_drop(v);
    /* The image and overlay are closed and the sector count is zero: no
     * reader can use the cache now, so release its blocks with the gate
     * dropped (one preemption point per block, F0-44). */
    read_cache_close(v);
    return error;
}

/* ---- f3-46 provider: VM read-cache blocks in the f0-48 replacement.
 * valid/retain/release run under IRQ exclusion; prepare/finish in pager
 * task context. A block in use (disk gate, hash buffer) is never taken. ---- */
static struct vm_cache_block *cache_block_of(struct reclaim_page *p)
{ return (struct vm_cache_block *)(void *)((char *)p - offsetof(struct vm_cache_block, reclaim)); }
static bool cache_block_live(const struct vm_cache_block *t)
{
    const struct dosvm *v = t->vm;
    return v && !t->busy && !v->disk_busy && t->slot < v->read_capacity && v->read_cache[t->slot].track == t;
}
static bool cache_block_valid(struct reclaim_page *p) { return cache_block_live(cache_block_of(p)); }
static void cache_block_retain(struct reclaim_page *p) { cache_block_of(p)->holds++; }
static bool cache_block_release(struct reclaim_page *p)
{
    struct vm_cache_block *t = cache_block_of(p);
    if (t->holds) t->holds--;
    if (t->holds || (t->vm && !t->evict)) return false;
    if (reclaim_untrack) reclaim_untrack(p);
    run_free(p->phys);
    kfree(t);
    return true;
}
static void cache_block_prepare(struct reclaim_page *p, struct reclaim_io *io, void *buffer)
{
    (void)buffer;
    struct vm_cache_block *t = cache_block_of(p);
    io->slot = 0; /* clean: the base image and overlay hold every byte */
    io->outcome = !t->vm ? RECLAIM_GONE : cache_block_live(t) ? RECLAIM_CLEAN : RECLAIM_DEFER;
}
static bool cache_block_finish(struct reclaim_page *p, const struct reclaim_io *io, bool clean)
{
    struct vm_cache_block *t = cache_block_of(p);
    struct dosvm *v = t->vm;
    if (!v || io->outcome != RECLAIM_CLEAN || !clean || !cache_block_live(t)) return true;
    unsigned slot = t->slot;
    v->read_cache[slot].phys = v->read_cache[slot].count = 0; v->read_cache[slot].track = 0;
    v->read_pages -= DOSVM_READ_PAGES;
    v->info.resident -= DOSVM_READ_PAGES * PAGE_SIZE;
    v->read_reclaimed++;
    t->evict = true; /* release frees the run after the queue drops its pin */
    return true;
}
static const struct reclaim_backing cache_provider = {cache_block_valid, cache_block_retain, cache_block_release,
    cache_block_finish, cache_block_prepare, 0};
