/* fx-10/fx-10b: Quake's CD audio through the production MSCDEX model, its
 * VM glue (V86 INT 2Fh and DPMI 0300h entries, vm-cdread and vm-cdaudio
 * steps, records), the production f5-12B2 ATAPI CD path (optical_cd.c, here
 * over a fake packet boundary that answers READ TOC, MODE SENSE page 2Ah and
 * READ CD like a drive) and the production native stream and mixer. The
 * guest's requests are laid out as id Software's WinQuake cd_audio.c (GPL
 * release) builds them; the VFS is a fake holding synthetic CD-DA WAV tracks.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <string.h>
#include <ciuki/kernel.h>
#define CIUKI_CPU_H
static uint8_t host_pages[160][4096];
static unsigned host_page_count;
#define P2V(p) ((void *)host_pages[(p) - 1])
#define V2P(p) ((uint32_t)0)
static inline uint32_t irq_save(void) { return 0; }
static inline void irq_restore(uint32_t f) { (void)f; }
#include "../../../src/kernel/audio/stream.c"
#include "../../../src/kernel/audio/mixer.c"
#define CIUKI_CDAUDIO_HOST
#include "../../../src/kernel/vm/dosvm_cdaudio.c"
/* After abi.h: its own W* status macros differ from the C library's. */
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#include <stdlib.h>

#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %s:%u: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static unsigned checks;

/* ---- kernel seams ---- */
volatile uint64_t g_ticks;
struct task *g_current;
struct ciuki_boot_info g_boot;
static char records[64][256];
static unsigned record_count;
void klog(const char *fmt, ...)
{
    char line[256]; va_list a; va_start(a, fmt); vsnprintf(line, sizeof(line), fmt, a); va_end(a);
    puts(line);
    if (record_count < 64) memcpy(records[record_count++], line, sizeof(line));
}
static const char *find_record(const char *needle)
{
    for (unsigned i = 0; i < record_count; i++) if (strstr(records[i], needle)) return records[i];
    return 0;
}
uint32_t pmm_alloc(void) { return host_page_count < 160 ? ++host_page_count : 0; }
static unsigned contig_runs;
uint32_t pmm_alloc_contig(uint32_t n)
{ if (host_page_count + n > 160) return 0; contig_runs++; host_page_count += n; return host_page_count - n + 1; }
void pmm_free(uint32_t p) { (void)p; }
void *kzalloc(size_t n) { return calloc(1, n); }
void kfree(void *p) { free(p); }
gen_t gen_alloc(void) { static gen_t next; return ++next; }
void kwait_init(struct kwait *q) { memset(q, 0, sizeof(*q)); }
void kwait_wake_all(struct kwait *q) { (void)q; }
void kwait_wake_io(struct kwait *q) { (void)q; }
void audio_wake(void) {}
void audio_fail(int e) { audio.error = e; }
uint64_t deadline_after_ms(uint32_t ms) { return g_ticks + ms; }
/* One pass of each task, reader first. */
static void pump(unsigned n) { while (n--) { (void)dosvm_cdaudio_read_step(); (void)dosvm_cdaudio_step(); } }
/* The tasks run where a caller would sleep for them. */
bool kwait_wait_until(struct kwait *q, kwait_cond_fn cond, void *arg, uint64_t deadline)
{
    (void)q; (void)deadline;
    for (unsigned i = 0; i < 8 && !cond(arg); i++) pump(1);
    return cond(arg);
}
struct task *task_create_kernel(const char *n, void (*fn)(void *), void *arg, enum task_prio p)
{ (void)n; (void)fn; (void)arg; (void)p; return 0; }
void task_start(struct task *t) { (void)t; }
void task_sleep_ms(uint32_t ms) { (void)ms; }
static uint32_t ifs_owned = 0x1c;              /* C:, D:, E: as on the T23 trial */
bool dosvm_file_unified_info(const struct dosvm *v, struct dosvm_unified_info *out)
{ (void)v; memset(out, 0, sizeof(*out)); out->owned = ifs_owned; return true; }

/* Guest conventional memory of the two VMs. */
static struct dosvm vm[2];
static uint8_t low[2][0x100000];
int dosvm_read(struct dosvm *v, uint32_t at, void *out, unsigned n)
{ if (at + n > sizeof(low[0])) return -EFAULT; memcpy(out, low[v->info.key.id - 1] + at, n); return 0; }
int dosvm_write(struct dosvm *v, uint32_t at, const void *in, unsigned n)
{ if (at + n > sizeof(low[0])) return -EFAULT; memcpy(low[v->info.key.id - 1] + at, in, n); return 0; }

