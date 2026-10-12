/* F3 workload row (directive f3-06; dos-dpmi-contract.md acceptance row "F3
 * workloads"). Runtime-selected probes, sweep order after dpmi:
 *   f3:dos4gw     pinned DOS/4GW client DPMI4GW.EXE (Open Watcom 2.0 runtime
 *                 1.97), 20 measured launch/exit loops, each in a fresh VM,
 *                 with VM ledger, free pages, tasks and kernel heap restored.
 *   f3:doom       original DOS DOOM (owner-local payload on the runner's
 *                 fixture disk): RUNREC.COM \DOOM DOOM.EXE -config CIUKI.CFG
 *                 -nosound -timedemo demo1, 5 loops; launch time, page flips,
 *                 gametics/realtics from the text screen, exit status, DPMI
 *                 and f3-05 audio counters.
 *   f3:doom-sound the same with sound (SB16 PCM + OPL music devices).
 *   f3:wolf3d     Wolf3D (owner-local): signon/title Mode X digests, scripted
 *                 keyboard to a game, XMS/EMS usage, F10+Y quit; 5 loops.
 *   f3:vm-starve  a CLI-loop VM beside a DOS/4GW VM: timer and keyboard
 *                 delivery and the native heartbeat keep their rates.
 * Game payloads are found as \CIUKIGAM.IMG on a further drive (the fixture
 * disk of scripts/test/dos_fixtures.py) and booted as the VM's read-only base
 * image (dosvm_config.base); without it the probe ends NOT_RUN
 * reason=fixtures_absent. Records: docs/design/f3-acceptance.md, f3-06.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/probe.h>
#include <ciuki/dosvm.h>
#include <ciuki/dosvm_audio.h>
#include <ciuki/dpmi.h>
#include <ciuki/abi.h>
#include <ciuki/sha256.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>
#include <ciuki/storage.h>

#ifndef CIUKI_BUILD_HEX8
#define CIUKI_BUILD_HEX8 "00000000"
#endif
#define DOS4GW_LOOPS 20u
#define GAME_LOOPS 5u
#define DOOM_LOOP_MS 1200000u      /* one timedemo under TCG; QEMU suite timeout covers 5 */
#define WOLF_LOOP_MS 300000u
#define WOLF_PLAY_START_MS 90000u    /* menu keys to steady 3D flipping */
#define BASE_BYTES (262144u * 512u)

/* ---- shared VM helpers ---- */
static bool wl_prompt(struct dosvm *v)
{
    const uint16_t *text = dosvm_text(v, 0);
    for (unsigned row = 0; row < 25; row++) {
        bool drive = false;
        for (unsigned col = 0; col < 80; col++) {
            unsigned i = row * 80 + col;
            if (col < 78 && (uint8_t)text[i] == 'C' && (uint8_t)text[i + 1] == ':' && (uint8_t)text[i + 2] == '\\') drive = true;
            if (drive && (uint8_t)text[i] == '>') return true;
        }
    }
    return false;
}
static int wl_boot(const struct dosvm_config *config, struct dosvm **out)
{
    int e = dosvm_create(config, out);
    if (e) return e;
    if ((e = dosvm_boot(*out))) return e;
    uint64_t until = deadline_after_ms(30000);
    for (;;) {
        struct dosvm_info info;
        dosvm_info(*out, &info);
        if (info.state >= DOSVM_STOPPING) return -V86_EIO;
        if (info.dos_ready && wl_prompt(*out)) return 0;
        if (deadline_passed(until)) return -V86_ETIMEDOUT;
        task_sleep_ms(1);
    }
}
/* The BIOS buffer holds 15 keys: wait for room instead of failing a long line. */
static int wl_type(struct dosvm *v, const char *text)
{
    struct dosvm_key key = dosvm_key(v);
    uint64_t until = deadline_after_ms(10000);
    for (unsigned i = 0; ; i++) {
        uint16_t code = text[i] ? (uint8_t)text[i] : 0x1C0D;
        while (!dosvm_keyboard(key, code)) {
            if (!dosvm_valid(key)) return -V86_EIO;
            if (deadline_passed(until)) return -ENOSPC;
            task_sleep_ms(2);
        }
        if (!text[i]) return 0;
    }
}
static int wl_exit(struct dosvm **v)
{
    struct dosvm_key key = dosvm_key(*v);
    struct dosvm_info info;
    dosvm_info(*v, &info);
    int e = info.state < DOSVM_STOPPING ? wl_type(*v, "EXIT") : 0;
    uint64_t until = deadline_after_ms(5000);
    for (; !e; task_sleep_ms(1)) {
        dosvm_info(*v, &info);
        if (info.state >= DOSVM_STOPPING) break;
        if (deadline_passed(until)) e = -V86_ETIMEDOUT;
    }
    int d = dosvm_destroy(key, e != 0);
    *v = 0;
    return e ? e : d;
}
static bool ledger_equal(const struct dosvm_ledger *a, const struct dosvm_ledger *b)
{
    return a->vms == b->vms && a->tasks == b->tasks && a->pages == b->pages &&
           a->handles == b->handles && a->devices == b->devices &&
           a->reserved == b->reserved && a->resident == b->resident;
}
struct wl_baseline { struct dosvm_ledger ledger; uint32_t free_pages, tasks; size_t heap; };
static void baseline_take(struct wl_baseline *b)
{
    dosvm_snapshot(&b->ledger);
    b->free_pages = pmm_free_count(); b->tasks = g_task_count; b->heap = kheap_in_use();
}
static bool baseline_equal(const struct wl_baseline *a, const struct wl_baseline *b)
{
    return ledger_equal(&a->ledger, &b->ledger) && a->free_pages == b->free_pages &&
           a->tasks == b->tasks && a->heap == b->heap;
}
static bool dpmi_released(const struct dpmi_info *d)
{
    return !d->active && !d->clients && !d->selectors && !d->blocks && !d->dos_blocks &&
           !d->callbacks && !d->watchpoints && !d->depth && !d->pool_used && !d->hstack;
}
static void cleanup_record(const char *name, const struct dosvm_ledger *before, int *error)
{
    struct dosvm_ledger after;
    dosvm_snapshot(&after);
    bool cleaned = ledger_equal(before, &after);
    if (!cleaned && !*error) *error = -ENOMEM;
    rec_emit(name, "DATA", "case=cleanup vms=%u tasks=%u pages=%u handles=%u devices=%u restored=%u",
             after.vms, after.tasks, after.pages, after.handles, after.devices, cleaned);
}

