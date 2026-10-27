/* Host test: ACPI soft-off discovery for system call 73 (core/power.c).
 * Fixtures model the QEMU i440fx tables (hw/i386/acpi-build.c: FADT with
 * PM1a_CNT 0x604, \_S5 = Package(4){Zero, Zero, Zero, Zero}) and an
 * ICH-style DSDT (Name(\_S5, Package(4){0x07, 0x07, Zero, Zero})).
 * SPDX-License-Identifier: GPL-2.0-only */
#define POWER_HOST 1
#include <stdio.h>
#include <string.h>
#include "../../src/kernel/core/power.c"
#include "../../src/kernel/core/apm_ports.c"   /* sec-wire: the fx-18 window from parsed blocks */

static unsigned failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

enum { MEM = 0x20000, RSDP = 0x0E0000 - 0xD0000, RSDT = 0x1000, FADT = 0x2000, DSDT = 0x4000, XSDT = 0x3000 };
static uint8_t mem[MEM];
static unsigned maps;

static const uint8_t *map(void *ctx, uint32_t phys, uint32_t len)
{
    (void)ctx;
    maps++;
    return (uint64_t)phys + len <= MEM ? mem + phys : 0;
}
static void put32(uint32_t at, uint32_t v) { for (int i = 0; i < 4; i++) mem[at + i] = (uint8_t)(v >> (8 * i)); }
static void fix(uint32_t at, uint32_t sum_at, uint32_t len)
{
    mem[sum_at] = 0;
    uint8_t s = 0;
    for (uint32_t i = 0; i < len; i++) s = (uint8_t)(s + mem[at + i]);
    mem[sum_at] = (uint8_t)-s;
}
static void header(uint32_t at, const char *sig, uint32_t len, uint8_t rev)
{
    memcpy(mem + at, sig, 4);
    put32(at + 4, len);
    mem[at + 8] = rev;
    memcpy(mem + at + 10, "CIUKI ", 6);
}
static void seal(uint32_t at) { fix(at, at + 9, mem[at + 4] | mem[at + 5] << 8 | mem[at + 6] << 16); }

/* rev 0 RSDP -> RSDT -> FADT (116 bytes) -> DSDT with the given body. */
static void tables(const uint8_t *aml, uint32_t aml_len, uint32_t pm1a, uint32_t pm1b)
{
    memset(mem, 0, sizeof(mem));
    memcpy(mem + RSDP, "RSD PTR ", 8);
    memcpy(mem + RSDP + 9, "CIUKI ", 6);
    put32(RSDP + 16, RSDT);
    fix(RSDP, RSDP + 8, 20);
    header(RSDT, "RSDT", 36 + 8, 1);
    put32(RSDT + 36, 0x1800);           /* an unrelated table first */
    header(0x1800, "APIC", 44, 1);
    seal(0x1800);
    put32(RSDT + 40, FADT);
    seal(RSDT);
    header(FADT, "FACP", 116, 1);
    put32(FADT + 40, DSDT);
    put32(FADT + 48, 0xB2);
    mem[FADT + 52] = 0xF1;
    put32(FADT + 64, pm1a);
    put32(FADT + 68, pm1b);
    mem[FADT + 88] = 4;                 /* PM1_EVT_LEN and PM1_CNT_LEN as every real FADT */
    mem[FADT + 89] = 2;
    seal(FADT);
    header(DSDT, "DSDT", 36 + aml_len, 1);
    memcpy(mem + DSDT + 36, aml, aml_len);
    seal(DSDT);
}

static int parse(struct acpi_soft_off *r, const char **why)
{
    memset(r, 0xEE, sizeof(*r));
    return acpi_soft_off_parse(map, 0, RSDP, r, why);
}

