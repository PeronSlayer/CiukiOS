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
 * owns the single orderly-shutdown path. fx-acpioff (2026-10-26): the
 * read-only parser below feeds the ACPI S5 register fallback
 * (core/power_acpi_s5.c), consulted only after the active provider.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/abi.h>
#include <ciuki/power.h>
#include <ciuki/boot_info.h>
#include <ciuki/apm_ports.h>
#if !defined(POWER_HOST) || defined(POWER_HOST_RUNTIME)
#include <ciuki/kernel.h>
#include <ciuki/security_device.h>
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
#include <ciuki/ata_power.h>
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

enum io_block_state { IO_BLOCK_ABSENT, IO_BLOCK_VALID, IO_BLOCK_UNSUPPORTED };
static enum io_block_state io_port(const uint8_t *fadt, uint32_t len, uint32_t legacy,
                                    uint32_t gas, unsigned bytes, unsigned access, uint16_t *out)
{
    uint32_t port = rd32(fadt + legacy);
    *out = 0;
    /* ACPI 2.0c: a present extended address supersedes the legacy block.
     * Unsupported is never equivalent to an optional, absent block. */
    if (len >= gas + 12 && (rd32(fadt + gas + 4) || rd32(fadt + gas + 8))) {
        if (fadt[gas] != 1 || rd32(fadt + gas + 8) || fadt[gas + 2] ||
            fadt[gas + 1] != bytes * 8u ||
            (fadt[gas + 3] && fadt[gas + 3] != access)) return IO_BLOCK_UNSUPPORTED;
        port = rd32(fadt + gas + 4);
    }
    if (!port) return IO_BLOCK_ABSENT;
    if (!bytes || (uint64_t)port + bytes > 0x10000u) return IO_BLOCK_UNSUPPORTED;
    *out = (uint16_t)port;
    return IO_BLOCK_VALID;
}

/* fx-acpioff: \_S5 may live in an SSDT (ACPI 2.0 5.2.11.2: SSDT definition
 * blocks are loaded after the DSDT and add objects to the namespace; iPXE
 * acpipwr.c extracts \_S5 "from DSDT or any SSDT"). A table that fails its
 * checksum or bounds is skipped, never guessed. */
static bool s5_in_ssdts(acpi_map_fn map, void *ctx, const uint8_t *root, uint32_t len, unsigned width,
                        uint8_t *typa, uint8_t *typb)
{
    for (uint32_t at = 36; at + width <= len; at += width) {
        if (width == 8 && rd32(root + at + 4)) continue;
        uint32_t phys = rd32(root + at), n;
        const uint8_t *h = phys ? map(ctx, phys, 36) : 0;
        if (!h || !sig_is(h, "SSDT")) continue;
        const uint8_t *t = table(map, ctx, phys, "SSDT", 36, ACPI_TABLE_MAX, &n);
        if (t && !acpi_s5_find(t + 36, n - 36, typa, typb)) return true;
    }
    return false;
}
/* RSDP (ACPI 1.0 20-byte checksum; 2.0 extended 36-byte one before the
 * XSDT is used), RSDT or XSDT, then the first FACP. NULL with *why naming
 * the failed step. */
