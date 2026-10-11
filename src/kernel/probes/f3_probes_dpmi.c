/* F3 DPMI row (directive f3-04, acceptance row "F3 DPMI" of
 * dos-dpmi-contract.md). Runtime-selected (f3:dpmi); VMs created with
 * dosvm_config.dpmi answer INT 2Fh/1687h while DOSVM_DPMI_PUBLIC is 0.
 *   1. DPMI16.COM and DPMI32.COM, each in a fresh VM: every INT 31h row with
 *      valid and invalid inputs, eight-plus nested callbacks/0302h, raw
 *      switches, exceptions, stale selectors, watchpoints, a PM IRQ0
 *      handler, a nested child client and cleanup. The kernel checks the
 *      payload verdict, the rows the host observed and the release of every
 *      DPMI resource after the client's exit.
 *   2. DPMI4GW.EXE: the pinned Open Watcom DOS/4GW client selects the kernel
 *      host (no HDPMI/VCPI exists), runs its tests and exits.
 *   3. A VM without the DPMI flag sees no host (gate), and DPMIFLT.COM
 *      faults at depth 2 in another VM: only that VM stops, unwound.
 *   4. SHA-256 of the clients and the extender, read from the VM's own disk
 *      (group=dos_payload, compared by the runner with the image manifest).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/probe.h>
#include <ciuki/dosvm.h>
#include <ciuki/dpmi.h>
#include <ciuki/sha256.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>
#include <ciuki/storage.h>

static const char *const name = "dpmi";

static bool at_prompt(struct dosvm *v)
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
/* Failure diagnostic: the guest text screen (spaces '_', '=' and
 * non-ASCII '.'), as the vm-boot/vm-memory controllers do. */
static void screen_dump(struct dosvm *v)
{
    const uint16_t *text = dosvm_text(v, 0);
    for (unsigned row = 0; row < 25; row++) {
        char line[81];
        unsigned len = 0;
        for (unsigned col = 0; col < 80; col++) {
            uint8_t ch = (uint8_t)text[row * 80 + col];
            line[col] = ch == ' ' || !ch ? '_' : ch > ' ' && ch < 127 && ch != '=' ? (char)ch : '.';
            if (ch > ' ' && ch < 127) len = col + 1;
        }
        if (!len) continue;
        line[len] = 0;
        rec_emit(name, "DATA", "group=screen row=%u text=%s", row, line);
    }
}
static int start_vm(bool dpmi, struct dosvm **out)
{
    const struct dosvm_config config = {.commitment = 0, .probe = "dpmi", .dpmi = dpmi};
    int e = dosvm_create(&config, out);
    if (e) return e;
    if ((e = dosvm_boot(*out))) return e;
    uint64_t until = deadline_after_ms(30000);
    for (;;) {
        struct dosvm_info info;
        dosvm_info(*out, &info);
        if (info.state >= DOSVM_STOPPING) return -V86_EIO;
        if (info.dos_ready && at_prompt(*out)) return 0;
        if (deadline_passed(until)) return -V86_ETIMEDOUT;
        task_sleep_ms(1);
    }
}
static int type_line(struct dosvm *v, const char *text)
{
    struct dosvm_key key = dosvm_key(v);
    for (unsigned i = 0; text[i]; i++)
        if (!dosvm_keyboard(key, (uint8_t)text[i])) return -ENOSPC;
    return dosvm_keyboard(key, 0x1C0D) ? 0 : -ENOSPC;
}
static int finish_vm(struct dosvm **v)
{
    struct dosvm_key key = dosvm_key(*v);
    int e = type_line(*v, "EXIT");
    uint64_t until = deadline_after_ms(5000);
    for (struct dosvm_info info; !e; task_sleep_ms(1)) {
        dosvm_info(*v, &info);
        if (info.state >= DOSVM_STOPPING) break;
        if (deadline_passed(until)) e = -V86_ETIMEDOUT;
    }
    int d = dosvm_destroy(key, e != 0);
    *v = 0;
    return e ? e : d;
}

/* Rows a 0.9 client must be able to use, and rows the host refuses (0.9
 * reserved, 0800h by the contract, 0A00h without a vendor API, 1.0-only). */
