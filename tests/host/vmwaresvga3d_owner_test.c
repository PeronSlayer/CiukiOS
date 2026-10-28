/* Real marker persistence and production SVGA3D owner against the strict
 * host model. Each process is one boot. GPL-2.0-only. */
#define main old_marker_main
#define klog old_marker_log
#include "display_attempt_test.c"
#undef klog
#undef main
#include <stdlib.h>
#include <ciuki/mm.h>
#include <ciuki/desktop.h>
#include "vmwaresvga3d_model.h"
#define M svga_model
#define THREE_KEY "C:/system/settings/accel-3d-vmwaresvga-15ad-0405.cfg"
struct ciuki_boot_info g_boot;
static struct pci_func pci3={.vendor=0x15ad,.device=0x0405,.class_code=3,.command=3,
    .bar={0xc001,MODEL_FB_PHYS,0xe8000000u}};
static struct resource claim3;
static unsigned claimed3,released3,quarantined3,variant3,logs3,checks2d;
static bool prepared3,qualified_log3,refused_log3;
static char last_log3[512];
static void *allocations3[64]; static unsigned allocation_count3;
static uint32_t page_sizes3[64],page_bytes3;
void klog(const char *fmt,...)
{
    assert(!M.locks);
    va_list ap; va_start(ap,fmt); vsnprintf(last_log3,sizeof(last_log3),fmt,ap); va_end(ap);
    logs3++;
    if (strstr(last_log3,"[3d] probe driver=vmwaresvga result=qualified")) qualified_log3=true;
    if (strstr(last_log3,"[3d] probe driver=vmwaresvga result=") && !strstr(last_log3,"result=qualified")) refused_log3=true;
}
static void *backing_alloc3(size_t n)
{
    assert(!M.locks);
    if (variant3==8 && allocation_count3==2) return 0;
    void *p=calloc(1,n); assert(p && allocation_count3<64); allocations3[allocation_count3++]=p; return p;
}
void kfree(void *p)
{
    assert(!M.locks);
    for (unsigned i=0;i<allocation_count3;i++) if (allocations3[i]==p) { free(p); allocations3[i]=0; return; }
    assert(!p);
}
void *(kmalloc)(size_t n)
{ return n>KHEAP_MAX_REQUEST ? 0 : backing_alloc3(n); }
void *desktop_buffer_alloc(uint32_t bytes,enum mm_owner owner)
{
    assert(bytes && owner==MM_IO_REQUEST);
    void *p=backing_alloc3(bytes);
    if (p) {
        page_sizes3[allocation_count3-1]=bytes;
        page_bytes3+=(bytes+PAGE_SIZE-1)&~(PAGE_SIZE-1);
        assert(page_bytes3+vmsvga3d_backend()->resident_bytes<=R3D_RAM_LIMIT);
    }
    return p;
}
void desktop_buffer_free(void *p,uint32_t bytes,enum mm_owner owner)
{
    assert(owner==MM_IO_REQUEST && p);
    for (unsigned i=0;i<allocation_count3;i++) if (allocations3[i]==p) {
        assert(page_sizes3[i]==bytes);
        page_sizes3[i]=0; page_bytes3-=(bytes+PAGE_SIZE-1)&~(PAGE_SIZE-1);
        kfree(p); return;
    }
    assert(0);
}
const struct pci_func *display_driver_owner(void) { return &pci3; }
uint32_t pmm_free_count(void) { return 10000; }
uint32_t pmm_total_usable(void) { return 20000; }
void registry_inventory(struct registry_inventory_copy *out)
{
    memset(out,0,sizeof(*out));
    if (variant3==10) out->truncated=true;
}
int registry_claim(enum res_type type,uint32_t first,uint32_t end,const char *owner,bool shared)
{
    assert(!M.locks && prepared3 && type==RES_MMIO && !shared && !strcmp(owner,"vmwaresvga-3d"));
    assert(first==MODEL_FB_PHYS+M.device.heap.first && end==MODEL_FB_PHYS+M.device.heap.first+M.device.heap.bytes);
    if (variant3==9) return -EBUSY;
    claimed3++; claim3=(struct resource){.type=type,.start=first,.end=end,.owner=owner,.generation=7,.state=RS_CLAIMED}; return 0;
}
const struct resource *registry_get(unsigned i) { assert(!i && claimed3); return &claim3; }
int registry_activate(int h,gen_t generation) { assert(!h && generation==7); claim3.state=RS_ACTIVE; return 0; }
int registry_quiesce(int h,gen_t generation,registry_idle_proof_fn proof)
{
    assert(!h && generation==7 && !M.locks); if (!proof(h,generation)) return -EIO; claim3.state=RS_QUIESCING; return 0;
}
int registry_release(int h,gen_t generation) { assert(!h && generation==7 && claim3.state==RS_QUIESCING); claim3.state=RS_RELEASED; released3++; return 0; }
int registry_quarantine(int h,gen_t generation) { assert(!h && generation==7); claim3.state=RS_QUARANTINED; quarantined3++; return 0; }
void *vmm_reserve_mmio(uint32_t bytes) { assert(!M.locks && bytes==M.device.heap.bytes); return M.vram; }
void *vmm_replace_mmio(void *p,uint32_t cap,uint32_t phys,uint32_t bytes)
{
    assert(p && cap>=bytes && phys==claim3.start);
    return variant3==4 ? 0 : M.vram+(phys-MODEL_FB_PHYS);
}
void vmm_revoke_mmio(void *p,uint32_t bytes) { assert(p && bytes && !M.locks); }
int fbdev_render3d_prepare(uint32_t generation) { assert(generation==42 && !prepared3); prepared3=true; return 0; }
void fbdev_render3d_prepared(void) { assert(prepared3 && !M.locks); prepared3=false; }
int fbdev_render3d_enter(uint32_t generation)
{
    assert(generation==42 && !opened && !table_open && !M.locks);
    if (variant3==6) return -EBUSY;
    M.locks=1; return 0;
}
void fbdev_render3d_leave(void) { assert(M.locks==1); M.locks=0; }
static int check_2d(void *engine)
{
    assert(engine==&M.engine && M.locks==1 && size_of(THREE_KEY)==14);
    assert(!svga_model_live() && M.engine.cpu_safe);
    checks2d++;
    if (variant3==20) fail_step=step+1; /* Fail marker clear after the check. */
    return variant3==18 ? -EIO : 0;
}
int main(int argc,char **argv)
{
    assert(argc==2); variant3=(unsigned)atoi(argv[1]); reset();
    enum svga_host host=variant3==1 ? SVGA_HOST_QEMU : variant3==14 ? SVGA_HOST_VBOX_GL :
        variant3==15 ? SVGA_HOST_VBOX_DX : SVGA_HOST_VMWARE;
    svga_model_init(256,64,host);
    if (variant3==16) { M.gb_objects=true; assert(!svga_model_engine()); }
    M.engine.self_test=check_2d;
    g_boot.display.vram_bytes=MODEL_VRAM_BYTES;
    strcpy(g_boot.options,variant3==0 ? "x3d=probe 3d=probeX" : "quiet 3d=probe");
    if (variant3==2) put(THREE_KEY,"accel=attempt\n");
    if (variant3==5) fail_step=5;
    if (variant3==7) M.hang=true;
    if (variant3==12) volume.readonly=true;
    if (variant3==13 || variant3==19) M.drop_draws=true;
    if (variant3==19) M.fail_context_destroy=true;
    if (variant3!=3) vmsvga3d_bind_vfs(&storage);
    uint64_t fifo_before=M.fifo_writes;
    vmsvga3d_boot_diagnostic(&M.engine,&M.fb);
    assert(!prepared3 && !M.locks);
    vmsvga3d_boot_diagnostic(&M.engine,&M.fb); /* reported once per boot */
    struct render3d_service *consumer=calloc(1,sizeof(*consumer)); assert(consumer);
    struct render3d_host h={0,svga_model_alloc,svga_model_free,svga_model_now,0};
    int attached=vmsvga3d_owner_attach(consumer,&h,42);
    bool success=variant3==11 || variant3==14 || variant3==16 || variant3==17;
    if (success) {
        assert(checks2d==1);
        assert(!attached && qualified_log3 && !refused_log3 && !size_of(THREE_KEY) && M.draws && M.context);
        assert(claimed3==1 && !released3 && !quarantined3 && !svga_model_live() && M.max_live<=VMSVGA3D_SIDS);
        assert(vmsvga3d_owner_attach(consumer,&h,42)==-EBUSY);
        struct ciuki_r3d_caps caps; render3d_backend_caps(consumer,&caps);
        assert(caps.max_width==256 && caps.max_height==64 && caps.combine_modes==(variant3==14 ? 3u : 15u));
        if (variant3==16) assert(M.dev_cap_reads==12);
        if (variant3==17) {
            /* A 2D fault after admission: 3D is lost too, its claim quarantined. */
            uint32_t context,color;
            assert(!render3d_create_context(consumer,7,R3D_HARDWARE_REQUIRED,&context));
            assert(!render3d_create_resource(consumer,7,context,R3D_COLOR,64,64,0,0,&color));
            void *shadow=consumer->contexts[0].resources[0].shadow;
            uint32_t charged=consumer->ram_bytes;
            vmsvga_engine_quarantine(&M.engine,"test_fault");
            assert(vmsvga3d_owner_stop(&M.engine)==-EIO && quarantined3==1 && !released3 && M.context);
            assert(consumer->quarantined && !consumer->admission && consumer->ram_bytes==charged &&
                   consumer->contexts[0].resources[0].shadow==shadow);
            assert(render3d_create_context(consumer,7,R3D_AUTO,&context)==-ENODEV);
            assert(render3d_close(consumer)==-EIO && consumer->ram_bytes==charged);
            assert(vmsvga3d_owner_attach(consumer,&h,42)==-ENODEV);
            /* Test process teardown after the modeled device is abandoned. */
            free(shadow); free(consumer->contexts[0].resources);
            free(consumer);
            for (unsigned i=0;i<allocation_count3;i++) free(allocations3[i]);
            svga_model_destroy();
            printf("PASS SVGA3D real owner/marker/claim boot variant %u (qualified, then 2D fault: quarantined)\n",variant3);
            return 0;
        }
        /* Stop under the presenter: consumer closed, context destroyed, claim released. */
        assert(!vmsvga3d_owner_stop(&M.engine)); assert(released3==1 && !M.context && M.context_destroys==1);
        assert(!vmsvga3d_owner_stop(&M.engine));
        assert(vmsvga3d_owner_attach(consumer,&h,42)==-ENODEV);
    } else {
        assert(attached==-ENODEV);
        if (variant3==0) assert(!logs3 && !claimed3 && fifo_before==M.fifo_writes);
        if (variant3==1) assert(refused_log3 && strstr(last_log3,"reason=no_svga3d") && !claimed3 && fifo_before==M.fifo_writes);
        if (variant3==7 || variant3==18 || variant3==19)
            assert(M.engine.quarantined && quarantined3 && !released3 && size_of(THREE_KEY)==14);
        else assert(!claimed3 || released3==1);
        if (variant3==18) assert(checks2d==1);
        if (variant3==19) assert(!checks2d && M.context_destroys==1);
        if (variant3==20) assert(checks2d==2 && !M.context && released3==1 && size_of(THREE_KEY)==14);
        if (variant3==2) assert(!commits && size_of(THREE_KEY)==14 && strstr(last_log3,"retained_marker") &&
                               strstr(last_log3,"marker=retained"));
        if (variant3==13 || variant3==15) {
            assert(M.batches && !M.engine.quarantined && M.engine.active && !size_of(THREE_KEY));
            assert(M.context_destroys==1 && !M.context && !svga_model_live() && strstr(last_log3,"minimum_probe"));
        }
        if (variant3<18 && variant3!=13 && variant3!=15 && variant3!=7 && variant3!=6) assert(fifo_before==M.fifo_writes);
        if (variant3==6) assert(fifo_before==M.fifo_writes && !M.context);
        assert(!vmsvga3d_owner_stop(&M.engine) || variant3==7 || variant3==18 || variant3==19);
    }
    if (variant3==2) {
        static const struct display_driver declaration={.id="vmwaresvga",.attempt_files={
            "C:/system/settings/vmsvga.cfg","C:/system/settings/accel-3d-vmwaresvga-15ad-%04x.cfg"}};
        put(DISPLAY_RETRY_PATH,"driver=vmwaresvga\npci=15ad:0405\n");
        retry_mode=true; bool retained=true;
        assert(!display_attempt_prepare(&storage,&declaration,&pci3,&retained));
        assert(!retained && !size_of(THREE_KEY));
    }
    if (!M.engine.quarantined) assert(!page_bytes3);
    free(consumer);
    for (unsigned i=0;i<allocation_count3;i++) free(allocations3[i]);
    svga_model_destroy();
    printf("PASS SVGA3D real owner/marker/claim boot variant %u%s\n",variant3,success ? " (qualified, stopped cleanly)" : "");
    return 0;
}