struct acpi_root { const uint8_t *table; uint32_t len; unsigned width; };
static const uint8_t *fadt_locate(acpi_map_fn map, void *ctx, uint32_t rsdp_phys, struct acpi_root *root,
                                  uint32_t *fadt_len, const char **why)
{
    *why = "rsdp";
    if (!rsdp_phys) return 0;
    const uint8_t *rsdp = map(ctx, rsdp_phys, 36);
    if (!rsdp || !sig_is(rsdp, "RSD ") || !sig_is(rsdp + 4, "PTR ") || sum8(rsdp, 20)) return 0;
    uint32_t len, rsdt_phys = rd32(rsdp + 16);
    const uint8_t *fadt = 0;
    const uint8_t *table_root = table(map, ctx, rsdt_phys, "RSDT", 36, ACPI_RSDT_MAX, &len);
    unsigned width = 4;
    if (!table_root && rsdp[15] >= 2) {
        /* ACPI 2.0+: an XSDT entry above 4 GiB is unreachable here. */
        const uint8_t *x = map(ctx, rsdp_phys, 36);
        if (x && !sum8(x, 36) && !rd32(x + 28))
            table_root = table(map, ctx, rd32(x + 24), "XSDT", 36, 36u + 8u * 256u, &len);
        width = 8;
    }
    *why = "rsdt";
    if (!table_root) return 0;
    for (uint32_t at = 36; at + width <= len && !fadt; at += width) {
        if (width == 8 && rd32(table_root + at + 4)) continue;
        uint32_t phys = rd32(table_root + at);
        const uint8_t *h = phys ? map(ctx, phys, 36) : 0;
        if (h && sig_is(h, "FACP"))
            fadt = table(map, ctx, phys, "FACP", 116, 4096, fadt_len);
    }
    *why = "fadt";
    *root = (struct acpi_root){table_root, len, width};
    return fadt;
}
int acpi_soft_off_parse(acpi_map_fn map, void *ctx, uint32_t rsdp_phys,
                        struct acpi_soft_off *out, const char **why)
{
    struct acpi_root tables;
    uint32_t fadt_len = 0;
    const uint8_t *fadt = fadt_locate(map, ctx, rsdp_phys, &tables, &fadt_len, why);
    if (!fadt) return -ENOTSUP;
    const uint8_t *root = tables.table;
    uint32_t len = tables.len;
    unsigned width = tables.width;
    struct acpi_soft_off r = { 0 };
    r.fadt_revision = fadt[8];
    r.flags = rd32(fadt + 112);
    r.sci_int = (uint16_t)(fadt[46] | fadt[47] << 8);
    r.pm1_evt_len = fadt[88]; r.pm1_cnt_len = fadt[89];
    r.gpe0_len = fadt[92]; r.gpe1_len = fadt[93];
    uint32_t smi = rd32(fadt + 48);
    *why = "smi_cmd";
    if (smi > 0xFFFFu) return -ENOTSUP;
    r.smi_cmd = (uint16_t)smi;
    r.acpi_enable = fadt[52]; r.acpi_disable = fadt[53];
    /* ACPI 5.0 HW_REDUCED_ACPI (flags bit 20): no PM1 registers exist;
     * the SLEEP_CONTROL_REG path is not implemented. */
    *why = "hw_reduced";
    if (r.fadt_revision >= 5 && (r.flags & (1u << 20))) return -ENOTSUP;
    *why = "pm1_cnt_len";
    if (r.pm1_cnt_len != 2) return -ENOTSUP;
    *why = "pm1a_cnt";
    if (io_port(fadt, fadt_len, 64, 172, r.pm1_cnt_len, 2, &r.pm1a_cnt) != IO_BLOCK_VALID) return -ENOTSUP;
    *why = "pm1b_cnt";
    if (io_port(fadt, fadt_len, 68, 184, r.pm1_cnt_len, 2, &r.pm1b_cnt) == IO_BLOCK_UNSUPPORTED) return -ENOTSUP;
    *why = "pm1a_evt";
    if (io_port(fadt, fadt_len, 56, 148, r.pm1_evt_len, 2, &r.pm1a_evt) == IO_BLOCK_UNSUPPORTED ||
        (r.pm1a_evt && r.pm1_evt_len != 4)) return -ENOTSUP;
    *why = "pm1b_evt";
    if (io_port(fadt, fadt_len, 60, 160, r.pm1_evt_len, 2, &r.pm1b_evt) == IO_BLOCK_UNSUPPORTED ||
        (r.pm1b_evt && r.pm1_evt_len != 4)) return -ENOTSUP;
    *why = "gpe0";
    if (io_port(fadt, fadt_len, 80, 220, r.gpe0_len, 1, &r.gpe0) == IO_BLOCK_UNSUPPORTED ||
        (r.gpe0 && ((r.gpe0_len & 1) || r.gpe0_len > 32))) return -ENOTSUP;
    *why = "gpe1";
    if (io_port(fadt, fadt_len, 84, 232, r.gpe1_len, 1, &r.gpe1) == IO_BLOCK_UNSUPPORTED ||
        (r.gpe1 && ((r.gpe1_len & 1) || r.gpe1_len > 32))) return -ENOTSUP;
    uint32_t dsdt_phys = rd32(fadt + 40);
    if (!dsdt_phys && fadt_len >= 148 && !rd32(fadt + 144))
        dsdt_phys = rd32(fadt + 140);
    uint32_t dsdt_len;
    const uint8_t *dsdt = table(map, ctx, dsdt_phys, "DSDT", 36, ACPI_TABLE_MAX, &dsdt_len);
    *why = "dsdt";
    if (!dsdt) return -ENOTSUP;
    *out = r;
    *why = "s5";
    if (!acpi_s5_find(dsdt + 36, dsdt_len - 36, &r.slp_typa, &r.slp_typb)) r.source = ACPI_S5_SOURCE_DSDT;
    else if (s5_in_ssdts(map, ctx, root, len, width, &r.slp_typa, &r.slp_typb)) r.source = ACPI_S5_SOURCE_SSDT;
    else return -ENOTSUP;
    *why = "ok";
    *out = r;
    return 0;
}

