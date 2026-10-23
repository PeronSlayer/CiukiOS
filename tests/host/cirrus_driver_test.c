/* Real adapter and registry, fake indexed ports/resource/VFS boundary.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdarg.h>
#include "cirrus_fake.h"
#define CIUKI_CPU_H
#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
bool g_cpu_tsc=true;
uint64_t g_tsc_per_ms=1000;
volatile uint64_t g_ticks;
struct ciuki_boot_info g_boot;
static struct cirrus_fake hw;
static struct fb_device active_fb;
static struct pci_func gpu, second_gpu;
static struct resource resources[2];
static unsigned pci_functions=1, claims, releases, probe_calls, locks, irq_depth;
static bool fail_claim, fail_activate, fail_release, refuse_activation, native_lock;
static uint8_t gfx_index=0x55, seq_index=0x66, seq[256];
static uint32_t irq_save(void) { assert(!irq_depth); irq_depth++; return 0x200; }
static void irq_restore(uint32_t flags) { assert(flags==0x200 && irq_depth==1); irq_depth--; }
static uint8_t inb(uint16_t port)
{
    assert(irq_depth);
    if (port==0x3ce) return gfx_index;
    if (port==0x3cf) return fake_read(&hw,gfx_index);
    if (port==0x3c4) return seq_index;
    assert(port==0x3c5); return seq[seq_index];
}
static void outb(uint16_t port, uint8_t value)
{
    assert(irq_depth);
    if (port==0x3ce) gfx_index=value;
    else if (port==0x3c4) seq_index=value;
    else {
        assert(port==0x3cf && locks && native_lock);
        fake_write(&hw,gfx_index,value);
    }
}
#include "../../src/kernel/drivers/video/cirrus/driver.c"

const struct display_driver s3savage_driver={0};
static struct vfs disk;
static struct fat_volume volume;
static unsigned file_size, file_calls, committed, cleared, closed, destroyed;
enum { IO_OK, IO_INIT, IO_STAT, IO_OPEN, IO_WRITE, IO_SHORT, IO_COMMIT, IO_CLOSE, IO_TRUNCATE };
static unsigned fail_io;
const struct fb_device *fbdev_get(void) { return &active_fb; }
unsigned fbdev_native_capabilities(void) { return engine.ops.capabilities ? engine.ops.capabilities(&engine) : 0; }
bool fbdev_cpu_ready(void) { return engine.cpu_safe || !engine.ops.sync; }
unsigned pci_count(void) { return pci_functions; }
const struct pci_func *pci_get(unsigned i) { return i==0 ? &gpu : i==1 ? &second_gpu : NULL; }
const struct resource *registry_get(unsigned i) { return i<2 ? &resources[i] : NULL; }
bool registry_valid(int h, gen_t gen)
{ return h>=0 && h<2 && resources[h].state==RS_ACTIVE && resources[h].generation==gen; }
int registry_claim(enum res_type type, uint32_t first, uint32_t last, const char *name, bool share)
{
    assert(!locks && persisted && type==RES_PORT && first==0x3c4 && last==0x3d0 && !share);
    assert(!strcmp(name,"cirrus-2d")); claims++;
    if (fail_claim) return -EBUSY;
    resources[1]=(struct resource){RES_PORT,first,last,name,7,RS_CLAIMED,false}; return 1;
}
int registry_activate(int h, gen_t gen)
{
    assert(!locks && h==1 && gen==7);
    if (fail_activate) return -EIO;
    resources[1].state=RS_ACTIVE; return 0;
}
int registry_release(int h, gen_t gen)
{
    assert(h==1 && gen==7 && resources[1].state==RS_QUIESCING); releases++;
    if (fail_release) return -EIO;
    resources[1].state=RS_RELEASED; return 0;
}
int registry_quiesce(int h, gen_t gen, registry_idle_proof_fn proof)
{
    assert(h==1 && gen==7 && resources[1].state==RS_ACTIVE);
    resources[1].state=RS_QUIESCING;
    assert(proof(h,gen)); return 0;
}
int registry_quarantine(int h, gen_t gen)
{ assert(h==1 && gen==7); resources[1].state=RS_QUARANTINED; return 0; }
uint64_t ktime_cycles(void) { assert(!irq_depth); return fake_now(&hw); }
void framebuffer_write_drain(void) { assert(!irq_depth); hw.drains++; }
void klog(const char *format, ...) { assert(!locks && format); }
int fbdev_accel_probe(const struct fb_device *d, int (*fn)(void *), void *ctx)
{
    assert(!locks && d==&active_fb); probe_calls++;
    if (refuse_activation) return -EBUSY;
    locks++; native_lock=true;
    int e=fn(ctx);
    if (e) (void)cirrus_driver.fallback();
    native_lock=false; locks--; return e;
}
int vfs_table_init(struct vfs *v, struct vfs_table *table, unsigned drive)
{ assert(!locks && v==&disk && table==&marker_table && drive==2); file_calls++; return fail_io==IO_INIT ? -EIO : 0; }
void vfs_table_destroy(struct vfs_table *table) { assert(!locks && table==&marker_table); destroyed++; }
int vfs_stat(struct vfs_table *table, const char *path, struct fat_entry *entry)
{
    assert(!locks && table==&marker_table && !strcmp(path,"C:/system/settings/accel.cfg"));
    if (fail_io==IO_STAT) return -EIO;
    entry->size=file_size; return file_size ? 0 : -FS_ENOENT;
}
int vfs_open(struct vfs_table *table, const char *path, unsigned flags, enum vfs_share share, uint8_t attr)
{
    assert(!locks && table==&marker_table && !strcmp(path,"C:/system/settings/accel.cfg"));
    assert(flags==(VFS_WRITE|VFS_CREATE) && share==VFS_DENY_ALL && !attr);
    return fail_io==IO_OPEN ? -EIO : 1;
}
int vfs_write(struct vfs_table *table, int fd, const void *data, size_t size, size_t *done)
{
    assert(!locks && table==&marker_table && fd==1);
    assert(size==strlen("accel=attempt\ndriver=cirrus\n") && !memcmp(data,"accel=attempt\ndriver=cirrus\n",size));
    *done=fail_io==IO_SHORT ? size-1 : size;
    if (fail_io==IO_WRITE) return -EIO;
    file_size=(unsigned)*done; return 0;
}
int vfs_truncate(struct vfs_table *table, int fd, uint64_t size)
{
    assert(!locks && table==&marker_table && fd==1 && !size);
    if (fail_io==IO_TRUNCATE) return -EIO;
    cleared++; file_size=0; return 0;
}
int vfs_commit(struct vfs_table *table, int fd)
{ assert(!locks && table==&marker_table && fd==1); committed++; return fail_io==IO_COMMIT ? -EIO : 0; }
int vfs_close(struct vfs_table *table, int fd)
{ assert(!locks && table==&marker_table && fd==1); closed++; return fail_io==IO_CLOSE ? -EIO : 0; }

static void setup(unsigned bpp, uint16_t id)
{
    memset(&engine,0,sizeof(engine)); memset(&hw,0,sizeof(hw)); memset(&g_boot,0,sizeof(g_boot));
    persisted=blocked=false; port_claim=-1; reason="pending";
    claims=releases=probe_calls=locks=irq_depth=0;
    fail_claim=fail_activate=fail_release=refuse_activation=native_lock=false;
    file_size=file_calls=committed=cleared=closed=destroyed=fail_io=0;
    memset(&disk,0,sizeof(disk)); memset(&volume,0,sizeof(volume));
    volume.mounted=true; disk.volumes[2]=&volume;
    memset(seq,0,sizeof(seq)); seq[6]=0x12; seq_index=0x66; gfx_index=0x55;
    hw.step=10; active_fb=fake_fb(&hw,bpp);
    resources[0]=(struct resource){RES_MMIO,active_fb.phys,active_fb.phys+active_fb.size,"fbdev",42,RS_ACTIVE,false};
    resources[1]=(struct resource){0};
    gpu=(struct pci_func){.vendor=0x1013,.device=id,.class_code=3,.command=3,.bar={active_fb.phys}};
    second_gpu=gpu; pci_functions=1; g_boot.display.vram_bytes=4*1024*1024;
    for (unsigned i=0;i<sizeof(hw.pixels);i++) hw.pixels[i]=(uint8_t)(i*73+11);
}
static void nomination(void)
{
    static const uint16_t ids[]={0xa0,0xa4,0xac,0xb8};
    for (unsigned i=0;i<ARRAY_SIZE(ids);i++) {
        setup(24,ids[i]);
        assert(display_driver_bind(&active_fb)==&cirrus_driver && cirrus_driver.eligible(&active_fb));
        assert(!hw.reads && !hw.trace_count && !claims && !file_calls);
    }
    setup(32,0xb8);
    gpu.device=0xd0; assert(!display_driver_bind(&active_fb));
    gpu.device=0xb8; gpu.command=2; assert(!display_driver_bind(&active_fb));
    gpu.command=3; gpu.bar[0]+=4096; assert(!display_driver_bind(&active_fb));
    gpu.bar[0]-=4096; pci_functions=2; assert(!display_driver_bind(&active_fb));
    pci_functions=1; resources[0].generation++; assert(!display_driver_bind(&active_fb));
    resources[0].generation--; g_boot.display.vram_bytes=1; assert(!display_driver_bind(&active_fb));
    g_boot.display.vram_bytes=4*1024*1024;
    g_boot.flags=CBI_F_SAFE_MODE;
    assert(cirrus_driver.attempt_begin(&disk) && !claims && !file_calls);
    g_boot.flags=0; strcpy(g_boot.options,"accel=0");
    assert(cirrus_driver.attempt_begin(&disk) && !claims && !file_calls);
    g_boot.options[0]=0; gpu.device=0xa0;
    assert(!cirrus_driver.eligible(&active_fb)); /* GD543x 32 bpp unsupported. */
}
static void activation(void)
{
    static const uint16_t ids[]={0xa0,0xa4,0xac,0xb8};
    for (unsigned i=0;i<ARRAY_SIZE(ids);i++) {
        setup(i==3 ? 32 : 24,ids[i]);
        assert(display_driver_bind(&active_fb)==&cirrus_driver);
        uint8_t saved[sizeof(hw.pixels)]; memcpy(saved,hw.pixels,sizeof(saved));
        assert(!cirrus_driver.start(&active_fb,NULL) && !claims); /* Marker required. */
        assert(!cirrus_driver.attempt_begin(&disk) && persisted && file_size && committed==1 && !hw.starts);
        const struct accel_ops *ops=cirrus_driver.start(&active_fb,NULL);
        assert(ops && hw.starts==2 && !memcmp(saved,hw.pixels,sizeof(saved)));
        assert(gfx_index==0x55 && seq_index==0x66 && !irq_depth && probe_calls==1);
        assert(!cirrus_driver.attempt_end(&disk) && !persisted && !file_size && cleared==1);
        display_driver_refresh();
        struct ciuki_display_engine_info info; display_driver_info(&info);
        assert(!strcmp(info.driver_id,"cirrus") && !strcmp(info.display_name,"Cirrus Logic GD54xx"));
        assert(info.ready && !info.native_used && info.capabilities==1 && info.cpu_ready);
        if (i==3) {
            display_driver_count(CIUKI_ENGINE_FILL,true); display_driver_info(&info);
            assert(info.native_used);
        }
        locks++; native_lock=true;
        assert(!ops->stop(ops->ctx) && port_claim<0);
        native_lock=false; locks--;
    }
}
static void failures(void)
{
    for (unsigned failure=IO_INIT;failure<=IO_CLOSE;failure++) {
        setup(24,0xb8); fail_io=failure;
        assert(cirrus_driver.attempt_begin(&disk) && !persisted && blocked && !hw.starts && !claims);
        assert(!cirrus_driver.start(&active_fb,NULL));
    }
    setup(24,0xb8); file_size=13; /* Anonymous legacy crash marker. */
    assert(cirrus_driver.attempt_begin(&disk) && file_size==13 && !cleared);
    assert(cirrus_driver.attempt_end(&disk) && file_size==13);
    setup(24,0xb8); volume.readonly=true;
    assert(cirrus_driver.attempt_begin(&disk) && !claims && !hw.starts);
    for (unsigned failure=0;failure<5;failure++) {
        setup(24,0xb8); assert(!cirrus_driver.attempt_begin(&disk));
        fail_claim=failure==0; fail_activate=failure==1; refuse_activation=failure==2;
        if (failure==3) seq[6]=0;
        if (failure==4) seq[0x12]=1;
        assert(!cirrus_driver.start(&active_fb,NULL));
        assert(!hw.starts && !hw.trace_count && !cirrus_driver.fallback());
        assert(!cirrus_driver.attempt_failed(&disk) && blocked && file_size);
    }
    setup(24,0xb8); assert(!cirrus_driver.attempt_begin(&disk));
    hw.corrupt=true;
    assert(!cirrus_driver.start(&active_fb,NULL) && engine.quarantined && engine.cpu_safe);
    assert(!cirrus_driver.fallback() && port_claim<0 && file_size);
    setup(24,0xb8); assert(!cirrus_driver.attempt_begin(&disk));
    hw.stuck=true; hw.partial=true; hw.reset_fails=true;
    assert(!cirrus_driver.start(&active_fb,NULL) && engine.quarantined && !engine.cpu_safe);
    locks++; native_lock=true;
    assert(cirrus_driver.fallback()==&engine.ops && port_claim>=0);
    assert(engine.ops.stop(&engine) && port_claim>=0 && !engine.ops.cpu_ready(&engine));
    hw.reset_fails=false;
    assert(!engine.ops.stop(&engine) && port_claim<0 && engine.cpu_safe);
    native_lock=false; locks--;
    assert(!cirrus_driver.eligible(&active_fb) && file_size && !cleared);
    setup(24,0xb8); assert(!cirrus_driver.attempt_begin(&disk));
    assert(cirrus_driver.start(&active_fb,NULL)); fail_io=IO_TRUNCATE;
    assert(cirrus_driver.attempt_end(&disk) && file_size && blocked);
    setup(24,0xb8); assert(!cirrus_driver.attempt_begin(&disk));
    assert(cirrus_driver.start(&active_fb,NULL));
    unsigned writes=hw.trace_count;
    resources[0].state=RS_QUIESCING;
    locks++; native_lock=true;
    assert(engine.ops.sync(&engine) && !engine.cpu_safe && engine.quarantined && hw.trace_count==writes);
    assert(cirrus_driver.fallback()==&engine.ops && port_claim>=0);
    native_lock=false; locks--;
}
int main(void)
{
    nomination(); activation(); failures();
    puts("cirrus adapter: PCI ownership, markers, indexed ports, activation and quarantine PASS");
    return 0;
}
