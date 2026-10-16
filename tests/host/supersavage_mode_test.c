/* Production mode provider on indexed-port, stream and planar VRAM models.
 * Expected sequences are grounded in the references, not firmware stubs.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include "../../src/kernel/drivers/supersavage_mode.h"

struct card {
    cvga_state vga;
    uint8_t cr[256],sr[256];
    uint32_t streams[40];
    uint64_t us;
    unsigned reads,writes,memcycles,pll_edges,retrace_reads;
    unsigned panel_status_writes;
    bool no_retrace,decorated,freeze_time;
    unsigned drop_cr,drop_sr,drop_mm;
    bool drop_once;
    const char *fail_step;
    const char *stages[64];
    unsigned stage_count;
};
static struct card card,original;
static struct supersavage_mode snapshot;
static cvga_state guest,returned;
static bool unlocked(const struct card *c)
{ return (c->cr[0x38]&0xD3)==0x40 && (c->cr[0x39]&0xE0)==0xA0; }
static uint8_t input(void *arg,uint16_t p)
{
    struct card *c=arg; c->reads++; if (!c->freeze_time) c->us++;
    unsigned r=c->vga.crtc_index,s=c->vga.seq_index;
    if (p==0x3D5 && r>=25) {
        uint8_t value=c->cr[r];
        if (c->decorated && r==0x66) value^=0x44; /* reserved CR66 bits */
        return value;
    }
    if (p==0x3C5 && s>=5) return c->sr[s];
    if (p==0x3DA) {
        (void)cvga_read_port(&c->vga,p,0);
        return c->no_retrace ? 0 : (++c->retrace_reads&1) ? 0 : 8;
    }
    return cvga_read_port(&c->vga,p,0);
}
static bool drop(struct card *c,unsigned r,unsigned which)
{
    unsigned *target=which==0 ? &c->drop_cr : which==1 ? &c->drop_sr : &c->drop_mm;
    if (!*target || *target!=r) return false;
    if (c->drop_once) *target=0;
    return true;
}
static void output(void *arg,uint16_t p,uint8_t v)
{
    struct card *c=arg; c->writes++; if (!c->freeze_time) c->us++;
    unsigned r=c->vga.crtc_index,s=c->vga.seq_index;
    if (p==0x3C4) { c->vga.seq_index=v; return; }
    if (p==0x3D5 && r>=25) {
        if (r!=0x38 && r!=0x39 && !unlocked(c)) return;
        if (!drop(c,r,0)) c->cr[r]=v;
        return;
    }
    if (p==0x3C5 && s>=5) {
        if (s!=8 && c->sr[8]!=6) return;
        if (s>=0x58 && s<=0x5A) { c->panel_status_writes++; return; }
        if (s==0x5B) v=(v&1)|(c->sr[s]&0xFE);
        if (s==0x15 && (v&0x20)) c->pll_edges++;
        if (!drop(c,s,1)) c->sr[s]=v;
        return;
    }
    if (p==0x3C9 && (c->sr[0x1B]&0x10)) {
        c->vga.dac[c->vga.dac_index][c->vga.dac_component]=v;
        if (++c->vga.dac_component==3) { c->vga.dac_component=0; c->vga.dac_index++; }
        return;
    }
    cvga_write_port(&c->vga,p,v);
}
static uint8_t memory_read(void *arg,unsigned at)
{
    struct card *c=arg;
    assert(!(c->cr[0x31]&8) && !(c->cr[0x3A]&8));
    if (!c->freeze_time && !(++c->memcycles&63)) c->us++;
    return cvga_read_vram(&c->vga,0xA0000+at);
}
static void memory_write(void *arg,unsigned at,uint8_t v)
{
    struct card *c=arg;
    assert(!(c->cr[0x31]&8) && !(c->cr[0x3A]&8));
    if (!c->freeze_time && !(++c->memcycles&63)) c->us++;
    cvga_write_vram(&c->vga,0xA0000+at,v);
}
static uint32_t mm_read(void *arg,unsigned at)
{
    struct card *c=arg;
    assert(at>=0x8180 && at<=0x821C && !(at&3) && unlocked(c) && (c->cr[0x40]&1));
    if (!c->freeze_time) c->us+=2;
    return c->streams[(at-0x8180)/4];
}
static void mm_write(void *arg,unsigned at,uint32_t v)
{
    struct card *c=arg;
    assert(at>=0x8180 && at<=0x821C && !(at&3) && unlocked(c) && (c->cr[0x40]&1));
    if ((c->cr[0x67]&0x0E) || !(c->vga.seq[1]&0x20))
        fprintf(stderr,"stream write %x cr67=%x sr1=%x stage=%s\n",at,c->cr[0x67],c->vga.seq[1],snapshot.stage);
    assert(!(c->cr[0x67]&0x0E) && (c->vga.seq[1]&0x20));
    if (!drop(c,at,2)) c->streams[(at-0x8180)/4]=v;
    if (!c->freeze_time) c->us++;
}
static int delay(void *arg,unsigned us) { struct card *c=arg; c->us+=us; return 0; }
static uint64_t now(void *arg) { return ((struct card *)arg)->us; }
static int checkpoint(void *arg,const char *name,int error)
{
    struct card *c=arg; (void)error;
    assert(c->stage_count<ARRAY_SIZE(c->stages)); c->stages[c->stage_count++]=name;
    return c->fail_step && !strcmp(c->fail_step,name) ? -EIO : 0;
}
static const struct supersavage_mode_io bus={{&card,input,output,memory_read,memory_write},mm_read,mm_write,delay,now,checkpoint};
static void fixture(void)
{
    memset(&card,0,sizeof(card)); memset(&snapshot,0,sizeof(snapshot));
    cvga_init(&card.vga); assert(cvga_set_bios_mode(&card.vga,0x12,0));
    for (unsigned i=0;i<256;i++) { card.cr[i]=i*13+7; card.sr[i]=i*11+5; }
    card.cr[0x38]=card.cr[0x39]=card.sr[8]=0;
    card.sr[0x26]=0; card.cr[0x40]=0;
    card.cr[0x31]=0x8C; card.cr[0x3A]=0x15; card.cr[0x58]=0x13;
    card.cr[0x66]=0x91;card.cr[0x67]=0xD9;card.cr[0x6B]=3;
    card.sr[0x1B]=0x98;card.sr[0x61]=127;card.sr[0x66]=0;
    card.sr[0x69]=255;card.sr[0x6E]=0x20; /* 1024x768 firmware panel */
    for (unsigned i=0;i<40;i++) card.streams[i]=0x12340000+i*0x102;
    for (unsigned p=0;p<4;p++) for (unsigned a=0;a<65536;a++) card.vga.plane[p][a]=a*17+(a>>8)*23+p*41;
    for (unsigned i=0;i<256;i++) for (unsigned k=0;k<3;k++) card.vga.dac[i][k]=i*3+k*17;
    card.vga.seq_index=2;card.vga.crtc_index=12;card.vga.gc_index=4;
    card.vga.attr_index=0x32;card.vga.attr_data_phase=0;
    original=card;
}
static void restored(void)
{
    assert(!memcmp(card.vga.seq,original.vga.seq,5));
    assert(!memcmp(card.vga.crtc,original.vga.crtc,25));
    assert(!memcmp(card.vga.gc,original.vga.gc,9));
    assert(!memcmp(card.vga.attr,original.vga.attr,21));
    assert(!memcmp(card.vga.dac,original.vga.dac,768));
    assert(!memcmp(card.vga.plane,original.vga.plane,sizeof(card.vga.plane)));
    assert(!memcmp(card.cr,original.cr,256));
    for (unsigned i=0;i<256;i++) {
        unsigned mask=i>=0x58 && i<=0x5A ? 0 : i==0x5B ? 1 : 255;
        assert(!((card.sr[i]^original.sr[i])&mask));
    }
    assert(!card.panel_status_writes);
    assert(!memcmp(card.streams,original.streams,sizeof(card.streams)));
    assert(card.vga.seq_index==original.vga.seq_index && card.vga.crtc_index==original.vga.crtc_index);
    assert(card.vga.misc==original.vga.misc && card.vga.enable==original.vga.enable);
    assert(!snapshot.saved && snapshot.restore_us<200000);
}
static void cycles(void)
{
    const unsigned modes[]={3,0x13,0x101};
    for (unsigned i=0;i<ARRAY_SIZE(modes);i++) {
        fixture(); assert(!supersavage_mode_save(&snapshot,&bus));
        assert(snapshot.saved && snapshot.planes_saved && !supersavage_mode_enter(&snapshot,&bus,modes[i]));
        assert(!(card.cr[0x67]&0x0E) && !(card.vga.seq[1]&0x20));
        assert(!memcmp(card.sr+0x54,original.sr+0x54,8));
        assert(card.sr[0x10]==original.sr[0x10] && card.sr[0x11]==original.sr[0x11]);
        if (modes[i]==0x101) {
            assert(card.cr[0x67]==1 && card.cr[0x90]==0x80 && card.cr[0x91]==80);
            assert(card.cr[0x3B]==0x5A && card.cr[0x3C]==0x2F); /* SavageModeInit FIFO midpoint */
            assert(card.cr[0x31]==0x8C && card.vga.crtc[0x13]==80 && card.sr[0x1B]==0x10);
            assert((card.vga.misc&12)==12 && card.pll_edges);
            unsigned r=(card.sr[0x12]>>6)|((card.sr[0x29]&4)),n=(card.sr[0x12]&63)|((card.sr[0x29]&0x10)<<2);
            uint64_t hz=14318180ULL*(card.sr[0x13]+2)/((n+2)<<r);
            assert(hz>25000000 && hz<25300000);
        } else {
            assert(!card.cr[0x31] && !(card.sr[0x1B]&0x98));
            assert(card.sr[0x12]==original.sr[0x12] && card.sr[0x13]==original.sr[0x13]);
        }
        assert(!supersavage_mode_restore(&snapshot,&bus)); restored();
    }
    puts("PASS SuperSavage reference sequences: VGA 03/13 and packed 640x480x8, stream addressing off before reset, retained firmware panel/MCLK, PLL load, exact VGA/extended/streams/8-bit DAC/256 KiB restore (<200 ms model)");
}
static void presentation(void)
{
    for (unsigned y=0;y<2;y++) {
        fixture();assert(!supersavage_mode_save(&snapshot,&bus));assert(!supersavage_mode_enter(&snapshot,&bus,0x13));
        cvga_init(&guest);assert(cvga_set_bios_mode(&guest,0x13,0));
        if (y) { guest.seq[4]=6; guest.crtc[0x14]=0;guest.crtc[0x17]=0xE3; }
        for (unsigned p=0;p<4;p++) for (unsigned a=0;a<65536;a++) guest.plane[p][a]=(a+p)&63;
        for (unsigned i=0;i<256;i++) guest.dac[i][0]=guest.dac[i][1]=guest.dac[i][2]=i&63;
        vga_hardware_load(&guest,&bus.vga);assert(!vga_hardware_verify(&guest,&bus.vga));
        uint8_t pixels[320]; assert(cvga_render_row8(&card.vga,0,pixels,sizeof(pixels),CVGA_BLINK_VISIBLE)==320);
        bool nonzero=false;for (unsigned i=0;i<320;i++) nonzero|=pixels[i]!=0;assert(nonzero);
        vga_hardware_capture(&returned,&bus.vga);assert(!memcmp(guest.plane,returned.plane,sizeof(guest.plane)));
        assert(!supersavage_mode_restore(&snapshot,&bus)); restored();
    }
    puts("PASS SuperSavage guest transport: production mode 13h and unchained Mode Y plane/DAC upload, nonblack scanout, guest capture, exact desktop return");
}
static void failures(void)
{
    fixture();card.cr[0x6B]=0x82;
    assert(supersavage_mode_save(&snapshot,&bus)==-EOPNOTSUPP && !snapshot.saved);
    assert(!card.memcycles && !memcmp(card.sr+0x54,original.sr+0x54,8));
    assert(card.cr[0x40]==original.cr[0x40] && !card.cr[0x38] && !card.cr[0x39] && !card.sr[8] && !card.sr[0x26]);
    fixture();card.cr[0x6B]=0;
    assert(supersavage_mode_save(&snapshot,&bus)==-EOPNOTSUPP && !snapshot.saved && !card.memcycles);
    const char *steps[]={"native-save","native-save-planes","native-streams-off","native-panel-clock","native-vga-mode","native-mode-readback"};
    for (unsigned i=0;i<ARRAY_SIZE(steps);i++) {
        fixture();card.fail_step=steps[i];
        int e=supersavage_mode_save(&snapshot,&bus);
        if (!e) e=supersavage_mode_enter(&snapshot,&bus,0x13);
        assert(e==-EIO);card.fail_step=0;
        assert(!supersavage_mode_restore(&snapshot,&bus)); restored();
    }
    fixture();assert(!supersavage_mode_save(&snapshot,&bus));
    card.drop_cr=0x67;card.drop_once=true;
    assert(supersavage_mode_enter(&snapshot,&bus,0x13)==-EIO && snapshot.bad_reg==0x67);
    assert(!supersavage_mode_restore(&snapshot,&bus));restored();
    fixture();assert(!supersavage_mode_save(&snapshot,&bus));card.decorated=true;
    assert(!supersavage_mode_enter(&snapshot,&bus,0x13));
    assert(!supersavage_mode_restore(&snapshot,&bus));card.decorated=false;restored();
    fixture();assert(!supersavage_mode_save(&snapshot,&bus));assert(!supersavage_mode_enter(&snapshot,&bus,0x13));
    card.drop_mm=0x81C8;card.streams[(0x81C8-0x8180)/4]^=1;
    assert(supersavage_mode_restore(&snapshot,&bus)==-EIO && snapshot.saved && snapshot.bad_reg==0x81C8);
    card.drop_mm=0;assert(!supersavage_mode_restore(&snapshot,&bus));restored();
    fixture();assert(!supersavage_mode_save(&snapshot,&bus));assert(!supersavage_mode_enter(&snapshot,&bus,0x13));
    card.drop_sr=0x12;card.sr[0x12]^=1;
    assert(supersavage_mode_restore(&snapshot,&bus)==-EIO && snapshot.saved && snapshot.bad_reg==0x112);
    assert(card.cr[0x67]==original.cr[0x67]); /* remaining recovery did run */
    card.drop_sr=0;assert(!supersavage_mode_restore(&snapshot,&bus));restored();
    assert(snapshot.bad_stage==0);
    fixture();assert(!supersavage_mode_save(&snapshot,&bus));card.no_retrace=card.freeze_time=true;
    assert(supersavage_mode_enter(&snapshot,&bus,0x13)==-EIO && card.retrace_reads<131080);
    card.no_retrace=card.freeze_time=false;assert(!supersavage_mode_restore(&snapshot,&bus));restored();
    fixture();assert(!supersavage_mode_save(&snapshot,&bus));assert(!supersavage_mode_enter(&snapshot,&bus,0x13));
    card.fail_step="native-restore-panel-clock";
    assert(!supersavage_mode_restore(&snapshot,&bus));restored();
    puts("PASS SuperSavage recovery: unknown/inactive panel refusal, each entry checkpoint failure, CR67 write loss, reserved-bit masks, retained snapshot on PLL/stream restore failure, bounded frozen-clock retrace, recovery despite failed durable checkpoint");
}
static void panel_status(void)
{
    const unsigned retained[]={8,0x80,0xFF};
    for (unsigned i=0;i<ARRAY_SIZE(retained);i++) {
        fixture();card.sr[0x58]=0;original=card;
        assert(!supersavage_mode_save(&snapshot,&bus) && !snapshot.sr[0x58]);
        assert(!supersavage_mode_enter(&snapshot,&bus,0x13));
        card.sr[0x58]=retained[i];card.sr[0x59]^=0x71;
        card.sr[0x5A]^=0xFF;card.sr[0x5B]^=0xFE;
        assert(!supersavage_mode_restore(&snapshot,&bus));
        assert(card.sr[0x58]==retained[i] && !snapshot.mask);restored();
    }
    fixture();assert(!supersavage_mode_save(&snapshot,&bus));
    assert(!supersavage_mode_enter(&snapshot,&bus,0x13));
    card.sr[0x54]^=8;card.drop_sr=0x54;
    assert(supersavage_mode_restore(&snapshot,&bus)==-EIO && snapshot.saved);
    assert(snapshot.bad_reg==0x154 && snapshot.mask==255 &&
           !strcmp(snapshot.bad_stage,"native-restore-panel-clock"));
    assert(card.cr[0x67]==original.cr[0x67]);
    card.drop_sr=0;assert(!supersavage_mode_restore(&snapshot,&bus));restored();
    puts("PASS f3-36 panel restore: SR58 00->08/80/ff and derived SR59/5A/5B retained without writes/false EIO; lost writable SR54 remains EIO with first-failure stage");
}
int main(void) { cycles();presentation();failures();panel_status();puts("SuperSavage native mode host tests: PASS");return 0; }