/* sec-wire: the fx-18 APM port window's FADT source when the ACPI
 * inventory loaded no tables (QEMU run of image ae977fbe: "[apm] window
 * ranges=2 source=none", then SeaBIOS's Set Power State Off wrote 2000h to
 * PM1a_CNT 604h and the BIOS VM refused it, "reason=port_denied"). The
 * blocks follow ACPI 2.0c 5.2.8 (legacy fields 48-93; a present X_ GAS in
 * SystemIO with matching width supersedes them, as io_port does); an
 * unsupported block is left out of the window, never guessed. */
int acpi_fadt_pm_blocks(acpi_map_fn map, void *ctx, uint32_t rsdp_phys, struct apm_pm_blocks *out,
                        const char **why)
{
    struct acpi_root tables;
    uint32_t fadt_len = 0;
    const uint8_t *fadt = fadt_locate(map, ctx, rsdp_phys, &tables, &fadt_len, why);
    if (!fadt) return -ENOTSUP;
    struct apm_pm_blocks b = { 0 };
    uint32_t smi = rd32(fadt + 48);
    b.smi_cmd = smi <= 0xFFFFu ? (uint16_t)smi : 0;
    b.pm1_evt_len = fadt[88]; b.pm1_cnt_len = fadt[89]; b.pm2_cnt_len = fadt[90];
    b.pm_tmr_len = fadt[91]; b.gpe0_len = fadt[92]; b.gpe1_len = fadt[93];
    static const struct { uint8_t legacy, gas, access; } at[] = {
        {56, 148, 2}, {60, 160, 2}, {64, 172, 2}, {68, 184, 2}, {72, 196, 1}, {76, 208, 4}, {80, 220, 1}, {84, 232, 1}};
    const uint8_t lengths[] = {b.pm1_evt_len, b.pm1_evt_len, b.pm1_cnt_len, b.pm1_cnt_len, b.pm2_cnt_len,
                               b.pm_tmr_len, b.gpe0_len, b.gpe1_len};
    uint16_t *ports[] = {&b.pm1a_evt, &b.pm1b_evt, &b.pm1a_cnt, &b.pm1b_cnt, &b.pm2_cnt, &b.pm_tmr, &b.gpe0, &b.gpe1};
    for (unsigned i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
        uint16_t port = 0;
        /* The GAS width covers the whole block, a GPE block's too (as for
         * the S5 parse above). */
        if (lengths[i] && io_port(fadt, fadt_len, at[i].legacy, at[i].gas, lengths[i], at[i].access, &port) == IO_BLOCK_VALID)
            *ports[i] = port;
    }
    *why = "pm1a_cnt";
    if (!b.pm1a_cnt || b.pm1_cnt_len != 2) return -ENOTSUP;
    *why = "ok";
    *out = b;
    return 0;
}
/* sec-wire: see power.h. ACPI 1.0b 5.2.5 (RSDP in the first KiB of the
 * EBDA or E0000h-FFFFFh); E820 types 3/4 hold the tables on the qualified
 * laptops (T23 trial 6c3b7800: "[acpi] inventory ... tables=8"). SeaBIOS
 * allocates every QEMU table but the RSDP from its ZoneHigh
 * (src/fw/romfile_loader.c ROMFILE_LOADER_ALLOC_ZONE_HIGH; QEMU
 * hw/i386/acpi-build.c allocates only the RSDP in FSEG), which
 * malloc_preinit (src/malloc.c) carves from the top of the highest RAM range
 * below 4 GiB and reports as E820_RESERVED, at most BUILD_MAX_HIGHTABLE =
 * 16 MiB (src/config.h); the unused part is given back as RAM, so the tail
 * starts where a usable range ends. Linux maps ACPI tables wherever the
 * root table points (drivers/acpi/osl.c); here only that tail is added. */