/* ---- FAT16 reader over a VM's own private disk (base + overlay) ---- */
struct wfat { uint32_t fat, root, data; uint16_t root_entries; uint8_t spc; };
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t)le16(p + 2) << 16; }
static int wfat_open(struct dosvm *v, struct wfat *f, uint8_t *sector)
{
    int e = dosvm_disk_read(v, 0, sector);
    if (e) return e;
    if (sector[510] != 0x55 || sector[511] != 0xAA || le16(sector + 11) != 512 || !sector[13] || !sector[16] ||
        !le16(sector + 22) || !le16(sector + 17))
        return -EINVAL;
    f->spc = sector[13];
    f->fat = le16(sector + 14);
    f->root_entries = le16(sector + 17);
    f->root = f->fat + sector[16] * (uint32_t)le16(sector + 22);
    f->data = f->root + ((uint32_t)f->root_entries * 32 + 511) / 512;
    return 0;
}
static int wfat_next(struct dosvm *v, const struct wfat *f, uint16_t cluster, uint16_t *next, uint8_t *sector)
{
    int e = dosvm_disk_read(v, f->fat + cluster / 256u, sector);
    if (e) return e;
    *next = le16(sector + (cluster % 256u) * 2);
    return 0;
}
/* Calls fn for every live short entry of a directory (dir 0 = root). */
typedef int (*wfat_visit)(void *arg, const uint8_t *entry);
static int wfat_dir(struct dosvm *v, const struct wfat *f, uint16_t dir, wfat_visit fn, void *arg, uint8_t *sector)
{
    uint16_t c = dir;
    for (unsigned guard = 0; guard < 4096; guard++) {
        unsigned count = dir ? f->spc : (f->root_entries * 32u + 511) / 512;
        uint32_t lba = dir ? f->data + (uint32_t)(c - 2) * f->spc : f->root;
        for (unsigned s = 0; s < count; s++) {
            int e = dosvm_disk_read(v, lba + s, sector);
            if (e) return e;
            for (unsigned i = 0; i < 16; i++) {
                const uint8_t *d = sector + i * 32;
                if (!d[0]) return 0;
                if (d[0] == 0xE5 || (d[11] & 0x08) || d[0] == '.') continue;
                uint8_t copy[32];
                memcpy(copy, d, 32);
                if ((e = fn(arg, copy))) return e > 0 ? 0 : e;
            }
        }
        if (!dir) return 0;
        int e = wfat_next(v, f, c, &c, sector);
        if (e) return e;
        if (c < 2 || c >= 0xFFF8) return 0;
    }
    return -EINVAL;
}
struct wfat_lookup { const char *n83; uint16_t cluster; uint32_t size; uint8_t attr; bool found; };
static int lookup_visit(void *arg, const uint8_t *d)
{
    struct wfat_lookup *l = arg;
    if (memcmp(d, l->n83, 11)) return 0;
    l->cluster = le16(d + 26); l->size = le32(d + 28); l->attr = d[11]; l->found = true;
    return 1;
}
static void name83(const char *name, char out[11])
{
    memset(out, ' ', 11);
    unsigned i = 0, k = 0;
    while (name[i] && name[i] != '.' && k < 8) out[k++] = name[i++];
    while (name[i] && name[i] != '.') i++;
    if (name[i] == '.') { i++; for (k = 8; name[i] && k < 11; ) out[k++] = name[i++]; }
}
/* "DIR/NAME.EXT" or "NAME.EXT" from the root. */
static int wfat_find(struct dosvm *v, const struct wfat *f, const char *path, uint16_t *cluster, uint32_t *size,
                     uint8_t *attr, uint8_t *sector)
{
    uint16_t dir = 0;
    char part[13];
    for (;;) {
        unsigned n = 0;
        while (*path == '/') path++;
        while (path[n] && path[n] != '/' && n < 12) { part[n] = path[n]; n++; }
        part[n] = 0;
        if (!n || (path[n] && path[n] != '/')) return -EINVAL;
        char n83[11];
        name83(part, n83);
        struct wfat_lookup l = {.n83 = n83};
        int e = wfat_dir(v, f, dir, lookup_visit, &l, sector);
        if (e) return e;
        if (!l.found) return -ENOENT;
        path += n;
        if (!*path) { *cluster = l.cluster; *size = l.size; *attr = l.attr; return 0; }
        if (!(l.attr & 0x10)) return -ENOTDIR;
        dir = l.cluster;
    }
}
/* SHA-256 of a cluster chain; flags a bound DOS/4GW Professional extender. */
static const char extender_mark[] = "DOS/4GW Professional";
static int wfat_hash(struct dosvm *v, const struct wfat *f, uint16_t cluster, uint32_t size, uint8_t digest[32],
                     bool *bound, uint8_t *sector, uint8_t *fat)
{
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    uint32_t left = size;
    unsigned match = 0;
    *bound = false;
    while (left) {
        if (cluster < 2 || cluster >= 0xFFF8) return -V86_EIO;
        for (unsigned s = 0; s < f->spc && left; s++) {
            int e = dosvm_disk_read(v, f->data + (uint32_t)(cluster - 2) * f->spc + s, sector);
            if (e) return e;
            uint32_t n = left < 512 ? left : 512;
            sha256_update(&ctx, sector, n);
            /* 'D' occurs once in the mark: restarting at a mismatch is exact. */
            for (uint32_t i = 0; i < n && !*bound; i++) {
                match = sector[i] == (uint8_t)extender_mark[match] ? match + 1 : sector[i] == 'D' ? 1 : 0;
                if (match == sizeof(extender_mark) - 1) *bound = true;
            }
            left -= n;
        }
        if (left) { int e = wfat_next(v, f, cluster, &cluster, fat); if (e) return e; }
    }
    sha256_final(&ctx, digest);
    return 0;
}
static void hex32(const uint8_t digest[32], char hex[65])
{
    for (unsigned k = 0; k < 32; k++) {
        hex[2 * k] = "0123456789abcdef"[digest[k] >> 4];
        hex[2 * k + 1] = "0123456789abcdef"[digest[k] & 15];
    }
    hex[64] = 0;
}
/* group=dos_payload: compared by the runner with the image manifest (f3-04). */
static int hash_payload(const char *name, struct dosvm *v, const char *path, bool *bound)
{
    uint8_t *buf = kmalloc(1024);
    if (!buf) return -ENOMEM;
    struct wfat f;
    uint16_t cluster = 0; uint32_t size = 0; uint8_t attr = 0, digest[32];
    int e = wfat_open(v, &f, buf);
    if (!e) e = wfat_find(v, &f, path, &cluster, &size, &attr, buf);
    if (!e) e = wfat_hash(v, &f, cluster, size, digest, bound, buf, buf + 512);
    if (!e) {
        char hex[65];
        hex32(digest, hex);
        rec_emit(name, "DATA", "group=dos_payload path=/%s size=%u sha256=%s", path, size, hex);
    }
    kfree(buf);
    return e;
}
/* Every regular file of a workload directory: group=workload_file, compared
 * by the runner with the fixture generator's manifest (dos_fixtures.py). */
