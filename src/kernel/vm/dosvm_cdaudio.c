/* fx-10/fx-10b: CD audio for DOS VMs. Each window VM gets an MSCDEX 2.23
 * drive (dosvm_mscdex.c) answered in the monitor, as Windows 98 answers
 * INT 2Fh AH=15h for its V86 VMs: the V86 INT 2Fh trap and the DPMI 0300h
 * translation of INT 2Fh (DJGPP's __dpmi_int, Quake's dos_int86) are both
 * served before the guest's own INT 2Fh chain, whose FreeDOS handler
 * returns BX unchanged for AH=15h (no MSCDEX: Quake disables its music).
 *
 * The disc the drive reports, in this order (fx-10b):
 *  1. a disc with audio tracks in the physical optical drive: its TOC from
 *     the f5-12B2 ATAPI path (atapi_cd_toc) and its audio by digital
 *     extraction, READ CD BEh (atapi_cd_read_audio), into the mixer;
 *  2. the WAV folder: RIFF/WAVE CD-DA files (44.1 kHz, 16-bit stereo PCM) in
 *     C:\GAMES\CDAUDIO\<TITLE>\TRACKnn.WAV or C:\GAMES\CDAUDIO\TRACKnn.WAV,
 *     <TITLE> the launched window's name (Quake -> QUAKE); track 1 is a
 *     data track unless TRACK01.WAV exists (Quake's music is tracks 2-11);
 *  3. a data-only disc in the drive (its TOC; nothing to play);
 *  4. no disc.
 * A change of the drive's medium publishes the new disc to the VM, which
 * reports Media Changed and ends play, as MSCDEX does after a disc change.
 * No tracks ship with CiukiOS: Quake shareware contains no music.
 *
 * Digital, not analog PLAY AUDIO MSF: the AC'97 driver programs only the
 * master (02h) and PCM (18h) registers, so the codec's CD input (12h) keeps
 * its muted reset value 8808h (AC'97 2.3; Realtek ALC203 datasheet 6.1.9),
 * and whether the T23's UltraBay drive has an analog
 * line to its CS4299 codec is undocumented; f4-audio-media-contract.md
 * leaves analog routing to hardware qualification.
 *
 * Two kernel tasks. vm-cdread does every blocking operation: the WAV scan,
 * the drive's TOC, file reads and READ CD packets, into a per-VM reservoir
 * of CD_RING sectors (150,528 bytes, 853 ms at 1x), at most one CD_CHUNK
 * packet (8 sectors, 18,816 bytes) per playing VM per CD_READ_MS (20 ms) and
 * only while the reservoir has room: steady state 75 sectors/s, at most 400.
 * vm-cdaudio never blocks on I/O: it applies the VM's requests, selects the
 * source, and every CD_FEED_MS (10 ms) copies the reservoir into the VM's
 * native mixer stream (44.1 kHz; the f4 mixer resamples to 48 kHz) with the
 * gain of the MSCDEX channel volume and the VM's CT1745 CD and master
 * levels. The VM worker never performs I/O or logging: requests and records
 * are posted to the tasks. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/ata.h>
#include <ciuki/audio.h>
#include "dosvm_internal.h"
#include "dosvm_mscdex.h"
#include "dosvm_cdaudio.h"

#define CD_ROOT "C:\\GAMES\\CDAUDIO"
#define CD_CHUNK 8u                 /* sectors per read: 18,816 bytes, 107 ms */
#define CD_RING 64u                 /* reservoir: 150,528 bytes, 853 ms at 1x */
#define CD_FEED_MS 10u              /* the stream ring holds 46 ms */
#define CD_READ_MS 20u
#define CD_POLL_MS 500u             /* drive state: field reads only, no packet */
#define CD_WAIT_MS 2000u
#define CD_EVENTS 8u
#define CD_HEADER 4096u
_Static_assert(CD_RING % CD_CHUNK == 0, "chunks never wrap the reservoir");

enum { CD_SRC_NONE, CD_SRC_WAV, CD_SRC_DRIVE };
#define CD_EV_LOST 100u             /* vm-cdaudio's own record: the medium went mid-play */
static const char *const source_name[] = {"none", "wav", "cd"};