bool acpi_firmware_extent(const struct ciuki_e820 *map, unsigned count, uint32_t phys, uint32_t len,
                          bool reserved_tail)
{
    if (!len || phys > UINT32_MAX - len) return false;
    uint64_t start = phys, end = (uint64_t)phys + len;
    if ((start >= 0xe0000 && end <= 0x100000) || (start >= 0x80000 && end <= 0xa0000)) return true;
    for (unsigned i = 0; map && i < count; i++) {
        uint64_t base = map[i].base, length = map[i].length;
        uint32_t type = map[i].type;
        if (length > UINT64_MAX - base || base > start || base + length < end) continue;
        if (type == 3 || type == 4) return true;
        if (!reserved_tail || type != 2 || length > ACPI_RESERVED_TAIL_MAX || base + length > 0x100000000ull) continue;
        for (unsigned j = 0; j < count; j++) {
            uint64_t ram = map[j].base, ram_length = map[j].length;
            if (map[j].type == 1 && ram_length && ram_length <= UINT64_MAX - ram && ram + ram_length == base) return true;
        }
    }
    return false;
}

#if !defined(POWER_HOST) || defined(POWER_HOST_RUNTIME)
extern int power_devices_quiesce_terminal(void) __attribute__((weak));
extern int ata_power_quiesce(void) __attribute__((weak));
extern int audio_settings_flush(void) __attribute__((weak));
/* fx-acpioff: the ACPI S5 register fallback (core/power_acpi_s5.c), weak so
 * host harnesses without it keep the two-provider behaviour. Provider order
 * (docs/design/acpi-events-power.md, "ACPI S5 register fallback"): the
 * active provider first (ACPI owned by the OS, else APM Off), then the
 * fallback when no provider is qualified or the provider's Off returned. */
#pragma weak power_s5_fallback_qualified
#pragma weak power_s5_fallback_state
#pragma weak power_s5_fallback_off
static bool s5_fallback_ready(void)
{ return power_s5_fallback_qualified && power_s5_fallback_qualified(); }
static const char *s5_fallback_state(void)
{ return power_s5_fallback_state ? power_s5_fallback_state() : "absent"; }
static bool power_can_off(void) { return power_source_can_off() || s5_fallback_ready(); }
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
/* fx-5: call 73 is one sequence. A concurrent request is refused with EBUSY
 * (nothing changed; the desktop retries). Completed stages are latched so a
 * repeated request resumes after them: storage once durable is not flushed
 * again, and a stopped machine goes straight to reset or soft-off. */
