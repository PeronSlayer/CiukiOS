/* Bounded IBM ThinkPad console-volume monitor/control; no AML interpreter.
 * References and analog-path policy: docs/design/f4-18-audio-levels.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/audio.h>
#include <ciuki/thinkpad_ec.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/task.h>

#define EC_DATA 0x62
#define EC_SC 0x66
#define EC_IBF 2
#define EC_OBF 1
#define EC_AUDIO 0x30
#define EC_MASK 0x4f
#define EC_TIMEOUT_MS 20u
#define EC_POLL_MS 250u

static bool ec_sum(const uint8_t *p, size_t n)
{ uint8_t sum = 0; for (size_t i = 0; i < n; i++) sum += p[i]; return sum == 0; }
bool thinkpad_ec_entry(const uint8_t *p, size_t n, uint32_t *address, uint16_t *bytes)
{
    if (n < 31 || memcmp(p, "_SM_", 4) || p[5] < 31 || p[5] > n ||
        !ec_sum(p, p[5]) || memcmp(p + 16, "_DMI_", 5) || !ec_sum(p + 16, 15)) return false;
    *bytes = p[22] | (uint16_t)p[23] << 8;
    *address = p[24] | (uint32_t)p[25] << 8 | (uint32_t)p[26] << 16 | (uint32_t)p[27] << 24;
    return *bytes >= 6 && *address >= 0x500 && (uint64_t)*address + *bytes <= PMM_MAX_PAGES * PAGE_SIZE;
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
bool thinkpad_ec_identity(const uint8_t *table, size_t n)
{
    bool found = false;
    for (size_t at = 0; at < n;) {
        const uint8_t *p = table + at;
        if (n - at < 6 || p[1] < 4 || p[1] > n - at - 2) return false;
        size_t end = p[1]; while (end + 1 < n - at && (p[end] || p[end + 1])) end++;
        if (end + 1 >= n - at) return false;
        if (p[0] == 1 && p[1] >= 7) {
            const char *vendor = ec_string(p, p[1], end + 1, p[4]);
            const char *product = ec_string(p, p[1], end + 1, p[5]);
            const char *version = ec_string(p, p[1], end + 1, p[6]);
            found = !strncmp(vendor, "IBM", 4) &&
                (!strncmp(product, "ThinkPad", 8) || !strncmp(version, "ThinkPad", 8));
        }
        if (p[0] == 127) return found;
        at += end + 2;
    }
    return found;
}
/* Each whole transaction has one deadline and a second, clock-independent
 * bound. Unexpected pending output is not consumed as a volume reply. */
static int ec_wait(struct thinkpad_ec *e, unsigned mask, unsigned value, uint64_t end)
{
    for (unsigned tries = 0; tries < 4096; tries++) {
        uint8_t status = e->io.read(e->io.ctx, EC_SC);
        if (status == 0xff) return -ENODEV;
        if ((status & mask) == value) return 0;
        if ((int64_t)(e->io.now(e->io.ctx) - end) >= 0) return -ETIMEDOUT;
        e->io.pause(e->io.ctx);
    }
    return -ETIMEDOUT;
}
static int ec_transfer(struct thinkpad_ec *e, bool write, uint8_t *value)
{
    uint64_t end = e->io.now(e->io.ctx) + EC_TIMEOUT_MS;
    int err = ec_wait(e, EC_IBF | EC_OBF, 0, end);
    if (!err) e->io.write(e->io.ctx, EC_SC, write ? 0x81 : 0x80);
    if (!err) err = ec_wait(e, EC_IBF, 0, end);
    if (!err) e->io.write(e->io.ctx, EC_DATA, EC_AUDIO);
    if (!err) err = ec_wait(e, EC_IBF, 0, end);
    if (!err && write) {
        e->io.write(e->io.ctx, EC_DATA, *value);
        err = ec_wait(e, EC_IBF, 0, end);
    } else if (!err) {
        err = ec_wait(e, EC_OBF, EC_OBF, end);
        if (!err) *value = e->io.read(e->io.ctx, EC_DATA);
    }
    if (err) e->active = false;
    return err;
}
static int ec_read(struct thinkpad_ec *e, uint8_t *status)
{
    int err = ec_transfer(e, false, status);
    if (!err && (*status & 15) > 14) { e->active = false; err = -EIO; }
    return err;
}
static void ec_master(uint8_t status, struct ciuki_mixer_master *m)
{
    m->left = m->right = ((status & 15u) * 100u + 7u) / 14u;
    m->mute = !!(status & 0x40);
}
static void ec_log(uint8_t status)
{ klog("[ec] thinkpad volume=%u mute=%u", status & 15u, !!(status & 0x40)); }
int thinkpad_ec_volume_set(struct thinkpad_ec *e, struct ciuki_mixer_master *m)
{
    if (!e->active) return -ENODEV;
    if (m->left > 100 || m->right > 100 || m->mute > 1) return -EINVAL;
    uint8_t status = 0;
    int err = ec_read(e, &status);
    unsigned level = (((m->left + m->right) / 2) * 14u + 50u) / 100u;
    uint8_t desired = (status & ~EC_MASK) | level | (m->mute ? 0x40 : 0);
    if (!err && desired != status) err = ec_transfer(e, true, &desired);
    if (!err) err = ec_read(e, &status); /* prove the write; never invent state */
    if (!err && (status & EC_MASK) != (desired & EC_MASK)) { e->active = false; err = -EIO; }
    if (!err) {
        if ((e->status & EC_MASK) != (status & EC_MASK)) ec_log(status);
        e->status = status; ec_master(status, m);
    }
    return err;
}
int thinkpad_ec_probe(struct thinkpad_ec *e, const struct thinkpad_ec_io *io, bool identified, struct ciuki_mixer_master *m)
{
    memset(e, 0, sizeof(*e));
    if (!identified || !io || !io->read || !io->write || !io->now || !io->pause) return -ENODEV;
    e->io = *io; e->active = true;
    int err = ec_read(e, &e->status);
    if (!err) err = thinkpad_ec_volume_set(e, m);
    if (!err) { ec_log(e->status); e->next_poll = io->now(io->ctx) + EC_POLL_MS; }
    return err;
}
int thinkpad_ec_volume_poll(struct thinkpad_ec *e, struct ciuki_mixer_master *m)
{
    if (!e->active) return -ENODEV;
    uint64_t now = e->io.now(e->io.ctx);
    if ((int64_t)(now - e->next_poll) < 0) return 0;
    e->next_poll = now + EC_POLL_MS;
    uint8_t status; int err = ec_read(e, &status);
    if (err) return err;
    if ((status & EC_MASK) == (e->status & EC_MASK)) return 0;
    e->status = status; ec_master(status, m); ec_log(status); return 1;
}

