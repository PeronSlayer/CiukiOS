/* ATAPI media lifecycle; shared ATA owner, ISO read-only namespace.
 * SFF-8020i 10.6 / 10.8.26.1: docs/design/optical-drive-binding.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/ata.h>
#include <ciuki/storage.h>
#include <ciuki/vfs_hooks.h>

#define OPTICAL_POLL_MS 1000u
int optical_bind(struct storage *s, unsigned slot, struct ata_device *d)
{
    if (!s || !s->ready || s->stopped || slot >= STORAGE_DISKS || !d) return -FS_EINVAL;
    int e = !d->identified || !d->atapi || d->packet_type != 5 ||
        (d->packet_bytes != 12 && d->packet_bytes != 16) ? -FS_EOPNOTSUPP :
        d->block.quarantined || d->channel->stopped || d->channel->quiescing ? -FS_EQUARANTINED : 0;
    klog("[optical] bind slot=%u channel=%u unit=%u identified=%u type=%u packet_bytes=%u dma=%u mode=pio sector_size=%u blkpart=skip result=%d",
         slot, d->channel->index, d->unit, d->identified, d->packet_type,
         d->packet_bytes, d->dma_enabled, d->block.sector_size, e);
    if (e) return e;
    if (s->optical_bindings[slot].device) return -FS_EBUSY;
    s->optical_bindings[slot].device = d;
    return 0;
}
void optical_service(struct storage *s, uint64_t now)
{
    if (!s || !s->ready || s->stopped) return;
    for (unsigned slot = 0; slot < STORAGE_DISKS; slot++) {
        struct optical_binding *b = &s->optical_bindings[slot];
        struct ata_device *d = b->device;
        if (!d || now < b->next_poll) continue;
        b->next_poll = now + OPTICAL_POLL_MS;
        vfs_lock(&s->vfs,0);
        if (s->stopped) { vfs_unlock(&s->vfs); return; }
        uint32_t mask=0;
        for (unsigned drive=3;drive<26;drive++) if (s->vfs.ro_volumes[drive]==&s->optical[slot]) mask|=1u<<drive;
        if (!mask) { unsigned drive=s->next_drive; while (drive<26 && vfs_drive_present(&s->vfs,drive)) drive++; mask=vfs_drive_mask(drive); }
        vfs_reserve_locked(&s->vfs,mask);
        int e = VFS_IO(&s->vfs,atapi_ready(d));
        for (unsigned drive = 3; drive < 26; drive++)
            if (s->vfs.ro_volumes[drive] == &s->optical[slot] &&
                (e || s->optical[slot].mount_generation != d->block.media_generation)) {
                /* Failed readiness must expire the old mount even when a
                 * device reports an unclassified medium error. */
                vfs_ro_expire_locked(&s->vfs, drive);
                klog("[optical] bind slot=%u stage=detach drive=%c generation=%u result=%d",
                     slot, 'A' + drive, d->block.media_generation, e);
            }
        if (!e && d->toc.valid && d->toc.first_audio) {
            /* f5-12B2: an audio-first disc is ready for /dev/media CD audio
             * only; nothing is mounted and no ISO descriptor is probed. */
            if (b->attempted_generation != d->block.media_generation) {
                b->attempted_generation = d->block.media_generation;
                klog("[optical] bind slot=%u stage=audio generation=%u tracks=%u audio_only=%u result=0 reason=audio_cd",
                     slot, d->block.media_generation, d->toc.count, d->toc.audio_only);
            }
        } else if (!e && !s->optical[slot].mounted && b->attempted_generation != d->block.media_generation) {
            b->attempted_generation = d->block.media_generation;
            /* Expiration and a new mount are separate operations. Do not
             * borrow another letter while holding the old reservation: a
             * pending shutdown can already have queued that letter. */
            vfs_unlock(&s->vfs);
            e = storage_add_ro(s, slot, &d->block, d->session_lba);
            vfs_lock(&s->vfs,0);
            klog("[optical] bind slot=%u stage=iso drive=%c mode=ro generation=%u session=%u result=%d reason=%s",
                 slot, e >= 3 ? 'A' + e : '-', d->block.media_generation, d->session_lba, e,
                 e >= 3 ? "mounted" : s->optical[slot].diagnostic ? s->optical[slot].diagnostic : "mount_refused");
        } else if (!e && s->optical[slot].mounted) {
            for (unsigned drive = 3; drive < 26; drive++)
                if (s->vfs.ro_volumes[drive] == &s->optical[slot]) { e = (int)drive; break; }
        } else if (!e) e = b->result; /* retain a refusal for this generation */
        if (b->observed_generation != d->block.media_generation || b->result != e) {
            klog("[storage] optical slot=%u drive=%c mode=ro generation=%u result=%d reason=%s",
                 slot, e >= 3 ? 'A' + e : '-', d->block.media_generation, e,
                 e >= 3 ? "mounted" : d->media == ATAPI_NO_MEDIUM ? "no_medium" :
                 !e && d->toc.valid && d->toc.first_audio ? "audio_cd" :
                 d->ready_error ? d->ready_stage : s->optical[slot].diagnostic ? s->optical[slot].diagnostic : "unavailable");
            b->observed_generation = d->block.media_generation; b->result = e;
        }
        vfs_unlock(&s->vfs);
    }
}
int optical_eject(struct storage *s, unsigned drive, uint32_t generation)
{
    if (!s || !s->ready || s->stopped || drive < 3 || drive >= 26 || !generation) return -FS_EINVAL;
    vfs_lock(&s->vfs,vfs_drive_mask(drive));
    int e = -FS_ENOENT;
    for (unsigned slot = 0; slot < STORAGE_DISKS; slot++) {
        struct ata_device *d = s->optical_bindings[slot].device;
        struct fs_ro_volume *ro = &s->optical[slot];
        if (!d || s->vfs.ro_volumes[drive] != ro) continue;
        if (generation != d->block.media_generation || VFS_IO(&s->vfs,fs_ro_check(ro))) { e = -FS_EIO; break; }
        e = 0;
        for (unsigned i = 0; i < VFS_NODES; i++)
            if (s->vfs.nodes[i].refs && s->vfs.nodes[i].drive == drive &&
                s->vfs.nodes[i].generation == s->vfs.generation[drive]) e = -FS_EBUSY;
        if (px_ro_busy && px_ro_busy(&s->vfs, ro)) e = -FS_EBUSY;
        if (!e) {
            e = vfs_detach_locked(&s->vfs, drive);
            if (!e) {
                e = VFS_IO(&s->vfs,atapi_eject(d));
                /* A failed eject leaves the disc unmounted. Let readiness
                 * publish it again on the next service turn. */
                s->optical_bindings[slot].attempted_generation = 0;
            }
        }
        klog("[optical] media slot=%u stage=eject drive=%c generation=%u result=%d",
             slot, 'A' + drive, generation, e);
        break;
    }
    vfs_unlock(&s->vfs); return e;
}
#ifndef FS_HOST
static void optical_task(void *arg)
{
    struct storage *s = arg;
    while (!s->stopped) { optical_service(s, deadline_after_ms(0)); task_sleep_ms(100); }
    task_exit(0);
}
void optical_start_service(struct storage *s)
{
    bool bound = false;
    for (unsigned i = 0; i < STORAGE_DISKS; i++) if (s->optical_bindings[i].device) bound = true;
    if (!bound) return;
    struct task *t = task_create_kernel("optical-media", optical_task, s, P_DEVICE);
    if (t) task_start(t);
    klog("[optical] bind stage=poller interval_ms=%u result=%d", OPTICAL_POLL_MS, t ? 0 : -FS_ENOMEM);
}
#endif