struct hash_dir { struct dosvm *v; const struct wfat *f; const char *name, *dir, *exe; uint8_t *buf; unsigned files;
                  bool exe_found, bound; int error; };
static int hash_dir_visit(void *arg, const uint8_t *d)
{
    struct hash_dir *h = arg;
    if (d[11] & 0x18) return 0;                    /* directories, labels */
    char file[13]; unsigned k = 0;
    for (unsigned i = 0; i < 8 && d[i] != ' '; i++) file[k++] = (char)d[i];
    if (d[8] != ' ') { file[k++] = '.'; for (unsigned i = 8; i < 11 && d[i] != ' '; i++) file[k++] = (char)d[i]; }
    file[k] = 0;
    for (unsigned i = 0; i < k; i++)
        if (!((file[i] >= 'A' && file[i] <= 'Z') || (file[i] >= '0' && file[i] <= '9') || file[i] == '.' ||
              file[i] == '_' || file[i] == '-')) { h->error = -EINVAL; return 1; }
    bool bound = false;
    uint8_t digest[32];
    /* The hash re-reads FAT/data sectors: the directory sector buffer is a copy. */
    int e = wfat_hash(h->v, h->f, le16(d + 26), le32(d + 28), digest, &bound, h->buf + 512, h->buf + 1024);
    if (e) { h->error = e; return 1; }
    char hex[65];
    hex32(digest, hex);
    bool generated = !strncmp(file, "CIUKI.CFG", sizeof(file));
    rec_emit(h->name, "DATA", "group=workload_file path=/%s/%s size=%u sha256=%s redistribution=%s",
             h->dir, file, le32(d + 28), hex, generated ? "free" : "local-only");
    if (!strncmp(file, h->exe, sizeof(file))) { h->exe_found = true; h->bound = bound; }
    h->files++;
    return 0;
}
/* exe: the workload's executable; *bound = it carries a DOS/4GW Professional
 * extender (DOOM.EXE). -ENOENT when the executable is not in the directory. */
static int hash_workload(const char *name, struct dosvm *v, const char *dir, const char *exe, unsigned *files, bool *bound)
{
    uint8_t *buf = kmalloc(1536);
    if (!buf) return -ENOMEM;
    struct wfat f;
    uint16_t cluster = 0; uint32_t size = 0; uint8_t attr = 0;
    int e = wfat_open(v, &f, buf);
    if (!e) e = wfat_find(v, &f, dir, &cluster, &size, &attr, buf);
    if (!e && !(attr & 0x10)) e = -ENOTDIR;
    struct hash_dir h = {v, &f, name, dir, exe, buf, 0, false, false, 0};
    if (!e) e = wfat_dir(v, &f, cluster, hash_dir_visit, &h, buf);
    if (!e) e = h.error;
    if (!e && !h.exe_found) e = -ENOENT;
    *files = h.files;
    *bound = h.bound;
    kfree(buf);
    return e;
}

static void base_record(const char *name, const char *path)
{
    rec_emit(name, "DATA", "case=fixture status=present drive=%c base=CIUKIGAM.IMG size=%u", path[0], BASE_BYTES);
}
/* The local workload disk (dosvm_workload_base): absent -> NOT_RUN, which a
 * sweep step counts as not_run (negative), never as a pass; a storage error
 * while looking is a FAIL. */
static int fixture_absent(const char *name, int found)
{
    if (found != -ENOENT) {
        rec_emit(name, "DATA", "case=fixture status=error error=%d", found);
        rec_emit(name, "END", "status=FAIL error=%d", found);
        return 1;
    }
    rec_emit(name, "DATA", "case=fixture status=not_run reason=fixtures_absent");
    rec_emit(name, "END", "status=NOT_RUN reason=fixtures_absent");
    return -ENOENT;
}

/* Waits for the RUNREC verdict and the shell prompt; samples DPMI and VGA
 * activity on the way (first DPMI entry, first page flip, Mode X seen). */
