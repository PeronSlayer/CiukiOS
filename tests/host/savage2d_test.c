/* Production register driver + production framebuffer renderer, ASan/UBSan.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main framebuffer_regression_main
#define savage2d_init boot_init
#include "fbdev_test.c"
#undef savage2d_init
#undef main
#include <ciuki/savage2d.h>
#include <ciuki/cpu.h>
#include <time.h>
#include <ciuki/work.h>
static unsigned binding_claims, binding_pci_count;
static struct pci_func binding_gpu;
static bool binding_allowed;
static struct resource binding_resources[4];
static bool binding_port_conflict;
static int binding_claim(enum res_type t,uint32_t s,uint32_t e,const char *o,bool share)
{
    CHECK(!share && !strcmp(o,t==RES_PORT ? "savage-vga" : "savage2d"));
    if (t==RES_PORT) CHECK(s==0x3b0 && e==0x3e0);
    else CHECK(t==RES_MMIO && s==(binding_gpu.bar[0]&~15u) && e==s+SAVAGE_MMIO_BYTES);
    binding_claims++;
    if (!binding_allowed || (t==RES_PORT && binding_port_conflict)) return -EBUSY;
    unsigned h=t==RES_PORT ? 1 : 2;
    CHECK(binding_resources[h].state==RS_RELEASED || !binding_resources[h].owner);
    binding_resources[h]=(struct resource){t,s,e,o,43+h,RS_CLAIMED,false};
    return (int)h;
}
static const struct resource *binding_get(unsigned i)
{ return i==1 || i==2 ? &binding_resources[i] : registry_get(i); }
static int binding_activate(int h, gen_t gen)
{ CHECK((h==1 || h==2) && gen==43u+h && binding_resources[h].state==RS_CLAIMED); binding_resources[h].state=RS_ACTIVE; return 0; }
static int binding_quarantine(int h, gen_t gen)
{ CHECK((h==1 || h==2) && gen==43u+h); binding_resources[h].state=RS_QUARANTINED; return 0; }
static int binding_quiesce(int h, gen_t gen, registry_idle_proof_fn proof)
{
    CHECK((h==1 || h==2) && gen==43u+h && binding_resources[h].state==RS_ACTIVE);
    binding_resources[h].state=RS_QUIESCING;
    return proof(h,gen) ? 0 : -EIO;
}
static int binding_release(int h, uint32_t gen)
{ CHECK((h==1 || h==2) && gen==43u+h && binding_resources[h].state==RS_QUIESCING); binding_resources[h].state=RS_RELEASED; return 0; }
static uint8_t test_inb(uint16_t);
static void test_outb(uint16_t,uint8_t);
static void *binding_reserve(uint32_t capacity)
{ CHECK(capacity==SAVAGE_MMIO_BYTES); return (void *)(uintptr_t)0x1000; }
#define registry_claim binding_claim
#define registry_get binding_get
#define registry_activate binding_activate
#define registry_quarantine binding_quarantine
#define registry_quiesce binding_quiesce
#define registry_release binding_release
#define inb test_inb
#define outb test_outb
#define vmm_reserve_mmio binding_reserve
#include "../../src/kernel/fs/vfs.h"
static void *sweep_alloc(size_t n) { return calloc(1,n); }
static void sweep_free(void *p,size_t n) { (void)n; free(p); }
#define fs_contig_alloc sweep_alloc
#define fs_contig_free sweep_free
#include "../../src/kernel/drivers/savage2d.c"
#undef fs_contig_alloc
#undef fs_contig_free
#undef registry_claim
#undef registry_get
#undef registry_activate
#undef registry_quarantine
#undef registry_quiesce
#undef registry_release
#undef inb
#undef outb
#undef vmm_reserve_mmio

unsigned pci_count(void) { return binding_pci_count; }
const struct pci_func *pci_get(unsigned i) { return i<binding_pci_count ? &binding_gpu : NULL; }
void *vmm_map_mmio(uint32_t p, uint32_t n, bool uc) { (void)p; (void)n; (void)uc; return NULL; }
static uint8_t binding_window[SAVAGE_MMIO_BYTES];
void *vmm_replace_mmio(void *w,uint32_t c,uint32_t p,uint32_t n)
{ (void)w; CHECK(c==SAVAGE_MMIO_BYTES && n==c && p==(binding_gpu.bar[0]&~15u)); return binding_window; }
int udelay(uint32_t us) { CHECK(us<=500); return 0; }
static bool marker_thread_running;
static unsigned normal_yields, device_work_requests;
static void (*init_interleave)(void);
void task_yield(void)
{
    CHECK(marker_thread_running && !probing && !locks);
    normal_yields++;
    if (accel_initializing && init_interleave) init_interleave();
}
void kwork_yield(void) { CHECK(false); }
static unsigned boot_calls;
bool kwork_queue(kwork_fn fn, void *arg)
{ (void)fn; (void)arg; device_work_requests++; CHECK(false); return false; }
static void marker_thread_step(void)
{
    CHECK(!marker_thread_running && marker_worker.prio==P_NORMAL);
    marker_thread_running=true;
    accel_marker_run();
    marker_thread_running=false;
}

/* Durable settings boundary: distinguish cache bytes from committed bytes.
 * An interrupted attempt is tested by retaining only disk_marker. */
static struct vfs settings_vfs;
static struct fat_volume settings_volume;
static char disk_marker[64], cache_marker[64];
static unsigned disk_size, cache_size, marker_commits;
static bool marker_exists, table_open, marker_open, marker_error, clear_error;
static unsigned marker_fault;
static bool registry_setting_present;
static int32_t registry_setting;
bool kernel_registry_int(struct vfs *v,const char *key,const char *name,int32_t *out)
{
    CHECK(v==&settings_vfs && !strcmp(key,"System\\Display\\Savage") && !strcmp(name,"EngineConfig"));
    if (!registry_setting_present) return false;
    *out=registry_setting; return true;
}
static uint32_t marker_cost_us;
static void marker_delay(void);
int vfs_table_init(struct vfs *v, struct vfs_table *t, unsigned drive)
{ CHECK(v==&settings_vfs && drive==2 && !table_open); if (marker_fault==1) return -FS_ENOMEM; table_open=true; t->vfs=v; return 0; }
void vfs_table_destroy(struct vfs_table *t)
{ CHECK(t==&attempt_table && table_open && !marker_open); table_open=false; }
int vfs_stat(struct vfs_table *t, const char *path, struct fat_entry *e)
{ CHECK(t==&attempt_table && !strcmp(path,ATTEMPT_PATH)); if (marker_fault==2) return -FS_EIO; e->size=disk_size; return marker_exists ? 0 : -FS_ENOENT; }
int vfs_open(struct vfs_table *t,const char *path,unsigned flags,enum vfs_share deny,uint8_t attr)
{
    CHECK(t==&attempt_table && !strcmp(path,ATTEMPT_PATH) && flags==(VFS_WRITE|VFS_CREATE) && deny==VFS_DENY_ALL && !attr);
    if (marker_error) return -FS_EIO;
    marker_open=true; memcpy(cache_marker,disk_marker,sizeof(cache_marker)); cache_size=disk_size; return 0;
}
int vfs_write(struct vfs_table *t,int fd,const void *buf,size_t n,size_t *done)
{
    CHECK(t==&attempt_table && fd==0 && marker_open && !locks && !initializing);
    CHECK((n==14 || n==30) && !memcmp(buf,"accel=attempt\n",14));
    if (n==30) CHECK(!memcmp((const char *)buf+14,"status=",7) && ((const char *)buf)[29]=='\n');
    memcpy(cache_marker,buf,n); cache_size=n; *done=marker_fault==3 ? n-1 : n; return marker_fault==4 ? -FS_EIO : 0;
}
int vfs_truncate(struct vfs_table *t,int fd,uint64_t n)
{ CHECK(t==&attempt_table && fd==0 && marker_open && !n); cache_size=0; return 0; }
int vfs_commit(struct vfs_table *t,int fd)
{
    CHECK(t==&attempt_table && fd==0 && marker_open);
    marker_delay();
    if ((!cache_size && clear_error) || marker_fault==5) return -FS_EIO;
    memcpy(disk_marker,cache_marker,sizeof(disk_marker)); disk_size=cache_size; marker_exists=true; marker_commits++; return 0;
}
int vfs_close(struct vfs_table *t,int fd)
{ CHECK(t==&attempt_table && fd==0 && marker_open); marker_open=false; return marker_fault==6 ? -FS_EIO : 0; }

