/* Independent HiQV register executor plus real registry/adapter lifecycle.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ciuki/kernel.h>
#include <ciuki/swdib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%u: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
/* Only the privileged boundary is substituted; no production test branch. */
#define CIUKI_CPU_H
#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
static uint8_t inb(uint16_t);
static void outb(uint16_t,uint8_t);
static uint32_t irq_save(void);
static void irq_restore(uint32_t);
#include "../../src/kernel/drivers/video/chipshiqv/chipshiqv.c"
#include "../../src/kernel/drivers/video/registry.c"
const struct display_driver s3savage_driver={0};
struct ciuki_boot_info g_boot;
volatile uint64_t g_ticks;
bool g_cpu_tsc=true;
uint64_t g_tsc_per_ms=1000;

static uint8_t video[0x100000], reference[sizeof(video)], saved[sizeof(video)];
static uint32_t mmio[PAGE_SIZE/4];
static uint8_t xr_regs[256],fr_regs[256],xr_index,fr_index,dac_index,dac_values[6];
static unsigned irq_depth, ports, delays, resets, drains, gos, drawing, marker_calls;
static uint64_t now;
static bool busy, dead, reset_works, frozen, ignore_hide, ignore_depth, corrupt_draw, lease_live, stall_go;
static unsigned busy_reads, probe_calls, pause_calls;
static int claim_fail_at, commit_error, close_error, stat_error, map_fail, pause_error;
static unsigned claim_calls, map_calls;
static uint32_t marker_size;
static bool marker_short;
static struct pci_func pci[2];
static unsigned functions;
static struct fb_device fb;
static struct resource resources[16];
static unsigned resource_used;
static struct { uint32_t reg,value; } trace[8192];
static unsigned trace_count;
static struct fat_volume volume;
static struct vfs vfs;