struct run_watch {
    uint64_t start, dpmi_ms, flip_ms, end_ms;
    bool graphics;
    uint32_t width, height;
    struct dosvm_info info;
    struct dpmi_info dpmi;
    struct dosvm_video_info video;
};
static int run_wait(struct dosvm *v, uint32_t ms, struct run_watch *w)
{
    struct dosvm_key key = dosvm_key(v);
    uint64_t until = deadline_after_ms(ms);
    int e = 0;
    for (;;) {
        dosvm_info(v, &w->info);
        if (w->info.state >= DOSVM_STOPPING) { e = -V86_EIO; break; }
        if (dosvm_dpmi_info(key, &w->dpmi) && w->dpmi.stats.entries && !w->dpmi_ms) w->dpmi_ms = g_ticks - w->start;
        if (dosvm_video_info(key, &w->video)) {
            if (w->video.starts && !w->flip_ms) w->flip_ms = g_ticks - w->start;
            if (w->video.unchained && w->video.width) {
                w->graphics = true; w->width = w->video.width; w->height = w->video.height;
            }
        }
        if (w->info.verdict_done && !w->dpmi.active && wl_prompt(v)) break;
        if (deadline_passed(until)) { e = -V86_ETIMEDOUT; break; }
        task_sleep_ms(20);
    }
    w->end_ms = g_ticks - w->start;
    dosvm_info(v, &w->info);
    dosvm_dpmi_info(key, &w->dpmi);
    dosvm_video_info(key, &w->video);
    return e;
}
/* RUNREC verdict: PASS = exit 0; "Rcc_Ttt" = exit code cc, type tt. */
static bool run_status(const struct dosvm_info *info, unsigned *rc, unsigned *type)
{
    *rc = *type = 0;
    if (!info->verdict_done) return false;
    if (info->verdict_pass) return true;
    const char *d = info->verdict_detail;
    if (d[0] != 'R' || d[3] != '_' || d[4] != 'T') return false;
    unsigned value[2] = {0, 0};
    const char *digits[2] = {d + 1, d + 5};
    for (unsigned k = 0; k < 2; k++)
        for (unsigned i = 0; i < 2; i++) {
            char c = digits[k][i];
            int x = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (x < 0) return false;
            value[k] = value[k] * 16 + (unsigned)x;
        }
    *rc = value[0]; *type = value[1];
    return true;
}

/* ---- f3:dos4gw ---- */
static int dos4gw_run(struct dosvm *v, struct run_watch *w)
{
    memset(w, 0, sizeof(*w));
    dosvm_expect(v, "DPMI4GW");
    w->start = g_ticks;
    int e = wl_type(v, "\\DPMI4GW.EXE");
    if (!e) e = run_wait(v, 120000, w);
    if (!e && (!w->info.verdict_pass || w->dpmi.stats.terminations != w->dpmi.stats.entries || !w->dpmi.stats.entries))
        e = -EFAULT;
    return e;
}
static int probe_f3_dos4gw(void)
{
    const char *name = "dos4gw";
    rec_emit(name, "BEGIN", "version=1 public=%u", DOSVM_DPMI_PUBLIC);
    rec_emit(name, "DATA", "case=identity build=%s loops=%u", CIUKI_BUILD_HEX8, DOS4GW_LOOPS);
    struct dosvm_ledger before;
    dosvm_snapshot(&before);
    const struct dosvm_config config = {.commitment = 0, .probe = "dos4gw", .dpmi = true};
    struct dosvm *v = 0;
    struct run_watch w;
    unsigned passed = 0;
    bool restored = true;
    int error = storage_enable_write(storage_get(), 2);
    /* Hashes of the pinned client and extender, from the VM's own disk. */
    if (!error && !(error = wl_boot(&config, &v))) {
        bool bound = false;
        error = hash_payload(name, v, "DPMI4GW.EXE", &bound);
        if (!error) error = hash_payload(name, v, "SYSTEM/DOS4GW.EXE", &bound);
        rec_emit(name, "DATA", "case=extender path=/SYSTEM/DOS4GW.EXE kind=external version=1.97 client=/DPMI4GW.EXE redistribution=open_question");
        /* Warm-up launch: retained heap pools before the measured loops. */
        if (!error) error = dos4gw_run(v, &w);
        int x = wl_exit(&v);
        if (!error) error = x;
    }
    struct wl_baseline base, now;
    baseline_take(&base);
    for (unsigned loop = 1; !error && loop <= DOS4GW_LOOPS; loop++) {
        memset(&w, 0, sizeof(w));
        int e = wl_boot(&config, &v);
        if (!e) e = dos4gw_run(v, &w);
        bool released = dpmi_released(&w.dpmi);
        if (e && v) probe_dos_screen(name, v);
        if (v) { int x = wl_exit(&v); if (!e) e = x; }
        baseline_take(&now);
        bool same = baseline_equal(&base, &now);
        rec_emit(name, "DATA", "case=loop loop=%u pass=%u entries=%u terminations=%u faults=%u stale=%u released=%u ms=%u restored=%u error=%d",
                 loop, w.info.verdict_pass, w.dpmi.stats.entries, w.dpmi.stats.terminations, w.dpmi.stats.faults,
                 w.dpmi.stats.stale, released, (uint32_t)w.end_ms, same, e);
        if (!same) {
            rec_emit(name, "DATA", "case=baseline loop=%u pages=%u/%u tasks=%u/%u heap=%u/%u vm_pages=%u/%u handles=%u/%u",
                     loop, base.free_pages, now.free_pages, base.tasks, now.tasks, (unsigned)base.heap, (unsigned)now.heap,
                     base.ledger.pages, now.ledger.pages, base.ledger.handles, now.ledger.handles);
            restored = false;
        }
        if (!e && (!released || w.dpmi.stats.faults || w.dpmi.stats.stale)) e = -EFAULT;
        if (!e && !same) e = -ENOMEM;
        if (!e) passed++;
        error = e;
    }
    rec_emit(name, "DATA", "case=loops completed=%u required=%u restored=%u", passed, DOS4GW_LOOPS, restored);
    if (v) { int x = dosvm_destroy(dosvm_key(v), true); if (!error) error = x; }
    if (!error && passed != DOS4GW_LOOPS) error = -EFAULT;
    cleanup_record(name, &before, &error);
    rec_emit(name, "END", "status=%s error=%d", error ? "FAIL" : "PASS", error);
    return !!error;
}
CIUKI_F3_PROBE("dos4gw", probe_f3_dos4gw);

/* ---- f3:doom, f3:doom-sound ---- */
/* "timed %i gametics in %i realtics" (G_CheckDemoStatus, I_Error) on the
 * text screen after DOOM returns to mode 03h. Read from the screen latched
 * at RUNREC's verdict line: by the time the shell prompt is back, the shell
 * has redrawn its title bar on row 0, where DOOM printed the line (QEMU
 * zz-doom c11506e9). The live screen is only a fallback. */
