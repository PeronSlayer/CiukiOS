/* Bounded ThinkPad console-volume monitor/control; no AML interpreter.
 * References: docs/design/f4-18-audio-levels.md and hardware-baseline.md F4-21.
 * f1-100: every EC transaction raises the T23 EC event line (GPI12, routed
 * to SMI# in legacy mode) and costs about two 2.1 ms SMIs; EC traffic is
 * therefore explicit, counted and bounded (docs/validation/f1-100-ec-traffic.md).
 * f2-96: the default poll is one RD_EC every 2 s, so the mixer follows the
 * volume keys again (docs/validation/f2-96-cursor-ec-volume.md).
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
#define EC_POLL_MS THINKPAD_EC_POLL_MS
#define EC_DEFAULT_POLL_MS THINKPAD_EC_DEFAULT_POLL_MS
#define EC_REFRESH_MS THINKPAD_EC_REFRESH_MS
#define EC_SYNC_MS THINKPAD_EC_SYNC_MS
#define EC_REPROBE_MS 10000u
#define EC_FAILURE_LIMIT 8u
#define EC_OP_TRANSACTIONS THINKPAD_EC_OP_TRANSACTIONS
/* Port accesses of one operation: each transfer makes at most five waits and
 * five non-status accesses, and every further status read follows one of the
 * operation's EC_TIMEOUT_MS sleeping polls. */
#define EC_OP_IO_BOUND (EC_TIMEOUT_MS + 10u * EC_OP_TRANSACTIONS)
#ifndef THINKPAD_EC_HOST
static uint8_t native_read(void *, uint16_t);
extern int acpi_ec_quirk_transfer(bool, uint8_t, uint8_t *) __attribute__((weak));
extern int acpi_ec_quirk_transfer_counted(bool, uint8_t, uint8_t *, unsigned, unsigned *) __attribute__((weak));
extern int acpi_ec_quirk_attach(void) __attribute__((weak));
extern bool acpi_ec_quirk_legacy(void) __attribute__((weak));
#endif

