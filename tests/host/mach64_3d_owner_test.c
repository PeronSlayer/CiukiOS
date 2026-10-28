/* Production Mach64 3D owner against the real keyed-marker implementation,
 * the committed/cache VFS fixture and the independent register model.
 * Each process is one boot. SPDX-License-Identifier: GPL-2.0-only */
#define main old_marker_main
#define klog old_marker_log
#include "display_attempt_test.c"
#undef klog
#undef main
#include <stdlib.h>
#include <ciuki/mm.h>
#include "mach64_3d_model.h"
#define THREE_KEY "C:/system/settings/accel-3d-mach64-1002-4c4d.cfg"
struct ciuki_boot_info g_boot;
static struct pci_func pci3={.vendor=0x1002,.device=0x4c4d,.class_code=3,.command=3,.revision=0x64,
    .bar={0xe0000000,0,0xe1000000}};
static struct resource claim3,reserved3;
static unsigned variant3,claimed3,released3,quarantined3,logs3;
static bool prepared3;
static void *allocations3[64]; static unsigned allocation_count3;
static char lines3[16][512]; static unsigned line_count3;
void klog(const char *fmt,...)
{
    assert(!m64m.locks); logs3++;
    char line[512]; va_list ap; va_start(ap,fmt); vsnprintf(line,sizeof(line),fmt,ap); va_end(ap);
    assert(strstr(line,"[3d] probe") || strstr(line,"[3d] fullsize") || strstr(line,"[3d] precision") || strstr(line,"[accel] retry "));
    if (line_count3<16) strcpy(lines3[line_count3++],line);
    if (getenv("M64_TRACE")) fprintf(stderr,"%s\n",line);
}
static bool logged(const char *text)
{
    for (unsigned i=0;i<line_count3;i++) if (strstr(lines3[i],text)) return true;
    return false;
}
void *kmalloc(size_t n)
{
    assert(!m64m.locks);
    if (variant3==8 && allocation_count3==3) return 0;
    void *p=calloc(1,n); assert(p && allocation_count3<64); allocations3[allocation_count3++]=p; return p;
}
void kfree(void *p)
{
    assert(!m64m.locks);
    for (unsigned i=0;i<allocation_count3;i++) if (allocations3[i]==p) { free(p); allocations3[i]=0; return; }
    assert(!p);
}
const struct pci_func *display_driver_owner(void) { return &pci3; }
uint32_t pmm_free_count(void) { return 10000; }
void registry_inventory(struct registry_inventory_copy *out)
{
    memset(out,0,sizeof(*out));
    if (variant3==10) out->truncated=true;
    if (variant3==11) { out->resource_count=1; out->resources[0]=reserved3; }
}
int registry_claim(enum res_type type,uint32_t first,uint32_t end,const char *owner,bool shared)
{
    assert(!m64m.locks && prepared3 && type==RES_MMIO && !shared && !strcmp(owner,"mach64-3d") && !claimed3);
    assert(first>=0xe0000000u+m64m.engine.fb.size && end<=0xe0000000u+0x7ff000u && !(first&4095) && !(end&4095));
    if (variant3==11) assert(first>=reserved3.end || end<=reserved3.start);
    if (variant3==9) return -EBUSY;
    claimed3++; claim3=(struct resource){.type=type,.start=first,.end=end,.owner=owner,.generation=7,.state=RS_CLAIMED};
    return 0;
}
const struct resource *registry_get(unsigned i) { assert(!i && claimed3); return &claim3; }
int registry_activate(int h,gen_t g) { assert(!h && g==7 && claim3.state==RS_CLAIMED); claim3.state=RS_ACTIVE; return 0; }
int registry_quiesce(int h,gen_t g,registry_idle_proof_fn proof)
{
    assert(!h && g==7 && !m64m.locks && claim3.state==RS_ACTIVE);
    if (!proof(h,g)) return -EIO;
    claim3.state=RS_QUIESCING; return 0;
}
int registry_release(int h,gen_t g)
{ assert(!h && g==7 && claim3.state==RS_QUIESCING); claim3.state=RS_RELEASED; released3++; return 0; }
int registry_quarantine(int h,gen_t g) { assert(!h && g==7); claim3.state=RS_QUARANTINED; quarantined3++; return 0; }
void *vmm_reserve_mmio(uint32_t bytes) { assert(!m64m.locks && bytes<=0x7ff000u); return m64m.vram; }
void *vmm_replace_mmio(void *p,uint32_t capacity,uint32_t phys,uint32_t bytes)
{
    assert(p==m64m.vram && capacity>=bytes && phys==claim3.start && bytes==claim3.end-claim3.start);
    return variant3==4 ? 0 : m64m.vram+(phys-0xe0000000u);
}
void vmm_revoke_mmio(void *p,uint32_t bytes) { assert(p && bytes && !m64m.locks); }
int fbdev_render3d_prepare(uint32_t g) { assert(g==42 && !prepared3 && !m64m.locks); prepared3=true; return 0; }
void fbdev_render3d_prepared(void) { assert(prepared3 && !m64m.locks); prepared3=false; }
int fbdev_render3d_enter(uint32_t g)
{
    assert(prepared3 && g==42 && !opened && !table_open && !m64m.locks);
    if (variant3==6) return -EBUSY;
    m64m.locks=1; return 0;
}
void fbdev_render3d_leave(void) { assert(m64m.locks==1); m64m.locks=0; }
int main(int argc,char **argv)
{
    assert(argc==2); variant3=(unsigned)atoi(argv[1]); reset();
    unsigned w=variant3==18 ? 1024 : 320,h=variant3==18 ? 768 : 240;
    m64m_init(w,h);
    g_boot.display.vram_bytes=M64M_VRAM;
    strcpy(g_boot.options,variant3==0 ? "x3d=probe 3d=probeX" : "accel=1 3d=probe");
    reserved3=(struct resource){.type=RES_MMIO,.start=0xe0000000u+0x400000u,.end=0xe0000000u+0x401000u,.state=RS_ACTIVE};
    if (variant3==11) memset(m64m.vram+0x400000u,0x6d,4096);
    switch (variant3) {
    case 1: m64m.engine.quarantined=true; m64m.engine.active=false; break;
    case 2: put(THREE_KEY,"accel=attempt\n"); break;
    case 5: fail_step=5; break;
    case 7: m64m.dead=true; break;
    case 12: volume.readonly=true; break;
    case 13: m64m.block1_off=true; break;
    case 14: m64m.mirror_off=true; break;
    case 16: m64m.wrong_tie=true; break;
    case 17: m64m.corrupt_guard=true; break;
    case 19: m64m.bad_selfcheck=true; break;
    case 20: pci3.device=0x4742; break;
    }
    if (variant3!=3) mach64_3d_bind_vfs(&storage);
    mach64_3d_boot_diagnostic(&m64m.engine,&m64m.engine.fb);
    unsigned previous=logs3; mach64_3d_boot_diagnostic(&m64m.engine,&m64m.engine.fb); assert(logs3==previous);
    assert(!prepared3 && !m64m.locks);
    struct render3d_service *consumer=calloc(1,sizeof(*consumer)); assert(consumer);
    struct render3d_host host={0,m64m_alloc,m64m_free,m64m_now,0};
    int attached=mach64_3d_owner_attach(consumer,&host,42);
    switch (variant3) {
    case 0: assert(!logs3 && !claimed3 && !m64m.writes && !commits); break;
    case 1: case 3: case 6: case 10: case 13: case 14: case 20:
        assert(!claimed3 && !m64m.writes && !m64m.memory_writes && !commits && attached==-ENODEV);
        if (variant3==13) assert(logged("reason=block1_disabled") && logged("bus_cntl=7333a040"));
        if (variant3==14 || variant3==20) assert(logged("reason=aperture_layout"));
        break;
    case 7: assert(!claimed3 && !m64m.writes && logged("reason=register_preflight") && !m64m.engine.quarantined); break;
    case 2: case 4: case 5: case 8: case 9: case 12:
        assert(!m64m.writes && attached==-ENODEV && !m64m.engine.quarantined);
        assert(variant3==9 ? !claimed3 : claimed3==1 && released3==1 && !quarantined3);
        if (variant3==2) assert(size_of(THREE_KEY)==14 && logged("reason=retained_marker"));
        break;
    case 11: case 15:
        assert(!attached && claimed3==1 && !released3 && !quarantined3 && !size_of(THREE_KEY));
        assert(m64m.engine.render3d_qualified && logged("result=qualified") && logged("[3d] fullsize result=qualified"));
        assert(logged("[3d] precision result=qualified"));
        {
            struct ciuki_r3d_caps caps; render3d_backend_caps(consumer,&caps);
            assert(caps.max_width==320 && caps.max_height==240 && caps.max_pixel_work==2*320*240);
        }
        /* Revocation: close admission, then release the heap claim. */
        assert(!mach64_3d_owner_stop(&m64m.engine) && released3==1 && !m64m.engine.render3d_qualified);
        assert(!mach64_3d_owner_stop(&m64m.engine));
        free(consumer); consumer=calloc(1,sizeof(*consumer)); assert(consumer);
        assert(mach64_3d_owner_attach(consumer,&host,42)==-ENODEV);
        break;
    case 16:
        assert(attached==-ENODEV && claimed3==1 && released3==1 && !quarantined3 && !m64m.engine.quarantined);
        assert(!size_of(THREE_KEY) && logged("reason=minimum_probe") && logged("coverage=1"));
        break;
    case 17: case 19:
        assert(attached==-ENODEV && m64m.engine.quarantined && quarantined3 && !released3 && size_of(THREE_KEY)==14);
        assert(logged("result=failed"));
        if (variant3==19) assert(logged("selfcheck_error=-5"));
        break;
    case 18:
        assert(attached==-ENODEV && claimed3==1 && released3==1 && !quarantined3 && m64m.draws);
        assert(!size_of(THREE_KEY) && logged("reason=fullsize_heap") && logged("fullsize=0"));
        break;
    default: assert(0);
    }
    if (variant3==11) for (unsigned i=0;i<4096;i++) assert(m64m.vram[0x400000u+i]==0x6d);
    if (variant3==2) {
        static const struct display_driver declaration={.id="mach64",.attempt_files={LEGACY,
            "C:/system/settings/accel-3d-mach64-1002-%04x.cfg"}};
        static const char other[]="C:/system/settings/accel-3d-mach64-1002-4742.cfg";
        put(other,"accel=attempt\n"); put(DISPLAY_RETRY_PATH,"driver=mach64\npci=1002:4c4d\n");
        retry_mode=true; bool retained=true;
        assert(!display_attempt_prepare(&storage,&declaration,&pci3,&retained));
        assert(!retained && !size_of(THREE_KEY) && size_of(other)==14);
    }
    free(consumer);
    /* Destroying the model is the reboot, including retained fault data. */
    for (unsigned i=0;i<allocation_count3;i++) free(allocations3[i]);
    m64m_destroy();
    printf("PASS mach64 real owner/marker/claim boot variant %u\n",variant3);
    return 0;
}