struct model {
    uint32_t reg[SAVAGE_MMIO_BYTES/4], packet[16];
    uint32_t bci_gbd[2], gbd_history[4][2];
    unsigned register_packets, fail_after_gbd, late_setup_fault;
    bool ignore_gbd, ignore_first_gbd, gbd_busy, require_gbd_enable;
    uint8_t cr[256], cursor_page[4096], colour[2][3];
    unsigned words, expected, packets, fault_count, busy, cr_writes, colour_at;
    uint8_t cr_sequence[128];
    bool stuck, frozen, bad_readback, fail_after_submit, partial, locked, dead, wrong_offset, delay_failed, bci_locked;
    uint8_t misc, seq[256], seq_index;
    uint8_t vga_enable, cr_index;
    bool alias_dead, mmio_enable_failed, locked39, decorated, reserved_readbacks, pipe_locked, alias_lock_mismatch;
    unsigned reset_fault, cr66_reads;
    unsigned port_reads, port_writes, alias_reads, alias_writes, engine_writes, reset_engine_writes;
    unsigned port_cr_reads[256];
    unsigned status_reads, delays, reset_phase;
    bool contradictory;
    bool control_reserved;
    uint64_t reset_set_us, reset_clear_us, last_status_us, first_status_us, idle_after_us;
    uint32_t status_value;
    uint64_t us;
    bool exact_clock, last_status_busy;
    bool strict_window, window_locked, postreset_enable, gbd_reserved, ignore_fill, depth_locked, corrupt_high, recover_on_final;
    unsigned half_reads, half_writes, dead_window_writes, dword_2d_reads;
    struct { uint32_t reg, value; } setup[64];
    unsigned setup_count;
};
static struct model m;
static struct savage2d test_engine;
uint64_t ktime_cycles(void) { return m.us * 1000; }
static void marker_delay(void) { CHECK(!locks && !initializing); m.us+=marker_cost_us; }
static void marker_before_register(void)
{
    if (initializing)
        CHECK(disk_size==14 && marker_commits==1 && !marker_open && !table_open && locks==(probing ? 1u : 0u));
}
static void execute_packet(struct model *p)
{
    /* Let setup read back correctly, then lose state at the first draw.
     * The failure snapshot must see current registers before teardown. */
    if (p->late_setup_fault==1) p->reg[0x816c/4]&=~8u;
    if (p->late_setup_fault==2) p->bci_gbd[1]&=~8u;
    if (p->late_setup_fault==3) {
        p->cr[0x50]&=~0x30u;
        p->reg[0x8128/4]=0xffu;
    }
    uint32_t cmd=p->packet[0], gbd=p->reg[0x816c/4];
    CHECK((cmd&0x1c00u)==0); /* destination is GBD, no inline descriptor */
    unsigned bpp=(gbd>>16)&255, stride=gbd&0x1ff0u;
    CHECK(bpp==8 || bpp==16 || bpp==32);
    CHECK(stride==device.pitch/(device.bpp/8) && !p->reg[0x8168/4]);
    unsigned cr_depth=p->cr[0x50]&0x30u;
    unsigned engine_bpp=cr_depth==0 ? 8 : cr_depth==0x10 ? 16 : 32;
    CHECK(cr_depth!=0x20 && (p->cr[0x50]&0xc1u)==0xc1u);
    if (bpp<engine_bpp) engine_bpp=bpp;
    /* MMIO and the command processor need not observe a depth change
     * together. Keep the captured stale-8-bpp scenario independent of the
     * readable register bank; SET_REGISTER updates the command state. */
    if (p->seq[0x26]==0x40) {
        unsigned queued_bpp=(p->bci_gbd[1]>>16)&255u;
        if (!queued_bpp) queued_bpp=8;
        CHECK(queued_bpp==8 || queued_bpp==16 || queued_bpp==32);
        if (queued_bpp<engine_bpp) engine_bpp=queued_bpp;
        /* Explicit f1-67 hypothesis: both descriptor transports must carry
         * bit 3. This models a discrepancy, not established silicon rules. */
        if (p->require_gbd_enable && (!(gbd&8u) || !(p->bci_gbd[1]&8u))) engine_bpp=8;
    }
    bool copying=(cmd&0x1e0u)==0x20u, pattern=(cmd&3u)==3u;
    unsigned index=copying ? 2 : pattern ? 3 : 2;
    uint32_t origin=p->packet[index], dimensions=p->packet[index+1];
    unsigned w=dimensions&4095,h=dimensions>>16;
    unsigned bytes=device.bpp/8;
    int32_t sx=0,sy=0,dx=origin&4095,dy=origin>>16;
    if (copying) {
        sx=p->packet[1]&4095; sy=p->packet[1]>>16;
    }
    CHECK(((p->packet[0]>>16)&255u)==(pattern ? 0xf0u : 0xccu));
    for (unsigned y=0; y<h; y++) for (unsigned x=0; x<w; x++) {
        int32_t px=dx+((p->packet[0]&XP) ? (int32_t)x : -(int32_t)x);
        int32_t py=dy+((p->packet[0]&YP) ? (int32_t)y : -(int32_t)y);
        CHECK(px>=0 && py>=0 && (unsigned)px<device.width && (unsigned)py<device.height);
        uint8_t *dst=device.mapped+(unsigned)py*device.pitch+(unsigned)px*bytes;
        uint32_t colour=p->packet[1];
        if (copying) {
            int32_t ax=sx+((p->packet[0]&XP) ? (int32_t)x : -(int32_t)x);
            int32_t ay=sy+((p->packet[0]&YP) ? (int32_t)y : -(int32_t)y);
            CHECK(ax>=0 && ay>=0 && (unsigned)ax<device.width && (unsigned)ay<device.height);
            memcpy(dst,device.mapped+(unsigned)ay*device.pitch+(unsigned)ax*bytes,bytes);
        } else {
            if (pattern) {
                unsigned shift=((unsigned)py&3u)*8;
                uint8_t row=(uint8_t)(p->packet[5+((unsigned)py&7u)/4]>>shift);
                colour=(row&(0x80u>>((unsigned)px&7u))) ? p->packet[1] : p->packet[2];
            }
            /* Model the T23's observed colour-lane replication when the
             * engine depth is narrower than the declared destination. This
             * is a regression hypothesis, not a full silicon emulator. */
            for (unsigned b=0;b<bytes;b++) dst[b]=(uint8_t)(colour>>((b%(engine_bpp/8))*8));
            if (p->corrupt_high && bytes==4) dst[3]=0x66;
        }
        if (p->partial && x==w/2) return;
    }
}
static uint32_t model_read(void *ctx,uint32_t offset)
{
    struct model *p=ctx;
    CHECK(offset<SAVAGE_MMIO_BYTES && !(offset&3u));
    marker_before_register();
    uint32_t value;
    if (offset==SAVAGE_STATUS) {
        CHECK(p->reset_phase==2 && p->reset_set_us>=10000 && p->reset_clear_us>=10000);
        if (p->last_status_busy) CHECK(p->us-p->last_status_us>=POLL_US);
        p->last_status_us=p->us; p->status_reads++;
        if (p->status_reads==1) p->first_status_us=p->us;
        if (p->dead) value=UINT32_MAX;
        else if (p->wrong_offset || p->stuck) value=0;
        else if (p->idle_after_us && p->us-p->first_status_us<p->idle_after_us)
            value=(p->status_value ? p->status_value : 0x00e00000u) | 1u;
        else if (p->busy) { p->busy--; value=(p->status_value ? p->status_value : 0x00e00000u)|1u; }
        else if (p->contradictory && p->status_reads>1) value=0x004e0000;
        else value=p->status_value ? p->status_value : 0x00e00000u;
        p->last_status_busy = p->busy || (value & 0x1ffffu);
    } else {
        CHECK(p->reset_phase==2 && p->reset_set_us>=10000 && p->reset_clear_us>=10000);
        value=p->dead ? UINT32_MAX : p->reg[offset/4];
        if (offset==BCI_CONTROL && !p->status_reads) CHECK(p->engine_writes==p->reset_engine_writes);
        if (offset==BCI_CONTROL && p->control_reserved) value|=0x4000;
        if (offset==0x816c && p->bad_readback) value^=16u;
        if (p->strict_window && offset>=0x8100 && offset<0x8180) {
            p->dword_2d_reads++;
            value=(value&0xffffu)|0xbadc0000u; /* unsupported upper half */
        }
    }
    return value;
}
static bool window_live(struct model *p)
{
    return !p->strict_window || (!p->window_locked && p->postreset_enable &&
        (p->vga_enable&1) && (p->misc&1) && (p->cr[0x38]&0xcc)==0x48 &&
        (p->cr[0x39]&0xe0)==0xa0 && (p->cr[0x40]&1) && p->cr[0x31]==0x0c &&
        p->seq[0x08]==6 && p->seq[0x26]==0x40);
}
static void setup_write(struct model *p, uint32_t reg, uint32_t value)
{
    if (p->setup_count<ARRAY_SIZE(p->setup)) { p->setup[p->setup_count].reg=reg; p->setup[p->setup_count].value=value; }
    p->setup_count++;
}
static void model_write(void *ctx,uint32_t offset,uint32_t value)
{
    struct model *p=ctx;
    marker_before_register();
    p->engine_writes++;
    CHECK(offset<SAVAGE_MMIO_BYTES && !(offset&3u));
    if (offset>=SAVAGE_BCI && offset<SAVAGE_BCI+64) {
        CHECK((p->reg[BCI_CONTROL/4]&15u)==8u);
        /* FIFO addresses do not select registers. A burst may contain
         * several commands, each with its own header and word count. */
        unsigned at=p->words;
        CHECK(at<16);
        CHECK(drains>0); /* CPU WC writes drained before command */
        p->packet[at]=value;
        if (!at) {
            if ((value&0xff000000u)==0x96000000u) {
                CHECK((value&0xffff0000u)==0x96010000u);
                CHECK((value&0xffffu)==0xe0 || (value&0xffffu)==0xe1);
                p->expected=2;
            } else {
                CHECK((value&0xfc00u)==(value&0x8000u));
                p->expected=(value&3u)==3u ? 7 : 4;
            }
        }
        if (++p->words==p->expected) {
            if ((p->packet[0]&0xff000000u)==0x96000000u) {
                unsigned index=p->packet[0]&1u;
                if (p->register_packets<ARRAY_SIZE(p->gbd_history)) {
                    p->gbd_history[p->register_packets][0]=p->packet[0];
                    p->gbd_history[p->register_packets][1]=p->packet[1];
                }
                if (!p->ignore_gbd && !(p->ignore_first_gbd && p->register_packets<2)) {
                    p->bci_gbd[index]=p->packet[1];
                    p->reg[(0x8168+index*4)/4]=p->packet[1];
                }
                p->register_packets++;
                p->busy=p->gbd_busy ? 3 : 0;
                if (p->fail_after_gbd && p->register_packets>=p->fail_after_gbd) p->stuck=true;
            } else {
                if (!p->ignore_fill) execute_packet(p);
                p->packets++; p->busy=3;
                if (p->fail_after_submit) p->stuck=true;
            }
            p->words=0;
        }
    } else {
        setup_write(p,offset,value);
        if (offset>=0x8100 && offset<0x8180 && !window_live(p)) { p->dead_window_writes++; return; }
        if (offset==BCI_CONTROL && p->bci_locked) return;
        p->reg[offset/4]=offset==0x816c && p->strict_window ? value&GBD_READ_MASK : value;
    }
}
static uint16_t model_read16(void *ctx,uint32_t offset)
{
    struct model *p=ctx; marker_before_register(); p->half_reads++;
    CHECK(offset>=0x8100 && offset<0x8180 && !(offset&1));
    CHECK(p->reset_phase==2 && p->reset_set_us>=10000 && p->reset_clear_us>=10000);
    uint32_t value=p->reg[(offset&~3u)/4];
    if ((offset&~3u)==0x816c) {
        if (p->gbd_reserved) value|=~GBD_READ_MASK;
        if (p->bad_readback) value^=16u;
    }
    uint16_t half=p->dead ? UINT16_MAX : !window_live(p) ? 0 : (uint16_t)(value>>((offset&2u)*8));
    return half;
}
static void model_write16(void *ctx,uint32_t offset,uint16_t value)
{
    struct model *p=ctx; marker_before_register(); p->half_writes++; p->engine_writes++;
    setup_write(p,offset,value);
    CHECK(offset==0x8134 || offset==0x8136);
    if (!window_live(p)) { p->dead_window_writes++; return; }
    unsigned shift=(offset&2u)*8;
    uint32_t *reg=&p->reg[(offset&~3u)/4];
    *reg=(*reg&~(0xffffu<<shift))|((uint32_t)value<<shift);
}
static uint8_t chip_cr_read(struct model *p,uint8_t index)
{
    if (index==0x45) p->colour_at=0;
    uint8_t value=p->cr[index];
    if (p->decorated) {
        if (index==0x38) value|=0x20;
        if (index==0x39) value|=8;
    }
    if (p->reserved_readbacks) {
        if (index==0x40) value^=0x80;
        if (index==0x66 && (++p->cr66_reads&1u)) value|=0x44;
    }
    if (index==0x66 && p->reset_fault==3 && p->reset_phase) value^=1u;
    return value;
}
static void chip_cr_write(struct model *p,uint8_t index,uint8_t value)
{
    setup_write(p,0x100000u|index,value);
    if (p->cr_writes<sizeof(p->cr_sequence)) p->cr_sequence[p->cr_writes]=index;
    p->cr_writes++;
    if (index==0x4a || index==0x4b) {
        CHECK(p->colour_at<3);
        p->colour[index==0x4b][p->colour_at++]=value;
    }
    if (p->locked || (index==0x39 && p->locked39)) return;
    if (index>=0x40 && ((p->cr[0x38]&0xcc)!=0x48 || (p->cr[0x39]&0xe0)!=0xa0)) return;
    if (index==0x40 && p->mmio_enable_failed) value=(value&~1u)|(p->cr[0x40]&1u);
    if (index==0x50 && p->depth_locked) value=(value&~0x30u)|(p->cr[0x50]&0x30u);
    if (index==0x40 && p->reset_phase==2 && value==1) p->postreset_enable=true;
    if (index==0x66) {
        CHECK((p->cr[index]&1u)==(value&1u));
        if (p->reset_fault==1) value&=~2u;
        if (p->reset_fault==2 && p->reset_phase) value|=2u;
        p->reset_phase=value&2u ? 1 : 2;
        if (value&2u) {
            p->postreset_enable=false;
            p->reset_set_us=0; p->reset_clear_us=0; p->status_reads=0;
            p->reset_engine_writes=p->engine_writes;
        }
    }
    p->cr[index]=value;
}
static bool alias_live(struct model *p) { return (p->cr[0x40]&1u) && !p->alias_dead; }
static uint8_t model_port_read(void *ctx,uint16_t port)
{
    struct model *p=ctx; marker_before_register(); p->port_reads++;
    CHECK(port>=0x3b0 && port<0x3e0);
    uint8_t value=0xff;
    if (port==0x3c3) value=p->vga_enable;
    else if (port==0x3cc) value=p->misc;
    else if (port==0x3c5) value=p->seq[p->seq_index];
    else if (port==0x3d5 && (p->vga_enable&1u) && (p->misc&1u)) {
        p->port_cr_reads[p->cr_index]++; value=chip_cr_read(p,p->cr_index);
    }
    else CHECK(false);
    return value;
}
static void model_port_write(void *ctx,uint16_t port,uint8_t value)
{
    struct model *p=ctx; marker_before_register(); p->port_writes++;
    CHECK(port>=0x3b0 && port<0x3e0);
    if (port==0x3c3) p->vga_enable=value;
    else if (port==0x3c2) p->misc=value;
    else if (port==0x3c4) p->seq_index=value;
    else if (port==0x3c5) {
        setup_write(p,0x200000u|p->seq_index,value);
        if (!(p->seq_index==0x26 && p->pipe_locked)) p->seq[p->seq_index]=value;
    }
    else if (port==0x3d4) { CHECK((p->vga_enable&1u) && (p->misc&1u)); p->cr_index=value; }
    else if (port==0x3d5) chip_cr_write(p,p->cr_index,value);
    else CHECK(false);
}
static uint8_t test_inb(uint16_t port) { return model_port_read(&m,port); }
static void test_outb(uint16_t port,uint8_t value) { model_port_write(&m,port,value); }
static uint64_t model_now(void *ctx)
{
    struct model *p=ctx;
    if (!p->frozen && !p->exact_clock) p->us+=100;
    return p->frozen ? 0 : p->us;
}
static int model_delay(void *ctx,uint32_t us)
{
    struct model *p=ctx; p->delays++;
    if (initializing) { marker_before_register(); if (probing) CHECK(!fbdev_console_begin()); }
    if (p->delay_failed) return -EFAULT;
    p->us+=us;
    if (p->reset_phase==1) p->reset_set_us+=us;
    else if (p->reset_phase==2) p->reset_clear_us+=us;
    return 0;
}
static uint8_t model_read8(void *ctx,uint32_t offset)
{
    struct model *p=ctx;
    marker_before_register();
    p->alias_reads++;
    CHECK(offset==0x83d5);
    uint8_t value=!alias_live(p) ? 0xff : chip_cr_read(p,p->cr_index);
    if (p->cr_index==0x38 && p->alias_lock_mismatch) value^=0x20;
    return value;
}
static void model_write8(void *ctx,uint32_t offset,uint8_t value)
{
    struct model *p=ctx;
    marker_before_register();
    p->alias_writes++;
    CHECK(offset==0x83d4 && value==0x38); /* diagnostic index only */
    if (alias_live(p)) p->cr_index=value;
}
static void model_drain(void *ctx)
{
    /* A new synchronized transaction may read immediately; pacing applies
     * only between busy reads within one wait. */
    struct model *p=ctx;
    p->last_status_busy=false;
    if (p->recover_on_final && engine.quarantined) { p->stuck=false; p->busy=0; }
    framebuffer_write_drain();
}
static void model_fault(void *ctx)
{
    ((struct model *)ctx)->fault_count++;
    if (initializing) hw_fault(NULL);
}
static struct savage_io model_io(void)
{
    return (struct savage_io){&m,model_read,model_write,
        model_now,model_delay,model_drain,model_fault,model_read8,model_write8,model_port_read,model_port_write,model_read16,model_write16};
}
const struct accel_ops *boot_init(const struct fb_device *d, struct vfs *vfs)
{ CHECK(marker_thread_running); boot_calls++; struct savage_io io=model_io(); return init_with_io(d,vfs,&io); }
static uint8_t *storage;
static void prepare(unsigned bpp)
{
    memset(&m,0,sizeof(m)); memset(&test_engine,0,sizeof(test_engine));
    g_cpu_tsc=false; g_tsc_per_ms=0; accel_log_cost=NULL; marker_cost_us=0;
    m.cr[0x66]=1;
    m.cr[0x50]=0; /* firmware VGA depth, independent of the active VBE mode */
    initialized=true; changing=false; accel_initializing=false; drawing=0; console_rows=8;
    g_boot.flags=0; acceleration=NULL;
    device=(struct fb_device){.width=96,.height=64,.pitch=128*(bpp/8),.size=64*128*(bpp/8),
        .phys=0xe0000000,.bpp=bpp,.red_size=bpp==16 ? 5 : 8,.red_pos=bpp==16 ? 11 : 16,
        .green_size=bpp==16 ? 6 : 8,.green_pos=bpp==16 ? 5 : 8,.blue_size=bpp==16 ? 5 : 8,
        .present=true,.handle=0,.generation=42};
    storage=malloc(device.size+32); CHECK(storage!=NULL);
    memset(storage,0xa5,device.size+32); device.mapped=storage+16;
    reservation=(struct resource){.state=RS_ACTIVE,.generation=42};
    struct savage_io io=model_io();
    CHECK(!savage2d_start(&test_engine,&io,&device,true,m.cursor_page,0x100000));
    acceleration=&test_engine.ops;
}
static void finish(void)
{
    for (unsigned i=0;i<16;i++) CHECK(storage[i]==0xa5 && storage[16+device.size+i]==0xa5);
    for (unsigned y=0;y<device.height;y++) for (unsigned x=device.width*(device.bpp/8);x<device.pitch;x++)
        CHECK(device.mapped[y*device.pitch+x]==0xa5);
    free(storage); acceleration=NULL;
}
static struct ciuki_accel_request request(unsigned op)
{ return (struct ciuki_accel_request){.size=sizeof(struct ciuki_accel_request),.operation=op,.generation=42}; }
static void commands(unsigned bpp)
{
    prepare(bpp);
    CHECK(!test_engine.ops.stop(&test_engine));
    memset(&m,0,sizeof(m)); m.cr[0x66]=1; m.alias_dead=true;
    m.cr[0x50]=bpp==16 ? 0x30 : 0x10; /* stale opposite depth must be replaced */
    test_engine=(struct savage2d){0}; struct savage_io io=model_io();
    CHECK(!savage2d_start(&test_engine,&io,&device,true,m.cursor_page,0x100000));
    CHECK(m.alias_reads==1 && m.alias_writes==1 && m.seq[0x08]==6 && m.seq[0x26]==0x40);
    CHECK(m.reg[0x48c18/4]==8 && m.reg[0x48c0c/4]==0);
    CHECK(m.reg[0x816c/4]==(test_engine.descriptor|9u));
    CHECK((m.cr[0x50]&0x30u)==(bpp==16 ? 0x10u : 0x30u) && m.cr[0x66]==1);
    struct ciuki_accel_request q=request(CIUKI_ACCEL_FILL);
    q.rect=(struct ciuki_rect){0,0,-5,-3,30,20}; q.colour=0x123456;
    uint8_t *expected=malloc(device.size); CHECK(expected!=NULL);
    memcpy(expected,device.mapped,device.size);
    struct fb_rect r={-5,-3,30,20}; reference_fill(&device,expected,&r,q.colour);
    CHECK(!fbdev_accel_request(&q)); CHECK(!memcmp(device.mapped,expected,device.size));
    CHECK(m.packet[0]==0x4bcc8000u && m.expected==4 && m.packet[3]==0x00110019u);
    q=request(CIUKI_ACCEL_PATTERN); q.rect=(struct ciuki_rect){0,0,3,4,20,15};
    q.colour=0xabcdef; q.background=0x643210;
    for (unsigned i=0;i<8;i++) q.pattern[i]=(uint8_t)(0x81u^(i*19));
    for (unsigned y=4;y<19;y++) for (unsigned x=3;x<23;x++)
        reference_pixel(&device,expected+y*device.pitch+x*(bpp/8),q.pattern[y&7]&(0x80u>>(x&7)) ? q.colour : q.background);
    CHECK(!fbdev_accel_request(&q)); CHECK(!memcmp(device.mapped,expected,device.size));
    CHECK(m.packet[0]==0x4bf08003u && m.expected==7);
    for (int direction=0;direction<8;direction++) {
        for (unsigned y=0;y<device.height;y++) for (unsigned x=0;x<device.width;x++)
            reference_pixel(&device,device.mapped+y*device.pitch+x*(bpp/8),(x*37u+y*9983u)&0xffffffu);
        memcpy(expected,device.mapped,device.size);
        uint8_t before[64*512]; memcpy(before,device.mapped,device.size);
        int dx=(direction&1) ? 5 : -5,dy=(direction&2) ? 4 : -4;
        if (direction&4) dy=0;
        q=request(CIUKI_ACCEL_COPY); q.rect=(struct ciuki_rect){20,20,20+dx,20+dy,48,25};
        for (unsigned y=0;y<25;y++) for (unsigned x=0;x<48;x++)
            memcpy(expected+(20+dy+y)*device.pitch+(20+dx+x)*(bpp/8),before+(20+y)*device.pitch+(20+x)*(bpp/8),bpp/8);
        CHECK(!fbdev_accel_request(&q)); CHECK(!memcmp(device.mapped,expected,device.size));
        CHECK((m.packet[0]&~0x03000000u)==0x48cc0020u && m.expected==4);
        CHECK((m.packet[0]&XP)!=0 ? dx<=0 : dx>0);
        CHECK((m.packet[0]&YP)!=0 ? dy<=0 : dy>0);
    }
    unsigned packets=m.packets;
    q.generation--; CHECK(fbdev_accel_request(&q)==-EAGAIN); CHECK(m.packets==packets);
    q.generation=42; q.rect.src_x=INT32_MAX; CHECK(fbdev_accel_request(&q)==-EINVAL); CHECK(m.packets==packets);
    q=request(CIUKI_ACCEL_CURSOR_SET); q.colour=0xffffff; q.background=0x101010;
    q.cursor_mask[0]=0xc0000000; q.cursor_image[0]=0x40000000;
    CHECK(!fbdev_accel_request(&q));
    CHECK(m.cursor_page[0]==0xff && m.cursor_page[1]==0x3f && m.cursor_page[2]==0 && m.cursor_page[3]==0x40);
    CHECK(m.cursor_page[4]==0xff && m.cursor_page[5]==0xff && !m.cursor_page[6] && !m.cursor_page[7]);
    CHECK(m.cr[0x4c]==4 && m.cr[0x4d]==0 && !(m.cr[0x55]&16u));
    q=request(CIUKI_ACCEL_CURSOR_MOVE); q.rect.dst_x=-5; q.rect.dst_y=511;
    unsigned at=m.cr_writes;
    CHECK(!fbdev_accel_request(&q)); CHECK(m.cr[0x4e]==5 && m.cr[0x4f]==0);
    CHECK(m.cr[0x46]==0 && m.cr[0x47]==0 && m.cr[0x48]==1 && m.cr[0x49]==255);
    CHECK(m.cr_sequence[at+5]==0x48);
    q=request(CIUKI_ACCEL_CURSOR_SHOW); q.colour=1;
    CHECK(!fbdev_accel_request(&q) && (m.cr[0x45]&1));
    CHECK(!test_engine.ops.stop(&test_engine) && !test_engine.active && !(m.cr[0x45]&1));
    CHECK(m.cr[0x50]==(bpp==16 ? 0x30u : 0x10u)); /* firmware value restored */
    CHECK(m.alias_reads==1 && m.alias_writes==1 && test_engine.operations>10);
    free(expected); finish();
}
static void failures_and_fallback(void)
{
    prepare(32);
    struct ciuki_accel_request q=request(CIUKI_ACCEL_COPY); q.rect=(struct ciuki_rect){4,4,8,8,40,30};
    m.partial=true; m.fail_after_submit=true;
    CHECK(fbdev_accel_request(&q)==-EAGAIN && test_engine.quarantined && m.fault_count==1);
    CHECK(!fbdev_console_begin()); /* no CPU writer until idle proof */
    struct fb_rect r={0,0,10,10};

    unsigned packets=m.packets;
    m.stuck=false; m.partial=false;
    uint32_t fault_status=test_engine.status;
    CHECK(!fbdev_fill(&r,0x102030)); /* idle now proven, quarantined software fill */
    CHECK(m.packets==packets && m.fault_count==1 && test_engine.quarantined);
    CHECK(test_engine.status==fault_status); /* original quarantine observation */
    CHECK(fbdev_console_begin()); fbdev_console_end();
    struct savage_io io=model_io();
    CHECK(savage2d_start(&test_engine,&io,&device,true,m.cursor_page,0x100000)==-EIO);
    finish();
    prepare(32); m.stuck=true; m.frozen=true;
    struct fb_rect point={0,0,1,1};
    CHECK(test_engine.ops.fill_rect(&test_engine,&point,0x123456)==-EIO && m.fault_count==1);
    finish();
    prepare(32);
    struct savage2d other={0}; io=model_io(); m.bad_readback=true;
    CHECK(savage2d_start(&other,&io,&device,false,NULL,0)==-EIO && other.quarantined);
    finish();
    prepare(32);io=model_io();other=(struct savage2d){0};
    CHECK(!savage2d_start(&other,&io,&device,false,NULL,0));
    CHECK(m.reg[0x816c/4]==(other.descriptor|9u) && other.ops.capabilities(&other)==1u);
    CHECK(!other.ops.stop(&other));
    m.cr[0x66]=0;other=(struct savage2d){0};
    CHECK(savage2d_start(&other,&io,&device,true,NULL,0)==-ENODEV);
    CHECK(!other.active && !other.quarantined && other.cpu_safe);
    device.bpp=24;CHECK(savage2d_start(&other,&io,&device,true,NULL,0)==-EINVAL);
    device.bpp=32;device.pitch++;CHECK(savage2d_start(&other,&io,&device,true,NULL,0)==-EINVAL);device.pitch--;
    finish();
    struct pci_func p={.class_code=3,.vendor=0x5333,.device=0x8c2e};
    CHECK(savage2d_supported(&p)); p.device=0x8a22; CHECK(savage2d_supported(&p));
    p.device=0x9102; CHECK(!savage2d_supported(&p)); p.device=0x8c2e;p.vendor=0x1234;CHECK(!savage2d_supported(&p));
    prepare(32);
    binding_gpu=(struct pci_func){.class_code=3,.vendor=0x5333,.device=0x8c2e,
        .command=3,.bar={0xe1000000,0xe0000000}};
    binding_pci_count=1;binding_claims=0;
    settings_volume=(struct fat_volume){.mounted=true}; settings_vfs.volumes[2]=&settings_volume;
    CHECK(!savage2d_init(&device,&settings_vfs) && binding_claims==1); /* exclusive ownership conflict */
    binding_gpu.command=0;CHECK(!savage2d_init(&device,&settings_vfs) && binding_claims==1);
    binding_gpu.command=3;binding_gpu.bar[0]|=4;CHECK(!savage2d_init(&device,&settings_vfs) && binding_claims==1); /* 64-bit BAR */
    binding_gpu.bar[0]=0xe1000000;binding_gpu.bar[1]=0xe2000000;CHECK(!savage2d_init(&device,&settings_vfs) && binding_claims==1);
    binding_gpu.bar[1]=device.phys;binding_pci_count=2;CHECK(!savage2d_init(&device,&settings_vfs) && binding_claims==1);
    binding_pci_count=0;finish();
}
static unsigned log_matches(const char *part)
{
    unsigned found=0;
    for (unsigned i=0;i<accel_count && i<ACCEL_LOG_CAP;i++) if (strstr(accel_log[i],part)) found++;
    return found;
}
/* Real MMIO dword glue and frozen production hw_now; VGA indexing is a
 * transport model because a byte array cannot implement indexed registers. */
