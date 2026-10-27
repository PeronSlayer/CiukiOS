/* f5-12B2 CD audio devctls on /dev/media, addressed by optical slot (an
 * audio-only disc has no drive letter). Records are copied in and out; every
 * request is checked against the cached TOC and the drive's media generation
 * before any packet (optical_cd.c). Contract: docs/design/f4-audio-media-contract.md
 * "CD audio transport", research: docs/validation/f5-12b2-cd-audio.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/storage.h>
#include <ciuki/ata.h>
#include <ciuki/mm.h>
#include <ciuki/cpu.h>

static struct ata_device *cd_device(struct storage *s, uint32_t slot)
{ return s && s->ready && !s->stopped && slot < STORAGE_DISKS ? s->optical_bindings[slot].device : 0; }

/* Driver results to errno: no medium wins, then a generation change (the
 * caller re-reads STATE/TOC), then the request's own refusal codes. A drive
 * that is present but not ready (spin-up, unit attention) is EAGAIN as well:
 * the caller retries after the next STATE. */
static int cd_error(const struct ata_device *d, uint32_t generation, int e)
{
    if (!e) return 0;
    if (d->media == ATAPI_NO_MEDIUM) return -ENXIO;
    if (e == -FS_ENOENT || e == -FS_EAGAIN || d->block.media_generation != generation) return -EAGAIN;
    if (e == -FS_EIO && !d->block.quarantined && d->sense_key == 5 && (d->asc == 0x64 || d->asc == 0x21)) return -EINVAL;
    if (e == -FS_EINVAL || e == -FS_EOPNOTSUPP) return e;
    return px_error(e);
}
static int cd_header(uint32_t address, uint32_t bytes, uint32_t expected, void *out)
{
    if (bytes != expected) return -EINVAL;
    int e = copy_from_user(out, address, bytes); if (e) return e;
    const uint32_t *h = out; /* version, size, slot lead every CD record */
    if (h[0] != CIUKI_MEDIA_CD_VERSION) return -EOPNOTSUPP;
    if (h[1] != bytes || h[2] >= STORAGE_DISKS) return -EINVAL;
    return 0;
}