static const uint16_t must_ok[] = {
    0x0000, 0x0001, 0x0002, 0x0003, 0x0006, 0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D,
    0x0100, 0x0101, 0x0102, 0x0200, 0x0201, 0x0202, 0x0203, 0x0204, 0x0205,
    0x0300, 0x0301, 0x0302, 0x0303, 0x0304, 0x0305, 0x0306, 0x0400,
    0x0500, 0x0501, 0x0502, 0x0503, 0x0600, 0x0601, 0x0602, 0x0603, 0x0604, 0x0702, 0x0703,
    0x0900, 0x0901, 0x0902, 0x0B00, 0x0B01, 0x0B02, 0x0B03,
};
static const uint16_t must_refuse[] = {
    0x0004, 0x0005, 0x000E, 0x000F, 0x0210, 0x0211, 0x0212, 0x0213, 0x0401,
    0x0504, 0x0505, 0x0506, 0x0507, 0x0508, 0x0509, 0x050A, 0x050B,
    0x0700, 0x0701, 0x0800, 0x0801, 0x0A00, 0x0C00, 0x0C01, 0x0D00, 0x0D01, 0x0D02, 0x0D03, 0x0E00, 0x0E01,
};
/* Rows whose invalid-input refusals the NASM clients exercise. */
static const uint16_t must_invalid[] = {
    0x0000, 0x0001, 0x0002, 0x0006, 0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D,
    0x0100, 0x0101, 0x0102, 0x0202, 0x0203, 0x0205, 0x0300, 0x0301, 0x0302, 0x0303, 0x0304,
    0x0501, 0x0502, 0x0503, 0x0600, 0x0601, 0x0703, 0x0B00, 0x0B01, 0x0B02, 0x0B03,
};
static bool row_set(const uint32_t *bits, uint16_t ax)
{
    int r = dpmi_row(ax);
    return r >= 0 && (bits[r / 32] >> (r % 32) & 1);
}
static bool rows_all(const uint32_t *bits, const uint16_t *list, unsigned n, bool want)
{
    for (unsigned i = 0; i < n; i++)
        if (row_set(bits, list[i]) != want) return false;
    return true;
}

static bool released(const struct dpmi_info *d)
{
    return !d->active && !d->clients && !d->selectors && !d->blocks && !d->dos_blocks &&
           !d->callbacks && !d->watchpoints && !d->depth && !d->pool_used && !d->hstack;
}
static void release_record(const char *client, const struct dpmi_info *d)
{
    rec_emit(name, "DATA", "case=release client=%s released=%u active=%u clients=%u selectors=%u blocks=%u dos=%u callbacks=%u watch=%u depth=%u pool=%u hstack=%u",
             client, released(d), d->active, d->clients, d->selectors, d->blocks, d->dos_blocks, d->callbacks,
             d->watchpoints, d->depth, d->pool_used, d->hstack);
}
/* Runs one client at the prompt and waits for its verdict line and its exit
 * (every entered client terminated, the shell prompt back). */
static int run_client(struct dosvm *v, const char *client, const char *command, uint32_t ms,
                      struct dosvm_info *info, struct dpmi_info *d)
{
    dosvm_expect(v, client);
    int e = type_line(v, command);
    uint64_t until = deadline_after_ms(ms);
    while (!e) {
        dosvm_info(v, info);
        if (!dosvm_dpmi_info(dosvm_key(v), d)) { e = -EINVAL; break; }
        if (info->state >= DOSVM_STOPPING) { e = -V86_EIO; break; }
        if (info->verdict_done && !d->active && d->stats.terminations == d->stats.entries && at_prompt(v)) break;
        if (deadline_passed(until)) { e = -V86_ETIMEDOUT; break; }
        task_sleep_ms(1);
    }
    dosvm_info(v, info);
    dosvm_dpmi_info(dosvm_key(v), d);
    return e;
}

