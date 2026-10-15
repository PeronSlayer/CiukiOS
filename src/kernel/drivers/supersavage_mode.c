/* Independent implementation of the mobile save/restore sequence documented
 * in docs/design/supersavage-fullscreen.md. No firmware or engine commands.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include "supersavage_mode.h"

struct reg8 { uint8_t index, mask; };
/* X.Org SavageSave, plus the bank/stride overflow and active-output state
 * changed by VGA takeover. Masks with reserved bits follow RE046-B; mobile
 * panel values have no equivalent Savage4 layout and compare saved bytes. */
static const struct reg8 cr_regs[] = {
    {0x31,0x4F},{0x32,0x50},{0x33,0xFF},{0x34,0xFF},{0x36,0xFF},
    {0x3A,0xBB},{0x3B,0xFF},{0x3C,0xFF},{0x40,0xFF},{0x42,0x20},
    {0x43,0xFF},{0x45,0xFF},{0x50,0xF1},{0x51,0x33},{0x53,0xFF},
    {0x58,0x13},{0x5B,0xFF},{0x5D,0xFF},{0x5E,0x57},{0x5F,0xFF},
    {0x60,0xFF},{0x65,0xD9},{0x66,0xBB},{0x67,0xFF},{0x68,0xFF},
    {0x69,0xFF},{0x6A,0x7F},{0x6B,0xFF},{0x6F,0xFF},{0x86,0xFF},
    {0x88,0xFF},{0x90,0xFF},{0x91,0xFF},{0xB0,0xFF}
};
static const struct reg8 sr_regs[] = {
    {0x0E,0xFF},{0x0F,0xFF},{0x10,0xFF},{0x11,0xFF},
    {0x12,0xFF},{0x13,0xFF},{0x15,0xDF},{0x18,0xFF},{0x1B,0x98},
    {0x29,0x1C},{0x30,0xFF},{0x39,0xFF},
    {0x54,0xFF},{0x55,0xFF},{0x56,0xFF},{0x57,0xFF},
    {0x58,0xFF},{0x59,0xFF},{0x5A,0xFF},{0x5B,0xFF},
    {0x61,0xFF},{0x66,0xFF},{0x69,0xFF},{0x6E,0xFF}
};
/* Every dword in 8180..821c is named by savage_streams.h for the NEW
 * mobile streams engine. Do not read the old engine's unrelated registers. */
static unsigned stream_reg(unsigned i) { return 0x8180u+4*i; }
static uint8_t in(const struct supersavage_mode_io *b, unsigned p)
{ return b->vga.in(b->vga.arg,(uint16_t)p); }
static void out(const struct supersavage_mode_io *b, unsigned p, unsigned v)
{ b->vga.out(b->vga.arg,(uint16_t)p,(uint8_t)v); }
static uint8_t rd(const struct supersavage_mode_io *b, unsigned p, unsigned r)
{ out(b,p,r); return in(b,p+1); }
static void wr(const struct supersavage_mode_io *b, unsigned p, unsigned r, unsigned v)
{ out(b,p,r); out(b,p+1,v); }
static void cr(const struct supersavage_mode_io *b, unsigned r, unsigned v) { wr(b,0x3D4,r,v); }
static void sr(const struct supersavage_mode_io *b, unsigned r, unsigned v) { wr(b,0x3C4,r,v); }
static int step(struct supersavage_mode *s, const struct supersavage_mode_io *b, const char *name)
{ s->stage=name; return b->checkpoint(b->vga.arg,name,0); }
static int compare(struct supersavage_mode *s, unsigned reg, uint32_t want, uint32_t got, uint32_t mask)
{
    if (!((want^got)&mask)) return 0;
    if (!s->mask) { s->bad_reg=reg; s->expected=want; s->actual=got; s->mask=mask; }
    return -EIO;
}
static int cw(struct supersavage_mode *s, const struct supersavage_mode_io *b,
              unsigned r, unsigned v, unsigned mask)
{ cr(b,r,v); return compare(s,r,v,rd(b,0x3D4,r),mask); }
static int sw(struct supersavage_mode *s, const struct supersavage_mode_io *b,
              unsigned r, unsigned v, unsigned mask)
{ sr(b,r,v); return compare(s,0x100u+r,v,rd(b,0x3C4,r),mask); }
static int mw(struct supersavage_mode *s, const struct supersavage_mode_io *b, unsigned r, uint32_t v)
{
    b->write32(b->vga.arg,r,v);
    /* Compare the saved register image, including stable firmware fields.
     * No guessed zero value for reserved bits and no engine/status reads. */
    return compare(s,r,v,b->read32(b->vga.arg,r),UINT32_MAX);
}
static void unlock(const struct supersavage_mode_io *b)
{ out(b,0x3C3,1); out(b,0x3C2,in(b,0x3CC)|1); cr(b,0x38,0x48); cr(b,0x39,0xA5); sr(b,8,6); sr(b,0x26,0x40); }
static void locks(struct supersavage_mode *s, const struct supersavage_mode_io *b)
{
    sr(b,0x26,s->iga); sr(b,8,s->lock08);
    cr(b,0x39,s->lock39); cr(b,0x38,s->lock38);
    out(b,0x3C2,s->misc); out(b,0x3C3,s->enable);
    out(b,0x3C4,s->sr_index); out(b,(s->misc&1) ? 0x3D4 : 0x3B4,s->cr_index);
}
/* Same two-edge wait as X.Org VerticalRetraceWait, with a time deadline
 * as well as a read cap. Frozen clocks and missing retrace are failures. */