static uint32_t irq_save(void) { CHECK(!irq_depth); irq_depth++; return 0x200; }
static void irq_restore(uint32_t flags) { CHECK(flags==0x200 && irq_depth==1); irq_depth--; }
static uint8_t inb(uint16_t port)
{
    CHECK(irq_depth); ports++;
    if (port==0x3d6) return xr_index;
    if (port==0x3d0) return fr_index;
    if (port==0x3c8) return dac_index;
    if (port==0x3d1) return fr_regs[fr_index];
    CHECK(port==0x3d7);
    if (dead) return 255;
    if (xr_index==0x20) {
        busy_reads++;
        return xr_regs[0x20]|(busy ? 1u : 0);
    }
    return xr_regs[xr_index];
}
static void outb(uint16_t port,uint8_t value)
{
    CHECK(irq_depth); ports++;
    if (port==0x3d6) { xr_index=value; return; }
    if (port==0x3d0) { fr_index=value; return; }
    if (port==0x3c8) { dac_index=value; return; }
    if (port==0x3c9) {
        CHECK(!(xr_regs[0xa0]&7u) && dac_index>=4 && dac_index<10);
        dac_values[dac_index++-4]=value; return;
    }
    CHECK(port==0x3d7);
    if (xr_index==0xa2 || xr_index==0xa3) CHECK(!(xr_regs[0xa0]&7u));
    if (xr_index==0xa0 && ignore_hide && !(value&7u)) return;
    if (xr_index==0x20) {
        if (value&2u) resets++;
        if (!(value&2u) && (xr_regs[0x20]&2u) && reset_works) busy=false;
        if (ignore_depth) return;
    }
    xr_regs[xr_index]=value;
}
uint64_t ktime_cycles(void) { return now; }
int udelay(uint32_t us) { CHECK(!irq_depth); delays++; if (!frozen) now+=us; return 0; }
void framebuffer_write_drain(void) { CHECK(!irq_depth); drains++; }
void klog(const char *fmt,...) { (void)fmt; CHECK(!drawing); }
const struct fb_device *fbdev_get(void) { return &fb; }
unsigned pci_count(void) { return functions; }
const struct pci_func *pci_get(unsigned i) { return i<functions ? &pci[i] : 0; }
const struct resource *registry_get(unsigned i) { return i<resource_used ? &resources[i] : 0; }
bool registry_valid(int h,gen_t gen)
{
    const struct resource *r=h>=0 ? registry_get((unsigned)h) : 0;
    return lease_live && r && r->generation==gen && (r->state==RS_ACTIVE || r->state==RS_CLAIMED);
}
int registry_claim(enum res_type type,uint32_t begin,uint32_t end,const char *name,bool share)
{
    CHECK(!share && !drawing && resource_used<ARRAY_SIZE(resources));
    claim_calls++; if ((int)claim_calls==claim_fail_at) return -EBUSY;
    for (unsigned i=0;i<resource_used;i++) if (resources[i].type==type && resources[i].state!=RS_RELEASED &&
        begin<resources[i].end && end>resources[i].start) return -EBUSY;
    int h=(int)resource_used++;
    resources[h]=(struct resource){type,begin,end,name,(gen_t)(100+h),RS_CLAIMED,false}; return h;
}
int registry_activate(int h,gen_t gen)
{ CHECK(registry_valid(h,gen)); resources[h].state=RS_ACTIVE; return 0; }
int registry_release(int h,gen_t gen)
{ CHECK(registry_valid(h,gen) && resources[h].state!=RS_ACTIVE); resources[h].state=RS_RELEASED; return 0; }
int registry_quiesce(int h,gen_t gen,registry_idle_proof_fn proof)
{
    CHECK(registry_valid(h,gen));
    resources[h].state=RS_QUIESCING;
    if (!proof(h,gen)) { resources[h].state=RS_QUARANTINED; return -EIO; }
    resources[h].state=RS_CLAIMED; return 0;
}
void *vmm_reserve_mmio(uint32_t bytes) { return bytes==PAGE_SIZE ? (void *)mmio : video+0x80000; }
void *vmm_replace_mmio(void *window,uint32_t capacity,uint32_t phys,uint32_t bytes)
{
    (void)window; CHECK(capacity==bytes); map_calls++;
    if ((int)map_calls==map_fail) return 0;
    if (phys==fb.phys+HIQV_MMIO_OFFSET) return mmio;
    CHECK(phys>=fb.phys+fb.size && phys-fb.phys+bytes<=sizeof(video)); return video+(phys-fb.phys);
}
void vmm_revoke_mmio(void *ptr,uint32_t bytes) { CHECK(ptr && bytes && !engine.configured && engine.cpu_safe); }

/* Independent executor interprets the register packet, not the driver helper.
 * It copies bytes in hardware order, so incorrect overlap directions corrupt
 * results. Reference images are generated independently by swdib. */
