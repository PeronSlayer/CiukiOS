/* Generation-aware delegation into the existing authority; no claims made.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "internal.h"
#include "acclib.h"
#include <ciuki/registry.h>
static struct {
    bool used;
    unsigned space;
    int handle;
    uint32_t generation;
    ACPI_PCI_ID pci;
} grants[16];
static bool owner(const struct resource *r)
{ return r && r->owner && (!strcmp(r->owner, "acpi-inventory") || !strcmp(r->owner, "acpi-power")) && r->state == RS_ACTIVE; }
void fw_regions_reset(void) { memset(grants, 0, sizeof(grants)); }
int fw_region_grant(unsigned space, int handle, uint32_t generation, const ACPI_PCI_ID *pci)
{
    if (space > 2 || handle < 0 || !registry_valid(handle, generation)) return -22;
    const struct resource *r = registry_get(handle);
    if (!owner(r) || r->generation != generation || r->type != (space == 1 ? RES_MMIO : RES_PORT)) return -22;
    if (space == 2 && (!pci || pci->Segment || pci->Bus > 255 || pci->Device > 31 || pci->Function > 7 ||
                      r->start > 0xcf8 || r->end < 0xd00)) return -22;
    for (unsigned i = 0; i < 16; i++) if (!grants[i].used) {
        grants[i].used = true; grants[i].space = space; grants[i].handle = handle;
        grants[i].generation = generation; if (pci) grants[i].pci = *pci; return 0;
    }
    return -28;
}
bool fw_region_check(unsigned space, uint64_t a, unsigned width, const ACPI_PCI_ID *pci)
{
    if (space > 2 || (width != 8 && width != 16 && width != 32 && (width != 64 || space != 1))) return false;
    uint64_t end = a + width / 8;
    if (a > UINT32_MAX || end > (space == 0 ? 65536ull : space == 2 ? 256ull : UINT32_MAX)) return false;
    if (space == 2 && (!pci || pci->Segment || pci->Bus > 255 || pci->Device > 31 || pci->Function > 7 || a % (width / 8))) return false;
    for (unsigned i = 0; i < 16; i++) if (grants[i].used && grants[i].space == space) {
        if (!registry_valid(grants[i].handle, grants[i].generation)) continue;
        const struct resource *r = registry_get(grants[i].handle);
        if (!owner(r) || r->generation != grants[i].generation) continue;
        if (space == 2) {
            if (memcmp((void *)pci, &grants[i].pci, sizeof(*pci))) continue;
            bool present = false;
            for (unsigned k = 0; k < pci_count(); k++) {
                const struct pci_func *p = pci_get(k);
                if (p && p->bus == pci->Bus && p->dev == pci->Device && p->fn == pci->Function) present = true;
            }
            if (present && r->start <= 0xcf8 && r->end >= 0xd00) return true;
        } else if (r->start <= a && r->end >= end) return true;
    }
    return false;
}