static int conformance(const char *client, const char *command)
{
    struct dosvm *v = 0;
    struct dosvm_info info = {0};
    struct dpmi_info d = {0};
    int e = start_vm(true, &v);
    if (!e) e = run_client(v, client, command, 120000, &info, &d);
    const struct dpmi_stats *s = &d.stats;
    bool ok_all = rows_all(s->rows_ok, must_ok, ARRAY_SIZE(must_ok), true);
    bool refused = rows_all(s->rows_ok, must_refuse, ARRAY_SIZE(must_refuse), false) &&
                   rows_all(s->rows_fail, must_refuse, ARRAY_SIZE(must_refuse), true);
    bool invalid = rows_all(s->rows_fail, must_invalid, ARRAY_SIZE(must_invalid), true);
    /* A record is at most 240 bytes with its 61-byte prefix: two rows. */
    rec_emit(name, "DATA", "case=client client=%s verdict=%u pass=%u detail=%s entries=%u entry_fails=%u terminations=%u error=%d",
             client, info.verdict_done, info.verdict_pass, info.verdict_detail[0] ? info.verdict_detail : "-",
             s->entries, s->entry_failures, s->terminations, e);
    rec_emit(name, "DATA", "case=counters client=%s exceptions=%u callbacks=%u raw=%u realcalls=%u max_depth=%u irq_pm=%u stale=%u faults=%u",
             client, s->exceptions, s->callbacks, s->raw_switches, s->realcalls, s->max_depth, s->irqs_pm, s->stale, s->faults);
    rec_emit(name, "DATA", "case=rows client=%s ok=%08x%08x%08x fail=%08x%08x%08x ok_all=%u refused=%u invalid=%u",
             client, s->rows_ok[2], s->rows_ok[1], s->rows_ok[0], s->rows_fail[2], s->rows_fail[1], s->rows_fail[0],
             ok_all, refused, invalid);
    release_record(client, &d);
    if (!e && (!info.verdict_pass || !ok_all || !refused || !invalid || !released(&d) || s->stale || s->faults)) e = -EFAULT;
    if (e && v) screen_dump(v);
    if (v) { int f = finish_vm(&v); if (!e) e = f; }
    return e;
}

static int dpmi16(void) { return conformance("DPMI16", "\\DPMI16.COM"); }
static int dpmi32(void) { return conformance("DPMI32", "\\DPMI32.COM"); }

static int dos4gw(void)
{
    struct dosvm *v = 0;
    struct dosvm_info info = {0};
    struct dpmi_info d = {0};
    int e = start_vm(true, &v);
    if (!e) e = run_client(v, "DPMI4GW", "\\DPMI4GW.EXE", 120000, &info, &d);
    const struct dpmi_stats *s = &d.stats;
    rec_emit(name, "DATA", "case=client client=DPMI4GW verdict=%u pass=%u detail=%s entries=%u entry_fails=%u terminations=%u error=%d",
             info.verdict_done, info.verdict_pass, info.verdict_detail[0] ? info.verdict_detail : "-",
             s->entries, s->entry_failures, s->terminations, e);
    rec_emit(name, "DATA", "case=counters client=DPMI4GW exceptions=%u callbacks=%u raw=%u realcalls=%u max_depth=%u irq_pm=%u stale=%u faults=%u",
             s->exceptions, s->callbacks, s->raw_switches, s->realcalls, s->max_depth, s->irqs_pm, s->stale, s->faults);
    rec_emit(name, "DATA", "case=dos4gw extender=/SYSTEM/DOS4GW.EXE version=1.97 redistribution=open_question");
    release_record("DPMI4GW", &d);
    if (!e && (!info.verdict_pass || s->entries < 1 || !released(&d) || s->stale || s->faults)) e = -EFAULT;
    if (e && v) screen_dump(v);
    if (v) { int f = finish_vm(&v); if (!e) e = f; }
    return e;
}

/* A VM without the flag: 1687h unanswered, the client reports stage 1.
 * The same VM is the witness while DPMIFLT.COM faults in another VM. */