/* Fake system volume. */
struct fake_file { const char *path; uint8_t *data; uint32_t size; bool dir; };
static struct fake_file files[16];
static unsigned file_count;
static struct storage fake_storage;
static struct { int file; uint64_t at; bool open; } handles[8];
static unsigned vfs_reads;
struct storage *storage_get(void) { return &fake_storage; }
static int tables;
int vfs_table_init(struct vfs *v, struct vfs_table *t, unsigned d) { (void)v; (void)t; (void)d; CHECK(!tables); tables++; return 0; }
void vfs_table_destroy(struct vfs_table *t) { (void)t; CHECK(tables == 1); tables--; }
static int lookup(const char *path)
{
    for (unsigned i = 0; i < file_count; i++) if (!strcmp(files[i].path, path)) return (int)i;
    return -1;
}
int vfs_stat(struct vfs_table *t, const char *path, struct fat_entry *e)
{
    (void)t; int i = lookup(path); if (i < 0) return -2;
    e->size = files[i].size; e->attr = files[i].dir ? 0x10 : 0x20; return 0;
}
int vfs_open(struct vfs_table *t, const char *path, unsigned flags, enum vfs_share share, uint8_t attr)
{
    (void)t; (void)attr; CHECK(flags == VFS_READ && share == VFS_DENY_WRITE);
    int i = lookup(path); if (i < 0 || files[i].dir) return -2;
    for (int h = 0; h < 8; h++) if (!handles[h].open) { handles[h].file = i; handles[h].at = 0; handles[h].open = true; return h; }
    return -24;
}
int vfs_close(struct vfs_table *t, int h) { (void)t; CHECK(handles[h].open); handles[h].open = false; return 0; }
int vfs_seek(struct vfs_table *t, int h, int64_t off, enum vfs_whence w, uint64_t *pos)
{ (void)t; CHECK(handles[h].open && w == VFS_SEEK_SET && !pos); handles[h].at = (uint64_t)off; return 0; }
int vfs_read(struct vfs_table *t, int h, void *buf, size_t n, size_t *done)
{
    (void)t; CHECK(handles[h].open);
    const struct fake_file *f = &files[handles[h].file];
    size_t left = handles[h].at < f->size ? f->size - handles[h].at : 0;
    if (n > left) n = left;
    memcpy(buf, f->data + handles[h].at, n); handles[h].at += n; *done = n; vfs_reads++;
    return 0;
}

/* Fake ATAPI drive under the production optical_cd.c: optical slot 2 on
 * channel 1, as on the T23 ("[optical] bind slot=2 channel=1 unit=0"). */
static struct ata_channel cd_channel = {.index = 1};
static struct ata_device cd = {.channel = &cd_channel, .atapi = true, .identified = true, .media = ATAPI_NO_MEDIUM};
static struct { unsigned count; uint8_t control[4]; uint32_t start[4], leadout; } disc;
static unsigned toc_reads, read_packets, read_sectors;
static int16_t cd_sample(uint32_t lba, unsigned frame, unsigned channel)
{ return (int16_t)((channel ? -1 : 1) * (int)((lba * 588u + frame) * 3u % 30000u)); }
static void insert(unsigned count, const uint8_t *control, const uint32_t *start, uint32_t leadout)
{
    disc.count = count; disc.leadout = leadout;
    for (unsigned i = 0; i < count; i++) { disc.control[i] = control[i]; disc.start[i] = start[i]; }
    cd.media = ATAPI_READY; cd.block.media_generation++;
}
static void remove_disc(void) { cd.media = ATAPI_NO_MEDIUM; cd.block.media_generation++; }
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
int atapi_packet(struct ata_device *d, const uint8_t *p, unsigned length, void *buffer, uint32_t capacity, uint32_t *done)
{
    CHECK(d == &cd && length == 12);
    uint8_t *raw = buffer;
    if (cd.media != ATAPI_READY) return -FS_ENOENT;
    if (p[0] == 0x43) {                                 /* READ TOC format 0, LBA */
        CHECK(!p[1] && !p[2] && capacity >= 4 + 8 * (disc.count + 1));
        unsigned bytes = 4 + 8 * (disc.count + 1);
        memset(raw, 0, bytes); raw[0] = (uint8_t)((bytes - 2) >> 8); raw[1] = (uint8_t)(bytes - 2);
        raw[2] = 1; raw[3] = (uint8_t)disc.count;
        for (unsigned i = 0; i <= disc.count; i++) {
            uint8_t *t = raw + 4 + 8 * i;
            t[1] = (uint8_t)(1u << 4 | (i < disc.count ? disc.control[i] : 0));
            t[2] = i < disc.count ? (uint8_t)(i + 1) : 0xAA;
            be32(t + 4, i < disc.count ? disc.start[i] : disc.leadout);
        }
        toc_reads++; *done = bytes; return 0;
    }
    if (p[0] == 0x5A && p[2] == 0x2A) {                  /* MODE SENSE(10) page 2Ah */
        memset(raw, 0, 28); raw[8] = 0x2A; raw[9] = 18; raw[12] = 1; raw[13] = 3; raw[14] = 8;
        *done = 28; return 0;
    }
    return -FS_EINVAL;
}
int atapi_packet_generation(struct ata_device *d, const uint8_t *p, unsigned length, void *buffer,
                            uint32_t capacity, uint32_t *done, uint32_t generation)
{
    CHECK(d == &cd && length == 12);
    if (cd.media != ATAPI_READY) return -FS_ENOENT;
    if (generation != cd.block.media_generation) return -FS_EAGAIN;
    CHECK(p[0] == 0xBE && p[1] == 4 && p[9] == 0x10);   /* READ CD, CD-DA, user data only */
    uint32_t lba = (uint32_t)p[2] << 24 | (uint32_t)p[3] << 16 | (uint32_t)p[4] << 8 | p[5];
    uint32_t sectors = (uint32_t)p[6] << 16 | (uint32_t)p[7] << 8 | p[8];
    CHECK(sectors && sectors <= CD_CHUNK && capacity == sectors * 2352u);  /* bounded packet */
    int16_t *pcm = buffer;
    for (uint32_t k = 0; k < sectors; k++)
        for (unsigned i = 0; i < 588; i++) {
            pcm[(k * 588 + i) * 2] = cd_sample(lba + k, i, 0); pcm[(k * 588 + i) * 2 + 1] = cd_sample(lba + k, i, 1);
        }
    read_packets++; read_sectors += sectors; *done = capacity; return 0;
}
int atapi_start(struct ata_device *d) { (void)d; return 0; }

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }
/* CD-DA sample k of a track: left ramps up, right down, track-tagged. */
static int16_t sample(unsigned track, unsigned frame, unsigned channel)
{ return (int16_t)((channel ? -1 : 1) * (int)((frame * 7u + track * 1000u) % 20000u)); }
static uint8_t *wav(unsigned track, uint32_t frames, unsigned channels, uint32_t rate, unsigned bits, uint32_t *size)
{
    uint32_t data = frames * channels * (bits / 8u);
    uint8_t *w = calloc(1, 44 + data + 8);
    memcpy(w, "RIFF", 4); put32(w + 4, 36 + data); memcpy(w + 8, "WAVE", 4);
    memcpy(w + 12, "fmt ", 4); put32(w + 16, 16); put16(w + 20, 1); put16(w + 22, (uint16_t)channels);
    put32(w + 24, rate); put32(w + 28, rate * channels * bits / 8u); put16(w + 32, (uint16_t)(channels * bits / 8u));
    put16(w + 34, (uint16_t)bits); memcpy(w + 36, "data", 4); put32(w + 40, data);
    for (uint32_t i = 0; i < frames && bits == 16 && channels == 2; i++) {
        put16(w + 44 + i * 4, (uint16_t)sample(track, i, 0)); put16(w + 46 + i * 4, (uint16_t)sample(track, i, 1));
    }
    *size = 44 + data; return w;
}
static void add_file(const char *path, uint8_t *data, uint32_t size) { files[file_count++] = (struct fake_file){path, data, size, false}; }
static void add_dir(const char *path) { files[file_count++] = (struct fake_file){path, 0, 0, true}; }