static void fake_br(void *ctx,uint32_t reg,uint32_t value)
{
    (void)ctx; CHECK(!irq_depth && !busy && reg<=HIQV_GO && !(reg&3u));
    CHECK(trace_count<ARRAY_SIZE(trace)); trace[trace_count].reg=reg; trace[trace_count++].value=value;
    mmio[reg/4]=value;
    if (reg!=HIQV_GO) return;
    CHECK(!busy && !(xr_regs[0x20]&2u)); gos++;
    unsigned width=value&8191u,height=(value>>16)&8191u,bytes=fb.bpp/8;
    unsigned pitch=mmio[HIQV_PITCH/4]>>16, source_pitch=mmio[HIQV_PITCH/4]&8191u;
    uint32_t rop=mmio[HIQV_ROP/4], dest=mmio[HIQV_DST/4], src=mmio[HIQV_SRC/4];
    CHECK(!(rop&0xc00u)); /* Neither source nor destination is system memory. */
    for (unsigned y=0;y<height;y++) for (unsigned b=0;b<width;b++) {
        int step=(rop&0x100u) ? -(int)b : (int)b;
        int row=(rop&0x200u) ? -(int)y : (int)y;
        int64_t d=(int64_t)dest+row*(int)pitch+step;
        CHECK(d>=0 && d<(int64_t)sizeof(video));
        uint8_t pixel;
        if ((rop&255u)==0xccu) {
            int64_t s=(int64_t)src+row*(int)source_pitch+step;
            CHECK(s>=0 && s<(int64_t)sizeof(video)); pixel=video[s];
        } else {
            CHECK((rop&255u)==0xf0u && (rop&0x40000u));
            unsigned shift=b%bytes;
            uint32_t colour=mmio[HIQV_FG/4];
            if (!(rop&0x80000u)) {
                unsigned col=((dest%pitch)/bytes+b/bytes)&7u;
                unsigned py=((rop>>20)+y)&7u;
                uint32_t pat=mmio[HIQV_PATTERN/4]; CHECK(pat+py<sizeof(video));
                colour=(video[pat+py]&(0x80u>>col)) ? colour : mmio[HIQV_BG/4];
            }
            pixel=(uint8_t)(colour>>(shift*8));
        }
        video[d]=pixel;
    }
    if (corrupt_draw) video[dest]^=0x40;
    if (stall_go) busy=true;
}
int fbdev_accel_probe(const struct fb_device *d,int (*probe)(void *),void *ctx)
{
    CHECK(d==&fb && !drawing && !irq_depth); probe_calls++; drawing++;
    engine.io.write_br=fake_br;
    int e=probe(ctx); drawing--; return e;
}
static int pause_test(void *ctx)
{ (void)ctx; CHECK(!irq_depth); pause_calls++; return pause_error; }

static void fs_call(void) { CHECK(!drawing && !irq_depth); marker_calls++; }
int vfs_table_init(struct vfs *v,struct vfs_table *t,unsigned drive)
{ (void)t; fs_call(); CHECK(v==&vfs && drive==2); return 0; }
void vfs_table_destroy(struct vfs_table *t) { (void)t; fs_call(); }
int vfs_stat(struct vfs_table *t,const char *path,struct fat_entry *entry)
{ (void)t; fs_call(); CHECK(!strcmp(path,"C:/system/settings/accel.cfg")); entry->size=marker_size; return stat_error; }
int vfs_open(struct vfs_table *t,const char *path,unsigned flags,enum vfs_share share,uint8_t attr)
{ (void)t; (void)path; (void)attr; fs_call(); CHECK(flags==(VFS_WRITE|VFS_CREATE) && share==VFS_DENY_ALL); return 3; }
int vfs_write(struct vfs_table *t,int fd,const void *p,size_t len,size_t *done)
{ (void)t; fs_call(); CHECK(fd==3 && strstr(p,"driver=chipshiqv")); *done=len-(marker_short ? 1u : 0); marker_size=(uint32_t)*done; return 0; }
int vfs_truncate(struct vfs_table *t,int fd,uint64_t size)
{ (void)t; fs_call(); CHECK(fd==3 && !size); marker_size=0; return 0; }
int vfs_commit(struct vfs_table *t,int fd) { (void)t; fs_call(); CHECK(fd==3); return commit_error; }
int vfs_close(struct vfs_table *t,int fd) { (void)t; fs_call(); CHECK(fd==3); return close_error; }