static bool doom_timed(struct dosvm *v, unsigned *gametics, unsigned *realtics)
{
    const uint16_t *text = dosvm_verdict_text(v);
    if (!text) text = dosvm_text(v, 0);
    static const char mark[] = "timed ", mid[] = " gametics in ";
    for (unsigned row = 0; row < 25; row++) {
        char line[81];
        for (unsigned col = 0; col < 80; col++) line[col] = (char)(uint8_t)text[row * 80 + col];
        line[80] = 0;
        for (unsigned col = 0; col + 6 < 80; col++) {
            if (memcmp(line + col, mark, 6)) continue;
            unsigned i = col + 6, a = 0, b = 0, digits = 0;
            while (i < 80 && line[i] >= '0' && line[i] <= '9' && digits < 9) { a = a * 10 + (unsigned)(line[i++] - '0'); digits++; }
            if (!digits || i + 13 > 80 || memcmp(line + i, mid, 13)) continue;
            i += 13; digits = 0;
            while (i < 80 && line[i] >= '0' && line[i] <= '9' && digits < 9) { b = b * 10 + (unsigned)(line[i++] - '0'); digits++; }
            if (!digits || i + 9 > 80 || memcmp(line + i, " realtics", 9)) continue;
            *gametics = a; *realtics = b;
            return true;
        }
    }
    return false;
}
static int doom_probe(const char *name, bool sound)
{
    rec_emit(name, "BEGIN", "version=1 public=%u", DOSVM_DPMI_PUBLIC);
    char base[24];
    int found = dosvm_workload_base(base);
    if (found) return fixture_absent(name, found);
    rec_emit(name, "DATA", "case=identity build=%s loops=%u sound=%u", CIUKI_BUILD_HEX8, GAME_LOOPS, sound);
    base_record(name, base);
    struct dosvm_ledger before;
    dosvm_snapshot(&before);
    const struct dosvm_config config = {.commitment = 0, .probe = name, .dpmi = true, .base = base};
    const char *command = sound ? "\\RUNREC.COM \\DOOM DOOM.EXE -config CIUKI.CFG -timedemo demo1"
                                : "\\RUNREC.COM \\DOOM DOOM.EXE -config CIUKI.CFG -nosound -timedemo demo1";
    struct dosvm *v = 0;
    unsigned passed = 0, files = 0;
    int error = storage_enable_write(storage_get(), 2);
    if (!error && !(error = wl_boot(&config, &v))) {
        bool bound = false, ignored = false;
        error = hash_workload(name, v, "DOOM", "DOOM.EXE", &files, &bound);
        if (!error) error = hash_payload(name, v, "RUNREC.COM", &ignored);
        rec_emit(name, "DATA", "case=extender path=/DOOM/DOOM.EXE kind=%s files=%u",
                 bound ? "bound_dos4gw_professional" : "none", files);
        if (!error && !bound) error = -ENOENT;     /* DOOM.EXE carries its own DOS/4GW */
        int x = wl_exit(&v);
        if (!error) error = x;
    }
    for (unsigned loop = 1; !error && loop <= GAME_LOOPS; loop++) {
        struct run_watch w;
        memset(&w, 0, sizeof(w));
        int e = wl_boot(&config, &v);
        if (!e) {
            dosvm_expect(v, "RUNREC");
            w.start = g_ticks;
            e = wl_type(v, command);
        }
        if (!e) e = run_wait(v, DOOM_LOOP_MS, &w);
        unsigned gametics = 0, realtics = 0, rc = 0, type = 0;
        bool timed = v && doom_timed(v, &gametics, &realtics);
        bool status = run_status(&w.info, &rc, &type);
        struct dosvm_audio_info a = {0};
        if (v) dosvm_audio_info(dosvm_key(v), &a);
        rec_emit(name, "DATA", "case=loop loop=%u dpmi_ms=%u launch_ms=%u run_ms=%u gametics=%u realtics=%u timed=%u status=%u rc=%u type=%u error=%d",
                 loop, (uint32_t)w.dpmi_ms, (uint32_t)w.flip_ms, (uint32_t)w.end_ms, gametics, realtics, timed, status, rc, type, e);
        rec_emit(name, "DATA", "case=video loop=%u graphics=%u width=%u height=%u flips=%u faults=%u pm_faults=%u maps=%u flushes=%u mode=%u",
                 loop, w.graphics, w.width, w.height, w.video.starts, w.video.faults, w.video.pm_faults,
                 w.video.maps, w.video.flushes, w.video.mode);
        rec_emit(name, "DATA", "case=dpmi loop=%u entries=%u terminations=%u exceptions=%u irq_pm=%u faults=%u stale=%u released=%u popf=%u",
                 loop, w.dpmi.stats.entries, w.dpmi.stats.terminations, w.dpmi.stats.exceptions, w.dpmi.stats.irqs_pm,
                 w.dpmi.stats.faults, w.dpmi.stats.stale, dpmi_released(&w.dpmi), w.dpmi.stats.if_restored);
        /* f3-05 counters: DSP resets, block boundaries, PCM frames, IRQ
         * edges, services, OPL events, DMA faults; consumer = the f4-05
         * native output attached to this VM (AC97 profiles), not a pass
         * condition: audible output is checked by the F4 cases. */
        rec_emit(name, "DATA", "case=audio loop=%u resets=%u blocks=%u frames=%u edges=%u services=%u opl=%u dma_faults=%u consumer=%u",
                 loop, a.resets, a.boundaries, (uint32_t)a.frames, a.irq_edges, a.services, (uint32_t)a.events, a.dma_faults,
                 a.consumer);
        if (!e && (!timed || !status || !w.graphics || !w.video.starts || !dpmi_released(&w.dpmi) ||
                   w.dpmi.stats.faults || w.dpmi.stats.stale || w.dpmi.stats.entries != w.dpmi.stats.terminations))
            e = -EFAULT;
        if (!e && sound && (!a.resets || !a.events)) e = -EFAULT;          /* SB detected, OPL music written */
        if (!e && !sound && (a.resets || a.events || a.frames)) e = -EFAULT; /* -nosound touches neither */
        if (e && v) probe_dos_screen(name, v);
        if (v) { int x = wl_exit(&v); if (!e) e = x; }
        struct dosvm_ledger now;
        dosvm_snapshot(&now);
        bool same = ledger_equal(&before, &now);
        rec_emit(name, "DATA", "case=loop_cleanup loop=%u restored=%u", loop, same);
        if (!e && !same) e = -ENOMEM;
        if (!e) passed++;
        error = e;
    }
    rec_emit(name, "DATA", "case=loops completed=%u required=%u", passed, GAME_LOOPS);
    if (v) { int x = dosvm_destroy(dosvm_key(v), true); if (!error) error = x; }
    if (!error && passed != GAME_LOOPS) error = -EFAULT;
    cleanup_record(name, &before, &error);
    rec_emit(name, "END", "status=%s error=%d", error ? "FAIL" : "PASS", error);
    return !!error;
}
static int probe_f3_doom(void) { return doom_probe("doom", false); }
static int probe_f3_doom_sound(void) { return doom_probe("doom-sound", true); }
CIUKI_F3_PROBE("doom", probe_f3_doom);
CIUKI_F3_PROBE("doom-sound", probe_f3_doom_sound);