uint32_t thinkpad_ec_poll_option(const char *s, size_t size, const char **source)
{
    /* Same token rules as the loader: separators are space, CR and LF (any
     * control byte here), the match is exact. f2-96: 2 s by default, 1 Hz
     * with ecpoll=1, off with ecpoll=0, which wins like a positive
     * safe-mode source. */
    bool off = false, on = false;
    size_t i = 0;
    while (i < size && s[i]) {
        while (i < size && s[i] && (unsigned char)s[i] <= 32) i++;
        size_t start = i;
        while (i < size && s[i] && (unsigned char)s[i] > 32) i++;
        if (i - start == 8 && !memcmp(s + start, "ecpoll=", 7) && (s[start + 7] == '0' || s[start + 7] == '1')) {
            if (s[start + 7] == '0') off = true; else on = true;
        }
    }
    if (source) *source = off ? "ecpoll=0" : on ? "ecpoll=1" : "default";
    return off ? 0 : on ? EC_POLL_MS : EC_DEFAULT_POLL_MS;
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
            if (p[1] > 8) ec_string_copy(s->bios_date, ec_string(p, p[1], end + 1, p[8]));
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
 * across all its transfers. The latter also bounds a stopped clock. f1-100:
 * it also counts the operation's port accesses and transactions, and refuses
 * a transaction beyond EC_OP_TRANSACTIONS. */
struct ec_budget { uint64_t start, end; unsigned pauses, io, transactions; bool refused; };
static struct ec_budget ec_budget_begin(struct thinkpad_ec *e)
{
    uint64_t now = e->io.now(e->io.ctx);
    return (struct ec_budget){now, now + EC_TIMEOUT_MS, 0, 0, 0, false};
}
static uint8_t ec_in(struct thinkpad_ec *e, struct ec_budget *b, uint16_t port)
{ b->io++; return e->io.read(e->io.ctx, port); }
static void ec_out(struct thinkpad_ec *e, struct ec_budget *b, uint16_t port, uint8_t value)
{ b->io++; e->io.write(e->io.ctx, port, value); }
/* Publish one finished operation (the window record runs in another task). */
static void ec_end(struct thinkpad_ec *e, const struct ec_budget *b)
{
    __atomic_fetch_add(&e->window_io, b->io, __ATOMIC_RELAXED);
    __atomic_fetch_add(&e->window_transactions, b->transactions, __ATOMIC_RELAXED);
    __atomic_fetch_add(&e->total_io, b->io, __ATOMIC_RELAXED);
    __atomic_fetch_add(&e->total_transactions, b->transactions, __ATOMIC_RELAXED);
    if (b->refused) __atomic_fetch_add(&e->window_refused, 1u, __ATOMIC_RELAXED);
    uint32_t max = __atomic_load_n(&e->window_max_io, __ATOMIC_RELAXED);
    while (b->io > max && !__atomic_compare_exchange_n(&e->window_max_io, &max, b->io, false,
                                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
bool thinkpad_ec_window(struct thinkpad_ec *e, char *out, size_t size)
{
    uint32_t io = __atomic_exchange_n(&e->window_io, 0u, __ATOMIC_RELAXED);
    uint32_t transactions = __atomic_exchange_n(&e->window_transactions, 0u, __ATOMIC_RELAXED);
    uint32_t max_io = __atomic_exchange_n(&e->window_max_io, 0u, __ATOMIC_RELAXED);
    uint32_t refused = __atomic_exchange_n(&e->window_refused, 0u, __ATOMIC_RELAXED);
    uint32_t requests = __atomic_exchange_n(&e->window_requests, 0u, __ATOMIC_RELAXED);
    if (!io && !transactions && !refused && !requests) return false;
    ksnprintf(out, size, "[ec] thinkpad io=%u transactions=%u max_io=%u refused=%u requests=%u poll_ms=%u total_io=%u total_transactions=%u",
              io, transactions, max_io, refused, requests, e->poll_ms,
              __atomic_load_n(&e->total_io, __ATOMIC_RELAXED),
              __atomic_load_n(&e->total_transactions, __ATOMIC_RELAXED));
    return true;
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
        uint8_t status = ec_in(e, b, EC_SC);
        if (status == 0xff) return -ENODEV;
        if ((status & mask) == value) return 0;
        /* Distinguish unsolicited OBF from IBF at transaction entry. */
        if (mask == (EC_IBF | EC_OBF)) stage = status & EC_IBF ? "ibf" : "obf";
        b->pauses++; e->io.pause(e->io.ctx);
    }
}
static int ec_transfer(struct thinkpad_ec *e, bool write, uint8_t *value, struct ec_budget *b)
{
#ifndef THINKPAD_EC_HOST
    if (e->io.read == native_read) {
        if (b->transactions >= EC_OP_TRANSACTIONS) { b->refused = true; return -EBUSY; }
        uint64_t now = e->io.now(e->io.ctx);
        if (now >= b->end) return -ETIMEDOUT;
        b->transactions++;
        if (acpi_ec_quirk_transfer_counted) {
            unsigned cost = 0;
            int err = acpi_ec_quirk_transfer_counted(write, EC_AUDIO, value, (unsigned)(b->end-now), &cost);
            b->io += cost; return err;
        }
        return acpi_ec_quirk_transfer ? acpi_ec_quirk_transfer(write, EC_AUDIO, value) : -ENODEV;
    }
#endif
    /* f1-100: no operation issues more transactions than its bound. */
    if (b->transactions >= EC_OP_TRANSACTIONS) { b->refused = true; return -EBUSY; }
    int err = 0;
    if (e->pending_read) {
        /* A late reply to our timed-out RD_EC must not be mistaken for a
         * new reply. Never drain unsolicited output. */
        err = ec_wait(e, EC_IBF, 0, b, "ibf");
        if (!err) {
            uint8_t status = ec_in(e, b, EC_SC);
            if (status == 0xff) err = -ENODEV;
            else if (status & EC_OBF) (void)ec_in(e, b, EC_DATA);
            /* An EC reset can lose the reply. Restart once IBF clears,
             * as Linux does, rather than waiting forever for old output. */
        }
    }
    if (!err) err = ec_wait(e, EC_IBF | EC_OBF, 0, b, "ibf");
    if (!err) {
        ec_out(e, b, EC_SC, write ? 0x81 : 0x80);
        b->transactions++;
        e->pending_read = false;
    }
    if (!err) err = ec_wait(e, EC_IBF, 0, b, write ? "write" : "read");
    if (!err) {
        ec_out(e, b, EC_DATA, EC_AUDIO);
        if (!write) e->pending_read = true;
        err = ec_wait(e, EC_IBF, 0, b, write ? "write" : "read");
    }
    if (!err && write) {
        ec_out(e, b, EC_DATA, *value);
        err = ec_wait(e, EC_IBF, 0, b, "write");
    } else if (!err) {
        err = ec_wait(e, EC_OBF, EC_OBF, b, "obf");
        if (!err) { *value = ec_in(e, b, EC_DATA); e->pending_read = false; }
    }
    return err;
}
static int ec_read(struct thinkpad_ec *e, uint8_t *status, struct ec_budget *b)
{
    int err = ec_transfer(e, false, status, b);
    if (!err && (*status & 15) > 14) err = -EIO;
    return err;
}
/* f4-29: five master points per key press. fx-3: the EC level is a
 * saturating 0..14 counter (Linux v6.12 thinkpad_acpi.c: TP_EC_VOLUME_MAX
 * 14; issue_volchange sends one key per level step between two samples; the
 * firmware keeps the level at its bounds), so a 2 s poll that sees several
 * presses applies every step instead of dropping them; no 14<->0 wrap. */
static void ec_master(uint8_t status, uint8_t previous, struct ciuki_mixer_master *m)
{
    int steps = (int)(status & 15u) - (int)(previous & 15u);
    int master = (int)(m->left + m->right) / 2 + 5 * steps;
    master = master < 0 ? 0 : master > 100 ? 100 : master;
    m->left = m->right = (unsigned)master;
    m->mute = !!(status & 0x40);
}
static void ec_log(uint8_t status)
{ klog("[ec] thinkpad volume=%u mute=%u", status & 15u, !!(status & 0x40)); }
/* fx-3 follow-up (lead decision 2026-10-23): the system master and the
 * console amplifier are one volume. System 0..100 -> EC 0..14 rounds to
 * the nearest level, (master * 14 + 50) / 100: 0->0, 5->1, 50->7, 95->13,
 * 100->14; only 0 maps to 0 and only 100 to 14, so a key press is always
 * visible in register 30h except at the true bounds. EC -> system stays
 * five points per level step (f4-29). Linux v6.12 thinkpad_acpi.c
 * __volume_set_volume_ec / __volume_set_mute_ec: read register 30h, replace
 * bits 0-3 (level, maximum 14) or bit 6 (mute), keep the other bits, write
 * only when the byte changes. */
unsigned thinkpad_ec_level(unsigned master) { return ((master > 100 ? 100 : master) * 14u + 50u) / 100u; }
static uint8_t ec_desired(const struct thinkpad_ec *e, uint8_t status, const struct ciuki_mixer_master *m)
{
    uint8_t want = (status & ~0x40u) | (m->mute ? 0x40 : 0);
    if (e->level_sync) want = (want & ~15u) | (uint8_t)thinkpad_ec_level((m->left + m->right) / 2u);
    return want;
}
static void ec_accept(struct thinkpad_ec *e, uint8_t status, struct ciuki_mixer_master *m)
{
    if ((e->status & EC_MASK) != (status & EC_MASK)) ec_log(status);
    e->status = status;
    m->mute = !!(status & 0x40);
}
/* WR_EC of the shared preference (mute; with level_sync also the level),
 * proved by a read-back; never invent state. */
static int ec_write_mute(struct thinkpad_ec *e, uint8_t desired, struct ciuki_mixer_master *m, struct ec_budget *b)
{
    uint8_t status = 0;
    int err = ec_transfer(e, true, &desired, b);
    if (!err) err = ec_read(e, &status, b);
    if (!err && (status & EC_MASK) != (desired & EC_MASK)) err = -EIO;
    if (!err) ec_accept(e, status, m);
    return err;
}
/* f1-100: one read; the write and its proof only when the shared mute
 * preference differs from the register (one transaction or three). */
static int ec_set(struct thinkpad_ec *e, struct ciuki_mixer_master *m, struct ec_budget *b)
{
    uint8_t status = 0;
    int err = ec_read(e, &status, b);
    if (err) return err;
    uint8_t desired = ec_desired(e, status, m);
    if (desired == status) { ec_accept(e, status, m); e->sync_pending = false; return 0; }
    err = ec_write_mute(e, desired, m, b);
    if (!err) e->sync_pending = false;
    return err;
}
static void ec_result(struct thinkpad_ec *e, int err)
{
    uint64_t now = e->io.now(e->io.ctx);
    /* Retries follow the poll cadence (2 s by default, 1 s with ecpoll=1);
     * with the poll off an explicit action retries after 1 s. */
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
/* f1-100: the boot probe is one RD_EC when the register already holds the
 * mute preference; only before a first write must it read the same twice. */
static int ec_discover(struct thinkpad_ec *e, struct ciuki_mixer_master *m, struct ec_budget *b)
{
    uint8_t status = 0, again = 0;
    int err = ec_read(e, &status, b);
    if (err) return err;
    uint8_t desired = ec_desired(e, status, m);
    if (desired == status) { ec_accept(e, status, m); return 0; }
    err = ec_read(e, &again, b);
    if (!err && again != status) err = -EIO;
    if (!err) err = ec_write_mute(e, desired, m, b);
    return err;
}
int thinkpad_ec_volume_set(struct thinkpad_ec *e, struct ciuki_mixer_master *m)
{
    if (m->left > 100 || m->right > 100 || m->mute > 1) return -EINVAL;
    if (!e->io.now) return -ENODEV;
    bool recovering = !e->available, disabled = !e->active;
    if (recovering) {
        /* f1-100: without a poll an explicit change is also the retry, at
         * the failure cadence (1 s, 10 s after eight failures). */
        if ((int64_t)(e->io.now(e->io.ctx) - e->next_poll) < 0) return -ENODEV;
    } else if (!!(e->status & 0x40) == !!m->mute) {
        /* Level only: no EC access here (f1-100). fx-3 follow-up: with
         * level_sync the worker writes the level once, EC_SYNC_MS after
         * the last change (a slider drag is one write). */
        if (e->level_sync && (e->status & 15u) != thinkpad_ec_level((m->left + m->right) / 2u)) {
            e->sync_pending = true; e->sync_due = e->io.now(e->io.ctx) + EC_SYNC_MS;
        }
        return 0;
    }
    struct ec_budget b = ec_budget_begin(e);
    int err = disabled ? ec_discover(e, m, &b) : ec_set(e, m, &b);
    ec_result(e, err);
    ec_end(e, &b);
    if (!err && disabled) klog("[ec] thinkpad resumed");
    return err;
}
int thinkpad_ec_probe(struct thinkpad_ec *e, const struct thinkpad_ec_io *io, bool identified, struct ciuki_mixer_master *m)
{
    memset(e, 0, sizeof(*e));
    if (!identified || !io || !io->read || !io->write || !io->now || !io->pause) return -ENODEV;
    e->io = *io;
    e->poll_ms = 0;      /* thinkpad_ec_start applies the default or ecpoll= */
    e->status = 0xff;    /* no valid register value: the first read is logged once */
    struct ec_budget b = ec_budget_begin(e);
    int err = ec_discover(e, m, &b);
    ec_end(e, &b);
    if (!err) ec_result(e, 0);
    else {
        e->next_poll = io->now(io->ctx) + EC_REPROBE_MS;
        klog("[ec] thinkpad gate=ec_handshake_failed stage=probe error=%d", err);
        /* Only a successfully qualified/claimed native device is monitored. */
        memset(&e->io, 0, sizeof(e->io));
    }
    return err;
}
static int ec_mirror(struct thinkpad_ec *e, struct ciuki_mixer_master *m)
{
    bool recovering = !e->available, disabled = !e->active;
    struct ec_budget b = ec_budget_begin(e);
    uint8_t status = e->status;
    /* Restore the saved/current preference before delegating master again. */
    int err = disabled ? ec_discover(e, m, &b) : recovering ? ec_set(e, m, &b) : ec_read(e, &status, &b);
    int changed = 0;
    if (!err && !disabled && !recovering) {
        uint64_t now = e->io.now(e->io.ctx);
        if ((status & EC_MASK) != (e->status & EC_MASK)) {
            /* Keys moved the EC: mirror every step now; requantize the EC
             * only after the keys have been quiet for EC_SYNC_MS, re-read
             * first, so a write never overtakes a press (no fighting). */
            uint8_t previous = e->status;
            e->status = status; ec_master(status, previous, m); ec_log(status); changed = 1;
            if (e->level_sync) {
                e->sync_pending = ec_desired(e, status, m) != status;
                e->sync_due = now + EC_SYNC_MS;
            }
        } else if (e->sync_pending && (int64_t)(now - e->sync_due) >= 0) {
            uint8_t desired = ec_desired(e, status, m);
            if ((desired & EC_MASK) != (status & EC_MASK)) err = ec_write_mute(e, desired, m, &b);
            if (!err) e->sync_pending = false;
        }
    }
    ec_result(e, err);
    ec_end(e, &b);
    /* A failed sync retries at the failure cadence, never in a tight loop. */
    if (err && e->sync_pending) e->sync_due = e->next_poll;
    if (err) return err;
    if (disabled) klog("[ec] thinkpad resumed");
    return changed;
}
int thinkpad_ec_volume_poll(struct thinkpad_ec *e, struct ciuki_mixer_master *m)
{
    if (!e->io.now) return -ENODEV;
    if (!e->poll_ms) return e->available ? 0 : -ENODEV; /* ecpoll=0: no periodic EC traffic */
    uint64_t now = e->io.now(e->io.ctx);
    bool sync = e->sync_pending && e->available && (int64_t)(now - e->sync_due) >= 0;
    if (!sync && (int64_t)(now - e->next_poll) < 0) return e->available ? 0 : -ENODEV;
    return ec_mirror(e, m);
}
int thinkpad_ec_volume_refresh(struct thinkpad_ec *e, struct ciuki_mixer_master *m)
{
    if (!e->io.now) return -ENODEV;
    uint64_t now = e->io.now(e->io.ctx);
    /* Rate bound for requested reads; an unavailable EC also keeps its
     * failure cadence, exactly as the poll does. */
    if ((int64_t)(now - e->next_refresh) < 0 || (!e->available && (int64_t)(now - e->next_poll) < 0))
        return e->available ? 0 : -ENODEV;
    e->next_refresh = now + EC_REFRESH_MS;
    return ec_mirror(e, m);
}

#ifndef THINKPAD_EC_HOST
static struct thinkpad_ec native_ec;
static struct kmutex ec_lock;
/* The worker serves the poll (f2-96: every 2 s by default, 1 Hz with
 * ecpoll=1) and user-requested refreshes (f1-100). */
static struct kwait ec_wake;
static bool ec_refresh_wanted, ec_worker_running, ec_rearm;
/* f1-94: every EC port access is a candidate SMI trigger in legacy mode. */
static uint8_t native_read(void *ctx, uint16_t port)
{ (void)ctx; (void)port; return 0xff; } /* transport is the generic broker */
static void native_write(void *ctx, uint16_t port, uint8_t v)
{ (void)ctx; (void)port; (void)v; }
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
static struct thinkpad_ec_smbios retained_identity;
void thinkpad_ec_smbios_snapshot(struct thinkpad_ec_smbios *out)
{ *out = retained_identity; }
/* fx-3: the probe waits for the generic EC (audio init runs before the
 * ACPI inventory has decided ownership); the identity of a deferred call is
 * retained, so the resumed call neither rescans nor logs it again. */
static bool ec_deferred;
void thinkpad_ec_init(void)
{
    if (native_ec.active) return;
    if (!ec_deferred) {
        struct thinkpad_ec_smbios s;
        native_identity(&s);
        retained_identity = s;
        thinkpad_ec_smbios_log(&s);
        if (!thinkpad_ec_gate(&s, g_boot.flags)) return;
        kmutex_init(&ec_lock);
    }
    if (!acpi_ec_quirk_transfer) { klog("[ec] thinkpad gate=generic_ec_unavailable"); return; }
    /* fx-3 follow-up (lead decision): ecpoll=0 disables EC access entirely;
     * the system volume is then the codec/software master only and the
     * keys drive the console amplifier under firmware control, unmirrored. */
    if (!thinkpad_ec_poll_option(g_boot.options, sizeof(g_boot.options), NULL)) {
        klog("[ec] thinkpad gate=ecpoll_off");
        ec_deferred = false;
        return;
    }
    int attached = acpi_ec_quirk_attach ? acpi_ec_quirk_attach() : 0;
    if (attached == -EAGAIN) {
        if (!ec_deferred) klog("[ec] thinkpad deferred until=acpi_inventory");
        ec_deferred = true;
        return;
    }
    ec_deferred = false;
    if (attached) {
        klog("[ec] thinkpad gate=ec_handshake_failed stage=attach error=%d", attached);
        return;
    }
    klog("[ec] thinkpad transport=%s", acpi_ec_quirk_legacy && acpi_ec_quirk_legacy() ? "legacy" : "acpi");
    struct thinkpad_ec_io io = {0, native_read, native_write, native_now, native_pause};
    int err = thinkpad_ec_probe(&native_ec, &io, true, &audio.master);
    if (err) {
        native_ec.active = native_ec.available = false;
        memset(&native_ec.io, 0, sizeof(native_ec.io));
        return;
    }
    audio.master_in_hardware = true;
}
int thinkpad_ec_master_set(struct ciuki_mixer_master *m)
{
    if (!native_ec.io.now) return -ENODEV;
    kmutex_lock(&ec_lock); int err = thinkpad_ec_volume_set(&native_ec, m);
    audio.master_in_hardware = native_ec.available;
    bool sync = native_ec.sync_pending;
    kmutex_unlock(&ec_lock);
    /* fx-3 follow-up: the worker re-arms its wait for the coalesced write. */
    if (sync && __atomic_load_n(&ec_worker_running, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&ec_rearm, true, __ATOMIC_RELAXED);
        kwait_wake_all(&ec_wake);
    }
    return err;
}
int thinkpad_ec_master_poll(struct ciuki_mixer_master *m)
{
    if (!native_ec.io.now) return -ENODEV;
    kmutex_lock(&ec_lock); int err = thinkpad_ec_volume_poll(&native_ec, m);
    audio.master_in_hardware = native_ec.available;
    kmutex_unlock(&ec_lock); return err;
}
int thinkpad_ec_master_refresh(struct ciuki_mixer_master *m)
{
    if (!native_ec.io.now) return -ENODEV;
    kmutex_lock(&ec_lock); int err = thinkpad_ec_volume_refresh(&native_ec, m);
    audio.master_in_hardware = native_ec.available;
    kmutex_unlock(&ec_lock); return err;
}
static bool ec_wanted(void *arg)
{ (void)arg; return __atomic_load_n(&ec_refresh_wanted, __ATOMIC_RELAXED) || __atomic_load_n(&ec_rearm, __ATOMIC_RELAXED); }
/* fx-3 follow-up: wait for the next poll or the pending level sync. */
static uint32_t ec_wait_ms(void)
{
    uint32_t wait = native_ec.poll_ms ? native_ec.poll_ms : 60000u;
    kmutex_lock(&ec_lock);
    if (native_ec.sync_pending && native_ec.available) {
        uint64_t now = native_now(0);
        uint64_t left = native_ec.sync_due > now ? native_ec.sync_due - now : 0;
        if (left < wait) wait = (uint32_t)left;
    }
    kmutex_unlock(&ec_lock);
    return wait;
}
static void ec_worker(void *arg)
{
    (void)arg;
    while (native_ec.io.now) {
        /* With the poll off (ecpoll=0) there is no EC traffic while idle:
         * only a request ends the wait; the 60 s timeout just re-checks it. */
        uint32_t wait = ec_wait_ms();
        if (wait) kwait_wait_until(&ec_wake, ec_wanted, 0, deadline_after_ms(wait));
        if (__atomic_exchange_n(&ec_refresh_wanted, false, __ATOMIC_RELAXED)) audio_master_refresh_requested();
        else if (__atomic_exchange_n(&ec_rearm, false, __ATOMIC_RELAXED)) continue; /* new sync deadline */
        else if (native_ec.poll_ms) audio_master_refresh();
    }
    __atomic_store_n(&ec_worker_running, false, __ATOMIC_RELEASE);
    __atomic_fetch_sub(&audio.commitment_pages, KSTACK_SIZE / PAGE_SIZE, __ATOMIC_RELAXED);
}
int thinkpad_ec_stop(void)
{
    if (!native_ec.io.now) return 0;
    if (!kmutex_lock_until(&ec_lock,deadline_after_ms(500))) return -EBUSY;
    native_ec.active = native_ec.available = false;
    native_ec.io.now = NULL;
    audio.master_in_hardware = false;
    kmutex_unlock(&ec_lock);
    if (__atomic_load_n(&ec_worker_running,__ATOMIC_ACQUIRE)) kwait_wake_all(&ec_wake);
    return 0;
}
void thinkpad_ec_refresh_request(void)
{
    if (!native_ec.io.now || !__atomic_load_n(&ec_worker_running, __ATOMIC_ACQUIRE)) return;
    __atomic_fetch_add(&native_ec.window_requests, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&ec_refresh_wanted, true, __ATOMIC_RELAXED);
    kwait_wake_all(&ec_wake);
}
void thinkpad_ec_window_emit(void)
{
    char line[192];
    if (native_ec.io.now && thinkpad_ec_window(&native_ec, line, sizeof(line))) klog_periodic("%s", line);
}
int thinkpad_ec_poll_state(void)
{ return native_ec.io.now ? (int)native_ec.poll_ms : -1; }
void thinkpad_ec_start(void)
{
    /* fx-3: audio init and the post-inventory attach both start it. */
    if (!native_ec.active || __atomic_load_n(&ec_worker_running, __ATOMIC_ACQUIRE)) return;
    /* f2-96: one RD_EC every 2 s by default (ecpoll=1: 1 Hz, ecpoll=0:
     * none). The EC drives the amplifier from the keys; this poll mirrors
     * the key presses into the shared master (tray, Control Panel, saved
     * preference, mute gate) within 2 s. With ecpoll=0 only an explicit
     * refresh (volume popup) reads the EC (f1-100). */
    const char *source = "default";
    native_ec.poll_ms = thinkpad_ec_poll_option(g_boot.options, sizeof(g_boot.options), &source);
    klog("[ec] thinkpad poll_ms=%u source=%s", native_ec.poll_ms, source);
    /* fx-3 follow-up: one volume. The saved master sets the EC level once
     * at the first worker turn; from then on every user change syncs. */
    kmutex_lock(&ec_lock);
    native_ec.level_sync = native_ec.poll_ms != 0;
    native_ec.sync_pending = native_ec.level_sync &&
        (native_ec.status & 15u) != thinkpad_ec_level((audio.master.left + audio.master.right) / 2u);
    native_ec.sync_due = native_now(0);
    kmutex_unlock(&ec_lock);
    kwait_init(&ec_wake);
    struct task *t = task_create_kernel("thinkpad-volume", ec_worker, 0, P_DEVICE);
    if (t) {
        __atomic_store_n(&ec_worker_running, true, __ATOMIC_RELEASE);
        task_start(t); audio.commitment_pages += KSTACK_SIZE / PAGE_SIZE;
    }
    else klog("[ec] thinkpad worker unavailable error=%d", -ENOMEM);
}
#endif
