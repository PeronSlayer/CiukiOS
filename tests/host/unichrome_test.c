/* Production port and registry over a fake MMIO/PCI/resource/VFS boundary.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/swdib.h>
#define UC_HOST_TEST
#include "../../src/kernel/drivers/video/unichrome/driver.c"
#include "../../src/kernel/drivers/video/registry.c"
const struct display_driver s3savage_driver={0};

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c); exit(1); } } while (0)
static uint8_t video[64*64*4], scratch_bytes[UC_SCRATCH_BYTES], mmio[UC_MMIO_BYTES];
static uint32_t regs[UC_MMIO_BYTES/4];
static struct { unsigned reg; uint32_t value; } trace[8192];
static unsigned writes, commands, status_reads, drains, pauses, probes, logs;
static unsigned indexed_writes;
static bool unlock_failure, enable_failure, restore_failure;
static unsigned pci_functions, resource_count, releases, revokes, maps;
static struct resource resources[16];
static struct pci_func functions[4];
static struct fb_device framebuffer;
static uint8_t cr[256], sr[256];
static bool stalled, stall_launch, dead, dead_mode, corrupt, hide_failure, invalid_lease;
static bool queue_empty;
static bool claim_failure, map_failure, pause_cancel, expire_on_pause;
static uint64_t test_now, time_step;
static uint32_t nb_size;
struct ciuki_boot_info g_boot;
volatile uint64_t g_ticks;
bool g_cpu_tsc;
uint64_t g_tsc_per_ms;

static void execute(uint32_t cmd)
{
    CHECK((cmd&0x00ffffffu)==(UC_FILL&0x00ffffffu) ||
          ((cmd&~(UC_DECX|UC_DECY))==UC_COPY));
    unsigned b=framebuffer.bpp/8;
    unsigned sx=regs[UC_SRC/4]&0xfffu, sy=(regs[UC_SRC/4]>>16)&0xfffu;
    unsigned dx=regs[UC_DST/4]&0xfffu, dy=(regs[UC_DST/4]>>16)&0xfffu;
    unsigned width=(regs[UC_DIM/4]&0xfffu)+1, height=(regs[UC_DIM/4]>>16)+1;
    unsigned dst_pitch=((regs[UC_PITCH/4]>>16)&0x7ffu)*8;
    unsigned src_pitch=(regs[UC_PITCH/4]&0x7ffu)*8;
    bool fill_cmd=(cmd>>24)==0xf0;
    CHECK(dst_pitch==framebuffer.pitch && regs[UC_DSTBASE/4]==0);
    CHECK(drains>0);
    if (corrupt) return;
    for (unsigned y=0;y<height;y++) for (unsigned x=0;x<width;x++) {
        int xx=(cmd&UC_DECX) ? -(int)x : (int)x, yy=(cmd&UC_DECY) ? -(int)y : (int)y;
        size_t dst=(size_t)((int)dy+yy)*dst_pitch+(unsigned)((int)dx+xx)*b;
        CHECK(dst+b<=framebuffer.size);
        if (fill_cmd) {
            uint32_t c=regs[UC_FG/4];
            memcpy(video+dst,&c,b);
        } else {
            const uint8_t *src;
            size_t off=(size_t)((int)sy+yy)*src_pitch+(unsigned)((int)sx+xx)*b;
            if (regs[UC_SRCBASE/4]) {
                CHECK(regs[UC_SRCBASE/4]*8==engine.scratch_offset);
                CHECK(off+b<=UC_SCRATCH_BYTES); src=scratch_bytes+off;
            } else { CHECK(off+b<=framebuffer.size); src=video+off; }
            memcpy(video+dst,src,b);
        }
    }
}
uint32_t uc_test_read(unsigned r)
{
    CHECK(r<UC_MMIO_BYTES && !(r&3u));
    if (r==UC_STATUS) { status_reads++; return dead ? UINT32_MAX : (sr[0x1a]&0x68u)!=0x68u ? 0 :
        (stalled ? UC_BUSY : 0) | (queue_empty ? UC_QUEUE_EMPTY : 0); }
    if (r==UC_MODE && dead_mode) return UINT32_MAX;
    return regs[r/4];
}
void uc_test_write(unsigned r, uint32_t v)
{
    CHECK(writes<ARRAY_SIZE(trace) && r<UC_MMIO_BYTES && !(r&3u));
    trace[writes].reg=r; trace[writes++].value=v;
    if (r==UC_CURSOR && hide_failure) return;
    regs[r/4]=v;
    if (r==UC_CMD) { commands++; execute(v); if (stall_launch) stalled=true; }
}
uint8_t uc_test_indexed(bool seq, uint8_t r) { return seq ? sr[r] : cr[r]; }
void uc_test_indexed_write(bool seq, uint8_t r, uint8_t v)
{
    CHECK(seq && (r==0x10 || r==0x1a)); indexed_writes++;
    if ((r==0x10 && unlock_failure) || (r==0x1a && enable_failure) ||
        (r==0x1a && restore_failure && v==engine.saved_sr1a)) return;
    sr[r]=v;
}
uint32_t uc_test_cfg(const struct pci_func *p, unsigned r)
{
    CHECK(p==&functions[1] && r==(functions[0].device==0x3122 || functions[0].device==0x7205 ? 0xe0u : 0xa0u));
    return nb_size;
}
unsigned pci_count(void) { return pci_functions; }
const struct pci_func *pci_get(unsigned i) { return i<pci_functions ? &functions[i] : 0; }
bool registry_valid(int h,gen_t g)
{
    return !invalid_lease && h>=0 && (unsigned)h<resource_count && resources[h].generation==g &&
           (resources[h].state==RS_ACTIVE || resources[h].state==RS_CLAIMED);
}
const struct resource *registry_get(unsigned i) { return i<resource_count ? &resources[i] : 0; }
int registry_claim(enum res_type t,uint32_t a,uint32_t b,const char *name,bool share)
{
    CHECK(t==RES_MMIO && a<b && !share);
    if (claim_failure) return -EBUSY;
    for (unsigned i=0;i<resource_count;i++)
        if (resources[i].state!=RS_RELEASED && a<resources[i].end && b>resources[i].start) return -EBUSY;
    CHECK(resource_count<ARRAY_SIZE(resources));
    unsigned h=resource_count++;
    resources[h]=(struct resource){t,a,b,name,100+h,RS_CLAIMED,false};
    return (int)h;
}
int registry_activate(int h,gen_t g)
{ CHECK(registry_valid(h,g)); resources[h].state=RS_ACTIVE; return 0; }
int registry_release(int h,gen_t g)
{ CHECK(registry_valid(h,g)); releases++; resources[h].state=RS_RELEASED; return 0; }
void *vmm_reserve_mmio(uint32_t n) { CHECK(n==UC_MMIO_BYTES || n==UC_SCRATCH_BYTES); return n==UC_MMIO_BYTES ? mmio : scratch_bytes; }
void *vmm_replace_mmio(void *window,uint32_t capacity,uint32_t phys,uint32_t n)
{
    CHECK(capacity==n && window && phys>=0xe0000000u); maps++;
    return map_failure ? 0 : n==UC_MMIO_BYTES ? mmio : scratch_bytes;
}
void vmm_revoke_mmio(void *p,uint32_t n) { CHECK(p && n); revokes++; }
const struct fb_device *fbdev_get(void) { return &framebuffer; }
bool fbdev_cpu_ready(void) { return engine.cpu_safe; }
unsigned fbdev_native_capabilities(void) { return engine.ops.capabilities ? engine.ops.capabilities(&engine) : 0; }
int fbdev_accel_probe(const struct fb_device *fb,int (*fn)(void *),void *ctx)
{ CHECK(fb==&framebuffer && registers && !commands); probes++; return fn(ctx); }
void framebuffer_write_drain(void) { drains++; }
uint64_t ktime_cycles(void) { return test_now++; }
void klog(const char *fmt,...) { CHECK(fmt); logs++; }

static struct fat_volume volume;
static struct vfs vfs;
static unsigned marker_size, legacy_size, file_writes, commits, closes, truncates;
static bool stat_failure, open_failure, short_write, commit_failure, close_failure;
static char marker_content[96];
int vfs_table_init(struct vfs *v,struct vfs_table *t,unsigned drive)
{ CHECK(v==&vfs && t==&marker_table && drive==2); return 0; }
void vfs_table_destroy(struct vfs_table *t) { CHECK(t==&marker_table); }
int vfs_stat(struct vfs_table *t,const char *path,struct fat_entry *out)
{
    CHECK(t==&marker_table);
    if (stat_failure) return -FS_EIO;
    memset(out,0,sizeof(*out));
    if (!strcmp(path,LEGACY_PATH)) out->size=legacy_size;
    else { CHECK(!strcmp(path,MARKER_PATH)); out->size=marker_size; }
    return out->size ? 0 : -FS_ENOENT;
}
int vfs_open(struct vfs_table *t,const char *path,unsigned flags,enum vfs_share share,uint8_t attr)
{ CHECK(t==&marker_table && !strcmp(path,MARKER_PATH) && flags==(VFS_WRITE|VFS_CREATE) && share==VFS_DENY_ALL && !attr); return open_failure ? -FS_EIO : 3; }
int vfs_write(struct vfs_table *t,int fd,const void *data,size_t size,size_t *done)
{
    CHECK(t==&marker_table && fd==3 && size<sizeof(marker_content));
    memcpy(marker_content,data,size); marker_content[size]=0; marker_size=(unsigned)size;
    file_writes++; *done=short_write ? size-1 : size; return 0;
}
int vfs_truncate(struct vfs_table *t,int fd,uint64_t size)
{ CHECK(t==&marker_table && fd==3 && !size); truncates++; marker_size=0; return 0; }
int vfs_commit(struct vfs_table *t,int fd)
{ CHECK(t==&marker_table && fd==3); commits++; return commit_failure ? -FS_EIO : 0; }
int vfs_close(struct vfs_table *t,int fd)
{ CHECK(t==&marker_table && fd==3); closes++; return close_failure ? -FS_EIO : 0; }

static uint32_t direct_read(void *ctx,unsigned r) { (void)ctx; return uc_test_read(r); }
static void direct_write(void *ctx,unsigned r,uint32_t v) { (void)ctx; uc_test_write(r,v); }
static uint8_t direct_indexed(void *ctx,bool seq,uint8_t r) { (void)ctx; return uc_test_indexed(seq,r); }
static uint64_t direct_now(void *ctx) { (void)ctx; test_now+=time_step; return test_now; }
static void direct_drain(void *ctx) { (void)ctx; drains++; }
static bool direct_valid(void *ctx) { (void)ctx; return !invalid_lease; }
static void direct_indexed_write(void *ctx,bool seq,uint8_t r,uint8_t v)
{ (void)ctx; uc_test_indexed_write(seq,r,v); }
static const struct uc_io direct_io={0,direct_read,direct_write,direct_indexed,direct_now,direct_drain,direct_valid,direct_indexed_write};
static int pause_test(void *ctx)
{
    CHECK(ctx==&pauses); pauses++;
    if (expire_on_pause) invalid_lease=true;
    return pause_cancel ? -EINTR : 0;
}
static void reset(unsigned bpp)
{
    memset(&engine,0,sizeof(engine)); memset(video,0,sizeof(video)); memset(regs,0,sizeof(regs));
    memset(&g_boot,0,sizeof(g_boot)); memset(functions,0,sizeof(functions));
    memset(resources,0,sizeof(resources)); memset(cr,0,sizeof(cr)); memset(sr,0,sizeof(sr));
    writes=commands=status_reads=drains=pauses=probes=logs=0;
    indexed_writes=0; unlock_failure=enable_failure=restore_failure=false;
    releases=revokes=maps=0; pci_functions=2; resource_count=1;
    stalled=stall_launch=dead=dead_mode=corrupt=hide_failure=invalid_lease=false;
    queue_empty=true;
    claim_failure=map_failure=pause_cancel=expire_on_pause=false;
    test_now=0; time_step=1; g_ticks=0; g_cpu_tsc=false; g_tsc_per_ms=0;
    registers=0; register_window=scratch_window=0; scratch_mapping=0;
    register_claim=scratch_claim=-1; engine_stop=0;
    blocked=marker_persisted=marker_retained=false; decision=software_reason="pending"; bound=0; owner=0;
    memset(native_count,0,sizeof(native_count)); memset(software_count,0,sizeof(software_count));
    marker_size=legacy_size=file_writes=commits=closes=truncates=0;
    stat_failure=open_failure=short_write=commit_failure=close_failure=false;
    memset(&volume,0,sizeof(volume)); volume.mounted=true; memset(&vfs,0,sizeof(vfs)); vfs.volumes[2]=&volume;
    nb_size=0x5000;
    framebuffer=(struct fb_device){.width=64,.height=64,.pitch=64*(bpp/8),.size=64*64*(bpp/8),
        .phys=0xe0000000,.mapped=video,.bpp=(uint8_t)bpp,.present=true,.handle=0,.generation=7,
        .red_size=bpp==16 ? 5 : 8,.red_pos=bpp==16 ? 11 : 16,
        .green_size=bpp==16 ? 6 : 8,.green_pos=bpp==16 ? 5 : 8,.blue_size=bpp==16 ? 5 : 8};
    resources[0]=(struct resource){RES_MMIO,framebuffer.phys,framebuffer.phys+framebuffer.size,"fbdev",7,RS_ACTIVE,false};
    functions[0]=(struct pci_func){.vendor=0x1106,.device=0x3122,.class_code=3,.command=2,
        .bar={0xe0000008,0xe4000000}};
    functions[1]=(struct pci_func){.vendor=0x1106,.device=0x3123,.class_code=6,.fn=3};
    g_boot.display.vram_bytes=32u<<20;
    g_boot.kernel_start=0x100000; g_boot.kernel_end=0x200000;
    unsigned pitch=framebuffer.pitch/8;
    cr[0x13]=(uint8_t)pitch; cr[0x35]=(uint8_t)((pitch>>3)&0xe0);
    sr[0x15]=bpp==16 ? 0x14 : 0x0c;
}
static void init_direct(unsigned bpp,bool upload)
{
    reset(bpp);
    CHECK(!uc_engine_init(&engine,&framebuffer,&direct_io,upload ? scratch_bytes : 0,framebuffer.size));
    CHECK(engine.active && engine.cpu_safe && engine.ops.capabilities(&engine)==1);
    CHECK(!commands && writes==22); /* six queue-disable + sixteen context writes */
    CHECK(indexed_writes==2 && sr[0x10]==1 && sr[0x1a]==0x68);
    CHECK(trace[0].reg==UC_TRANSET && trace[0].value==0x00fe0000);
    const uint32_t sequence[]={4,0x40008c0f,0x44000000,0x45080c04,0x46800408};
    for (unsigned i=0;i<5;i++) CHECK(trace[i+1].reg==UC_TRANSPACE && trace[i+1].value==sequence[i]);
}
static void compare_pixels(const uint8_t *reference) { CHECK(!memcmp(video,reference,framebuffer.size)); }
static void check_trace(unsigned start,const unsigned *registers_expected,const uint32_t *values,unsigned count)
{
    CHECK(writes==start+count);
    for (unsigned i=0;i<count;i++) {
        CHECK(trace[start+i].reg==registers_expected[i]);
        CHECK(trace[start+i].value==values[i]);
    }
}
static void test_commands(void)
{
    for (unsigned bpp=16;bpp<=32;bpp+=16) {
        init_direct(bpp,true);
        uint8_t reference[sizeof(video)], saved[sizeof(video)];
        struct fb_device ref=framebuffer; ref.mapped=reference;
        for (unsigned i=0;i<framebuffer.size;i++) video[i]=(uint8_t)(i*19u);
        memcpy(reference,video,framebuffer.size); memcpy(saved,video,framebuffer.size);
        struct fb_rect r={3,5,17,13};
        unsigned old=writes;
        swdib_fill(&ref,&r,0x123456); CHECK(!engine.ops.fill_rect(&engine,&r,0x123456)); compare_pixels(reference);
        CHECK(writes==old+7 && trace[old].reg==UC_MODE && trace[old+6].reg==UC_CMD);
        const unsigned fill_regs[]={UC_MODE,UC_DST,UC_DIM,UC_FG,UC_DSTBASE,UC_PITCH,UC_CMD};
        const uint32_t fill_values[]={bpp==16 ? 0x100u : 0x300u,5u<<16|3,12u<<16|16,0x123456,0,
            0x80000000u|framebuffer.pitch<<13,UC_FILL};
        check_trace(old,fill_regs,fill_values,ARRAY_SIZE(fill_regs));
        CHECK(regs[UC_CMD/4]==UC_FILL && regs[UC_DIM/4]==(12u<<16|16));
        CHECK(regs[UC_PITCH/4]==(0x80000000u|framebuffer.pitch<<13));
        /* Horizontal, vertical and diagonal overlap in all directions. */
        const int destinations[][2]={{8,5},{1,5},{3,8},{3,1},{8,8},{1,1},{8,1},{1,8}};
        for (unsigned n=0;n<ARRAY_SIZE(destinations);n++) {
            memcpy(video,saved,framebuffer.size); memcpy(reference,saved,framebuffer.size);
            int x=destinations[n][0],y=destinations[n][1];
            old=writes;
            swdib_copy(&ref,&r,x,y); CHECK(!engine.ops.copy_rect(&engine,&r,x,y)); compare_pixels(reference);
            unsigned cmd=UC_COPY | (r.x<x ? UC_DECX : 0) | (r.y<y ? UC_DECY : 0);
            CHECK(regs[UC_CMD/4]==cmd);
            CHECK(regs[UC_SRC/4]==((unsigned)(r.x+(r.x<x ? r.width-1 : 0)) |
                  (unsigned)(r.y+(r.y<y ? r.height-1 : 0))<<16));
            CHECK(regs[UC_SRCBASE/4]==0 && regs[UC_DSTBASE/4]==0);
            const unsigned copy_regs[]={UC_MODE,UC_SRC,UC_DST,UC_DIM,UC_SRCBASE,UC_DSTBASE,UC_PITCH,UC_CMD};
            const uint32_t copy_values[]={bpp==16 ? 0x100u : 0x300u,
                (unsigned)(r.x+(r.x<x ? r.width-1 : 0)) | (unsigned)(r.y+(r.y<y ? r.height-1 : 0))<<16,
                (unsigned)(x+(r.x<x ? r.width-1 : 0)) | (unsigned)(y+(r.y<y ? r.height-1 : 0))<<16,
                12u<<16|16,0,0,0x80000000u|framebuffer.pitch<<13|framebuffer.pitch>>3,cmd};
            check_trace(old,copy_regs,copy_values,ARRAY_SIZE(copy_regs));
        }
        CHECK(!engine.ops.sync(&engine)); CHECK(engine.operations==9);
        unsigned before=writes;
        struct fb_rect invalid={-1,0,8,8}, empty={0,0,0,0};
        CHECK(engine.ops.fill_rect(&engine,&invalid,0)==-EOPNOTSUPP);
        CHECK(engine.ops.copy_rect(&engine,&r,60,60)==-EOPNOTSUPP);
        CHECK(!engine.ops.fill_rect(&engine,&empty,0)); CHECK(writes==before);
        CHECK(!uc_engine_probe(&engine)); compare_pixels(reference);
        CHECK(engine.verified_write && !engine.ops.verify_write(&engine));
        uint8_t rows[4*17*4]; for (unsigned i=0;i<sizeof(rows);i++) rows[i]=(uint8_t)(i*31u);
        struct fb_rect dst={2,4,17,4}, clip={4,5,11,2};
        swdib_write(&ref,&dst,&clip,rows,17*(bpp/8));
        CHECK(!engine.ops.write_rect(&engine,&dst,&clip,rows,17*(bpp/8))); compare_pixels(reference);
        CHECK(regs[UC_SRCBASE/4]==engine.scratch_offset/8 && !(regs[UC_CMD/4]&0xc0u));
        before=writes;
        CHECK(engine.ops.write_rect(&engine,&dst,&clip,rows,1)==-EOPNOTSUPP);
        CHECK(engine.ops.write_rect(&engine,&dst,&clip,video,17*4)==-EOPNOTSUPP);
        CHECK(engine.ops.write_rect(&engine,&dst,&clip,scratch_bytes,17*4)==-EOPNOTSUPP);
        CHECK(engine.ops.write_rect(&engine,&dst,&clip,(const void *)(UINTPTR_MAX-2),17*4)==-EOPNOTSUPP);
        struct accel_cursor shape={0};
        CHECK(engine.ops.cursor_set(&engine,&shape)==-EOPNOTSUPP);
        CHECK(engine.ops.cursor_move(&engine,INT32_MIN,INT32_MAX)==-EOPNOTSUPP);
        CHECK(engine.ops.cursor_show(&engine,true)==-EOPNOTSUPP && writes==before);
        CHECK(!(engine.ops.capabilities(&engine)&2u));
        CHECK(!engine.ops.stop(&engine) && !engine.active && engine.cpu_safe);
        CHECK(!sr[0x10] && !sr[0x1a] && !engine.transport_saved);
    }
}
static void test_faults(void)
{
    init_direct(32,false);
    struct fb_rect r={0,0,4,4};
    engine.ops.set_pause(&engine,pause_test,&pauses);
    stall_launch=true;
    CHECK(engine.ops.fill_rect(&engine,&r,0xabcdef)==-ETIMEDOUT);
    CHECK(commands==1 && pauses && engine.quarantined && !engine.cpu_safe && !engine.active);
    CHECK(!engine.ops.capabilities(&engine));
    unsigned before=writes;
    CHECK(engine.ops.copy_rect(&engine,&r,1,1)==-EIO && writes==before);
    CHECK(engine.ops.stop(&engine)==-ETIMEDOUT && engine.saved_valid);
    stalled=false; CHECK(!engine.ops.sync(&engine) && engine.cpu_safe && engine.quarantined);
    CHECK(!engine.ops.stop(&engine) && !engine.saved_valid);
    CHECK(uc_engine_init(&engine,&framebuffer,&direct_io,0,0)==-EIO);
    init_direct(16,false); stalled=true; time_step=0;
    CHECK(engine.ops.sync(&engine)==-ETIMEDOUT && status_reads>=UC_POLL_LIMIT);
    init_direct(32,false); stalled=true; time_step=10;
    engine.ops.set_pause(&engine,pause_test,&pauses); pause_cancel=true;
    CHECK(engine.ops.sync(&engine)==-EINTR && engine.quarantined && !engine.cpu_safe);
    init_direct(32,false); stalled=true;
    engine.ops.set_pause(&engine,pause_test,&pauses); expire_on_pause=true;
    CHECK(engine.ops.sync(&engine)==-EIO && engine.quarantined && !engine.cpu_safe);
    init_direct(32,false); dead=true;
    CHECK(engine.ops.sync(&engine)==-EIO && !engine.cpu_safe && engine.quarantined);
    init_direct(32,false); queue_empty=false;
    CHECK(engine.ops.sync(&engine)==-ETIMEDOUT && !engine.cpu_safe && engine.quarantined);
    init_direct(32,false); dead_mode=true;
    CHECK(engine.ops.fill_rect(&engine,&r,0)==-EIO && !commands && !engine.cpu_safe);
    init_direct(32,false); regs[UC_CURSOR/4]=1; hide_failure=true;
    uc_engine_quarantine(&engine,"cursor_stuck");
    CHECK(!engine.cpu_safe && engine.quarantined && engine.ops.sync(&engine)==-EIO);
    init_direct(32,false); invalid_lease=true;
    before=writes;
    unsigned reads=status_reads;
    CHECK(engine.ops.fill_rect(&engine,&r,0)==-EIO && !commands && !engine.cpu_safe);
    CHECK(writes==before && status_reads==reads);
    init_direct(32,false); corrupt=true;
    CHECK(uc_engine_probe(&engine)==-EIO && engine.quarantined && engine.cpu_safe);
    reset(32); regs[UC_CURSOR/4]=1;
    CHECK(!uc_engine_init(&engine,&framebuffer,&direct_io,0,0));
    CHECK(!(regs[UC_CURSOR/4]&1u) && trace[0].reg==UC_CURSOR && engine.cpu_safe);
    regs[UC_CURSOR/4]=1; hide_failure=true;
    CHECK(engine.ops.stop(&engine)==-EIO && !engine.cpu_safe && engine.saved_valid);
    init_direct(32,false); restore_failure=true;
    CHECK(engine.ops.stop(&engine)==-EIO && !engine.cpu_safe && engine.quarantined && engine.transport_saved);
    reset(32); unlock_failure=true;
    CHECK(uc_engine_init(&engine,&framebuffer,&direct_io,0,0)==-EIO && !commands && engine.quarantined);
    reset(32); enable_failure=true;
    CHECK(uc_engine_init(&engine,&framebuffer,&direct_io,0,0)==-EIO && !commands && engine.quarantined);
    reset(32); sr[0x1a]=255;
    CHECK(uc_engine_init(&engine,&framebuffer,&direct_io,0,0)==-EIO && !commands && indexed_writes==1);
    CHECK(!engine.mmio_changed && !engine.ops.stop(&engine) && indexed_writes==2 && !sr[0x10]);
    reset(32); sr[0x10]=0xa0; sr[0x1a]=3;
    CHECK(!uc_engine_init(&engine,&framebuffer,&direct_io,0,0) && sr[0x10]==0xa1 && sr[0x1a]==0x6b);
    CHECK(!engine.ops.stop(&engine) && sr[0x10]==0xa0 && sr[0x1a]==3);
}
static void test_refusal_and_registry(void)
{
    const unsigned ids[]={0x3122,0x7205,0x3108,0x3118};
    for (unsigned i=0;i<ARRAY_SIZE(ids);i++) {
        reset(32); functions[0].device=(uint16_t)ids[i];
        CHECK(uc_pci_supported(&functions[0]));
        CHECK(display_driver_bind(&framebuffer)==&unichrome_driver && display_driver_owner()==&functions[0]);
        CHECK(!writes && !maps && !file_writes && !indexed_writes);
    }
    const unsigned excluded[][2]={{0x1023,0x8400},{0x1023,0x8500},{0x1106,0x1122},{0x1106,0x3344},{0x1106,0xffff}};
    for (unsigned i=0;i<ARRAY_SIZE(excluded);i++) {
        reset(32); functions[0].vendor=(uint16_t)excluded[i][0]; functions[0].device=(uint16_t)excluded[i][1];
        CHECK(!uc_pci_supported(&functions[0]) && !display_driver_bind(&framebuffer));
    }
    for (unsigned what=0;what<11;what++) {
        reset(32);
        if (what==0) functions[0].bar[0]+=0x1000;
        if (what==1) { functions[2]=functions[0]; pci_functions=3; }
        if (what==2) functions[0].command=0;
        if (what==3) resources[0].generation++;
        if (what==4) resources[0].owner="foreign";
        if (what==5) g_boot.display.vram_bytes=1;
        if (what==6) { g_boot.e820_count=1; g_boot.e820[0]=(struct ciuki_e820){.base=framebuffer.phys,.length=framebuffer.size,.type=CBI_E820_RAM}; }
        if (what==7) functions[0].bar[0]|=4;
        if (what==8) framebuffer.phys=0xfffff000;
        if (what==9) functions[0].header_type=1;
        if (what==10) framebuffer.size=0;
        CHECK(!display_driver_bind(&framebuffer) && !writes && !maps);
    }
    reset(24); CHECK(!eligible(&framebuffer) && !writes);
    reset(32); functions[0].bar[1]=0x100000; CHECK(!eligible(&framebuffer));
    reset(32); functions[0].bar[1]=0xe1000000; CHECK(!eligible(&framebuffer));
    reset(32); functions[0].bar[1]|=4; CHECK(!eligible(&framebuffer));
    reset(32); g_boot.flags=CBI_F_SAFE_MODE; CHECK(!eligible(&framebuffer) && attempt_begin(&vfs));
    reset(32); memcpy(g_boot.options,"accel=0",8); CHECK(!eligible(&framebuffer) && !maps && !file_writes);
    reset(32); cr[0x0c]=1; CHECK(uc_engine_init(&engine,&framebuffer,&direct_io,0,0)==-EOPNOTSUPP && !writes);
    reset(32); cr[0x6a]=0x80; CHECK(uc_engine_init(&engine,&framebuffer,&direct_io,0,0)==-EOPNOTSUPP && !writes);
    reset(32); cr[0x13]++; CHECK(uc_engine_init(&engine,&framebuffer,&direct_io,0,0)==-EOPNOTSUPP && !writes);
    reset(16); sr[0x15]=4; CHECK(uc_engine_init(&engine,&framebuffer,&direct_io,0,0)==-EOPNOTSUPP && !writes);
    reset(32); dead_mode=true; CHECK(uc_engine_init(&engine,&framebuffer,&direct_io,0,0)==-EIO && !writes);
    reset(32); stalled=true; CHECK(uc_engine_init(&engine,&framebuffer,&direct_io,0,0)==-ETIMEDOUT && !writes && !engine.cpu_safe);
    reset(32); queue_empty=false;
    CHECK(uc_engine_init(&engine,&framebuffer,&direct_io,0,0)==-ETIMEDOUT && !writes && !engine.cpu_safe);
    reset(32); framebuffer.pitch=0x4000;
    CHECK(uc_engine_init(&engine,&framebuffer,&direct_io,0,0)==-EOPNOTSUPP && !writes);
    reset(32); framebuffer.width=4097;
    CHECK(uc_engine_init(&engine,&framebuffer,&direct_io,0,0)==-EOPNOTSUPP && !writes);
    reset(32); framebuffer.generation++;
    CHECK(!owner_of(&framebuffer));
}
static void test_runtime(void)
{
    const uint16_t ids[]={0x3122,0x7205,0x3108,0x3118}, northbridges[]={0x3123,0x3205,0x3204,0x3259};
    for (unsigned i=0;i<ARRAY_SIZE(ids);i++) {
        reset(32); functions[0].device=ids[i]; functions[1].device=northbridges[i];
        CHECK(!attempt_begin(&vfs));
        const struct accel_ops *selected=start(&framebuffer,0);
        CHECK(selected && selected->write_rect && commands==12 && probes==1);
        CHECK(!attempt_end(&vfs) && !selected->stop(selected->ctx));
    }
    for (unsigned bpp=16;bpp<=32;bpp+=16) {
        reset(bpp); uint8_t saved[sizeof(video)]; memcpy(saved,video,sizeof(video));
        CHECK(display_driver_bind(&framebuffer)==&unichrome_driver);
        CHECK(!attempt_begin(&vfs) && file_writes==1 && commits==1 && closes==1);
        CHECK(strstr(marker_content,"driver=unichrome\npci=1106:3122\n"));
        const struct accel_ops *ops=start(&framebuffer,0);
        CHECK(ops && probes==1 && commands && engine.verified_write && scratch_mapping);
        CHECK(!memcmp(saved,video,sizeof(video)));
        CHECK(!attempt_end(&vfs) && truncates==1 && !marker_size);
        bool ready,used; const char *why; CHECK(summary(&ready,&used,&why) && ready);
        CHECK(!strcmp(unichrome_driver.name,"VIA UniChrome / UniChrome Pro"));
        struct ciuki_display_engine_info info; display_driver_refresh(); display_driver_info(&info);
        CHECK(!strcmp(info.driver_id,"unichrome") && !strcmp(info.display_name,unichrome_driver.name));
        CHECK(info.pci_vendor==0x1106 && info.pci_device==0x3122 && info.ready && !info.native_used);
        display_driver_count(CIUKI_ENGINE_FILL,true); display_driver_info(&info); CHECK(info.native_used);
        CHECK(!ops->stop(ops->ctx) && releases==2 && revokes==2 && !registers);
    }
    reset(32); CHECK(!start(&framebuffer,0) && !writes && !maps);
    reset(32); marker_persisted=true; claim_failure=true;
    CHECK(!start(&framebuffer,0) && !writes && !maps && !fallback());
    reset(32); marker_persisted=true; map_failure=true;
    CHECK(!start(&framebuffer,0) && releases==1 && !writes && !fallback());
    reset(32); marker_persisted=true; cr[0x0c]=1;
    CHECK(!start(&framebuffer,0) && !writes && releases==2 && !fallback());
    reset(32); marker_persisted=true; nb_size=0;
    const struct accel_ops *ops=start(&framebuffer,0);
    CHECK(ops && !ops->write_rect && !scratch_mapping && !ops->stop(ops->ctx));
    reset(32); marker_persisted=true; functions[1].device=0xffff;
    ops=start(&framebuffer,0); CHECK(ops && !ops->write_rect && !ops->stop(ops->ctx));
    reset(32); marker_persisted=true; nb_size=UINT32_MAX;
    ops=start(&framebuffer,0); CHECK(ops && !ops->write_rect && !ops->stop(ops->ctx));
    reset(32); marker_persisted=true; g_boot.display.vram_bytes=framebuffer.size;
    ops=start(&framebuffer,0); CHECK(ops && !ops->write_rect && !ops->stop(ops->ctx));
    reset(32); marker_persisted=true; g_boot.e820_count=1;
    g_boot.e820[0]=(struct ciuki_e820){.base=framebuffer.phys+framebuffer.size,.length=4096,.type=CBI_E820_RAM};
    ops=start(&framebuffer,0); CHECK(ops && !ops->write_rect && !ops->stop(ops->ctx));
    reset(32); CHECK(!attempt_begin(&vfs)); stall_launch=true;
    CHECK(!start(&framebuffer,0) && blocked && engine.quarantined && !engine.cpu_safe);
    CHECK(fallback()==&engine.ops && !engine.cpu_safe && !releases && !revokes && marker_size);
    CHECK(attempt_end(&vfs)==-EIO && !truncates);
    CHECK(!attempt_failed(&vfs) && marker_size);
    stalled=false; CHECK(fallback()==&engine.ops && engine.cpu_safe && engine.quarantined);
    CHECK(!engine.ops.stop(&engine) && releases==2);
    CHECK(!eligible(&framebuffer) && !start(&framebuffer,0));
    reset(32); CHECK(!attempt_begin(&vfs)); corrupt=true;
    CHECK(!start(&framebuffer,0) && blocked && engine.cpu_safe && engine.quarantined);
    CHECK(fallback() && !engine.ops.stop(&engine));
}
static void test_markers(void)
{
    for (unsigned what=0;what<8;what++) {
        reset(32);
        if (what==0) legacy_size=13;
        if (what==1) marker_size=27;
        if (what==2) stat_failure=true;
        if (what==3) open_failure=true;
        if (what==4) short_write=true;
        if (what==5) commit_failure=true;
        if (what==6) close_failure=true;
        if (what==7) volume.readonly=true;
        CHECK(attempt_begin(&vfs)<0 && blocked && !marker_persisted && !writes && !maps && !truncates);
        CHECK(!start(&framebuffer,0));
    }
    reset(32); CHECK(!attempt_begin(&vfs));
    CHECK(start(&framebuffer,0)); commit_failure=true;
    CHECK(attempt_end(&vfs)<0 && blocked); quarantine("marker_clear_failed");
    CHECK(engine.quarantined && engine.cpu_safe);
    reset(32); g_boot.flags=CBI_F_HW_CHECK; legacy_size=13;
    bool retained=false; CHECK(!check_begin(&vfs,&retained) && retained && !file_writes && !truncates);
    struct display_selftest_result result;
    CHECK(selftest(100,0,0,&result)==-EOPNOTSUPP && !result.passed && result.restored);
    CHECK(!start(&framebuffer,0) && attempt_end(&vfs)==-EIO && !truncates);
    reset(32); g_boot.flags=CBI_F_HW_CHECK;
    CHECK(!check_begin(&vfs,&retained) && !retained && marker_persisted);
    CHECK(!selftest(100,0,0,&result) && result.passed && !commands);
    CHECK(!attempt_end(&vfs));
    CHECK(selftest(0,0,0,&result)==-ETIMEDOUT);
}
int main(void)
{
    test_commands(); test_faults(); test_refusal_and_registry(); test_runtime(); test_markers();
    puts("unichrome: register traces, software goldens, ownership, markers, quarantine PASS");
    return 0;
}
