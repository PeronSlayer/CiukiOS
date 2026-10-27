/* fx-4b: the unified DOS base is verified in the background, never while a
 * user waits. Windows 98 does not hash its system files at startup: System
 * File Checker (Sfc.exe) verifies them on demand against a verification data
 * file (Resource Kit, wrkc27_2.htm and wrkc10_7.htm). CiukiOS keeps the f3-51
 * identity guarantee (the digest names the bytes the VM boots and must match
 * C:\SYSTEM\DOS\UNIFIED.SHA) but moves the cold SHA-256 out of CREATE:
 *
 * - after the first desktop frame an idle-class task hashes UNIFIED.IMG in
 *   64 KiB blocks (4 KiB hash steps with preemption points), pausing at
 *   least three times each block's read time (at most a quarter of the disk)
 *   unless a launch waits for it; a waiting launch lends its priority (kwait
 *   owner donation) and sees per-mille progress (CIUKI_DOSVM_IMAGE_QUERY);
 * - a verified match is recorded in C:\SYSTEM\DOS\UNIFIED.VFY, keyed by the
 *   file's on-disk identity (size, first cluster, directory cluster and
 *   slot, write date and time) and the expected SHA-256. FAT keeps no
 *   persistent write generation, so the stamp also lapses when the volume
 *   was recovered dirty at this mount (FAT[1] ClnShutBitMask 0: "did not
 *   Dismount the volume properly", Microsoft FAT On-Disk Format 1.02) or a
 *   game removal is pending, and the
 *   only kernel writer of the image (game removal) deletes it first;
 * - a later boot with an unchanged identity and sidecar reuses the stamp;
 *   any change hashes again; a mismatch is published, never stamped, and
 *   the launch refuses at the sidecar check with an honest record;
 * - the check pins the image's content version (vfs_version_pin) from its
 *   start and keeps the pin with a finished result: an image with no change
 *   record of its own reports the volume's change floor as its write
 *   generation, and the floor rises whenever more than VFS_CHANGES other
 *   files change on C: (QEMU 56e58374: "failed step=version" on every boot,
 *   so the first DOS Box hashed 27.7 MB inside CREATE). Changes of the image
 *   itself still advance the pinned version and void the result.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/abi.h>
#include <ciuki/sha256.h>
#include <ciuki/mm.h>
#include <ciuki/task.h>
#include <ciuki/storage.h>
#include "dosvm_image.h"

#define IMAGE_BASE "C:\\SYSTEM\\DOS\\UNIFIED.IMG"
#define IMAGE_SIDECAR "C:\\SYSTEM\\DOS\\UNIFIED.SHA"
#define STAMP_MAGIC "CIUVFY01"
#define STAMP_BYTES 128u
#define IV_BLOCK_PAGES 16u
#define IV_BLOCK (IV_BLOCK_PAGES * PAGE_SIZE)
#define IV_CHUNK 4096u
#define IV_SETTLE_MS 5000u
#define IV_PAUSE_MIN_MS 20u
#define IV_PAUSE_FACTOR 3u
#define IV_URGENT_MS 1500u
/* Weak: the host harness has no scheduler classes. */
#pragma weak task_set_class

static struct {
    struct kwait wake, done;
    struct task *task;
    bool desktop, armed, starting, busy, cancel, held, settled, raised;
    bool quiesced, resume_arm; /* sec-wire: stopped for the storage shutdown preflight */
    uint64_t urgent_until;
    unsigned waiters;
    uint32_t state, error, size, at, blocks, pauses;
    uint64_t started, block_started;
    struct vfs_file_version version;
    struct vfs_version_pin pin; /* the checked version; kept with a finished result */
    char expected[65], sha[65];
    const char *source;
    struct sha256_ctx hash;
    struct vfs_table table;
    int fd;
    bool table_ready;
    uint32_t buffer; /* physical 64 KiB block, IV_BLOCK_PAGES pages */
    bool test; /* host harness: no task, the test drives dosvm_image_step */
} iv = {.fd = -1};

