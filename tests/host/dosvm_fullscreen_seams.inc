/* Physical transport seams. The production state machine is included below. */
#include "../../src/kernel/arch/vga_io.h"
#include <ciuki/display.h>
static uint8_t fs_bitmap[8193];
static cvga_state fs_card;
static bool fs_ports,fs_display,fs_refuse;
static bool fs_stuck_blank;
static unsigned fs_begins,fs_ends;
static int fs_end_result;
static unsigned fs_exit_stage;
static bool fs_return_order;
static unsigned fs_watchdog_returns;
static struct dosvm *fs_expected_owner;
static uint32_t fs_expected_ptes[32];
static const char *fs_fail_checkpoint;
static void (*fs_checkpoint_hook)(const char *);
static unsigned fs_checkpoints;
static bool fs_test_checkpoint_pin;
static unsigned fs_recovery_ms;
static unsigned fs_end_sleeps; /* fx-22: safe points inside the native restore */
static bool fs_vbe_available;
static uint8_t fs_linear[DOS_VBE_BYTES];
static unsigned fs_linear_writes;
static unsigned fs_linear_bytes;
static int fs_linear_error;
static bool fs_direct_available;
static uint32_t fs_direct_phys,fs_direct_cache;
static unsigned fs_direct_bad;
bool vmm_cache_alias_ok(uint32_t physical,uint32_t bytes,uint32_t cache)
{ return physical==fs_direct_phys && bytes<=DOS_VBE_BYTES && cache==fs_direct_cache && fs_direct_bad!=5; }
void framebuffer_write_drain(void) {}
int display_fullscreen_lfb(struct display_lfb *out)
{
    if (!fs_direct_available) return -EOPNOTSUPP;
    const struct dos_video_mode *m=dos_video_mode(fs_expected_owner->vbe_mode);
    CHECK(fs_display && m);
    *out=(struct display_lfb){.physical=fs_direct_phys,.mapped=P2V(fs_direct_phys),
        .bytes=fs_direct_bad==1 ? PAGE_SIZE : DOS_VBE_BYTES,.width=m->width,
        .height=m->height,.pitch=m->pitch,.bpp=m->bpp,.cache_flags=fs_direct_cache};
    if (fs_direct_bad==2) out->pitch++;
    if (fs_direct_bad==3) out->physical++;
    if (fs_direct_bad==4) out->bpp=32;
    return 0;
}
bool display_fullscreen_supported(uint16_t mode)
{ return fs_vbe_available && dos_video_mode(mode); }
int display_fullscreen_write(uint32_t offset,const void *p,uint32_t bytes)
{
    CHECK(fs_display && offset<=DOS_VBE_BYTES && bytes<=DOS_VBE_BYTES-offset);
    if (fs_linear_error) return fs_linear_error;
    memcpy(fs_linear+offset,p,bytes);
    if (fs_direct_available) memcpy(P2V(fs_direct_phys+offset),p,bytes);
    fs_linear_writes++;fs_linear_bytes+=bytes;return 0;
}
void display_fullscreen_return_request(void) {}
int display_fullscreen_checkpoint(const char *step,int result)
{
    (void)result; fs_checkpoints++;
    if (!strcmp(step,"watchdog-return")) fs_watchdog_returns++;
    if (fs_return_order && !strcmp(step,"guest-revoke")) {
        CHECK(!fs_exit_stage);fs_exit_stage=1;
    }
    if (fs_return_order && !strcmp(step,"vga-capture")) {
        CHECK(fs_exit_stage==1 && !fs_ports && !fs_expected_owner->fullscreen);
        for (unsigned i=0;i<32;i++) {
            uint32_t pte;as_lookup(&fs_expected_owner->as,0xA0000+i*4096,&pte);
            CHECK((pte&~0x60u)==(fs_expected_ptes[i]&~0x60u));
        }
        fs_exit_stage=2;
    }
    if (fs_test_checkpoint_pin && !strcmp(step,"vm-park"))
        CHECK(dosvm_destroy(dosvm_focused_key(),true)==-EBUSY);
    if (fs_checkpoint_hook) fs_checkpoint_hook(step);
    return fs_fail_checkpoint && !strcmp(step,fs_fail_checkpoint) ? -EIO : 0;
}
uint32_t grants_display_pid(void) { return 10; }
static uint8_t inb(uint16_t p)
{
    uint8_t byte=cvga_read_port(&fs_card,p,0);
    return fs_stuck_blank && p==0x3C5 && fs_card.seq_index==1 ? byte|0x20 : byte;
}
static void outb(uint16_t p,uint8_t b) { cvga_write_port(&fs_card,p,b); }
void arch_vga_io(bool on) { fs_ports=on; arch_vga_bitmap(fs_bitmap,on); }
void *vmm_map_mmio(uint32_t p,uint32_t bytes,bool uncached)
{ CHECK(p==0xA0000 && bytes==0x20000 && uncached); return P2V(p); }
int display_fullscreen_begin(uint16_t mode)
{ CHECK(cvga_find_mode(mode) || (fs_vbe_available && dos_video_mode(mode))); fs_begins++; if (fs_refuse) return DISPLAY_FULLSCREEN_WINDOWED; CHECK(!fs_display); fs_display=true; return 0; }
int display_fullscreen_end(void) {
    CHECK(fs_display);
    if (fs_direct_available && fs_expected_owner)
        CHECK(!fs_expected_owner->vbe_direct.pages && !fs_expected_owner->vbe_direct.copying);
    if (fs_return_order) { CHECK(fs_exit_stage==2 && !fs_ports);fs_exit_stage=3; }
    g_ticks+=fs_recovery_ms;
    for (unsigned i=0;i<fs_end_sleeps;i++) task_sleep_ms(1);
    fs_display=false; fs_ends++; return fs_end_result;
}
