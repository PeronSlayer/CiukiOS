/* Synthetic VBE with legacy VGA and linear modes. RAM stays VM-owned in both display
 * paths; no guest address is interpreted as a host physical address.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/display.h>
#include "dosvm_internal.h"

void dosvm_vbe_init(struct dosvm *v)
{
    /* F3000..F37ff holds the complete 8x8 font; the remaining half of
     * this private, guest-read-only ROM page holds VBE data, never code. */
    uint8_t *rom=(uint8_t *)P2V(v->frames[0xF3])+0x800;
    const uint16_t modes[]={0x101,0x103,0x111,0x114,
                           0,1,2,3,0x0D,0x0E,0x10,0x11,0x12,0x13,0xFFFF};
    for (unsigned i=0;i<ARRAY_SIZE(modes);i++) vm_w16(rom+0x80+2*i,modes[i]);
    memcpy(rom+0x10,"CiukiOS",8);
    memcpy(rom+0x20,"Native SuperSavage VBE",22);
    memcpy(rom+0x40,"F3-37",6);
}
static bool vbe_backing(struct dosvm *v)
{ return v->vbe_buffer.used || dosvm_pool_alloc(v,&v->vbe_buffer,DOS_VBE_BYTES); }
static bool vbe_available(struct dosvm *v,uint16_t mode)
{
    (void)v;
    return dos_video_mode(mode) && display_fullscreen_supported(mode);
}
static const cvga_mode_params *vbe_vga_mode(unsigned mode)
{ return mode<=0x13 ? cvga_find_mode(mode) : 0; }
static void vbe_vga_info(uint8_t *info,const cvga_mode_params *m)
{
    bool packed=m->mode==0x13;
    /* Fixed VGA aperture, not a bank-switching or linear framebuffer. */
    vm_w16(info,m->text ? 0x000F : 0x001F); info[2]=6;
    vm_w16(info+4,m->text ? 32 : 64); vm_w16(info+6,m->text ? 32 : 64);
    vm_w16(info+8,m->segment);
    vm_w16(info+16,m->text ? m->columns*2 : packed ? m->width : m->width/8);
    vm_w16(info+18,m->text ? m->columns : m->width);
    vm_w16(info+20,m->text ? m->rows : m->height);
    info[22]=m->text ? m->width/m->columns : 8; info[23]=m->char_height;
    info[24]=m->text || packed ? 1 : 4; info[25]=packed ? 8 : 4;
    info[26]=1; info[27]=m->text ? 0 : packed ? 4 : 3;
    info[29]=m->text ? 7 : packed ? 0 : 0x10000/m->page_bytes-1;
    info[30]=1;
}
static int vbe_output(struct dosvm *v,struct v86_frame *f,const void *data,unsigned bytes)
{
    if (!vm_buffer(v,f->es,f->tf.edi,bytes,true)) return -EFAULT;
    return dosvm_write(v,vm_ptr(f->es,f->tf.edi),data,bytes);
}
int dosvm_vbe_bios(struct dosvm *v,struct v86_frame *f)
{
    unsigned ax=(uint16_t)f->tf.eax,bx=(uint16_t)f->tf.ebx;
    unsigned bl=bx&255;
    unsigned cx=(uint16_t)f->tf.ecx,dx=(uint16_t)f->tf.edx;
    bool ok=false;
    uint8_t info[512]={0};
    if (ax==0x4F00 && vbe_available(v,0x101)) {
        /* VBE 1.x callers supply 256 bytes; only VBE2 requests permit 512. */
        if (!vm_buffer(v,f->es,f->tf.edi,4,false) ||
            dosvm_read(v,vm_ptr(f->es,f->tf.edi),info,4)) return -EFAULT;
        unsigned bytes=!memcmp(info,"VBE2",4) ? 512 : 256;
        memset(info,0,sizeof(info)); memcpy(info,"VESA",4); vm_w16(info+4,0x0200);
        vm_w32(info+6,0xF0003810);vm_w32(info+14,0xF0003880);
        vm_w16(info+18,DOS_VBE_BYTES/65536);vm_w16(info+20,1);
        vm_w32(info+22,0xF0003810);vm_w32(info+26,0xF0003820);vm_w32(info+30,0xF0003840);
        int e=vbe_output(v,f,info,bytes); if (e) return e; ok=true;
    } else if (ax==0x4F01 && !(cx&~0x41FFu) &&
               vbe_vga_mode(cx&0x1FF) && vbe_available(v,0x101)) {
        vbe_vga_info(info,vbe_vga_mode(cx&0x1FF));
        int e=vbe_output(v,f,info,256); if (e) return e; ok=true;
    } else if (ax==0x4F01 && !(cx&~0x41FFu) && vbe_available(v,cx&0x1FF)) {
        const struct dos_video_mode *m=dos_video_mode(cx&0x1FF);
        /* Supported, colour, graphics, no VGA window, linear available.
         * 16-bpp is non-VGA-compatible; neither mode advertises banking. */
        vm_w16(info,0x00DB|(m->bpp==16 ? 0x20 : 0));
        vm_w16(info+16,m->pitch); vm_w16(info+18,m->width); vm_w16(info+20,m->height);
        info[22]=8; info[23]=16; info[24]=1; info[25]=m->bpp;
        info[27]=m->bpp==8 ? 4 : 6;
        if (m->bpp==16) { info[31]=5;info[32]=11;info[33]=6;info[34]=5;info[35]=5; }
        vm_w32(info+40,DOS_VBE_PHYS);
        int e=vbe_output(v,f,info,256); if (e) return e; ok=true;
    } else if (ax==0x4F02 && !(bx&~0x81FFu) &&
               vbe_vga_mode(bx&0x1FF) && vbe_available(v,0x101)) {
        /* Reuse the VGA BIOS transaction, including ordered native return,
         * BDA updates, font/palette setup and presenter invalidation. */
        struct v86_frame mode=*f;
        vm_low(&mode.tf.eax,(bx&0x1FF)|((bx&0x8000)>>8));
        int e=dosvm_vga_bios(v,&mode); if (e) return e; ok=true;
    } else if (ax==0x4F02 && (bx&0x4000) && !(bx&~0xC1FFu) && vbe_available(v,bx&0x1FF)) {
        if (vbe_backing(v)) {
            uint8_t ctl;
            if (dosvm_read(v,0x487,&ctl,1)) return -EFAULT;
            ctl=(ctl&0x7F)|((bx&0x8000)>>8);
            if (dosvm_write(v,0x487,&ctl,1)) return -EFAULT;
            dosvm_vga_sync(v,true);
            /* Capture the old VGA transport before changing its model. */
            if (v->fullscreen && !v->vbe_mode) dosvm_fullscreen_model_begin(v);
            if (!(bx&0x8000)) {
                for (unsigned i=0;i<v->vbe_buffer.pages;i++)
                    memset(P2V(as_lookup(&v->as,v->vbe_buffer.base+i*PAGE_SIZE,0)),0,PAGE_SIZE);
            }
            v->vbe_mode=bx&0x1FF; v->video_presented=false;
            v->video_blank=0;v->video_vblank_us=0;
            v->vga.display_changes++; dosvm_fullscreen_mode_changed(v);
            dosvm_launch_video(v); ok=true;
        }
    } else if (ax==0x4F03 && vbe_available(v,0x101)) {
        uint8_t mode,ctl;
        if (dosvm_read(v,0x449,&mode,1) || dosvm_read(v,0x487,&ctl,1)) return -EFAULT;
        vm_low(&f->tf.ebx,(v->vbe_mode ? v->vbe_mode|0x4000 : mode)|((ctl&0x80)<<8)); ok=true;
    } else if (ax==0x4F06 && bl==1 && v->vbe_mode) {
        const struct dos_video_mode *m=dos_video_mode(v->vbe_mode);
        vm_low(&f->tf.ebx,m->pitch); vm_low(&f->tf.ecx,m->width); vm_low(&f->tf.edx,m->height); ok=true;
    } else if (ax==0x4F07 && v->vbe_mode && (bx==1 || (bx==0 && !cx && !dx))) {
        vm_low(&f->tf.ecx,0); vm_low(&f->tf.edx,0); ok=true;
    } else if (ax==0x4F08 && vbe_available(v,0x101) && (bl==1 || (bl==0 && (bx>>8)==6))) {
        vm_low(&f->tf.ebx,0x0600); ok=true; /* only standard six-bit guest DAC */
    } else if (ax==0x4F09 && v->vbe_mode && (bl==0 || bl==1) && dx<=256 && cx<=256-dx) {
        bool read=bl==1; unsigned bytes=cx*4;
        if (!vm_buffer(v,f->es,f->tf.edi,bytes,read)) return -EFAULT;
        /* Validate the whole transfer before changing any DAC entry. */
        for (unsigned i=0;i<cx;i++) {
            uint8_t entry[4]={0}; unsigned at=vm_ptr(f->es,f->tf.edi)+4*i;
            if (read) {
                for (unsigned k=0;k<3;k++) entry[k]=v->vga.dac[dx+i][2-k];
                if (dosvm_write(v,at,entry,4)) return -EFAULT;
            } else {
                if (dosvm_read(v,at,entry,4)) return -EFAULT;
                for (unsigned k=0;k<3;k++) v->vga.dac[dx+i][2-k]=entry[k]&63;
            }
        }
        if (!read) { v->vga.display_changes++; v->video_presented=false; }
        ok=true;
    }
    vm_low(&f->tf.eax,ok ? 0x004F : 0x014F);
    return 0;
}

