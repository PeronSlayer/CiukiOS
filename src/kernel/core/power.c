/* System call 73: reboot, ACPI S5 soft-off and capability query.
 *
 * Sources (checked 2026-10-12, recorded in docs/design/execution-abi.md):
 * - FADT field offsets: Linux include/acpi/actbl.h struct acpi_table_fadt
 *   (DSDT 40, SMI_CMD 48, ACPI_ENABLE 52, PM1a_CNT_BLK 64, PM1b_CNT_BLK 68,
 *   X_DSDT 140, X_PM1a_CNT_BLK 172, X_PM1b_CNT_BLK 184).
 * - PM1 control bits: Linux drivers/acpi/acpica/aclocal.h (SCI_EN 0x0001,
 *   SLP_TYP 0x1C00 at bit 10, SLP_EN 0x2000).
 * - Sequence: Linux drivers/acpi/acpica/hwsleep.c acpi_hw_legacy_sleep
 *   (write SLP_TYP first, then SLP_TYP | SLP_EN).
 * - AML opcodes: Linux drivers/acpi/acpica/amlcode.h (ZeroOp 00, OneOp 01,
 *   NameOp 08, BytePrefix 0A, WordPrefix 0B, DWordPrefix 0C, PackageOp 12,
 *   RootChar 5C); PkgLength encoding per ACPI AML grammar.
 * - QEMU: hw/acpi/core.c acpi_pm_cnt_write powers off on SLP_EN with
 *   SLP_TYP 0 and does not test SCI_EN; hw/i386/acpi-build.c builds
 *   \_S5 = Package(4){0, 0, 0, 0}; SeaBIOS fw/paravirt.c moves the PM base
 *   to 0x600 (PM1a_CNT 0x604) when QEMU supplies etc/table-loader.
 * Runtime ownership and AML sleep preparation are supplied by acpi/events.c
 * (or APM 1.2, core/apm_platform.c) through ciuki/power_source.h; this file
 * owns the single orderly-shutdown path and retains the read-only parser for
 * compatibility tests only.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/abi.h>
#include <ciuki/power.h>
#if !defined(POWER_HOST) || defined(POWER_HOST_RUNTIME)
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/task.h>
#include <ciuki/process.h>
#include <ciuki/supervisor.h>
#include <ciuki/storage.h>
#include <ciuki/reboot.h>
#include <ciuki/setup_mode.h>
#pragma weak setup_mode
#include <ciuki/blackbox.h>
#include <ciuki/uaddr.h>
#include <ciuki/power_source.h>
#pragma weak blackbox_checkpoint
#endif

#define ACPI_TABLE_MAX   (256u * 1024u)   /* a DSDT above this is refused */
#define ACPI_RSDT_MAX    (36u + 4u * 256u)

static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static bool sig_is(const uint8_t *p, const char *s) { return p[0] == s[0] && p[1] == s[1] && p[2] == s[2] && p[3] == s[3]; }
static uint8_t sum8(const uint8_t *p, uint32_t n) { uint8_t s = 0; while (n--) s = (uint8_t)(s + *p++); return s; }

/* Map a table header, then the whole table; verify signature/length/sum. */
static const uint8_t *table(acpi_map_fn map, void *ctx, uint32_t phys, const char *sig,
                            uint32_t min, uint32_t max, uint32_t *len)
{
    if (!phys) return 0;
    const uint8_t *h = map(ctx, phys, 36);
    if (!h || !sig_is(h, sig)) return 0;
    uint32_t n = rd32(h + 4);
    if (n < min || n > max || (uint64_t)phys + n > 0x100000000ull) return 0;
    const uint8_t *t = map(ctx, phys, n);
    if (!t || !sig_is(t, sig) || rd32(t + 4) != n || sum8(t, n)) return 0;
    *len = n;
    return t;
}