static int gate_and_fault(void)
{
    struct dosvm *w = 0, *v = 0;
    struct dosvm_info info = {0}, wi = {0};
    struct dpmi_info d = {0};
    uint32_t sentinel = 0xD9A1F0C5, observed = 0;
    int e = start_vm(false, &w);
    if (!e) {
        dosvm_expect(w, "DPMI16");
        e = type_line(w, "\\DPMI16.COM");
        uint64_t until = deadline_after_ms(30000);
        while (!e) {
            dosvm_info(w, &wi);
            if (wi.state >= DOSVM_STOPPING) { e = -V86_EIO; break; }
            if (wi.verdict_done && at_prompt(w)) break;
            if (deadline_passed(until)) e = -V86_ETIMEDOUT;
            task_sleep_ms(1);
        }
        dosvm_dpmi_info(dosvm_key(w), &d);
    }
    bool gated = !e && wi.verdict_done && !wi.verdict_pass && !strncmp(wi.verdict_detail, "01_01", 6) && !d.stats.entries;
    rec_emit(name, "DATA", "case=gate unadvertised=%u verdict=%u pass=%u detail=%s entries=%u error=%d",
             gated, wi.verdict_done, wi.verdict_pass, wi.verdict_detail[0] ? wi.verdict_detail : "-", d.stats.entries, e);
    if (!e && !gated) e = -EFAULT;
    if (!e) e = dosvm_write(w, 0x180, &sentinel, 4);
    if (!e) e = start_vm(true, &v);
    if (!e) e = type_line(v, "\\DPMIFLT.COM");
    uint64_t until = deadline_after_ms(30000);
    while (!e) {
        dosvm_info(v, &info);
        if (info.state >= DOSVM_STOPPING) break;
        if (deadline_passed(until)) e = -V86_ETIMEDOUT;
        task_sleep_ms(1);
    }
    memset(&d, 0, sizeof(d));
    if (v) dosvm_dpmi_info(dosvm_key(v), &d);
    if (w) dosvm_info(w, &wi);
    bool witness = w && wi.state < DOSVM_STOPPING && !dosvm_read(w, 0x180, &observed, 4) && observed == sentinel && at_prompt(w);
    bool fault = !e && info.state == DOSVM_STOPPING && info.reason == DOSVM_FAULT && info.fault_vector == 13 &&
                 d.stats.faults == 1 && d.stats.max_depth >= 2;
    rec_emit(name, "DATA", "case=fault stopped=%u reason=%u vector=%u faults=%u max_depth=%u witness=%u error=%d",
             fault, info.reason, info.fault_vector, d.stats.faults, d.stats.max_depth, witness, e);
    if (!e && (!fault || !witness)) e = -EFAULT;
    if (e && w) screen_dump(w);
    if (v) { int f = dosvm_destroy(dosvm_key(v), true); if (!e) e = f; }
    if (w) { int f = finish_vm(&w); if (!e) e = f; }
    return e;
}