static void glue_stalled_clock(uint32_t status)
{
    prepare(32);
    uint8_t *window = calloc(1, SAVAGE_MMIO_BYTES); CHECK(window != NULL);
    memcpy(window + SAVAGE_STATUS, &status, sizeof(status));
    registers = window; g_ticks = 0; g_boot.flags = CBI_F_VERBOSE;
    step_count = accel_count = 0;
    struct savage_io io = model_io(); io.read32=hw_read; io.write32=hw_write; io.now_us=hw_now;
    struct savage2d e = {0};
    unsigned delays=m.delays;
    CHECK(savage2d_start(&e, &io, &device, true, NULL, 0) == -EIO);
    CHECK(e.quarantined && !e.touched && !e.active && e.idle_waits == 1 && e.timeouts == 1);
    CHECK(!step_count); /* no synchronous transport trace */
    CHECK(log_matches("vga_alias reg=083d5 port=48 alias=48 match=1")==1);
    for (unsigned i=0;i<step_count && i<ACCEL_LOG_CAP;i++) CHECK(strstr(step_log[i]," value="));
    CHECK(log_matches("savage status reg=48c60 raw=")==1 && !e.idle_mask);
    CHECK(e.status==status && e.status_logged);
    CHECK(log_matches("savage status sample=")== (status==UINT32_MAX ? 1u : STATUS_SAMPLES));
    CHECK(m.delays-delays==(status==UINT32_MAX ? 2u : 2u+INITIAL_POLLS-1));
    registers = NULL; g_boot.flags = 0; free(window);
    finish();
}