static void test_register_validation(const uint8_t *aml, unsigned size)
{
    static const struct { unsigned legacy, gas, bytes, access; const char *why; } blocks[] = {
        {64, 172, 2, 2, "pm1a_cnt"}, {68, 184, 2, 2, "pm1b_cnt"},
        {56, 148, 4, 2, "pm1a_evt"}, {60, 160, 4, 2, "pm1b_evt"},
        {80, 220, 4, 1, "gpe0"}, {84, 232, 4, 1, "gpe1"}
    };
    struct acpi_soft_off r;
    const char *why;
    for (unsigned b = 0; b < sizeof(blocks) / sizeof(blocks[0]); b++) {
        unsigned gas = blocks[b].gas;
        /* Each extended block is independent: all absent optional blocks
         * are accepted, all unsupported present blocks refuse. */
        tables(aml, size, 0x604, 0); header(FADT, "FACP", 244, 3);
        put32(FADT + blocks[b].legacy, 0);
        mem[FADT + 92] = mem[FADT + 93] = 4;
        mem[FADT + gas] = 1; mem[FADT + gas + 1] = blocks[b].bytes * 8;
        mem[FADT + gas + 3] = blocks[b].access;
        put32(FADT + gas + 4, 0x700); seal(FADT);
        CHECK(parse(&r, &why) == 0);
        mem[FADT + gas] = 0; seal(FADT); /* present SystemMemory */
        CHECK(parse(&r, &why) == -ENOTSUP && !strcmp(why, blocks[b].why));
        mem[FADT + gas] = 1;
        mem[FADT + gas + 2] = 1; seal(FADT);
        CHECK(parse(&r, &why) == -ENOTSUP && !strcmp(why, blocks[b].why));
        mem[FADT + gas + 2] = 0;
        mem[FADT + gas + 1] = 8; seal(FADT);
        CHECK(parse(&r, &why) == -ENOTSUP && !strcmp(why, blocks[b].why));
        mem[FADT + gas + 1] = blocks[b].bytes * 8;
        mem[FADT + gas + 3] = 3; seal(FADT);
        CHECK(parse(&r, &why) == -ENOTSUP && !strcmp(why, blocks[b].why));
        mem[FADT + gas + 3] = 0; seal(FADT); /* unspecified access is valid */
        CHECK(parse(&r, &why) == 0);
        put32(FADT + gas + 8, 1); seal(FADT);
        CHECK(parse(&r, &why) == -ENOTSUP && !strcmp(why, blocks[b].why));
        put32(FADT + gas + 8, 0);
        put32(FADT + gas + 4, 0x10000 - blocks[b].bytes); seal(FADT);
        CHECK(parse(&r, &why) == 0); /* last complete extent */
        put32(FADT + gas + 4, 0x10001 - blocks[b].bytes); seal(FADT);
        CHECK(parse(&r, &why) == -ENOTSUP && !strcmp(why, blocks[b].why));
        /* The same extent guard applies to legacy fields. */
        put32(FADT + gas + 4, 0);
        put32(FADT + blocks[b].legacy, 0x10000 - blocks[b].bytes); seal(FADT);
        CHECK(parse(&r, &why) == 0);
        put32(FADT + blocks[b].legacy, 0x10001 - blocks[b].bytes); seal(FADT);
        CHECK(parse(&r, &why) == -ENOTSUP && !strcmp(why, blocks[b].why));
        put32(FADT + blocks[b].legacy, 0x10000); seal(FADT);
        CHECK(parse(&r, &why) == -ENOTSUP && !strcmp(why, blocks[b].why));
    }
    /* A present unsupported extended PM1b must not be hidden by legacy IO. */
    tables(aml, size, 0x604, 0x644); header(FADT, "FACP", 244, 3);
    mem[FADT + 185] = 16; put32(FADT + 188, 0x700); seal(FADT);
    CHECK(parse(&r, &why) == -ENOTSUP && !strcmp(why, "pm1b_cnt"));
    mem[FADT + 184] = 1; seal(FADT);
    CHECK(parse(&r, &why) == 0 && r.pm1b_cnt == 0x700);
    tables(aml, size, 0x604, 0); put32(FADT + 48, 0x10000); seal(FADT);
    CHECK(parse(&r, &why) == -ENOTSUP && !strcmp(why, "smi_cmd"));
    tables(aml, size, 0x604, 0); mem[FADT + 89] = 4; seal(FADT);
    CHECK(parse(&r, &why) == -ENOTSUP && !strcmp(why, "pm1_cnt_len"));
    puts("PASS Review round 1 P2: absent/valid/unsupported GAS, independent blocks, metadata, precedence and full legacy/GAS I/O extents");
}
/* sec-wire (QEMU run of image ae977fbe, SeaBIOS 1.17, 512 MiB i440fx:
 * "[power] acpi s5 fallback unavailable step=rsdt", "[apm] window ranges=2
 * source=none", then APM Off denied at port 604h). SeaBIOS copies the RSDP
 * to the F segment and every other table to its ZoneHigh, an E820
 * reserved range at the top of RAM below 4 GiB that starts where the
 * usable range ends. The map applies the production firmware-extent rule
 * to this exact E820 table; the memory behind it is sparse. */