/* ---- Quake's requests (WinQuake cd_audio.c), sent as DPMI 0300h ---- */
#define REQ 0x9000u          /* struct cd_request in DOS memory (dos_getmemory) */
#define INFO (REQ + 26u)     /* union readInfo_u right after it */
static struct v86_frame frame;
static void int2f(struct dosvm *v, uint16_t ax, uint16_t bx, uint16_t cx, uint16_t es)
{
    memset(&frame, 0, sizeof(frame));
    frame.tf.eax = 0xABCD0000u | ax; frame.tf.ebx = bx; frame.tf.ecx = cx; frame.es = es;
    frame.tf.cs = 0x1234; frame.tf.eip = 0x10; frame.tf.user_ss = 0x0700; frame.tf.user_esp = 0x0FFA;
    frame.tf.eflags = 0x20202u | V86_CF;
    v->monitor.vif = false;
    CHECK(!dosvm_cdaudio_reflect(v, &frame, 0x0202));
    /* Resumes at the host's real-mode return with the IRET frame popped. */
    CHECK(frame.tf.cs == 0xF000 && frame.tf.eip == DPMI_RM_RETURN && frame.tf.user_esp == 0x1000);
    CHECK(v->monitor.vif && (frame.tf.eflags & V86_VM));
}
static uint8_t *mem(struct dosvm *v, uint32_t at) { return low[v->info.key.id - 1] + at; }
static uint16_t request(struct dosvm *v, unsigned drive, unsigned command, const uint8_t *body, unsigned length)
{
    uint8_t *r = mem(v, REQ);
    memset(r, 0, 26); r[0] = 13; r[2] = (uint8_t)command;
    if (body) memcpy(r + 13, body, length);
    int2f(v, 0x1510, REQ & 15, (uint16_t)drive, REQ >> 4);
    CHECK(!(frame.tf.eflags & V86_CF));
    return get16(r + 3);
}
static uint16_t ioctl(struct dosvm *v, unsigned drive, unsigned command, const uint8_t *control, unsigned length)
{
    memcpy(mem(v, INFO), control, length);
    uint8_t body[13] = {0};
    put16(body + 1, INFO & 15); put16(body + 3, INFO >> 4); put16(body + 5, (uint16_t)length);
    return request(v, drive, command, body, sizeof(body));
}
static uint16_t set_volume(struct dosvm *v, unsigned drive, uint8_t volume)
{
    uint8_t c[9] = {3, 0, 0, 1, 0, 2, 0, 3, 0};
    CHECK(ioctl(v, drive, 12, c, 9) & MSCDEX_ST_DONE);
    c[2] = c[4] = volume;
    return ioctl(v, drive, 12, c, 9);
}
static uint16_t play(struct dosvm *v, unsigned drive, uint8_t mode, uint32_t start, uint32_t sectors)
{
    uint8_t body[9]; body[0] = mode; put32(body + 1, start); put32(body + 5, sectors);
    return request(v, drive, 132, body, 9);
}
static int RedBookToSector(uint32_t rb) { return (int)((rb >> 16 & 0xff) * 4500u + (rb >> 8 & 0xff) * 75u + (rb & 0xff)); }

