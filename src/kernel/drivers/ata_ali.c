/*
 * pata_ali.c 	- ALI 15x3 PATA for new ATA layer
 *			  (C) 2005 Red Hat Inc
 *
 * based in part upon
 * linux/drivers/ide/pci/alim15x3.c		Version 0.17	2003/01/02
 *
 *  Copyright (C) 1998-2000 Michel Aubry, Maintainer
 *  Copyright (C) 1998-2000 Andrzej Krzysztofowicz, Maintainer
 *  Copyright (C) 1999-2000 CJ, cjtsai@ali.com.tw, Maintainer
 *
 *  Copyright (C) 1998-2000 Andre Hedrick (andre@linux-ide.org)
 *  May be copied or modified under the terms of the GNU General Public License
 *  Copyright (C) 2002 Alan Cox <alan@redhat.com>
 *  ALi (now ULi M5228) support by Clear Zhang <Clear.Zhang@ali.com.tw>
 *
 *  Documentation
 *	Chipset documentation available under NDA only
 *
 *  TODO/CHECK
 *	Cannot have ATAPI on both master & slave for rev < c2 (???) but
 *	otherwise should do atapi DMA (For now for old we do PIO only for
 *	ATAPI)
 *	Review Sunblade workaround.
 */
/* Ciuki adaptation of Linux v6.12 drivers/ata/pata_ali.c: UDMA only.
 * Original legacy GPL grant in docs/licenses/ata-chipset-NOTICES.txt.
 */
#include <ciuki/ata_chipset.h>
void ata_ali_detect(struct ata_chipset *s)
{
    s->family = ATA_CHIP_ALI;
    if (s->pci->device != 0x5229) return;
    unsigned rev = s->pci->revision;
    if (rev < 0xc2) { s->reason = "ali_pre_c2_errata"; return; }
    s->no_lba48_dma = rev < 0xc5;
    s->bridge = ata_chip_bridge(s, 0x10b9, 0x1533);
    bool ali_north = false;
    for (unsigned i = 0; i < pci_count(); i++) {
        const struct pci_func *p = pci_get(i);
        if (!p->bus && !p->dev && !p->fn && p->vendor == 0x10b9) ali_north = true;
    }
    if (rev < 0xc5 && ali_north) {
        if (!s->bridge || !(s->ops->read(s->ctx, s->bridge, 0x79) & (rev == 0xc2 ? 4 : 2))) {
            s->reason = "ali_bridge_erratum"; return;
        }
    }
    if (!(ata_chip_read(s, 0x4a, 1) & 0x20) || (ata_chip_read(s, 0x4b, 1) & 9) != 9 ||
        (ata_chip_read(s, 0x53, 1) & (rev >= 0xc7 ? 3u : 1u)) != (rev >= 0xc7 ? 3u : 1u)) {
        s->reason = "ali_firmware_fixup_required"; return;
    }
    s->maximum = rev <= 0xc3 ? 4 : rev == 0xc4 ? 5 : 6;
    s->cable80 = !(ata_chip_read(s, 0x4a, 1) & (1u << s->channel));
    s->reason = "ready";
}
bool ata_ali_plan(struct ata_chipset *s, unsigned u, unsigned m, struct ata_timing_plan *p)
{
    static const unsigned timing[] = {0xc,0xb,0xa,0x9,0x8,0xf,0xd};
    unsigned shift = 4 * u;
    return ata_plan_add(p, 0x54 + s->channel, 0xfu << shift, 8u << shift) &&
        ata_plan_add(p, 0x56 + s->channel, 0xfu << shift, timing[m] << shift);
}