enum { SB_FSEG = 0xF0000, SB_RSDP = 0xF5A50, SB_TAIL = 0x1FFE0000, SB_TAIL_LEN = 0x20000,
       SB_FACS = SB_TAIL, SB_DSDT = SB_TAIL + 0x40, SB_FADT = SB_TAIL + 0x1C99, SB_APIC = SB_TAIL + 0x1D0D,
       SB_RSDT = SB_TAIL + 0x1E2D };
static uint8_t sb_fseg[0x10000], sb_tail[SB_TAIL_LEN];
static const struct ciuki_e820 sb_e820[] = {
    {0x0, 0x9FC00, 1, 1}, {0x9FC00, 0x400, 2, 1}, {0xF0000, 0x10000, 2, 1},
    {0x100000, SB_TAIL - 0x100000, 1, 1}, {SB_TAIL, SB_TAIL_LEN, 2, 1},
    {0xFEFFC000, 0x4000, 2, 1}, {0xFFFC0000, 0x40000, 2, 1}};
static bool sb_tail_rule;
static unsigned sb_refused;
static const uint8_t *sb_map(void *ctx, uint32_t phys, uint32_t len)
{
    (void)ctx;
    if (!acpi_firmware_extent(sb_e820, sizeof(sb_e820) / sizeof(sb_e820[0]), phys, len, sb_tail_rule)) {
        sb_refused++; return 0;
    }
    if (phys >= SB_FSEG && (uint64_t)phys + len <= SB_FSEG + sizeof(sb_fseg)) return sb_fseg + (phys - SB_FSEG);
    if (phys >= SB_TAIL && (uint64_t)phys + len <= SB_TAIL + sizeof(sb_tail)) return sb_tail + (phys - SB_TAIL);
    return 0;
}
static uint8_t *sb_at(uint32_t phys) { return phys >= SB_TAIL ? sb_tail + (phys - SB_TAIL) : sb_fseg + (phys - SB_FSEG); }
static void sb_put32(uint32_t phys, uint32_t v) { for (int i = 0; i < 4; i++) sb_at(phys)[i] = (uint8_t)(v >> (8 * i)); }
static void sb_header(uint32_t phys, const char *sig, uint32_t len, uint8_t rev)
{ memcpy(sb_at(phys), sig, 4); sb_put32(phys + 4, len); sb_at(phys)[8] = rev; memcpy(sb_at(phys) + 10, "BOCHS ", 6); }
static void sb_seal(uint32_t phys, uint32_t sum_at, uint32_t len)
{
    uint8_t *t = sb_at(phys), x = 0; t[sum_at] = 0;
    for (uint32_t i = 0; i < len; i++) x = (uint8_t)(x + t[i]);
    t[sum_at] = (uint8_t)-x;
}
static void sb_tables(void)
{
    static const uint8_t s5[] = { 0x10, 0x0F, 0x5C, 0x00, 0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x04, 0, 0, 0, 0, 0xA3 };
    memset(sb_fseg, 0, sizeof(sb_fseg)); memset(sb_tail, 0, sizeof(sb_tail));
    memcpy(sb_at(SB_FACS), "FACS", 4); sb_put32(SB_FACS + 4, 64);
    sb_header(SB_DSDT, "DSDT", 36 + sizeof(s5), 1); memcpy(sb_at(SB_DSDT) + 36, s5, sizeof(s5)); sb_seal(SB_DSDT, 9, 36 + sizeof(s5));
    /* QEMU acpi-build.c (PIIX4 PM at 600h): PM1a_EVT 600h/4, PM1a_CNT 604h/2,
     * PM_TMR 608h/4, GPE0 AFE0h/4, SMI_CMD B2h, ACPI_ENABLE F1h, SCI 9. */
    sb_header(SB_FADT, "FACP", 116, 1);
    sb_put32(SB_FADT + 36, SB_FACS); sb_put32(SB_FADT + 40, SB_DSDT);
    sb_at(SB_FADT)[46] = 9; sb_put32(SB_FADT + 48, 0xB2); sb_at(SB_FADT)[52] = 0xF1; sb_at(SB_FADT)[53] = 0xF0;
    sb_put32(SB_FADT + 56, 0x600); sb_put32(SB_FADT + 64, 0x604); sb_put32(SB_FADT + 76, 0x608); sb_put32(SB_FADT + 80, 0xAFE0);
    sb_at(SB_FADT)[88] = 4; sb_at(SB_FADT)[89] = 2; sb_at(SB_FADT)[91] = 4; sb_at(SB_FADT)[92] = 4;
    sb_seal(SB_FADT, 9, 116);
    sb_header(SB_APIC, "APIC", 44, 1); sb_seal(SB_APIC, 9, 44);
    sb_header(SB_RSDT, "RSDT", 36 + 8, 1); sb_put32(SB_RSDT + 36, SB_FADT); sb_put32(SB_RSDT + 40, SB_APIC);
    sb_seal(SB_RSDT, 9, 44);
    memcpy(sb_at(SB_RSDP), "RSD PTR ", 8); memcpy(sb_at(SB_RSDP) + 9, "BOCHS ", 6);
    sb_put32(SB_RSDP + 16, SB_RSDT); sb_seal(SB_RSDP, 8, 20);
}
static void test_seabios(void)
{
    struct acpi_soft_off r; struct apm_pm_blocks b; const char *why;
    sb_tables();
    /* The QEMU record: the ACPICA/OSL rule refuses the reserved tail. */
    sb_tail_rule = false; sb_refused = 0;
    CHECK(acpi_soft_off_parse(sb_map, 0, SB_RSDP, &r, &why) == -ENOTSUP && !strcmp(why, "rsdt") && sb_refused);
    CHECK(acpi_fadt_pm_blocks(sb_map, 0, SB_RSDP, &b, &why) == -ENOTSUP && !strcmp(why, "rsdt"));
    /* The power windows: the tables, \_S5 and the fixed-hardware blocks. */
    sb_tail_rule = true; sb_refused = 0;
    CHECK(acpi_soft_off_parse(sb_map, 0, SB_RSDP, &r, &why) == 0 && !strcmp(why, "ok") && !sb_refused);
    CHECK(r.pm1a_cnt == 0x604 && !r.pm1b_cnt && r.slp_typa == 0 && r.slp_typb == 0 && r.source == ACPI_S5_SOURCE_DSDT);
    CHECK(r.smi_cmd == 0xB2 && r.acpi_enable == 0xF1 && r.pm1a_evt == 0x600 && r.gpe0 == 0xAFE0 && r.sci_int == 9);
    memset(&b, 0xEE, sizeof(b));
    CHECK(acpi_fadt_pm_blocks(sb_map, 0, SB_RSDP, &b, &why) == 0 && !strcmp(why, "ok"));
    CHECK(b.pm1a_cnt == 0x604 && b.pm1_cnt_len == 2 && !b.pm1b_cnt && b.pm1a_evt == 0x600 && b.pm1_evt_len == 4);
    CHECK(b.pm_tmr == 0x608 && b.pm_tmr_len == 4 && b.gpe0 == 0xAFE0 && b.gpe0_len == 4 && !b.gpe1 && !b.pm2_cnt);
    CHECK(b.smi_cmd == 0xB2);
    /* fx-18 window from these blocks: SeaBIOS apm_shutdown's outw(2000h,
     * 604h) is admitted for Set Power State Off only; SCI_EN never; the PM
     * timer read-only; nothing outside the FADT blocks. */
    struct apm_port_window w; uint8_t k; const char *reason;
    apm_port_window_build(&w, &b, 0, 0);
    CHECK(w.source == APM_PORT_SOURCE_FADT);
    CHECK(apm_port_check(&w, 0x604, 2, true, 0x2000, 0x5307, &k, &reason) == APM_PORT_ALLOW && k == APM_PORT_PM1_CNT);
    CHECK(apm_port_check(&w, 0x604, 2, true, 0x2000, 0x5300, &k, &reason) == APM_PORT_DENY && !strcmp(reason, "slp_en"));
    CHECK(apm_port_check(&w, 0x604, 2, true, 0x0001, 0x5307, &k, &reason) == APM_PORT_DENY && !strcmp(reason, "sci_en"));
    CHECK(apm_port_check(&w, 0x608, 4, false, 0, 0x530a, &k, &reason) == APM_PORT_ALLOW && k == APM_PORT_PM_TMR);
    CHECK(apm_port_check(&w, 0x608, 4, true, 0, 0x530a, &k, &reason) == APM_PORT_DENY);
    CHECK(apm_port_check(&w, 0x610, 1, true, 0, 0x5307, &k, &reason) == APM_PORT_DENY && !strcmp(reason, "outside_window"));
    /* Bounds of the tail rule: device windows, oversize, not adjacent to
     * RAM, above 4 GiB, straddling, and the narrower ACPICA rule. */
    const unsigned n = sizeof(sb_e820) / sizeof(sb_e820[0]);
    CHECK(acpi_firmware_extent(sb_e820, n, SB_RSDT, 44, true) && !acpi_firmware_extent(sb_e820, n, SB_RSDT, 44, false));
    CHECK(acpi_firmware_extent(sb_e820, n, SB_RSDP, 20, false));             /* F segment: both rules */
    CHECK(!acpi_firmware_extent(sb_e820, n, 0xFEFFC000, 36, true));          /* not adjacent to RAM */
    CHECK(!acpi_firmware_extent(sb_e820, n, 0xFFFC0000, 36, true));          /* BIOS flash */
    CHECK(!acpi_firmware_extent(sb_e820, n, SB_TAIL - 16, 36, true));        /* straddles usable RAM */
    CHECK(!acpi_firmware_extent(sb_e820, n, SB_TAIL + SB_TAIL_LEN - 8, 36, true)); /* past the tail */
    CHECK(!acpi_firmware_extent(sb_e820, n, 0x200000, 36, true));            /* kernel RAM */
    CHECK(!acpi_firmware_extent(sb_e820, n, 0, 0, true) && !acpi_firmware_extent(sb_e820, n, 0xFFFFFFF0u, 36, true));
    struct ciuki_e820 big[] = {{0x100000, 0x7EF00000, 1, 1}, {0x7F000000, 0x1000000 + 0x1000, 2, 1}};
    CHECK(!acpi_firmware_extent(big, 2, 0x7F000000, 36, true));            /* above BUILD_MAX_HIGHTABLE */
    big[1].length = 0x1000000;
    CHECK(acpi_firmware_extent(big, 2, 0x7FFFFFC0, 36, true));
    struct ciuki_e820 high[] = {{0x100000, 0xFFE00000, 1, 1}, {0xFFF00000, 0x200000, 2, 1}};
    CHECK(!acpi_firmware_extent(high, 2, 0xFFF00000, 36, true));            /* the range runs past 4 GiB */
    struct ciuki_e820 acpi3[] = {{0x100000, 0x1FEE0000, 1, 1}, {0x1FFE0000, 0x20000, 3, 1}};
    CHECK(acpi_firmware_extent(acpi3, 2, 0x1FFE1000, 36, false));            /* T23/VirtualBox shape unchanged */
    puts("PASS sec-wire SeaBIOS tables: RSDT/FADT/DSDT in the E820 reserved RAM tail parsed for S5 and the APM window (604h admitted for 5307 only); device windows, oversize, non-adjacent and straddling ranges refused");
}
int main(void)
{
    struct acpi_soft_off r;
    const char *why;
    /* QEMU: Scope(\) { Name(_S5, Package(4){Zero, Zero, Zero, Zero}) }. */
    static const uint8_t qemu[] = { 0x10, 0x0F, 0x5C, 0x00, 0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x04, 0, 0, 0, 0, 0xA3 };
    tables(qemu, sizeof(qemu), 0x604, 0);
    CHECK(parse(&r, &why) == 0 && !strcmp(why, "ok"));
    CHECK(r.pm1a_cnt == 0x604 && r.pm1b_cnt == 0 && r.slp_typa == 0 && r.slp_typb == 0);
    CHECK(r.smi_cmd == 0xB2 && r.acpi_enable == 0xF1 && r.fadt_revision == 1);

    /* ICH style: Name(\_S5, Package(4){0x07, 0x07, Zero, Zero}), after a
     * method that only references \_S5_ (no NameOp: skipped). */
    static const uint8_t ich[] = { 0x70, 0x5C, '_', 'S', '5', '_', 0x60,
                                   0x08, 0x5C, '_', 'S', '5', '_', 0x12, 0x08, 0x04, 0x0A, 0x07, 0x0A, 0x07, 0, 0 };
    tables(ich, sizeof(ich), 0x1004, 0x1044);
    CHECK(parse(&r, &why) == 0 && r.slp_typa == 7 && r.slp_typb == 7);
    CHECK(r.pm1a_cnt == 0x1004 && r.pm1b_cnt == 0x1044);

    /* A zero PkgLength cannot contain any elements (old parser accepted it). */
    static const uint8_t wide[] = { 0x08, '_', 'S', '5', '_', 0x12, 0x40, 0x00, 0x01, 0x01 };
    uint8_t a = 9, b = 9;
    CHECK(acpi_s5_find(wide, sizeof(wide), &a, &b) == -ENOENT && a == 9 && b == 9);
    static const uint8_t escaped[] = {0x08, '_','S','5','_', 0x12, 0x03, 0x02, 0x01, 0x01};
    CHECK(acpi_s5_find(escaped, sizeof(escaped), &a, &b) == -ENOENT);
    static const uint8_t buffer[] = {0x08, 'B','U','F','0', 0x11, 0x0F, 0x0A, 0x0B,
        0x08, '_','S','5','_', 0x12, 0x06, 0x04, 0, 0, 0, 0};
    CHECK(acpi_s5_find(buffer, sizeof(buffer), &a, &b) == -ENOENT);
    static const uint8_t nested[] = {0x10, 0x10, '_','S','B','_',
        0x08, '_','S','5','_', 0x12, 0x06, 0x04, 0, 0, 0, 0};
    CHECK(acpi_s5_find(nested, sizeof(nested), &a, &b) == -ENOENT);
    static const uint8_t nested_absolute[] = {0x10, 0x11, '_','S','B','_',
        0x08, 0x5C, '_','S','5','_', 0x12, 0x06, 0x04, 0, 0, 0, 0};
    CHECK(acpi_s5_find(nested_absolute, sizeof(nested_absolute), &a, &b) == -ENOENT);
    /* Non-minimal but valid multi-byte lengths, complete two-element package. */
    static const uint8_t wide_valid[] = {0x08,'_','S','5','_',0x12,0x45,0,2,1,0};
    CHECK(!acpi_s5_find(wide_valid,sizeof(wide_valid),&a,&b) && a==1 && b==0);
    for (unsigned i=0;i<sizeof(wide_valid);i++) CHECK(acpi_s5_find(wide_valid,i,&a,&b)==-ENOENT);
    static const uint8_t one[] = {0x08,'_','S','5','_',0x12,3,1,1};
    CHECK(acpi_s5_find(one,sizeof(one),&a,&b)==-ENOENT);
    /* Fake signatures in data are skipped even when a real root Name follows. */
    uint8_t sequence[sizeof(buffer)+sizeof(wide_valid)];
    memcpy(sequence,buffer,sizeof(buffer)); memcpy(sequence+sizeof(buffer),wide_valid,sizeof(wide_valid));
    CHECK(!acpi_s5_find(sequence,sizeof(sequence),&a,&b) && a==1 && b==0);

    /* Refusals: out-of-range value, truncation, missing object. */
    static const uint8_t big[] = { 0x08, '_', 'S', '5', '_', 0x12, 0x05, 0x02, 0x0A, 0x08, 0x00 };
    CHECK(acpi_s5_find(big, sizeof(big), &a, &b) == -ENOENT);
    static const uint8_t cut[] = { 0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x04, 0x0A };
    CHECK(acpi_s5_find(cut, sizeof(cut), &a, &b) == -ENOENT);
    static const uint8_t none[] = { 0x08, '_', 'S', '4', '_', 0x12, 0x06, 0x04, 0, 0, 0, 0 };
    tables(none, sizeof(none), 0x604, 0);
    CHECK(parse(&r, &why) == -ENOTSUP && !strcmp(why, "s5"));

    /* Checksums and signatures are verified at every level. */
    tables(qemu, sizeof(qemu), 0x604, 0);
    mem[DSDT + 40]++;
    CHECK(parse(&r, &why) == -ENOTSUP && !strcmp(why, "dsdt"));
    tables(qemu, sizeof(qemu), 0x604, 0);
    mem[FADT + 64]++;
    CHECK(parse(&r, &why) == -ENOTSUP && !strcmp(why, "fadt"));
    tables(qemu, sizeof(qemu), 0x604, 0);
    mem[RSDP + 16]++;
    CHECK(parse(&r, &why) == -ENOTSUP && !strcmp(why, "rsdp"));
    tables(qemu, sizeof(qemu), 0, 0);
    CHECK(parse(&r, &why) == -ENOTSUP && !strcmp(why, "pm1a_cnt"));
    CHECK(acpi_soft_off_parse(map, 0, 0, &r, &why) == -ENOTSUP && !strcmp(why, "rsdp"));
    /* A DSDT length beyond the mapped memory is refused, never read. */
    tables(qemu, sizeof(qemu), 0x604, 0);
    put32(DSDT + 4, 0x40000);
    CHECK(parse(&r, &why) == -ENOTSUP && !strcmp(why, "dsdt"));

    /* ACPI 2.0: RSDT absent, XSDT with 64-bit entries; X_PM1a_CNT_BLK in
     * SystemIO when the legacy field is zero (FADT revision 3, 244 bytes). */
    tables(qemu, sizeof(qemu), 0, 0);
    mem[RSDP + 15] = 2;
    put32(RSDP + 16, 0);
    put32(RSDP + 20, 36);
    put32(RSDP + 24, XSDT);
    fix(RSDP, RSDP + 8, 20);
    fix(RSDP, RSDP + 32, 36);
    header(XSDT, "XSDT", 36 + 8, 1);
    put32(XSDT + 36, FADT);
    seal(XSDT);
    header(FADT, "FACP", 244, 3);
    put32(FADT + 40, DSDT);
    mem[FADT + 172] = 1;                /* SystemIO */
    mem[FADT + 173] = 16;
    put32(FADT + 176, 0x604);
    seal(FADT);
    CHECK(parse(&r, &why) == 0 && r.pm1a_cnt == 0x604 && r.fadt_revision == 3);
    mem[FADT + 172] = 0;                /* SystemMemory is not an I/O port */
    seal(FADT);
    CHECK(parse(&r, &why) == -ENOTSUP && !strcmp(why, "pm1a_cnt"));

    /* fx-acpioff: the fixed-hardware blocks the S5 fallback clears, the
     * FADT variants of real machines, and \_S5 packages in an SSDT. */
    tables(qemu, sizeof(qemu), 0x604, 0);
    put32(FADT + 56, 0x600); put32(FADT + 60, 0x640); put32(FADT + 80, 0x60C); put32(FADT + 84, 0x620);
    mem[FADT + 46] = 9; mem[FADT + 53] = 0xF0; mem[FADT + 92] = 4; mem[FADT + 93] = 8; put32(FADT + 112, 0xA5);
    seal(FADT);
    CHECK(parse(&r, &why) == 0 && r.source == ACPI_S5_SOURCE_DSDT);
    CHECK(r.pm1a_evt == 0x600 && r.pm1b_evt == 0x640 && r.gpe0 == 0x60C && r.gpe1 == 0x620);
    CHECK(r.pm1_evt_len == 4 && r.pm1_cnt_len == 2 && r.gpe0_len == 4 && r.gpe1_len == 8);
    CHECK(r.sci_int == 9 && r.acpi_disable == 0xF0 && r.flags == 0xA5);
    /* SMI_CMD zero (always ACPI mode) parses; the handshake fields are zero. */
    tables(qemu, sizeof(qemu), 0x604, 0);
    put32(FADT + 48, 0); mem[FADT + 52] = 0; seal(FADT);
    CHECK(parse(&r, &why) == 0 && !r.smi_cmd && !r.acpi_enable && r.pm1a_cnt == 0x604 && !r.pm1b_cnt);
    /* PM1_CNT_LEN below 2 or a hardware-reduced FADT (revision 5+, flag bit 20) refuse. */
    tables(qemu, sizeof(qemu), 0x604, 0);
    mem[FADT + 89] = 1; seal(FADT);
    CHECK(parse(&r, &why) == -ENOTSUP && !strcmp(why, "pm1_cnt_len"));
    tables(qemu, sizeof(qemu), 0x604, 0);
    mem[FADT + 8] = 5; put32(FADT + 112, 1u << 20); seal(FADT);
    CHECK(parse(&r, &why) == -ENOTSUP && !strcmp(why, "hw_reduced"));
    tables(qemu, sizeof(qemu), 0x604, 0);
    put32(FADT + 112, 1u << 20); seal(FADT);   /* the bit is reserved before revision 5 */
    CHECK(parse(&r, &why) == 0);
    /* Two-element package with Zero/One, and a four-element package with
     * BytePrefix values, as real BIOSes emit them. */
    static const uint8_t two[] = { 0x08, '_', 'S', '5', '_', 0x12, 0x04, 0x02, 0x01, 0x00 };
    tables(two, sizeof(two), 0x604, 0);
    CHECK(parse(&r, &why) == 0 && r.slp_typa == 1 && r.slp_typb == 0);
    static const uint8_t four[] = { 0x08, 0x5C, '_', 'S', '5', '_', 0x12, 0x0A, 0x04, 0x0A, 0x05, 0x0A, 0x05, 0x0A, 0x00, 0x0A, 0x00 };
    tables(four, sizeof(four), 0x604, 0x644);
    CHECK(parse(&r, &why) == 0 && r.slp_typa == 5 && r.slp_typb == 5 && r.pm1b_cnt == 0x644);
    /* \_S5 in an SSDT: the DSDT has none; the root table names an APIC, a
     * bad-checksum SSDT (skipped), an SSDT without \_S5 and the one with it.
     * On the "s5" failure the FADT description is still returned. */
    static const uint8_t ssdt_other[] = { 0x08, '_', 'S', '3', '_', 0x12, 0x06, 0x04, 0x05, 0x05, 0, 0 };
    enum { SSDT_A = 0x8000, SSDT_B = 0x9000, SSDT_C = 0xA000 };
    tables(none, sizeof(none), 0x604, 0);
    header(RSDT, "RSDT", 36 + 20, 1);
    put32(RSDT + 44, SSDT_A); put32(RSDT + 48, SSDT_B); put32(RSDT + 52, SSDT_C); seal(RSDT);
    header(SSDT_A, "SSDT", 36 + sizeof(ich), 1); memcpy(mem + SSDT_A + 36, ich, sizeof(ich)); seal(SSDT_A); mem[SSDT_A + 40]++;
    header(SSDT_B, "SSDT", 36 + sizeof(ssdt_other), 1); memcpy(mem + SSDT_B + 36, ssdt_other, sizeof(ssdt_other)); seal(SSDT_B);
    header(SSDT_C, "SSDT", 36 + sizeof(two), 1); memcpy(mem + SSDT_C + 36, two, sizeof(two)); seal(SSDT_C);
    CHECK(parse(&r, &why) == 0 && r.source == ACPI_S5_SOURCE_SSDT && r.slp_typa == 1 && r.slp_typb == 0);
    /* The DSDT wins over an SSDT when both define it. */
    header(DSDT, "DSDT", 36 + sizeof(ich), 1); memcpy(mem + DSDT + 36, ich, sizeof(ich)); seal(DSDT);
    CHECK(parse(&r, &why) == 0 && r.source == ACPI_S5_SOURCE_DSDT && r.slp_typa == 7);
    /* A truncated SSDT (length beyond the mapped memory) is skipped. */
    header(DSDT, "DSDT", 36 + sizeof(none), 1); memcpy(mem + DSDT + 36, none, sizeof(none)); seal(DSDT);
    put32(SSDT_C + 4, 0x40000);
    CHECK(parse(&r, &why) == -ENOTSUP && !strcmp(why, "s5"));
    CHECK(r.pm1a_cnt == 0x604 && r.source == ACPI_S5_SOURCE_NONE && r.smi_cmd == 0xB2);
    /* No \_S5 anywhere, no SSDTs: the FADT description still comes back. */
    tables(none, sizeof(none), 0x604, 0x644);
    CHECK(parse(&r, &why) == -ENOTSUP && !strcmp(why, "s5") && r.pm1b_cnt == 0x644 && r.source == ACPI_S5_SOURCE_NONE);

    test_register_validation(qemu, sizeof(qemu));
    test_seabios();
    printf("power: acpi soft-off parser, %u failures (%u maps)\n", failures, maps);
    return failures ? 1 : 0;
}
