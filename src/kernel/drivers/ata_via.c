// SPDX-License-Identifier: GPL-2.0-only
/*
 * pata_via.c 	- VIA PATA for new ATA layer
 *			  (C) 2005-2006 Red Hat Inc
 *
 *  Documentation
 *	Most chipset documentation available under NDA only
 *
 *  VIA version guide
 *	VIA VT82C561	-	early design, uses ata_generic currently
 *	VIA VT82C576	-	MWDMA, 33Mhz
 *	VIA VT82C586	-	MWDMA, 33Mhz
 *	VIA VT82C586a	-	Added UDMA to 33Mhz
 *	VIA VT82C586b	-	UDMA33
 *	VIA VT82C596a	-	Nonfunctional UDMA66
 *	VIA VT82C596b	-	Working UDMA66
 *	VIA VT82C686	-	Nonfunctional UDMA66
 *	VIA VT82C686a	-	Working UDMA66
 *	VIA VT82C686b	-	Updated to UDMA100
 *	VIA VT8231	-	UDMA100
 *	VIA VT8233	-	UDMA100
 *	VIA VT8233a	-	UDMA133
 *	VIA VT8233c	-	UDMA100
 *	VIA VT8235	-	UDMA133
 *	VIA VT8237	-	UDMA133
 *	VIA VT8237A	-	UDMA133
 *	VIA VT8237S	-	UDMA133
 *	VIA VT8251	-	UDMA133
 *
 *	Most registers remain compatible across chips. Others start reserved
 *	and acquire sensible semantics if set to 1 (eg cable detect). A few
 *	exceptions exist, notably around the FIFO settings.
 *
 *	One additional quirk of the VIA design is that like ALi they use few
 *	PCI IDs for a lot of chips.
 *
 *	Based heavily on:
 *
 * Version 3.38
 *
 * VIA IDE driver for Linux. Supported southbridges:
 *
 *   vt82c576, vt82c586, vt82c586a, vt82c586b, vt82c596a, vt82c596b,
 *   vt82c686, vt82c686a, vt82c686b, vt8231, vt8233, vt8233c, vt8233a,
 *   vt8235, vt8237
 *
 * Copyright (c) 2000-2002 Vojtech Pavlik
 *
 * Based on the work of:
 *	Michel Aubry
 *	Jeff Garzik
 *	Andre Hedrick

 */
/* Ciuki adaptation of Linux v6.12 drivers/ata/pata_via.c: UDMA only.
 * Original notice in docs/licenses/ata-chipset-NOTICES.txt.
 */
#include <ciuki/ata_chipset.h>
void ata_via_detect(struct ata_chipset *s)
{
    s->family = ATA_CHIP_VIA;
    if (s->pci->device != 0x0571 && s->pci->device != 0x1571) return;
    static const struct { uint16_t id; uint8_t lo, hi, mode; } table[] = {
        {0x8231,0,0x2f,5}, {0x3074,0,0x2f,5}, {0x3109,0,0x2f,5},
        {0x3147,0,0x2f,6}, {0x3177,0,0x2f,6}, {0x3227,0,0x2f,6},
        {0x3337,0,0x2f,6}, {0x3372,0,0x2f,6}, {0x3287,0,0x2f,6},
        {0x0686,0x40,0x4f,5}, {0x0686,0x10,0x2f,4}, {0x0686,0,0x0f,2},
        {0x0596,0x10,0x2f,4}, {0x0596,0,0x0f,2}, {0x0586,0x20,0x4f,2}
    };
    for (unsigned i = 0; i < sizeof(table)/sizeof(table[0]); i++) {
        const struct pci_func *b = ata_chip_bridge(s, 0x1106, table[i].id);
        if (!b || b->revision < table[i].lo || b->revision > table[i].hi) continue;
        if (s->bridge && s->bridge != b) { s->maximum = -1; s->reason = "ambiguous_bridge"; return; }
        s->bridge = b; s->maximum = table[i].mode;
    }
    if (!s->bridge) { s->reason = "via_bridge_unknown"; return; }
    unsigned enables = ata_chip_read(s, 0x40, 1) & 3;
    if (!(enables & (2u >> s->channel))) { s->maximum = -1; s->reason = "channel_disabled"; return; }
    unsigned timing = ata_chip_read(s, 0x50, 4);
    if ((s->maximum == 4 && (timing & 0x80008) != 0x80008) ||
        (s->maximum == 2 && s->bridge->device != 0x0586 && (timing & 0x80008))) {
        s->maximum = -1; s->reason = "via_clock_erratum"; return;
    }
    if (s->bridge->device == 0x0586) {
        static const unsigned fifo[] = {0,0x60,0,0x20};
        unsigned v = ata_chip_read(s, 0x43, 1);
        bool bad_preq = s->bridge->revision >= 0x40 && s->bridge->revision <= 0x46;
        if ((v & 0x60) != fifo[enables] || (bad_preq && (v & 0x80))) {
            s->maximum = -1; s->reason = "via_fifo_erratum"; return;
        }
    }
    /* UDMA66 has only drive-side detection: keep the host limit at 33. */
    s->cable80 = s->maximum >= 5 && (timing & (0x10100000u >> (16 * s->channel)));
    s->layout = (unsigned)s->maximum;
    s->reason = "ready";
}
bool ata_via_plan(struct ata_chipset *s, unsigned u, unsigned m, struct ata_timing_plan *p)
{
    /* UDMA periods from Linux v6.12 libata-pata-timings.c / ATA-6.
     * Leave shared PIO, command and BAD_AST setup timings untouched. */
    static const unsigned ns[] = {120,80,60,45,30,20,15};
    unsigned divisor = s->layout == 4 ? 2 : s->layout == 5 ? 3 : s->layout == 6 ? 4 : 1;
    unsigned ticks = (ns[m] * 1000 + 30000/divisor - 1) / (30000/divisor);
    if (ticks < 2) ticks = 2;
    unsigned max = s->layout == 2 ? 5 : 9;
    if (ticks > max) ticks = max; /* donor's saturating hardware encoding */
    unsigned v = (s->layout == 4 ? 0xe8 : 0xe0) | (ticks - 2);
    return ata_plan_add(p, 0x53 - 2 * s->channel - u, 0xef, v);
}
