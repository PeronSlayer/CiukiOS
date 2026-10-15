/* Production console with counted row claims and a RAM framebuffer.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/fbdev.h>
#define CIUKI_CPU_H
#define P2V(p) ((void *)(uintptr_t)(p))
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"console:%d: %s\n",__LINE__,#c);exit(1); } } while (0)
struct ciuki_boot_info g_boot;
volatile uint64_t g_ticks;
static uint8_t pixels[640*480*4];
const uint8_t font_cfn_regular[95+95*32]={0};
static uint32_t flags=0x200;
static unsigned claims,moves,points,locks;
static bool take_display, claim_held, deny_claim;
static uint32_t irq_save(void) { uint32_t f=flags;flags=0;locks++;return f; }
static void irq_restore(uint32_t f) { flags=f; }
static struct fb_device device;
const struct fb_device *fbdev_get(void) { CHECK(flags & 0x200); return &device; }
bool fbdev_console_begin(void) { CHECK(!claim_held); claims++; if (deny_claim) return false; claim_held=true; return true; }
void fbdev_console_end(void) { CHECK((flags & 0x200) && claim_held); claim_held=false; }
void fbdev_console_region(uint32_t bytes) { (void)bytes; }
void *vmm_map_framebuffer(uint32_t p,uint32_t n) { (void)p;CHECK(n<=sizeof(pixels));return pixels; }
void vmm_revoke_mmio(void *p,uint32_t n) { (void)p;(void)n; }
uint64_t ktime_cycles(void) { return g_ticks*1000; }
void kpreempt_point(void) { CHECK(!claim_held); points++; if (take_display) { take_display=false;console_display_owned(true); } }
void serial_write(const char *s,size_t n) { (void)s;(void)n; }
struct task *task_create_kernel(const char *n,void (*fn)(void *),void *arg,enum task_prio p)
{ (void)n;(void)fn;(void)arg;(void)p;return 0; }
void task_start(struct task *t) { (void)t; }
void task_sleep_ms(uint32_t n) { (void)n; }
__attribute__((noreturn)) void panic(const char *s,...) { fprintf(stderr,"%s\n",s);exit(1); }
static void *move_pixels(void *d,const void *s,size_t n) { moves++;return memmove(d,s,n); }
#define memmove move_pixels
#include "../../src/kernel/core/console.c"
#undef memmove
static unsigned finish_frame(void)
{
    unsigned calls=0;
    do {
        unsigned before=claims;
        console_service();
        CHECK(claims-before<=1 && !claim_held && flags==0x200);
        CHECK(++calls<2000);
    } while (paint.active);
    return calls;
}
int main(void)
{
    g_boot=(struct ciuki_boot_info){.fb_phys=1,.fb_pitch=640*4,.fb_width=640,.fb_height=480,.fb_bpp=32,
        .fb_red_pos=16,.fb_green_pos=8,.fb_blue_pos=0,.fb_red_size=8,.fb_green_size=8,.fb_blue_size=8};
    CHECK(console_init_lfb());claims=points=0;
    char cr[257];memset(cr,'\r',sizeof(cr));
    unsigned before_cr=locks;console_write(cr,sizeof(cr));
    CHECK(locks-before_cr>=5 && !linelen && flags==0x200);
    for(unsigned i=0;i<500;i++) console_write("one line\n",9);
    CHECK(!claims);
    CHECK(finish_frame()==rows*((cols+PAINT_CELLS-1)/PAINT_CELLS));
    CHECK(points==claims && paint_valid);
    unsigned old=claims;
    for(unsigned i=0;i<500;i++) console_write("more lines\n",11);
    g_ticks=99;console_service();CHECK(claims==old);
    g_ticks=100;finish_frame();CHECK(claims>old && !repaint_pending);
    old=claims;moves=0;console_write("new row\n",8);g_ticks=200;finish_frame();
    CHECK(moves && claims-old-moves==2*((cols+PAINT_CELLS-1)/PAINT_CELLS));
    console_display_owned(true);old=claims;
    console_write("owned\n",6);g_ticks=300;console_service();CHECK(claims==old);
    console_display_owned(false);take_display=true;console_service();CHECK(claims==old+1 && display_owned);
    take_display=false;console_display_owned(false);g_ticks=400;
    console_pin_page("Hardware check complete\nPASS timer\n");
    finish_frame();CHECK(pinned_page && !repaint_pending);
    old=claims;console_write("background log\n",15);g_ticks=500;console_service();
    CHECK(pinned_page && !repaint_pending && claims==old);
    /* A busy presenter retries the same chunk; no pixels are skipped. */
    console_pin_page("new page\n");deny_claim=true;console_service();
    CHECK(paint.active && !paint.col && !paint.row);
    deny_claim=false;finish_frame();
    /* Geometry/grant changes abort partial frames before the next store. */
    console_pin_page("partial\n");g_ticks=600;console_service();CHECK(paint.active);
    console_display_owned(true);CHECK(!paint.active);console_display_owned(false);
    g_ticks=700;finish_frame();
    old=claims;console_reclaim();console_write("panic\n",6);
    CHECK(claims==old+rows*((cols+PAINT_CELLS-1)/PAINT_CELLS) && !pinned_page);
    CHECK(flags==0x200 && !claim_held);
    puts("console latency: <=8 cells/one scanline per call, IF enabled, claim released before yield, scroll, busy/grant retry, pinned-page silence and panic PASS");
}
