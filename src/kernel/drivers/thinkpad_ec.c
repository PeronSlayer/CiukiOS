/* Bounded ThinkPad console-volume monitor/control; no AML interpreter.
 * References: docs/design/f4-18-audio-levels.md and hardware-baseline.md F4-21.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/audio.h>
#include <ciuki/thinkpad_ec.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/task.h>
#include <ciuki/smi.h>

#define EC_DATA 0x62
#define EC_SC 0x66
#define EC_IBF 2
#define EC_OBF 1
#define EC_AUDIO 0x30
#define EC_MASK 0x4f
#define EC_TIMEOUT_MS 500u
#define EC_POLL_MS THINKPAD_EC_POLL_DEFAULT_MS
#define EC_REPROBE_MS 10000u
#define EC_FAILURE_LIMIT 8u

uint32_t thinkpad_ec_poll_option(const char *s, size_t size)
{
    /* Same token rules as the loader: separators are space, CR and LF (any
     * control byte here), the match is exact; anything else keeps 1 Hz. */
    size_t i = 0;
    while (i < size && s[i]) {
        while (i < size && s[i] && (unsigned char)s[i] <= 32) i++;
        size_t start = i;
        while (i < size && s[i] && (unsigned char)s[i] > 32) i++;
        if (i - start == 8 && !memcmp(s + start, "ecpoll=0", 8)) return 0;
    }
    return EC_POLL_MS;
}
static bool ec_sum(const uint8_t *p, size_t n)
{ uint8_t sum = 0; for (size_t i = 0; i < n; i++) sum += p[i]; return sum == 0; }
bool thinkpad_ec_entry(const uint8_t *p, size_t n, uint32_t *address, uint16_t *bytes)
{
    if (n < 31 || memcmp(p, "_SM_", 4) || p[5] < 31 || p[5] > n ||
        !ec_sum(p, p[5]) || memcmp(p + 16, "_DMI_", 5) || !ec_sum(p + 16, 15)) return false;
    *bytes = p[22] | (uint16_t)p[23] << 8;
    *address = p[24] | (uint32_t)p[25] << 8 | (uint32_t)p[26] << 16 | (uint32_t)p[27] << 24;
    return *bytes >= 6 && *address >= 0x500 && (uint64_t)*address + *bytes <= 0x100000000ull;
}
static const char *ec_string(const uint8_t *p, size_t formatted, size_t end, unsigned index)
{
    if (!index) return "";
    size_t at = formatted;
    while (at < end && p[at]) {
        size_t next = at; while (next < end && p[next]) next++;
        if (next == end) break;
        if (!--index) return (const char *)p + at;
        at = next + 1;
    }
    return "";
}
static void ec_string_copy(char out[48], const char *s)
{
    size_t i = 0;
    for (; i < 47 && s[i]; i++) {
        unsigned char c = s[i];
        out[i] = c < 32 || c > 126 || c == '"' || c == '\\' ? '?' : c;
    }
    out[i] = 0;
}
static bool ec_type_model(const char *s)
{
    size_t n = strlen(s);
    if (n < 4 || n > 11 || s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') return false;
    for (size_t i = 2; i < n; i++) {
        if (i == 4 && s[i] == '-') continue;
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'Z'))) return false;
    }
    return s[n - 1] != '-';
}
bool thinkpad_ec_smbios_decode(const uint8_t *table, size_t n, struct thinkpad_ec_smbios *s)
{
    memset(s, 0, sizeof(*s));
    s->verdict = "malformed_table";
    if (!table || !n) return false;
    for (size_t at = 0; at < n;) {
        const uint8_t *p = table + at;
        if (n - at < 6 || p[1] < 4 || p[1] > n - at - 2) return false;
        size_t end = p[1]; while (end + 1 < n - at && (p[end] || p[end + 1])) end++;
        if (end + 1 >= n - at) return false;
        if (p[0] == 0) {
            if (p[1] < 6 || s->has_bios) return false;
            s->has_bios = true;
            ec_string_copy(s->bios_vendor, ec_string(p, p[1], end + 1, p[4]));
            ec_string_copy(s->bios_version, ec_string(p, p[1], end + 1, p[5]));
        }
        if (p[0] == 1) {
            if (p[1] < 7 || s->has_system) return false;
            s->has_system = true;
            const char *vendor = ec_string(p, p[1], end + 1, p[4]);
            const char *product = ec_string(p, p[1], end + 1, p[5]);
            const char *version = ec_string(p, p[1], end + 1, p[6]);
            s->hint = !strncmp(product, "ThinkPad", 8) || !strncmp(version, "ThinkPad", 8) || ec_type_model(product);
            s->eligible = !strncmp(vendor, "IBM", 4) || (!strncmp(vendor, "LENOVO", 7) && s->hint);
            s->qemu = !strncmp(vendor, "QEMU", 5) || !strncmp(product, "QEMU", 4);
            ec_string_copy(s->vendor, vendor);
            ec_string_copy(s->product, product);
            ec_string_copy(s->version, version);
        }
        if (p[0] == 127) break;
        at += end + 2;
    }
    s->valid = true; s->verdict = "ok";
    return true;
}
bool thinkpad_ec_identity(const uint8_t *table, size_t n)
{
    struct thinkpad_ec_smbios s;
    return thinkpad_ec_smbios_decode(table, n, &s) && s.has_system && s.eligible && !s.qemu;
}
void thinkpad_ec_smbios_log(const struct thinkpad_ec_smbios *s)
{
    klog("[smbios] entry=%s address=%08x table=%08x bytes=%u", s->verdict,
         s->entry_phys, s->table_phys, s->table_bytes);
    klog("[smbios] version=%u.%u vendor=\"%s\" product=\"%s\" version=\"%s\" type1=%u",
         s->major, s->minor, s->vendor, s->product, s->version, s->has_system);
    klog("[smbios] bios vendor=\"%s\" version=\"%s\" type0=%u",
         s->bios_vendor, s->bios_version, s->has_bios);
}
bool thinkpad_ec_gate(const struct thinkpad_ec_smbios *s, uint32_t flags)
{
    const char *reason = NULL;
    if (flags & CBI_F_SAFE_MODE) reason = "safe_mode";
    else if ((flags & CBI_F_SMBIOS_QEMU) || s->qemu) reason = "qemu";
    else if (!s->valid || !s->has_system) reason = "no_smbios";
    else if (!s->eligible) reason = "not_ibm";
    klog("[ec] thinkpad hint=%u", s->hint);
    if (reason) { klog("[ec] thinkpad gate=%s", reason); return false; }
    return true;
}
/* One task operation shares a wall-clock deadline and sleeping-poll budget
 * across all its transfers. The latter also bounds a stopped clock. */
