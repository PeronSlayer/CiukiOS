// SPDX-License-Identifier: GPL-2.0-only
/*
 *    pata_sis.c - SiS ATA driver
 *
 *	(C) 2005 Red Hat
 *	(C) 2007,2009 Bartlomiej Zolnierkiewicz
 *
 *    Based upon linux/drivers/ide/pci/sis5513.c
 * Copyright (C) 1999-2000	Andre Hedrick <andre@linux-ide.org>
 * Copyright (C) 2002		Lionel Bouton <Lionel.Bouton@inet6.fr>, Maintainer
 * Copyright (C) 2003		Vojtech Pavlik <vojtech@suse.cz>
 * SiS Taiwan		: for direct support and hardware.
 * Daniela Engert	: for initial ATA100 advices and numerous others.
 * John Fremlin, Manfred Spraul, Dave Morgan, Peter Kjellerstedt	:
 *			  for checking code correctness, providing patches.
 * Original tests and design on the SiS620 chipset.
 * ATA100 tests and design on the SiS735 chipset.
 * ATA16/33 support from specs
 * ATA133 support for SiS961/962 by L.C. Chang <lcchang@sis.com.tw>
 *
 *
 *	TODO
 *	Check MWDMA on drives that don't support MWDMA speed pio cycles ?
 *	More Testing
 */
/* Ciuki adaptation of Linux v6.12 drivers/ata/pata_sis.c: UDMA only.
 * Original notice in docs/licenses/ata-chipset-NOTICES.txt.
 */
#include <ciuki/ata_chipset.h>
void ata_sis_detect(struct ata_chipset *s)
{
    s->family = ATA_CHIP_SIS;
    if (s->pci->device != 0x5513) return;
    static const struct { uint16_t id; uint8_t layout, mode; } table[] = {
        {0x0968,3,6},{0x0966,3,6},{0x0965,3,6},
        {0x0745,2,5},{0x0735,2,5},{0x0733,2,5},{0x0635,2,5},{0x0633,2,5},
        {0x0730,1,5},{0x0550,1,5},{0x0640,1,4},{0x0630,1,4},
        {0x0620,1,4},{0x0540,1,4},{0x0530,1,4},
        {0x5600,0,2},{0x5598,0,2},{0x5597,0,2},{0x5591,0,2},{0x5582,0,2},{0x5581,0,2}
    };
    for (unsigned i = 0; i < sizeof(table)/sizeof(table[0]); i++) {
        const struct pci_func *b = ata_chip_bridge(s, 0x1039, table[i].id);
        if (!b) continue;
        if (s->bridge) { s->maximum = -1; s->reason = "ambiguous_bridge"; return; }
        s->bridge = b; s->layout = table[i].layout; s->maximum = table[i].mode;
        if (b->device == 0x0630 && b->revision >= 0x30) s->maximum = 5;
    }
    if (!s->bridge) { s->reason = "sis_concealed_bridge"; return; }
    bool ready = (ata_chip_read(s, 0x4a, 1) & (2u << s->channel)) != 0;
    if (!s->layout) ready = ready && !(s->pci->prog_if & 0xf) && (ata_chip_read(s, 0x52, 1) & 8);
    if (s->layout == 1) ready = ready && (ata_chip_read(s, 0x52, 1) & 4);
    if (s->layout == 2) ready = ready && (ata_chip_read(s, 0x49, 1) & 1);
    if (s->layout == 3) ready = ready && !(ata_chip_read(s, 0x50 + 2 * s->channel, 1) & 8);
    if (!ready) { s->maximum = -1; s->reason = "sis_firmware_fixup_required"; return; }
    s->cable80 = s->layout == 3 ? !(ata_chip_read(s, 0x51 + 2 * s->channel, 1) & 0x80) :
        s->layout && !(ata_chip_read(s, 0x48, 1) & (0x10u << s->channel));
    s->reason = "ready";
}
bool ata_sis_plan(struct ata_chipset *s, unsigned u, unsigned m, struct ata_timing_plan *p)
{
    unsigned reg = 0x41 + 4 * s->channel + 2 * u;
    if (s->layout == 0) {
        static const unsigned bits[] = {0xe0,0xc0,0xa0};
        return ata_plan_add(p, reg, 0xe0, bits[m]);
    }
    if (s->layout == 1) {
        static const unsigned bits[] = {0xf0,0xd0,0xb0,0xa0,0x90,0x80};
        return ata_plan_add(p, reg, 0xf0, bits[m]);
    }
    if (s->layout == 2) {
        static const unsigned bits[] = {0x8b,0x87,0x85,0x83,0x82,0x81};
        return ata_plan_add(p, reg, 0x8f, bits[m]);
    }
    unsigned base = (ata_chip_read(s, 0x54, 4) & 0x40000000) ? 0x70 : 0x40;
    reg = base + 8 * s->channel + 4 * u;
    unsigned current = ata_chip_read(s, reg, 4);
    static const unsigned t100[] = {0x6b0,0x470,0x350,0x140,0x120,0x110};
    static const unsigned t133[] = {0x9f0,0x6a0,0x470,0x250,0x230,0x220,0x210};
    if (!(current & 8) && m > 5) return false;
    unsigned t = current & 8 ? t133[m] : t100[m];
    return ata_plan_add(p, reg, 0xf4, (t & 0xf0) | 4) && ata_plan_add(p, reg + 1, 0x0f, t >> 8);
}