static bool power_running, storage_flushed;
static uint32_t power_requests;
static int flush_storage(void)
{
    if (storage_flushed) return 0;
    struct storage *s = storage_get();
    if (!s || !s->ready) return 0;
    int e = storage_sync();
    if (e) { klog("[power] storage flush failed: %d", e); power_checkpoint(); }
    else storage_flushed = true;
    return e;
}
static int power_sequence(bool off)
{
    int e;
    if (!(power_progress & CIUKI_POWER_STOPPED)) {
        /* fx-9b: a preference applied but not yet saved (volume keys) is
         * written before the storage flush makes it durable; a failure is
         * recorded and never stops the shutdown. */
        if (audio_settings_flush && !storage_flushed) {
            int a = audio_settings_flush();
            if (a) klog("[power] audio settings not saved: %d", a);
        }
        e = flush_storage();
        /* EBUSY before any irreversible step: the session continues intact. */
        if (e) { if (e != -EBUSY) power_progress = CIUKI_POWER_UNCERTAIN; return e; }
        power_progress = CIUKI_POWER_UNCERTAIN;
        e = power_devices_quiesce_terminal ? power_devices_quiesce_terminal() : -ENOTSUP;
        if (e) { klog("[power] native quiesce failed: %d", e); power_checkpoint(); return -EBUSY; }
        power_progress = CIUKI_POWER_STOPPED;
        /* The orderly-shutdown proof for every provider: ACPI S5 and APM Off. */
        power_source_mark_stopped();
    }
    if (!off) { klog("[power] stage=reset fallback=8042-triple-fault"); power_checkpoint(); kernel_reboot(); }
    struct power_source_snapshot provider; power_source_snapshot(&provider);
    const char *name = provider.provider == POWER_SOURCE_ACPI ? "acpi" : provider.provider == POWER_SOURCE_APM ? "apm" : "none";
    bool active = power_source_can_off(), fallback = s5_fallback_ready();
    if (!active && !fallback) {
        klog("[power] no qualified soft-off provider=%s acpi_s5=%s; devices stopped, safe to turn off", name, s5_fallback_state());
        power_checkpoint(); return -ENOTSUP;
    }
    if (active) {
        klog("[power] stage=%s-off state=quiescent", name); power_checkpoint();
        e = power_source_soft_off();
        klog("[power] %s soft-off returned error=%d; stopped devices and leases retained", name, e); power_checkpoint();
        if (!fallback) return e;
        klog("[power] stage=acpi-s5-off provider=fallback after=%s error=%d", name, e);
    } else klog("[power] stage=acpi-s5-off provider=fallback state=quiescent unqualified=%s", name);
    power_checkpoint();
    int fallback_error = power_s5_fallback_off(false);
    klog("[power] acpi s5 fallback returned error=%d; stopped devices and leases retained", fallback_error); power_checkpoint();
    if (!active || fallback_error != -ENOTSUP) e = fallback_error;
    return e;
}
/* T23 trial c42f556e (docs/validation/t23-win32-hang-shutdown-2026-10-25.md):
 * the forced end of a shutdown that cannot finish, after the user chose it.
 * It runs beside a REBOOT/OFF still blocked on another thread (no
 * power_running), takes no storage lock itself and waits for nothing
 * unbounded: a best-effort durable flush through the bounded suspend-flush
 * worker (POWER_FORCE_FLUSH_MS), then a bounded device fence through the
 * platform's nonblocking quiesce step (power_source_quiesce_dma, the same
 * native DMA/IRQ stop path as the orderly sequence, on its own worker;
 * POWER_FORCE_QUIESCE_MS), the UNCERTAIN latch, then reset, or the
 * provider's soft-off only when the fence proved every device stopped
 * (emergency latch, distinct from the orderly proof; review 2 finding 5).
 * A reset needs no fence; an Off with active DMA is refused (EBUSY) and
 * the user turns the machine off with its power button. */