/* Game removal (held) or the storage shutdown preflight (quiesced) keeps
 * new checks out. */
static bool held_now(void) { return iv.held || iv.quiesced; }
static bool hex64(const char *s)
{
    for (unsigned i = 0; i < 64; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}
/* The expected digest: 64 lowercase hex digits and a newline (f3-63). */
static int sidecar_read(struct vfs_table *t, char out[65])
{
    char text[66];
    int fd = vfs_open(t, IMAGE_SIDECAR, VFS_READ, VFS_DENY_WRITE, 0);
    if (fd < 0) return fd;
    size_t got = 0;
    int e = vfs_read(t, fd, text, sizeof(text), &got);
    int closed = vfs_close(t, fd);
    if (!e) e = closed;
    if (!e && (got != 65 || text[64] != '\n' || !hex64(text))) e = -FS_EIO;
    if (!e) { memcpy(out, text, 64); out[64] = 0; }
    return e;
}
static void put32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint32_t get32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
/* 128 bytes: magic, size, first cluster, directory cluster, slot, write
 * date/time, zero, digest (hex), SHA-256 of bytes 0..95. */
static void stamp_encode(uint8_t out[STAMP_BYTES], const struct vfs_file_version *v, const char sha[65])
{
    memset(out, 0, STAMP_BYTES);
    memcpy(out, STAMP_MAGIC, 8);
    put32(out + 8, v->size); put32(out + 12, v->first); put32(out + 16, v->parent); put32(out + 20, v->index);
    out[24] = (uint8_t)v->write_date; out[25] = (uint8_t)(v->write_date >> 8);
    out[26] = (uint8_t)v->write_time; out[27] = (uint8_t)(v->write_time >> 8);
    memcpy(out + 32, sha, 64);
    struct sha256_ctx c; sha256_init(&c); sha256_update(&c, out, 96); sha256_final(&c, out + 96);
}
static bool stamp_matches(const uint8_t in[STAMP_BYTES], const struct vfs_file_version *v, const char expected[65])
{
    uint8_t check[32]; struct sha256_ctx c;
    sha256_init(&c); sha256_update(&c, in, 96); sha256_final(&c, check);
    return !memcmp(in, STAMP_MAGIC, 8) && !memcmp(check, in + 96, 32) &&
           get32(in + 8) == v->size && get32(in + 12) == v->first && get32(in + 16) == v->parent &&
           get32(in + 20) == v->index && get32(in + 28) == 0 &&
           (uint16_t)(in[24] | in[25] << 8) == v->write_date && (uint16_t)(in[26] | in[27] << 8) == v->write_time &&
           !memcmp(in + 32, expected, 64);
}
static bool removal_pending(struct vfs_table *t)
{
    for (unsigned game = 1; game <= CIUKI_DOSVM_GAME_COUNT; game++) {
        char path[32]; struct fat_entry entry;
        ksnprintf(path, sizeof(path), "C:\\SYSTEM\\REMOVE%u.PND", game);
        int e = vfs_stat(t, path, &entry);
        if (e != -FS_ENOENT) return true; /* present, or unknown: do not trust */
    }
    return false;
}
static bool volume_recovered(const struct vfs_file_version *v)
{
    struct storage *s = storage_get();
    if (!s || v->drive >= 26) return true;
    const struct fat_volume *fat = &s->volumes[v->drive].fat;
    return fat->dirty_recovered || (fat->ro_reasons & (FAT_RO_DIRTY | FAT_RO_LOST | FAT_RO_IO_FLAG));
}
/* A verified stamp for exactly this identity and sidecar, or false. */
static bool stamp_valid(struct vfs_table *t, const struct vfs_file_version *v, const char expected[65])
{
    if (volume_recovered(v) || removal_pending(t)) return false;
    uint8_t record[STAMP_BYTES + 1];
    int fd = vfs_open(t, DOSVM_IMAGE_STAMP, VFS_READ, VFS_DENY_WRITE, 0);
    if (fd < 0) return false;
    size_t got = 0;
    int e = vfs_read(t, fd, record, sizeof(record), &got);
    int closed = vfs_close(t, fd);
    return !e && !closed && got == STAMP_BYTES && stamp_matches(record, v, expected);
}
static int stamp_write(struct vfs_table *t, const struct vfs_file_version *v, const char sha[65])
{
    uint8_t record[STAMP_BYTES];
    stamp_encode(record, v, sha);
    int fd = vfs_open(t, DOSVM_IMAGE_STAMP, VFS_WRITE | VFS_CREATE | VFS_TRUNCATE, VFS_DENY_ALL, 0);
    if (fd < 0) return fd;
    size_t done = 0;
    int e = vfs_write(t, fd, record, sizeof(record), &done);
    if (!e && done != sizeof(record)) e = -FS_EIO;
    if (!e) e = vfs_commit(t, fd);
    int closed = vfs_close(t, fd);
    return e ? e : closed;
}

/* ---- background verification ---- */
/* The pin is taken out under irq_save, together with the state it belongs
 * to, and released outside it (identity_lock may sleep). */
static struct vfs_version_pin pin_take_locked(void)
{
    struct vfs_version_pin old = iv.pin;
    iv.pin = (struct vfs_version_pin){0};
    return old;
}
static void pin_set(struct vfs_version_pin pin)
{
    uint32_t f = irq_save();
    struct vfs_version_pin old = pin_take_locked();
    iv.pin = pin;
    irq_restore(f);
    vfs_version_unpin(&old);
}
static void pin_drop(void) { pin_set((struct vfs_version_pin){0}); }
/* Pin the version the check starts from, so changes of other files on the
 * volume cannot make the untouched image look changed. A version that moved
 * between reading and pinning is read again; without a free watch the check
 * runs unpinned, as before (the end-of-check comparison stays). */
static void version_pin(struct vfs_file_version *version)
{
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        struct vfs_version_pin pin = {0};
        int e = vfs_version_pin(&iv.table, iv.fd, version, &pin);
        if (!e) { pin_set(pin); return; }
        if (e != -FS_EAGAIN || vfs_handle_version(&iv.table, iv.fd, version)) {
            klog("[dosvm] image verify pin error=%d", e);
            return;
        }
    }
    klog("[dosvm] image verify pin error=%d attempts=3 unpinned", -FS_EAGAIN);
}
static void finish(uint32_t state, int error, const char *source)
{
    if (iv.fd >= 0) (void)vfs_close(&iv.table, iv.fd);
    iv.fd = -1;
    if (iv.table_ready) vfs_table_destroy(&iv.table);
    iv.table_ready = false;
    if (iv.buffer) for (unsigned i = 0; i < IV_BLOCK_PAGES; i++) pmm_free(iv.buffer + i * PAGE_SIZE);
    iv.buffer = 0;
    /* A result (verified or mismatch) keeps its version pinned for the
     * launch (finished_result); anything else releases it. */
    struct vfs_version_pin old = {0};
    uint32_t f = irq_save();
    iv.state = state; iv.error = error < 0 ? (uint32_t)-error : (uint32_t)error; iv.source = source;
    iv.busy = iv.starting = false;
    if (state != CIUKI_DOSVM_IMAGE_VERIFIED && state != CIUKI_DOSVM_IMAGE_MISMATCH) old = pin_take_locked();
    irq_restore(f);
    vfs_version_unpin(&old);
    kwait_set_owner(&iv.done, 0);
    kwait_wake_all(&iv.done);
}
static void report(const char *result)
{
    klog("[dosvm] image verify result=%s source=%s bytes=%u ms=%u blocks=%u pauses=%u error=%u sha256=%s",
         result, iv.source ? iv.source : "none", iv.at, (uint32_t)(g_ticks - iv.started),
         iv.blocks, iv.pauses, iv.error, iv.sha[0] ? iv.sha : "none");
}
/* Tamper or damage: the bytes do not match UNIFIED.SHA. Never stamped; the
 * launch refuses at the sidecar check (dosvm_disk.c, step base-sidecar). */