static int retrace(const struct supersavage_mode_io *b)
{
    if (!(rd(b,0x3D4,0x17)&0x80)) return 0;
    uint64_t started=b->now_us(b->vga.arg);
    for (unsigned edge=0;edge<2;edge++) {
        bool seen=false;
        for (unsigned i=0;i<65536;i++) {
            bool active=!!(in(b,0x3DA)&8);
            if (active==(edge!=0)) { seen=true; break; }
            if (b->now_us(b->vga.arg)-started>=40000) return -ETIMEDOUT;
        }
        if (!seen) return -ETIMEDOUT;
    }
    return 0;
}
bool supersavage_mode_supported(uint16_t mode)
{ return mode==0x101 || (mode<0x80 && cvga_find_mode(mode)); }

int supersavage_mode_save(struct supersavage_mode *s, const struct supersavage_mode_io *b)
{
    if (s->saved) return -EBUSY;
    memset(s,0,sizeof(*s));
    int e=step(s,b,"native-save"); if (e) return e;
    s->misc=in(b,0x3CC); s->enable=in(b,0x3C3);
    unsigned cp=(s->misc&1) ? 0x3D4 : 0x3B4;
    s->cr_index=in(b,cp); s->sr_index=in(b,0x3C4);
    s->lock38=rd(b,cp,0x38); s->lock39=rd(b,cp,0x39);
    s->lock08=rd(b,0x3C4,8);
    /* Open the sequencer before reading its IGA selector. */
    sr(b,8,6); s->iga=rd(b,0x3C4,0x26);
    unlock(b);
    uint8_t cr40=rd(b,0x3D4,0x40);
    e=cw(s,b,0x40,cr40|1,1);
    if (!e) e=compare(s,0x126,0x40,rd(b,0x3C4,0x26),255);
    if (e) { cr(b,0x40,cr40); locks(s,b); return e; }
    for (unsigned i=0;i<ARRAY_SIZE(cr_regs);i++) s->cr[cr_regs[i].index]=rd(b,0x3D4,cr_regs[i].index);
    s->cr[0x40]=cr40;
    for (unsigned i=0;i<ARRAY_SIZE(sr_regs);i++) s->sr[sr_regs[i].index]=rd(b,0x3C4,sr_regs[i].index);
    unsigned panel_x=(s->sr[0x61]+((s->sr[0x66]&2)<<7)+1)*8;
    unsigned panel_y=s->sr[0x69]+((s->sr[0x6E]&0x70)<<4)+1;
    /* No native power-on, TV or two independent display pipelines recipe.
     * Restoring the same active panel is the supported firmware contract. */
    if (!(s->misc&1) || !(s->cr[0x6B]&2) || (s->cr[0x6B]&0xA4) ||
        panel_x<640 || panel_x>1600 || panel_y<480 || panel_y>1200 ||
        (s->cr[0x66]&2)) {
        cr(b,0x40,cr40); locks(s,b); return -EOPNOTSUPP;
    }
    bool all_dead=true;
    for (unsigned i=0;i<SUPERSAVAGE_STREAM_REGS;i++) {
        s->streams[i]=b->read32(b->vga.arg,stream_reg(i));
        if (s->streams[i]!=UINT32_MAX) all_dead=false;
    }
    if (all_dead) { cr(b,0x40,cr40); locks(s,b); return -EIO; }
    vga_hardware_read_registers(&s->vga,&b->vga,!!(s->sr[0x1B]&0x10));
    /* capture's index state belongs to the original caller, not our probes. */
    s->vga.seq_index=s->sr_index; s->vga.crtc_index=s->cr_index;
    s->saved=true; /* rollback is possible before the first scanout write */
    e=step(s,b,"native-save-planes"); if (e) return e;
    s->touched=true;
    sr(b,1,s->vga.seq[1]|0x20);
    if ((e=b->delay_us(b->vga.arg,100))) return e; /* >= three VGA HSYNCs */
    /* RE046-B CR31.3 overrides standard planar addressing; CR3A.3 can
     * redirect VGA memory to the top of VRAM. Clear both before capture. */
    if ((e=cw(s,b,0x31,0,0x4F)) ||
        (e=cw(s,b,0x3A,s->cr[0x3A]&~0x38u,0xBB)) ||
        (e=cw(s,b,0x58,s->cr[0x58]&~0x10u,0x13))) return e;
    vga_hardware_capture_planes(&s->vga,&b->vga); s->planes_saved=true;
    return 0;
}

