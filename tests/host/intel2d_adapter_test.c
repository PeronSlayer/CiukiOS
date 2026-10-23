/* Hardware/VFS boundary fixture for the real adapter, never privileged I/O.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define CIUKI_CPU_H
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <ciuki/timing.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)
static uint32_t pci_address;
static uint32_t irq_save(void) { return 0x200; }
static void irq_restore(uint32_t f) { CHECK(f==0x200); }
static void outl(uint16_t port, uint32_t value) { CHECK(port==0xcf8); pci_address=value; }
static uint32_t inl(uint16_t);
static void cpuid(uint32_t, uint32_t *, uint32_t *, uint32_t *, uint32_t *);
static void cpu_write_drain(void) { }
static unsigned wbinvd_calls;
static void wbinvd(void) { wbinvd_calls++; }
static uint32_t dma_memory[25*4096/4];
#define P2V(p) ((void *)dma_memory)
#define V2P(p) ((uint32_t)0x01000000)
#include "../../src/kernel/fs/vfs.h"
#include "../../src/kernel/drivers/video/intel2d/driver.c"

struct ciuki_boot_info g_boot;
volatile uint64_t g_ticks;
uint64_t g_tsc_per_ms;
bool g_cpu_tsc;
static struct pci_func functions[4];
static unsigned count;
static struct fb_device fb;
static uint8_t pixels[2560];
static uint32_t mmio[MMIO_BYTES/4], table[GTT_BYTES/4], flush_page[1024];
static struct resource resources[8];
static unsigned claims, releases, allocations, frees, pins, unpins, maps, revokes, owner_pages;
static unsigned cfg_writes, files, commits, closes, probe_calls;
static bool no_clflush, pin_fail, marker_present, marker_fail, commit_fail, close_fail;
static bool exercise_publish;
static struct fat_volume volume;
static struct vfs filesystem;
static uint32_t host_ggc, gfx_msac, host_ifp, host_drb, host_tseg;

static uint32_t inl(uint16_t port)
{
    CHECK(port==0xcfc);
    if (exercise_publish && wbinvd_calls) mmio[0x70/4]&=~(1u<<31);
    unsigned dev=(pci_address>>11)&31, reg=pci_address&252, fn=(pci_address>>8)&7;
    CHECK(!((pci_address>>16)&255));
    if (dev==0) {
        if (fn==1) {
            CHECK(functions[2].device==0x3584);
            return !reg ? 0x35848086u : reg==0x40 ? host_drb : 0;
        }
        CHECK(!fn);
        if (reg==0x50) return host_ggc;
        if (reg==0x60) return functions[1].device==0x2582 ? host_ifp :
            functions[1].device==0x2562 ? host_drb : host_tseg;
        if (reg==0x9c) return host_tseg;
        return (uint32_t)functions[0].device<<16|0x8086u;
    }
    CHECK(dev==2 && !fn);
    if (!reg) return (uint32_t)functions[1].device<<16|0x8086;
    if (reg==4) return functions[1].command;
    if (reg>=0x10 && reg<=0x24) return functions[1].bar[(reg-0x10)/4];
    if (reg==0x5c) return 0x08000000;
    if (reg==0x60) return gfx_msac;
    return 0;
}
static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{ CHECK(leaf==1); *a=*c=0; *b=8u<<8; *d=no_clflush ? 0 : 1u<<19; }
unsigned pci_count(void) { return count; }
const struct pci_func *pci_get(unsigned i) { return i<count ? functions+i : 0; }
bool display_accel_disabled(const char **why)
{ if (g_boot.flags&CBI_F_SAFE_MODE) { *why="safe_mode"; return true; } return false; }
const struct fb_device *fbdev_get(void) { return &fb; }
const struct resource *registry_get(unsigned i) { return i<8 ? &resources[i] : 0; }
bool registry_valid(int i, gen_t generation)
{ return i>=0 && i<8 && resources[i].generation==generation && resources[i].state==RS_ACTIVE; }
int registry_claim(enum res_type type, uint32_t start, uint32_t end, const char *name, bool share)
{
    CHECK(type==RES_MMIO && !share && !strcmp(name,"intel2d") && claims<4);
    unsigned i=++claims;
    resources[i]=(struct resource){type,start,end,name,100+i,RS_CLAIMED,false};
    return (int)i;
}
int registry_activate(int i, gen_t generation)
{ CHECK(resources[i].generation==generation); resources[i].state=RS_ACTIVE; return 0; }
int registry_quiesce(int i, gen_t generation, registry_idle_proof_fn proof)
{
    CHECK(registry_valid(i,generation)); CHECK(proof(i,generation));
    resources[i].state=RS_QUIESCING; return 0;
}
int registry_release(int i, gen_t generation)
{
    CHECK(resources[i].generation==generation && resources[i].state==RS_QUIESCING);
    resources[i].state=RS_RELEASED; releases++; return 0;
}
void *vmm_map_mmio(uint32_t phys, uint32_t bytes, bool uc)
{
    CHECK(uc); maps++;
    if (phys==0xf0000000 && (bytes==MMIO_BYTES || bytes==0x10000u)) return mmio;
    if (phys==0xf1000000 && bytes==GTT_BYTES) return table;
    if (phys==0xf0010000 && (bytes==0x10000u || bytes==0x20000u)) return table;
    if (phys==0xf0070000 && bytes==0x2000u) return mmio+0x70000/4;
    CHECK(phys==0xf2000000 && bytes==4096); return flush_page;
}
void vmm_revoke_mmio(void *p, uint32_t bytes)
{
    CHECK((p==mmio && (bytes==MMIO_BYTES || bytes==0x10000u)) ||
        (p==table && (bytes==GTT_BYTES || bytes==0x10000u || bytes==0x20000u)) ||
        (p==mmio+0x70000/4 && bytes==0x2000u) || (p==flush_page && bytes==4096));
    revokes++;
}
unsigned pmm_reservations(const struct pmm_reservation **out) { *out=0; return 0; }
uint32_t pmm_alloc_contig(uint32_t pages)
{ CHECK(pages==25); allocations++; return 0x01000000; }
void pmm_free(uint32_t phys) { CHECK(phys>=0x01000000 && phys<0x01019000); frees++; }
void pmm_free_owned(uint32_t phys, enum mm_owner owner)
{ CHECK(owner==MM_DMA && owner_pages); owner_pages--; pmm_free(phys); }
void ledger_move(uint32_t phys, uint32_t pages, enum mm_owner from, enum mm_owner to)
{ CHECK(phys==0x01000000 && pages==25 && from==MM_KERNEL && to==MM_DMA); owner_pages+=pages; }
int mm_pin(struct mm_pin *p)
{
    CHECK(owner_pages==25 && p->pages==25 && p->first_pfn==0x1000 && p->generation==42 &&
        p->direction==MM_PIN_BIDIRECTIONAL);
    if (pin_fail) return -ENOMEM;
    pins++; p->ticket=77; return 0;
}
int mm_unpin(struct mm_pin *p, uint32_t generation)
{ CHECK(p->ticket==77 && generation==42); p->ticket=0; unpins++; return 0; }
void framebuffer_write_drain(void) { }
uint64_t ktime_cycles(void) { return 0; }
int fbdev_accel_probe(const struct fb_device *d, int (*callback)(void *), void *ctx)
{
    CHECK(d==&fb && callback==activation && ctx==&engine);
    CHECK(pins==1 && owner_pages==25 && claims==4 && engine.cpu_safe && !engine.mapped);
    CHECK(live(0));
    if (chip->gen2) {
        uint32_t saved=host_drb;
        host_drb^=1u<<24; CHECK(!live(0)); host_drb=saved;
        saved=host_tseg;
        host_tseg^=1u<<(chip->device==0x2562 ? 16 : 8); CHECK(!live(0)); host_tseg=saved;
        CHECK(live(0));
        if (exercise_publish) {
            mmio[0x70/4]=0x1234u;
            struct intel_wait wait={.deadline=INTEL_WAIT_US};
            CHECK(!publish(0,dma_memory,64,&wait));
            CHECK(wbinvd_calls==1 && mmio[0x70/4]==0x1234u && !flush_page[0]);
        }
    }
    probe_calls++; return -EBUSY; /* presenter refused the exclusive claim */
}
void klog(const char *fmt, ...) { (void)fmt; }
int ksnprintf(char *buf, size_t n, const char *fmt, ...)
{ va_list ap; va_start(ap,fmt); int r=vsnprintf(buf,n,fmt,ap); va_end(ap); return r; }
static void check_marker(const char *path)
{
    char expected[64];
    snprintf(expected,sizeof(expected),"C:/system/settings/intel2d-%04x.cfg",functions[1].device);
    CHECK(!strcmp(path,expected));
}
int vfs_table_init(struct vfs *vfs, struct vfs_table *t, unsigned drive)
{ CHECK(vfs==&filesystem && t==&marker_table && drive==2); files++; return 0; }
void vfs_table_destroy(struct vfs_table *t) { CHECK(t==&marker_table); }
int vfs_stat(struct vfs_table *t, const char *path, struct fat_entry *entry)
{ check_marker(path); CHECK(t==&marker_table); entry->size=marker_present ? 12 : 0; return marker_present ? 0 : -FS_ENOENT; }
int vfs_open(struct vfs_table *t, const char *path, unsigned flags, enum vfs_share share, uint8_t attr)
{ check_marker(path); CHECK(t==&marker_table && flags==(VFS_WRITE|VFS_CREATE) && share==VFS_DENY_ALL && !attr); return marker_fail ? -FS_EIO : 1; }
int vfs_write(struct vfs_table *t, int fd, const void *data, size_t n, size_t *done)
{
    char expected[32]; snprintf(expected,sizeof(expected),"pci=8086:%04x",functions[1].device);
    CHECK(t==&marker_table && fd==1 && strstr(data,expected));
    *done=n; marker_present=true; return 0;
}
int vfs_truncate(struct vfs_table *t, int fd, uint64_t n)
{ CHECK(t==&marker_table && fd==1 && !n); marker_present=false; return 0; }
int vfs_commit(struct vfs_table *t, int fd)
{ CHECK(t==&marker_table && fd==1); commits++; return commit_fail ? -FS_EIO : 0; }
int vfs_close(struct vfs_table *t, int fd)
{ CHECK(t==&marker_table && fd==1); closes++; return close_fail ? -FS_EIO : 0; }
static void reset(void)
{
    memset(&engine,0,sizeof(engine)); engine.cpu_safe=true;
    memset(&g_boot,0,sizeof(g_boot));
    g_boot.e820_count=1; g_boot.e820[0]=(struct ciuki_e820){0x100000,0x07f00000,CBI_E820_RAM,0};
    g_boot.kernel_start=0x200000; g_boot.kernel_end=0x400000;
    memset(functions,0,sizeof(functions));
    functions[0]=(struct pci_func){.vendor=0x8086,.device=0x2580};
    functions[1]=(struct pci_func){.dev=2,.vendor=0x8086,.device=0x2582,.class_code=3,.command=6,
        .bar={0xf0000000,0,0xc0000008,0xf1000000}};
    count=2;
    fb=(struct fb_device){.width=32,.height=20,.pitch=128,.size=2560,.phys=0xc0000000,
        .mapped=pixels,.bpp=32,.red_size=8,.red_pos=16,.green_size=8,.green_pos=8,.blue_size=8,
        .present=true,.handle=0,.generation=42};
    resources[0]=(struct resource){RES_MMIO,fb.phys,fb.phys+fb.size,"fbdev",42,RS_ACTIVE,false};
    memset(mmio,0,sizeof(mmio)); memset(table,0,sizeof(table));
    mmio[0x70180/4]=(1u<<31)|(6u<<26); mmio[0x70188/4]=128;
    mmio[0x2020/4]=0x087c0001; mmio[0x209c/4]=INTEL_IDLE;
    mmio[0x2098/4]=65535; /* firmware already masks asynchronous HWS ISR reports */
    table[0]=0x08000001;
    for (unsigned i=0;i<4;i++) leases[i]=(struct lease){.handle=-1};
    host_ggc=3u<<20; gfx_msac=2u<<16; host_ifp=0xf2000001;
    dma_phys=0; dma_pin=(struct mm_pin){0}; reason="pending"; marker_written=blocked=false;
    claims=releases=allocations=frees=pins=unpins=maps=revokes=owner_pages=0;
    files=commits=closes=probe_calls=0;
    no_clflush=pin_fail=marker_present=marker_fail=commit_fail=close_fail=false;
    exercise_publish=false; memset(flush_page,0,sizeof(flush_page));
    wbinvd_calls=0; host_drb=host_tseg=0;
    volume.mounted=true; volume.readonly=false; filesystem.volumes[2]=&volume;
}
static void gen2_reset(uint16_t device, bool small_aperture)
{
    reset();
    functions[0].device=device==0x2562 ? 0x2560 : 0x3580;
    functions[1].device=device;
    functions[1].bar[0]=0xc0000008; functions[1].bar[1]=0xf0000000;
    functions[1].bar[2]=1; functions[1].bar[3]=0;
    if (device==0x3582) {
        functions[2]=(struct pci_func){.vendor=0x8086,.device=0x3584,.fn=1}; count=3;
    }
    /* TOM 160 MiB; TSEG 1 MiB; UMA 8 MiB -> stolen [151,159) MiB. */
    host_drb=5u<<24;
    host_tseg=device==0x2562 ? 7u<<16 : 1u<<8;
    host_ggc=((device==0x2562 ? 4u : 3u)<<20)|(4u<<16)|(small_aperture ? 1u<<16 : 0);
    mmio[0x2020/4]=0x09ee0001u; /* table in reserved UMA, outside scanout */
    mmio[0x209c/4]=0; mmio[0x2090/4]=0x01ffffc0u;
    table[0]=0x09700001u;
}
static void unqualified_chips(void)
{
    static const struct { uint16_t device, bridge; const char *why; } chips[]={
        {0x3577,0x3575,"i830m_chipset_flush_unproven"},
        {0x2572,0x2570,"i865g_chipset_flush_unproven"}
    };
    static uint32_t saved_mmio[ARRAY_SIZE(mmio)], saved_table[ARRAY_SIZE(table)];
    static uint32_t saved_dma[ARRAY_SIZE(dma_memory)];
    for (unsigned i=0;i<ARRAY_SIZE(chips);i++) for (unsigned small=0;small<2;small++) {
        reset();
        functions[0].device=chips[i].bridge;
        functions[1].device=chips[i].device;
        functions[1].bar[0]=fb.phys|8u;
        functions[1].bar[1]=0xf0000000;
        functions[1].bar[2]=1; functions[1].bar[3]=0;
        host_ggc=(3u<<20)|(4u<<16)|(small ? 1u<<16 : 0);
        /* Even hostile firmware state must be left untouched by a gated chip. */
        mmio[0x70/4]=0x80001234u; mmio[0x203c/4]=1;
        mmio[0x2030/4]=0x80; mmio[0x2034/4]=0x40;
        table[32767]=0xdead0001u;
        memcpy(saved_mmio,mmio,sizeof(mmio));
        memcpy(saved_table,table,sizeof(table));
        memcpy(saved_dma,dma_memory,sizeof(dma_memory));
        const struct display_driver *driver=&intel2d_gen2_driver;
        CHECK(driver->probe(&functions[1]) && !driver->eligible(&fb));
        CHECK(driver->attempt_begin(&filesystem)==-ENODEV);
        CHECK(!driver->start(&fb,&filesystem));
        /* A pre-existing marker flag must not let start bypass eligibility. */
        marker_written=true;
        CHECK(!driver->start(&fb,&filesystem));
        bool ready=true, used=true; const char *why=0;
        CHECK(driver->summary(&ready,&used,&why) && !ready && !used);
        CHECK(why && !strcmp(why,chips[i].why));
        CHECK(!claims && !releases && !allocations && !frees && !pins && !unpins);
        CHECK(!maps && !revokes && !owner_pages && !files && !commits && !closes);
        CHECK(!probe_calls && !wbinvd_calls && !cfg_writes);
        CHECK(!engine.mapped && !engine.touched && !engine.quarantined && engine.cpu_safe);
        CHECK(!engine.ops.fill_rect && !engine.ops.copy_rect && !engine.ops.sync);
        CHECK(!memcmp(saved_mmio,mmio,sizeof(mmio)));
        CHECK(!memcmp(saved_table,table,sizeof(table)));
        CHECK(!memcmp(saved_dma,dma_memory,sizeof(dma_memory)));
    }
}
int main(void)
{
    unqualified_chips();
    reset(); CHECK(probe(&functions[1]) && eligible(&fb));
    CHECK(!claims && !files && !maps && !cfg_writes);
    const unsigned ids[]={0x3577,0x2562,0x3582,0x2572};
    for (unsigned i=0;i<4;i++) {
        reset(); functions[1].device=ids[i]; CHECK(probe(&functions[1]));
        CHECK(!eligible(&fb) && !strcmp(reason,i==0 ? "i830m_chipset_flush_unproven" :
            i==3 ? "i865g_chipset_flush_unproven" : "host_bridge_mismatch"));
    }
    reset(); functions[1].device=0x2782; CHECK(!probe(&functions[1]));
    reset(); functions[2]=functions[1]; functions[2].fn=1; count=3; CHECK(!eligible(&fb));
    reset(); functions[0].device=0x2590; CHECK(!eligible(&fb));
    reset(); functions[1].command=2; CHECK(!probe(&functions[1]));
    reset(); no_clflush=true; CHECK(!eligible(&fb));
    reset(); g_boot.flags=CBI_F_SAFE_MODE; CHECK(attempt_begin(&filesystem)==-ENODEV && !files && !claims);
    reset(); fb.generation++; CHECK(!eligible(&fb));
    reset(); marker_present=true; CHECK(attempt_begin(&filesystem) && blocked && !claims && !commits);
    reset(); marker_fail=true; CHECK(attempt_begin(&filesystem) && !marker_written && !claims);
    reset(); commit_fail=true; CHECK(attempt_begin(&filesystem) && !marker_written && closes==1);
    reset(); close_fail=true; CHECK(attempt_begin(&filesystem) && !marker_written && commits==1);
    reset(); CHECK(!attempt_begin(&filesystem) && marker_written && marker_present && commits==1);
    CHECK(!attempt_end(&filesystem) && !marker_written && !marker_present && commits==2);
    reset(); CHECK(!attempt_begin(&filesystem)); host_ifp=0;
    CHECK(!start(&fb,0) && !claims && !allocations);
    reset(); CHECK(!attempt_begin(&filesystem)); table[32767]=0x08010001;
    CHECK(!start(&fb,0) && !probe_calls && allocations==1 && frees==25 && pins==1 && unpins==1);
    CHECK(releases==4 && revokes==3 && !owner_pages);
    reset(); CHECK(!attempt_begin(&filesystem)); pin_fail=true;
    CHECK(!start(&fb,0) && !probe_calls && allocations==1 && frees==25 && !pins && releases==3);
    reset(); CHECK(!attempt_begin(&filesystem)); mmio[0x70184/4]=4096;
    CHECK(!start(&fb,0) && !allocations && !probe_calls && releases==3);
    reset(); CHECK(!attempt_begin(&filesystem));
    CHECK(!start(&fb,0) && probe_calls==1 && frees==25 && unpins==1 && releases==4 && !dma_phys);
    CHECK(!mmio[0x203c/4] && !table[32767] && !fallback());
    quarantine("test"); CHECK(!eligible(&fb) && attempt_begin(&filesystem));
    reset(); CHECK(!attempt_begin(&filesystem)); mmio[0x2098/4]=0;
    CHECK(!start(&fb,0) && !probe_calls && !mmio[0x203c/4] && frees==25 && unpins==1);
    reset();
    CHECK(intel2d_gen2_driver.framebuffer_bar==0 && intel2d_gen3_driver.framebuffer_bar==2);
    CHECK(!strcmp(intel2d_gen3_driver.id,"intel2d") &&
        !strcmp(intel2d_gen3_driver.name,"Intel integrated graphics"));
    bool ready, used; const char *why;
    CHECK(summary(&ready,&used,&why) && !ready && !used);
    engine.active=true; engine.operations=1;
    CHECK(summary(&ready,&used,&why) && ready && used);
    intel_engine_quarantine(&engine,"completion_timeout");
    CHECK(summary(&ready,&used,&why) && !ready && !strcmp(why,"completion_timeout"));
    engine.mapped=true; engine.cpu_safe=false;
    CHECK(release_all()==-EIO && !frees && !unpins && !releases && !revokes);
    CHECK(!cfg_writes);
    for (unsigned which=0;which<2;which++) for (unsigned small=0;small<2;small++) {
        uint16_t id=which ? 0x3582 : 0x2562;
        gen2_reset(id,small!=0);
        CHECK(probe(&functions[1]) && eligible(&fb) && !maps && !files && !claims);
        CHECK(!attempt_begin(&filesystem) && marker_written);
        CHECK(!start(&fb,0) && probe_calls==1 && frees==25 && unpins==1 && releases==4);
        CHECK(aperture==(small ? 64u<<20 : 128u<<20) && gtt_bytes==(small ? 0x10000u : 0x20000u));
        CHECK(stolen_start==0x09700000u && stolen_end==0x09f00000u && !wbinvd_calls);
        CHECK(!mmio[0x203c/4] && !cfg_writes && !owner_pages);
        CHECK(!attempt_end(&filesystem) && !marker_present);
        gen2_reset(id,small!=0); CHECK(!attempt_begin(&filesystem));
        host_ggc|=15u<<20;
        CHECK(!start(&fb,0) && !claims && !allocations);
        gen2_reset(id,small!=0); CHECK(!attempt_begin(&filesystem));
        table[(small ? 16384u : 32768u)-1]|=1u;
        CHECK(!start(&fb,0) && !probe_calls && pins==1 && unpins==1 && releases==4);
        gen2_reset(id,small!=0); CHECK(!attempt_begin(&filesystem));
        mmio[0x210c/4]=1; /* foreign secondary ring must remain untouched */
        CHECK(!start(&fb,0) && !probe_calls && !cfg_writes && !mmio[0x203c/4]);
    }
    gen2_reset(0x3582,false); count=2; CHECK(!eligible(&fb));
    gen2_reset(0x2562,false); CHECK(!attempt_begin(&filesystem)); host_tseg=1u<<16;
    CHECK(!start(&fb,0) && !claims);
    gen2_reset(0x3582,false); CHECK(!attempt_begin(&filesystem)); host_drb=128u<<24;
    CHECK(!start(&fb,0) && !claims);
    gen2_reset(0x2562,false); exercise_publish=true; CHECK(!attempt_begin(&filesystem));
    CHECK(!start(&fb,0) && probe_calls==1 && wbinvd_calls==1 && unpins==1 && releases==4);
    gen2_reset(0x3582,false); exercise_publish=true; CHECK(!attempt_begin(&filesystem));
    CHECK(!start(&fb,0) && probe_calls==1 && wbinvd_calls==1 && unpins==1 && releases==4);
    printf("intel2d adapter: PASS (probe, ownership, marker, rollback, UI identity, pinned unsafe guard)\n");
    return 0;
}