/* ACPI 6.6 §20: PkgLength includes its own encoding, not the opcode. */
static bool pkg_length(const uint8_t *p, uint32_t avail, uint32_t *used, uint32_t *extent)
{
    if (!avail) return false;
    unsigned follow = p[0] >> 6;
    if ((follow && (p[0] & 0x30)) || 1u + follow > avail) return false;
    uint32_t length = p[0] & (follow ? 15u : 63u);
    for (unsigned i = 0; i < follow; i++) length |= (uint32_t)p[i + 1] << (4 + 8 * i);
    *used = 1u + follow;
    if (length < *used || length > avail) return false;
    *extent = length;
    return true;
}
static bool aml_integer(const uint8_t *p, uint32_t avail, uint32_t *value, uint32_t *used)
{
    if (!avail) return false;
    unsigned n;
    switch (p[0]) {
    case 0: case 1: *value = p[0]; *used = 1; return true;
    case 0xFF: *value = UINT32_MAX; *used = 1; return true;
    case 0x0A: n = 1; break;
    case 0x0B: n = 2; break;
    case 0x0C: n = 4; break;
    case 0x0E: n = 8; break;
    default: return false;
    }
    if (avail < n + 1) return false;
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        if (i < 4) v |= (uint32_t)p[i + 1] << (8 * i);
        else if (p[i + 1]) v = UINT32_MAX;
    }
    *value = v; *used = n + 1;
    return true;
}
static bool aml_name(const uint8_t *p, uint32_t len, uint32_t *used, bool *s5, bool *root_scope)
{
    uint32_t at = 0;
    bool root = len && p[0] == 0x5C, parent = false;
    if (root) at++;
    while (at < len && p[at] == 0x5E) { parent = true; at++; }
    if (at >= len || (root && parent)) return false;
    unsigned count = 1;
    if (p[at] == 0) { count = 0; at++; }
    else if (p[at] == 0x2E) { count = 2; at++; }
    else if (p[at] == 0x2F) {
        if (++at >= len || !p[at]) return false;
        count = p[at++];
    }
    if (count > (len - at) / 4) return false;
    *s5 = !parent && count == 1 && sig_is(p + at, "_S5_");
    *root_scope = root && !parent && !count;
    for (unsigned i = 0; i < count * 4; i++) {
        uint8_t c = p[at + i];
        if (!(c == '_' || (c >= 'A' && c <= 'Z') || (i % 4 && c >= '0' && c <= '9'))) return false;
    }
    *used = at + count * 4;
    return true;
}
/* Skip a data/reference object as a unit. Its bytes cannot declare a Name. */
static bool aml_data(const uint8_t *p, uint32_t len, uint32_t *used)
{
    uint32_t ignored, n, extent;
    bool s5, root;
    if (!len) return false;
    if (aml_integer(p, len, &ignored, used)) return true;
    if (p[0] == 0x11 || p[0] == 0x12 || p[0] == 0x13) {
        if (!pkg_length(p + 1, len - 1, &n, &extent) || extent <= n) return false;
        *used = 1 + extent; return true;
    }
    if (p[0] == 0x0D) {
        for (n = 1; n < len; n++) if (!p[n]) { *used = n + 1; return true; }
        return false;
    }
    if (p[0] >= 0x60 && p[0] <= 0x6E) { *used = 1; return true; }
    return aml_name(p, len, used, &s5, &root);
}
static bool s5_package(const uint8_t *p, uint32_t len, uint8_t *typa, uint8_t *typb)
{
    uint32_t n, extent, at, v[2];
    if (!len || p[0] != 0x12 || !pkg_length(p + 1, len - 1, &n, &extent)) return false;
    at = 1 + n;
    uint32_t end = 1 + extent;
    if (at >= end || p[at] < 2) return false;
    unsigned elements = p[at++];
    for (unsigned i = 0; i < elements; i++) {
        if (i < 2) {
            if (!aml_integer(p + at, end - at, &v[i], &n) || v[i] > 7) return false;
        } else if (!aml_data(p + at, end - at, &n)) return false;
        at += n;
    }
    if (at != end) return false;
    *typa = (uint8_t)v[0]; *typb = (uint8_t)v[1]; return true;
}
/* A bounded root TermList walker, not an AML interpreter. Unknown terms
 * refuse discovery rather than guessing where their data ends. Only Scope(\)
 * is visited; non-root scopes, methods and data packages are opaque. */