#define POWER_FORCE_FLUSH_MS 3000u
#define POWER_FORCE_QUIESCE_MS 2000u
static int power_force(bool off, uint32_t pid)
{
    klog("[power] forced %s requested by pid %u running=%u progress=%u storage=%s", off ? "power off" : "reboot",
         pid, power_running, power_progress, storage_flushed ? "durable" : "pending");
    if (!storage_flushed) {
        uint64_t end = deadline_after_ms(POWER_FORCE_FLUSH_MS);
        int e;
        while ((e = storage_sync_bounded(end)) == -EAGAIN && !deadline_passed(end)) task_sleep_ms(10);
        klog("[power] forced flush result=%d", e == -EAGAIN ? -ETIMEDOUT : e);
    }
    int fence;
    {
        uint64_t end = deadline_after_ms(POWER_FORCE_QUIESCE_MS);
        while ((fence = power_source_quiesce_dma(end)) == -EAGAIN && !deadline_passed(end)) task_sleep_ms(10);
        if (fence == -EAGAIN) fence = -ETIMEDOUT;
        /* Review 4, finding 5: the platform step is the suspend-capable stop
         * (network, audio, USB); the ATA storage fence is terminal and runs
         * here, on the forced path only, whatever the platform step said. */
        int devices = fence;
        int disks = ata_power_quiesce ? ata_power_quiesce() : 0;
        if (!fence) fence = disks;
        klog("[power] forced device fence result=%d devices=%d storage=%d", fence, devices, disks);
        power_progress |= CIUKI_POWER_UNCERTAIN;
        if (!off) {
            /* Review 4 finding 3 and review 5 finding 1: the 8042 pulse and
             * the triple fault are a CPU INIT that leaves bus masters running.
             * The ATA fence stops or revokes every channel's bus master even
             * when a device cannot prove itself idle, so a failed disk proof
             * leaves no DMA; a failed or timed-out platform step (network,
             * audio, USB keep their uncertain claims) does. Then only the
             * qualified hard PCI reset (PCIRST#) may be written; without one,
             * or when it did not take, everything is kept and the user resets
             * the machine (the desktop shows the reset-button message). */
            if (devices) {
                klog("[power] stage=forced-reset order=pci-reset-only devices=%d", devices);
                int e = kernel_reboot_pci_only();
                klog("[power] forced reset refused: DMA unresolved and %s; use the computer's reset button",
                     e == -ENOTSUP ? "no qualified hard reset" : "the hard reset did not take");
                return -EBUSY;
            }
            klog("[power] stage=forced-reset fallback=8042-triple-fault"); kernel_reboot();
        }
    }
    bool active = power_source_can_off(), fallback = s5_fallback_ready();
    if (!active && !fallback) { klog("[power] forced off: no soft-off provider acpi_s5=%s", s5_fallback_state()); return -ENOTSUP; }
    if (fence) { klog("[power] forced off refused: devices not quiescent (error %d); turn the computer off with its power button", fence); return -EBUSY; }
    power_source_mark_emergency_stopped();
    int e = -ENOTSUP;
    if (active) {
        e = power_source_soft_off_forced();   /* the forced authorization, never the orderly entry (review 3, finding 4) */
        klog("[power] forced soft-off returned error=%d", e);
    }
    if (e && fallback) {
        /* fx-acpioff: the same S5 write on the emergency proof; a provider
         * without a forced entry (ENOTSUP) or whose Off returned is followed
         * by the register fallback, never preceded by it. */
        klog("[power] stage=acpi-s5-off provider=fallback forced=1 after=%d", e);
        int fallback_error = power_s5_fallback_off(true);
        klog("[power] forced acpi s5 fallback returned error=%d", fallback_error);
        if (fallback_error != -ENOTSUP) e = fallback_error;
    }
    return e;
}
/* Session desktop coordinates local actions, including Guest. SYSTEM Setup
 * and the fixed logon roles have their separate, bounded power routes. */
static bool power_authorized(const struct process *caller)
{
    return security_device_desktop(caller) ||
        (security_device_setup(caller) && setup_mode && setup_mode()) ||
        (security_device_system(caller) && (security_role(caller) == CIUKI_SECURITY_ROLE_LOGON ||
                                           security_role(caller) == CIUKI_SECURITY_ROLE_LOGON_UI));
}
/* G11 (docs/design/power-policy.md): the drive standby timer (ATA IDLE
 * E3h, programmed by drivers/ata.c on every fixed disk with the Power
 * Management feature set) and the cached EVENTS record. */
