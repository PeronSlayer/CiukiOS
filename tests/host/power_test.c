/* Host test: ACPI soft-off discovery for system call 73 (core/power.c).
 * Fixtures model the QEMU i440fx tables (hw/i386/acpi-build.c: FADT with
 * PM1a_CNT 0x604, \_S5 = Package(4){Zero, Zero, Zero, Zero}) and an
 * ICH-style DSDT (Name(\_S5, Package(4){0x07, 0x07, Zero, Zero})).
 * SPDX-License-Identifier: GPL-2.0-only */
#define POWER_HOST 1
#include <stdio.h>
#include <string.h>
#include "../../src/kernel/core/power.c"

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

    printf("power: acpi soft-off parser, %u failures (%u maps)\n", failures, maps);
    return failures ? 1 : 0;
}