static void mismatch_record(void)
{
    klog("[dosvm] image verify result=mismatch expected=%s actual=%s bytes=%u", iv.expected, iv.sha, iv.size);
}
/* Open, identify and either finish from the session cache/stamp or prepare
 * the hash. True when a verification is now busy and needs steps. */
static bool begin(void)
{
    struct storage *s = storage_get();
    uint32_t f = irq_save();
    if (iv.busy || iv.starting || held_now() || !iv.armed) { irq_restore(f); return false; }
    iv.armed = false; iv.cancel = false; iv.starting = true;
    iv.at = iv.blocks = iv.pauses = iv.error = 0; iv.size = 0; iv.sha[0] = 0; iv.source = "background";
    irq_restore(f);
    iv.started = g_ticks;
    pin_drop(); /* the previous result's version */
    struct vfs_file_version version;
    int e = !s || !s->ready || s->stopped ? -FS_EIO : vfs_table_init(&s->vfs, &iv.table, 2);
    const char *step = "storage";
    if (!e) { iv.table_ready = true; step = "base-open"; iv.fd = vfs_open(&iv.table, IMAGE_BASE, VFS_READ, VFS_DENY_WRITE, 0); }
    if (!e && iv.fd < 0) e = iv.fd;
    if (!e) { step = "base-version"; e = vfs_handle_version(&iv.table, iv.fd, &version); }
    if (!e) version_pin(&version);
    if (!e) { step = "base-sidecar"; e = sidecar_read(&iv.table, iv.expected); }
    if (e) {
        klog("[dosvm] image verify result=failed step=%s error=%d", step, e);
        finish(CIUKI_DOSVM_IMAGE_FAILED, e, "background");
        return false;
    }
    /* Busy only with a known version: a launch compares it to its own. */
    f = irq_save();
    iv.version = version; iv.size = version.size;
    iv.busy = true; iv.starting = false; iv.state = CIUKI_DOSVM_IMAGE_VERIFYING;
    irq_restore(f);
    kwait_set_owner(&iv.done, g_current);
    if (dosvm_game_digest_find(&iv.version, iv.sha)) {
        iv.source = "session"; iv.at = iv.size;
        bool match = !memcmp(iv.sha, iv.expected, 64);
        if (!match) mismatch_record();
        finish(match ? CIUKI_DOSVM_IMAGE_VERIFIED : CIUKI_DOSVM_IMAGE_MISMATCH, 0, "session");
        if (match) report("verified");
        return false;
    }
    if (stamp_valid(&iv.table, &iv.version, iv.expected)) {
        memcpy(iv.sha, iv.expected, 65); iv.at = iv.size;
        dosvm_game_digest_publish(&iv.table, iv.fd, &iv.version, iv.sha);
        finish(CIUKI_DOSVM_IMAGE_VERIFIED, 0, "stamp");
        report("verified");
        return false;
    }
    uint64_t position = 0;
    e = vfs_seek(&iv.table, iv.fd, 0, VFS_SEEK_SET, &position);
    if (!e && position) e = -FS_EIO;
    if (!e) { iv.buffer = pmm_alloc_contig(IV_BLOCK_PAGES); if (!iv.buffer) e = -FS_ENOMEM; }
    if (e) {
        klog("[dosvm] image verify result=failed step=prepare error=%d", e);
        finish(CIUKI_DOSVM_IMAGE_FAILED, e, "hash");
        return false;
    }
    sha256_init(&iv.hash); iv.source = "hash";
    klog("[dosvm] image verify stage=start bytes=%u", iv.size);
    return true;
}
/* fx-23: the check's reads are background I/O (fs_port.h): queued behind
 * every foreground request, no read-ahead on the storage writer, drop-behind
 * in the cache. While a launch waits for the result the user is waiting, so
 * its blocks are read as ordinary demand reads. */
