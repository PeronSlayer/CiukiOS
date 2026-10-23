/* f5-12B2 CD-audio packet paths on the F4 ATAPI boundary: READ TOC format 0
 * (LBA) cached per media generation, MODE SENSE(10) page 2Ah capability
 * bits, READ CD (BEh) of CD-DA user data for the digital path, PLAY AUDIO
 * MSF / PAUSE-RESUME / READ SUB-CHANNEL for the analog fallback, and the
 * tray load. Every request is validated against the cached TOC before any
 * packet; the kernel never retries a failed data request (F4 rule).
 * Research: MMC-3 r10g 5.10, 5.13, 5.17 (Tables 147/148/153/154), 5.22
 * (Tables 216-220), 5.23 (Tables 230-233), 6.3.11 (Table 361), 4.1.4;
 * SFF-8020i r2.6 10.8.6.4, 10.8.15, 10.8.19, 10.8.25 (Table 155);
 * docs/validation/f5-12b2-cd-audio.md. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/ata.h>

static uint32_t cd_be32(const uint8_t *p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* MMC-3 Table 2 / 4.2: LBA = ((M * 60) + S) * 75 + F - 150 for the program
 * area (00:02:00 is LBA 0); PLAY AUDIO MSF addresses are absolute MSF. */
void atapi_lba_to_msf(uint32_t lba, uint8_t msf[3])
{
    uint32_t frames = lba + 150;
    msf[0] = (uint8_t)(frames / 4500); msf[1] = (uint8_t)((frames / 75) % 60); msf[2] = (uint8_t)(frames % 75);
}
uint32_t atapi_msf_to_lba(const uint8_t msf[3])
{
    uint32_t frames = ((uint32_t)msf[0] * 60 + msf[1]) * 75 + msf[2];
    return frames >= 150 ? frames - 150 : 0;
}

static int cd_admitted(struct ata_device *d, uint32_t generation)
{
    if (!d || !d->identified || !d->atapi) return -FS_EINVAL;
    if (d->block.quarantined) return -FS_EQUARANTINED;
    if (d->media != ATAPI_READY) return -FS_ENOENT; /* media_dev maps: no medium / not ready */
    if (generation != d->block.media_generation) return -FS_EAGAIN;
    return 0;
}

/* READ TOC/PMA/ATIP 43h, TIME=0 (LBA), Format 0000b in byte 2 (MMC-3 Table
 * 230; SFF-8020i keeps byte 9 zero for the same format). The response is
 * the 4-byte header plus one 8-byte descriptor per track and the lead-out
 * (track AAh); 804 bytes hold the 99-track maximum (SFF-8020i 10.8.19). */
int atapi_cd_toc(struct ata_device *d, bool refresh)
{
    if (!d || !d->identified || !d->atapi) return -FS_EINVAL;
    uint32_t gen = d->block.media_generation;
    if (!refresh && d->toc.valid && d->toc.generation == gen) return 0;
    uint8_t p[12] = { 0x43, 0, 0, 0, 0, 0, 0, (uint8_t)(ATAPI_TOC_BYTES >> 8), (uint8_t)ATAPI_TOC_BYTES };
    uint8_t raw[ATAPI_TOC_BYTES]; uint32_t n = 0;
    d->toc.valid = false;
    int e = atapi_packet(d, p, sizeof(p), raw, sizeof(raw), &n);
    if (e) return e;
    if (gen != d->block.media_generation) return -FS_EAGAIN;
    if (n < 12) return -FS_EIO;
    uint32_t length = (uint32_t)raw[0] << 8 | raw[1];
    if (length > n - 2 || length < 10 || (length - 2) % 8) return -FS_EIO;
    unsigned descriptors = (length - 2) / 8;
    uint8_t first = raw[2], last = raw[3];
    if (!first || first > ATAPI_TOC_TRACKS || last < first || last > ATAPI_TOC_TRACKS) return -FS_EIO;
    struct atapi_toc t;
    memset(&t, 0, sizeof(t));
    t.first = first; t.last = last;
    bool leadout = false;
    for (unsigned i = 0; i < descriptors; i++) {
        const uint8_t *desc = raw + 4 + 8 * i;
        uint8_t number = desc[2];
        if (number == 0xaa) {
            if (leadout || i + 1 != descriptors) return -FS_EIO;
            t.leadout = cd_be32(desc + 4); leadout = true; continue;
        }
        if (number != first + t.count || number > last || t.count >= ATAPI_TOC_TRACKS) return -FS_EIO;
        if (t.count && number <= t.number[t.count - 1]) return -FS_EIO; /* not ascending */
        t.number[t.count] = number; t.control[t.count] = desc[1] & 15; t.adr[t.count] = desc[1] >> 4;
        t.start[t.count] = cd_be32(desc + 4); t.count++;
    }
    if (!leadout || t.count != (unsigned)(last - first + 1)) return -FS_EIO;
    for (unsigned i = 0; i < t.count; i++)
        if (t.start[i] >= t.leadout || (i && t.start[i] <= t.start[i - 1])) return -FS_EIO;
    t.audio_only = true;
    for (unsigned i = 0; i < t.count; i++) if (t.control[i] & 4) t.audio_only = false;
    t.first_audio = !(t.control[0] & 4);
    t.generation = gen; t.valid = true;
    d->toc = t;
    return 0;
}

