/* Promise PATA. Adapted from Linux v6.12 pata_pdc202xx_old.c and
 * pata_pdc2027x.c; Red Hat / Alan Cox / Bartlomiej Zolnierkiewicz,
 * Andre Hedrick / Promise / Frank Tiernan / Albert Lee (IBM).
 * Original notices: docs/licenses/ata-addin-NOTICES.txt.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/ata.h>

static int pdc_counter(struct ata_channel *c, uint32_t *value)
{
    for(unsigned retry=0;retry<3;retry++) {
        uint32_t lo=ata_addin_read(c,0x1120,4)&0x7fff, hi=ata_addin_read(c,0x1220,4)&0x7fff;
        uint32_t lo2=ata_addin_read(c,0x1120,4)&0x7fff, hi2=ata_addin_read(c,0x1220,4)&0x7fff;
        if(hi==hi2 && lo>=lo2) {*value=(hi<<15)|lo;return 0;}
    }
    return -FS_EIO;
}
int ata_promise_prepare(struct ata_channel *c)
{
    struct ata_chipset *s=&c->chipset;
    if(s->family==ATA_CHIP_PDC_OLD) {
        /* Preserve firmware PIO/command timing and require the slow clock. */
        if(s->pci->device!=0x4d33) {
            unsigned mask=s->channel ? 8 : 2;
            int e=ata_addin_write(c,0x11,1,ata_addin_read(c,0x11,1)&~mask);
            if(e) return e;
            e=ata_addin_write(c,0x1f,1,ata_addin_read(c,0x1f,1)|1);
            if(e) return e;
        }
        s->clock_mhz=33;return 0;
    }
    unsigned ch=s->channel, off=ch*0x100;
    if(!(ata_addin_read(c,0x1104+off,1)&2)) {s->reason="promise_port_disabled";return -FS_EOPNOTSUPP;}
    s->cable80=!(ata_addin_read(c,0x1108+off,4)&(1u<<26));
    if(!s->prepared) {
        uint32_t saved=ata_addin_read(c,0x1100,4), first=0,last=0;
        if(saved&(1u<<14)) return -FS_EBUSY;
        int e=ata_addin_write(c,0x1100,4,saved|(1u<<14));
        if(!e) e=pdc_counter(c,&first);
        uint64_t start=c->ops->now_ms(c->port_ctx);
        if(!e) e=ata_addin_wait(c,100);
        if(!e) e=pdc_counter(c,&last);
        uint64_t elapsed=c->ops->now_ms(c->port_ctx)-start;
        int restore=ata_addin_write(c,0x1100,4,saved);
        if(e || restore || elapsed<80 || elapsed>500) return -FS_EIO;
        unsigned khz=((first-last)&0x3fffffffu)/(unsigned)elapsed;
        if(khz<5000 || khz>70000) {s->reason="promise_pll_input";return -FS_EIO;}
        unsigned target=s->pci->device==0x4d68 ? 100000000u : 133333333u;
        unsigned ratio=target/khz, r=ratio<8600 ? 13 : ratio<12900 ? 8 : ratio<16100 ? 6 : 0;
        if(ratio>=64000 || ratio*(r+2)/1000<2) return -FS_EIO;
        unsigned f=ratio*(r+2)/1000-2;
        if(f>127 || ata_addin_write(c,0x1202,2,(r<<8)|f) ||
            ata_addin_wait(c,30)) return -FS_EIO;
        s->clock_mhz=target==100000000u ? 100 : 133;
        /* Changing the clock invalidates firmware PIO timing. Rebase both
         * devices on BOTH channels to PIO0 before the first reset/command. */
        for(unsigned channel=0;channel<2;channel++) for(unsigned u=0;u<2;u++) {
            unsigned reg=0x110c+channel*0x100+u*8;
            uint32_t a=ata_addin_read(c,reg,4), b=ata_addin_read(c,reg+4,4);
            if(ata_addin_write(c,reg,4,(a&0xffff0000u)|0x2bfb) ||
                ata_addin_write(c,reg+4,4,(b&0x00ffffffu)|0xac000000u)) return -FS_EIO;
        }
    }
    return 0;
}
int ata_addin_post_mode(struct ata_device *d)
{
    struct ata_channel *c=d->channel;
    if(c->chipset.family!=ATA_CHIP_PDC_NEW || c->chipset.clock_mhz!=133) return 0;
    if(d->udma_mode>6 || !ata_addin_idle(c) || c->modes_published) return -FS_EBUSY;
    static const uint32_t timing[]={0xd50f4a,0xd00a3a,0xcd072a,0xcd051a,0xcd031a,0xcb021a,0xcb011a};
    unsigned reg=0x110c+0x100*c->chipset.channel+8*d->unit;
    /* SET FEATURES snooping rewrites PIO and UDMA. Repair before IDENTIFY. */
    uint32_t a=ata_addin_read(c,reg,4);
    if(ata_addin_write(c,reg,4,(a&0xffff0000u)|0x2bfb)) return -FS_EIO;
    return ata_addin_write(c,reg+4,4,0xac000000u|timing[d->udma_mode]);
}