/* Integer counterpart of SavageCalcClock(25175,1,1,127,0,4,180000,360000).
 * Compare rational errors without floating point in kernel context. */
static void clock_25175(uint8_t *sr12, uint8_t *sr13, uint8_t *sr29)
{
    uint64_t best=UINT64_MAX; unsigned denom_best=1, bm=0,bn=0,br=0;
    for (unsigned r=0;r<=4;r++) for (unsigned n=3;n<=129;n++) {
        unsigned denom=n<<r;
        unsigned m=(25175000ULL*denom+7159090)/14318180;
        if (m<3 || m>129 || 14318180ULL*m<180000000ULL*n || 14318180ULL*m>360000000ULL*n) continue;
        uint64_t actual=14318180ULL*m,want=25175000ULL*denom;
        uint64_t error=actual>want ? actual-want : want-actual;
        if (best==UINT64_MAX || error*denom_best<best*denom) {
            best=error; denom_best=denom; bm=m-2; bn=n-2; br=r;
        }
    }
    *sr12=(br<<6)|(bn&63); *sr13=bm;
    *sr29=(br&4)|((bm&0x100)>>5)|((bn&0x40)>>2);
}
static int pll(struct supersavage_mode *s, const struct supersavage_mode_io *b,
               uint8_t r12, uint8_t r13, uint8_t r29, uint8_t r15)
{
    int e=0;
    e|=sw(s,b,0x12,r12,255); e|=sw(s,b,0x13,r13,255); e|=sw(s,b,0x29,r29,0x1C);
    uint8_t tmp=r15&~0x21u;
    sr(b,0x15,tmp|3); sr(b,0x15,tmp|0x23); sr(b,0x15,tmp|3);
    e|=sw(s,b,0x15,r15,0xDF);
    int delay=b->delay_us(b->vga.arg,100);
    return e ? -EIO : delay;
}
static int streams_off(struct supersavage_mode *s, const struct supersavage_mode_io *b)
{
    int e=retrace(b);
    if (cw(s,b,0x67,rd(b,0x3D4,0x67)&~0x0Eu,255)) return -EIO;
    static const unsigned regs[]={0x81C8,0x81B8,0x81C0,0x81C4,0x81B0,0x81B4};
    for (unsigned i=0;i<ARRAY_SIZE(regs);i++) e|=mw(s,b,regs[i],0);
    return e ? -EIO : 0;
}
static void standard_mode(const struct supersavage_mode_io *b, const cvga_mode_params *v, bool packed)
{
    sr(b,0,1); out(b,0x3C2,v->misc|(packed ? 0x0C : 0));
    for (unsigned i=0;i<4;i++) sr(b,i+1,v->seq[i]|(i==0 ? 0x20 : 0));
    if (packed) sr(b,4,0x0E);
    cr(b,0x11,v->crtc[0x11]&0x7F);
    for (unsigned i=0;i<25;i++) if (i!=0x11) cr(b,i,packed && i==0x13 ? 80 : v->crtc[i]);
    if (packed) { cr(b,0x14,0); cr(b,0x17,0xEB); }
    cr(b,0x11,v->crtc[0x11]);
    for (unsigned i=0;i<9;i++) wr(b,0x3CE,i,packed && i==5 ? 0x40 : v->gc[i]);
    for (unsigned i=0;i<21;i++) {
        in(b,0x3DA); out(b,0x3C0,i);
        out(b,0x3C0,packed && i==0x10 ? 0x41 : i<20 ? v->attr[i] : 0);
    }
    in(b,0x3DA); out(b,0x3C0,0x20); sr(b,0,3);
}
static int verify_mode(struct supersavage_mode *s, const struct supersavage_mode_io *b,
                       const cvga_mode_params *v, bool packed)
{
    static const uint8_t sm[]={3,0x3D,15,0x3F,14},gm[]={15,15,15,31,3,0x7B,15,15,255};
    int e=compare(s,0x3CC,v->misc|(packed ? 12 : 0),in(b,0x3CC),0xEF);
    for (unsigned i=0;i<5;i++) {
        unsigned expected=i==0 ? 3 : v->seq[i-1]|(i==1 ? 0x20 : 0);
        if (packed && i==4) expected=0x0E;
        e|=compare(s,0x100+i,expected,rd(b,0x3C4,i),sm[i]);
    }
    for (unsigned i=0;i<25;i++) {
        unsigned expected=v->crtc[i];
        if (packed && i==0x13) expected=80;
        if (packed && i==0x14) expected=0;
        if (packed && i==0x17) expected=0xEB;
        e|=compare(s,i,expected,rd(b,0x3D4,i),255);
    }
    for (unsigned i=0;i<9;i++)
        e|=compare(s,0x300+i,packed && i==5 ? 0x40 : v->gc[i],rd(b,0x3CE,i),gm[i]);
    for (unsigned i=0;i<21;i++) {
        in(b,0x3DA); out(b,0x3C0,i);
        unsigned expected=packed && i==0x10 ? 0x41 : i<20 ? v->attr[i] : 0;
        e|=compare(s,0x400+i,expected,in(b,0x3C1),255);
    }
    in(b,0x3DA); out(b,0x3C0,0x20);
    return e ? -EIO : 0;
}
int supersavage_mode_enter(struct supersavage_mode *s, const struct supersavage_mode_io *b, uint16_t mode)
{
    if (!s->saved || !s->planes_saved || !supersavage_mode_supported(mode)) return -EOPNOTSUPP;
    int e=step(s,b,"native-streams-off"); if (e) return e;
    if ((e=streams_off(s,b))) return e;
    e=step(s,b,"native-panel-clock"); if (e) return e;
    bool packed=mode==0x101;
    /* Panel timing, expansion coefficients and MCLK stay firmware selected.
     * Clear only doubling, forced PLL and gamma/8-bit CLUT for legacy VGA. */
    if ((e=sw(s,b,0x15,s->sr[0x15]&~0x10u,0xDF)) ||
        (e=sw(s,b,0x18,s->sr[0x18]&~0x80u,255)) ||
        (e=sw(s,b,0x1B,packed ? 0x10 : 0,0x98))) return e;
    if (packed) {
        uint8_t r12,r13,r29; clock_25175(&r12,&r13,&r29);
        if ((e=pll(s,b,r12,r13,r29,0x83))) return e;
    }
    e=step(s,b,"native-vga-mode"); if (e) return e;
    const cvga_mode_params *v=cvga_find_mode(packed ? 0x12 : mode);
    standard_mode(b,v,packed);
    /* Legacy VGA mapping/shift/overflow controls are from RE046-B. Native
     * packed controls/stride use SavageModeInit's 8-bpp non-BIOS branch. */
    const struct reg8 legacy[]={{0x31,0},{0x32,(uint8_t)(s->cr[0x32]&~0x10u)},
        {0x33,0},{0x42,0},{0x43,0},{0x45,(uint8_t)(s->cr[0x45]&~1u)},
        {0x50,0},{0x51,0},{0x53,(uint8_t)(s->cr[0x53]&~0x10u)},
        {0x5B,0},{0x5D,0},{0x5E,0},{0x5F,0},{0x65,0},
        {0x66,(uint8_t)(s->cr[0x66]&~1u)},{0x67,0},
        {0x69,(uint8_t)(s->cr[0x69]&~0x1Fu)},{0x6A,0},{0x90,0},{0x91,0}};
    for (unsigned i=0;i<ARRAY_SIZE(legacy);i++) {
        unsigned r=legacy[i].index;
        uint8_t mask=255;
        for (unsigned j=0;j<ARRAY_SIZE(cr_regs);j++) if (cr_regs[j].index==r) mask=cr_regs[j].mask;
        if ((e=cw(s,b,r,legacy[i].mask,mask))) return e;
    }
    if (packed) {
        if ((e=cw(s,b,0x31,0x8C,0x4F)) || (e=cw(s,b,0x3A,(s->cr[0x3A]&0x87)|0x15,0xBB)) ||
            (e=cw(s,b,0x58,(s->cr[0x58]&0x80)|0x13,0x13)) ||
            (e=cw(s,b,0x34,0x10,255)) || (e=cw(s,b,0x3B,0x5A,255)) ||
            (e=cw(s,b,0x3C,0x2F,255)) || (e=cw(s,b,0x5E,0x40,0x57)) ||
            (e=cw(s,b,0x50,0x40,0xF1)) || (e=cw(s,b,0x90,0x80,255)) ||
            (e=cw(s,b,0x91,80,255)) || (e=cw(s,b,0x67,1,255))) return e;
    }
    e=step(s,b,"native-mode-readback"); if (e) return e;
    if ((e=verify_mode(s,b,v,packed))) return e;
    /* Standard state is installed again, with memory and palette, by the
     * f3-20 guest transport. Only release blanking after chipset readback. */
    sr(b,1,v->seq[0]); out(b,0x3C4,0); out(b,0x3D4,0);
    return 0;
}