struct cd_file { uint32_t data_offset, data_bytes; };
struct cd_slot {
    /* Published under irq_save. */
    struct dosvm_key key;
    bool used, closing, scanned, released;
    uint32_t want_seq, want_disc;
    bool want_play;
    uint32_t want_lba, want_sectors;
    uint8_t want_gain[2];
    struct { uint32_t a, b, c; uint8_t kind; } ev[CD_EVENTS];
    uint8_t ev_head, ev_count;
    uint32_t ev_dropped;
    struct mscdex_disc next_disc;   /* vm-cdaudio -> VM worker */
    uint32_t disc_seq;
    /* Read job and reservoir (head/count/job fields under irq_save). */
    uint8_t *res;
    uint32_t res_phys, res_pages, cap, head, count;
    uint32_t job_epoch, job_lba, job_remaining, job_generation;
    uint8_t job_source;
    bool job_active, job_lost, reader_busy;
    struct mscdex_disc job_disc;    /* the played disc, constant while a job runs */
    /* VM worker. */
    struct dosvm *vm;
    struct mscdex m;
    uint32_t applied_seq;
    bool lettered;
    uint8_t volume[2];
    char title[9];
    /* vm-cdread. */
    char dir[40];
    const char *result;
    unsigned wav_tracks;
    struct mscdex_disc wav_disc;
    struct cd_file file[MSCDEX_TRACKS + 1];
    int fd;
    unsigned fd_track;
    bool read_logged;
    uint32_t read_errors;
    /* vm-cdaudio. */
    uint32_t have_seq, offset, streamed, stream_errors, underruns, source_seq;
    uint8_t source;
    bool playing, starving;
    uint32_t refused_logged;
    struct audio_stream *stream;
};
static struct cd_slot slots[DOSVM_LIMIT];
static struct {
    struct task *task, *reader;
    struct kwait wake, read_wake, done;
    bool creating, table_ready;
    struct vfs_table table;         /* 20 KB, vm-cdread only, while a scan or a file needs it */
    struct fat_entry entry;
    uint8_t header[CD_HEADER];
} svc;
/* The physical drive as vm-cdread last observed it (published under irq_save). */
static struct {
    struct ata_device *device;
    unsigned slot;
    enum atapi_media media;
    uint32_t generation, seq, audio;
    bool present, cdda;
    struct mscdex_disc disc;
} drive;

static void wake_all(void) { kwait_wake_all(&svc.wake); kwait_wake_all(&svc.read_wake); }

/* ---- VM worker side ---------------------------------------------------- */
static struct cd_slot *slot_of(const struct dosvm *v)
{
    unsigned id = v->info.key.id - 1u;
    if (id >= DOSVM_LIMIT) return 0;
    struct cd_slot *s = &slots[id];
    uint32_t f = irq_save();
    bool live = s->used && !s->closing && s->disc_seq && s->vm == v &&
                s->key.generation == v->info.key.generation;
    irq_restore(f);
    return live ? s : 0;
}
static void post_event(struct cd_slot *s, unsigned kind, uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t f = irq_save();
    if (s->ev_count == CD_EVENTS) s->ev_dropped++;
    else {
        unsigned i = (s->ev_head + s->ev_count++) % CD_EVENTS;
        s->ev[i].kind = (uint8_t)kind; s->ev[i].a = a; s->ev[i].b = b; s->ev[i].c = c;
    }
    irq_restore(f);
    kwait_wake_all(&svc.wake);
}
static int op_read(void *ctx, uint32_t at, void *buffer, unsigned bytes)
{ return dosvm_read(((struct cd_slot *)ctx)->vm, at, buffer, bytes); }
static int op_write(void *ctx, uint32_t at, const void *buffer, unsigned bytes)
{ return dosvm_write(((struct cd_slot *)ctx)->vm, at, buffer, bytes); }
static uint32_t op_now(void *ctx) { (void)ctx; return (uint32_t)g_ticks; }
static void refresh_gain(struct cd_slot *s)
{
    const uint8_t *mixer = s->vm->audio.sb.mixer;
    uint8_t left = (uint8_t)mscdex_gain(s->volume[0], mixer[0x36] >> 3, mixer[0x30] >> 3);
    uint8_t right = (uint8_t)mscdex_gain(s->volume[1], mixer[0x37] >> 3, mixer[0x31] >> 3);
    uint32_t f = irq_save();
    s->want_gain[0] = left; s->want_gain[1] = right;
    irq_restore(f);
}
static void op_audio(void *ctx, bool play, uint32_t lba, uint32_t sectors)
{
    struct cd_slot *s = ctx;
    refresh_gain(s);
    uint32_t f = irq_save();
    s->want_play = play; s->want_lba = lba; s->want_sectors = sectors; s->want_seq++;
    s->want_disc = s->applied_seq;  /* a request for a disc already replaced is void */
    irq_restore(f);
    kwait_wake_all(&svc.wake);
}
static void op_volume(void *ctx, uint8_t left, uint8_t right)
{
    struct cd_slot *s = ctx;
    s->volume[0] = left; s->volume[1] = right;
    refresh_gain(s);
}
static void op_event(void *ctx, unsigned kind, uint32_t a, uint32_t b, uint32_t c)
{ post_event(ctx, kind, a, b, c); }
static const struct mscdex_ops cd_ops = {0, op_read, op_write, op_now, op_audio, op_volume, op_event};

/* MSCDEX takes the first letter after the VM's DOS drives (DFV2 IFS drive
 * set: C:, D:, E: on the unified image), at least D:. */
static uint8_t drive_letter(const struct dosvm *v)
{
    struct dosvm_unified_info info;
    uint32_t owned = dosvm_file_unified_info && dosvm_file_unified_info(v, &info) ? info.owned : 0;
    unsigned letter = 3;
    for (unsigned i = 0; i < 25; i++) if (owned >> i & 1u) letter = i + 1u;
    return (uint8_t)(letter < 3 ? 3 : letter);
}
/* A disc published by vm-cdaudio replaces the model's; the first one is the
 * disc present at boot, later ones are media changes. */
