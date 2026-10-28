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
#include "matrox3d_model.h"
#define THREE_KEY "C:/system/settings/accel-3d-matrox-102b-0525.cfg"
struct ciuki_boot_info g_boot;
static struct pci_func pci3={.vendor=0x102b,.device=0x0525,.revision=1,.command=3,.class_code=3,
    .bar={0xe0000000,0xd0000000,0}};
static struct fb_device fb3={.width=1024,.height=768,.pitch=4096,.size=3145728,
    .phys=0xe0000000,.bpp=32,.present=true,.generation=42};
static struct matrox_engine engine3;
static struct resource claims3[1];
static unsigned variant3, claimed3, released3, quarantined3, locked3, logs3;
static bool prepared3;
static void *allocations3[64];
static unsigned allocations_count3, reserves3;
static uint32_t page_sizes3[64],page_bytes3;
static unsigned page_attempts3;
void klog(const char *fmt,...)
{
    assert(!locked3); logs3++;
    char line[512]; va_list ap; va_start(ap,fmt); vsnprintf(line,sizeof(line),fmt,ap); va_end(ap);
    if (getenv("CIUKI_TEST_TRACE")) fprintf(stderr,"%s\n",line);
    assert(strstr(line,"[3d] probe") || strstr(line,"[3d] fullsize") || strstr(line,"[3d] precision") || strstr(line,"[accel] retry "));
}
static void *backing_alloc3(size_t bytes)
{
    assert(!locked3);
    if (variant3==9 && allocations_count3==2) return 0;
    void *p=calloc(1,bytes); assert(p && allocations_count3<64);
    allocations3[allocations_count3++]=p; return p;
}
void *(kmalloc)(size_t bytes)
{ return bytes>KHEAP_MAX_REQUEST ? 0 : backing_alloc3(bytes); }
void *desktop_buffer_alloc(uint32_t bytes,enum mm_owner owner)
{
    assert(bytes && owner==MM_IO_REQUEST);
    if (variant3==17 && !page_attempts3++) return 0;
    void *p=backing_alloc3(bytes);
    if (p) {
        page_sizes3[allocations_count3-1]=bytes;
        page_bytes3+=(bytes+PAGE_SIZE-1)&~(PAGE_SIZE-1);
        assert(page_bytes3+matrox3d_backend()->resident_bytes<=R3D_RAM_LIMIT);
    }
    return p;
}
void desktop_buffer_free(void *p,uint32_t bytes,enum mm_owner owner)
{
    assert(!locked3 && p && owner==MM_IO_REQUEST);
    for (unsigned i=0;i<allocations_count3;i++) if (allocations3[i]==p) {
        assert(page_sizes3[i]==bytes);
        page_sizes3[i]=0; page_bytes3-=(bytes+PAGE_SIZE-1)&~(PAGE_SIZE-1);
        allocations3[i]=0; free(p); return;
    }
    assert(0);
}
void kfree(void *p)
{
    assert(!locked3);
    for (unsigned i=0;i<allocations_count3;i++) if (allocations3[i]==p) { assert(!page_sizes3[i]); allocations3[i]=0; free(p); return; }
    assert(!p);
}
const struct pci_func *display_driver_owner(void) { return &pci3; }
const struct display_driver *display_driver_bound(void)
{
    static const struct render3d_family family={.status=R3D_UNQUALIFIED,.backend=matrox3d_backend};
    static const struct display_driver driver={.render3d=&family}; return &driver;
}
const struct fb_device *fbdev_get(void) { return &fb3; }
uint32_t pmm_free_count(void) { return 10000; }
uint32_t pmm_total_usable(void) { return 20000; }
int registry_claim(enum res_type type,uint32_t first,uint32_t end,const char *owner,bool share)
{
    assert(!locked3 && prepared3 && claimed3<1 && type==RES_MMIO && !share && !strcmp(owner,"matrox-3d"));
    assert(first>=0xe0300000 && end==0xe0fff000);
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
        out->resources[0]=(struct resource){.type=RES_MMIO,.start=0xe0301000,.end=0xe0fff000,.state=RS_QUARANTINED};
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
void *vmm_reserve_mmio(uint32_t bytes) { reserves3++; return backing_alloc3(bytes); }
void *vmm_replace_mmio(void *p,uint32_t capacity,uint32_t phys,uint32_t bytes)
{
    assert(!locked3 && capacity>=bytes && (phys==0xe0300000));
    if (variant3==4) return 0;
    model.ram=p; model.first=phys-0xe0000000; model.bytes=bytes; return p;
}
void vmm_revoke_mmio(void *p,uint32_t bytes) { assert(!locked3 && p && bytes); }
int fbdev_render3d_prepare(uint32_t generation)
{ assert(!locked3 && !prepared3 && generation==42); prepared3=true; return 0; }
void fbdev_render3d_prepared(void) { assert(!locked3 && prepared3); prepared3=false; }
int fbdev_render3d_enter(uint32_t generation)
{
    assert(!locked3 && prepared3 && generation==42 && size_of(THREE_KEY)==14 && !opened && !table_open);
    if (variant3==6) return -EBUSY;
    if (variant3==7) model.dead=true; /* fault after the durable BEGIN */
    locked3=1; model.locks=1; return 0;
}
void fbdev_render3d_leave(void) { assert(locked3); locked3=0; model.locks=0; }
static void *attach_alloc(void *ctx,size_t bytes) { (void)ctx; return kmalloc(bytes); }
static void attach_free(void *ctx,void *p) { (void)ctx; kfree(p); }
static uint64_t attach_now(void *ctx) { (void)ctx; return model.time; }
int main(int argc,char **argv)
{
    assert(argc==2); variant3=(unsigned)atoi(argv[1]); reset();
    g_boot.display.vram_bytes=16*1024*1024;
    strcpy(g_boot.options,variant3==0 ? "x3d=probe 3d=probeX" : "3d=probe");
    memset(&model,0,sizeof(model)); model.credits=model.capacity=16;
    model.no_draw=variant3!=16; /* numerical refusal, or positive full boot */
    engine3=model_engine(); engine3.cursor_offset=0xfff000;
    if (variant3==15) model.dead=true; /* preflight refusal, no attempt */
    if (variant3==1) engine3.cursor_offset=0x301000;
    if (variant3==2) put(THREE_KEY,"accel=attempt\n");
    if (variant3==5) fail_step=5; /* commit cut */
    if (variant3==11) fail_step=6; /* committed BEGIN, failed close */
    if (variant3==14) volume.readonly=true;
    if (variant3==8) pci3.bar[0]|=4; /* unsupported 64-bit BAR, no sizing */
    if (variant3!=3) matrox3d_bind_vfs(&storage);
    matrox3d_boot_diagnostic(&engine3,&fb3);
    unsigned previous_logs=logs3; matrox3d_boot_diagnostic(&engine3,&fb3); assert(logs3==previous_logs);
    assert(!locked3 && !prepared3);
    if (variant3==16) {
        static struct render3d_service attached,second;
        const struct render3d_host host={0,attach_alloc,attach_free,attach_now,0};
        assert(!size_of(THREE_KEY) && commits==2 && !released3 && !quarantined3);
        assert(matrox3d_owner_attach(&attached,&host,43)==-ENODEV);
        assert(!render3d_native_init(&attached,&host));
        struct ciuki_r3d_caps caps; render3d_backend_caps(&attached,&caps);
        assert(caps.max_width==1024 && caps.max_height==768 && caps.max_texture_size==256);
        assert(caps.sampling_features&CIUKI_R3D_PROJECTIVE_BILINEAR);
        assert(matrox3d_owner_attach(&second,&host,42)==-EBUSY);
        assert(!matrox3d_owner_stop(&engine3) && released3==1 && !quarantined3);
        assert(matrox3d_owner_attach(&attached,&host,42)==-ENODEV);
    }
    if (claimed3) assert(reserves3==1);
    if (variant3==0) assert(!logs3 && !claimed3 && !commits);
    else if (variant3==1 || variant3==3 || variant3==8 || variant3==12 || variant3==13 || variant3==15)
        assert(!claimed3 && !commits && !model.writes);
    else if (variant3==7) assert(engine3.quarantined && !engine3.active && quarantined3==1 && !released3 && size_of(THREE_KEY)==14);
    else if (variant3!=16) {
        assert(claimed3==1 && released3==1 && !quarantined3);
        if (variant3==2) assert(size_of(THREE_KEY)==14 && !commits && !truncates && !model.writes);
        if (variant3==6) assert(!size_of(THREE_KEY) && commits==2 && truncates==1 && !model.writes);
        if (variant3==11) assert(size_of(THREE_KEY)==14 && commits==1 && !truncates);
    }
    if (variant3==17) assert(!commits && !model.writes && released3==1);
    assert(variant3==7 ? page_bytes3>0 : page_bytes3==0);
    /* Destroy the emulated device/boot, then free all retained allocations.
     * This is fixture teardown, not a production quarantine-release path. */
    for (unsigned i=0;i<allocations_count3;i++) free(allocations3[i]);
    printf("owner marker/claim pre-write variant %u PASS\n",variant3); return 0;
}
