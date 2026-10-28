/* Production 3D owner against the real keyed-marker implementation and the
 * unchanged committed/cache VFS fixture. Each variant is a separate boot.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main old_marker_main
#define klog old_marker_log
#include "display_attempt_test.c"
#undef klog
#undef main
#include <stdlib.h>
#include <ciuki/mm.h>
#include "../../src/kernel/drivers/video/radeon2d/radeon3d.h"
#define THREE_KEY "C:/system/settings/accel-3d-radeon-1002-5159.cfg"
struct ciuki_boot_info g_boot;
static struct pci_func pci3={.vendor=0x1002,.device=0x5159,.revision=1,.command=3,
    .bar={0xe0000000,0,0xd0000000}};
static struct fb_device fb3={.width=1024,.height=768,.pitch=4096,.size=3145728,
    .phys=0xe0000000,.bpp=32,.present=true,.generation=42};
static struct radeon_engine engine3;
static const struct radeon_group_info group3={"R100",RADEON_RECT,0};
static struct resource claims3[1];
static unsigned variant3, claimed3, released3, quarantined3, locked3, stores3, packets3, logs3;
static bool prepared3;
static void *allocations3[32];
static unsigned allocations_count3, reserves3;
void klog(const char *fmt,...)
{
    assert(!locked3); logs3++;
    char line[512]; va_list ap; va_start(ap,fmt); vsnprintf(line,sizeof(line),fmt,ap); va_end(ap);
    assert(strstr(line,"[3d] probe") || strstr(line,"[3d] fullsize") || strstr(line,"[3d] precision") || strstr(line,"[accel] retry "));
}
void *kmalloc(size_t bytes)
{
    assert(!locked3);
    if (variant3==9 && allocations_count3==2) return 0;
    void *p=calloc(1,bytes); assert(p && allocations_count3<32);
    allocations3[allocations_count3++]=p; return p;
}
void kfree(void *p)
{
    assert(!locked3);
    for (unsigned i=0;i<allocations_count3;i++) if (allocations3[i]==p) { allocations3[i]=0; free(p); return; }
    assert(!p);
}
const struct pci_func *display_driver_owner(void) { return &pci3; }
uint32_t pmm_free_count(void) { return 10000; }
uint32_t pmm_total_usable(void) { return 20000; }
int registry_claim(enum res_type type,uint32_t first,uint32_t end,const char *owner,bool share)
{
    assert(!locked3 && prepared3 && claimed3<1 && type==RES_MMIO && !share && !strcmp(owner,"radeon-3d"));
    assert(first==0xe0300000 && end==0xe0fff000);
    unsigned i=claimed3++;
    claims3[i]=(struct resource){.type=type,.start=first,.end=end,.owner=owner,.generation=7+i,.state=RS_CLAIMED};
    return (int)i;
}
const struct resource *registry_get(unsigned i) { assert(i<claimed3); return &claims3[i]; }
void registry_inventory(struct registry_inventory_copy *out)
{
    memset(out,0,sizeof(*out));
    if (variant3==12) {
        out->resource_count=1;
        out->resources[0]=(struct resource){.type=RES_MMIO,.start=0xe0900000,.end=0xe0901000,.state=RS_QUARANTINED};
    }
    if (variant3==13) out->truncated=true;
}
int registry_activate(int h,gen_t generation)
{
    assert(!locked3 && h>=0 && (unsigned)h<claimed3 && generation==claims3[h].generation && claims3[h].state==RS_CLAIMED);
    claims3[h].state=RS_ACTIVE; return 0;
}
int registry_quiesce(int h,gen_t generation,registry_idle_proof_fn proof)
{
    assert(!locked3 && h>=0 && (unsigned)h<claimed3 && generation==claims3[h].generation && claims3[h].state==RS_ACTIVE);
    claims3[h].state=RS_QUIESCING; return proof(h,generation) ? 0 : -EIO;
}
int registry_release(int h,uint32_t generation)
{
    assert(!locked3 && h>=0 && (unsigned)h<claimed3 && generation==claims3[h].generation && claims3[h].state==RS_QUIESCING);
    claims3[h].state=RS_RELEASED; released3++; return 0;
}
int registry_quarantine(int h,gen_t generation)
{
    assert(h>=0 && (unsigned)h<claimed3 && generation==claims3[h].generation);
    claims3[h].state=RS_QUARANTINED; quarantined3++; return 0;
}
void *vmm_reserve_mmio(uint32_t bytes) { reserves3++; return kmalloc(bytes); }
void *vmm_replace_mmio(void *p,uint32_t capacity,uint32_t phys,uint32_t bytes)
{
    assert(!locked3 && capacity>=bytes && phys==0xe0300000);
    return variant3==4 ? 0 : p;
}
void vmm_revoke_mmio(void *p,uint32_t bytes) { assert(!locked3 && p && bytes); }
int fbdev_render3d_prepare(uint32_t generation)
{ assert(!locked3 && !prepared3 && generation==42); prepared3=true; return 0; }
void fbdev_render3d_prepared(void) { assert(!locked3 && prepared3); prepared3=false; }
int fbdev_render3d_enter(uint32_t generation)
{
    assert(!locked3 && prepared3 && generation==42 && size_of(THREE_KEY)==14 && !opened && !table_open);
    if (variant3==6) return -EBUSY;
    locked3=1; return 0;
}
void fbdev_render3d_leave(void) { assert(locked3); locked3=0; }
static uint32_t read_engine3(void *ctx,uint32_t reg)
{ (void)ctx;(void)reg;assert(locked3 && size_of(THREE_KEY)==14 && !opened && !table_open);return UINT32_MAX; }
static void write_engine3(void *ctx,uint32_t reg,uint32_t value)
{ (void)ctx;(void)reg;(void)value;assert(locked3);stores3++; }
static void drain_engine3(void *ctx) { (void)ctx; }
static uint64_t clock_engine3(void *ctx) { (void)ctx;return 0; }
static int delay_engine3(void *ctx,uint32_t us) { (void)ctx;(void)us;return 0; }
int radeon_engine_idle(struct radeon_engine *s) { return s->cpu_safe ? 0 : -EIO; }
void radeon_engine_quarantine(struct radeon_engine *s,const char *reason)
{ s->active=false;s->quarantined=true;s->cpu_safe=false;s->reason=reason; }
int main(int argc,char **argv)
{
    assert(argc==2); variant3=(unsigned)atoi(argv[1]); reset();
    g_boot.display.vram_bytes=16*1024*1024;
    strcpy(g_boot.options,variant3==0 ? "x3d=probe 3d=probeX" : "3d=probe");
    engine3=(struct radeon_engine){.group=&group3,.group_id=RADEON_R100,.active=true,.cpu_safe=true,
        .local_bytes=16*1024*1024,.cursor_offset=0xfff000,
        .io={.read=read_engine3,.write=write_engine3,.now=clock_engine3,
            .drain=drain_engine3,.delay=delay_engine3}};
    if (variant3==1) engine3.cursor_offset=0x301000;
    if (variant3==2) put(THREE_KEY,"accel=attempt\n");
    if (variant3==5) fail_step=5; /* commit cut */
    if (variant3==11) fail_step=6; /* committed BEGIN, failed close */
    if (variant3==14) volume.readonly=true;
    if (variant3==8) pci3.bar[0]|=4; /* unsupported 64-bit BAR, no sizing */
    if (variant3!=3) radeon3d_bind_vfs(&storage);
    radeon3d_boot_diagnostic(&engine3,&fb3);
    unsigned previous_logs=logs3; radeon3d_boot_diagnostic(&engine3,&fb3); assert(logs3==previous_logs);
    assert(!locked3 && !prepared3 && !engine3.render3d_qualified);
    if (claimed3) assert(reserves3==1);
    assert(!packets3); /* every variant stops before the first vertex command */
    if (variant3==0) assert(!logs3 && !claimed3 && !commits);
    else if (variant3==1 || variant3==3 || variant3==8 || variant3==12 || variant3==13) assert(!claimed3 && !commits && !stores3);
    else if (variant3==7 || variant3==10) assert(engine3.quarantined && !engine3.active && quarantined3==1 && !released3 && size_of(THREE_KEY)==14 && !stores3);
    else {
        assert(claimed3==1 && released3==1 && !quarantined3 && !stores3);
        if (variant3==2) assert(size_of(THREE_KEY)==14 && !commits && !truncates);
        if (variant3==6) assert(!size_of(THREE_KEY) && commits==2 && truncates==1);
        if (variant3==11) assert(size_of(THREE_KEY)==14 && commits==1 && !truncates);
    }
    if (variant3==2) {
        /* Same production descriptor policy: an explicit retry clears only
         * this device's 2D and 3D attempt files, retaining another key. */
        static const struct display_driver declaration={.id="radeon2d",
            .attempt_files={LEGACY,"C:/system/settings/accel-3d-radeon-1002-%04x.cfg"}};
        static const char other[]="C:/system/settings/accel-3d-radeon-1002-8a22.cfg";
        put(other,"accel=attempt\n"); put(DISPLAY_RETRY_PATH,"driver=radeon2d\npci=1002:5159\n");
        bool retained=true; retry_mode=true;
        assert(!display_attempt_prepare(&storage,&declaration,&pci3,&retained));
        assert(!retained && !size_of(THREE_KEY) && size_of(other)==14);
    }
    /* Destroy the emulated device/boot, then free all retained allocations.
     * This is fixture teardown, not a production quarantine-release path. */
    for (unsigned i=0;i<allocations_count3;i++) free(allocations3[i]);
    printf("owner marker/claim pre-write variant %u PASS\n",variant3); return 0;
}
