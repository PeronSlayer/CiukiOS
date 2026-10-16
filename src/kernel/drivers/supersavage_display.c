/* Exclusive native scanout provider for 5333:8c2e. Port/MMIO ownership spans
 * guest execution. The display service owns mediated recovery after release.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/cpu.h>
#include <ciuki/display.h>
#include <ciuki/registry.h>
#include <ciuki/timing.h>
#include <ciuki/supersavage_display.h>
#include "supersavage_mode.h"
#define MODE_MMIO_BYTES 0x80000u
static struct supersavage_mode saved_mode;
static const struct pci_func *adapter;
static void *register_window,*aperture_window;
static volatile uint8_t *register_mapping,*aperture_mapping;
static int claims[3]={-1,-1,-1};
static gen_t generations[3];
static bool poisoned;
static uint8_t native_in(void *arg,uint16_t port) { (void)arg; return inb(port); }
static void native_out(void *arg,uint16_t port,uint8_t v) { (void)arg; outb(port,v); }
static uint8_t native_read(void *arg,unsigned at) { (void)arg; return aperture_mapping[at]; }
static void native_write(void *arg,unsigned at,uint8_t v) { (void)arg; aperture_mapping[at]=v; }
static uint32_t native_read32(void *arg,unsigned at)
{
    (void)arg;
    /* Packed register readback requires low/high halfword bus cycles. */
    uint32_t lo=*(volatile uint16_t *)(register_mapping+at);
    return lo|(uint32_t)*(volatile uint16_t *)(register_mapping+at+2)<<16;
}
static void native_write32(void *arg,unsigned at,uint32_t v)
{ (void)arg; *(volatile uint32_t *)(register_mapping+at)=v; }
static int native_delay(void *arg,unsigned us) { (void)arg; return udelay(us); }
static uint64_t native_now(void *arg)
{
    (void)arg;
    if (!g_cpu_tsc || !g_tsc_per_ms) return g_ticks*1000;
    uint64_t cycles=ktime_cycles();
    return cycles/g_tsc_per_ms*1000+cycles%g_tsc_per_ms*1000/g_tsc_per_ms;
}
static int native_checkpoint(void *arg,const char *step,int result)
{ (void)arg; return display_fullscreen_checkpoint(step,result); }
static const struct supersavage_mode_io io={
    {0,native_in,native_out,native_read,native_write},
    native_read32,native_write32,native_delay,native_now,native_checkpoint
};
static bool extent(uint32_t start,uint32_t bytes)
{
    uint64_t end=(uint64_t)start+bytes;
    if (start<0x100000 || end>UINT32_MAX || (start<g_boot.kernel_end && end>g_boot.kernel_start)) return false;
    for (unsigned i=0;i<g_boot.e820_count;i++) {
        const struct ciuki_e820 *m=&g_boot.e820[i];
        if (m->type==CBI_E820_RAM && start<m->base+m->length && m->base<end) return false;
    }
    return true;
}
int supersavage_display_prepare(const struct fb_device *f,uint16_t mode)
{
    if (poisoned) return -EIO;
    if (saved_mode.saved || claims[0]>=0) return -EBUSY;
    adapter=0;
    unsigned displays=0;
    for (unsigned i=0;i<pci_count();i++) {
        const struct pci_func *p=pci_get(i);
        if (p && p->class_code==3) { displays++; adapter=p; }
    }
    if (displays!=1 || !adapter || adapter->vendor!=0x5333 || adapter->device!=0x8C2E ||
        !supersavage_mode_supported(mode) || !f || !f->present || (adapter->command&3)!=3 ||
        (adapter->bar[0]&7) || (adapter->bar[1]&7) ||
        !extent(adapter->bar[0]&~15u,MODE_MMIO_BYTES) ||
        ((adapter->bar[0]&~15u)&(MODE_MMIO_BYTES-1)) ||
        (adapter->bar[1]&~15u)!=f->phys || !extent(f->phys,f->size) ||
        ((uint64_t)(adapter->bar[0]&~15u)<(uint64_t)f->phys+f->size &&
         f->phys<(uint64_t)(adapter->bar[0]&~15u)+MODE_MMIO_BYTES)) return -EOPNOTSUPP;
    if (!register_window) register_window=vmm_reserve_mmio(MODE_MMIO_BYTES);
    if (!aperture_window) aperture_window=vmm_reserve_mmio(0x20000);
    return register_window && aperture_window ? 0 : -ENOMEM;
}
static bool native_idle(int h,gen_t generation)
{
    (void)h; (void)generation;
    /* A retained RAM snapshot is not a hardware writer. Programming is
     * synchronous CPU I/O only: no DMA, IRQ, worker or callback to drain. */
    return !register_mapping && !aperture_mapping;
}
static int release_claims(void)
{
    if (register_mapping) vmm_revoke_mmio((void *)register_mapping,MODE_MMIO_BYTES);
    if (aperture_mapping) vmm_revoke_mmio((void *)aperture_mapping,0x20000);
    register_mapping=aperture_mapping=0;
    int e=0;
    for (unsigned i=0;i<3;i++) if (claims[i]>=0) {
        const struct resource *rsc=registry_get(claims[i]);
        if (!rsc || rsc->state!=RS_ACTIVE) {
            registry_quarantine(claims[i],generations[i]); e=-EIO; poisoned=true; continue;
        }
        int r=registry_quiesce(claims[i],generations[i],native_idle);
        if (!r) r=registry_release(claims[i],generations[i]);
        if (r) { registry_quarantine(claims[i],generations[i]); e=-EIO; poisoned=true; }
        else claims[i]=-1;
    }
    return e;
}
static void outcome(const char *phase,int e)
{
    const char *stage=saved_mode.bad_stage ? saved_mode.bad_stage : saved_mode.stage;
    unsigned reg=saved_mode.bad_reg;
    klog("[display] native_supersavage phase=%s result=%d stage=%s reg=%x reg_space=%s index=%x expected=%08x actual=%08x mask=%08x restore_us=%llu restore_budget_us=200000",
        phase,e,stage ? stage : "claims",reg,
        reg>=0x8180 ? "mmio" : reg>=0x100 && reg<0x200 ? "sr" : "vga",
        reg>=0x100 && reg<0x200 ? reg-0x100 : reg,
        saved_mode.expected,saved_mode.actual,saved_mode.mask,saved_mode.restore_us);
}
int supersavage_display_begin(uint16_t mode)
{
    if (!adapter || poisoned || !register_window || !aperture_window) return -EIO;
    int e=display_fullscreen_checkpoint("native-claim",0);
    if (e) return e;
    const enum res_type types[]={RES_PORT,RES_MMIO,RES_MMIO};
    const uint32_t starts[]={0x3B0,adapter->bar[0]&~15u,0xA0000};
    const uint32_t lengths[]={0x30,MODE_MMIO_BYTES,0x20000};
    for (unsigned i=0;i<3;i++) {
        claims[i]=registry_claim(types[i],starts[i],starts[i]+lengths[i],"supersavage-mode",false);
        if (claims[i]<0) { e=claims[i]; claims[i]=-1; goto failed; }
        generations[i]=registry_get(claims[i])->generation;
        if ((e=registry_activate(claims[i],generations[i]))) goto failed;
    }
    register_mapping=vmm_replace_mmio(register_window,MODE_MMIO_BYTES,starts[1],MODE_MMIO_BYTES);
    aperture_mapping=vmm_replace_mmio(aperture_window,0x20000,0xA0000,0x20000);
    if (!register_mapping || !aperture_mapping) { e=-ENOMEM; goto failed; }
    e=supersavage_mode_save(&saved_mode,&io);
    klog("[display] native_supersavage state result=%d cr31=%02x cr3a=%02x cr58=%02x cr66=%02x cr67=%02x cr69=%02x panel=%02x sr12=%02x sr13=%02x sr29=%02x sr15=%02x sr1b=%02x sr54=%02x sr56=%02x sr59=%02x sr5b=%02x",
        e,saved_mode.cr[0x31],saved_mode.cr[0x3A],saved_mode.cr[0x58],saved_mode.cr[0x66],
        saved_mode.cr[0x67],saved_mode.cr[0x69],saved_mode.cr[0x6B],saved_mode.sr[0x12],
        saved_mode.sr[0x13],saved_mode.sr[0x29],saved_mode.sr[0x15],saved_mode.sr[0x1B],
        saved_mode.sr[0x54],saved_mode.sr[0x56],saved_mode.sr[0x59],saved_mode.sr[0x5B]);
    if (!e) e=supersavage_mode_enter(&saved_mode,&io,mode);
    outcome("enter",e);
    /* display_fullscreen_end owns rollback, including a partial save. */
    return e;
failed:
    outcome("claim",e);
    int released=release_claims();
    return released ? released : e;
}
int supersavage_display_end(void)
{
    if (poisoned) return -EIO; /* aliases were revoked on the first failure */
    int e=supersavage_mode_restore(&saved_mode,&io);
    outcome("restore",e);
    if (e) poisoned=true;
    /* Even failed readback must relinquish CPU access so the display service
     * can attempt a VBE mode set. Registry failures still quarantine. */
    int released=release_claims();
    return e ? e : released;
}