struct ec_budget { uint64_t start, end; unsigned pauses; };
static struct ec_budget ec_budget_begin(struct thinkpad_ec *e)
{
    uint64_t now = e->io.now(e->io.ctx);
    return (struct ec_budget){now, now + EC_TIMEOUT_MS, 0};
}
static int ec_timeout(struct thinkpad_ec *e, struct ec_budget *b, const char *stage)
{
    uint64_t now = e->io.now(e->io.ctx);
    if (!e->timeout_logged || (int64_t)(now - e->next_timeout_log) >= 0) {
        klog("[ec] thinkpad timeout stage=%s ms=%u", stage, (unsigned)(now - b->start));
        e->timeout_logged = true; e->next_timeout_log = now + 1000;
    }
    return -ETIMEDOUT;
}
static int ec_wait(struct thinkpad_ec *e, unsigned mask, unsigned value,
                   struct ec_budget *b, const char *stage)
{
    for (;;) {
        if ((int64_t)(e->io.now(e->io.ctx) - b->end) >= 0 || b->pauses >= EC_TIMEOUT_MS)
            return ec_timeout(e, b, stage);
        uint8_t status = e->io.read(e->io.ctx, EC_SC);
        if (status == 0xff) return -ENODEV;
        if ((status & mask) == value) return 0;
        /* Distinguish unsolicited OBF from IBF at transaction entry. */
        if (mask == (EC_IBF | EC_OBF)) stage = status & EC_IBF ? "ibf" : "obf";
        b->pauses++; e->io.pause(e->io.ctx);
    }
}
static int ec_transfer(struct thinkpad_ec *e, bool write, uint8_t *value, struct ec_budget *b)
{
    int err = 0;
    if (e->pending_read) {
        /* A late reply to our timed-out RD_EC must not be mistaken for a
         * new reply. Never drain unsolicited output. */
        err = ec_wait(e, EC_IBF, 0, b, "ibf");
        if (!err) {
            uint8_t status = e->io.read(e->io.ctx, EC_SC);
            if (status == 0xff) err = -ENODEV;
            else if (status & EC_OBF) (void)e->io.read(e->io.ctx, EC_DATA);
            /* An EC reset can lose the reply. Restart once IBF clears,
             * as Linux does, rather than waiting forever for old output. */
        }
    }
    if (!err) err = ec_wait(e, EC_IBF | EC_OBF, 0, b, "ibf");
    if (!err) {
        e->io.write(e->io.ctx, EC_SC, write ? 0x81 : 0x80);
        e->pending_read = false;
    }
    if (!err) err = ec_wait(e, EC_IBF, 0, b, write ? "write" : "read");
    if (!err) {
        e->io.write(e->io.ctx, EC_DATA, EC_AUDIO);
        if (!write) e->pending_read = true;
        err = ec_wait(e, EC_IBF, 0, b, write ? "write" : "read");
    }
    if (!err && write) {
        e->io.write(e->io.ctx, EC_DATA, *value);
        err = ec_wait(e, EC_IBF, 0, b, "write");
    } else if (!err) {
        err = ec_wait(e, EC_OBF, EC_OBF, b, "obf");
        if (!err) { *value = e->io.read(e->io.ctx, EC_DATA); e->pending_read = false; }
    }
    return err;
}
static int ec_read(struct thinkpad_ec *e, uint8_t *status, struct ec_budget *b)
{
    int err = ec_transfer(e, false, status, b);
    if (!err && (*status & 15) > 14) err = -EIO;
    return err;
}
static void ec_master(uint8_t status, uint8_t previous, struct ciuki_mixer_master *m)
{
    unsigned old = previous & 15u, level = status & 15u;
    unsigned up = (level + 15u - old) % 15u;
    int step = up == 1 ? 1 : up == 14 ? -1 : 0;
    unsigned master = (m->left + m->right) / 2u;
    if (step > 0) master = master > 95 ? 100 : master + 5;
    if (step < 0) master = master < 5 ? 0 : master - 5;
    m->left = m->right = master;
    m->mute = !!(status & 0x40);
}
static void ec_log(uint8_t status)
{ klog("[ec] thinkpad volume=%u mute=%u", status & 15u, !!(status & 0x40)); }
static int ec_set(struct thinkpad_ec *e, struct ciuki_mixer_master *m, struct ec_budget *b)
{
    uint8_t status = 0;
    int err = ec_read(e, &status, b);
    uint8_t desired = (status & ~0x40u) | (m->mute ? 0x40 : 0);
    if (!err && desired != status) err = ec_transfer(e, true, &desired, b);
    if (!err) err = ec_read(e, &status, b); /* prove the write; never invent state */
    if (!err && (status & EC_MASK) != (desired & EC_MASK)) err = -EIO;
    if (!err) {
        if ((e->status & EC_MASK) != (status & EC_MASK)) ec_log(status);
        e->status = status;
        m->mute = !!(status & 0x40);
    }
    return err;
}
static void ec_result(struct thinkpad_ec *e, int err)
{
    uint64_t now = e->io.now(e->io.ctx);
    /* Retries follow the poll cadence; a disabled poll keeps the default. */
    uint64_t poll = e->poll_ms ? e->poll_ms : EC_POLL_MS;
    if (!err) { e->active = e->available = true; e->failures = 0; e->next_poll = now + poll; return; }
    e->available = false;
    if (e->failures < EC_FAILURE_LIMIT) e->failures++;
    if (e->active && e->failures == EC_FAILURE_LIMIT) {
        e->active = false;
        klog("[ec] thinkpad disabled error=%d", err);
    }
    e->next_poll = now + (e->active ? poll : EC_REPROBE_MS);
}
int thinkpad_ec_volume_set(struct thinkpad_ec *e, struct ciuki_mixer_master *m)
{
    if (m->left > 100 || m->right > 100 || m->mute > 1) return -EINVAL;
    if (!e->active || !e->available) return -ENODEV;
    struct ec_budget b = ec_budget_begin(e);
    int err = ec_set(e, m, &b);
    ec_result(e, err);
    return err;
}
static int ec_discover(struct thinkpad_ec *e, struct ciuki_mixer_master *m, struct ec_budget *b)
{
    uint8_t status, again;
    int err = ec_read(e, &status, b);
    if (!err) err = ec_read(e, &again, b);
    if (!err && again != status) err = -EIO;
    if (!err) err = ec_set(e, m, b);
    return err;
}
int thinkpad_ec_probe(struct thinkpad_ec *e, const struct thinkpad_ec_io *io, bool identified, struct ciuki_mixer_master *m)
{
    memset(e, 0, sizeof(*e));
    if (!identified || !io || !io->read || !io->write || !io->now || !io->pause) return -ENODEV;
    e->io = *io;
    e->poll_ms = EC_POLL_MS;
    struct ec_budget b = ec_budget_begin(e);
    int err = ec_discover(e, m, &b);
    if (!err) { ec_result(e, 0); ec_log(e->status); }
    else {
        e->next_poll = io->now(io->ctx) + EC_REPROBE_MS;
        klog("[ec] thinkpad gate=ec_handshake_failed stage=probe error=%d", err);
        /* Only a successfully qualified/claimed native device is monitored. */
        memset(&e->io, 0, sizeof(e->io));
    }
    return err;
}
int thinkpad_ec_volume_poll(struct thinkpad_ec *e, struct ciuki_mixer_master *m)
{
    if (!e->io.now) return -ENODEV;
    if (!e->poll_ms) return e->available ? 0 : -ENODEV; /* ecpoll=0: no EC traffic */
    uint64_t now = e->io.now(e->io.ctx);
    if ((int64_t)(now - e->next_poll) < 0) return e->available ? 0 : -ENODEV;
    bool recovering = !e->available, disabled = !e->active;
    struct ec_budget b = ec_budget_begin(e);
    uint8_t status = e->status;
    /* Restore the saved/current preference before delegating master again. */
    int err = disabled ? ec_discover(e, m, &b) : recovering ? ec_set(e, m, &b) : ec_read(e, &status, &b);
    ec_result(e, err);
    if (err) return err;
    if (disabled) klog("[ec] thinkpad resumed");
    if (recovering) return 0;
    if ((status & EC_MASK) == (e->status & EC_MASK)) return 0;
    uint8_t previous = e->status;
    e->status = status; ec_master(status, previous, m); ec_log(status); return 1;
}

