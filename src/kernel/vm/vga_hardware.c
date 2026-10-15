/* VGA indexed registers, palette and planar memory transport. See the dated
 * f3-20 firmware ownership amendment for latch/readback limitations.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "vga_hardware.h"
static uint8_t hw_rd(const struct vga_hardware_bus *b, uint16_t p) { return b->in(b->arg,p); }
static void hw_wr(const struct vga_hardware_bus *b, uint16_t p, uint8_t v) { b->out(b->arg,p,v); }
static void indexed(const struct vga_hardware_bus *b, uint16_t p, unsigned i, uint8_t v)
{ hw_wr(b,p,(uint8_t)i); hw_wr(b,p+1,v); }
static uint16_t crport(const cvga_state *v) { return v->misc & 1 ? 0x3D4 : 0x3B4; }
static void registers(const cvga_state *v, const struct vga_hardware_bus *b, int blank)
{
    uint16_t c=crport(v);
    hw_wr(b,0x3C3,1);
    indexed(b,0x3C4,0,1); /* synchronous reset before changing clocks */
    hw_wr(b,0x3C2,v->misc);
    for (unsigned i=1;i<5;i++) indexed(b,0x3C4,i,v->seq[i] | (i==1 && blank ? 0x20 : 0));
    indexed(b,0x3C4,0,v->seq[0]);
    indexed(b,c,0x11,v->crtc[0x11]&0x7F);
    for (unsigned i=0;i<25;i++) if (i!=0x11) indexed(b,c,i,v->crtc[i]);
    indexed(b,c,0x11,v->crtc[0x11]);
    for (unsigned i=0;i<9;i++) indexed(b,0x3CE,i,v->gc[i]);
    for (unsigned i=0;i<21;i++) {
        hw_rd(b,c+6); hw_wr(b,0x3C0,(uint8_t)i); hw_wr(b,0x3C0,v->attr[i]);
    }
    hw_wr(b,c+6,v->feature);
    hw_wr(b,0x3C6,v->dac_mask); hw_wr(b,0x3C8,0);
    for (unsigned i=0;i<256;i++) for (unsigned k=0;k<3;k++) hw_wr(b,0x3C9,v->dac[i][k]);
}
static void indexes(const cvga_state *v, const struct vga_hardware_bus *b)
{
    hw_wr(b,0x3C4,v->seq_index); hw_wr(b,0x3CE,v->gc_index); hw_wr(b,crport(v),v->crtc_index);
    hw_rd(b,crport(v)+6); hw_wr(b,0x3C0,v->attr_index);
    if (!v->attr_data_phase) hw_rd(b,crport(v)+6);
    hw_wr(b,v->dac_read_mode ? 0x3C7 : 0x3C8,v->dac_index);
    /* Restore a partial DAC transaction by replaying its preceding components. */
    for (unsigned k=0;k<v->dac_component && k<3;k++) {
        if (v->dac_read_mode) hw_rd(b,0x3C9);
        else hw_wr(b,0x3C9,v->dac[v->dac_index][k]);
    }
    hw_wr(b,0x3C3,v->enable);
}
static void planar(const struct vga_hardware_bus *b)
{
    indexed(b,0x3C4,1,0x21); /* blank during transfer */
    indexed(b,0x3C4,4,6); /* extended, sequential, chain-4 off */
    indexed(b,0x3CE,0,0); indexed(b,0x3CE,1,0); indexed(b,0x3CE,3,0);
    indexed(b,0x3CE,5,0); indexed(b,0x3CE,6,5); indexed(b,0x3CE,8,0xFF);
}
void vga_hardware_read_registers(cvga_state *v, const struct vga_hardware_bus *b, bool dac8)
{
    v->misc=hw_rd(b,0x3CC); v->feature=hw_rd(b,0x3CA); v->enable=hw_rd(b,0x3C3);
    v->seq_index=hw_rd(b,0x3C4); v->gc_index=hw_rd(b,0x3CE); v->crtc_index=hw_rd(b,crport(v));
    v->attr_index=hw_rd(b,0x3C0); v->attr_data_phase=0;
    v->dac_mask=hw_rd(b,0x3C6); v->dac_index=hw_rd(b,0x3C8); v->dac_component=0; v->dac_read_mode=0;
    for (unsigned i=0;i<5;i++) { hw_wr(b,0x3C4,i); v->seq[i]=hw_rd(b,0x3C5); }
    for (unsigned i=0;i<9;i++) { hw_wr(b,0x3CE,i); v->gc[i]=hw_rd(b,0x3CF); }
    for (unsigned i=0;i<25;i++) { hw_wr(b,crport(v),i); v->crtc[i]=hw_rd(b,crport(v)+1); }
    for (unsigned i=0;i<21;i++) {
        hw_rd(b,crport(v)+6); hw_wr(b,0x3C0,i); v->attr[i]=hw_rd(b,0x3C1);
    }
    hw_wr(b,0x3C7,0);
    for (unsigned i=0;i<256;i++) for (unsigned k=0;k<3;k++)
        v->dac[i][k]=hw_rd(b,0x3C9)&(dac8 ? 255 : 63);
    indexes(v,b);
}
void vga_hardware_capture_planes(cvga_state *v, const struct vga_hardware_bus *b)
{
    planar(b);
    for (unsigned p=0;p<4;p++) {
        indexed(b,0x3CE,4,p);
        for (unsigned a=0;a<CVGA_PLANE_SIZE;a++) v->plane[p][a]=b->read(b->arg,a);
    }
    /* No standard latch readback exists. Preserve every VRAM byte, and make
     * the model agree with the latches loaded by the final capture read. */
    for (unsigned p=0;p<4;p++) v->latch[p]=v->plane[p][CVGA_PLANE_SIZE-1];
    for (unsigned p=0;p<4;p++) for (unsigned i=0;i<CVGA_DIRTY_BYTES;i++) v->dirty[p][i]=0xFF;
    v->changes++; v->display_changes++;
}
void vga_hardware_capture(cvga_state *v, const struct vga_hardware_bus *b)
{
    vga_hardware_read_registers(v,b,false);
    vga_hardware_capture_planes(v,b);
    registers(v,b,0); indexes(v,b);
}
static void load(const cvga_state *v, const struct vga_hardware_bus *b, int blank)
{
    registers(v,b,1); planar(b);
    for (unsigned p=0;p<4;p++) {
        indexed(b,0x3C4,2,1u<<p);
        for (unsigned a=0;a<CVGA_PLANE_SIZE;a++) b->write(b->arg,a,v->plane[p][a]);
    }
    /* Load latches through a temporary byte, then restore the original byte
     * with mode-0 writes (writes do not load latches). No reserved guest byte. */
    for (unsigned p=0;p<4;p++) {
        indexed(b,0x3C4,2,1u<<p); b->write(b->arg,0xFFFF,v->latch[p]);
    }
    b->read(b->arg,0xFFFF);
    for (unsigned p=0;p<4;p++) {
        indexed(b,0x3C4,2,1u<<p); b->write(b->arg,0xFFFF,v->plane[p][0xFFFF]);
    }
    registers(v,b,blank); indexes(v,b);
}
void vga_hardware_load(const cvga_state *v, const struct vga_hardware_bus *b) { load(v,b,0); }
void vga_hardware_load_blanked(const cvga_state *v, const struct vga_hardware_bus *b) { load(v,b,1); }
void vga_hardware_unblank(const cvga_state *v, const struct vga_hardware_bus *b)
{ indexed(b,0x3C4,1,v->seq[1]); indexes(v,b); }