/* Request clocks are in microseconds (blk_request_remaining_ms). */
static uint64_t iv_now(void *arg) { (void)arg; return (uint64_t)g_ticks * 1000u; }
static void iv_pause(void *arg) { (void)arg; task_sleep_ms(1); }
static struct vfs_request iv_request = {.deadline = UINT64_MAX / 2, .now = iv_now, .pause = iv_pause,
                                       .operation = "verify", .site = "vfs", .background = true};
static bool urgent(void);
static int block_read(void *block, uint32_t amount, size_t *got)
{
    if (urgent()) return vfs_read(&iv.table, iv.fd, block, amount, got);
    struct fs_request_scope scope;
    fs_request_enter(&scope, &iv_request);
    int e = vfs_read(&iv.table, iv.fd, block, amount, got);
    fs_request_leave(&scope);
    return e;
}
/* One 64 KiB block; false once finished. */
static bool step_block(void)
{
    struct storage *s = storage_get();
    if (iv.cancel || !s || s->stopped) {
        bool cancelled = iv.cancel;
        iv.error = cancelled ? ECANCELED : EIO;
        report(cancelled ? "cancelled" : "stopped");
        finish(cancelled ? CIUKI_DOSVM_IMAGE_UNCHECKED : CIUKI_DOSVM_IMAGE_FAILED,
               cancelled ? -ECANCELED : -EIO, "hash");
        return false;
    }
    if (iv.at < iv.size) {
        uint32_t amount = iv.size - iv.at < IV_BLOCK ? iv.size - iv.at : IV_BLOCK;
        uint8_t *block = P2V(iv.buffer);
        size_t got = 0;
        iv.block_started = g_ticks;
        int e = block_read(block, amount, &got);
        if (!e && got != amount) e = -FS_EIO;
        if (e) {
            klog("[dosvm] image verify result=failed step=read at=%u error=%d", iv.at, e);
            finish(CIUKI_DOSVM_IMAGE_FAILED, e, "hash");
            return false;
        }
        for (uint32_t done = 0; done < amount; done += IV_CHUNK) {
            sha256_update(&iv.hash, block + done, amount - done < IV_CHUNK ? amount - done : IV_CHUNK);
            kpreempt_point();
        }
        iv.at += amount; iv.blocks++;
        if (iv.at < iv.size) return true;
    }
    uint8_t binary[32]; struct vfs_file_version after;
    sha256_final(&iv.hash, binary); sha256_hex(binary, iv.sha);
    int e = vfs_handle_version(&iv.table, iv.fd, &after);
    if (e || !vfs_file_version_equal(&after, &iv.version)) {
        /* Deny-write holds the bytes; a raw change still voids the result. */
        klog("[dosvm] image verify result=failed step=version error=%d", e ? e : -FS_EAGAIN);
        iv.sha[0] = 0;
        finish(CIUKI_DOSVM_IMAGE_UNCHECKED, e ? e : -FS_EAGAIN, "hash");
        return false;
    }
    dosvm_game_digest_publish(&iv.table, iv.fd, &iv.version, iv.sha);
    bool match = !memcmp(iv.sha, iv.expected, 64);
    if (match) {
        int written = stamp_write(&iv.table, &iv.version, iv.sha);
        if (written) klog("[dosvm] image stamp write error=%d", written);
    } else mismatch_record();
    finish(match ? CIUKI_DOSVM_IMAGE_VERIFIED : CIUKI_DOSVM_IMAGE_MISMATCH, 0, "hash");
    if (match) report("verified");
    return false;
}
/* A launch is waiting for the result (CREATE) or watching it (QUERY). */
static bool urgent(void) { return iv.waiters || g_ticks < iv.urgent_until; }
/* Pause after a block: at least three times its read time (the verifier
 * uses at most a quarter of the disk), none while a launch waits. */
