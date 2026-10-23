// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *    ata_piix.c - Intel PATA/SATA controllers
 *
 *    Maintained by:  Tejun Heo <tj@kernel.org>
 *    		    Please ALWAYS copy linux-ide@vger.kernel.org
 *		    on emails.
 *
 *	Copyright 2003-2005 Red Hat Inc
 *	Copyright 2003-2005 Jeff Garzik
 *
 *	Copyright header from piix.c:
 *
 *  Copyright (C) 1998-1999 Andrzej Krzysztofowicz, Author and Maintainer
 *  Copyright (C) 1998-2000 Andre Hedrick <andre@linux-ide.org>
 *  Copyright (C) 2003 Red Hat Inc
 *
 *  libata documentation is available via 'make {ps|pdf}docs',
 *  as Documentation/driver-api/libata.rst
 *
 *  Hardware documentation available at http://developer.intel.com/
 *
 * Documentation
 *	Publicly available from Intel web site. Errata documentation
 * is also publicly available. As an aide to anyone hacking on this
 * driver the list of errata that are relevant is below, going back to
 * PIIX4. Older device documentation is now a bit tricky to find.
 *
 * The chipsets all follow very much the same design. The original Triton
 * series chipsets do _not_ support independent device timings, but this
 * is fixed in Triton II. With the odd mobile exception the chips then
 * change little except in gaining more modes until SATA arrives. This
 * driver supports only the chips with independent timing (that is those
 * with SITRE and the 0x44 timing register). See pata_oldpiix and pata_mpiix
 * for the early chip drivers.
 *
 * Errata of note:
 *
 * Unfixable
 *	PIIX4    errata #9	- Only on ultra obscure hw
 *	ICH3	 errata #13     - Not observed to affect real hw
 *				  by Intel
 *
 * Things we must deal with
 *	PIIX4	errata #10	- BM IDE hang with non UDMA
 *				  (must stop/start dma to recover)
 *	440MX   errata #15	- As PIIX4 errata #10
 *	PIIX4	errata #15	- Must not read control registers
 * 				  during a PIO transfer
 *	440MX   errata #13	- As PIIX4 errata #15
 *	ICH2	errata #21	- DMA mode 0 doesn't work right
 *	ICH0/1  errata #55	- As ICH2 errata #21
 *	ICH2	spec c #9	- Extra operations needed to handle
 *				  drive hotswap [NOT YET SUPPORTED]
 *	ICH2    spec c #20	- IDE PRD must not cross a 64K boundary
 *				  and must be dword aligned
 *	ICH2    spec c #24	- UDMA mode 4,5 t85/86 should be 6ns not 3.3
 *	ICH7	errata #16	- MWDMA1 timings are incorrect
 *
 * Should have been BIOS fixed:
 *	450NX:	errata #19	- DMA hangs on old 450NX
 *	450NX:  errata #20	- DMA hangs on old 450NX
 *	450NX:  errata #25	- Corruption with DMA on old 450NX
 *	ICH3    errata #15      - IDE deadlock under high load
 *				  (BIOS must set dev 31 fn 0 bit 23)
 *	ICH3	errata #18	- Don't use native mode
 */
/* Ciuki adaptation of Linux v6.12 drivers/ata/ata_piix.c: UDMA only.
 * Original notice in docs/licenses/ata-chipset-NOTICES.txt.
 */
#include <ciuki/ata_chipset.h>
void ata_piix_detect(struct ata_chipset *s)
{
    s->family = ATA_CHIP_PIIX;
    unsigned id = s->pci->device;
    switch (id) {
    case 0x7010: s->reason = "piix3_no_udma"; return;
    case 0x7111: case 0x7199: case 0x7601: case 0x84ca: s->maximum = 2; break;
    case 0x2421: s->maximum = 2; s->layout = 1; break;
    case 0x2411: s->maximum = 4; s->layout = 1; break;
    case 0x244a: case 0x244b: case 0x248a: case 0x248b: case 0x24c1:
    case 0x24ca: case 0x24cb: case 0x24db: case 0x245b: case 0x25a2:
    case 0x266f: case 0x27df: case 0x269e: case 0x2850:
        s->maximum = 5; s->layout = 1; break;
    default: return;
    }
    for (unsigned i = 0; i < pci_count(); i++) {
        const struct pci_func *p = pci_get(i);
        if (p->vendor != 0x8086 || p->device != 0x84cb) continue;
        unsigned cfg = s->ops->read(s->ctx, p, 0x41) | (unsigned)s->ops->read(s->ctx, p, 0x42) << 8;
        if (!p->revision || (p->revision < 5 && (cfg & 0x4000))) {
            s->maximum = -1; s->reason = "intel_450nx_erratum"; return;
        }
    }
    if (!(ata_chip_read(s, 0x41 + s->channel * 2, 1) & 0x80)) {
        s->maximum = -1; s->reason = "channel_disabled"; return;
    }
    s->cable80 = s->layout && (ata_chip_read(s, 0x54, 1) & (0x30u << (2 * s->channel)));
    s->reason = "ready";
}
bool ata_piix_plan(struct ata_chipset *s, unsigned u, unsigned m, struct ata_timing_plan *p)
{
    unsigned dn = s->channel * 2 + u, shift = (dn & 1) * 4;
    unsigned speed = m == 0 ? 0 : (m & 1) ? 1 : 2;
    /* Program timing/clock with the selected device's UDMA disabled. */
    if (!ata_plan_add(p, 0x48, 1u << dn, 0) ||
        !ata_plan_add(p, 0x4a + dn / 2, 3u << shift, speed << shift)) return false;
    if (s->layout) {
        if (!ata_plan_add(p, 0x54, 1u << dn, m > 2 && m != 5 ? 1u << dn : 0) ||
            !ata_plan_add(p, 0x55, 0x10u << dn, m == 5 ? 0x10u << dn : 0)) return false;
    }
    return ata_plan_add(p, 0x48, 1u << dn, 1u << dn);
}