unsigned vga_hardware_verify(const cvga_state *v, const struct vga_hardware_bus *b)
{
    static const uint8_t sr_mask[]={3,0x3D,15,0x3F,14};
    static const uint8_t gr_mask[]={15,15,15,31,3,0x7B,15,15,255};
    unsigned bad=0;
    if (((hw_rd(b,0x3CC)^v->misc)&0xEF) || (hw_rd(b,0x3C3)&1)!=(v->enable&1)) bad|=1;
    for (unsigned i=0;i<5;i++) {
        hw_wr(b,0x3C4,i);
        if ((hw_rd(b,0x3C5)^v->seq[i])&sr_mask[i]) bad|=2;
    }
    for (unsigned i=0;i<9;i++) {
        hw_wr(b,0x3CE,i);
        if ((hw_rd(b,0x3CF)^v->gc[i])&gr_mask[i]) bad|=4;
    }
    uint16_t c=crport(v);
    for (unsigned i=0;i<25;i++) {
        hw_wr(b,c,i);
        if (hw_rd(b,c+1)!=v->crtc[i]) bad|=8;
    }
    for (unsigned i=0;i<21;i++) {
        hw_rd(b,c+6); hw_wr(b,0x3C0,i);
        if (hw_rd(b,0x3C1)!=v->attr[i]) bad|=16;
    }
    indexes(v,b);
    return bad;
}
