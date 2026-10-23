/* fx-10: CD audio for DOS VMs. Each window VM gets an MSCDEX 2.23 drive
 * (dosvm_mscdex.c) answered in the monitor, as Windows 98 answers INT 2Fh
 * AH=15h for its V86 VMs: the V86 INT 2Fh trap and the DPMI 0300h
 * translation of INT 2Fh (DJGPP's __dpmi_int, Quake's dos_int86) are both
 * served before the guest's own INT 2Fh chain, whose FreeDOS handler
 * returns BX unchanged for AH=15h (no MSCDEX: Quake disables its music).
 *
 * The disc is virtual: Red Book tracks supplied as RIFF/WAVE CD-DA files
 * (44.1 kHz, 16-bit stereo PCM) in C:\GAMES\CDAUDIO\<TITLE>\TRACKnn.WAV, or
 * C:\GAMES\CDAUDIO\TRACKnn.WAV, where <TITLE> is the launched program's
 * window name (Quake -> QUAKE). Track 1 is a data track unless TRACK01.WAV
 * exists, as on mixed-mode game CDs (Quake's music is tracks 2-11). No such
 * files ship with CiukiOS: Quake shareware's archive contains no music, and
 * its CD tracks are commercial content. Without tracks the drive reports no
 * disc, and the VM's records say so.
 *
 * One kernel task streams every playing VM's tracks into its own native
 * mixer stream (44.1 kHz stereo; the f4 mixer resamples to 48 kHz), with
 * the gain of the MSCDEX channel volume and the VM's CT1745 CD and master
 * levels. The VM worker never performs file I/O or emits a log line: its
 * requests and records are posted to the task.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/audio.h>
#include "dosvm_internal.h"
#include "dosvm_mscdex.h"
#include "dosvm_cdaudio.h"

#define CD_ROOT "C:\\GAMES\\CDAUDIO"
#define CD_CHUNK 8u                 /* sectors per read: 18816 bytes, 107 ms */
#define CD_POLL_MS 10u              /* feed interval; the stream ring holds 46 ms */
#define CD_WAIT_MS 2000u
#define CD_EVENTS 8u
#define CD_HEADER 4096u

struct cd_file { uint32_t data_offset, data_bytes; };
struct cd_slot {
    /* Published under irq_save: lifetime and the VM's latest request. */
    struct dosvm_key key;
    bool used, closing, scanned;
    uint32_t want_seq;
    bool want_play;
    uint32_t want_lba, want_sectors;
    uint8_t want_gain[2];
    struct { uint32_t a, b, c; uint8_t kind; } ev[CD_EVENTS];
    uint8_t ev_head, ev_count;
    uint32_t ev_dropped;
    /* VM worker: the model; the disc inside it is written by the scan only. */
    struct dosvm *vm;
    struct mscdex m;
    bool lettered;
    uint8_t volume[2];
    char title[9];
    /* Task: disc source and stream. */
    char dir[40];
    const char *result;
    unsigned audio_tracks;
    struct cd_file file[MSCDEX_TRACKS + 1];
    uint32_t have_seq;
    struct audio_stream *stream;
    int fd;
    unsigned fd_track;
    bool playing, read_logged;
    uint32_t refused_logged;      /* error codes already recorded */
    uint32_t lba, remaining, buffer_pos, buffer_len;
    uint32_t streamed, read_errors, stream_errors;
    uint8_t buffer[CD_CHUNK * MSCDEX_SECTOR_BYTES];
};
static struct cd_slot slots[DOSVM_LIMIT];
static struct {
    struct task *task;
    struct kwait wake, done;
    bool creating, table_ready;
    struct vfs_table table;      /* 20 KB, task only; held while a scan or a track file needs it */
    struct fat_entry entry;
    uint8_t header[CD_HEADER];
} svc;