#pragma weak ata_standby_timer_all
#define POWER_DISK_MAX (5u * 3600u + 1800u) /* ATA count 251: 5 h 30 min */
static uint32_t disk_seconds = CIUKI_POWER_UNKNOWN;
static int power_policy_check(const struct ciuki_power_policy *p)
{
    return p->version != CIUKI_POWER_POLICY_VERSION || p->size != sizeof(*p) || p->reserved ||
           p->disk_seconds > POWER_DISK_MAX ? -EINVAL : 0;
}
int power_policy_apply(struct ciuki_power_policy *p)
{
    if (power_policy_check(p)) return -EINVAL;
    p->disks = p->refused = p->applied_seconds = 0; p->error = 0;
    if (!ata_standby_timer_all) return -ENODEV;
    struct ata_standby_result r;
    int fixed = ata_standby_timer_all(p->disk_seconds, &r);
    p->disks = r.disks; p->refused = r.refused; p->applied_seconds = r.applied_seconds; p->error = r.error;
    int e = fixed <= 0 ? -ENODEV : r.disks ? 0 : r.error;
    if (r.disks) disk_seconds = r.applied_seconds;
    klog("[power] policy disk_seconds=%u applied=%u disks=%u refused=%u error=%d result=%d",
         p->disk_seconds, p->applied_seconds, p->disks, p->refused, p->error, e);
    return e;
}
void power_events_fill(struct ciuki_power_events *out)
{
    struct power_source_counters c; power_source_counters(&c);
    out->sleep_requests = c.sleep_requests; out->battery_low = c.battery_low; out->critical = c.critical;
    out->resumes = c.resumes; out->status_changes = c.status_changes;
    out->standby_reason = power_source_standby_reason();
    out->disk_seconds = disk_seconds; out->reserved = 0;
}
/* Stand by only where the provider and every native DMA driver qualified
 * (power_source_standby_reason); the provider's standby owns the device
 * suspend/resume and must write dirty storage back without stopping it. */
static uint32_t standby_refusals;
static int32_t power_standby(void)
{
    uint32_t reason = power_source_standby_reason();
    if (reason != CIUKI_POWER_STANDBY_AVAILABLE) {
        if (standby_refusals++ < 4) klog("[power] standby refused reason=%u", reason);
        return -ENOTSUP;
    }
    int e = power_source_standby();
    klog("[power] standby result=%d", e);
    return e;
}
#ifndef POWER_HOST
/* POLICY returns at once: a drive in standby spins up for the IDLE command,
 * which must not stall the desktop. The "power-policy" task applies the
 * latest request; the call reports the last completed application and
 * EVENTS.disk_seconds the programmed time. */