static void setup(unsigned bpp)
{
    memset(&g_boot,0,sizeof(g_boot)); g_boot.display.vram_bytes=sizeof(video);
    memset(resources,0,sizeof(resources)); memset(&engine,0,sizeof(engine));
    memset(mmio,0,sizeof(mmio)); memset(xr_regs,0,sizeof(xr_regs)); memset(fr_regs,0,sizeof(fr_regs));
    unsigned bytes=bpp/8;
    fb=(struct fb_device){.present=true,.width=64,.height=48,.pitch=72*bytes,.size=72*bytes*48,
        .phys=0xe0000000,.mapped=video,.bpp=(uint8_t)bpp,.generation=42,.handle=0,
        .red_size=bpp==16 ? 5 : 8,.red_pos=bpp==16 ? 11 : 16,.green_size=bpp==16 ? 6 : 8,
        .green_pos=bpp==16 ? 5 : 8,.blue_size=bpp==16 ? 5 : 8};
    resources[0]=(struct resource){RES_MMIO,fb.phys,fb.phys+fb.size,"fbdev",42,RS_ACTIVE,false}; resource_used=1;
    pci[0]=(struct pci_func){.vendor=0x102c,.device=0xe5,.class_code=3,.command=3,.bar={fb.phys}};
    pci[1]=pci[0]; functions=1;
    xr_regs[0]=0x2c; xr_regs[1]=0x10; xr_regs[2]=0xe5; xr_regs[0x0a]=2;
    xr_regs[0x80]=0x80; xr_index=0x55; fr_index=0x66; dac_index=1;
    for (unsigned i=0;i<sizeof(video);i++) video[i]=(uint8_t)(i*71u+i/31u);
    memcpy(reference,video,sizeof(video)); memcpy(saved,video,sizeof(video));
    ports=delays=resets=drains=gos=drawing=marker_calls=busy_reads=probe_calls=pause_calls=0;
    now=g_ticks=0; irq_depth=0; trace_count=0; claim_calls=map_calls=0;
    busy=dead=frozen=ignore_hide=ignore_depth=corrupt_draw=marker_short=stall_go=false; reset_works=lease_live=true;
    claim_fail_at=commit_error=close_error=stat_error=map_fail=pause_error=0; marker_size=0;
    attempt_owned=attempt_blocked=false; decline_reason="pending";
    registers=0; scratch=0; port_claim=reg_claim=scratch_claim=-1;
    bound=0; owner=0; memset(&cached_snapshot,0,sizeof(cached_snapshot));
    memset(native_count,0,sizeof(native_count)); memset(software_count,0,sizeof(software_count));
    memset(&volume,0,sizeof(volume)); volume.mounted=true; memset(&vfs,0,sizeof(vfs)); vfs.volumes[2]=&volume;
    CHECK(display_driver_bind(&fb)==&chipshiqv_driver);
}
static const struct accel_ops *activate(void)
{
    CHECK(!hiqv_attempt_begin(&vfs)); CHECK(marker_size && !gos);
    const struct accel_ops *ops=hiqv_start(&fb,0);
    CHECK(ops && probe_calls==1 && engine.active && engine.cpu_safe && engine.cursor_hidden);
    CHECK(!hiqv_attempt_end(&vfs) && !marker_size);
    CHECK(!memcmp(video,saved,fb.size)); trace_count=0;
    memcpy(reference,video,sizeof(video)); return ops;
}
static void golden(void)
{
    CHECK(!memcmp(video,reference,fb.size));
    uint32_t tail=engine.scratch_offset+HIQV_SCRATCH_BYTES;
    CHECK(!memcmp(video+fb.size,reference+fb.size,engine.scratch_offset-fb.size));
    CHECK(tail<=sizeof(video) && !memcmp(video+tail,reference+tail,sizeof(video)-tail));
}
static struct fb_device ref_device(void) { struct fb_device r=fb; r.mapped=reference; return r; }
static void packet(unsigned i,uint32_t reg,uint32_t value)
{ CHECK(i<trace_count && trace[i].reg==reg && trace[i].value==value); }

