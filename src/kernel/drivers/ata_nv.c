// SPDX-License-Identifier: GPL-2.0-only
/*
 * pata_amd.c 	- AMD PATA for new ATA layer
 *			  (C) 2005-2006 Red Hat Inc
 *
 *  Based on pata-sil680. Errata information is taken from data sheets
 *  and the amd74xx.c driver by Vojtech Pavlik. Nvidia SATA devices are
 *  claimed by sata-nv.c.
 *
 *  TODO:
 *	Variable system clock when/if it makes sense
 *	Power management on ports
 *
 *
 *  Documentation publicly available.
 */
/* Ciuki adaptation of Linux v6.12 drivers/ata/pata_amd.c: NVIDIA UDMA only.
 * Original notice in docs/licenses/ata-chipset-NOTICES.txt.
 */
#include <ciuki/ata_chipset.h>
void ata_nv_detect(struct ata_chipset *s)
{
    s->family = ATA_CHIP_NV;
    switch (s->pci->device) {
    case 0x01bc: s->maximum = 5; break;
    case 0x0065: case 0x0085: case 0x00d5: case 0x00e5: case 0x0053: case 0x0035:
        s->maximum = 6; break;
    default: return;
    }
    if (!(ata_chip_read(s, 0x50, 1) & (2u >> s->channel))) {
        s->maximum = -1; s->reason = "channel_disabled"; return;
    }
    for (unsigned u = 0; u < 2; u++) {
        s->saved_udma[u] = (uint8_t)ata_chip_read(s, 0x63 - 2 * s->channel - u, 1);
        /* pata_amd's BIOS mode filter is the host-side cable evidence.
         * Without ACPI GTM or a high BIOS mode, cap this drive at UDMA2. */
        if ((s->saved_udma[u] & 0xc0) == 0xc0 && (s->saved_udma[u] & 7) >= 4) s->cable80 = true;
    }
    s->reason = "ready";
}
bool ata_nv_plan(struct ata_chipset *s, unsigned u, unsigned m, struct ata_timing_plan *p)
{
    /* 33.333 MHz PCI; 15 ns UDMA clock and the donor's mode5/6 overrides. */
    static const unsigned encoding[] = {0xc2,0xc1,0xc0,0xc4,0xc5,0xc6,0xc7};
    return ata_plan_add(p, 0x63 - 2 * s->channel - u, 0xff, encoding[m]);
}