static void boot_fixture_mode(bool retain_marker, unsigned bpp)
{
    /* Allocate the actual framebuffer, but make initialization entirely late. */
    prepare(bpp); acceleration=NULL;
    memset(&m,0,sizeof(m)); m.cr[0x66]=1;
    engine=(struct savage2d){0}; attempt_blocked=false; reg_claim=cursor_claim=port_claim=-1;
    registers=NULL; register_window=cursor_window=NULL; accel_vfs=NULL; accel_queued=false;
    binding_allowed=true; binding_claims=0; binding_pci_count=1;
    binding_port_conflict=false; memset(binding_resources,0,sizeof(binding_resources));
    binding_gpu=(struct pci_func){.class_code=3,.vendor=0x5333,.device=0x8c2e,.command=3,
        .bar={0xe1000000,device.phys}};
    settings_volume=(struct fat_volume){.mounted=true};
    memset(&settings_vfs,0,sizeof(settings_vfs)); settings_vfs.volumes[2]=&settings_volume;
    marker_commits=marker_fault=0; marker_error=clear_error=false;
    if (!retain_marker) { disk_size=0; marker_exists=false; memset(disk_marker,0,sizeof(disk_marker)); }
    CHECK(!table_open && !marker_open);
    normal_yields=device_work_requests=0; g_boot.flags=CBI_F_VERBOSE; step_count=accel_count=0;
    attempt_in_progress=false; init_interleave=NULL;
    accel_marker_task=NULL; accel_marker_request=ACCEL_MARKER_IDLE;
    fail_marker_worker=false; marker_worker_creates=marker_worker_starts=0;
}
static void boot_fixture_accel(bool retain_marker) { boot_fixture_mode(retain_marker,32); }
static void run_accel_init(void)
{
    CHECK(accel_marker_request==ACCEL_MARKER_BEGIN);
    marker_thread_step();
    if (accel_marker_request==ACCEL_MARKER_INIT) {
        CHECK(disk_size==14 && marker_commits==1 && !table_open && !marker_open);
        marker_thread_step();
        CHECK(accel_marker_request==((acceleration && !engine.quarantined) ? ACCEL_MARKER_CLEAR : ACCEL_MARKER_STATUS));
        marker_thread_step();
    }
    CHECK(!accel_queued && !table_open && !marker_open && !locks && !accel_marker_pending(NULL));
    CHECK(!device_work_requests);
}
static void software_desktop(void)
{
    CHECK(fbdev_get()->present);
    struct fb_rect r={16,16,10,10}; unsigned packets=m.packets;
    CHECK(!fbdev_fill(&r,0x123456));
    uint8_t expected[4]; reference_pixel(&device,expected,0x123456);
    CHECK(!memcmp(device.mapped+16*device.pitch+16*4,expected,4));
    if (!acceleration) {
        CHECK(m.packets==packets);
        struct ciuki_accel_request q=request(CIUKI_ACCEL_QUERY);
        CHECK(fbdev_accel_request(&q)==0);
    }
}
static void deferred_boots(void)
{
    /* Every failure reaches the actual software framebuffer presenter, while
     * the normal thread runs the production marker and register decisions. */
    for (unsigned variant=0;variant<13;variant++) {
        boot_fixture_accel(false);
        if (variant==0) { m.stuck=true; m.frozen=true; }
        if (variant==1) m.dead=true;
        if (variant==2) m.wrong_offset=true;
        if (variant==3) m.locked=true;
        if (variant==4) m.busy=12;
        if (variant==5) { m.reg[BCI_CONTROL/4]=8; m.bci_locked=true; }
        if (variant==6) m.delay_failed=true;
        if (variant==7) { m.busy=8; m.status_value=0x004e0000; }
        if (variant==8) m.status_value=0x00a00000; /* Linux-only cannot prove idle */
        if (variant==9) m.contradictory=true;
        if (variant==10) m.control_reserved=true;
        if (variant==11) m.alias_dead=true;
        if (variant==12) m.reg[BCI_CONTROL/4]=2; /* reject before status */
        fbdev_accel_schedule(&settings_vfs); fbdev_accel_schedule(&settings_vfs);
        CHECK(accel_marker_request==ACCEL_MARKER_BEGIN && !marker_commits && !m.cr_writes && !binding_claims);
        software_desktop(); /* desktop can draw before initialization */
        run_accel_init();
        bool ready=variant==4 || variant==7 || variant==10 || variant==11;
        bool guarded=variant==9; /* queued setup lost its completion proof */
        bool annotated=!ready && engine.status_logged;
        CHECK(acceleration==((ready || guarded) ? &engine.ops : NULL) && marker_commits==((ready || annotated) ? 2u : 1u));
        CHECK(disk_size==(ready ? 0u : annotated ? 30u : 14u) && m.status_reads<=INITIAL_POLLS+1);
        CHECK(log_matches(ready ? "[accel] savage ready" : "[accel] savage quarantined reason=")==1);
        if (variant==1 || variant==3 || variant==6 || variant==12) CHECK(!m.status_reads);
        else CHECK(log_matches("savage status reg=48c60 raw=")==1);
        if (variant==1) CHECK(log_matches("engine_probe reg=48c18 raw=ffffffff")==1);
        if (variant==11) CHECK(log_matches("vga_alias reg=083d5 port=48 alias=ff match=0")==1);
        if (variant==12) CHECK(log_matches("quarantined reason=firmware_command_buffer")==1);
        if (!ready && !guarded) software_desktop();
        if (guarded) {
            CHECK(log_matches("quarantined reason=status_contradiction")==1);
            CHECK(engine.submitted && engine.final_idle_checked && !engine.cpu_safe && !m.packets);
            unsigned reads=m.status_reads;
            struct fb_rect point={16,16,1,1};
            CHECK(fbdev_fill(&point,0x123456)==-EIO && !fbdev_console_begin());
            CHECK(m.status_reads==reads);
        }
        printf("deferred savage boot variant=%u ready=%u status_reads=%u marker_bytes=%u desktop=visible\n",
            variant,ready,m.status_reads,disk_size);
        finish();
    }
    /* Simulated hard reset retains only the committed attempt, including on
     * a read-only second mount. Absolutely no register/claim follows it. */
    boot_fixture_accel(false);
    bool previous=false; CHECK(!attempt_marker(&settings_vfs,false,&previous) && !previous && disk_size==14);
    finish(); boot_fixture_accel(true); settings_volume.readonly=true;
    fbdev_accel_schedule(&settings_vfs); run_accel_init();
    CHECK(!acceleration && !binding_claims && !m.cr_writes && !m.status_reads && !marker_commits);
    CHECK(log_matches("skipped after a failed attempt")==1 && log_matches("quarantined reason=previous_attempt")==1);
    software_desktop(); finish();
    for (unsigned variant=0;variant<5;variant++) {
        boot_fixture_accel(false);
        if (variant==0) settings_vfs.volumes[2]=NULL;
        if (variant==1) settings_volume.readonly=true;
        if (variant==2) marker_error=true;
        if (variant==3) { g_boot.flags|=CBI_F_SAFE_MODE; }
        if (variant==4) fail_marker_worker=true;
        fbdev_accel_schedule(&settings_vfs);
        if (accel_marker_pending(NULL)) run_accel_init();
        CHECK(!acceleration && !binding_claims && !m.cr_writes && !m.status_reads);
        if (variant==4) CHECK(!marker_commits && log_matches("marker_worker_unavailable")==1);
        software_desktop(); finish();
    }
    boot_fixture_accel(false); clear_error=true;
    fbdev_accel_schedule(&settings_vfs); run_accel_init();
    CHECK(!acceleration && engine.quarantined && disk_size==14);
    CHECK(log_matches("quarantined reason=marker_clear_failed")==1);
    software_desktop(); finish();
    for (unsigned failure=1;failure<=6;failure++) {
        boot_fixture_accel(false); marker_fault=failure;
        fbdev_accel_schedule(&settings_vfs); run_accel_init();
        CHECK(!acceleration && !binding_claims && !m.cr_writes && !m.status_reads);
        CHECK(log_matches("quarantined reason=marker_unavailable")==1);
        software_desktop(); finish();
    }
    binding_allowed=false; binding_pci_count=0; accel_vfs=NULL;
}
static void async_marker(void)
{
    boot_fixture_accel(false); m.alias_dead=true; marker_cost_us=337704;
    fbdev_accel_schedule(&settings_vfs); fbdev_accel_schedule(&settings_vfs);
    CHECK(marker_worker_creates==1 && marker_worker_starts==1 && marker_worker.prio==P_NORMAL);
    CHECK(accel_queued && accel_marker_request==ACCEL_MARKER_BEGIN);
    CHECK(!marker_commits && !table_open && !binding_claims && !m.port_reads);
    software_desktop();
    marker_thread_step();
    CHECK(marker_commits==1 && disk_size==14 && accel_marker_request==ACCEL_MARKER_INIT && accel_queued && !locks);
    CHECK(!m.port_reads && !m.alias_reads && !m.status_reads && !binding_claims);
    CHECK(!table_open && !marker_open && accel_marker_request==ACCEL_MARKER_INIT);
    software_desktop();
    marker_thread_step();
    CHECK(acceleration && marker_commits==1 && disk_size==14 && accel_queued);
    CHECK(accel_marker_request==ACCEL_MARKER_CLEAR && !locks);
    software_desktop();
    marker_thread_step();
    CHECK(!accel_queued && disk_size==0 && marker_commits==2);
    CHECK(!acceleration->stop(acceleration->ctx)); acceleration=NULL;
    marker_commits=0;
    fbdev_accel_schedule(&settings_vfs); run_accel_init();
    CHECK(acceleration && marker_worker_creates==1 && marker_worker_starts==1);
    finish();

    /* Interrupt the real asynchronous path between persistence and probe.
     * Reboot retains only committed bytes, never a pending INIT phase. */
    boot_fixture_accel(false); fbdev_accel_schedule(&settings_vfs); marker_thread_step();
    CHECK(disk_size==14 && accel_marker_request==ACCEL_MARKER_INIT && !binding_claims && !m.port_reads);
    finish(); boot_fixture_accel(true);
    fbdev_accel_schedule(&settings_vfs); run_accel_init();
    CHECK(!acceleration && !m.port_reads && !m.alias_reads && !m.status_reads && !marker_commits);
    CHECK(log_matches("quarantined reason=previous_attempt")==1);
    software_desktop(); finish();

    /* Decision comes first: ineligible modes/devices never write a marker
     * or allocate a persistence thread. */
    for (unsigned variant=0;variant<4;variant++) {
        boot_fixture_accel(false);
        if (variant==0) binding_gpu.device=0x9102;
        if (variant==1) binding_gpu.command=1;
        if (variant==2) binding_gpu.bar[0]=0;
        if (variant==3) device.pitch++;
        fbdev_accel_schedule(&settings_vfs);
        CHECK(!accel_queued && !accel_marker_pending(NULL) && !marker_worker_creates);
        CHECK(!marker_commits && !table_open && !binding_claims && !m.port_reads);
        if (variant==3) device.pitch--;
        finish();
    }
    binding_allowed=false; binding_pci_count=0; accel_vfs=NULL;
}
static void optional_alias(void)
{
    for (unsigned variant=0;variant<3;variant++) {
        prepare(32); CHECK(!test_engine.ops.stop(&test_engine));
        memset(&m,0,sizeof(m)); m.cr[0x66]=1; m.alias_dead=true;
        struct savage2d e={0}; struct savage_io io=model_io();
        if (variant!=1) io.read8=NULL;
        if (variant!=0) io.write8=NULL;
        CHECK(!savage2d_start(&e,&io,&device,true,NULL,0) && e.active);
        CHECK(!m.alias_reads && !m.alias_writes && m.status_reads==2);
        CHECK(!e.ops.stop(&e)); finish();
    }
}
static void port_bootstrap(void)
{
    static const char *reasons[]={NULL,NULL,"crtc_locked","crtc_locked",
        "crtc_locked",NULL,"engine_disabled","delay_failed","descriptor_readback_failed"};
    for (unsigned variant=0;variant<ARRAY_SIZE(reasons);variant++) {
        prepare(32);
        memset(&m,0,sizeof(m)); m.cr[0x66]=variant==6 ? 0 : 1;
        m.cr[0x40]=variant==1 ? 0xabu : 0xaau;
        m.vga_enable=0x24; m.misc=0x8e;
        m.cr_index=0x38;
        CHECK(model_read8(&m,0x83d5)==(variant==1 ? 0 : 0xff));
        m.alias_reads=0;
        m.locked=variant==2; m.locked39=variant==3;
        m.mmio_enable_failed=variant==4; m.alias_dead=variant==5;
        m.delay_failed=variant==7; m.bad_readback=variant==8;
        struct savage2d e={0}; struct savage_io io=model_io();
        step_count=accel_count=0;
        int err=savage2d_start(&e,&io,&device,true,NULL,0);
        CHECK(m.vga_enable==0x25 && m.misc==0x8f);
        CHECK(m.port_cr_reads[0x38]==1 && m.port_cr_reads[0x39]==1);
        if (variant<2 || variant==5) {
            CHECK(!err && e.active && e.saved40==(variant==1 ? 0xab : 0xaa));
            CHECK(m.cr[0x40]==1 && m.cr[0x39]==0xa0);
            CHECK(!step_count);
            CHECK(log_matches(variant==5 ? "vga_alias reg=083d5 port=48 alias=ff match=0" : "vga_alias reg=083d5 port=48 alias=48 match=1")==1);
            CHECK(m.seq[0x26]==0x40);
            m.cr[0x40]=0x55; /* preserve changes to all bits except ours */
            CHECK(!e.ops.stop(&e) && !e.active && !e.mmio_saved);
            CHECK(m.cr[0x40]==(variant==1 ? 0x55 : 0x54));
            unsigned aliases=m.alias_reads+m.alias_writes;
            CHECK(!e.ops.stop(&e) && aliases==m.alias_reads+m.alias_writes);
        } else {
            CHECK(err==(variant==6 ? -ENODEV : -EIO));
            CHECK(e.reason && !strcmp(e.reason,reasons[variant]));
            CHECK(!e.active && !e.mmio_saved && m.cr[0x40]==(variant==8 ? 0 : 0xaa));
            if (variant<=4) CHECK(!m.alias_reads && !m.alias_writes);
            if (variant<=7) CHECK(!m.status_reads && !m.engine_writes);
            if (variant!=6) CHECK(e.quarantined && m.fault_count==1);
            if (variant<=7) {
                unsigned reads=m.status_reads;
                CHECK(e.ops.stop(&e)==(variant==6 ? 0 : -EIO));
                CHECK(m.status_reads==reads); /* failed bootstrap never polls status */
            }
        }
        for (unsigned i=0;i<step_count && i<ACCEL_LOG_CAP;i++) CHECK(strstr(step_log[i]," value="));
        finish();
    }
    /* Exercise production port bounds without executing privileged host I/O. */
    unsigned reads=m.port_reads,writes=m.port_writes;
    CHECK(hw_port_read(NULL,0x3af)==0xff && hw_port_read(NULL,0x3e0)==0xff);
    hw_port_write(NULL,0x3af,0); hw_port_write(NULL,0x3e0,0);
    CHECK(m.port_reads==reads && m.port_writes==writes);
    CHECK(hw_port_read(NULL,0x3c3)==m.vga_enable);
    hw_port_write(NULL,0x3c3,0x25); CHECK(m.vga_enable==0x25);
}
static void port_ownership(void)
{
    boot_fixture_accel(false); binding_port_conflict=true;
    fbdev_accel_schedule(&settings_vfs); run_accel_init();
    CHECK(!acceleration && binding_claims==1 && !m.port_reads && !m.port_writes && !m.alias_reads);
    CHECK(log_matches("quarantined reason=port_claim_failed")==1 && disk_size==14);
    software_desktop(); finish();
    boot_fixture_accel(false); binding_gpu.command=2;
    fbdev_accel_schedule(&settings_vfs);
    CHECK(!accel_marker_pending(NULL) && !marker_commits && !marker_worker_creates);
    CHECK(!acceleration && !binding_claims && !m.port_reads && !m.port_writes);
    CHECK(log_matches("quarantined reason=io_decode_disabled")==1);
    finish();
    boot_fixture_accel(false);
    fbdev_accel_schedule(&settings_vfs); run_accel_init();
    CHECK(acceleration && binding_claims==2 && binding_resources[1].state==RS_ACTIVE);
    struct ciuki_accel_info snapshot;
    unsigned status_reads=m.status_reads,port_reads=m.port_reads;
    CHECK(!fbdev_accel_info(device.generation,&snapshot));
    CHECK(snapshot.driver==CIUKI_ACCEL_DRIVER_SAVAGE && (snapshot.capabilities&1) &&
        snapshot.capabilities==acceleration->capabilities(acceleration->ctx) &&
        !snapshot.quarantined && snapshot.operations==engine.operations && snapshot.idle_waits==engine.idle_waits);
    CHECK(m.status_reads==status_reads && m.port_reads==port_reads); /* purely cached */
    uint64_t operations=snapshot.operations;
    struct ciuki_accel_request sample={.size=sizeof(sample),.operation=CIUKI_ACCEL_FILL,
        .generation=device.generation,.rect={0,0,0,0,1,1},.colour=0x123456};
    CHECK(!fbdev_accel_request(&sample));
    CHECK(!fbdev_accel_info(device.generation,&snapshot) && snapshot.operations==operations+1);
    CHECK(!acceleration->stop(acceleration->ctx)); acceleration=NULL;
    CHECK(port_claim<0 && reg_claim<0 && !registers && !(m.cr[0x40]&1u));
    CHECK(binding_resources[1].state==RS_RELEASED && binding_resources[2].state==RS_RELEASED);
    /* A new work item can acquire the same exclusive ports after firmware. */
    marker_commits=0;
    fbdev_accel_schedule(&settings_vfs); run_accel_init();
    CHECK(acceleration && binding_claims==4 && (m.cr[0x40]&1u));
    savage2d_quarantine("ownership_test"); acceleration=NULL;
    CHECK(!fbdev_accel_info(device.generation,&snapshot) && snapshot.quarantined && !snapshot.capabilities &&
        snapshot.driver==CIUKI_ACCEL_DRIVER_SAVAGE);
    CHECK(!(m.cr[0x40]&1u) && !engine.active && binding_resources[1].state==RS_QUARANTINED);
    CHECK(binding_resources[2].state==RS_QUARANTINED && port_claim>=0 && reg_claim>=0);
    finish();
    binding_allowed=false; binding_pci_count=0; accel_vfs=NULL;
}
static void decorated_readbacks(void)
{
    static const char *reasons[]={NULL,"crtc_locked","crtc_locked",NULL,
        "reset_readback_failed","reset_readback_failed","reset_readback_failed",
        "pipe_select_failed"};
    for (unsigned variant=0;variant<ARRAY_SIZE(reasons);variant++) {
        boot_fixture_accel(false);
        m.decorated=true; m.reserved_readbacks=true;
        if (variant==1) m.locked=true;
        if (variant==2) m.locked39=true;
        if (variant==3) m.alias_lock_mismatch=true;
        if (variant>=4 && variant<=6) m.reset_fault=variant-3;
        if (variant==7) m.pipe_locked=true;
        fbdev_accel_schedule(&settings_vfs); run_accel_init();
        CHECK(!!acceleration==(variant==0 || variant==3));
        CHECK(!step_count);
        if (variant==1 || variant==2) CHECK(log_matches("init reg=00038") == 1);
        else CHECK(m.cr[0x38]==0x48 && m.cr[0x39]==0xa0);
        if (!variant || variant==3) {
            CHECK(m.status_reads==6 && m.delays==5); /* reset plus BCI completion */
            CHECK(log_matches(variant==3 ? "vga_alias reg=083d5 port=68 alias=48 match=0" : "vga_alias reg=083d5 port=68 alias=68 match=1")==1);
            CHECK(disk_size==0 && engine.active && !engine.quarantined);
        } else {
            char reason[96]; snprintf(reason,sizeof(reason),"quarantined reason=%s us=",reasons[variant]);
            CHECK(log_matches(reason)==1 && disk_size==14);
            CHECK(!m.status_reads && !m.engine_writes && !engine.status_ready);
            if (variant<3) CHECK(!m.alias_reads && !m.alias_writes);
            software_desktop();
        }
        for (unsigned i=0;i<step_count && i<ACCEL_LOG_CAP;i++) CHECK(strstr(step_log[i]," us="));
        finish();
    }
}
static uint64_t simulated_trace_us, simulated_bytes, simulated_log_us;
static uint64_t host_time_us(void)
{
    struct timespec ts;
    CHECK(!clock_gettime(CLOCK_MONOTONIC,&ts));
    return (uint64_t)ts.tv_sec*1000000+(uint64_t)ts.tv_nsec/1000;
}
static void serial_log_cost(const char *line)
{
    /* Default UART: divisor 3, 38400 baud, 8N1, plus CR/LF per line.
     * Inject elapsed cost, never sleep or claim a physical measurement. */
    CHECK(!probing && !locks);
    uint64_t bytes=strlen(line)+2, us=(bytes*10000000+38399)/38400;
    simulated_bytes+=bytes; simulated_log_us+=us; m.us+=us;
    if (!strncmp(line,"[accel] savage step ",20) || !strncmp(line,"[accel] savage init ",20) ||
        !strncmp(line,"[accel] savage readback ",24)) simulated_trace_us+=us;
}
static uint64_t log_number(const char *line,const char *field)
{
    const char *at=strstr(line,field); CHECK(at!=NULL);
    return at ? strtoull(at+strlen(field),NULL,10) : 0;
}
static void activation_timing(void)
{
    for (unsigned variant=0;variant<3;variant++) {
        boot_fixture_accel(false);
        m.exact_clock=true; m.decorated=true; m.locked=variant==0;
        g_cpu_tsc=true; g_tsc_per_ms=1000000;
        marker_cost_us=8000;
        simulated_trace_us=simulated_bytes=simulated_log_us=0;
        accel_log_cost=serial_log_cost;
        if (variant==2) g_boot.flags=0;
        uint64_t host_start=host_time_us();
        fbdev_accel_schedule(&settings_vfs); run_accel_init();
        uint64_t host_us=host_time_us()-host_start;
        CHECK(log_matches("phase=marker_write us=8000 error=0")==1);
        if (variant) CHECK(log_matches("phase=marker_clear us=8000 error=0")==1);
        const char *terminal=NULL;
        for (unsigned i=0;i<accel_count && i<ACCEL_LOG_CAP;i++)
            if (strstr(accel_log[i],variant ? "savage ready us=" : "quarantined reason=crtc_locked us=")) terminal=accel_log[i];
        CHECK(terminal!=NULL);
        uint64_t us=terminal ? log_number(terminal," us=") : 0;
        CHECK(terminal && log_number(terminal," trace_us=")==simulated_trace_us);
        if (!variant) CHECK(!m.status_reads && !m.delays && us>=8000+simulated_trace_us);
        if (variant==2) CHECK(m.delays==5 && engine.reset_us>=20000 && engine.poll_us==15 && simulated_trace_us>0);
        /* Clock conversion and tick fallback both execute production code. */
        CHECK(hw_now(NULL)==m.us);
        g_cpu_tsc=false; g_ticks=7; CHECK(hw_now(NULL)==7000);
        printf("savage timing model variant=%u activation_us=%llu trace_us=%llu reset_us=%llu poll_us=%llu serial_bytes=%llu serial_us=%llu marker_us=%u host_us=%llu\n",
            variant,(unsigned long long)us,(unsigned long long)simulated_trace_us,
            (unsigned long long)engine.reset_us,(unsigned long long)engine.poll_us,
            (unsigned long long)simulated_bytes,(unsigned long long)simulated_log_us,marker_cost_us,(unsigned long long)host_us);
        accel_log_cost=NULL; marker_cost_us=0;
        finish();
    }
}
static void initial_idle_diagnostics(void)
{
    for (unsigned variant=0;variant<9;variant++) {
        boot_fixture_accel(false); g_boot.flags |= CBI_F_DIAG_VERBOSE; m.exact_clock=true;
        if (!variant) binding_gpu.device=0x8a22; /* unchanged Savage4 fake */
        else m.status_value=0xfffe0000; /* idle plus ignored SuperSavage bits */
        if (variant==2) {
            m.idle_after_us=18000;
            simulated_trace_us=simulated_bytes=simulated_log_us=0;
            accel_log_cost=serial_log_cost;
        }
        if (variant==3) m.status_value=0xff7e0000; /* complete Rev-B layout */
        if (variant==4) m.status_value=0xfffe0001; /* occupied FIFO is still busy */
        if (variant==5) m.status_value=0xffbe0000; /* Linux idle without X.Org bit 22 */
        if (variant==6) m.status_value=UINT32_MAX;
        if (variant==7) { m.stuck=true; m.frozen=true; }
        if (variant==8) m.idle_after_us=21000;
        fbdev_accel_schedule(&settings_vfs); run_accel_init();
        bool ready=variant<4;
        CHECK(!!acceleration==ready && engine.quarantined==!ready);
        CHECK(!table_open && !marker_open && !locks && !initializing);
        CHECK(marker_commits==2 && disk_size==(ready ? 0u : 30u));
        uint32_t raw=variant==7 ? 0 : variant==8 ? 0xfffe0001 : m.status_value ? m.status_value : 0x00e00000;
        CHECK(engine.status==raw && engine.status_logged);
        unsigned samples=0;
        for (unsigned i=0;i<accel_count && i<ACCEL_LOG_CAP;i++) {
            const char *line=accel_log[i];
            if (!strstr(line,"savage status sample=")) continue;
            CHECK(log_number(line,"sample=")==samples);
            CHECK(log_number(line," us=")== (variant==7 ? 0u : samples*1000u));
            const char *at=strstr(line,"raw="); CHECK(at!=NULL);
            uint32_t expected=variant==2 ? 0xfffe0001 : raw;
            CHECK(at && strtoul(at+4,NULL,16)==expected);
            samples++;
        }
        CHECK(samples==((variant==2 || variant==4 || variant==5 || variant==7 || variant==8) ? 8u : 1u));
        if (ready) {
            CHECK(engine.idle_mask==(variant==3 ? 0x00cfffffu : 0x00e1ffffu));
            CHECK(engine.idle_value==(variant==3 ? 0x004e0000u : 0x00e00000u));
            /* Status transport records are outside every polling deadline.
             * Only the initial wait and marker command's busy reads count. */
            CHECK(engine.poll_us==(variant==2 ? 18015u : 15u));
            CHECK(m.status_reads==(variant==2 ? 24u : 6u));
            unsigned initial_samples=log_matches("savage status sample=");
            software_desktop();
            CHECK(engine.operations>0 && log_matches("savage status sample=")==initial_samples);
        } else {
            CHECK(!engine.touched && !m.engine_writes && !m.packets);
            CHECK(m.status_reads==(variant==6 ? 1u : INITIAL_POLLS));
            CHECK(m.us-m.first_status_us>= (variant==6 ? 0u : WAIT_US));
            char status[64]; snprintf(status,sizeof(status),"status=%08x status_valid=1",raw);
            CHECK(log_matches(status)>=2); /* terminal and periodic quarantine */
            snprintf(status,sizeof(status),"accel=attempt\nstatus=%08x\n",raw);
            CHECK(!memcmp(disk_marker,status,30));
            software_desktop();
        }
        printf("savage initial idle variant=%u ready=%u reads=%u samples=%u status=%08x poll_us=%llu\n",
            variant,ready,m.status_reads,samples,engine.status,(unsigned long long)engine.poll_us);
        accel_log_cost=NULL; finish();
    }
    /* An annotated attempt blocks a read-only reboot without any bus access. */
    boot_fixture_accel(true); settings_volume.readonly=true;
    fbdev_accel_schedule(&settings_vfs); run_accel_init();
    CHECK(!acceleration && !binding_claims && !m.status_reads && disk_size==30 && !marker_commits);
    CHECK(log_matches("quarantined reason=previous_attempt")==1);
    software_desktop(); finish();

    /* Every diagnostic-storage error retains the original durable marker;
     * it never polls/reset/reclaims the quarantined engine to retry. */
    for (unsigned failure=1;failure<=6;failure++) {
        boot_fixture_accel(false); m.stuck=true; m.exact_clock=true;
        fbdev_accel_schedule(&settings_vfs); marker_thread_step();
        CHECK(disk_size==14 && marker_commits==1 && accel_marker_request==ACCEL_MARKER_INIT);
        marker_thread_step();
        CHECK(accel_marker_request==ACCEL_MARKER_STATUS && accel_queued && !locks);
        software_desktop();
        marker_fault=failure;
        /* Diagnostic updates do not stat: an open failure exercises that
         * boundary instead of the begin-only stat failure. */
        if (failure==2) marker_error=true;
        unsigned reads=m.status_reads;
        marker_thread_step();
        CHECK(!accel_queued && !table_open && !marker_open && !locks && engine.quarantined);
        CHECK(m.status_reads==reads && !m.engine_writes && !m.packets);
        CHECK(disk_size==(failure==6 ? 30u : 14u) && marker_commits==(failure==6 ? 2u : 1u));
        CHECK(log_matches("phase=marker_status")==1 && log_matches("error=-")==1);
        finish();
    }
    binding_allowed=false; binding_pci_count=0; accel_vfs=NULL;
}
static unsigned setup_at(uint32_t reg, uint32_t value, unsigned begin)
{
    for (unsigned i=begin;i<m.setup_count && i<ARRAY_SIZE(m.setup);i++)
        if (m.setup[i].reg==reg && m.setup[i].value==value) return i;
    CHECK(false);
    return ARRAY_SIZE(m.setup);
}
static void supersavage_window_and_records(void)
{
    for (unsigned variant=0;variant<5;variant++) {
        boot_fixture_accel(false);
        g_boot.flags=0; /* paired records must survive a nonverbose boot */
        m.strict_window=true; m.gbd_reserved=true;
        m.alias_dead=true; /* the operational ports remain authoritative */
        m.cr[0x31]=0xe1; m.cr[0x50]=0x30; m.reg[0x816c/4]=0xffffffff;
        if (variant==1) m.window_locked=true;
        if (variant==2) m.bad_readback=true;
        if (variant==3) m.ignore_fill=true;
        if (variant==4) m.fail_after_submit=true;
        CHECK(!window_live(&m)); /* 2D bank starts disabled despite live status */
        uint8_t before[4];
        uint8_t *last=device.mapped+(device.height-1)*device.pitch+(device.width-1)*4;
        memcpy(before,last,sizeof(before));
        fbdev_accel_schedule(&settings_vfs); run_accel_init();
        CHECK(!m.dword_2d_reads && m.half_reads>=8 && m.half_writes==2);
        CHECK(!step_count && log_matches("savage init reg=")<=40);
        if (variant) {
            CHECK(log_matches("init reg=00038 wrote=00000048 read=00000048 bank=cr width=8")==1);
            CHECK(log_matches("init reg=00039 wrote=000000a0 read=000000a0 bank=cr width=8")==1);
            CHECK(log_matches("init reg=00040 wrote=00000001 read=00000001 bank=cr width=8")==2);
            CHECK(log_matches("init reg=00031 wrote=0000000c read=0000000c bank=cr width=8")==1);
            CHECK(log_matches("init reg=00050 wrote=000000f1 read=000000f1 bank=cr width=8")==1);
            CHECK(log_matches("init reg=00008 wrote=00000006 read=00000006 bank=sr width=8")==1);
            CHECK(log_matches("init reg=00026 wrote=00000040 read=00000040 bank=sr width=8")==1);
            CHECK(log_matches("init reg=003c3 ")==1 && log_matches("init reg=003c2 ")==2);
            CHECK(log_matches("init reg=00066 ")==2);
            CHECK(log_matches("init reg=48c18 ")==2 && log_matches("init reg=48c10 ")==1 && log_matches("init reg=48c0c ")==1);
            CHECK(log_matches("init reg=08128 ")==1 && log_matches("init reg=0812c ")==1);
            CHECK(log_matches("init reg=08134 ")==1 && log_matches("init reg=08136 ")==1);
            CHECK(log_matches("init reg=08168 ")==1 && log_matches("init reg=0816c ")==2);
        } else CHECK(log_matches("savage init reg=")==init_count);
        /* These engine-only writes must retain upstream values and order.
         * No CRTC timing, DAC, primary stream or tiling write is permitted. */
        unsigned at=setup_at(0x100038,0x48,0);
        at=setup_at(0x100039,0xa0,at+1);
        at=setup_at(0x200008,6,at+1);
        at=setup_at(0x200026,0x40,at+1);
        at=setup_at(0x100066,3,at+1);
        at=setup_at(0x100066,1,at+1);
        at=setup_at(0x100040,1,at+1);
        at=setup_at(0x100031,0x0c,at+1);
        at=setup_at(BCI_CONTROL,0,at+1);
        at=setup_at(0x8128,UINT32_MAX,at+1);
        at=setup_at(0x812c,UINT32_MAX,at+1);
        at=setup_at(0x8134,0x27,at+1);
        at=setup_at(0x8136,7,at+1);
        at=setup_at(0x48c10,0x00700040,at+1);
        at=setup_at(0x48c0c,0,at+1);
        at=setup_at(BCI_CONTROL,8,at+1);
        at=setup_at(0x816c,0x10000009,at+1);
        at=setup_at(0x100050,0xf1,at+1);
        at=setup_at(0x8168,0,at+1);
        setup_at(0x816c,engine.descriptor|9,at+1);
        for (unsigned i=0;i<m.setup_count && i<ARRAY_SIZE(m.setup);i++) {
            uint32_t reg=m.setup[i].reg;
            CHECK(reg==0x100038 || reg==0x100039 || reg==0x100040 || reg==0x100031 ||
                reg==0x100066 || reg==0x100050 || reg==0x100045 || reg==0x200008 || reg==0x200026 ||
                reg==BCI_CONTROL || reg==0x48c10 || reg==0x48c0c ||
                reg==0x8128 || reg==0x812c || reg==0x8134 || reg==0x8136 || reg==0x8168 || reg==0x816c);
        }
        CHECK(m.setup_count<ARRAY_SIZE(m.setup));
        if (!variant) {
            CHECK(acceleration && engine.active && !engine.quarantined && engine.operations==1);
            CHECK(m.packets==1 && !m.dead_window_writes && m.postreset_enable);
            CHECK((m.reg[0x816c/4]&GBD_READ_MASK)==(engine.descriptor|9));
            CHECK(log_matches("bci_test result=ok error=0")==1 && log_matches("read_valid=1 restored=1")==1);
            char raw[32]; snprintf(raw,sizeof(raw),"read=%08x",(engine.descriptor|9)|~GBD_READ_MASK);
            CHECK(log_matches(raw)>=1); /* raw reserved decorations remain visible */
            CHECK(!memcmp(last,before,sizeof(before)) && disk_size==0);
            /* Demonstrate why a dword equality check is a false rejection. */
            CHECK(model_read(&m,0x816c)!=(engine.descriptor|9));
            CHECK(read_2d(&engine,0x816c)!=(engine.descriptor|9));
            CHECK((read_2d(&engine,0x816c)&GBD_READ_MASK)==(engine.descriptor|9));
        } else {
            CHECK(engine.quarantined && !engine.active && disk_size==30);
            CHECK(log_matches(variant<3 ? "descriptor_readback_failed" : variant==3 ? "bci_test_readback_failed" : "idle_timeout")>=1);
            if (variant<3) {
                CHECK(!acceleration && !m.packets && !engine.submitted);
                CHECK(log_matches("bci_test result=skipped")==1);
                CHECK(!memcmp(last,before,sizeof(before)));
                software_desktop();
            } else {
                CHECK(acceleration==(variant==3 ? NULL : &engine.ops) && engine.submitted && m.packets==1);
                CHECK(log_matches("bci_test result=failed error=-5")==1);
                if (variant==3) {
                    CHECK(engine.cpu_safe && !memcmp(last,before,sizeof(before)));
                    CHECK(log_matches("read_valid=1 restored=1")==1);
                    software_desktop();
                } else {
                    CHECK(!engine.cpu_safe && !fbdev_console_begin());
                    CHECK(log_matches("read_valid=0 restored=0")==1);
                    uint8_t frame[64*512]; memcpy(frame,device.mapped,device.size);
                    struct fb_rect r={16,16,10,10};
                    CHECK(fbdev_fill(&r,0x123456)==-EIO);
                    CHECK(!memcmp(frame,device.mapped,device.size) && m.packets==1);
                    CHECK(engine.ops.stop(&engine)==-EIO); /* no idle proof for firmware takeover */
                    unsigned reads=m.status_reads;
                    m.stuck=false;
                    CHECK(fbdev_fill(&r,0x123456)==-EIO && m.status_reads==reads);
                    CHECK(!engine.cpu_safe && m.packets==1 && engine.quarantined);
                }
            }
            CHECK(!memcmp(disk_marker,"accel=attempt\nstatus=",21));
        }
        printf("f4-19 SuperSavage variant=%u records=%u commands=%u quarantined=%u\n",
            variant,log_matches("savage init reg="),m.packets,engine.quarantined);
        finish();
    }
    binding_allowed=false; binding_pci_count=0; accel_vfs=NULL;
}
static void halfword_glue_and_16bit_marker(void)
{
    prepare(16);
    uint8_t *last=device.mapped+(device.height-1)*device.pitch+(device.width-1)*2;
    uint8_t before[2]; memcpy(before,last,sizeof(before));
    CHECK(!bci_test(&test_engine,&device) && !memcmp(before,last,sizeof(before)));
    CHECK(m.packets==1 && test_engine.operations==1 && test_engine.cpu_safe);
    CHECK(!test_engine.ops.stop(&test_engine)); finish();

    /* Actual production UC halfword accessors, backed by a byte window. */
    registers=binding_window;
    hw_write16(NULL,0x8134,0x27); hw_write16(NULL,0x8136,7);
    CHECK(hw_read16(NULL,0x8134)==0x27 && hw_read16(NULL,0x8136)==7);
    hw_write(NULL,0x816c,0x10200401);
    CHECK(hw_read16(NULL,0x816c)==0x401 && hw_read16(NULL,0x816e)==0x1020);
    registers=NULL;
    CHECK(hw_read16(NULL,0x816c)==UINT16_MAX);
}
static void engine_depth_and_trace_thread(void)
{
    /* Reproduce the exact observed T23 marker failure with the old CR50
     * value despite a correct 32-bpp command and global descriptor. */
    for (unsigned field=0;field<2;field++) {
        prepare(32);
        uint8_t *last=device.mapped+(device.height-1)*device.pitch+(device.width-1)*4;
        uint32_t before=0x0010243c;
        memcpy(last,&before,4);
        if (!field) m.cr[0x50]&=~0x30u;
        else m.reg[0x816c/4]=(m.reg[0x816c/4]&~0xff0000u)|0x80000u;
        accel_count=0;
        CHECK(bci_test(&test_engine,&device)==-EIO);
        CHECK(log_matches("wrote=004a8166 read=66666666 read_valid=1 restored=1")==1);
        CHECK(test_engine.quarantined && test_engine.cpu_safe && m.packets==1);
        CHECK(!memcmp(last,&before,4));
        finish();
    }
    for (unsigned bpp=16;bpp<=32;bpp+=16) for (unsigned firmware=0;firmware<3;firmware++) {
        boot_fixture_mode(false,bpp);
        m.strict_window=true; m.gbd_reserved=true; m.exact_clock=true;
        uint8_t saved50=(firmware==0 ? 0 : firmware==1 ? 0x10 : 0x30)|0x0e;
        m.cr[0x50]=saved50;
        uint8_t saved67=bpp==16 ? 0x59 : 0xd9;
        m.cr[0x67]=saved67;
        m.reg[0x8168/4]=0x1000; m.reg[0x816c/4]=0x10080401; /* stale GBD too */
        uint8_t *last=device.mapped+(device.height-1)*device.pitch+(device.width-1)*(bpp/8);
        uint32_t before=bpp==32 ? 0x0010243c : 0x1234;
        memcpy(last,&before,bpp/8);
        simulated_trace_us=simulated_bytes=simulated_log_us=0;
        accel_log_cost=serial_log_cost;
        fbdev_accel_schedule(&settings_vfs); run_accel_init();
        uint8_t expected50=(saved50&~0x30u)|(bpp==16 ? 0x10 : 0x30)|0xc1;
        CHECK(acceleration && engine.active && !engine.quarantined && disk_size==0);
        CHECK(m.cr[0x50]==expected50 && m.cr[0x67]==saved67 && !m.port_cr_reads[0x67]);
        CHECK(m.reg[0x8168/4]==0 && (m.reg[0x816c/4]&GBD_READ_MASK)==(engine.descriptor|9));
        CHECK(log_matches(bpp==32 ? "wrote=004a8166 read=004a8166" : "wrote=0000b76e read=0000b76e")==1);
        CHECK(log_matches("bci_test result=ok error=0")==1 && !memcmp(last,&before,bpp/8));
        char record50[100];
        snprintf(record50,sizeof(record50),"init reg=00050 wrote=%08x read=%08x bank=cr width=8",expected50,expected50);
        CHECK(log_matches(record50)==1 && log_matches("savage init reg=")==init_count && !step_count);
        CHECK(normal_yields>=20);
        CHECK(!device_work_requests && simulated_trace_us>0);
        unsigned records=step_count+log_matches("savage init reg="), yields=normal_yields;
        struct fb_rect r={0,0,1,1};
        CHECK(!fbdev_fill(&r,0x4a8166) && engine.operations==2);
        CHECK(step_count+log_matches("savage init reg=")==records && normal_yields==yields);
        /* Exercise the actual reset pacing glue as well as the model delay:
         * 10 ms becomes twenty <=500-us slices on this same normal task. */
        marker_thread_running=initializing=true;
        accel_initializing=true;
        CHECK(!hw_delay(NULL,10000) && normal_yields==yields+20 && !device_work_requests);
        accel_initializing=false;
        marker_thread_running=initializing=false;
        CHECK(!acceleration->stop(acceleration->ctx) && m.cr[0x50]==saved50 && m.cr[0x67]==saved67);
        printf("f4-22 depth=%u firmware_cr50=%02x programmed=%02x trace_us=%llu normal_yields=%u device_work_requests=%u\n",
            bpp,saved50,expected50,(unsigned long long)simulated_trace_us,normal_yields,device_work_requests);
        accel_log_cost=NULL;
        finish();
    }
    boot_fixture_accel(false); m.depth_locked=true;
    fbdev_accel_schedule(&settings_vfs); run_accel_init();
    CHECK(!acceleration && engine.quarantined && !m.packets && disk_size==30);
    CHECK(log_matches("quarantined reason=depth_readback_failed")==1);
    CHECK(log_matches("init reg=00050 wrote=000000f1 read=000000c1 bank=cr width=8")==1);
    software_desktop(); finish();
    /* A firmware switch or nonwaiting console between BEGIN and INIT must
     * retain the marker without touching hardware or claiming registers. */
    for (unsigned blocked=0;blocked<2;blocked++) {
        boot_fixture_accel(false);
        fbdev_accel_schedule(&settings_vfs); marker_thread_step();
        if (!blocked) changing=true;
        else CHECK(fbdev_console_begin());
        marker_thread_step();
        CHECK(accel_marker_request==ACCEL_MARKER_STATUS && !acceleration);
        CHECK(!blocked ? (!binding_claims && !m.port_reads) : (binding_claims==2 && m.port_reads>0 && !m.packets));
        if (!blocked) changing=false;
        else { CHECK(log_matches("quarantined reason=bci_test_busy")==1); fbdev_console_end(); }
        marker_thread_step();
        CHECK(!accel_queued && disk_size==(!blocked ? 14u : 30u) && marker_commits==(!blocked ? 1u : 2u) && !device_work_requests);
        software_desktop(); finish();
    }
    binding_allowed=false; binding_pci_count=0; accel_vfs=NULL;
}
/* Firmware entry must never happen while setup pins the current mapping. */
void console_framebuffer_revoke(void) { CHECK(false); }
void console_framebuffer_changed(const struct fb_device *d) { (void)d; CHECK(false); }
static unsigned setup_frames, setup_consoles;
static uint64_t first_setup_frame_us;
static void render_during_setup(void)
{
    CHECK(accel_initializing && !locks && !probing);
    unsigned reads=m.status_reads, packets=m.packets;
    uint64_t waits=engine.idle_waits;
    if (!setup_frames) {
        uint32_t frame[96*64];
        for (unsigned i=0;i<ARRAY_SIZE(frame);i++) frame[i]=0x0010243c;
        struct fb_surface first={frame,96,64,96*4,sizeof(frame)};
        struct fb_rect full={0,0,96,64};
        CHECK(!fbdev_present(&first,&full));
    }
    struct fb_rect r={16,16,1,1};
    uint32_t colour=0x00123456;
    struct fb_surface surface={&colour,1,1,4,4};
    CHECK(!fbdev_present(&surface,&r));
    CHECK(!fbdev_fill(&r,0xabcdef));
    struct ciuki_accel_request q=request(CIUKI_ACCEL_QUERY);
    CHECK(!fbdev_accel_request(&q));
    struct ciuki_accel_info info;
    CHECK(!fbdev_accel_info(device.generation,&info) && !info.driver && !info.capabilities);
    CHECK(fbdev_console_begin()); fbdev_console_end(); setup_consoles++;
    CHECK(fbdev_change_begin()==-EBUSY && !changing);
    CHECK(m.status_reads==reads && m.packets==packets && engine.idle_waits==waits);
    if (!setup_frames++) first_setup_frame_us=m.us;
}
static void gbd_idle_and_async_regressions(void)
{
    for (unsigned failure=0;failure<3;failure++) {
        boot_fixture_accel(false);
        m.exact_clock=true; m.strict_window=true;
        if (failure==1) m.ignore_fill=true;
        if (failure==2) m.corrupt_high=true;
        uint32_t before=0x0010243c;
        uint8_t *last=device.mapped+(device.height-1)*device.pitch+(device.width-1)*4;
        memcpy(last,&before,4);
        setup_frames=setup_consoles=0; first_setup_frame_us=0;
        init_interleave=render_during_setup;
        simulated_trace_us=simulated_bytes=simulated_log_us=0;
        accel_log_cost=serial_log_cost;
        fbdev_accel_schedule(&settings_vfs); run_accel_init();
        init_interleave=NULL;
        CHECK(setup_frames>=20 && setup_consoles==setup_frames && first_setup_frame_us<1000000);
        CHECK(m.packets==1 && m.packet[0]==0x4bcc8000 && m.packet[1]==0x004a8166);
        CHECK(m.packet[2]==((device.height-1)<<16 | (device.width-1)) && m.packet[3]==0x00010001);
        CHECK(!memcmp(last,&before,4));
        if (failure) {
            CHECK(!acceleration && engine.quarantined && engine.cpu_safe && engine.final_idle_checked);
            CHECK(disk_size==30 && binding_resources[1].state==RS_QUARANTINED && binding_resources[2].state==RS_QUARANTINED);
            CHECK(log_matches("bci_test_readback_failed")>=1 && log_matches("savage init reg=")<=40);
            if (failure==2) CHECK(log_matches("read=664a8166 read_valid=1 restored=1")==1);
        } else {
            CHECK(acceleration && !engine.quarantined && log_matches("savage init reg=")==init_count && simulated_trace_us>0);
            CHECK(log_matches("wrote=004a8166 read=004a8166")==1);
        }
        unsigned reads=m.status_reads, packets=m.packets;
        uint64_t waits=engine.idle_waits;
        for (unsigned i=0;i<100;i++) {
            uint32_t pixel=0x00112233;
            struct fb_rect point={16,16,1,1};
            struct fb_surface surface={&pixel,1,1,4,4};
            CHECK(!fbdev_present(&surface,&point));
            if (failure) CHECK(!fbdev_fill(&point,pixel));
            CHECK(fbdev_console_begin()); fbdev_console_end();
        }
        CHECK(engine.idle_waits==waits && m.status_reads==reads && m.packets==packets);
        if (!failure) {
            uint64_t ops=engine.operations;
            struct fb_rect point={16,16,1,1};
            CHECK(!fbdev_fill(&point,0x123456) && engine.operations==ops+1 && engine.idle_waits==waits+1);
        }
        printf("f4-26 failure=%u setup_frames=%u first_frame_model_us=%llu cpu_presents=100 idle_waits_delta=0 trace_records=%u\n",
            failure,setup_frames,(unsigned long long)first_setup_frame_us,log_matches("savage init reg="));
        accel_log_cost=NULL; finish();
    }
    /* A failed completion gets exactly one final proof. Recovery then
     * permits plain software; a failed proof remains a cached refusal even
     * if the hardware later reports idle. */
    for (unsigned recovered=0;recovered<2;recovered++) {
        boot_fixture_accel(false); g_boot.flags |= CBI_F_DIAG_VERBOSE; m.exact_clock=true; m.fail_after_submit=true;
        m.recover_on_final=recovered;
        fbdev_accel_schedule(&settings_vfs); run_accel_init();
        CHECK(engine.quarantined && engine.cpu_safe==!!recovered && engine.final_idle_checked);
        CHECK(acceleration==(recovered ? NULL : &engine.ops));
        CHECK(engine.idle_waits==4); /* initial, setup, command, final proof */
        unsigned reads=m.status_reads;
        uint64_t waits=engine.idle_waits;
        uint32_t pixel=0x112233;
        struct fb_surface surface={&pixel,1,1,4,4};
        struct fb_rect point={16,16,1,1};
        m.stuck=false;
        for (unsigned i=0;i<100;i++) {
            CHECK(fbdev_present(&surface,&point)==(recovered ? 0 : -EIO));
            CHECK(fbdev_fill(&point,pixel)==(recovered ? 0 : -EIO));
            if (recovered) {
                CHECK(fbdev_console_begin()); fbdev_console_end();
                CHECK(!savage2d_fallback_ops());
            } else {
                CHECK(!fbdev_console_begin() && acceleration->stop(acceleration->ctx)==-EIO);
                CHECK(!savage2d_fallback_ops()->cpu_ready(&engine));
            }
        }
        CHECK(m.status_reads==reads && engine.idle_waits==waits && m.packets==1 && disk_size==30);
        finish();
    }
    binding_allowed=false; binding_pci_count=0; accel_vfs=NULL;
}
static void queued_gbd_depth(void)
{
    /* Reapply the old MMIO-only setup with a stale BCI depth. Readback
     * still agrees, while the exact captured colour fails the pixel test. */
    prepare(32);
    uint8_t *last=device.mapped+(device.height-1)*device.pitch+(device.width-1)*4;
    uint32_t before=0x00dce4ec;
    memcpy(last,&before,4);
    m.bci_gbd[1]=(test_engine.descriptor&~0xff0000u)|0x00080001u;
    unsigned registers_sent=m.register_packets;
    model_write(&m,0x8168,0);
    model_write(&m,0x816c,test_engine.descriptor|9u);
    CHECK((read_2d(&test_engine,0x816c)&GBD_READ_MASK)==(test_engine.descriptor|9u));
    accel_count=0;
    CHECK(bci_test(&test_engine,&device)==-EIO && test_engine.quarantined);
    CHECK(log_matches("wrote=008641b6 read=b6b6b6b6 read_valid=1 restored=1")==1);
    CHECK(!memcmp(last,&before,4) && m.register_packets==registers_sent);
    CHECK(test_engine.cpu_safe && !test_engine.ops.capabilities(&test_engine));
    printf("f1-65 MMIO-only depth=32 wrote=008641b6 read=b6b6b6b6 restored=1 quarantined=1\n");
    finish();

    for (unsigned bpp=16;bpp<=32;bpp+=16) for (unsigned variant=0;variant<5;variant++) {
        boot_fixture_mode(false,bpp);
        m.strict_window=true; m.gbd_reserved=true; m.exact_clock=true;
        /* A different stale offset/depth must be replaced by the queued
         * descriptor, not inherited from readable MMIO or firmware. */
        m.bci_gbd[0]=0x1000;
        m.bci_gbd[1]=0x10080401;
        m.ignore_first_gbd=variant==1;
        m.ignore_gbd=variant==2;
        m.fail_after_gbd=variant>=3 ? 4 : 0;
        m.recover_on_final=variant==4;
        m.gbd_busy=true;
        last=device.mapped+(device.height-1)*device.pitch+(device.width-1)*(bpp/8);
        before=bpp==32 ? 0x00dce4ec : 0xe4ec;
        memcpy(last,&before,bpp/8);
        uint8_t *frame=malloc(device.size); CHECK(frame!=NULL);
        memcpy(frame,device.mapped,device.size);
        fbdev_accel_schedule(&settings_vfs); run_accel_init();
        CHECK(m.register_packets==4 && !m.words);
        for (unsigned i=0;i<4;i++) {
            CHECK(m.gbd_history[i][0]==((i&1) ? 0x960100e1u : 0x960100e0u));
            CHECK(m.gbd_history[i][1]==((i&1) ? (engine.descriptor|9u) : 0));
        }
        CHECK(!memcmp(frame,device.mapped,device.size));
        free(frame);
        CHECK(engine.submitted && m.packets==(variant>=3 ? 0u : 1u));
        CHECK(engine.operations==(variant>=3 ? 0u : 1u));
        bool ready=variant<2;
        CHECK(engine.quarantined==!ready && engine.active==ready);
        CHECK(!!engine.ops.capabilities(&engine)==ready);
        if (ready) {
            CHECK(acceleration==&engine.ops && !disk_size && log_matches("savage init reg=")==init_count);
            CHECK(m.bci_gbd[0]==0 && m.bci_gbd[1]==(engine.descriptor|9u));
            CHECK(log_matches(bpp==32 ? "wrote=008641b6 read=008641b6" : "wrote=000041b6 read=000041b6")==1);
            CHECK(log_matches("bci_test result=ok error=0")==1);
        } else {
            CHECK(disk_size==30 && binding_resources[1].state==RS_QUARANTINED &&
                binding_resources[2].state==RS_QUARANTINED);
            CHECK(log_matches(variant==2 ? "bci_test_readback_failed" : "idle_timeout")>=1);
            CHECK(log_matches(variant>=3 ? "bci_test result=skipped" : "bci_test result=failed")==1);
            CHECK(engine.cpu_safe==(variant!=3));
            CHECK(acceleration==(variant==3 ? &engine.ops : NULL));
            if (variant==2)
                CHECK(log_matches(bpp==32 ? "read=b6b6b6b6 read_valid=1 restored=1" : "read=0000b6b6 read_valid=1 restored=1")==1);
            unsigned reads=m.status_reads, packets=m.packets, sent=m.register_packets;
            struct fb_rect point={16,16,1,1};
            CHECK(fill_rect(&engine,&point,0x123456)==-ENODEV);
            CHECK(fbdev_fill(&point,0x123456)==(variant==3 ? -EIO : 0));
            CHECK(m.status_reads==reads && m.packets==packets && m.register_packets==sent);
        }
        printf("f1-65 queued depth=%u variant=%u register_commands=%u draws=%u caps=%u cpu_safe=%u quarantined=%u\n",
            bpp,variant,m.register_packets,m.packets,engine.ops.capabilities(&engine),engine.cpu_safe,engine.quarantined);
        finish();
    }
    binding_allowed=false; binding_pci_count=0; accel_vfs=NULL;
}
static void expect_readback(const char *bank, uint32_t reg, unsigned width, uint32_t wrote, uint32_t read)
{
    char line[128];
    snprintf(line,sizeof(line),"savage readback reg=%05x wrote=%08x read=%08x bank=%s width=%u",
        reg,wrote,read,bank,width);
    CHECK(log_matches(line)==1);
}
static void shared_enable_and_failure_readbacks(void)
{
    for (unsigned bpp=16;bpp<=32;bpp+=16) {
        unsigned setup_half_reads=0;
        for (unsigned variant=0;variant<5;variant++) {
            boot_fixture_mode(false,bpp);
            free(storage);
            device.width=1024; device.height=768; device.pitch=1024*(bpp/8);
            device.size=device.height*device.pitch;
            device.mode_id=bpp==16 ? 0x117 : 0x118;
            storage=malloc(device.size+32); CHECK(storage!=NULL);
            memset(storage,0xa5,device.size+32); device.mapped=storage+16;
            g_boot.flags=variant&1u ? CBI_F_DIAG_VERBOSE : 0;
            m.exact_clock=m.strict_window=m.gbd_reserved=m.require_gbd_enable=true;
            m.late_setup_fault=variant<4 ? variant : 0;
            m.fail_after_submit=variant==4;
            /* Preserve firmware's retry and primary/secondary shadow-load
             * controls, plus the stream addresses, stride and mode. */
            m.cr[0x66]=0xb9; m.cr[0x67]=bpp==16 ? 0x59 : 0xd9;
            m.reg[0x81c0/4]=0x10000; m.reg[0x81c4/4]=0x80000000; m.reg[0x81c8/4]=device.pitch;
            m.reg[BCI_CONTROL/4]=0x1230; /* defined preserved counters */
            uint8_t *last=device.mapped+device.size-bpp/8;
            uint32_t before=bpp==32 ? 0x00dce4ec : 0xe4ec;
            memcpy(last,&before,bpp/8);
            fbdev_accel_schedule(&settings_vfs); run_accel_init();
            uint32_t gbd=bpp==16 ? 0x10100409u : 0x10200409u;
            uint8_t cr50=bpp==16 ? 0xd1 : 0xf1;
            CHECK(engine.descriptor==(gbd&~9u) && m.register_packets==4 && m.packets==1);
            CHECK(log_matches("savage init reg=")==init_count && init_count<INIT_RECORDS);
            CHECK(log_matches("savage probe pci=")==1 && log_matches("savage mapped mmio=")==1);
            CHECK(log_matches(variant ? "caps=0 init_result=-5" : "caps=1 init_result=0")==1);
            CHECK(m.cr[0x66]==0xb9 && m.cr[0x67]==(bpp==16 ? 0x59 : 0xd9) && !m.port_cr_reads[0x67]);
            CHECK(m.reg[0x81c0/4]==0x10000 && m.reg[0x81c4/4]==0x80000000 && m.reg[0x81c8/4]==device.pitch);
            unsigned at=setup_at(BCI_CONTROL,0x1230,0);
            at=setup_at(0x48c10,0x00700040,at+1);
            at=setup_at(0x48c0c,0,at+1);
            at=setup_at(BCI_CONTROL,0x1238,at+1);
            at=setup_at(0x816c,0x10000009,at+1);
            at=setup_at(0x100050,cr50,at+1);
            at=setup_at(0x8168,0,at+1);
            setup_at(0x816c,gbd,at+1);
            for (unsigned i=0;i<4;i++)
                CHECK(m.gbd_history[i][1]==((i&1u) ? gbd : 0));
            CHECK(!m.dword_2d_reads && !m.dead_window_writes);
            if (!variant) {
                setup_half_reads=m.half_reads;
                CHECK(acceleration && !engine.quarantined && engine.cpu_safe && !disk_size);
                CHECK(log_matches("bci_test result=ok error=0")==1 && !log_matches("savage readback "));
                CHECK(!memcmp(last,&before,bpp/8));
            } else {
                CHECK(engine.quarantined && !engine.active && disk_size==30);
                CHECK(!engine.ops.capabilities(&engine) && log_matches("bci_test result=failed error=-5")==1);
                if (variant==4) {
                    CHECK(acceleration==&engine.ops && !engine.cpu_safe && engine.final_idle_checked);
                    CHECK(m.half_reads==setup_half_reads); /* no diagnostic reads of a busy engine */
                    CHECK(log_matches("savage readback source=cached_init status=00000000 status_valid=1")==1);
                    CHECK(!log_matches("savage readback reg=") && !fbdev_console_begin());
                } else {
                    CHECK(!acceleration && engine.cpu_safe && !memcmp(last,&before,bpp/8));
                    CHECK(log_matches("savage readback source=idle_snapshot status=00e00000 status_valid=1")==1);
                    expect_readback("cr",0x31,8,0x0c,0x0c);
                    expect_readback("cr",0x40,8,1,1); /* captured before the restored zero */
                    expect_readback("cr",0x50,8,cr50,variant==3 ? cr50&~0x30u : cr50);
                    expect_readback("cr",0x66,8,0xb9,0xb9);
                    expect_readback("mmio",0x8128,32,UINT32_MAX,variant==3 ? 0xffu : UINT32_MAX);
                    expect_readback("mmio",0x812c,32,UINT32_MAX,UINT32_MAX);
                    expect_readback("mmio",0x8134,16,0x27,0x27);
                    expect_readback("mmio",0x8136,16,7,7);
                    expect_readback("mmio",0x48c10,32,0x00700040,0x00700040);
                    expect_readback("mmio",0x48c0c,32,0,0);
                    expect_readback("mmio",BCI_CONTROL,32,0x1238,0x1238);
                    expect_readback("mmio",0x8168,32,0,0);
                    uint32_t raw=(variant==1 ? gbd&~8u : gbd)|~GBD_READ_MASK;
                    expect_readback("mmio",0x816c,32,gbd,raw);
                    expect_readback("bci",BCI_GBD_LOW,32,0,0);
                    expect_readback("bci",BCI_GBD_HIGH,32,gbd,raw);
                    CHECK(!(m.cr[0x40]&1u));
                }
            }
            printf("f1-67 1024x768 depth=%u pitch=%u gbd=%08x variant=%u caps=%u readback=%s\n",
                bpp,device.pitch,gbd,variant,engine.ops.capabilities(&engine),
                !variant ? "none" : variant==4 ? "cached_init" : "idle_snapshot");
            finish();
        }
    }
    binding_allowed=false; binding_pci_count=0; accel_vfs=NULL;
}
int main(void)
{
    CHECK(!framebuffer_regression_main());
    CHECK(boot_calls==0); /* framebuffer activation never enters the engine */
    commands(16); commands(32); failures_and_fallback();
    glue_stalled_clock(0);            /* never idle, frozen clock */
    glue_stalled_clock(UINT32_MAX);   /* master-aborted (dead) aperture */
    deferred_boots(); async_marker(); optional_alias();
    port_bootstrap(); port_ownership();
    decorated_readbacks(); activation_timing(); initial_idle_diagnostics();
    supersavage_window_and_records();
    halfword_glue_and_16bit_marker();
    engine_depth_and_trace_thread();
    gbd_idle_and_async_regressions();
    queued_gbd_depth();
    shared_enable_and_failure_readbacks();
    for (int32_t id=-1;id<=96;id++) {
        registry_setting_present=true; registry_setting=id; load_config(&settings_vfs);
        CHECK(selected_config==(id<0 || id>=96 ? 0u : (unsigned)id));
    }
    registry_setting_present=false; load_config(&settings_vfs); CHECK(!selected_config);
    boot_fixture_accel(false); g_boot.flags|=CBI_F_HW_CHECK;
    bool retained=false;
    CHECK(!savage2d_check_begin(&settings_vfs,&retained) && !retained && disk_size==14);
    unsigned commits=marker_commits;
    CHECK(!savage2d_check_begin(&settings_vfs,&retained) && retained && marker_commits==commits && disk_size==14);
    engine.quarantined=true; CHECK(savage2d_check_begin(&settings_vfs,&retained)==-EIO);
    finish();
    puts("Savage hardware selection: all registry IDs/defaults, durable retained marker, no in-session quarantine bypass PASS");
    printf("savage2d register/presenter model: %s (%u failures)\n",failures ? "FAIL" : "PASS",failures);
    return failures ? 1 : 0;
}