#ifndef THINKPAD_EC_HOST
static struct thinkpad_ec native_ec;
static struct kmutex ec_lock;
/* f1-94: every EC port access is a candidate SMI trigger in legacy mode. */
static uint8_t native_read(void *ctx, uint16_t port)
{ (void)ctx; if (smi_mark) smi_mark(SMI_EC_IO); return inb(port); }
static void native_write(void *ctx, uint16_t port, uint8_t v)
{ (void)ctx; if (smi_mark) smi_mark(SMI_EC_IO); outb(port, v); }
static uint64_t native_now(void *ctx) { (void)ctx; return g_ticks; }
static void native_pause(void *ctx) { (void)ctx; task_sleep_ms(1); }
static bool native_table_reserved(uint32_t address, uint16_t length)
{
    /* The boot map is validated, sorted and non-overlapping. Require every
     * byte of the rounded pages, not just the structure bytes, to be type 2.
     * In particular a PMM-used bit alone cannot prove firmware ownership. */
    uint64_t at = PAGE_ALIGN_DOWN(address);
    uint64_t end = ((uint64_t)address + length + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    for (unsigned i = 0; i < g_boot.e820_count; i++) {
        const struct ciuki_e820 *e = &g_boot.e820[i];
        uint64_t limit = e->base + e->length;
        if (limit <= at) continue;
        if (e->base > at || e->type != 2) return false;
        if (limit >= end) return true;
        at = limit;
    }
    return false;
}
static void native_table_decode(uint32_t address, uint16_t length, struct thinkpad_ec_smbios *s)
{
    bool mapped = (uint64_t)address + length <= (uint64_t)PMM_MAX_PAGES * PAGE_SIZE;
    if (mapped) {
        for (uint32_t p = PAGE_ALIGN_DOWN(address); p < address + length; p += PAGE_SIZE) {
            uint32_t pte = vmm_kernel_pte(KERNEL_VBASE + p);
            if (!(pte & PTE_P) || (pte & ~(PAGE_SIZE - 1)) != p) { mapped = false; break; }
        }
    }
    if (mapped) { thinkpad_ec_smbios_decode(P2V(address), length, s); return; }
    const uint8_t *table = native_table_reserved(address, length) ? vmm_map_physical_ro(address, length) : NULL;
    if (table) {
        thinkpad_ec_smbios_decode(table, length, s);
        vmm_revoke_mmio((void *)table, length);
    } else {
        memset(s, 0, sizeof(*s)); s->verdict = "unmapped_table";
    }
}
static void native_identity(struct thinkpad_ec_smbios *s)
{
    memset(s, 0, sizeof(*s)); s->verdict = "not_found";
    for (uint32_t at = 0xf0000; at <= 0x100000 - 31; at += 16) {
        const uint8_t *entry = P2V(at);
        if (memcmp(entry, "_SM_", 4)) continue;
        uint32_t address; uint16_t length;
        if (!thinkpad_ec_entry(entry, 0x100000 - at, &address, &length)) {
            if (!s->entry_phys) { s->entry_phys = at; s->verdict = "bad_entry"; }
            continue;
        }
        native_table_decode(address, length, s);
        s->entry_phys = at; s->table_phys = address; s->table_bytes = length;
        s->major = entry[6]; s->minor = entry[7];
        if (s->valid) return;
    }
}
void thinkpad_ec_init(void)
{
    struct thinkpad_ec_smbios s;
    native_identity(&s);
    thinkpad_ec_smbios_log(&s);
    if (!thinkpad_ec_gate(&s, g_boot.flags)) return;
    kmutex_init(&ec_lock);
    int data = registry_claim(RES_PORT, EC_DATA, EC_DATA + 1, "thinkpad-ec", false);
    if (data < 0) {
        klog("[ec] thinkpad gate=ec_handshake_failed stage=claim_data error=%d", data); return;
    }
    int command = registry_claim(RES_PORT, EC_SC, EC_SC + 1, "thinkpad-ec", false);
    /* Keep claims quarantined on a partial claim/protocol failure: no other
     * driver may issue a different command into an ambiguous transaction. */
    gen_t dg = registry_get(data)->generation;
    if (command < 0) {
        registry_quarantine(data, dg);
        klog("[ec] thinkpad gate=ec_handshake_failed stage=claim_command error=%d", command); return;
    }
    gen_t cg = registry_get(command)->generation;
    struct thinkpad_ec_io io = {0, native_read, native_write, native_now, native_pause};
    int err = thinkpad_ec_probe(&native_ec, &io, true, &audio.master);
    bool probed = !err;
    if (!err) err = registry_activate(data, dg);
    if (!err) err = registry_activate(command, cg);
    if (err) {
        native_ec.active = native_ec.available = false;
        memset(&native_ec.io, 0, sizeof(native_ec.io));
        registry_quarantine(data, dg); registry_quarantine(command, cg);
        if (probed) klog("[ec] thinkpad gate=ec_handshake_failed stage=activate error=%d", err);
        return;
    }
    audio.master_in_hardware = true;
}
int thinkpad_ec_master_set(struct ciuki_mixer_master *m)
{
    if (!native_ec.io.now) return -ENODEV;
    kmutex_lock(&ec_lock); int err = thinkpad_ec_volume_set(&native_ec, m);
    audio.master_in_hardware = native_ec.available;
    kmutex_unlock(&ec_lock); return err;
}
int thinkpad_ec_master_poll(struct ciuki_mixer_master *m)
{
    if (!native_ec.io.now) return -ENODEV;
    kmutex_lock(&ec_lock); int err = thinkpad_ec_volume_poll(&native_ec, m);
    audio.master_in_hardware = native_ec.available;
    kmutex_unlock(&ec_lock); return err;
}
static void ec_worker(void *arg)
{ (void)arg; while (native_ec.io.now && native_ec.poll_ms) { task_sleep_ms(native_ec.poll_ms); audio_master_refresh(); } }
int thinkpad_ec_poll_state(void)
{ return native_ec.io.now ? (int)native_ec.poll_ms : -1; }
void thinkpad_ec_start(void)
{
    if (!native_ec.active) return;
    /* f1-94 amendment 1: 1 Hz unless BOOT.CFG says ecpoll=0. The EC keeps
     * driving the amplifier from the keys; only the mixer mirror stops. */
    native_ec.poll_ms = thinkpad_ec_poll_option(g_boot.options, sizeof(g_boot.options));
    klog("[ec] thinkpad poll_ms=%u source=%s", native_ec.poll_ms,
         native_ec.poll_ms ? "default" : "ecpoll=0");
    if (!native_ec.poll_ms) return;
    struct task *t = task_create_kernel("thinkpad-volume", ec_worker, 0, P_DEVICE);
    if (t) { task_start(t); audio.commitment_pages += KSTACK_SIZE / PAGE_SIZE; }
    else klog("[ec] thinkpad poller unavailable error=%d", -ENOMEM);
}
#endif