static void test_model(void)
{
    CHECK(mscdex_redbook(0) == 0x000200u);
    CHECK(mscdex_hsg(0x0024180Cu) == 163662u);             /* MSCDEX21.DOC example 36:24.12 */
    CHECK(mscdex_redbook(163662u) == 0x0024180Cu);
    CHECK(mscdex_hsg(0x000149u) == UINT32_MAX && mscdex_hsg(0x00003C00u) == UINT32_MAX);
    CHECK(mscdex_gain(255, 31, 31) == 100 && mscdex_gain(0, 31, 31) == 0);
    CHECK(mscdex_gain(255, 26, 31) == 32 && mscdex_gain(128, 31, 31) == 50);
    struct mscdex_wav w; const char *reason; uint32_t size;
    uint8_t *f = wav(2, 10, 1, 44100, 16, &size);
    CHECK(mscdex_wav_parse(f, size, size, &w, &reason) && !strcmp(reason, "channels")); free(f);
    f = wav(2, 10, 2, 22050, 16, &size);
    CHECK(mscdex_wav_parse(f, size, size, &w, &reason) && !strcmp(reason, "rate")); free(f);
    f = wav(2, 10, 2, 44100, 8, &size);
    CHECK(mscdex_wav_parse(f, size, size, &w, &reason) && !strcmp(reason, "bits")); free(f);
    f = wav(2, 10, 2, 44100, 16, &size); put16(f + 20, 3);
    CHECK(mscdex_wav_parse(f, size, size, &w, &reason) && !strcmp(reason, "format")); free(f);
    f = wav(2, 10, 2, 44100, 16, &size);
    CHECK(!mscdex_wav_parse(f, size, size, &w, &reason) && w.data_offset == 44 && w.data_bytes == 40);
    CHECK(!mscdex_wav_parse(f, size, size - 6, &w, &reason) && w.data_bytes == 32); /* truncated rip */
    memcpy(f + 8, "AVI ", 4);
    CHECK(mscdex_wav_parse(f, size, size, &w, &reason) && !strcmp(reason, "header")); free(f);
    struct mscdex_disc d; memset(&d, 0, sizeof(d));
    CHECK(!mscdex_disc_add(&d, MSCDEX_DATA_SECTORS, true) && !mscdex_disc_add(&d, 10, false));
    CHECK(d.track[2].lba == MSCDEX_DATA_SECTORS + MSCDEX_PREGAP && d.leadout == d.track[2].lba + 10);
    CHECK(mscdex_track_at(&d, 0) == 1 && mscdex_track_at(&d, 300) == 0 && mscdex_track_at(&d, 450) == 2);
}

static uint8_t c10[11] = {10}, status6[5] = {6}, audio15[11] = {15}, changed9[2] = {9};
/* CDAudio_Init's 1500h/150Ch, then GetAudioDiskInfo: returns the drive. */
static unsigned quake_init(struct dosvm *v)
{
    int2f(v, 0x1500, 0, 0, 0);
    CHECK((uint16_t)frame.tf.ebx == 1);
    unsigned cdrom = (uint16_t)frame.tf.ecx;
    int2f(v, 0x150C, 0, 0, 0);
    CHECK((uint16_t)frame.tf.ebx == 0x0217 && (frame.tf.eax >> 16) == 0xABCD);
    return cdrom;
}
static uint32_t track_start(struct dosvm *v, unsigned cdrom, unsigned t, uint8_t *control)
{
    uint8_t q[7] = {11, (uint8_t)t};
    CHECK(ioctl(v, cdrom, 3, q, 7) == MSCDEX_ST_DONE && mem(v, INFO)[1] == t);
    *control = mem(v, INFO)[6];
    return get32(mem(v, INFO) + 2);
}
static void mix(unsigned quanta) { struct audio_frame out[AUDIO_QUANTUM]; while (quanta--) audio_mix(out, AUDIO_QUANTUM); }