static void drawing_tests(unsigned bpp)
{
    setup(bpp); const struct accel_ops *ops=activate(); struct fb_device ref=ref_device();
    unsigned bytes=bpp/8;
    CHECK(ops->capabilities(ops->ctx)==3);
    struct fb_rect r={3,2,19,13}; uint32_t colour=swdib_pack(&fb,0x3659c2);
    CHECK(!ops->fill_rect(ops->ctx,&r,colour)); swdib_fill(&ref,&r,colour); golden();
    CHECK(trace_count==6); packet(0,HIQV_BG,colour); packet(1,HIQV_FG,colour);
    packet(2,HIQV_ROP,0xc00f0); packet(3,HIQV_PITCH,fb.pitch<<16);
    packet(4,HIQV_DST,2*fb.pitch+3*bytes); packet(5,HIQV_GO,(13u<<16)|19*bytes);
    const int targets[][2]={{8,7},{0,0},{8,0},{0,7},{8,2},{0,2},{3,7},{3,0},{3,2}};
    for (unsigned i=0;i<ARRAY_SIZE(targets);i++) {
        trace_count=0; r=(struct fb_rect){3,2,23,20}; int x=targets[i][0],y=targets[i][1];
        CHECK(!ops->copy_rect(ops->ctx,&r,x,y)); swdib_copy(&ref,&r,x,y); golden();
        uint32_t rop=0xccu|(x>3 ? 0x100u : 0)|(y>2 ? 0x200u : 0);
        CHECK(trace_count==5); packet(0,HIQV_ROP,rop); packet(1,HIQV_PITCH,(fb.pitch<<16)|fb.pitch);
        packet(2,HIQV_SRC,(y>2 ? 21u : 2u)*fb.pitch+3*bytes+(x>3 ? 23*bytes-1 : 0));
        packet(3,HIQV_DST,(unsigned)(y+(y>2 ? 19 : 0))*fb.pitch+(unsigned)x*bytes+(x>3 ? 23*bytes-1 : 0));
        packet(4,HIQV_GO,(20u<<16)|23*bytes);
    }
    for (unsigned y=0;y<8;y++) for (unsigned x=0;x<8;x++) {
        const uint8_t pat[8]={0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x01};
        r=(struct fb_rect){(int)x,(int)y,19,13}; trace_count=0;
        CHECK(!ops->pattern_rect(ops->ctx,&r,pat,colour,0)); swdib_pattern(&ref,&r,pat,colour,0); golden();
        CHECK(trace_count==8); packet(0,HIQV_PATTERN,engine.scratch_offset+PAGE_SIZE);
        packet(6,HIQV_ROP,0x400f0|(y<<20));
    }
    uint8_t upload[10*128]; for (unsigned i=0;i<sizeof(upload);i++) upload[i]=(uint8_t)(i*19);
    struct fb_rect dst={2,3,20,10},clip={5,5,13,6}; trace_count=0;
    CHECK(!ops->write_rect(ops->ctx,&dst,&clip,upload,128)); swdib_write(&ref,&dst,&clip,upload,128); golden();
    CHECK(trace_count==30); packet(0,HIQV_ROP,0xcc); packet(1,HIQV_PITCH,fb.pitch<<16);
    packet(2,HIQV_SRC,engine.scratch_offset+HIQV_UPLOAD_OFFSET);
    packet(3,HIQV_DST,5*fb.pitch+5*bytes); packet(4,HIQV_GO,(1u<<16)|13*bytes);
    unsigned prior=gos; CHECK(!ops->sync(ops->ctx) && gos==prior && ops->cpu_ready(ops->ctx));
    CHECK(!ops->cursor_show(ops->ctx,true));
    CHECK(!ops->stop(ops->ctx)); CHECK(!(xr_regs[0xa0]&7u));
    CHECK(port_claim<0 && reg_claim<0 && scratch_claim<0 && !registers && !scratch);
}
static void cursor_tests(void)
{
    for (unsigned bpp=16;bpp<=24;bpp+=8) {
        setup(bpp); const struct accel_ops *ops=activate();
        struct accel_cursor c={.foreground=swdib_pack(&fb,0xf87230),.background=swdib_pack(&fb,0x1256a3)};
        c.mask[0]=0x60000000; c.image[0]=0xa0000000;
        CHECK(!ops->cursor_set(ops->ctx,&c)); CHECK(engine.cursor_hidden && !(xr_regs[0xa0]&7u));
        CHECK(engine.scratch[0]==0x9f && engine.scratch[8]==0x20);
        CHECK(engine.scratch[4]==255 && engine.scratch[12]==0);
        CHECK(engine.scratch[32*16]==255 && engine.scratch[32*16+8]==0);
        CHECK(xr_regs[0xa2]==((engine.scratch_offset>>8)&0xf0u));
        CHECK(xr_regs[0xa3]==(engine.scratch_offset>>16));
        CHECK(xr_index==0x55 && dac_index==1 && xr_regs[0x80]==0x80);
        uint8_t saved_palette[6]; memcpy(saved_palette,dac_values,6);
        xr_regs[0x80]=0; CHECK(!ops->cursor_set(ops->ctx,&c));
        for (unsigned i=0;i<6;i++) CHECK(dac_values[i]==saved_palette[i]>>2);
        CHECK(!ops->cursor_move(ops->ctx,-31,-17)); CHECK(xr_regs[0xa4]==31 && xr_regs[0xa5]==0x80);
        CHECK(xr_regs[0xa6]==17 && xr_regs[0xa7]==0x80);
        CHECK(!ops->cursor_move(ops->ctx,2047,0)); CHECK(xr_regs[0xa4]==255 && xr_regs[0xa5]==7);
        unsigned previous=ports; CHECK(ops->cursor_move(ops->ctx,INT32_MIN,0)==-EOPNOTSUPP && ports==previous);
        CHECK(!ops->cursor_show(ops->ctx,true) && (xr_regs[0xa0]&15u)==5);
        CHECK(!ops->sync(ops->ctx) && ops->cpu_ready(ops->ctx));
        CHECK(!ops->cursor_set(ops->ctx,&c) && !(xr_regs[0xa0]&7u));
        CHECK(hiqv_cursor_check(ops,g_ticks,0,0)==-ETIMEDOUT && !(xr_regs[0xa0]&7u));
        hiqv_quarantine("test"); CHECK(!(xr_regs[0xa0]&7u) && !engine.cpu_safe);
        CHECK(hiqv_fallback()==ops && engine.cpu_safe && !engine.active);
        CHECK(!ops->stop(ops->ctx));
    }
}
static void refusal_tests(void)
{
    setup(16); unsigned previous=ports;
    CHECK(hiqv_probe(&pci[0]) && hiqv_eligible(&fb) && ports==previous && !gos);
    for (unsigned id=0;id<3;id++) {
        pci[0].device=(uint16_t[]){0xe0,0xe4,0xe5}[id]; CHECK(hiqv_probe(&pci[0]));
    }
    for (unsigned id=0;id<3;id++) { pci[0].device=(uint16_t[]){0xc0,0xc4,0xc5}[id]; CHECK(!hiqv_probe(&pci[0])); }
    pci[0].device=0xd8; CHECK(!hiqv_probe(&pci[0])); pci[0].device=0xe5;
    functions=2; CHECK(!display_driver_bind(&fb)); functions=1; CHECK(display_driver_bind(&fb));
    resources[0].end--; CHECK(!hiqv_eligible(&fb)); resources[0].end++;
    g_boot.flags=CBI_F_SAFE_MODE; CHECK(!hiqv_eligible(&fb)); g_boot.flags=0;
    strcpy(g_boot.options,"accel=0"); CHECK(!hiqv_eligible(&fb)); g_boot.options[0]=0;
    pci[0].command=2; CHECK(!hiqv_probe(&pci[0])); pci[0].command=3;
    fb.phys+=PAGE_SIZE; CHECK(!hiqv_eligible(&fb)); fb.phys-=PAGE_SIZE;
    g_boot.e820_count=1; g_boot.e820[0].type=CBI_E820_RAM; g_boot.e820[0].base=fb.phys+HIQV_MMIO_OFFSET;
    g_boot.e820[0].length=PAGE_SIZE; CHECK(!hiqv_eligible(&fb)); g_boot.e820_count=0;
    fb.bpp=32; CHECK(!hiqv_eligible(&fb)); fb.bpp=16;
    fb.pitch=8192; CHECK(!hiqv_mode_supported(&fb)); fb.pitch=144;
    CHECK(!hiqv_start(&fb,0) && !claim_calls && ports==previous);
    const struct accel_ops *ops=activate(); trace_count=0; unsigned reads=ports;
    struct fb_rect bad={INT32_MAX,0,4,4}, r={0,0,4,4};
    CHECK(ops->fill_rect(ops->ctx,&bad,0)==-EOPNOTSUPP);
    CHECK(ops->copy_rect(ops->ctx,&r,INT32_MAX,0)==-EOPNOTSUPP);
    CHECK(ops->write_rect(ops->ctx,&r,&r,video,16)==-EOPNOTSUPP);
    CHECK(!trace_count && ports==reads);
    lease_live=false; CHECK(ops->fill_rect(ops->ctx,&r,0)==-EIO && !trace_count && engine.quarantined && ports==reads);
}
static void faults_and_markers(void)
{
    setup(16); marker_size=8; CHECK(hiqv_attempt_begin(&vfs)==-EIO && marker_size==8 && !claim_calls);
    CHECK(!strcmp(decline_reason,"previous_attempt"));
    setup(16); stat_error=-FS_EIO; CHECK(hiqv_attempt_begin(&vfs)<0 && !claim_calls);
    setup(16); commit_error=-FS_EIO; CHECK(hiqv_attempt_begin(&vfs)<0 && marker_size && !gos);
    setup(16); close_error=-FS_EIO; CHECK(hiqv_attempt_begin(&vfs)<0 && marker_size);
    setup(16); marker_short=true; CHECK(hiqv_attempt_begin(&vfs)<0 && marker_size);
    for (int fail=1;fail<=2;fail++) {
        setup(16); CHECK(!hiqv_attempt_begin(&vfs)); claim_fail_at=fail;
        CHECK(!hiqv_start(&fb,0) && !gos && port_claim<0 && reg_claim<0 && !registers);
    }
    setup(16); CHECK(!hiqv_attempt_begin(&vfs)); map_fail=1;
    CHECK(!hiqv_start(&fb,0) && port_claim<0 && reg_claim<0 && !gos);
    setup(16); CHECK(!hiqv_attempt_begin(&vfs)); xr_regs[2]=0xff;
    CHECK(!hiqv_start(&fb,0) && !gos && !registers);
    setup(16); CHECK(!hiqv_attempt_begin(&vfs)); corrupt_draw=true;
    CHECK(!hiqv_start(&fb,0) && engine.quarantined && engine.cpu_safe && marker_size);
    CHECK(!memcmp(video,saved,fb.size)); CHECK(hiqv_attempt_end(&vfs)<0 && marker_size);
    setup(16); const struct accel_ops *ops=activate();
    busy=true; reset_works=false; frozen=true; engine.ops.set_pause(&engine,pause_test,0);
    unsigned previous=gos, initial_reads=busy_reads; CHECK(ops->sync(ops->ctx)<0);
    CHECK(resets==1 && busy_reads-initial_reads<3*HIQV_WAIT_POLLS+10 && pause_calls && engine.quarantined && !engine.cpu_safe);
    CHECK(gos==previous && ops->capabilities(ops->ctx)==0);
    CHECK(hiqv_fallback()==ops && !engine.cpu_safe && !hiqv_eligible(&fb));
    unsigned polls=busy_reads; CHECK(ops->sync(ops->ctx)<0 && busy_reads==polls);
    CHECK(ops->stop(ops->ctx)<0 && registers && reg_claim>=0);
    setup(16); ops=activate(); busy=true; previous=gos;
    CHECK(ops->sync(ops->ctx)<0 && resets==1 && engine.cpu_safe && engine.quarantined && gos==previous);
    CHECK(!ops->stop(ops->ctx) && !registers);
    setup(16); ops=activate(); dead=true; CHECK(ops->sync(ops->ctx)<0 && !engine.cpu_safe && !resets);
    setup(16); ops=activate(); CHECK(!ops->cursor_show(ops->ctx,true)); ignore_hide=true;
    hiqv_quarantine("cursor_wedged"); CHECK(hiqv_fallback()==ops && !engine.cpu_safe && registers);
    setup(16); CHECK(!hiqv_attempt_begin(&vfs)); ignore_depth=true;
    CHECK(!hiqv_start(&fb,0) && engine.quarantined && marker_size);
    setup(16); ops=activate(); stall_go=true; previous=gos;
    struct fb_rect source={0,0,12,12};
    CHECK(ops->copy_rect(ops->ctx,&source,4,4)==-EAGAIN && gos==previous+1 && engine.quarantined && engine.cpu_safe);
    setup(16); ops=activate(); stall_go=true; previous=gos;
    CHECK(ops->fill_rect(ops->ctx,&source,0)==-ETIMEDOUT && gos==previous+1 && engine.quarantined && engine.cpu_safe);
    setup(16); ops=activate(); busy=true; engine.ops.set_pause(&engine,pause_test,0); pause_error=-EOPNOTSUPP;
    CHECK(ops->sync(ops->ctx)==-EIO && engine.quarantined);
    setup(16); CHECK(!hiqv_attempt_begin(&vfs)); ops=hiqv_start(&fb,0); CHECK(ops);
    commit_error=-FS_EIO; CHECK(hiqv_attempt_end(&vfs)<0);
    CHECK(!ops->stop(ops->ctx)); hiqv_quarantine("marker_clear_failed");
    CHECK(!hiqv_fallback() && engine.cpu_safe && !registers && attempt_blocked);
}
static void optional_and_identity(void)
{
    for (unsigned mode=0;mode<3;mode++) {
        setup(24);
        if (!mode) fr_regs[0x1a]=1;
        else if (mode==1) g_boot.display.vram_bytes=fb.size;
        else claim_fail_at=3;
        const struct accel_ops *ops=activate();
        CHECK(!scratch && ops->capabilities(ops->ctx)==1 && !ops->pattern_rect && !ops->write_rect && !ops->cursor_set);
        CHECK(!ops->stop(ops->ctx));
    }
    setup(16); const struct accel_ops *ops=activate();
    struct ciuki_display_engine_info info;
    CHECK(!display_driver_refresh()); display_driver_info(&info);
    CHECK(!strcmp(info.driver_id,"chipshiqv") && !strcmp(info.display_name,"Chips & Technologies HiQV"));
    CHECK(info.ready && !info.native_used && info.kind==CIUKI_ENGINE_NATIVE);
    display_driver_count(CIUKI_ENGINE_FILL,true); display_driver_info(&info); CHECK(info.native_used);
    hiqv_quarantine("test"); CHECK(display_driver_refresh());
    CHECK(hiqv_fallback()==ops); display_driver_info(&info);
    CHECK(info.quarantined && !info.ready && info.cpu_ready && !strcmp(info.reason,"test"));
    CHECK(!ops->stop(ops->ctx));
    /* Marker clear failure after a safe stop must still admit software. */
    hiqv_quarantine("marker_clear_failed"); CHECK(!hiqv_fallback() && engine.cpu_safe);
}
/* Registry query references generic fbdev APIs; no presenter bypass is used. */
unsigned fbdev_native_capabilities(void) { return engine.ops.capabilities ? engine.ops.capabilities(&engine) : 0; }
bool fbdev_cpu_ready(void) { return engine.cpu_safe; }
int main(void)
{
    drawing_tests(16); drawing_tests(24); cursor_tests(); refusal_tests();
    faults_and_markers(); optional_and_identity();
    puts("chipshiqv: register sequences, software goldens, ownership, markers, cursor, quarantine OK");
    return 0;
}