static void apply_disc(struct cd_slot *s)
{
    uint32_t f = irq_save();
    if (s->disc_seq == s->applied_seq) { irq_restore(f); return; }
    bool first = !s->applied_seq;
    memcpy(&s->m.disc, &s->next_disc, sizeof(s->m.disc));
    s->applied_seq = s->disc_seq;
    irq_restore(f);
    mscdex_media_changed(&s->m, !first);
}
static struct mscdex_regs regs_of(const struct v86_frame *f)
{
    return (struct mscdex_regs){(uint16_t)f->tf.eax, (uint16_t)f->tf.ebx, (uint16_t)f->tf.ecx,
        (uint16_t)f->tf.edx, (uint16_t)f->tf.esi, (uint16_t)f->tf.edi, (uint16_t)f->es,
        (uint16_t)f->tf.eflags};
}
int dosvm_cdaudio_interrupt(struct dosvm *v, struct v86_frame *f)
{
    if ((f->tf.eax & 0xFF00u) != 0x1500u) return -ENOSYS;
    struct cd_slot *s = slot_of(v);
    if (!s) return -ENOSYS;
    if (!s->lettered) { s->m.drive = drive_letter(v); s->lettered = true; }
    apply_disc(s);
    struct mscdex_regs r = regs_of(f);
    int e = mscdex_interrupt(&s->m, &r);
    if (e) return e;
    vm_low(&f->tf.eax, r.ax); vm_low(&f->tf.ebx, r.bx); vm_low(&f->tf.ecx, r.cx);
    f->tf.eflags = (f->tf.eflags & ~V86_CF) | (r.flags & V86_CF);
    if (s->m.playing) refresh_gain(s); /* the guest may have moved its CD/master levels */
    return 0;
}
/* DPMI 0300h for INT 2Fh: answer on the translated real-mode frame and
 * resume at the host's real-mode return, as if the IVT handler had IRETed
 * (the same frame dosvm_file_reflect builds). */
int dosvm_cdaudio_reflect(struct dosvm *v, struct v86_frame *f, uint16_t image)
{
    if ((f->tf.eax & 0xFF00u) != 0x1500u || !slot_of(v)) return -ENOSYS;
    struct v86_frame returned = *f;
    returned.tf.cs = 0xF000; returned.tf.eip = DPMI_RM_RETURN;
    vm_low(&returned.tf.user_esp, (uint16_t)((uint16_t)returned.tf.user_esp + 6));
    returned.tf.eflags = (returned.tf.eflags & ~0x0CD5u) | (image & 0x0CD5u) | V86_VM | V86_IF | 2u;
    int e = dosvm_cdaudio_interrupt(v, &returned);
    if (e) return e;
    v->monitor.vif = (image & V86_IF) != 0;
    *f = returned;
    return 0;
}

/* ---- vm-cdread: scan, drive state, extraction --------------------------- */
static bool storage_live(void)
{
    struct storage *st = storage_get();
    return st && st->ready && !st->stopped;
}
static bool table_open(void)
{
    if (svc.table_ready) return true;
    if (!storage_live() || vfs_table_init(&storage_get()->vfs, &svc.table, 2)) return false;
    svc.table_ready = true;
    return true;
}
static void track_close(struct cd_slot *s)
{
    if (s->fd >= 0) (void)vfs_close(&svc.table, s->fd);
    s->fd = -1; s->fd_track = 0;
}
/* The system has a few VFS tables (VFS_TABLES): return it when idle. */
static void table_close_idle(bool force)
{
    if (!svc.table_ready) return;
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) {
        if (!slots[i].used) continue;   /* fd is meaningful only in a used slot */
        if (force) track_close(&slots[i]);
        else if (slots[i].fd >= 0) return;
    }
    vfs_table_destroy(&svc.table); svc.table_ready = false;
}
static int track_open(struct cd_slot *s, unsigned track)
{
    if (s->fd >= 0 && s->fd_track == track) return 0;
    track_close(s);
    if (!table_open()) return -EIO;
    char path[64];
    ksnprintf(path, sizeof(path), "%s\\TRACK%02u.WAV", s->dir, track);
    int fd = vfs_open(&svc.table, path, VFS_READ, VFS_DENY_WRITE, 0);
    if (fd < 0) return fd;
    s->fd = fd; s->fd_track = track;
    return 0;
}
/* Header and size of one track file; 0 when usable. */
static int probe(const char *path, struct cd_file *out, const char **reason)
{
    *reason = "absent";
    if (vfs_stat(&svc.table, path, &svc.entry) || (svc.entry.attr & 0x10)) return -ENOENT;
    uint32_t size = svc.entry.size;
    int fd = vfs_open(&svc.table, path, VFS_READ, VFS_DENY_WRITE, 0);
    *reason = "open";
    if (fd < 0) return fd;
    size_t got = 0;
    int e = vfs_read(&svc.table, fd, svc.header, sizeof(svc.header), &got);
    (void)vfs_close(&svc.table, fd);
    *reason = "read";
    if (e) return e;
    struct mscdex_wav w;
    e = mscdex_wav_parse(svc.header, (unsigned)got, size, &w, reason);
    if (e) return e;
    *out = (struct cd_file){w.data_offset, w.data_bytes};
    return 0;
}
static bool is_dir(const char *path)
{ return !vfs_stat(&svc.table, path, &svc.entry) && (svc.entry.attr & 0x10); }
static void scan(struct cd_slot *s)
{
    struct mscdex_disc *d = &s->wav_disc;
    memset(d, 0, sizeof(*d)); memset(s->file, 0, sizeof(s->file));
    s->wav_tracks = 0; s->result = "no_tracks";
    ksnprintf(s->dir, sizeof(s->dir), "%s", CD_ROOT);
    if (!table_open()) { s->result = "storage"; goto done; }
    if (s->title[0]) {
        char dir[40];
        ksnprintf(dir, sizeof(dir), "%s\\%s", CD_ROOT, s->title);
        if (is_dir(dir)) ksnprintf(s->dir, sizeof(s->dir), "%s", dir);
    }
    for (unsigned n = 1; n <= MSCDEX_TRACKS; n++) {
        char path[64];
        const char *reason;
        struct cd_file file;
        ksnprintf(path, sizeof(path), "%s\\TRACK%02u.WAV", s->dir, n);
        int e = probe(path, &file, &reason);
        if (e && n == 1) {
            if (e != -ENOENT) klog("[dosvm] cdaudio track file=%s result=refused reason=%s", path, reason);
            continue;                       /* track 1 stays the data track */
        }
        if (e) {
            /* Tracks are contiguous: the first gap ends the disc. */
            if (e != -ENOENT) klog("[dosvm] cdaudio track file=%s result=refused reason=%s", path, reason);
            break;
        }
        if (n == 2 && !d->present) (void)mscdex_disc_add(d, MSCDEX_DATA_SECTORS, true);
        uint32_t sectors = (file.data_bytes + MSCDEX_SECTOR_BYTES - 1) / MSCDEX_SECTOR_BYTES;
        if (mscdex_disc_add(d, sectors, false)) {
            klog("[dosvm] cdaudio track file=%s result=refused reason=length sectors=%u", path, sectors);
            break;
        }
        s->file[n] = file; s->wav_tracks++;
    }
    if (s->wav_tracks) s->result = "disc";
    else memset(d, 0, sizeof(*d));          /* a data placeholder alone is no audio disc */
done:
    klog("[dosvm] cdaudio disc vm=%u generation=%u source=%s tracks=%u audio=%u leadout=%u result=%s",
         s->key.id, s->key.generation, s->dir, d->present ? d->last : 0u, s->wav_tracks,
         d->leadout, s->result);
}
/* READ TOC format 0 as the MSCDEX disc: CONTROL in the high nibble, ADR low;
 * a track runs to the next track's start (pregaps included) or the lead-out. */