static uint32_t pause_ms(void)
{
    if (urgent()) return 0;
    uint64_t spent = g_ticks - iv.block_started;
    uint64_t pause = spent * IV_PAUSE_FACTOR;
    return pause < IV_PAUSE_MIN_MS ? IV_PAUSE_MIN_MS : pause > 1000 ? 1000u : (uint32_t)pause;
}
static bool armed_ready(void *arg) { (void)arg; return iv.armed && !held_now(); }
static bool settle_over(void *arg) { (void)arg; return urgent() || held_now(); }
/* Idle class; a waiting or watching launch raises it to normal for the
 * remaining blocks (and a CREATE waiter also lends its own level). */
static void verifier_class(void)
{
    bool raise = urgent();
    if (raise != iv.raised && task_set_class) { task_set_class(g_current, raise ? P_NORMAL : P_IDLE); iv.raised = raise; }
}
static void verifier_main(void *arg)
{
    (void)arg;
    for (;;) {
        kwait_wait_until(&iv.wake, armed_ready, 0, UINT64_MAX);
        if (!iv.settled) {           /* let the first desktop frames settle */
            iv.settled = true;
            kwait_wait_until(&iv.wake, settle_over, 0, deadline_after_ms(IV_SETTLE_MS));
        }
        verifier_class();
        if (!begin()) continue;
        for (;;) {
            verifier_class();
            if (!step_block()) break;
            uint32_t pause = pause_ms();
            if (pause) { iv.pauses++; task_sleep_ms(pause); }
        }
    }
}
/* Host harness: one verification step (begin, or one block). */
bool dosvm_image_step(void) { return iv.busy ? step_block() : begin(); }
void dosvm_image_test_mode(void)
{
    memset(&iv, 0, sizeof(iv)); iv.fd = -1; iv.test = true;
    kwait_init(&iv.wake); kwait_init(&iv.done);
}