/* ---- f3:wolf3d ---- */
/* Set-1 make codes (break = make | 80h) through the VM's 8042 and IRQ1, as
 * Wolf3D's own INT 9 handler reads port 60h (ID_IN.C). */
static bool wl_key(struct dosvm_key key, unsigned code)
{
    struct ciuki_input_event e = {.type = CIUKI_INPUT_KEY, .code = (int32_t)code, .value = 1};
    if (!dosvm_input_event(key, &e)) return false;
    task_sleep_ms(80);
    e.value = 0;
    bool ok = dosvm_input_event(key, &e);
    task_sleep_ms(80);
    return ok;
}
/* A Mode X/Y frame is stable when its digest stays the same for stable_ms
 * (sampled every 300 ms), differs from avoid and is not all black (fades,
 * cleared planes). The signon waits 6 s: its memory report is drawn before
 * the loading finishes and "Press a key" appears (WL_MAIN.C FinishSignon). */
static uint8_t black_frame[32];
static void black_digest(void)
{
    static const uint8_t zero[640u * 4u];     /* one XRGB row of the 640x480 presenter frame */
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    for (unsigned y = 0; y < 480; y++) sha256_update(&ctx, zero, sizeof(zero));
    sha256_final(&ctx, black_frame);
}
static int wolf_stable(struct dosvm *v, const uint8_t avoid[32], uint8_t digest[32], uint32_t ms, uint32_t stable_ms,
                       struct dosvm_frame *frame)
{
    uint64_t until = deadline_after_ms(ms), since = 0;
    uint8_t current[32];
    bool have = false;
    for (;;) {
        struct dosvm_info info;
        dosvm_info(v, &info);
        if (info.state >= DOSVM_STOPPING) return -V86_EIO;
        struct dosvm_video_info video;
        if (dosvm_video_info(dosvm_key(v), &video) && video.unchained && video.width == 320 &&
            !dosvm_frame_digest(v, 0, frame) && memcmp(frame->digest, black_frame, 32) &&
            (!avoid || memcmp(avoid, frame->digest, 32))) {
            if (!have || memcmp(current, frame->digest, 32)) {
                memcpy(current, frame->digest, 32);
                since = g_ticks;
                have = true;
            } else if (g_ticks - since >= stable_ms) {
                memcpy(digest, current, 32);
                return 0;
            }
        } else have = false;
        if (deadline_passed(until)) return -V86_ETIMEDOUT;
        task_sleep_ms(300);
    }
}
static int probe_f3_wolf3d(void)
{
    const char *name = "wolf3d";
    rec_emit(name, "BEGIN", "version=1 public=%u", DOSVM_DPMI_PUBLIC);
    char base[24];
    int found = dosvm_workload_base(base);
    if (found) return fixture_absent(name, found);
    rec_emit(name, "DATA", "case=identity build=%s loops=%u", CIUKI_BUILD_HEX8, GAME_LOOPS);
    base_record(name, base);
    black_digest();
    struct dosvm_ledger before;
    dosvm_snapshot(&before);
    /* Real-mode workload: no DPMI flag; XMS, EMS and UMBs of f3-03 offered. */
    const struct dosvm_config config = {.commitment = 0, .probe = name, .base = base};
    struct dosvm *v = 0;
    unsigned passed = 0, files = 0;
    int error = storage_enable_write(storage_get(), 2);
    if (!error && !(error = wl_boot(&config, &v))) {
        bool bound = false;
        error = hash_workload(name, v, "WOLF3D", "WOLF3D.EXE", &files, &bound);
        rec_emit(name, "DATA", "case=extender path=/WOLF3D/WOLF3D.EXE kind=%s files=%u", bound ? "dos4gw" : "real_mode", files);
        int x = wl_exit(&v);
        if (!error) error = x;
    }
    for (unsigned loop = 1; !error && loop <= GAME_LOOPS; loop++) {
        struct run_watch w;
        memset(&w, 0, sizeof(w));
        struct dosvm_frame frame = {0};
        uint8_t signon[32] = {0}, screen[32] = {0};
        char hex[65];
        unsigned screens = 0;
        int e = wl_boot(&config, &v);
        struct dosvm_key key = v ? dosvm_key(v) : (struct dosvm_key){0};
        if (!e) {
            dosvm_expect(v, "RUNREC");
            w.start = g_ticks;
            e = wl_type(v, "\\RUNREC.COM \\WOLF3D WOLF3D.EXE");
        }
        /* Signon "Press a key" (WL_MAIN.C FinishSignon, IN_Ack): the pinned
         * Mode X frame. Keys go through the 8042 from here on. */
        if (!e) e = wolf_stable(v, 0, signon, 180000, 6000, &frame);
        if (!e) { w.graphics = true; w.flip_ms = g_ticks - w.start; }   /* launch: signon shown */
        if (!e && !dosvm_input_focus(key, true)) e = -EFAULT;
        if (!e) {
            hex32(signon, hex);
            rec_emit(name, "DATA", "case=frame loop=%u stage=signon mode=%u width=%u height=%u digest=%s",
                     loop, frame.mode, frame.width, frame.height, hex);
            memcpy(screen, signon, 32);
        }
        /* Static screens up to the menu (NonShareware, PG13, title): 'N' at
         * each advances (IN_Ack/IN_UserInput take any key) and, once the
         * control panel is up, only moves its cursor to "New Game" (the
         * menu starts on "Read This!" in id's registered build: WL_MENU.C
         * STARTITEM, HandleMenu's initial-letter search). */
        for (unsigned step = 0; !e && step < 12; step++) {
            if (!wl_key(key, 0x31)) { e = -EFAULT; break; }
            uint8_t next[32];
            int s = wolf_stable(v, screen, next, 12000, 2000, &frame);
            if (s == -V86_ETIMEDOUT) break;                /* no new screen: the menu */
            if (s) { e = s; break; }
            memcpy(screen, next, 32);
            screens++;
            hex32(next, hex);
            rec_emit(name, "DATA", "case=frame loop=%u stage=%u mode=%u width=%u height=%u digest=%s",
                     loop, step + 1, frame.mode, frame.width, frame.height, hex);
        }
        /* New Game -> episode 1 -> default difficulty (WL_MENU.C CP_NewGame). */
        struct dosvm_video_info play0 = {0}, play1 = {0};
        for (unsigned i = 0; !e && i < 3; i++) {
            if (!wl_key(key, 0x1C)) e = -EFAULT;
            task_sleep_ms(2500);
        }
        /* SetupGameLevel, PreloadGraphics ("Get Psyched") and the fizzle-in
         * take an unknown time under TCG: play has begun once ThreeDRefresh
         * flips steadily (WL_DRAW.C, CRTC 0Ch each frame), at least 3 flips
         * in one second; the 3 s measurement starts there. */
        uint64_t play_wait = g_ticks;
        bool started = false;
        while (!e && !started && g_ticks - play_wait < WOLF_PLAY_START_MS) {
            struct dosvm_video_info a = {0}, b = {0};
            dosvm_video_info(key, &a);
            task_sleep_ms(1000);
            dosvm_video_info(key, &b);
            started = b.starts - a.starts >= 3;
        }
        uint32_t start_ms = (uint32_t)(g_ticks - play_wait);
        if (!e) {
            dosvm_video_info(key, &play0);
            task_sleep_ms(3000);
            dosvm_video_info(key, &play1);
        }
        uint32_t flips = play1.starts - play0.starts;
        bool playing = !e && started && flips >= 10 && play1.unchained && play1.width == 320;
        if (!e) {
            dosvm_frame_digest(v, 0, &frame);
            hex32(frame.digest, hex);
            rec_emit(name, "DATA", "case=play loop=%u playing=%u flips=%u start_ms=%u faults=%u burst=%u maps=%u digest=%s",
                     loop, playing, flips, start_ms, play1.faults - play0.faults, play1.burst - play0.burst,
                     play1.maps - play0.maps, hex);
        }
        /* F10 pops the quit dialog in play (WL_PLAY.C CheckKeys); Y confirms. */
        if (!e && !wl_key(key, 0x44)) e = -EFAULT;
        if (!e) { task_sleep_ms(1500); if (!wl_key(key, 0x15)) e = -EFAULT; }
        if (!e) e = run_wait(v, WOLF_LOOP_MS, &w);
        unsigned rc = 0, type = 0;
        bool status = run_status(&w.info, &rc, &type);
        rec_emit(name, "DATA", "case=loop loop=%u launch_ms=%u run_ms=%u screens=%u status=%u rc=%u type=%u xms_calls=%u ems_calls=%u error=%d",
                 loop, (uint32_t)w.flip_ms, (uint32_t)w.end_ms, screens, status, rc, type, w.info.xms_calls, w.info.ems_calls, e);
        if (!e && (!playing || !status || rc || type || !w.graphics)) e = -EFAULT;
        if (e && v) probe_dos_screen(name, v);
        if (v) { int x = wl_exit(&v); if (!e) e = x; }
        struct dosvm_ledger now;
        dosvm_snapshot(&now);
        bool same = ledger_equal(&before, &now);
        rec_emit(name, "DATA", "case=loop_cleanup loop=%u restored=%u", loop, same);
        if (!e && !same) e = -ENOMEM;
        if (!e) passed++;
        error = e;
    }
    rec_emit(name, "DATA", "case=loops completed=%u required=%u", passed, GAME_LOOPS);
    if (v) { int x = dosvm_destroy(dosvm_key(v), true); if (!error) error = x; }
    if (!error && passed != GAME_LOOPS) error = -EFAULT;
    cleanup_record(name, &before, &error);
    rec_emit(name, "END", "status=%s error=%d", error ? "FAIL" : "PASS", error);
    return !!error;
}
CIUKI_F3_PROBE("wolf3d", probe_f3_wolf3d);