static unsigned toc_disc(const struct atapi_toc *t, struct mscdex_disc *d)
{
    memset(d, 0, sizeof(*d));
    if (!t->valid || !t->count) return 0;
    unsigned audio = 0;
    d->present = true; d->first = t->first; d->last = t->last; d->leadout = t->leadout;
    for (unsigned i = 0; i < t->count; i++) {
        uint32_t end = i + 1u < t->count ? t->start[i + 1] : t->leadout;
        d->track[t->number[i]] = (struct mscdex_track){t->start[i], end - t->start[i],
            (uint8_t)((t->control[i] & 15u) << 4 | (t->adr[i] & 15u))};
        if (!(t->control[i] & 4)) audio++;
    }
    return audio;
}
/* Field reads every pass; READ TOC (and page 2Ah once per drive) only after
 * the medium or its generation changed. */
static void drive_poll(void)
{
    struct storage *st = storage_get();
    struct ata_device *d = 0;
    unsigned slot = 0;
    if (storage_live())
        for (unsigned i = 0; i < STORAGE_DISKS && !d; i++) if (st->optical_bindings[i].device) { d = st->optical_bindings[i].device; slot = i; }
    enum atapi_media media = d ? d->media : ATAPI_NO_MEDIUM;
    uint32_t generation = d ? d->block.media_generation : 0;
    if (d == drive.device && media == drive.media && generation == drive.generation && drive.seq) return;
    static struct mscdex_disc disc;  /* vm-cdread only */
    memset(&disc, 0, sizeof(disc));
    const char *result = !d ? "no_drive" : media == ATAPI_NO_MEDIUM ? "no_medium" : "not_ready";
    unsigned audio = 0;
    bool cdda = false;
    int e = 0;
    if (d && media == ATAPI_READY) {
        e = atapi_cd_toc(d, false);
        if (!e && d->block.media_generation != generation) e = -FS_EAGAIN;
        if (e) result = "toc_error";
        else {
            audio = toc_disc(&d->toc, &disc);
            (void)atapi_cd_caps(d);
            /* A drive that rejects page 2Ah is tried; one that reports no
             * CD-DA commands is not (READ CD would be refused). */
            cdda = !d->cd_caps_valid || (d->cd_caps[5] & 1);
            result = audio ? "audio" : "data_only";
        }
    }
    uint32_t f = irq_save();
    drive.device = d; drive.slot = slot; drive.media = media; drive.generation = generation;
    drive.present = disc.present; drive.audio = audio; drive.cdda = cdda;
    memcpy(&drive.disc, &disc, sizeof(disc));
    drive.seq++;
    irq_restore(f);
    klog("[dosvm] cdaudio drive slot=%u generation=%u result=%s tracks=%u audio=%u first=%u last=%u leadout=%u cdda=%u error=%d",
         slot, generation, result, disc.present ? (unsigned)(disc.last - disc.first + 1) : 0u, audio,
         disc.first, disc.last, disc.leadout, cdda, e);
    kwait_wake_all(&svc.wake);
}
/* One chunk of one VM's job; true while that job still wants data. */
static bool read_chunk(struct cd_slot *s)
{
    uint32_t f = irq_save();
    if (!s->used || s->closing || !s->job_active || !s->job_remaining || !s->res) {
        bool idle = s->used && !s->job_active;
        irq_restore(f);
        if (idle && s->fd >= 0) track_close(s);
        return false;
    }
    if (s->cap - s->count < CD_CHUNK) { irq_restore(f); return true; }
    uint32_t at = (s->head + s->count) % s->cap, epoch = s->job_epoch, lba = s->job_lba;
    uint32_t generation = s->job_generation;
    uint32_t n = s->job_remaining < CD_CHUNK ? s->job_remaining : CD_CHUNK;
    uint8_t source = s->job_source;
    if (n > s->cap - at) n = s->cap - at;
    s->reader_busy = true;
    irq_restore(f);

    const struct mscdex_disc *d = source == CD_SRC_WAV ? &s->wav_disc : &s->job_disc;
    unsigned t = mscdex_track_at(d, lba);
    uint32_t in = 0;
    if (t) {
        in = lba - d->track[t].lba;
        if (n > d->track[t].sectors - in) n = d->track[t].sectors - in;
    }
    uint32_t bytes = n * MSCDEX_SECTOR_BYTES;
    uint8_t *buffer = s->res + at * MSCDEX_SECTOR_BYTES;
    bool audio = t && !(d->track[t].control & 0x40);
    bool lost = false;
    const char *reason = 0;
    int e = 0;
    size_t got = 0;
    memset(buffer, 0, bytes);           /* data tracks, pregaps and a rip's padding are silence */
    if (audio && source == CD_SRC_WAV) {
        const struct cd_file *file = &s->file[t];
        uint32_t offset = in * MSCDEX_SECTOR_BYTES;
        if (file->data_bytes > offset) {
            uint32_t want = file->data_bytes - offset < bytes ? file->data_bytes - offset : bytes;
            e = track_open(s, t);
            if (!e) e = vfs_seek(&svc.table, s->fd, (int64_t)file->data_offset + offset, VFS_SEEK_SET, 0);
            if (!e) e = vfs_read(&svc.table, s->fd, buffer, want, &got);
            if (!e && got != want) e = -EIO;
            if (e) { track_close(s); reason = "file"; }
        }
    } else if (audio && source == CD_SRC_DRIVE) {
        struct ata_device *dev = drive.device;
        e = dev ? atapi_cd_read_audio(dev, generation, lba, n, buffer) : -FS_ENOENT;
        if (e) {
            memset(buffer, 0, bytes);   /* a failed PIO transfer may have left a partial sector */
            if (!dev || e == -FS_ENOENT || e == -FS_EAGAIN || dev->media != ATAPI_READY ||
                dev->block.media_generation != generation) { lost = true; reason = "no_medium"; }
            else if (e == -FS_EOPNOTSUPP) { lost = true; reason = "no_cdda"; }
            else reason = "read";
        }
    }
    f = irq_save();
    s->reader_busy = false;
    bool current = s->job_active && s->job_epoch == epoch;
    if (current && lost) { s->job_remaining = 0; s->job_lost = true; }
    else if (current) { s->count += n; s->job_lba += n; s->job_remaining -= n; }
    irq_restore(f);
    if (current && e) {
        s->read_errors++;
        if (lost || !s->read_logged) {
            s->read_logged = true;
            klog("[dosvm] cdaudio read vm=%u source=%s track=%u lba=%u sectors=%u result=%d reason=%s fallback=%s",
                 s->key.id, source_name[source], t, lba, n, e, reason, lost ? "stop" : "silence");
        }
    }
    if (current) kwait_wake_all(&svc.wake);
    return current && !lost;
}
static bool scan_pending(const struct cd_slot *s) { return s->used && !s->closing && !s->scanned; }
static bool read_pending(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) {
        const struct cd_slot *s = &slots[i];
        if (scan_pending(s) || (s->used && s->closing && !s->released) ||
            (s->job_active && s->job_remaining && s->cap - s->count >= CD_CHUNK)) return true;
    }
    return false;
}
bool dosvm_cdaudio_read_step(void)
{
    if (!storage_live()) table_close_idle(true);   /* no file stays open on a stopping volume */
    drive_poll();
    bool active = false;
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) {
        struct cd_slot *s = &slots[i];
        uint32_t f = irq_save();
        bool closing = s->used && s->closing && !s->released, scanning = scan_pending(s);
        irq_restore(f);
        if (closing) {
            track_close(s);
            f = irq_save(); s->released = true; irq_restore(f);
            kwait_wake_all(&svc.wake);
            continue;
        }
        if (scanning) {
            scan(s);
            f = irq_save(); s->scanned = true; irq_restore(f);
            kwait_wake_all(&svc.wake);
        }
        active |= read_chunk(s);
    }
    table_close_idle(false);
    return active;
}

