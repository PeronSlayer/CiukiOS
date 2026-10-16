/* Display-provider ownership lifecycle, with the production register model.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main register_model_main
#include "supersavage_mode_test.c"
#undef main
#include <ciuki/cpu.h>
#include <ciuki/registry.h>
#include <ciuki/supersavage_display.h>
#include <ciuki/fbdev.h>
#include <stdlib.h>
static struct pci_func gpu={.vendor=0x5333,.device=0x8C2E,.class_code=3,.command=3,.bar={0xF0000000,0xE0000000}};
struct ciuki_boot_info g_boot;
volatile uint64_t g_ticks;
bool g_cpu_tsc;
uint64_t g_tsc_per_ms;
static struct resource leases[8];
static unsigned nleases,claims_attempted,mapped,revoked;
static unsigned fail_claim,fail_map,fail_activate;
static bool extra_gpu;
static const char *runtime_fail_step;
static void *windows[2];
static unsigned nwindows;
unsigned pci_count(void) { return extra_gpu ? 2 : 1; }
const struct pci_func *pci_get(unsigned i) { return i<pci_count() ? &gpu : 0; }
int registry_claim(enum res_type t,uint32_t start,uint32_t end,const char *owner,bool share)
{
    assert(!share && (!strcmp(owner,"supersavage-mode") || !strcmp(owner,"firmware-test")));
    if (++claims_attempted==fail_claim) return -EBUSY;
    for (unsigned i=0;i<nleases;i++)
        assert(leases[i].state==RS_RELEASED || leases[i].end<=start || leases[i].start>=end || leases[i].type!=t);
    assert(nleases<8);unsigned h=nleases++;
    leases[h]=(struct resource){t,start,end,owner,h+1,RS_CLAIMED,false};return (int)h;
}
const struct resource *registry_get(unsigned h) { assert(h<nleases);return &leases[h]; }
int registry_activate(int h,gen_t g)
{ assert(leases[h].generation==g && leases[h].state==RS_CLAIMED);if ((unsigned)h+1==fail_activate) return -EIO;leases[h].state=RS_ACTIVE;return 0; }
int registry_quiesce(int h,gen_t g,registry_idle_proof_fn proof)
{ assert(leases[h].generation==g && leases[h].state==RS_ACTIVE);leases[h].state=RS_QUIESCING;assert(proof(h,g));return 0; }
int registry_release(int h,uint32_t g)
{ assert(leases[h].generation==g && leases[h].state==RS_QUIESCING);leases[h].state=RS_RELEASED;return 0; }
int registry_quarantine(int h,gen_t g)
{ assert(leases[h].generation==g);leases[h].state=RS_QUARANTINED;return 0; }
void *vmm_reserve_mmio(uint32_t bytes)
{ assert(nwindows<2);windows[nwindows]=calloc(1,bytes);assert(windows[nwindows]);return windows[nwindows++]; }
void *vmm_replace_mmio(void *window,uint32_t cap,uint32_t phys,uint32_t bytes)
{ (void)phys;assert(window && cap==bytes);return ++mapped==fail_map ? 0 : window; }
void vmm_revoke_mmio(void *window,uint32_t bytes) { assert(window && bytes);revoked++; }
int udelay(uint32_t us) { card.us+=us;return 0; }
uint64_t ktime_cycles(void) { return card.us; }
int display_fullscreen_checkpoint(const char *step,int error)
{ (void)error;return runtime_fail_step && !strcmp(runtime_fail_step,step) ? -EIO : 0; }
void klog(const char *fmt,...) { (void)fmt; }
static uint8_t port_in(uint16_t p) { return input(&card,p); }
static void port_out(uint16_t p,uint8_t v) { output(&card,p,v); }
static int mode_save(struct supersavage_mode *s,const struct supersavage_mode_io *real)
{
    assert(real && mapped==2 && leases[0].state==RS_ACTIVE && leases[1].state==RS_ACTIVE && leases[2].state==RS_ACTIVE);
    return supersavage_mode_save(s,&bus);
}
static int mode_enter(struct supersavage_mode *s,const struct supersavage_mode_io *real,uint16_t mode)
{ (void)real;return supersavage_mode_enter(s,&bus,mode); }
static int mode_restore(struct supersavage_mode *s,const struct supersavage_mode_io *real)
{ (void)real;return supersavage_mode_restore(s,&bus); }
#define inb port_in
#define outb port_out
#define supersavage_mode_save mode_save
#define supersavage_mode_enter mode_enter
#define supersavage_mode_restore mode_restore
#include "../../src/kernel/drivers/supersavage_display.c"
#undef supersavage_mode_save
#undef supersavage_mode_enter
#undef supersavage_mode_restore
#undef inb
#undef outb
static struct fb_device desktop;
static void runtime_fixture(void)
{
    for (unsigned i=0;i<nwindows;i++) free(windows[i]);nwindows=0;
    register_window=aperture_window=0;register_mapping=aperture_mapping=0;
    memset(&saved_mode,0,sizeof(saved_mode));poisoned=false;adapter=0;
    for (unsigned i=0;i<3;i++) claims[i]=-1;
    memset(leases,0,sizeof(leases));nleases=claims_attempted=mapped=revoked=0;
    fail_claim=fail_map=fail_activate=0;extra_gpu=false;runtime_fail_step=0;
    fixture();gpu.command=3;gpu.bar[0]=0xF0000000;gpu.bar[1]=0xE0000000;
    g_boot=(struct ciuki_boot_info){.kernel_start=0x100000,.kernel_end=0x500000,.e820_count=1};
    g_boot.e820[0]=(struct ciuki_e820){0x100000,0x7F00000,CBI_E820_RAM,1};
    desktop=(struct fb_device){.present=true,.phys=0xE0000000,.size=1024*768*4};
}
static void ownership(void)
{
    runtime_fixture();gpu.command=2;
    assert(supersavage_display_prepare(&desktop,0x13)==-EOPNOTSUPP && !claims_attempted && !card.reads && !nwindows);
    gpu.command=3;extra_gpu=true;assert(supersavage_display_prepare(&desktop,0x13)==-EOPNOTSUPP);
    extra_gpu=false;gpu.bar[0]=0x100000;assert(supersavage_display_prepare(&desktop,0x13)==-EOPNOTSUPP);
    gpu.bar[0]=0xF0000000;
    assert(!supersavage_display_prepare(&desktop,0x13) && !claims_attempted && !card.reads);
    assert(!supersavage_display_begin(0x13));
    assert(leases[0].start==0x3B0 && leases[0].end==0x3E0 && leases[1].start==0xF0000000 &&
           leases[1].end==0xF0080000 && leases[2].start==0xA0000 && leases[2].end==0xC0000);
    /* Exercise the real halfword MMIO transport on the reserved window. */
    native_write32(NULL,0x81C8,0x12345678);assert(native_read32(NULL,0x81C8)==0x12345678);
    assert(!supersavage_display_end() && revoked==2 && !poisoned);
    for (unsigned i=0;i<3;i++) assert(leases[i].state==RS_RELEASED);
    assert(!memcmp(card.vga.plane,original.vga.plane,sizeof(card.vga.plane)));
    puts("PASS SuperSavage provider ownership: side-effect-free PCI/BAR/decode preflight, exclusive VGA/MMIO/A000 leases, halfword stream readback, revoked aliases before release");
}
static void refusal_and_recovery(void)
{
    for (unsigned i=1;i<=3;i++) {
        runtime_fixture();assert(!supersavage_display_prepare(&desktop,0x13));fail_claim=i;
        assert(supersavage_display_begin(0x13)==-EBUSY && !card.reads);
        assert(!supersavage_display_end());
        for (unsigned j=0;j<nleases;j++) assert(leases[j].state==RS_RELEASED);
    }
    for (unsigned i=1;i<=2;i++) {
        runtime_fixture();assert(!supersavage_display_prepare(&desktop,0x13));fail_map=i;
        assert(supersavage_display_begin(0x13)==-ENOMEM && !card.reads);
        assert(!supersavage_display_end());
    }
    runtime_fixture();assert(!supersavage_display_prepare(&desktop,0x13));fail_activate=2;
    assert(supersavage_display_begin(0x13)==-EIO && poisoned && leases[1].state==RS_QUARANTINED && !card.reads);
    runtime_fixture();assert(!supersavage_display_prepare(&desktop,0x13));card.cr[0x6B]=0x82;
    assert(supersavage_display_begin(0x13)==-EOPNOTSUPP && !card.memcycles);
    assert(!supersavage_display_end() && !poisoned);
    runtime_fixture();assert(!supersavage_display_prepare(&desktop,0x13));assert(!supersavage_display_begin(0x13));
    card.sr[0x12]^=1;card.drop_sr=0x12;
    assert(supersavage_display_end()==-EIO && poisoned && saved_mode.saved && revoked==2);
    for (unsigned i=0;i<3;i++) assert(leases[i].state==RS_RELEASED);
    /* Failure of mode proof does not keep CPU-only access claims live.
     * A mediated firmware caller can acquire them without quarantine reset. */
    int h=registry_claim(RES_PORT,0x3B0,0x3E0,"firmware-test",false);
    assert(h>=0 && !registry_activate(h,leases[h].generation));
    h=registry_claim(RES_MMIO,0xA0000,0xC0000,"firmware-test",false);
    assert(h>=0 && !registry_activate(h,leases[h].generation));
    assert(supersavage_display_prepare(&desktop,0x13)==-EIO);
    assert(supersavage_display_end()==-EIO && revoked==2); /* no access through revoked aliases */
    puts("PASS SuperSavage provider failures: claim/map rollback, unsupported panel locks, activation failure quarantined; failed native restore retains snapshot/blocks retry but releases revoked CPU leases for firmware recovery");
    runtime_fixture();card.sr[0x58]=0;
    assert(!supersavage_display_prepare(&desktop,0x13) && !supersavage_display_begin(0x13));
    card.sr[0x58]=8;
    assert(!supersavage_display_end() && !poisoned && !saved_mode.saved && revoked==2);
    for (unsigned i=0;i<3;i++) assert(leases[i].state==RS_RELEASED);
    puts("PASS f3-36 provider retained SR58 bit 3: successful restore and complete lease return");
}
int main(void)
{
    ownership();refusal_and_recovery();
    for (unsigned i=0;i<nwindows;i++) free(windows[i]);
    puts("SuperSavage display provider host tests: PASS");return 0;
}