/* ---- VM worker side ---------------------------------------------------- */
static struct cd_slot *slot_of(const struct dosvm *v)
{
    unsigned id = v->info.key.id - 1u;
    if (id >= DOSVM_LIMIT) return 0;
    struct cd_slot *s = &slots[id];
    uint32_t f = irq_save();
    bool live = s->used && !s->closing && s->scanned && s->vm == v &&
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

/* ---- Streaming task ---------------------------------------------------- */
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
/* The system has a few VFS tables (VFS_TABLES): return it when idle. */
static void table_close_idle(void)
{
    if (!svc.table_ready) return;
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) if (slots[i].fd >= 0 && slots[i].used) return;
    vfs_table_destroy(&svc.table); svc.table_ready = false;
}
static void track_close(struct cd_slot *s)
{
    if (s->fd >= 0) (void)vfs_close(&svc.table, s->fd);
    s->fd = -1; s->fd_track = 0;
}
static void stream_end(struct cd_slot *s)
{
    if (s->stream) audio_stream_close(s->stream);
    s->stream = 0; s->playing = false; s->remaining = 0;
    s->buffer_pos = s->buffer_len = 0;
    track_close(s);
}
static void stream_begin(struct cd_slot *s, uint32_t lba, uint32_t sectors)
{
    const char *stage = "open";
    int e = audio_stream_open(&s->stream);
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
    s->playing = true; s->lba = lba; s->remaining = sectors;
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
/* Next chunk of CD-DA sectors, never across a track: file data, zeros for
 * the data track, pregaps and a rip's padding of its last sector. */
static void fill(struct cd_slot *s)
{
    const struct mscdex_disc *d = &s->m.disc;
    uint32_t n = s->remaining < CD_CHUNK ? s->remaining : CD_CHUNK;
    unsigned t = mscdex_track_at(d, s->lba);
    uint32_t in = 0;
    if (t) {
        in = s->lba - d->track[t].lba;
        if (n > d->track[t].sectors - in) n = d->track[t].sectors - in;
    }
    uint32_t bytes = n * MSCDEX_SECTOR_BYTES;
    memset(s->buffer, 0, bytes);
    const struct cd_file *file = t ? &s->file[t] : 0;
    uint32_t offset = in * MSCDEX_SECTOR_BYTES;
    if (file && file->data_bytes > offset) {
        uint32_t want = file->data_bytes - offset < bytes ? file->data_bytes - offset : bytes;
        size_t got = 0;
        int e = track_open(s, t);
        if (!e) e = vfs_seek(&svc.table, s->fd, (int64_t)file->data_offset + offset, VFS_SEEK_SET, 0);
        if (!e) e = vfs_read(&svc.table, s->fd, s->buffer, want, &got);
        if (e || got != want) {
            s->read_errors++;
            if (!s->read_logged) {
                s->read_logged = true;
                klog("[dosvm] cdaudio read vm=%u track=%u sector=%u result=%d bytes=%u/%u fallback=silence",
                     s->key.id, t, in, e, (unsigned)got, want);
            }
            track_close(s);
        }
    }
    s->lba += n; s->remaining -= n; s->streamed += n;
    s->buffer_pos = 0; s->buffer_len = bytes;
}
static void feed(struct cd_slot *s)
{
    for (unsigned turn = 0; turn < 32; turn++) {
        if (s->buffer_pos == s->buffer_len) {
            if (!s->remaining) {
                /* Last sector queued: let the mixer play out the tail. */
                s->stream->drain_goal = s->stream->accepted; s->stream->draining = true;
                s->playing = false; track_close(s); return;
            }
            fill(s);
        }
        int wrote = audio_stream_enqueue(s->stream, s->buffer + s->buffer_pos, s->buffer_len - s->buffer_pos);
        if (wrote == -EAGAIN) return;
        if (wrote < 0) {
            s->stream_errors++;
            klog("[dosvm] cdaudio stream vm=%u generation=%u stage=enqueue result=%d",
                 s->key.id, s->key.generation, wrote);
            stream_end(s); return;
        }
        s->buffer_pos += (uint32_t)wrote;
    }
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
    struct mscdex_disc *d = &s->m.disc;
    memset(d, 0, sizeof(*d)); memset(s->file, 0, sizeof(s->file));
    s->audio_tracks = 0; s->result = "no_tracks";
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
        s->file[n] = file; s->audio_tracks++;
    }
    if (s->audio_tracks) s->result = "disc";
    else memset(d, 0, sizeof(*d));          /* a data placeholder alone is no audio disc */
done:
    klog("[dosvm] cdaudio disc vm=%u generation=%u source=%s tracks=%u audio=%u leadout=%u result=%s",
         s->key.id, s->key.generation, s->dir, d->present ? d->last : 0u, s->audio_tracks,
         d->leadout, s->result);
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
        switch (kind) {
        case MSCDEX_EV_INSTALL:
            klog("[dosvm] cdaudio mscdex vm=%u drive=%c version=2.23 disc=%u source=%s result=%s",
                 id, 'A' + (int)a, b, s->dir, s->result);
            break;
        case MSCDEX_EV_PLAY:
            klog("[dosvm] cdaudio play vm=%u track=%u lba=%u sectors=%u gain=%u/%u", id, a, b, c, gl, gr);
            break;
        case MSCDEX_EV_STOP:
            klog("[dosvm] cdaudio stop vm=%u position=%u paused=%u streamed=%u", id, a, b, s->streamed);
            break;
        case MSCDEX_EV_RESUME:
            klog("[dosvm] cdaudio resume vm=%u lba=%u sectors=%u", id, a, b);
            break;
        case MSCDEX_EV_COMPLETE:
            klog("[dosvm] cdaudio complete vm=%u end=%u streamed=%u read_errors=%u", id, a, s->streamed, s->read_errors);
            break;
        case MSCDEX_EV_REFUSED:
            /* Status polls repeat; the first refusal per error code is the evidence. */
            if (c < 32 && !(s->refused_logged & 1u << c)) {
                s->refused_logged |= 1u << c;
                klog("[dosvm] cdaudio refused vm=%u command=%u code=%u error=%u reason=%s source=%s",
                     id, a, b, c, s->audio_tracks ? "request" : "no_tracks", s->dir);
            }
            break;
        }
    }
}
/* True while the slot needs the feed interval. */
static bool service_slot(struct cd_slot *s)
{
    uint32_t f = irq_save();
    bool used = s->used, closing = s->closing, scanned = s->scanned, play = s->want_play;
    uint32_t seq = s->want_seq, lba = s->want_lba, sectors = s->want_sectors;
    uint8_t gain[2] = {s->want_gain[0], s->want_gain[1]};
    irq_restore(f);
    if (!used) return false;
    drain_events(s);
    if (closing) {
        stream_end(s);
        f = irq_save(); s->used = false; s->closing = false; irq_restore(f);
        kwait_wake_all(&svc.done);
        return false;
    }
    if (!scanned) {
        scan(s);
        f = irq_save(); s->scanned = true; irq_restore(f);
        kwait_wake_all(&svc.done);
    }
    if (seq != s->have_seq) {
        s->have_seq = seq;
        stream_end(s);
        if (play) stream_begin(s, lba, sectors);
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
        if (s->used && (!s->scanned || s->closing || s->ev_count || s->want_seq != s->have_seq)) return true;
    }
    return false;
}
bool dosvm_cdaudio_step(void)
{
    bool active = false;
    /* Shutdown: no file stays open on a stopping volume. */
    if (svc.table_ready && !storage_live())
        for (unsigned i = 0; i < DOSVM_LIMIT; i++) if (slots[i].used) stream_end(&slots[i]);
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) active |= service_slot(&slots[i]);
    table_close_idle();
    return active;
}
static void service_main(void *arg)
{
    (void)arg;
    for (;;) {
        bool active = dosvm_cdaudio_step();
        kwait_wait_until(&svc.wake, pending, 0, deadline_after_ms(active ? CD_POLL_MS : 1000u));
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
    kwait_init(&svc.wake); kwait_init(&svc.done);
    struct task *t = task_create_kernel("vm-cdaudio", service_main, 0, P_NORMAL);
    if (t) task_start(t);
    f = irq_save(); svc.task = t; svc.creating = false; irq_restore(f);
    return t ? 0 : -ENOMEM;
}
#ifdef CIUKI_CDAUDIO_HOST
/* Host harness: the test drives dosvm_cdaudio_step itself. */
void dosvm_cdaudio_test_mode(void) { kwait_init(&svc.wake); kwait_init(&svc.done); svc.task = (struct task *)1; }
#endif

static bool slot_free(void *arg) { return !((struct cd_slot *)arg)->used; }
static bool slot_scanned(void *arg) { const struct cd_slot *s = arg; return s->scanned || !s->used; }
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
    kwait_wake_all(&svc.wake);
    kwait_wait_until(&svc.done, slot_scanned, s, deadline_after_ms(CD_WAIT_MS));
    if (!s->scanned)
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
    klog("[dosvm] cdaudio close vm=%u gen=%u req=%u play=%u stop=%u resume=%u done=%u refused=%u sectors=%u read_err=%u stream_err=%u dropped=%u",
         s->key.id, s->key.generation, m->requests, m->plays, m->stops, m->resumes, m->completions,
         m->refused, s->streamed, s->read_errors, s->stream_errors, s->ev_dropped);
    kwait_wake_all(&svc.wake);
    kwait_wait_until(&svc.done, slot_free, s, deadline_after_ms(CD_WAIT_MS));
    if (s->used) klog("[dosvm] cdaudio close vm=%u result=late reason=stream", v->info.key.id);
}