/* The first full desktop frame (or the console stand-in) arms the first
 * verification. Deterministic test and hardware-check boots keep the
 * synchronous launch path (no task, nothing armed). */
void dosvm_image_desktop_presented(void)
{
    uint32_t f = irq_save();
    bool first = !iv.desktop;
    iv.desktop = true;
    irq_restore(f);
    if (!first) return;
    if (!iv.test) {
        if (g_boot.flags & (CBI_F_TEST_REQUEST | CBI_F_HW_CHECK)) return;
        kwait_init(&iv.wake); kwait_init(&iv.done);
        iv.task = task_create_kernel("dos-image", verifier_main, 0, P_IDLE);
        if (!iv.task) { klog("[dosvm] image verify result=failed step=task error=%d", -ENOMEM); return; }
    }
    f = irq_save();
    if (!held_now()) iv.armed = true;
    irq_restore(f);
    if (iv.task) task_start(iv.task);
}

static bool not_busy(void *arg) { (void)arg; return !iv.busy && !iv.starting; }
/* Game removal: stop a verification that holds the image open and keep new
 * ones out until dosvm_image_release. */
void dosvm_image_hold(void)
{
    uint32_t f = irq_save();
    iv.held = true; iv.armed = false;
    if (iv.busy) iv.cancel = true;
    irq_restore(f);
    if (iv.task) kwait_wake_all(&iv.wake);
    while (iv.busy || iv.starting) {
        if (iv.busy) iv.cancel = true; /* a start that was opening meanwhile */
        if (iv.test) { (void)dosvm_image_step(); continue; }
        kwait_wait_until(&iv.done, not_busy, 0, deadline_after_ms(1000));
    }
    pin_drop();
}
/* The image may have changed: forget the result and verify again. */
void dosvm_image_release(void)
{
    uint32_t f = irq_save();
    iv.held = false; iv.cancel = false; iv.state = CIUKI_DOSVM_IMAGE_UNCHECKED; iv.sha[0] = 0;
    struct vfs_version_pin old = pin_take_locked();
    if (iv.desktop && (iv.task || iv.test)) iv.armed = true;
    irq_restore(f);
    vfs_version_unpin(&old);
    if (iv.task) kwait_wake_all(&iv.wake);
}
/* sec-wire (QEMU run of image ae977fbe): the check reads UNIFIED.IMG
 * through its own kernel VFS table, so the shutdown preflight (fs/mount.c
 * shutdown_volumes) refused nine power-off attempts with "[storage]
 * shutdown busy drive=C holder=open" until the first-boot hash of 54.6 MB
 * finished (ms=21862, pauses=833). Windows 98 has no such boot-time reader
 * (System File Checker runs on demand, Resource Kit wrkc27_2/wrkc10_7), and
 * here the check is a deferrable convenience: storage_shutdown stops it
 * first, within wait_ms, the way game removal does (dosvm_image_hold), and
 * a refused shutdown resumes it. A finished result (verified or mismatch)
 * and its pin are kept; a check that was armed, starting or running (it
 * ends cancelled, unchecked) is armed again on resume. 0, or -EBUSY when the check still holds the image at the
 * deadline (the preflight then refuses before any change). */