static int s5_terms(const uint8_t *p, uint32_t len, unsigned depth, uint8_t *a, uint8_t *b)
{
    if (depth > 8) return -ENOENT;
    uint32_t at = 0;
    while (at < len) {
        uint8_t op = p[at++];
        uint32_t n, extent;
        bool name_s5, root;
        if (op == 0x08) {
            if (!aml_name(p + at, len - at, &n, &name_s5, &root)) return -ENOENT;
            at += n;
            if (name_s5 && s5_package(p + at, len - at, a, b)) return 0;
            if (!aml_data(p + at, len - at, &n)) return -ENOENT;
            at += n;
        } else if (op == 0x10 || op == 0x14 || op == 0xA0 || op == 0xA1 || op == 0xA2) {
            if (!pkg_length(p + at, len - at, &n, &extent)) return -ENOENT;
            if (op == 0x10) {
                uint32_t name_len;
                if (!aml_name(p + at + n, extent - n, &name_len, &name_s5, &root)) return -ENOENT;
                if (root && !s5_terms(p + at + n + name_len, extent - n - name_len, depth + 1, a, b)) return 0;
            }
            at += extent;
        } else if (op == 0x06) { /* Alias: two names */
            for (unsigned i = 0; i < 2; i++) {
                if (!aml_name(p + at, len - at, &n, &name_s5, &root)) return -ENOENT;
                at += n;
            }
        } else if (op == 0x70) { /* Store: data source and simple target */
            for (unsigned i = 0; i < 2; i++) {
                if (!aml_data(p + at, len - at, &n)) return -ENOENT;
                at += n;
            }
        } else if (op == 0x15) { /* External */
            if (!aml_name(p + at, len - at, &n, &name_s5, &root) || len - at - n < 2) return -ENOENT;
            at += n + 2;
        } else if (op == 0x5B) {
            if (at >= len) return -ENOENT;
            op = p[at++];
            if (op >= 0x81 && op <= 0x87) { /* Field, Device, Processor, PowerResource, ThermalZone, IndexField, BankField */
                if (!pkg_length(p + at, len - at, &n, &extent)) return -ENOENT;
                at += extent;
            } else if (op == 0x01 || op == 0x02 || op == 0x80) {
                if (!aml_name(p + at, len - at, &n, &name_s5, &root)) return -ENOENT;
                at += n;
                if (op != 0x02) { if (at >= len) return -ENOENT; at++; }
                if (op == 0x80) for (unsigned i = 0; i < 2; i++) {
                    if (!aml_data(p + at, len - at, &n)) return -ENOENT;
                    at += n;
                }
            } else return -ENOENT;
        } else if (op != 0xA3) return -ENOENT; /* Noop */
    }
    return -ENOENT;
}
int acpi_s5_find(const uint8_t *aml, uint32_t len, uint8_t *typa, uint8_t *typb)
{
    return s5_terms(aml, len, 0, typa, typb);
}

static uint16_t io_port(const uint8_t *fadt, uint32_t len, uint32_t legacy, uint32_t gas)
{
    uint32_t port = rd32(fadt + legacy);
    /* The extended block counts only in SystemIO space (space id 1). */
    if (!port && len >= gas + 12 && fadt[gas] == 1 && !rd32(fadt + gas + 8))
        port = rd32(fadt + gas + 4);
    return port <= 0xFFFFu ? (uint16_t)port : 0;
}

int acpi_soft_off_parse(acpi_map_fn map, void *ctx, uint32_t rsdp_phys,
                        struct acpi_soft_off *out, const char **why)
{
    *why = "rsdp";
    if (!rsdp_phys) return -ENOTSUP;
    const uint8_t *rsdp = map(ctx, rsdp_phys, 36);
    if (!rsdp || !sig_is(rsdp, "RSD ") || !sig_is(rsdp + 4, "PTR ") || sum8(rsdp, 20)) return -ENOTSUP;
    uint32_t len, rsdt_phys = rd32(rsdp + 16);
    const uint8_t *fadt = 0;
    uint32_t fadt_len = 0;
    const uint8_t *root = table(map, ctx, rsdt_phys, "RSDT", 36, ACPI_RSDT_MAX, &len);
    unsigned width = 4;
    if (!root && rsdp[15] >= 2) {
        /* ACPI 2.0+: an XSDT entry above 4 GiB is unreachable here. */
        const uint8_t *x = map(ctx, rsdp_phys, 36);
        if (x && !sum8(x, 36) && !rd32(x + 28))
            root = table(map, ctx, rd32(x + 24), "XSDT", 36, 36u + 8u * 256u, &len);
        width = 8;
    }
    *why = "rsdt";
    if (!root) return -ENOTSUP;
    for (uint32_t at = 36; at + width <= len && !fadt; at += width) {
        if (width == 8 && rd32(root + at + 4)) continue;
        uint32_t phys = rd32(root + at);
        const uint8_t *h = phys ? map(ctx, phys, 36) : 0;
        if (h && sig_is(h, "FACP"))
            fadt = table(map, ctx, phys, "FACP", 116, 4096, &fadt_len);
    }
    *why = "fadt";
    if (!fadt) return -ENOTSUP;
    struct acpi_soft_off r = { 0 };
    r.fadt_revision = fadt[8];
    r.pm1a_cnt = io_port(fadt, fadt_len, 64, 172);
    r.pm1b_cnt = io_port(fadt, fadt_len, 68, 184);
    uint32_t smi = rd32(fadt + 48);
    r.smi_cmd = smi <= 0xFFFFu ? (uint16_t)smi : 0;
    r.acpi_enable = fadt[52];
    *why = "pm1a_cnt";
    if (!r.pm1a_cnt) return -ENOTSUP;
    uint32_t dsdt_phys = rd32(fadt + 40);
    if (!dsdt_phys && fadt_len >= 148 && !rd32(fadt + 144))
        dsdt_phys = rd32(fadt + 140);
    uint32_t dsdt_len;
    const uint8_t *dsdt = table(map, ctx, dsdt_phys, "DSDT", 36, ACPI_TABLE_MAX, &dsdt_len);
    *why = "dsdt";
    if (!dsdt) return -ENOTSUP;
    *why = "s5";
    if (acpi_s5_find(dsdt + 36, dsdt_len - 36, &r.slp_typa, &r.slp_typb)) return -ENOTSUP;
    *why = "ok";
    *out = r;
    return 0;
}