/* ---- vm-cdaudio: requests, source, stream ------------------------------ */
static void job_stop(struct cd_slot *s)
{
    uint32_t f = irq_save();
    s->job_active = false; s->job_epoch++; s->job_remaining = 0; s->head = s->count = 0;
    irq_restore(f);
    s->offset = 0;
}
static void stream_end(struct cd_slot *s)
{
    if (s->stream) audio_stream_close(s->stream);
    s->stream = 0; s->playing = false; s->starving = false;
    job_stop(s);
}
static bool reservoir(struct cd_slot *s)
{
    if (s->res) return true;
    for (uint32_t sectors = CD_RING; sectors >= CD_CHUNK; sectors /= 2) {
        uint32_t pages = (sectors * MSCDEX_SECTOR_BYTES + PAGE_SIZE - 1) / PAGE_SIZE;
        uint32_t phys = pmm_alloc_contig(pages);
        if (!phys) continue;
        s->res = P2V(phys); s->res_phys = phys; s->res_pages = pages; s->cap = sectors;
        return true;
    }
    return false;
}
static void reservoir_free(struct cd_slot *s)
{
    for (uint32_t i = 0; s->res && i < s->res_pages; i++) pmm_free(s->res_phys + i * PAGE_SIZE);
    s->res = 0; s->res_phys = s->res_pages = s->cap = 0;
}
static void stream_begin(struct cd_slot *s, uint32_t lba, uint32_t sectors)
{
    const char *stage = "reservoir";
    int e = reservoir(s) ? 0 : -ENOMEM;
    if (!e) { stage = "open"; e = audio_stream_open(&s->stream); }
    if (!e) {
        struct ciuki_audio_format format = {.version = 1, .size = sizeof(format), .rate = 44100,
                                            .format = CIUKI_AUDIO_S16LE, .channels = 2};
        stage = "format";
        e = audio_stream_format(s->stream, &format);
    }
    if (e) {
        s->stream_errors++;
        klog("[dosvm] cdaudio stream vm=%u generation=%u stage=%s result=%d",
             s->key.id, s->key.generation, stage, e);
        stream_end(s); return;
    }
    audio_stream_producer(s->stream, g_current, SCHED_PRODUCER_WRITER);
    if (svc.reader && svc.reader != (struct task *)1)
        audio_stream_producer(s->stream, svc.reader, SCHED_PRODUCER_READER);
    uint32_t f = irq_save();
    if (s->source == CD_SRC_DRIVE) s->job_generation = drive.generation;
    s->job_epoch++; s->job_active = true; s->job_lost = false; s->job_source = s->source;
    s->job_lba = lba; s->job_remaining = sectors; s->head = s->count = 0;
    irq_restore(f);
    s->offset = 0; s->playing = true; s->starving = false;
    kwait_wake_all(&svc.read_wake);
}
/* Reservoir to mixer stream; never blocks. */
static void feed(struct cd_slot *s)
{
    for (unsigned turn = 0; turn < 8; turn++) {
        uint32_t f = irq_save();
        uint32_t count = s->count, head = s->head, remaining = s->job_remaining;
        bool lost = s->job_lost;
        irq_restore(f);
        if (!count) {
            if (!remaining) {
                /* Every sector queued (or the medium went): play out the tail. */
                s->stream->drain_goal = s->stream->accepted; s->stream->draining = true;
                s->playing = false; job_stop(s);
                if (lost) post_event(s, CD_EV_LOST, 0, 0, 0);
                return;
            }
            /* The reservoir ran dry after the first sector: a late read. */
            if (!s->starving && s->stream->accepted && s->stream->queued < AUDIO_QUANTUM) { s->starving = true; s->underruns++; }
            return;
        }
        s->starving = false;
        uint32_t run = s->cap - head < count ? s->cap - head : count;
        uint32_t bytes = run * MSCDEX_SECTOR_BYTES - s->offset;
        int wrote = audio_stream_enqueue(s->stream, s->res + head * MSCDEX_SECTOR_BYTES + s->offset, bytes);
        if (wrote == -EAGAIN) return;
        if (wrote < 0) {
            s->stream_errors++;
            klog("[dosvm] cdaudio stream vm=%u generation=%u stage=enqueue result=%d",
                 s->key.id, s->key.generation, wrote);
            stream_end(s); return;
        }
        s->offset += (uint32_t)wrote;
        uint32_t whole = s->offset / MSCDEX_SECTOR_BYTES;
        s->offset %= MSCDEX_SECTOR_BYTES;
        if (whole) {
            f = irq_save(); s->head = (s->head + whole) % s->cap; s->count -= whole; irq_restore(f);
            s->streamed += whole;
            kwait_wake_all(&svc.read_wake);
        }
        if ((uint32_t)wrote < bytes) return;
    }
}
/* Source order: an audio disc in the drive, the WAV folder, a data-only disc. */
static void select_source(struct cd_slot *s)
{
    uint32_t f = irq_save();
    bool present = drive.present;
    uint32_t audio = drive.audio, seq = drive.seq;
    irq_restore(f);
    uint8_t want = present && audio ? CD_SRC_DRIVE : s->wav_tracks ? CD_SRC_WAV : present ? CD_SRC_DRIVE : CD_SRC_NONE;
    bool same = s->disc_seq && want == s->source && (want != CD_SRC_DRIVE || seq == s->source_seq);
    s->source_seq = seq;
    if (same) return;
    stream_end(s);
    const char *reason = want == CD_SRC_WAV ? (present ? "drive_data_only" : "wav") :
                         want == CD_SRC_DRIVE ? (audio ? "audio_disc" : "data_only") : "no_tracks";
    f = irq_save();
    if (want == CD_SRC_DRIVE) { memcpy(&s->job_disc, &drive.disc, sizeof(s->job_disc)); seq = drive.seq; }
    const struct mscdex_disc *d = want == CD_SRC_DRIVE ? &s->job_disc : &s->wav_disc;
    if (want == CD_SRC_NONE) memset(&s->next_disc, 0, sizeof(s->next_disc));
    else memcpy(&s->next_disc, d, sizeof(s->next_disc));
    s->disc_seq++;
    uint32_t generation = drive.generation, tracks = d->present && want ? (uint32_t)(d->last - d->first + 1) : 0;
    uint32_t leadout = want ? d->leadout : 0;
    irq_restore(f);
    s->source = want;
    klog("[dosvm] cdaudio source vm=%u source=%s generation=%u tracks=%u audio=%u leadout=%u reason=%s",
         s->key.id, source_name[want], want == CD_SRC_DRIVE ? generation : 0u, tracks,
         want == CD_SRC_DRIVE ? audio : want == CD_SRC_WAV ? s->wav_tracks : 0u, leadout, reason);
    kwait_wake_all(&svc.done);
}
static void drain_events(struct cd_slot *s)
{
    for (;;) {
        uint32_t f = irq_save();
        if (!s->ev_count) { irq_restore(f); return; }
        unsigned kind = s->ev[s->ev_head].kind;
        uint32_t a = s->ev[s->ev_head].a, b = s->ev[s->ev_head].b, c = s->ev[s->ev_head].c;
        s->ev_head = (uint8_t)((s->ev_head + 1u) % CD_EVENTS); s->ev_count--;
        uint8_t gl = s->want_gain[0], gr = s->want_gain[1];
        irq_restore(f);
        unsigned id = s->key.id;
        const char *source = source_name[s->source];
        switch (kind) {
        case MSCDEX_EV_INSTALL:
            klog("[dosvm] cdaudio mscdex vm=%u drive=%c version=2.23 disc=%u source=%s dir=%s result=%s",
                 id, 'A' + (int)a, b, source, s->dir, s->result);
            break;
        case MSCDEX_EV_PLAY:
            klog("[dosvm] cdaudio play vm=%u source=%s track=%u lba=%u sectors=%u gain=%u/%u",
                 id, source, a, b, c, gl, gr);
            break;
        case MSCDEX_EV_STOP:
            klog("[dosvm] cdaudio stop vm=%u position=%u paused=%u streamed=%u", id, a, b, s->streamed);
            break;
        case MSCDEX_EV_RESUME:
            klog("[dosvm] cdaudio resume vm=%u lba=%u sectors=%u", id, a, b);
            break;
        case MSCDEX_EV_COMPLETE:
            klog("[dosvm] cdaudio complete vm=%u end=%u streamed=%u read_errors=%u underruns=%u",
                 id, a, s->streamed, s->read_errors, s->underruns);
            break;
        case MSCDEX_EV_REFUSED:
            /* Status polls repeat; the first refusal per error code is the evidence. */
            if (c < 32 && !(s->refused_logged & 1u << c)) {
                s->refused_logged |= 1u << c;
                klog("[dosvm] cdaudio refused vm=%u command=%u code=%u error=%u reason=%s source=%s dir=%s",
                     id, a, b, c, s->source == CD_SRC_NONE ? "no_tracks" : "request", source, s->dir);
            }
            break;
        case CD_EV_LOST:
            klog("[dosvm] cdaudio play vm=%u source=%s result=lost streamed=%u", id, source, s->streamed);
            break;
        }
    }
}
/* True while the slot needs the feed interval. */
static bool service_slot(struct cd_slot *s)
{
    uint32_t f = irq_save();
    bool used = s->used, closing = s->closing, scanned = s->scanned, released = s->released;
    bool play = s->want_play;
    uint32_t seq = s->want_seq, lba = s->want_lba, sectors = s->want_sectors, disc = s->want_disc;
    uint8_t gain[2] = {s->want_gain[0], s->want_gain[1]};
    irq_restore(f);
    if (!used) return false;
    drain_events(s);
    if (closing) {
        stream_end(s);
        f = irq_save();
        bool busy = s->reader_busy;
        irq_restore(f);
        if (!released || busy) return true;     /* vm-cdread still owns its file or a packet */
        reservoir_free(s);
        f = irq_save(); s->used = false; s->closing = false; irq_restore(f);
        kwait_wake_all(&svc.done);
        return false;
    }
    if (!scanned) return false;
    select_source(s);
    if (seq != s->have_seq) {
        s->have_seq = seq;
        stream_end(s);
        if (play && disc == s->disc_seq && s->source != CD_SRC_NONE) stream_begin(s, lba, sectors);
    }
    if (s->stream) { s->stream->gain[0] = gain[0]; s->stream->gain[1] = gain[1]; }
    if (s->playing) { feed(s); return true; }
    if (s->stream && s->stream->queued && !audio.error) return true;
    if (s->stream) stream_end(s);
    return false;
}
static bool pending(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) {
        const struct cd_slot *s = &slots[i];
        if (s->used && (s->closing || s->ev_count || s->want_seq != s->have_seq ||
                        (s->scanned && !s->disc_seq))) return true;
        if (s->used && s->disc_seq && drive.seq != s->source_seq) return true;
    }
    return false;
}
bool dosvm_cdaudio_step(void)
{
    bool active = false;
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) active |= service_slot(&slots[i]);
    return active;
}
static void service_main(void *arg)
{
    (void)arg;
    for (;;) {
        bool active = dosvm_cdaudio_step();
        kwait_wait_until(&svc.wake, pending, 0, deadline_after_ms(active ? CD_FEED_MS : 1000u));
    }
}
static void reader_main(void *arg)
{
    (void)arg;
    for (;;) {
        bool active = dosvm_cdaudio_read_step();
        kwait_wait_until(&svc.read_wake, read_pending, 0, deadline_after_ms(active ? CD_READ_MS : CD_POLL_MS));
    }
}
static int service_start(void)
{
    uint32_t f = irq_save();
    if (svc.task || svc.creating) {
        irq_restore(f);
        while (!svc.task) { if (!svc.creating) return -ENOMEM; task_sleep_ms(1); }
        return 0;
    }
    svc.creating = true;
    irq_restore(f);
    kwait_init(&svc.wake); kwait_init(&svc.read_wake); kwait_init(&svc.done);
    struct task *reader = task_create_kernel("vm-cdread", reader_main, 0, P_NORMAL);
    struct task *t = reader ? task_create_kernel("vm-cdaudio", service_main, 0, P_NORMAL) : 0;
    if (t) { svc.reader = reader; task_start(reader); task_start(t); }
    f = irq_save(); svc.task = t; svc.creating = false; irq_restore(f);
    return t ? 0 : -ENOMEM;
}
#ifdef CIUKI_CDAUDIO_HOST
/* Host harness: the test drives both steps itself. */
void dosvm_cdaudio_test_mode(void)
{
    kwait_init(&svc.wake); kwait_init(&svc.read_wake); kwait_init(&svc.done);
    svc.task = svc.reader = (struct task *)1;
}
#endif