static bool pending(void);
int dosvm_image_shutdown(uint32_t wait_ms)
{
    uint32_t f = irq_save();
    if (!iv.quiesced) iv.resume_arm = pending();
    iv.quiesced = true; iv.armed = false;
    if (iv.busy) iv.cancel = true;
    irq_restore(f);
    if (iv.task) kwait_wake_all(&iv.wake);
    uint64_t end = deadline_after_ms(wait_ms);
    while (iv.busy || iv.starting) {
        if (iv.busy) iv.cancel = true; /* a start that was opening meanwhile */
        if (iv.test) { (void)dosvm_image_step(); continue; }
        if (deadline_passed(end)) {
            klog("[dosvm] image verify shutdown stop timeout wait_ms=%u", wait_ms);
            return -EBUSY;
        }
        kwait_wait_until(&iv.done, not_busy, 0, end);
    }
    return 0;
}
void dosvm_image_shutdown_resume(void)
{
    uint32_t f = irq_save();
    /* Restore exactly the pending check the preflight took away. */
    if (iv.quiesced && iv.resume_arm && iv.desktop && (iv.task || iv.test)) iv.armed = true;
    iv.quiesced = iv.resume_arm = false;
    if (!iv.busy) iv.cancel = false;
    irq_restore(f);
    if (iv.task) kwait_wake_all(&iv.wake);
}

/* Launch path (dosvm_disk.c base_identity, unified base only, after a
 * session-cache miss). An armed, starting or running verification is
 * waited for (urgent, the waiter lends its priority, cancellable by the
 * creating owner) and its result used when it names this same version;
 * otherwise a verified stamp for this identity and sidecar. -ENOENT: the
 * caller hashes now, as before fx-4b. */
static bool pending(void) { return (iv.armed && !held_now()) || iv.starting || iv.busy; }
static bool settled_result(void *arg) { (void)arg; return !pending(); }
/* fx-23: the result of a check that already finished in this session, for
 * this exact version. The hash was made after this mount, so the stamp's
 * "recovered at mount" lapse does not apply to it. */
static bool finished_result(const struct vfs_file_version *version, char sha[65])
{
    uint32_t f = irq_save();
    bool hit = !pending() && (iv.state == CIUKI_DOSVM_IMAGE_VERIFIED || iv.state == CIUKI_DOSVM_IMAGE_MISMATCH) &&
               iv.sha[0] && vfs_file_version_equal(&iv.version, version);
    if (hit) memcpy(sha, iv.sha, 65);
    irq_restore(f);
    return hit;
}
/* fx-23: no usable result: verify again on the background task (raised while
 * this launch waits, progress in IMAGE_QUERY) instead of hashing 27.7 MB
 * synchronously on the launching thread inside CREATE. False when there is
 * no verifier (test-request and hardware-check boots keep the synchronous
 * path) or a game removal holds the image. */
