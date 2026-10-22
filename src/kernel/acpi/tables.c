/* Validated, reserved firmware bytes only. SPDX-License-Identifier: GPL-2.0-only */
#include "internal.h"
#include "acclib.h"

struct fw_table fw_tables[FW_MAX];
unsigned fw_table_count;
uint32_t fw_rsdp;
static uint32_t le32(const uint8_t *p)
{ return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static bool checksum(const uint8_t *p, uint32_t n)
{ uint8_t sum = 0; while (n--) sum += *p++; return !sum; }
static ACPI_STATUS remember(uint32_t a, uint32_t n)
{
    for (unsigned i = 0; i < fw_table_count; i++)
        if (fw_tables[i].address == a) return fw_tables[i].length == n ? AE_OK : AE_BAD_HEADER;
    if (fw_table_count == FW_MAX) return AE_LIMIT;
    fw_tables[fw_table_count++] = (struct fw_table){a, n}; return AE_OK;
}
static const uint8_t *table(uint64_t a, const char *signature, ACPI_STATUS *status)
{
    *status = AE_BAD_ADDRESS;
    if (!a || a > UINT32_MAX - 36) return NULL;
    const uint8_t *p = fw_os->table_map(a, 36);
    if (!p) return NULL;
    uint32_t n = le32(p + 4);
    bool facs = !memcmp((void *)p, "FACS", 4);
    bool valid = (!signature || !memcmp((void *)p, (void *)signature, 4)) && n >= (facs ? 64u : 36u) &&
                 n <= FW_TABLE_MAX && a + n <= UINT32_MAX;
    fw_os->table_unmap(p, 36);
    *status = AE_BAD_HEADER;
    if (!valid) return NULL;
    p = fw_os->table_map(a, n);
    if (!p) { *status = AE_BAD_ADDRESS; return NULL; }
    if (le32(p + 4) != n || (signature && memcmp((void *)p, (void *)signature, 4)) || (!facs && !checksum(p, n))) {
        fw_os->table_unmap(p, n); *status = AE_BAD_CHECKSUM; return NULL;
    }
    *status = remember(a, n);
    if (*status) { fw_os->table_unmap(p, n); return NULL; }
    return p;
}
ACPI_STATUS fw_tables_discover(uint32_t rsdp)
{
    fw_table_count = 0; fw_rsdp = 0;
    if (!rsdp || rsdp > UINT32_MAX - 36) return AE_NOT_FOUND;
    const uint8_t *p = fw_os->table_map(rsdp, 36);
    if (!p) return AE_BAD_ADDRESS;
    bool valid = !memcmp((void *)p, "RSD PTR ", 8) && checksum(p, 20);
    uint8_t revision = p[15]; uint32_t n = revision >= 2 ? le32(p + 20) : 36;
    uint64_t root = revision >= 2 && le64(p + 24) ? le64(p + 24) : le32(p + 16);
    bool xsdt = revision >= 2 && le64(p + 24);
    fw_os->table_unmap(p, 36);
    if (!valid || n < 36 || n > 4096) return AE_BAD_HEADER;
    p = fw_os->table_map(rsdp, n);
    if (!p) return AE_BAD_ADDRESS;
    valid = revision < 2 || checksum(p, n);
    fw_os->table_unmap(p, n);
    if (!valid) return AE_BAD_CHECKSUM;
    ACPI_STATUS status = remember(rsdp, n);
    if (status) return status;
    p = table(root, xsdt ? "XSDT" : "RSDT", &status);
    if (!p) return status;
    uint32_t length = le32(p + 4), stride = xsdt ? 8 : 4;
    if ((length - 36) % stride || (length - 36) / stride > FW_MAX - 4) {
        fw_os->table_unmap(p, length); return AE_BAD_HEADER;
    }
    /* Retain addresses, not a borrowed root pointer across further mappings. */
    uint64_t addresses[FW_MAX]; unsigned count = (length - 36) / stride;
    for (unsigned i = 0; i < count; i++) addresses[i] = xsdt ? le64(p + 36 + stride * i) : le32(p + 36 + stride * i);
    fw_os->table_unmap(p, length);
    bool fadt_seen = false;
    for (unsigned i = 0; i < count; i++) {
        p = table(addresses[i], NULL, &status);
        if (!p) return status;
        length = le32(p + 4);
        bool fadt = !memcmp((void *)p, "FACP", 4);
        uint64_t dsdt = 0, facs = 0;
        if (fadt && length >= 116) {
            fadt_seen = true; dsdt = le32(p + 40); facs = le32(p + 36);
            if (length >= 148 && le64(p + 140)) dsdt = le64(p + 140);
            if (length >= 140 && le64(p + 132)) facs = le64(p + 132);
        }
        fw_os->table_unmap(p, length);
        if (fadt) {
            if (!dsdt) return AE_BAD_HEADER;
            p = table(dsdt, "DSDT", &status); if (!p) return status;
            length = le32(p + 4); fw_os->table_unmap(p, length);
            if (facs) {
                p = table(facs, "FACS", &status); if (!p) return status;
                length = le32(p + 4); fw_os->table_unmap(p, length);
            }
        }
    }
    if (!fadt_seen) return AE_NO_ACPI_TABLES;
    fw_rsdp = rsdp; return AE_OK;
}