/* MODE SENSE(10) 5Ah, PC=00b current values, page 2Ah (MMC-3 6.3.11 Table
 * 361; SFF-8020i Table 68). The mode parameter header is 8 bytes with the
 * block descriptor length in bytes 6-7; the page follows. Fetched once per
 * drive: the capability bits are static. A drive that rejects the page
 * leaves cd_caps_valid false and the caller treats the bits as unknown. */
int atapi_cd_caps(struct ata_device *d)
{
    if (!d || !d->identified || !d->atapi) return -FS_EINVAL;
    if (d->cd_caps_valid) return 0;
    uint8_t p[12] = { 0x5a, 0, 0x2a, 0, 0, 0, 0, 0, 64 };
    uint8_t raw[64]; uint32_t n = 0;
    int e = atapi_packet(d, p, sizeof(p), raw, sizeof(raw), &n);
    if (e) return e;
    if (n < 8) return -FS_EIO;
    uint32_t block_descriptors = (uint32_t)raw[6] << 8 | raw[7];
    uint32_t at = 8 + block_descriptors;
    if (at + 12 > n || (raw[at] & 0x3f) != 0x2a || raw[at + 1] < 10) return -FS_EIO;
    uint32_t page_bytes = 2u + raw[at + 1];
    if (page_bytes > n - at) page_bytes = n - at;
    memset(d->cd_caps, 0, sizeof(d->cd_caps));
    memcpy(d->cd_caps, raw + at, page_bytes < sizeof(d->cd_caps) ? page_bytes : sizeof(d->cd_caps));
    d->cd_caps_valid = true;
    klog("[optical] cd channel=%u unit=%u stage=caps audio_play=%u cdda_read=%u cdda_accurate=%u eject=%u volume_levels=%u",
         d->channel->index, d->unit, d->cd_caps[4] & 1, d->cd_caps[5] & 1, (d->cd_caps[5] >> 1) & 1,
         (d->cd_caps[6] >> 3) & 1, (unsigned)d->cd_caps[10] << 8 | d->cd_caps[11]);
    return 0;
}

/* [lba, lba + sectors) must lie inside the program area and touch only
 * audio tracks (CONTROL bit 2 clear, MMC-3 Table 222). Refused before any
 * packet: MMC-3 4.1.4 terminates a read at a data/audio transition and a
 * CD-DA read of a data sector is ILLEGAL MODE FOR THIS TRACK (5.17). */
int atapi_cd_range_check(const struct atapi_toc *t, uint32_t lba, uint32_t sectors)
{
    if (!t || !t->valid || !sectors || !t->count) return -FS_EINVAL;
    uint32_t end = lba + sectors;
    if (end < lba || end > t->leadout || lba < t->start[0]) return -FS_EINVAL;
    for (unsigned i = 0; i < t->count; i++) {
        uint32_t track_end = i + 1 < t->count ? t->start[i + 1] : t->leadout;
        if (lba < track_end && end > t->start[i] && (t->control[i] & 4)) return -FS_EINVAL;
    }
    return 0;
}

/* Everything READ CD needs before a packet: ready unit, current media
 * generation, 1..27 sectors, the range inside audio tracks of the cached TOC
 * and a drive that does not report "CD-DA Commands Supported" = 0. media_cd.c
 * calls it before it allocates the transfer buffer. */
int atapi_cd_read_check(struct ata_device *d, uint32_t generation, uint32_t lba, uint32_t sectors)
{
    int e = cd_admitted(d, generation);
    if (e) return e;
    if (!sectors || sectors > ATAPI_CD_READ_MAX) return -FS_EINVAL;
    if ((e = atapi_cd_toc(d, false)) || (e = atapi_cd_range_check(&d->toc, lba, sectors))) return e;
    if (d->cd_caps_valid && !(d->cd_caps[5] & 1)) return -FS_EOPNOTSUPP; /* CD-DA Commands Supported = 0 */
    return 0;
}

/* READ CD BEh (MMC-3 5.17): Expected Sector Type 001b (CD-DA) in byte 1
 * bits 4-2, 24-bit transfer length, byte 9 = 10h (user data only: 2,352
 * bytes per sector, Table 153), no sub-channel. The whole range is one
 * packet of at most 27 sectors (63,504 bytes) under the 64 KiB limit. */