/* ---- f3:vm-starve ---- */
static struct task *heartbeat(uint32_t *data)
{
    struct task *t = task_create_user("f3-starve-heartbeat", P_NORMAL, 0x400000, 0x800000, 0, 0, 0);
    if (!t) return 0;
    static const uint8_t code[] = {0xFF,0x05,0x00,0x00,0x50,0x00,0xEB,0xF8};  /* inc dword [500000h]; jmp */
    for (unsigned i = 0; i < 3; i++) {
        uint32_t p = pmm_alloc();
        if (!p) goto fail;
        memset(P2V(p), 0, PAGE_SIZE);
        uint32_t address = i == 0 ? 0x400000 : i == 1 ? 0x500000 : 0x7FF000;
        if (as_map(&t->as, address, p, PTE_U | (i ? PTE_W : 0))) { pmm_free(p); goto fail; }
        if (i == 0) memcpy(P2V(p), code, sizeof(code));
        if (i == 1) *data = p;
    }
    task_start(t);
    return t;
fail:
    task_kill(t, 0); task_reap(t); return 0;
}
struct starve_phase { uint64_t ms; uint32_t irq0, heartbeat, keys, key_ms; bool pass; };
/* One DPMI4GW run in VM a: IRQ0 deliveries per ms over the run, native
 * heartbeat increments, and 8 Left Shift make/break pairs injected through
 * the 8042 while the client runs (16 IRQ1 deliveries; Shift changes only
 * the BIOS shift state, so no key reaches the shell's line afterwards). */