static bool rearm(void)
{
    uint32_t f = irq_save();
    bool ok = iv.desktop && (iv.task || iv.test) && !held_now();
    if (ok && !pending()) { iv.armed = true; iv.settled = true; }
    irq_restore(f);
    return ok;
}
int dosvm_image_lookup(struct vfs_table *t, const struct vfs_file_version *version, char sha[65],
                       const char **source, bool (*cancelled)(void *), void *arg)
{
    char expected[65];
    if (finished_result(version, sha)) { *source = "background"; return 0; }
    bool rearmed = false;
again:
    if (pending()) {
        uint32_t f = irq_save(); iv.waiters++; irq_restore(f);
        if (iv.task) kwait_wake_all(&iv.wake);
        int e = 0;
        while (pending()) {
            if (cancelled && cancelled(arg)) { e = -EINTR; break; }
            if (iv.test) { (void)dosvm_image_step(); kpreempt_point(); continue; }
            kwait_wait_until(&iv.done, settled_result, 0, deadline_after_ms(100));
        }
        f = irq_save(); iv.waiters--; irq_restore(f);
        if (e) return e;
        if ((iv.state == CIUKI_DOSVM_IMAGE_VERIFIED || iv.state == CIUKI_DOSVM_IMAGE_MISMATCH) &&
            iv.sha[0] && vfs_file_version_equal(&iv.version, version)) {
            memcpy(sha, iv.sha, 65); *source = "background"; return 0;
        }
    }
    if (!sidecar_read(t, expected) && stamp_valid(t, version, expected)) {
        memcpy(sha, expected, 65); *source = "stamp";
        return 0;
    }
    /* One fresh check of the current version; a second miss (the file
     * changed during it, or the check failed) falls back to the caller. */
    if (!rearmed && rearm()) { rearmed = true; goto again; }
    return -ENOENT;
}
/* Launch path after a synchronous hash matched the sidecar: stamp it so the
 * next boot does not hash, and report it verified. */
void dosvm_image_publish(struct vfs_table *t, const struct vfs_file_version *version, const char sha[65])
{
    int e = stamp_write(t, version, sha);
    if (e) klog("[dosvm] image stamp write error=%d", e);
    /* The launch's digest store pins this version; an older pin is stale. */
    struct vfs_version_pin old = {0};
    uint32_t f = irq_save();
    if (!iv.busy && !iv.starting) {
        iv.version = *version; memcpy(iv.sha, sha, 65); iv.size = iv.at = version->size;
        iv.state = CIUKI_DOSVM_IMAGE_VERIFIED; iv.source = "launch"; iv.error = 0;
        old = pin_take_locked();
    }
    irq_restore(f);
    vfs_version_unpin(&old);
}
/* CIUKI_DOSVM_IMAGE_QUERY: state, per-mille progress, positive errno. A
 * launcher watching an armed or running check makes it urgent. */
void dosvm_image_query(uint32_t *state, uint32_t *progress, uint32_t *error)
{
    uint32_t f = irq_save();
    bool waiting = pending();
    *state = waiting ? CIUKI_DOSVM_IMAGE_VERIFYING : iv.state;
    *error = *state == CIUKI_DOSVM_IMAGE_FAILED ? iv.error : 0;
    *progress = iv.busy && iv.size ? (uint32_t)((uint64_t)iv.at * 1000u / iv.size) : 0;
    if (*state == CIUKI_DOSVM_IMAGE_VERIFIED || *state == CIUKI_DOSVM_IMAGE_MISMATCH) *progress = 1000;
    if (waiting) iv.urgent_until = g_ticks + IV_URGENT_MS;
    irq_restore(f);
    if (waiting && iv.task) kwait_wake_all(&iv.wake);
}