int dosvm_vbe_map(struct dosvm *v,struct v86_frame *f,unsigned ax)
{
    if (!v->dpmi.clients) return 1;
    unsigned client=v->dpmi.clients-1;
    uint32_t address=((uint32_t)(uint16_t)f->tf.ebx<<16)|(uint16_t)f->tf.ecx;
    if (ax==0x0801) {
        if (!v->vbe_mapped[client] || address!=v->vbe_buffer.base) return 1;
        v->vbe_mapped[client]=false; return 0;
    }
    uint32_t bytes=((uint32_t)(uint16_t)f->tf.esi<<16)|(uint16_t)f->tf.edi;
    if (ax!=0x0800 || address!=DOS_VBE_PHYS || !bytes || bytes>DOS_VBE_BYTES ||
        !vbe_available(v,0x101) || !vbe_backing(v)) return 1;
    v->vbe_mapped[client]=true;
    vm_low(&f->tf.ebx,v->vbe_buffer.base>>16); vm_low(&f->tf.ecx,v->vbe_buffer.base);
    return 0;
}

static uint32_t *vbe_pte(struct dosvm *v,unsigned page)
{
    uint32_t va=v->vbe_buffer.base+page*PAGE_SIZE;
    uint32_t *pd=P2V(v->as.pd_phys);
    return &((uint32_t *)P2V(pd[va>>22]&~0xFFFu))[(va>>12)&1023];
}
bool dosvm_vbe_dirty(struct dosvm *v,bool clear)
{
    bool dirty=false;
    for (unsigned i=0;i<v->vbe_buffer.pages;i++) {
        uint32_t *pte=vbe_pte(v,i);
        if (*pte&0x40) {
            dirty=true;
            if (clear) { *pte&=~0x40u; if (read_cr3()==v->as.pd_phys) invlpg(v->vbe_buffer.base+i*PAGE_SIZE); }
        }
    }
    return dirty;
}
int dosvm_vbe_flush(struct dosvm *v,bool force)
{
    const struct dos_video_mode *m=dos_video_mode(v->vbe_mode);
    if (!m || !v->vbe_buffer.used) return -EIO;
    unsigned bytes=m->pitch*m->height; bool dirty=false;
    for (unsigned i=0;i<(bytes+PAGE_SIZE-1)/PAGE_SIZE;i++) {
        uint32_t *pte=vbe_pte(v,i);
        if (!force && !(*pte&0x40)) continue;
        unsigned n=bytes-i*PAGE_SIZE; if (n>PAGE_SIZE) n=PAGE_SIZE;
        int e=display_fullscreen_write(i*PAGE_SIZE,P2V(*pte&~0xFFFu),n);
        if (e) return e;
        dirty|=!!(*pte&0x40); *pte&=~0x40u;
        if (read_cr3()==v->as.pd_phys) invlpg(v->vbe_buffer.base+i*PAGE_SIZE);
    }
    return dirty;
}
int dosvm_vbe_present(struct dosvm *v,int (*row)(void *,unsigned,const void *,unsigned),
                      void *arg,uint64_t tick,struct ciuki_rect *damage)
{
    const struct dos_video_mode *m=dos_video_mode(v->vbe_mode);
    if (!m || !v->vbe_buffer.used) return -EIO;
    if (v->video_presented && (tick-v->video_tick)*1000u<dos_video_frame_us(m)) return 0;
    bool dirty=dosvm_vbe_dirty(v,true);
    if (v->video_presented && !dirty && v->video_display==v->vga.display_changes) return 0;
    uint32_t display=v->vga.display_changes;
    uint16_t mode=v->vbe_mode;
    uint32_t base=v->vbe_buffer.base;
    v->video_present_rate=(unsigned)((1000000u+dos_video_frame_us(m)/2)/dos_video_frame_us(m));
    uint8_t pixels[1600];
    for (unsigned y=0;y<DOSVM_VIDEO_HEIGHT;y++) {
        if (!(y&3)) kpreempt_point();
        /* A mode change may release/reallocate the linear buffer. Never
         * dereference its old extent after a cooperative point. */
        if (v->vbe_mode!=mode || !v->vbe_buffer.used || v->vbe_buffer.base!=base ||
            v->fullscreen || v->fullscreen_hold || v->info.state>=DOSVM_STOPPING) {
            v->video_presented=false;return 0;
        }
        unsigned sy=y*m->height/DOSVM_VIDEO_HEIGHT;
        if (dosvm_read(v,base+sy*m->pitch,pixels,m->pitch)) {
            v->video_presented=false;return -EFAULT;
        }
        for (unsigned x=0;x<DOSVM_VIDEO_WIDTH;x++) {
            unsigned sx=x*m->width/DOSVM_VIDEO_WIDTH; uint32_t color;
            if (m->bpp==16) {
                unsigned c=vm_u16(pixels+2*sx),r=c>>11,g=(c>>5)&63,b=c&31;
                color=(((r<<3)|(r>>2))<<16)|(((g<<2)|(g>>4))<<8)|(b<<3)|(b>>2);
            } else {
                const uint8_t *p=v->vga.dac[pixels[sx]&v->vga.dac_mask];
                color=(((p[0]<<2)|(p[0]>>4))<<16)|(((p[1]<<2)|(p[1]>>4))<<8)|(p[2]<<2)|(p[2]>>4);
            }
            v->video_pixels[x]=color;
        }
        int e=row(arg,y,v->video_pixels,sizeof(v->video_pixels));
        if (e) { v->video_presented=false; return e; }
    }
    /* A surface writer may yield to the guest. Preserve a pending mode or
     * palette change rather than acknowledging a mixed frame as current. */
    v->video_presented=v->vbe_mode==mode && v->vga.display_changes==display;
    v->video_tick=tick;v->video_display=display;
    v->video_frames++; v->video_bands=(1ull<<(DOSVM_VIDEO_HEIGHT/DOSVM_VIDEO_BAND))-1;
    v->video_geometry=(cvga_geometry){.width=m->width,.height=m->height};
    *damage=(struct ciuki_rect){0,0,0,0,DOSVM_VIDEO_WIDTH,DOSVM_VIDEO_HEIGHT};
    return 1;
}
