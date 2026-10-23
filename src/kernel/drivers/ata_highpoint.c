/* HighPoint PATA. Linux v6.12 pata_hpt366.c, pata_hpt37x.c,
 * pata_hpt3x2n.c: Andre Hedrick, Sun, Red Hat, MontaVista, Alan Cox.
 * Original notices: docs/licenses/ata-addin-NOTICES.txt.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/ata.h>
#include "ata_highpoint_tables.h"

static bool hpt_model(const uint8_t *id, const char *const *list)
{
    char name[41]; unsigned len=40;
    for(unsigned i=0;i<40;i++) name[i]=(char)id[54+(i^1)];
    while(len && (name[len-1]==' ' || !name[len-1])) len--;
    name[len]=0;
    for(unsigned i=0;list[i];i++) if(!strncmp(name,list[i],sizeof(name))) return true;
    return false;
}
int ata_addin_limit(struct ata_chipset *s, unsigned unit, const uint8_t *id, int mode)
{
    if(!s->prepared) {s->reason="addin_not_prepared";return -1;}
    if(s->family==ATA_CHIP_PDC_NEW && unit && mode>5) mode=5;
    if(s->family>=ATA_CHIP_HPT36) {
        /* SATA bridges are a different qualification target. */
        unsigned w76=fs_rd16(id+152);
        if(w76 && w76!=0xffff) {s->reason="highpoint_sata_bridge";return -1;}
        if((s->family==ATA_CHIP_HPT36 || (s->pci->device==4 && s->pci->revision==3)) && hpt_model(id,hpt_bad33)) {
            s->reason="highpoint_drive_erratum";return -1;
        }
        if(s->family==ATA_CHIP_HPT36) {
            if(hpt_model(id,hpt_bad66) && mode>3) mode=3;
            if(hpt_model(id,hpt_bad66_3) && mode>2) mode=2;
        } else if(s->pci->device==4 && s->pci->revision<=4 && hpt_model(id,hpt_bad66) && mode>4) mode=4;
    }
    return mode;
}
bool ata_addin_plan(struct ata_chipset *s, unsigned unit, unsigned mode, struct ata_timing_plan *p)
{
    if(!s->prepared || mode>6) return false;
    if(s->family==ATA_CHIP_PDC_NEW) return true; /* SET FEATURES snoop, then MMIO repair */
    if(s->family==ATA_CHIP_PDC_OLD) {
        static const uint8_t b[]={0x60,0x40,0x20,0x40,0x20,0x20}, c[]={3,2,1,2,1,1};
        unsigned reg=0x60+8*s->channel+4*unit;
        return mode<6 && ata_plan_add(p,reg+1,0xe0,b[mode]) && ata_plan_add(p,reg+2,15,c[mode]);
    }
    uint32_t mask=s->family==ATA_CHIP_HPT36 ? 0xf0070000u : 0x303c0000u;
    uint32_t timing=s->family==ATA_CHIP_HPT36 ? hpt36_udma[s->layout][mode]&~0xc0000000u :
        s->family==ATA_CHIP_HPTN ? hptn_udma[0][mode] : hpt37_udma[s->layout][mode];
    unsigned reg=0x40+4*(unit+(s->family==ATA_CHIP_HPT36 ? 0 : 2*s->channel));
    for(unsigned i=0;i<4;i++) if((mask>>(8*i))&255)
        if(!ata_plan_add(p,reg+i,(mask>>(8*i))&255,(timing&mask)>>(8*i)&255)) return false;
    return true;
}
static int hpt_calibrate(struct ata_channel *c, unsigned low, unsigned high, bool n)
{
    struct ata_chipset *s=&c->chipset;
    for(unsigned adjust=0;adjust<8;adjust++) {
        if(ata_addin_cfg(s,0x5c,4,(high<<16)|low|0x100) || ata_addin_cfg(s,0x5b,1,0x21)) return -FS_EIO;
        bool locked=false;
        for(unsigned i=0;i<0x5000;i++) {
            if(c->ops->delay_us(c->port_ctx,50)) return -FS_EIO;
            if(ata_chip_read(s,0x5b,1)&0x80) {locked=true;break;}
        }
        for(unsigned i=0;locked && i<0x1000;i++) if(!(ata_chip_read(s,0x5b,1)&0x80)) locked=false;
        if(locked) return ata_addin_cfg(s,0x5c,4,ata_chip_read(s,0x5c,4)&~0x100u);
        if(!n) {if(adjust&1) low-=adjust>>1;else high+=adjust>>1;}
    }
    s->reason="highpoint_pll_unstable";return -FS_EIO;
}
int ata_highpoint_prepare(struct ata_channel *c)
{
    struct ata_chipset *s=&c->chipset;
    unsigned ch=s->channel;
    /* hpt366_set_mode disables PIO bus mastering/FIFO to contain errors.
     * Do this before IDENTIFY as well, including the unknown-clock fallback;
     * a firmware PRD must never receive our PIO identify/metadata data. */
    for(unsigned dn=0;dn<(s->family==ATA_CHIP_HPT36 ? 2u : 4u);dn++) {
        unsigned reg=0x40+4*dn;uint32_t old=ata_chip_read(s,reg,4);
        if((old&0xc0000000u) && ata_addin_cfg(s,reg,4,old&~0xc0000000u)) return -FS_EIO;
    }
    if(s->family==ATA_CHIP_HPT36) {
        if((ata_chip_read(s,0x50,1)&0x30)!=0x30) {s->reason="highpoint_port_disabled";return -FS_EOPNOTSUPP;}
        unsigned clk=(ata_chip_read(s,0x40,4)>>8)&15;
        if(clk!=5 && clk!=7 && clk!=9) {s->maximum=-1;s->reason="highpoint_unknown_clock";return 0;}
        s->layout=clk==5 ? 0 : clk==9 ? 2 : 1;
        s->clock_mhz=s->layout==0 ? 25 : s->layout==1 ? 33 : 40;
        s->cable80=!(ata_chip_read(s,0x5a,1)&2);
        if(ata_addin_cfg(s,0x51,1,ata_chip_read(s,0x51,1)&~0x80u)) return -FS_EIO;
    } else {
        if(!(ata_chip_read(s,0x50+4*ch,1)&4)) {s->reason="highpoint_port_disabled";return -FS_EOPNOTSUPP;}
        if(!s->prepared) {
            if(ata_addin_cfg(s,0x5a,1,ata_chip_read(s,0x5a,1)&~0x10u)) return -FS_EIO;
            unsigned base=s->family==ATA_CHIP_HPTN ? 77 : s->pci->device==8 ? 48 :
                s->pci->device==5 ? 66 : s->pci->revision==5 ? 55 : 48;
            uint32_t fcnt=0;
            bool fn1=s->pci->device==8 && (s->pci->fn&1);
            if(fn1 && !s->pci_mhz) {s->reason="highpoint_fn0_clock_missing";return -FS_EOPNOTSUPP;}
            if(!fn1) fcnt=ata_addin_read(c,0x90,4);
            if(!fn1 && (fcnt>>12)!=0xabcde) {
                unsigned total=0;
                for(unsigned i=0;i<128;i++) {
                    total+=ata_chip_read(s,0x78,2)&0x1ff;
                    if(c->ops->delay_us(c->port_ctx,15)) return -FS_EIO;
                }
                fcnt=total/128;
            }
            unsigned freq=fn1 ? s->pci_mhz : (fcnt&0x1ff)*base/192;
            if(freq<20 || freq>75) {s->maximum=-1;s->reason="highpoint_unknown_clock";return 0;}
            unsigned mhz=freq<40 ? 33 : freq<45 ? 40 : freq<55 ? 50 : 66;
            s->pci_mhz=mhz;
            bool n=s->family==ATA_CHIP_HPTN;
            bool pll=n || s->maximum==6 || mhz==40 || s->pci->device==8 ||
                (s->pci->device==4 && s->pci->revision==3 && mhz!=33) ||
                (s->pci->device==4 && s->pci->revision==4 && mhz==66);
            if(pll) {
                unsigned target=s->maximum==6 ? 66 : 50, low=mhz*48/target;
                int e=hpt_calibrate(c,low,low+2+(!n && mhz>40 ? 2 : 0),n);
                if(e) return e;
                mhz=target;
            } else if(ata_addin_cfg(s,0x5b,1,0x23)) return -FS_EIO;
            s->clock_mhz=mhz;s->layout=mhz==33 ? 0 : mhz==50 ? 1 : 2;
            if(mhz==33 && s->pci->device==4 && s->pci->revision<5) s->maximum=4;
            if(s->pci->device==5 && s->pci->revision<2 && ata_addin_write(c,0x9c,1,0x0e)) return -FS_EIO;
            /* A clock change needs safe command/data timing on both units
             * of both channels before SRST or IDENTIFY. */
            for(unsigned dn=0;dn<4;dn++) {
                unsigned reg=0x40+4*dn;
                uint32_t timing=n ? hptn_pio0[0] : hpt37_pio0[s->layout];
                uint32_t old=ata_chip_read(s,reg,4), mask=0xcfc3ffffu;
                if(ata_addin_cfg(s,reg,4,(old&~mask)|(timing&mask))) return -FS_EIO;
            }
        }
        unsigned saved=ata_chip_read(s,0x5b,1);
        unsigned gate=s->pci->device==8 && (s->pci->fn&1) ? 0x53+4*ch : 0x5b;
        unsigned before=ata_chip_read(s,gate,1);
        if(ata_addin_cfg(s,gate,1,gate==0x5b ? saved&~1u : before|0x80)) return -FS_EIO;
        int e=c->ops->delay_us(c->port_ctx,10);
        s->cable80=!(ata_chip_read(s,0x5a,1)&(2u>>ch));
        int restore=ata_addin_cfg(s,gate,1,before& (gate==0x5b ? 0x7fu : 0xffu));
        if(e || restore) return -FS_EIO;
        if(ata_addin_cfg(s,0x50+4*ch,1,0x37) || c->ops->delay_us(c->port_ctx,100)) return -FS_EIO;
        unsigned reg=0x51+4*ch, m=ata_chip_read(s,reg,1);
        m=s->pci->device==4 && s->pci->revision<5 ? (m&~2u)|1 : m&~7u;
        if(ata_addin_cfg(s,reg,1,m)) return -FS_EIO;
    }
    return 0;
}