static bool slot_free(void *arg) { return !((struct cd_slot *)arg)->used; }
static bool slot_ready(void *arg) { const struct cd_slot *s = arg; return s->disc_seq || !s->used; }
static void title_of(const char *name, char out[9])
{
    unsigned n = 0;
    for (unsigned i = 0; name[i] && i < 32 && n < 8; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out[n++] = c;
        else if (c == ' ' || c == '(') break;  /* "Quake (shareware)" -> QUAKE */
    }
    out[n] = 0;
}
int dosvm_cdaudio_open(struct dosvm *v)
{
    unsigned id = v->info.key.id - 1u;
    if (id >= DOSVM_LIMIT) return -EINVAL;
    struct cd_slot *s = &slots[id];
    int e = service_start();
    if (e) { klog("[dosvm] cdaudio vm=%u result=refused reason=task error=%d", v->info.key.id, e); return e; }
    if (s->used) kwait_wait_until(&svc.done, slot_free, s, deadline_after_ms(CD_WAIT_MS));
    if (s->used) {
        klog("[dosvm] cdaudio vm=%u result=refused reason=previous-close", v->info.key.id);
        return -EBUSY;
    }
    memset(s, 0, sizeof(*s));
    s->key = v->info.key; s->vm = v; s->fd = -1;
    s->volume[0] = s->volume[1] = 0xFF;
    title_of(v->name, s->title);
    mscdex_init(&s->m, &cd_ops, 3);
    s->m.ops.ctx = s;
    refresh_gain(s);
    uint32_t f = irq_save(); s->used = true; irq_restore(f);
    wake_all();
    /* The scan (vm-cdread) and the source choice (vm-cdaudio) precede boot. */
    kwait_wait_until(&svc.done, slot_ready, s, deadline_after_ms(CD_WAIT_MS));
    if (!s->disc_seq)
        klog("[dosvm] cdaudio vm=%u result=late reason=scan wait_ms=%u", v->info.key.id, CD_WAIT_MS);
    return 0;
}
void dosvm_cdaudio_close(struct dosvm *v)
{
    unsigned id = v->info.key.id - 1u;
    if (id >= DOSVM_LIMIT) return;
    struct cd_slot *s = &slots[id];
    uint32_t f = irq_save();
    bool mine = s->used && s->vm == v && s->key.generation == v->info.key.generation;
    if (mine) s->closing = true;
    irq_restore(f);
    if (!mine) return;
    const struct mscdex *m = &s->m;
    klog("[dosvm] cdaudio close vm=%u gen=%u req=%u play=%u stop=%u resume=%u done=%u refused=%u sectors=%u read_err=%u underruns=%u dropped=%u",
         s->key.id, s->key.generation, m->requests, m->plays, m->stops, m->resumes, m->completions,
         m->refused, s->streamed, s->read_errors, s->underruns, s->ev_dropped);
    wake_all();
    kwait_wait_until(&svc.done, slot_free, s, deadline_after_ms(CD_WAIT_MS));
    if (s->used) klog("[dosvm] cdaudio close vm=%u result=late reason=stream", v->info.key.id);
}