static int cd_state(struct storage *s, uint32_t address, uint32_t bytes)
{
    struct ciuki_media_cd_state r;
    int e = cd_header(address, bytes, sizeof(r), &r); if (e) return e;
    if (!s || !s->ready) return -ENODEV;
    struct ata_device *d = cd_device(s, r.slot);
    uint32_t slot = r.slot; memset(&r, 0, sizeof(r));
    r.version = CIUKI_MEDIA_CD_VERSION; r.size = sizeof(r); r.slot = slot; r.drive = 26;
    if (d) {
        r.present = 1; r.generation = d->block.media_generation;
        r.state = d->media == ATAPI_READY ? CIUKI_CD_READY : d->media == ATAPI_NO_MEDIUM ? CIUKI_CD_NO_MEDIUM : CIUKI_CD_NOT_READY;
        r.audio_ready = d->media == ATAPI_READY && d->toc.valid && d->toc.generation == r.generation && d->toc.first_audio;
        vfs_lock(&s->vfs, 0);
        for (unsigned drive = 3; drive < 26; drive++)
            if (s->vfs.ro_volumes[drive] == &s->optical[slot] && s->optical[slot].mounted &&
                s->optical[slot].mount_generation == r.generation) { r.mounted = 1; r.drive = drive; }
        vfs_unlock(&s->vfs);
    }
    return copy_to_user(address, &r, sizeof(r));
}
static int cd_info(struct storage *s, uint32_t address, uint32_t bytes)
{
    struct ciuki_media_cd_info r;
    int e = cd_header(address, bytes, sizeof(r), &r); if (e) return e;
    struct ata_device *d = cd_device(s, r.slot); if (!d) return -ENODEV;
    if (r.generation && r.generation != d->block.media_generation) return -EAGAIN;
    uint32_t slot = r.slot; memset(&r, 0, sizeof(r));
    r.version = CIUKI_MEDIA_CD_VERSION; r.size = sizeof(r); r.slot = slot;
    r.packet_bytes = d->packet_bytes; r.max_read_sectors = ATAPI_CD_READ_MAX;
    memcpy(r.model, d->model, sizeof(r.model));
    if (d->media == ATAPI_READY) (void)atapi_cd_caps(d); /* page 2Ah needs a ready unit; cached per drive */
    r.generation = d->block.media_generation;
    if (d->cd_caps_valid) {
        r.capabilities = CIUKI_CD_CAP_REPORTED | (d->cd_caps[4] & 1 ? CIUKI_CD_CAP_AUDIO_PLAY : 0) |
            (d->cd_caps[5] & 1 ? CIUKI_CD_CAP_CDDA_READ : 0) | (d->cd_caps[5] & 2 ? CIUKI_CD_CAP_CDDA_ACCURATE : 0) |
            (d->cd_caps[6] & 8 ? CIUKI_CD_CAP_EJECT : 0) | (d->cd_caps[7] & 1 ? CIUKI_CD_CAP_SEPARATE_VOLUME : 0);
        r.volume_levels = (uint32_t)d->cd_caps[10] << 8 | d->cd_caps[11];
    }
    return copy_to_user(address, &r, sizeof(r));
}
static int cd_toc(struct storage *s, uint32_t address, uint32_t bytes)
{
    struct ciuki_media_cd_toc r; /* Per caller: packet I/O can yield. */
    int e = cd_header(address, bytes, sizeof(r), &r); if (e) return e;
    struct ata_device *d = cd_device(s, r.slot); if (!d) return -ENODEV;
    uint32_t slot = r.slot, wanted = r.generation;
    if (d->media == ATAPI_NO_MEDIUM) return -ENXIO;
    if (d->media != ATAPI_READY) return -EAGAIN;
    if (wanted && wanted != d->block.media_generation) return -EAGAIN;
    e = atapi_cd_toc(d, false); if (e) return cd_error(d, d->block.media_generation, e);
    memset(&r, 0, sizeof(r)); r.version = CIUKI_MEDIA_CD_VERSION; r.size = sizeof(r); r.slot = slot;
    r.generation = d->toc.generation; r.first = d->toc.first; r.last = d->toc.last;
    r.count = d->toc.count; r.leadout_lba = d->toc.leadout;
    for (unsigned i = 0; i < d->toc.count && i < CIUKI_CD_TRACKS_MAX; i++) {
        r.tracks[i].number = d->toc.number[i]; r.tracks[i].control = d->toc.control[i];
        r.tracks[i].adr = d->toc.adr[i]; r.tracks[i].start_lba = d->toc.start[i];
    }
    return copy_to_user(address, &r, sizeof(r));
}

/* READ CD: the PIO loop must not fault on user pages while it owns the
 * channel, and no mutex may be held across copy_to_user (a fault can page
 * through any channel, whose mutex address order is not ours). The bounce is
 * therefore a transient physically contiguous run, reached through the
 * direct map, owned by this call alone; ENOMEM tells the reader to ask for
 * fewer sectors. */