static int starve_phase(struct dosvm *a, volatile uint32_t *beat, struct starve_phase *p)
{
    struct dosvm_key key = dosvm_key(a);
    struct dosvm_device_info d0 = {0}, d1 = {0};
    struct run_watch w;
    memset(&w, 0, sizeof(w));
    dosvm_device_info(key, &d0);
    uint32_t b0 = *beat;
    dosvm_expect(a, "DPMI4GW");
    w.start = g_ticks;
    int e = wl_type(a, "\\DPMI4GW.EXE");
    if (!e && !dosvm_input_focus(key, true)) e = -EFAULT;
    uint64_t start = g_ticks;
    for (unsigned i = 0; !e && i < 8; i++) {
        struct ciuki_input_event k = {.type = CIUKI_INPUT_KEY, .code = 0x2A, .value = 1};
        if (!dosvm_input_event(key, &k)) { e = -EFAULT; break; }
        k.value = 0;
        if (!dosvm_input_event(key, &k)) { e = -EFAULT; break; }
        task_sleep_ms(20);
    }
    uint64_t until = deadline_after_ms(3000);
    while (!e) {
        dosvm_device_info(key, &d1);
        if (d1.delivered[1] - d0.delivered[1] >= 16 || deadline_passed(until)) break;
        task_sleep_ms(1);
    }
    p->keys = d1.delivered[1] - d0.delivered[1];
    p->key_ms = (uint32_t)(g_ticks - start);
    dosvm_input_focus(key, false);
    if (!e) e = run_wait(a, 120000, &w);
    if (!e && (!w.info.verdict_pass || w.dpmi.stats.terminations != w.dpmi.stats.entries || !w.dpmi.stats.entries))
        e = -EFAULT;
    dosvm_device_info(key, &d1);
    p->ms = w.end_ms ? w.end_ms : 1;
    p->irq0 = d1.delivered[0] - d0.delivered[0];
    p->heartbeat = *beat - b0;
    p->pass = !e;
    return e;
}
static void phase_record(const char *phase, const struct starve_phase *p)
{
    rec_emit("vm-starve", "DATA", "case=phase phase=%s pass=%u ms=%u irq0=%u irq0_per_s=%u heartbeat=%u keys=%u key_ms=%u",
             phase, p->pass, (uint32_t)p->ms, p->irq0, (uint32_t)(p->irq0 * 1000ull / p->ms), p->heartbeat, p->keys, p->key_ms);
}
static int probe_f3_vm_starve(void)
{
    const char *name = "vm-starve";
    rec_emit(name, "BEGIN", "version=1 public=%u", DOSVM_DPMI_PUBLIC);
    rec_emit(name, "DATA", "case=identity build=%s", CIUKI_BUILD_HEX8);
    struct dosvm_ledger before;
    dosvm_snapshot(&before);
    /* 16 + 8 MiB: two admissions with the 32 MiB headroom fit 128 MiB guests. */
    const struct dosvm_config ca = {.commitment = 16u << 20, .probe = name, .dpmi = true};
    const struct dosvm_config cb = {.commitment = DOSVM_LOW_CAP, .probe = name};
    struct dosvm *a = 0, *b = 0;
    struct task *native = 0;
    uint32_t data = 0;
    struct starve_phase ref = {0}, loaded = {0};
    struct dosvm_device_info b0 = {0}, b1 = {0};
    int error = storage_enable_write(storage_get(), 2);
    if (!error && !(native = heartbeat(&data))) error = -ENOMEM;
    volatile uint32_t *beat = native ? (volatile uint32_t *)P2V(data) : 0;
    if (!error) error = wl_boot(&ca, &a);
    if (!error) { struct run_watch w; error = dos4gw_run(a, &w); }    /* warm-up */
    if (!error) error = starve_phase(a, beat, &ref);
    phase_record("reference", &ref);
    if (!error && !(error = wl_boot(&cb, &b))) {
        dosvm_expect(b, "CLILOOP");
        error = wl_type(b, "\\CLILOOP.COM");
        uint64_t until = deadline_after_ms(10000);
        struct dosvm_info info;
        while (!error) {
            dosvm_info(b, &info);
            if (info.verdict_done) break;
            if (info.state >= DOSVM_STOPPING) error = -V86_EIO;
            else if (deadline_passed(until)) error = -V86_ETIMEDOUT;
            else task_sleep_ms(1);
        }
        task_sleep_ms(200);
    }
    if (!error) {
        dosvm_device_info(dosvm_key(b), &b0);
        error = starve_phase(a, beat, &loaded);
        dosvm_device_info(dosvm_key(b), &b1);
    }
    phase_record("loaded", &loaded);
    struct dosvm_info bi = {0};
    if (b) dosvm_info(b, &bi);
    /* The CLI VM keeps running (preempted, never stopped) and takes no IRQ0. */
    bool spinning = b && bi.state < DOSVM_STOPPING && b1.delivered[0] == b0.delivered[0] && b1.pit_edges > b0.pit_edges;
    uint32_t timer_pct = ref.irq0 && ref.ms ? (uint32_t)((uint64_t)loaded.irq0 * ref.ms * 100 / ((uint64_t)ref.irq0 * loaded.ms)) : 0;
    uint32_t beat_pct = ref.heartbeat && ref.ms ? (uint32_t)((uint64_t)loaded.heartbeat * ref.ms * 100 / ((uint64_t)ref.heartbeat * loaded.ms)) : 0;
    rec_emit(name, "DATA", "case=rates timer_pct=%u heartbeat_pct=%u keys_ref=%u keys_loaded=%u key_ms_loaded=%u cli_vm_running=%u cli_vm_irq0=%u",
             timer_pct, beat_pct, ref.keys, loaded.keys, loaded.key_ms, spinning, b1.delivered[0] - b0.delivered[0]);
    if (!error && (!ref.pass || !loaded.pass || !spinning || ref.keys < 16 || loaded.keys < 16 ||
                   timer_pct < 50 || beat_pct < 20 || loaded.key_ms > 1000))
        error = -EFAULT;
    if (error && a) probe_dos_screen(name, a);
    if (b) { int x = dosvm_destroy(dosvm_key(b), true); if (!error) error = x; }
    if (a) { int x = wl_exit(&a); if (!error) error = x; }
    if (native) { task_kill(native, 0); task_reap(native); }
    cleanup_record(name, &before, &error);
    rec_emit(name, "END", "status=%s error=%d", error ? "FAIL" : "PASS", error);
    return !!error;
}
CIUKI_F3_PROBE("vm-starve", probe_f3_vm_starve);