#ifndef THINKPAD_EC_HOST
static struct thinkpad_ec native_ec;
static struct kmutex ec_lock;
static uint8_t native_read(void *ctx, uint16_t port) { (void)ctx; return inb(port); }
static void native_write(void *ctx, uint16_t port, uint8_t v) { (void)ctx; outb(port, v); }
static uint64_t native_now(void *ctx) { (void)ctx; return g_ticks; }
static void native_pause(void *ctx) { (void)ctx; task_yield(); }
static bool native_identity(void)
{
    for (uint32_t at = 0xf0000; at <= 0x100000 - 31; at += 16) {
        uint32_t address; uint16_t length;
        if (!thinkpad_ec_entry(P2V(at), 0x100000 - at, &address, &length)) continue;
        bool mapped = true;
        for (uint32_t p = PAGE_ALIGN_DOWN(address); p < address + length; p += PAGE_SIZE)
            if (!(vmm_kernel_pte(KERNEL_VBASE + p) & PTE_P)) { mapped = false; break; }
        if (mapped && thinkpad_ec_identity(P2V(address), length)) return true;
    }
    return false;
}
void thinkpad_ec_init(void)
{
    if ((g_boot.flags & (CBI_F_SMBIOS_QEMU | CBI_F_SAFE_MODE)) || !native_identity()) return;
    kmutex_init(&ec_lock);
    int data = registry_claim(RES_PORT, EC_DATA, EC_DATA + 1, "thinkpad-ec", false);
    if (data < 0) return;
    int command = registry_claim(RES_PORT, EC_SC, EC_SC + 1, "thinkpad-ec", false);
    /* Keep claims quarantined on a partial claim/protocol failure: no other
     * driver may issue a different command into an ambiguous transaction. */
    gen_t dg = registry_get(data)->generation;
    if (command < 0) { registry_quarantine(data, dg); return; }
    gen_t cg = registry_get(command)->generation;
    struct thinkpad_ec_io io = {0, native_read, native_write, native_now, native_pause};
    int err = thinkpad_ec_probe(&native_ec, &io, true, &audio.master);
    if (!err) err = registry_activate(data, dg);
    if (!err) err = registry_activate(command, cg);
    if (err) {
        native_ec.active = false; registry_quarantine(data, dg); registry_quarantine(command, cg);
        klog("[ec] thinkpad disabled error=%d", err); return;
    }
    audio.master_in_hardware = true;
}
int thinkpad_ec_master_set(struct ciuki_mixer_master *m)
{
    kmutex_lock(&ec_lock); int err = thinkpad_ec_volume_set(&native_ec, m); kmutex_unlock(&ec_lock); return err;
}
int thinkpad_ec_master_poll(struct ciuki_mixer_master *m)
{
    kmutex_lock(&ec_lock); int err = thinkpad_ec_volume_poll(&native_ec, m); kmutex_unlock(&ec_lock); return err;
}
static void ec_worker(void *arg)
{ (void)arg; while (native_ec.active) { task_sleep_ms(EC_POLL_MS); audio_master_refresh(); } }
void thinkpad_ec_start(void)
{
    if (!native_ec.active) return;
    struct task *t = task_create_kernel("thinkpad-volume", ec_worker, 0, P_DEVICE);
    if (t) { task_start(t); audio.commitment_pages += KSTACK_SIZE / PAGE_SIZE; }
    else klog("[ec] thinkpad poller unavailable error=%d", -ENOMEM);
}
#endif