int atapi_cd_read_audio(struct ata_device *d, uint32_t generation, uint32_t lba, uint32_t sectors, void *buffer)
{
    int e = atapi_cd_read_check(d, generation, lba, sectors);
    if (e) return e;
    if (!buffer) return -FS_EINVAL;
    uint8_t p[12] = { 0xbe, 1 << 2, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                      (uint8_t)(sectors >> 16), (uint8_t)(sectors >> 8), (uint8_t)sectors, 0x10, 0, 0 };
    uint32_t done = 0, bytes = sectors * ATAPI_CD_SECTOR;
    e = atapi_packet_generation(d, p, sizeof(p), buffer, bytes, &done, generation);
    if (!e && (done != bytes || generation != d->block.media_generation)) e = -FS_EIO;
    if (!e) { d->cd_audio_sectors += sectors; d->cd_audio_packets++; }
    return e;
}

/* PLAY AUDIO MSF 47h (MMC-3 5.13): absolute start and end MSF; the end is
 * exclusive ("all contiguous audio sectors between" them). The range is
 * validated against the TOC like a read. Requires page 2Ah Audio Play. */
int atapi_cd_play(struct ata_device *d, uint32_t generation, uint32_t start_lba, uint32_t end_lba)
{
    int e = cd_admitted(d, generation);
    if (e) return e;
    if (end_lba <= start_lba || end_lba > 256u * 4500u - 151u) return -FS_EINVAL;
    if ((e = atapi_cd_toc(d, false)) || (e = atapi_cd_range_check(&d->toc, start_lba, end_lba - start_lba))) return e;
    if (d->cd_caps_valid && !(d->cd_caps[4] & 1)) return -FS_EOPNOTSUPP;
    uint8_t p[12] = { 0x47 }; uint32_t n;
    atapi_lba_to_msf(start_lba, p + 3); atapi_lba_to_msf(end_lba, p + 6);
    e = atapi_packet_generation(d, p, sizeof(p), 0, 0, &n, generation);
    if (!e) d->cd_play_commands++;
    return e;
}

/* PAUSE/RESUME 4Bh (MMC-3 5.10): byte 8 bit 0 Resume. */
int atapi_cd_pause(struct ata_device *d, uint32_t generation, bool resume)
{
    int e = cd_admitted(d, generation);
    if (e) return e;
    uint8_t p[12] = { 0x4b, 0, 0, 0, 0, 0, 0, 0, resume ? 1 : 0 }; uint32_t n;
    return atapi_packet_generation(d, p, sizeof(p), 0, 0, &n, generation);
}

/* STOP PLAY/SCAN 4Eh (MMC-3 5.37): stops a play operation, also a paused
 * one; stopping when nothing plays is not an error. */
int atapi_cd_stop(struct ata_device *d, uint32_t generation)
{
    int e = cd_admitted(d, generation);
    if (e) return e;
    uint8_t p[12] = { 0x4e }; uint32_t n;
    e = atapi_packet_generation(d, p, sizeof(p), 0, 0, &n, generation);
    return e;
}

/* READ SUB-CHANNEL 42h (MMC-3 5.22): TIME=0 (LBA), SubQ=1, format 01h CD
 * current position. 4-byte header (audio status in byte 1, Table 219) and
 * the 12-byte position block (Table 220): track, index, absolute and
 * track-relative addresses. */
int atapi_cd_position(struct ata_device *d, uint32_t generation, uint8_t *audio_status,
                      uint8_t *track, uint8_t *index, uint32_t *absolute, uint32_t *relative)
{
    int e = cd_admitted(d, generation);
    if (e) return e;
    uint8_t p[12] = { 0x42, 0, 0x40, 1, 0, 0, 0, 0, 16 }, raw[16]; uint32_t n = 0;
    e = atapi_packet_generation(d, p, sizeof(p), raw, sizeof(raw), &n, generation);
    if (e) return e;
    if (n < 4) return -FS_EIO;
    *audio_status = raw[1]; *track = *index = 0; *absolute = *relative = 0;
    uint32_t length = (uint32_t)raw[2] << 8 | raw[3];
    if ((raw[1] == 0x11 || raw[1] == 0x12) && (length < 12 || n < 16)) return -FS_EIO;
    if (length >= 12 && n >= 16) {
        if (raw[4] != 1) return -FS_EIO;
        *track = raw[6]; *index = raw[7]; *absolute = cd_be32(raw + 8); *relative = cd_be32(raw + 12);
    }
    return 0;
}

/* START STOP UNIT 1Bh LoEj=1 Start=1: load the disc / close the tray
 * (SFF-8020i Table 155), non-immediate under the 10 s start deadline. The
 * existing explicit start packet already has this encoding. */
int atapi_cd_load(struct ata_device *d)
{
    if (!d || !d->identified || !d->atapi) return -FS_EINVAL;
    int e = atapi_start(d);
    klog("[optical] cd channel=%u unit=%u stage=load result=%d", d->channel->index, d->unit, e);
    return e;
}