static struct task *policy_worker;
static struct kwait policy_wake;
static bool policy_pending;
static struct ciuki_power_policy policy_request, policy_done = {.applied_seconds = CIUKI_POWER_UNKNOWN};
static bool policy_ready(void *arg) { (void)arg; return policy_pending; }
static void policy_main(void *arg)
{
    (void)arg;
    for (;;) {
        if (!kwait_wait_until(&policy_wake, policy_ready, NULL, UINT64_MAX)) continue;
        uint32_t f = irq_save(); struct ciuki_power_policy p = policy_request; policy_pending = false; irq_restore(f);
        int e = power_policy_apply(&p);
        if (e && !p.error) p.error = e;
        f = irq_save(); policy_done = p; irq_restore(f);
    }
}
static int policy_queue(struct ciuki_power_policy *p)
{
    if (power_policy_check(p)) return -EINVAL;
    uint32_t f = irq_save();
    bool start = !policy_worker;
    irq_restore(f);
    if (start) {
        kwait_init(&policy_wake);
        struct task *t = task_create_kernel("power-policy", policy_main, NULL, P_DEVICE);
        if (!t) return -ENOMEM;
        policy_worker = t; task_start(t);
    }
    f = irq_save();
    policy_request = *p; policy_pending = true;
    struct ciuki_power_policy done = policy_done;
    irq_restore(f);
    kwait_wake_all(&policy_wake);
    p->disks = done.disks; p->refused = done.refused;
    p->applied_seconds = done.applied_seconds; p->error = done.error;
    return 0;
}
int32_t power_record_call(uint32_t action, uint32_t address, uint32_t bytes)
{
    if (action == CIUKI_POWER_EVENTS) {
        struct ciuki_power_events out;
        if (bytes != sizeof(out)) return -EINVAL;
        int e = copy_from_user(&out, address, 2 * sizeof(uint32_t));
        if (e) return e;
        if (out.version != CIUKI_POWER_EVENTS_VERSION || out.size != sizeof(out)) return -EINVAL;
        power_events_fill(&out);
        return copy_to_user(address, &out, sizeof(out));
    }
    if (action != CIUKI_POWER_POLICY) return -EINVAL;
    if (!security_device_admin(proc_current())) return -EPERM;
    struct ciuki_power_policy p;
    if (bytes != sizeof(p)) return -EINVAL;
    int e = copy_from_user(&p, address, sizeof(p));
    if (e) return e;
    int r = policy_queue(&p);
    if (r) return r;
    return copy_to_user(address, &p, sizeof(p));
}
#endif
int32_t power_syscall(uint32_t action)
{
    struct process *caller = proc_current();
    /* Both take the same orderly path below: storage flush, native device
     * quiesce, the stopped latch, then reset or the active provider's
     * soft-off (ACPI S5 or APM Off). */
    if (action == CIUKI_POWER_QUERY)
        return power_progress | CIUKI_POWER_CAN_REBOOT | (power_can_off() ? CIUKI_POWER_CAN_OFF : 0) |
               (!(power_progress & (CIUKI_POWER_STOPPED | CIUKI_POWER_UNCERTAIN)) &&
                power_source_standby_reason() == CIUKI_POWER_STANDBY_AVAILABLE ? CIUKI_POWER_CAN_STANDBY : 0);
    if (!power_authorized(caller)) return -EPERM;
    if (security_device_system(caller) && !security_device_setup(caller) &&
        action != CIUKI_POWER_REBOOT && action != CIUKI_POWER_OFF) return -EPERM;
    if (action == CIUKI_POWER_STANDBY || action == CIUKI_POWER_CANCEL_STANDBY) {
        uint32_t f=irq_save(); bool busy=power_running || power_progress;
        if (!busy) power_running=true;
        irq_restore(f);
        if (busy) return -EBUSY;
        int e=action==CIUKI_POWER_CANCEL_STANDBY ? power_source_cancel_standby() : power_standby();
        f=irq_save(); power_running=false; irq_restore(f);
        return e;
    }
    if (action == CIUKI_POWER_FORCE_REBOOT || action == CIUKI_POWER_FORCE_OFF)
        return power_force(action == CIUKI_POWER_FORCE_OFF, caller->pid);
    if (action != CIUKI_POWER_REBOOT && action != CIUKI_POWER_OFF) return -EINVAL;
    bool off = action == CIUKI_POWER_OFF;
    uint32_t f = irq_save();
    bool busy = power_running;
    if (!busy) { power_running = true; power_requests++; }
    uint32_t attempt = power_requests;
    irq_restore(f);
    if (busy) { klog("[power] %s request by pid %u refused: sequence in progress", off ? "power off" : "reboot", caller->pid); return -EBUSY; }
    klog("[power] %s requested by pid %u attempt=%u progress=%u storage=%s", off ? "power off" : "reboot", caller->pid,
         attempt, power_progress, storage_flushed ? "durable" : "pending");
    int e = power_sequence(off);
    f = irq_save(); power_running = false; irq_restore(f);
    return e;
}
#endif