static int cd_read_audio(struct process *p, struct storage *s, uint32_t address, uint32_t bytes)
{
    struct ciuki_media_cd_read r;
    int e = cd_header(address, bytes, sizeof(r), &r); if (e) return e;
    struct ata_device *d = cd_device(s, r.slot); if (!d) return -ENODEV;
    if (!r.sectors || r.sectors > ATAPI_CD_READ_MAX || !r.buffer) return -EINVAL;
    uint32_t count = r.sectors * ATAPI_CD_SECTOR;
    /* The user range is checked before the packet, so a bad buffer never
     * costs a drive command; the copy itself runs after the transfer. */
    if (!ua_range(p->memory, r.buffer, count, PROT_READ | PROT_WRITE)) return -EFAULT;
    e = atapi_cd_read_check(d, r.generation, r.lba, r.sectors);
    if (e) return cd_error(d, r.generation, e);
    uint32_t pages = (count + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t phys = pmm_alloc_contig(pages);
    if (!phys) return -ENOMEM;
    void *bounce = P2V(phys);
    e = cd_error(d, r.generation, atapi_cd_read_audio(d, r.generation, r.lba, r.sectors, bounce));
    if (!e) e = copy_to_user(r.buffer, bounce, count);
    for (uint32_t i = 0; i < pages; i++) pmm_free(phys + i * PAGE_SIZE);
    if (e) return e;
    r.done = count;
    return copy_to_user(address, &r, sizeof(r));
}

static int cd_tray(struct storage *s, uint32_t address, uint32_t bytes)
{
    struct ciuki_media_cd_tray r;
    int e = cd_header(address, bytes, sizeof(r), &r); if (e) return e;
    struct ata_device *d = cd_device(s, r.slot); if (!d) return -ENODEV;
    if (r.generation && r.generation != d->block.media_generation) return -EAGAIN;
    if (r.action == CIUKI_CD_TRAY_CLOSE) return cd_error(d, d->block.media_generation, atapi_cd_load(d));
    if (r.action != CIUKI_CD_TRAY_OPEN) return -EINVAL;
    if (d->cd_playing) return -EBUSY;
    /* A mounted data session goes through the F2-74 preflight (open handles
     * refuse with EBUSY); an audio disc has no letter to detach. */
    unsigned drive = 26; uint32_t generation = d->block.media_generation;
    vfs_lock(&s->vfs, 0);
    for (unsigned i = 3; i < 26; i++) if (s->vfs.ro_volumes[i] == &s->optical[r.slot]) drive = i;
    vfs_unlock(&s->vfs);
    if (drive < 26) return px_error(optical_eject(s, drive, generation));
    e = atapi_eject(d);
    s->optical_bindings[r.slot].attempted_generation = 0;
    unsigned letter = s->optical_bindings[r.slot].drive; /* fx-emptydrive: kept while empty */
    klog("[optical] media slot=%u stage=eject drive=%c generation=%u result=%d", r.slot,
         letter >= 3 && letter < 26 ? 'A' + letter : '-', generation, e);
    return e == -FS_EINVAL ? -EINVAL : px_error(e);
}

int media_cd_devctl(struct process *p, struct storage *s, uint32_t op, uint32_t address, uint32_t bytes)
{
    switch (op) {
    case MEDIA_CD_STATE: return cd_state(s, address, bytes);
    case MEDIA_CD_INFO: return cd_info(s, address, bytes);
    case MEDIA_CD_TOC: return cd_toc(s, address, bytes);
    case MEDIA_CD_READ_AUDIO: return cd_read_audio(p, s, address, bytes);
    case MEDIA_CD_TRAY: return cd_tray(s, address, bytes);
    case MEDIA_CD_PLAY: {
        struct ciuki_media_cd_play r;
        int e = cd_header(address, bytes, sizeof(r), &r); if (e) return e;
        struct ata_device *d = cd_device(s, r.slot); if (!d) return -ENODEV;
        return cd_error(d, r.generation, atapi_cd_play(d, r.generation, r.start_lba, r.end_lba));
    }
    case MEDIA_CD_PAUSE: {
        struct ciuki_media_cd_pause r;
        int e = cd_header(address, bytes, sizeof(r), &r); if (e) return e;
        struct ata_device *d = cd_device(s, r.slot); if (!d) return -ENODEV;
        if (r.action > CIUKI_CD_ACTION_STOP || r.reserved) return -EINVAL;
        return cd_error(d, r.generation, r.action == CIUKI_CD_ACTION_STOP ? atapi_cd_stop(d, r.generation) :
                        atapi_cd_pause(d, r.generation, r.action == CIUKI_CD_ACTION_RESUME));
    }
    case MEDIA_CD_POSITION: {
        struct ciuki_media_cd_position r;
        int e = cd_header(address, bytes, sizeof(r), &r); if (e) return e;
        struct ata_device *d = cd_device(s, r.slot); if (!d) return -ENODEV;
        uint8_t status = 0, track = 0, index = 0; uint32_t absolute = 0, relative = 0;
        e = atapi_cd_position(d, r.generation, &status, &track, &index, &absolute, &relative);
        if (e) return cd_error(d, r.generation, e);
        r.audio_status = status; r.track = track; r.index = index; r.absolute_lba = absolute; r.relative_lba = relative;
        memset(r.reserved, 0, sizeof(r.reserved));
        return copy_to_user(address, &r, sizeof(r));
    }
    }
    return -ENOTTY;
}