int supersavage_mode_restore(struct supersavage_mode *s, const struct supersavage_mode_io *b)
{
    if (!s->saved) return 0;
    if (!s->touched) {
        cr(b,0x40,s->cr[0x40]); locks(s,b); s->saved=false; return 0;
    }
    uint64_t began=b->now_us(b->vga.arg);
    s->mask=0;
    (void)step(s,b,"native-restore-blank");
    unlock(b); sr(b,1,rd(b,0x3C4,1)|0x20);
    int e=cw(s,b,0x40,rd(b,0x3D4,0x40)|1,1);
    if (e) { locks(s,b); return e; }
    e=b->delay_us(b->vga.arg,100);
    /* A failed retrace/readback must not skip the remaining recovery. */
    e|=streams_off(s,b);
    (void)step(s,b,"native-restore-panel-clock");
    for (unsigned i=0;i<ARRAY_SIZE(sr_regs);i++) {
        unsigned r=sr_regs[i].index;
        if (r==0x15) continue;
        e|=sw(s,b,r,s->sr[r],sr_regs[i].mask);
    }
    e|=pll(s,b,s->sr[0x12],s->sr[0x13],s->sr[0x29],s->sr[0x15]);
    (void)step(s,b,"native-restore-vga");
    e|=cw(s,b,0x31,0,0x4F);
    e|=cw(s,b,0x3A,s->cr[0x3A]&~0x38u,0xBB);
    e|=cw(s,b,0x58,s->cr[0x58]&~0x10u,0x13);
    if (s->planes_saved) vga_hardware_load_blanked(&s->vga,&b->vga);
    else {
        /* Save was refused before the planar transfer. No VRAM was changed. */
        vga_hardware_unblank(&s->vga,&b->vga);
        sr(b,1,s->vga.seq[1]|0x20);
    }
    for (unsigned i=0;i<ARRAY_SIZE(cr_regs);i++) {
        unsigned r=cr_regs[i].index;
        if (r==0x67 || r==0x40 || r==0x36 || r==0x60 || r==0x68 || r==0x6F || r==0xB0) continue;
        e|=cw(s,b,r,s->cr[r],cr_regs[i].mask);
    }
    /* These saved memory-configuration registers never change in this
     * provider. Verify rather than rewrite strap/reset controls. */
    const unsigned unchanged[]={0x36,0x60,0x68,0x6F,0xB0};
    for (unsigned i=0;i<ARRAY_SIZE(unchanged);i++)
        e|=compare(s,unchanged[i],s->cr[unchanged[i]],rd(b,0x3D4,unchanged[i]),255);
    for (unsigned r=0x10;r<=0x11;r++) e|=compare(s,0x100+r,s->sr[r],rd(b,0x3C4,r),255);
    (void)step(s,b,"native-restore-streams");
    if (!(rd(b,0x3D4,0x67)&0x0E)) {
        for (unsigned i=0;i<SUPERSAVAGE_STREAM_REGS;i++) e|=mw(s,b,stream_reg(i),s->streams[i]);
    } else e=-EIO;
    e|=retrace(b); e|=cw(s,b,0x67,s->cr[0x67],255);
    e|=cw(s,b,0x40,s->cr[0x40],255);
    vga_hardware_unblank(&s->vga,&b->vga);
    unsigned bad=vga_hardware_verify(&s->vga,&b->vga);
    if (bad) { e=-EIO; compare(s,0x200,0,bad,255); }
    locks(s,b);
    s->restore_us=b->now_us(b->vga.arg)-began;
    if (!e) s->saved=s->touched=s->planes_saved=false;
    return e ? -EIO : 0;
}
