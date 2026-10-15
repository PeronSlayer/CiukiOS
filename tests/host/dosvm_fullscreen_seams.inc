/* Physical transport seams. The production state machine is included below. */
#include "../../src/kernel/arch/vga_io.h"
static uint8_t fs_bitmap[8193];
static cvga_state fs_card;
static bool fs_ports,fs_display,fs_refuse;
static bool fs_stuck_blank;
static unsigned fs_begins,fs_ends;
static const char *fs_fail_checkpoint;
static unsigned fs_checkpoints;
static bool fs_test_checkpoint_pin;
void display_fullscreen_return_request(void) {}
int display_fullscreen_checkpoint(const char *step,int result)
{
    (void)result; fs_checkpoints++;
    if (fs_test_checkpoint_pin && !strcmp(step,"vm-park"))
        CHECK(dosvm_destroy(dosvm_focused_key(),true)==-EBUSY);
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
{ CHECK(cvga_find_mode(mode)); fs_begins++; if (fs_refuse) return -EBUSY; CHECK(!fs_display); fs_display=true; return 0; }
int display_fullscreen_end(void) { CHECK(fs_display); fs_display=false; fs_ends++; return 0; }