#if !defined(POWER_HOST) || defined(POWER_HOST_RUNTIME)
extern int power_devices_quiesce(void) __attribute__((weak));
static uint32_t power_progress;
static void power_checkpoint(void)
{ if (blackbox_checkpoint) (void)blackbox_checkpoint(); }
void power_discover(void)
{ klog("[power] ACPI/APM ownership and soft-off qualification belong to the power_source provider"); }
#ifndef POWER_HOST
int32_t power_status_query(uint32_t address, uint32_t bytes)
{
    struct ciuki_power_status out;
    if (bytes != sizeof(out)) return -EINVAL;
    int e = copy_from_user(&out, address, __builtin_offsetof(struct ciuki_power_status, batteries));
    if (e) return e;
    if (out.version != CIUKI_POWER_VERSION || out.size != sizeof(out) || out.reserved) return -EINVAL;
    power_source_status(&out);
    return copy_to_user(address, &out, sizeof(out));
}
#endif
static int flush_storage(void)
{
    struct storage *s = storage_get();
    if (!s || !s->ready) return 0;
    int e = storage_sync();
    if (e) { klog("[power] storage flush failed: %d", e); power_checkpoint(); }
    return e;
}
int32_t power_syscall(uint32_t action)
{
    struct process *caller = proc_current();
    /* The desktop, or Setup's capability holder in Setup mode, owns call 73.
     * Both take the same orderly path below: storage flush, native device
     * quiesce, the stopped latch, then reset or the active provider's
     * soft-off (ACPI S5 or APM Off). */
    if (!caller || (!(setup_mode && setup_mode() && caller->setup_capability) &&
        (!supervisor_desktop_pid() || caller->pid != supervisor_desktop_pid())))
        return -EPERM;
    if (action == CIUKI_POWER_QUERY)
        return power_progress | CIUKI_POWER_CAN_REBOOT | (power_source_can_off() ? CIUKI_POWER_CAN_OFF : 0);
    if (action != CIUKI_POWER_REBOOT && action != CIUKI_POWER_OFF) return -EINVAL;
    bool off = action == CIUKI_POWER_OFF;
    klog("[power] %s requested by pid %u", off ? "power off" : "reboot", caller->pid);
    int e = flush_storage();
    if (e) { if (e != -EBUSY) power_progress = CIUKI_POWER_UNCERTAIN; return e; }
    power_progress = CIUKI_POWER_UNCERTAIN;
    e = power_devices_quiesce ? power_devices_quiesce() : -ENOTSUP;
    if (e) { klog("[power] native quiesce failed: %d", e); power_checkpoint(); return -EBUSY; }
    power_progress = CIUKI_POWER_STOPPED;
    /* The orderly-shutdown proof for every provider: ACPI S5 and APM Off. */
    power_source_mark_stopped();
    if (!off) { klog("[power] stage=reset fallback=8042-triple-fault"); power_checkpoint(); kernel_reboot(); }
    struct power_source_snapshot provider; power_source_snapshot(&provider);
    const char *name = provider.provider == POWER_SOURCE_ACPI ? "acpi" : provider.provider == POWER_SOURCE_APM ? "apm" : "none";
    if (!power_source_can_off()) {
        klog("[power] no qualified soft-off provider=%s; devices stopped, safe to turn off", name); power_checkpoint(); return -ENOTSUP;
    }
    klog("[power] stage=%s-off state=quiescent", name); power_checkpoint();
    e = power_source_soft_off();
    klog("[power] %s soft-off returned error=%d; stopped devices and leases retained", name, e); power_checkpoint();
    return e;
}
#endif