int main(void)
{
    test_model();
    fake_storage.ready = true;
    fake_storage.optical_bindings[2].device = &cd;   /* drive present, no medium */
    audio.ready = true; audio.master.left = audio.master.right = 100;
    dosvm_cdaudio_test_mode();

    /* Owner-supplied tracks of the CD for Quake: track 2 = 3 sectors and
     * 100 bytes (a rip's partial last sector), track 3 = 2 sectors; an
     * 8-bit TRACK04.WAV is refused and ends the disc. */
    uint32_t s2, s3, s4;
    uint8_t *t2 = wav(2, 3 * 588 + 25, 2, 44100, 16, &s2), *t3 = wav(3, 2 * 588, 2, 44100, 16, &s3);
    uint8_t *t4 = wav(4, 588, 2, 44100, 8, &s4);
    add_dir("C:\\GAMES\\CDAUDIO\\QUAKE");
    add_file("C:\\GAMES\\CDAUDIO\\QUAKE\\TRACK02.WAV", t2, s2);
    add_file("C:\\GAMES\\CDAUDIO\\QUAKE\\TRACK03.WAV", t3, s3);
    add_file("C:\\GAMES\\CDAUDIO\\QUAKE\\TRACK04.WAV", t4, s4);

    /* ---- 1. WAV folder (no disc in the drive) ---- */
    struct dosvm *v = &vm[0];
    v->info.key = (struct dosvm_key){1, 7};
    strcpy(v->name, "Quake");
    for (unsigned i = 0x30; i <= 0x37; i++) v->audio.sb.mixer[i] = 31u << 3;
    g_ticks = 5000;
    CHECK(!dosvm_cdaudio_open(v));
    CHECK(find_record("cdaudio drive slot=2 generation=0 result=no_medium tracks=0 audio=0"));
    CHECK(find_record("cdaudio track file=C:\\GAMES\\CDAUDIO\\QUAKE\\TRACK04.WAV result=refused reason=bits"));
    CHECK(find_record("cdaudio disc vm=1 generation=7 source=C:\\GAMES\\CDAUDIO\\QUAKE tracks=3 audio=2 leadout=456 result=disc"));
    CHECK(find_record("cdaudio source vm=1 source=wav generation=0 tracks=3 audio=2 leadout=456 reason=wav"));
    CHECK(!tables && !toc_reads);                                                /* table returned after the scan */

    memset(&frame, 0, sizeof(frame)); frame.tf.eax = 0x1680;                    /* not ours */
    CHECK(dosvm_cdaudio_reflect(v, &frame, 0x202) == -ENOSYS && dosvm_cdaudio_interrupt(v, &frame) == -ENOSYS);
    unsigned cdrom = quake_init(v);
    CHECK(cdrom == 5);                                                           /* F: after C:-E: */
    memset(&frame, 0, sizeof(frame)); frame.tf.eax = 0x150B; frame.tf.ecx = cdrom;  /* V86 path */
    CHECK(!dosvm_cdaudio_interrupt(v, &frame) && (uint16_t)frame.tf.ebx == 0xADAD && (uint16_t)frame.tf.eax);
    CHECK(set_volume(v, cdrom, 255) == MSCDEX_ST_DONE);
    CHECK(ioctl(v, cdrom, 3, changed9, 2) == MSCDEX_ST_DONE && mem(v, INFO)[1] == 1); /* boot disc: not "changed" */
    CHECK(ioctl(v, cdrom, 3, c10, 7) == MSCDEX_ST_DONE);
    const uint8_t *info = mem(v, INFO);
    CHECK(info[1] == 1 && info[2] == 3 && get32(info + 3) == mscdex_redbook(456));
    uint8_t control[4];
    uint32_t start[4];
    for (unsigned t = 1; t <= 3; t++) start[t] = track_start(v, cdrom, t, &control[t]);
    CHECK(control[1] == 0x41 && control[2] == 0x01 && control[3] == 0x01);
    CHECK(RedBookToSector(start[2]) - 150 == 450 && RedBookToSector(start[3]) - 150 == 454);
    uint32_t length2 = (uint32_t)(RedBookToSector(start[3]) - RedBookToSector(start[2]));
    CHECK(ioctl(v, cdrom, 3, status6, 5) == MSCDEX_ST_DONE);
    CHECK(get32(info + 1) == (MSCDEX_DEV_UNLOCKED | MSCDEX_DEV_RAW | MSCDEX_DEV_AUDIO | MSCDEX_DEV_CHANNELS | MSCDEX_DEV_REDBOOK));

    /* CDAudio_Play(2, true). */
    CHECK(set_volume(v, cdrom, 255) == MSCDEX_ST_DONE);
    CHECK(play(v, cdrom, 1, start[2], length2) == (MSCDEX_ST_DONE | MSCDEX_ST_BUSY));
    pump(2);
    CHECK(find_record("cdaudio mscdex vm=1 drive=F version=2.23 disc=1 source=wav dir=C:\\GAMES\\CDAUDIO\\QUAKE result=disc"));
    CHECK(find_record("cdaudio play vm=1 source=wav track=2 lba=450 sectors=4 gain=100/100"));
    struct audio_stream *st = slots[0].stream;
    CHECK(st && audio.streams[0] == st && st->rate == 44100 && st->channels == 2 && st->gain[0] == 100);
    CHECK(slots[0].cap == CD_RING && contig_runs == 1);                           /* 37-page reservoir */
    CHECK(st->queued == AUDIO_RING);
    for (unsigned i = 0; i < 64; i++)
        CHECK(audio_frame_at(st, i)->left == sample(2, i, 0) && audio_frame_at(st, i)->right == sample(2, i, 1));
    struct audio_frame out[AUDIO_QUANTUM];
    audio_mix(out, AUDIO_QUANTUM);
    CHECK(out[0].left == sample(2, 0, 0) && out[0].right == sample(2, 0, 1));    /* mixer routing, phase 0 */
    unsigned moving = 0;
    for (unsigned i = 1; i < AUDIO_QUANTUM; i++) moving += out[i].left != out[i - 1].left;
    CHECK(moving > 200 && st->consumed > 200 && st->consumed < 240);            /* 256 * 44100 / 48000 */
    for (unsigned i = 0; i < 40 && slots[0].playing; i++) { mix(1); pump(1); }
    CHECK(!slots[0].playing && st->draining && st->accepted == 4 * 588 && st->drain_goal == 4 * 588);
    CHECK(!slots[0].read_errors && vfs_reads);

    /* CDAudio_Update: busy while playing, done after 4 sectors (53 ms). */
    g_ticks += 40;
    CHECK(ioctl(v, cdrom, 3, audio15, 11) == (MSCDEX_ST_DONE | MSCDEX_ST_BUSY));
    CHECK(get16(info + 1) == 0 && get32(info + 3) == mscdex_redbook(450) && get32(info + 7) == mscdex_redbook(454));
    g_ticks += 20;
    CHECK(ioctl(v, cdrom, 3, audio15, 11) == MSCDEX_ST_DONE);
    for (unsigned i = 0; i < 20; i++) { mix(1); pump(1); }
    CHECK(find_record("cdaudio complete vm=1 end=454 streamed=4 read_errors=0 underruns=0"));
    CHECK(!slots[0].stream && !audio.streams[0]);

    /* Looping replay, CDAudio_Pause (STOP) and CDAudio_Resume. */
    CHECK(play(v, cdrom, 1, start[2], length2) == (MSCDEX_ST_DONE | MSCDEX_ST_BUSY));
    pump(2);
    CHECK(slots[0].stream);
    g_ticks += 27;                                                                /* 2 sectors */
    CHECK(request(v, cdrom, 133, 0, 0) == MSCDEX_ST_DONE);
    pump(1);
    CHECK(!slots[0].stream && !audio.streams[0] && !slots[0].job_active);         /* stop is immediate */
    CHECK(find_record("cdaudio stop vm=1 position=452 paused=1"));
    CHECK(ioctl(v, cdrom, 3, audio15, 11) == MSCDEX_ST_DONE && get16(info + 1) == 1 &&
          get32(info + 3) == mscdex_redbook(452) && get32(info + 7) == mscdex_redbook(454));
    CHECK(request(v, cdrom, 136, 0, 0) == (MSCDEX_ST_DONE | MSCDEX_ST_BUSY));
    pump(2);
    CHECK(slots[0].stream && audio_frame_at(slots[0].stream, 0)->left == sample(2, 2 * 588, 0));
    CHECK(find_record("cdaudio resume vm=1 lba=452 sectors=2"));
    /* bgmvolume 0.5 and the guest's CT1745 CD level move the stream gain. */
    CHECK(set_volume(v, cdrom, 128) == (MSCDEX_ST_DONE | MSCDEX_ST_BUSY));
    v->audio.sb.mixer[0x37] = 26u << 3;
    CHECK(ioctl(v, cdrom, 3, audio15, 11) & MSCDEX_ST_BUSY);
    pump(1);
    CHECK(slots[0].stream->gain[0] == 50 && slots[0].stream->gain[1] == 16);
    v->audio.sb.mixer[0x37] = 31u << 3;
    CHECK(set_volume(v, cdrom, 255) & MSCDEX_ST_DONE);
    /* Second STOP clears the resume point; RESUME then fails. */
    CHECK(request(v, cdrom, 133, 0, 0) == MSCDEX_ST_DONE && request(v, cdrom, 133, 0, 0) == MSCDEX_ST_DONE);
    CHECK(request(v, cdrom, 136, 0, 0) == (MSCDEX_ST_DONE | MSCDEX_ST_ERROR | MSCDEX_ERR_GENERAL));
    /* Data track, wrong drive, HSG play clipped to the lead-out, unknown command. */
    CHECK(play(v, cdrom, 1, start[1], 10) == (MSCDEX_ST_DONE | MSCDEX_ST_ERROR | MSCDEX_ERR_GENERAL));
    memset(mem(v, REQ), 0, 26); mem(v, REQ)[2] = 133;
    int2f(v, 0x1510, REQ & 15, 2, REQ >> 4);
    CHECK((frame.tf.eflags & V86_CF) && (uint16_t)frame.tf.eax == 0x000F);
    CHECK(play(v, cdrom, 0, 453, 100) == (MSCDEX_ST_DONE | MSCDEX_ST_BUSY));
    CHECK(slots[0].want_lba == 453 && slots[0].want_sectors == 3);
    CHECK(request(v, cdrom, 134, 0, 0) == (MSCDEX_ST_DONE | MSCDEX_ST_BUSY | MSCDEX_ST_ERROR | MSCDEX_ERR_UNKNOWN_COMMAND));
    /* Shutdown: a stopping volume closes the track file and the VFS table. */
    CHECK(play(v, cdrom, 0, 450, 100) == (MSCDEX_ST_DONE | MSCDEX_ST_BUSY));    /* 6 sectors */
    pump(2);
    CHECK(slots[0].job_active && slots[0].fd >= 0 && tables == 1);
    fake_storage.stopped = true;
    pump(2);
    CHECK(slots[0].fd < 0 && !tables && find_record("cdaudio read vm=1 source=wav track=3"));
    fake_storage.stopped = false;
    /* Eject ends play and the disc until the tray closes; media changed once. */
    uint8_t eject[1] = {0}, tray[1] = {5};
    CHECK(ioctl(v, cdrom, 12, eject, 1) == MSCDEX_ST_DONE);
    CHECK(ioctl(v, cdrom, 3, status6, 5) == MSCDEX_ST_DONE &&
          (get32(info + 1) & (MSCDEX_DEV_DOOR_OPEN | MSCDEX_DEV_NO_DISC)) == (MSCDEX_DEV_DOOR_OPEN | MSCDEX_DEV_NO_DISC));
    CHECK(ioctl(v, cdrom, 3, c10, 7) == (MSCDEX_ST_DONE | MSCDEX_ST_ERROR | MSCDEX_ERR_NOT_READY));
    CHECK(ioctl(v, cdrom, 12, tray, 1) == MSCDEX_ST_DONE);
    CHECK(ioctl(v, cdrom, 3, changed9, 2) == MSCDEX_ST_DONE && info[1] == 0xFF);
    CHECK(ioctl(v, cdrom, 3, changed9, 2) == MSCDEX_ST_DONE && info[1] == 1);
    pump(2);

    /* ---- 2. A VM with no tracks and no disc ---- */
    struct dosvm *w = &vm[1];
    w->info.key = (struct dosvm_key){2, 9};
    strcpy(w->name, "Doom");
    for (unsigned i = 0x30; i <= 0x37; i++) w->audio.sb.mixer[i] = 31u << 3;
    CHECK(!dosvm_cdaudio_open(w));
    CHECK(find_record("cdaudio disc vm=2 generation=9 source=C:\\GAMES\\CDAUDIO tracks=0 audio=0 leadout=0 result=no_tracks"));
    CHECK(find_record("cdaudio source vm=2 source=none generation=0 tracks=0 audio=0 leadout=0 reason=no_tracks"));
    unsigned wdrive = quake_init(w);                                              /* not "MSCDEX not loaded" */
    CHECK(set_volume(w, wdrive, 255) == MSCDEX_ST_DONE);
    CHECK(ioctl(w, wdrive, 3, c10, 7) == (MSCDEX_ST_DONE | MSCDEX_ST_ERROR | MSCDEX_ERR_NOT_READY)); /* "No CD in player" */
    CHECK(ioctl(w, wdrive, 3, status6, 5) == MSCDEX_ST_DONE && (get32(mem(w, INFO) + 1) & MSCDEX_DEV_NO_DISC));
    pump(1);
    CHECK(find_record("cdaudio mscdex vm=2 drive=F version=2.23 disc=0 source=none dir=C:\\GAMES\\CDAUDIO result=no_tracks"));
    CHECK(find_record("cdaudio refused vm=2 command=3 code=10 error=2 reason=no_tracks source=none dir=C:\\GAMES\\CDAUDIO"));
    unsigned before = record_count;
    CHECK(ioctl(w, wdrive, 3, c10, 7) & MSCDEX_ST_ERROR);
    pump(1);
    CHECK(record_count == before);                                               /* polls are not re-recorded */

    /* ---- 3. The original CD in the drive wins over the WAV folder ---- */
    const uint8_t mixed_control[3] = {4, 0, 0};            /* track 1 data, 2-3 audio, as Quake's CD */
    const uint32_t mixed_start[3] = {0, 1200, 1300};
    insert(3, mixed_control, mixed_start, 1400);
    pump(1);
    CHECK(toc_reads == 1);
    CHECK(find_record("cdaudio drive slot=2 generation=1 result=audio tracks=3 audio=2 first=1 last=3 leadout=1400 cdda=1"));
    CHECK(find_record("cdaudio source vm=1 source=cd generation=1 tracks=3 audio=2 leadout=1400 reason=audio_disc"));
    CHECK(find_record("cdaudio source vm=2 source=cd generation=1 tracks=3 audio=2 leadout=1400 reason=audio_disc"));
    /* Quake with -cdmediacheck (or "cd reset"): media changed, then the TOC. */
    CHECK(ioctl(v, cdrom, 3, changed9, 2) == MSCDEX_ST_DONE && info[1] == 0xFF);
    CHECK(ioctl(v, cdrom, 3, c10, 7) == MSCDEX_ST_DONE && info[1] == 1 && info[2] == 3 &&
          get32(info + 3) == mscdex_redbook(1400));
    for (unsigned t = 1; t <= 3; t++) start[t] = track_start(v, cdrom, t, &control[t]);
    CHECK(control[1] == 0x41 && control[2] == 0x01 && control[3] == 0x01);
    CHECK(start[2] == mscdex_redbook(1200) && start[3] == mscdex_redbook(1300));
    CHECK(ioctl(v, cdrom, 3, status6, 5) == MSCDEX_ST_DONE && !(get32(info + 1) & MSCDEX_DEV_NO_DISC));
    uint32_t length = (uint32_t)(RedBookToSector(start[3]) - RedBookToSector(start[2]));
    CHECK(length == 100);
    CHECK(set_volume(v, cdrom, 255) == MSCDEX_ST_DONE);
    uint32_t streamed0 = slots[0].streamed, errors0 = slots[0].read_errors;
    CHECK(play(v, cdrom, 1, start[2], length) == (MSCDEX_ST_DONE | MSCDEX_ST_BUSY));
    pump(1);                                                                      /* job starts */
    CHECK(find_record("cdaudio play vm=1 source=cd track=2 lba=1200 sectors=100 gain=100/100"));
    unsigned packets = read_packets;
    pump(1);                                                                      /* one 8-sector READ CD */
    CHECK(read_packets == packets + 1 && read_sectors == 8);
    st = slots[0].stream;
    CHECK(st && st->rate == 44100 && st->queued == AUDIO_RING);
    for (unsigned i = 0; i < 588; i += 7)
        CHECK(audio_frame_at(st, i)->left == cd_sample(1200, i, 0) && audio_frame_at(st, i)->right == cd_sample(1200, i, 1));
    /* Bounded prefetch: without consumption the reservoir fills to 64 sectors
     * and stops; one packet per pass. */
    for (unsigned i = 0; i < 20; i++) {
        unsigned p0 = read_packets;
        pump(1);
        CHECK(read_packets - p0 <= 1 && slots[0].count <= slots[0].cap);
    }
    CHECK(slots[0].count <= CD_RING && read_sectors == slots[0].count + (slots[0].streamed - streamed0));
    CHECK(read_sectors <= CD_RING + CD_CHUNK);                                    /* reservoir + what the stream holds */
    CHECK(slots[0].cap - slots[0].count < CD_CHUNK);
    audio_mix(out, AUDIO_QUANTUM);
    CHECK(out[0].left == cd_sample(1200, 0, 0) && out[0].right == cd_sample(1200, 0, 1));
    /* Steady state: the music plays out sector by sector from the drive. */
    for (unsigned i = 0; i < 30; i++) { mix(2); pump(1); }
    CHECK(slots[0].streamed - streamed0 > 20 && slots[0].read_errors == errors0 && slots[0].playing && !slots[0].underruns);

    /* ---- 4. Disc removed mid-play: stop, record, back to the WAV folder ---- */
    for (unsigned i = 0; i < 40 && slots[0].cap - slots[0].count < CD_CHUNK; i++) { mix(2); (void)dosvm_cdaudio_step(); }
    CHECK(slots[0].cap - slots[0].count >= CD_CHUNK && slots[0].job_remaining);
    remove_disc();
    (void)dosvm_cdaudio_read_step();
    CHECK(find_record("cdaudio drive slot=2 generation=2 result=no_medium"));
    CHECK(find_record("cdaudio read vm=1 source=cd track=2") && find_record("reason=no_medium fallback=stop"));
    (void)dosvm_cdaudio_step();
    CHECK(!slots[0].stream && !slots[0].job_active && !audio.streams[0]);
    CHECK(find_record("cdaudio source vm=1 source=wav generation=0 tracks=3 audio=2 leadout=456 reason=wav"));
    CHECK(find_record("cdaudio source vm=2 source=none"));
    CHECK(ioctl(v, cdrom, 3, audio15, 11) == MSCDEX_ST_DONE);                      /* busy cleared */
    CHECK(ioctl(v, cdrom, 3, changed9, 2) == MSCDEX_ST_DONE && info[1] == 0xFF);
    CHECK(ioctl(v, cdrom, 3, c10, 7) == MSCDEX_ST_DONE && info[2] == 3 && get32(info + 3) == mscdex_redbook(456));
    CHECK(ioctl(w, wdrive, 3, c10, 7) == (MSCDEX_ST_DONE | MSCDEX_ST_ERROR | MSCDEX_ERR_NOT_READY));
    /* A play request made for the removed disc is void. */
    pump(2);
    CHECK(!slots[0].stream);

    /* ---- 5. A data-only disc: the WAV folder stays first; without one the
     * drive reports the data track, as MSCDEX does, and nothing plays ---- */
    const uint8_t data_control[1] = {4};
    const uint32_t data_start[1] = {0};
    unsigned vm1_seq = slots[0].disc_seq;
    insert(1, data_control, data_start, 5000);
    pump(1);
    CHECK(find_record("cdaudio drive slot=2 generation=3 result=data_only tracks=1 audio=0 first=1 last=1 leadout=5000"));
    CHECK(slots[0].disc_seq == vm1_seq && slots[0].source == CD_SRC_WAV);
    CHECK(find_record("cdaudio source vm=2 source=cd generation=3 tracks=1 audio=0 leadout=5000 reason=data_only"));
    CHECK(ioctl(w, wdrive, 3, c10, 7) == MSCDEX_ST_DONE);
    const uint8_t *winfo = mem(w, INFO);
    CHECK(winfo[1] == 1 && winfo[2] == 1 && get32(winfo + 3) == mscdex_redbook(5000));
    uint8_t wcontrol;
    CHECK(track_start(w, wdrive, 1, &wcontrol) == mscdex_redbook(0) && wcontrol == 0x41);
    CHECK(play(w, wdrive, 1, mscdex_redbook(0), 75) == (MSCDEX_ST_DONE | MSCDEX_ST_ERROR | MSCDEX_ERR_GENERAL));
    pump(2);
    CHECK(!slots[1].stream && !slots[1].job_active);

    /* ---- 6a. Closing while vm-cdread still owns its file: the service step
     * stops the stream and then waits for the reader instead of reporting
     * pending work (pending() stayed true, vm-cdaudio spun without yielding
     * and the system hung when DOOM's DOS Box was closed in QEMU) ---- */
    slots[0].closing = true;
    CHECK(dosvm_cdaudio_step());
    CHECK(slots[0].used && !slots[0].released && !slots[0].stream);
    CHECK(!pending(0));
    (void)dosvm_cdaudio_read_step();
    CHECK(slots[0].released && pending(0));

    /* ---- 6. Close: streams stop, files close, slots free ---- */
    dosvm_cdaudio_close(v);
    CHECK(!slots[0].used && !slots[0].stream && !slots[0].res && !audio.streams[0]);
    CHECK(find_record("cdaudio close vm=1 gen=7 req="));
    for (int h = 0; h < 8; h++) CHECK(!handles[h].open);
    CHECK(!tables);
    memset(&frame, 0, sizeof(frame)); frame.tf.eax = 0x1500;
    CHECK(dosvm_cdaudio_interrupt(v, &frame) == -ENOSYS);                        /* gone with its VM */
    dosvm_cdaudio_close(w);
    CHECK(!slots[1].used);
    free(t2); free(t3); free(t4);
    printf("cdaudio_test: %u checks passed\n", checks);
    return 0;
}