/* ---- payload hashes from the VM disk (FAT16, DOSBOOT.IMG geometry) ---- */
struct dfat { uint32_t fat, root, data; uint16_t root_entries; uint8_t spc; };
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t)le16(p + 2) << 16; }
static int dfat_open(struct dosvm *v, struct dfat *f, uint8_t *sector)
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
static int dfat_next(struct dosvm *v, const struct dfat *f, uint16_t cluster, uint16_t *next, uint8_t *sector)
{
    int e = dosvm_disk_read(v, f->fat + cluster / 256u, sector);
    if (e) return e;
    *next = le16(sector + (cluster % 256u) * 2);
    return 0;
}
/* Looks up an 8.3 name in the root (dir 0) or in a subdirectory chain. */
static int dfat_find(struct dosvm *v, const struct dfat *f, uint16_t dir, const char n83[11],
                    uint16_t *cluster, uint32_t *size, uint8_t *sector)
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
                if (!d[0]) return -EINVAL;
                if (d[0] == 0xE5 || (d[11] & 0x08) || memcmp(d, n83, 11)) continue;
                *cluster = le16(d + 26);
                *size = le32(d + 28);
                return 0;
            }
        }
        if (!dir) return -EINVAL;
        int e = dfat_next(v, f, c, &c, sector);
        if (e) return e;
        if (c < 2 || c >= 0xFFF8) return -EINVAL;
    }
    return -EINVAL;
}
static int dfat_hash(struct dosvm *v, const struct dfat *f, uint16_t cluster, uint32_t size,
                    uint8_t digest[32], uint8_t *sector, uint8_t *fat)
{
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    uint32_t left = size;
    while (left) {
        if (cluster < 2 || cluster >= 0xFFF8) return -V86_EIO;
        for (unsigned s = 0; s < f->spc && left; s++) {
            int e = dosvm_disk_read(v, f->data + (uint32_t)(cluster - 2) * f->spc + s, sector);
            if (e) return e;
            uint32_t n = left < 512 ? left : 512;
            sha256_update(&ctx, sector, n);
            left -= n;
        }
        if (left) { int e = dfat_next(v, f, cluster, &cluster, fat); if (e) return e; }
    }
    sha256_final(&ctx, digest);
    return 0;
}
static int hashes(void)
{
    static const struct { const char *path, *dir, *n83; } files[] = {
        {"/DPMI16.COM", 0, "DPMI16  COM"}, {"/DPMI32.COM", 0, "DPMI32  COM"},
        {"/DPMICH16.COM", 0, "DPMICH16COM"}, {"/DPMICH32.COM", 0, "DPMICH32COM"},
        {"/DPMIFLT.COM", 0, "DPMIFLT COM"}, {"/DPMI4GW.EXE", 0, "DPMI4GW EXE"},
        {"/SYSTEM/DOS4GW.EXE", "SYSTEM     ", "DOS4GW  EXE"},
    };
    struct dosvm *v = 0;
    uint8_t *sector = kmalloc(1024);
    if (!sector) return -ENOMEM;
    struct dfat f;
    unsigned hashed = 0;
    int e = start_vm(false, &v);
    if (!e) e = dfat_open(v, &f, sector);
    for (unsigned i = 0; !e && i < ARRAY_SIZE(files); i++) {
        uint16_t dir = 0, cluster = 0;
        uint32_t size = 0;
        uint8_t digest[32];
        if (files[i].dir) {
            uint32_t ignored;
            if ((e = dfat_find(v, &f, 0, files[i].dir, &dir, &ignored, sector))) break;
        }
        if ((e = dfat_find(v, &f, dir, files[i].n83, &cluster, &size, sector)) ||
            (e = dfat_hash(v, &f, cluster, size, digest, sector, sector + 512)))
            break;
        char hex[65];
        for (unsigned k = 0; k < 32; k++) {
            hex[2 * k] = "0123456789abcdef"[digest[k] >> 4];
            hex[2 * k + 1] = "0123456789abcdef"[digest[k] & 15];
        }
        hex[64] = 0;
        rec_emit(name, "DATA", "group=dos_payload path=%s size=%u sha256=%s", files[i].path, size, hex);
        hashed++;
    }
    rec_emit(name, "DATA", "case=hashes files=%u error=%d", hashed, e);
    kfree(sector);
    if (v) { int g = finish_vm(&v); if (!e) e = g; }
    return e;
}

static bool ledger_equal(const struct dosvm_ledger *a, const struct dosvm_ledger *b)
{
    return a->vms == b->vms && a->tasks == b->tasks && a->pages == b->pages &&
           a->handles == b->handles && a->devices == b->devices &&
           a->reserved == b->reserved && a->resident == b->resident;
}
static int probe_f3_dpmi(void)
{
    rec_emit(name, "BEGIN", "version=1 public=%u", DOSVM_DPMI_PUBLIC);
    struct dosvm_ledger before, after;
    dosvm_snapshot(&before);
    /* Every section cleans up its VMs, so later sections still run after a
     * failure (one QEMU run yields the whole row); the first error wins. */
    int error = storage_enable_write(storage_get(), 2);
    if (!error) {
        int (*const section[])(void) = { hashes, dpmi16, dpmi32, dos4gw, gate_and_fault };
        for (unsigned i = 0; i < ARRAY_SIZE(section); i++) {
            int e = section[i]();
            if (!error) error = e;
        }
    }
    dosvm_snapshot(&after);
    bool cleaned = ledger_equal(&before, &after);
    if (!cleaned && !error) error = -ENOMEM;
    rec_emit(name, "DATA", "case=cleanup vms=%u tasks=%u pages=%u handles=%u devices=%u restored=%u",
             after.vms, after.tasks, after.pages, after.handles, after.devices, cleaned);
    rec_emit(name, "END", "status=%s error=%d", error ? "FAIL" : "PASS", error);
    return !!error;
}
CIUKI_F3_PROBE("dpmi", probe_f3_dpmi);
