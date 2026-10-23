/* Production H-page dispatch, with mode-changing hooks that must stay unused.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/hardware_check.h>
#include <ciuki/display.h>
#include <ciuki/display_driver.h>
#include <ciuki/storage.h>
struct ciuki_boot_info g_boot;
static struct fb_device fb={.present=true,.mode_id=0x118,.bpp=32};
static struct storage disk;
static struct display_driver adapter={.id="fixture",.name="Native fixture"};
static unsigned diagnostics,lines;
static bool disabled,absent;
static bool quarantined,unsafe;
static int result;
static uint32_t evidence;
const struct fb_device *fbdev_get(void) { return &fb; }
const struct display_driver *display_driver_bind(const struct fb_device *d)
{ assert(d->mode_id==0x118 && d->bpp==32); return absent ? NULL : &adapter; }
bool display_accel_disabled(const char **why) { *why="boot_disabled"; return disabled; }
struct storage *storage_get(void) { return &disk; }
void display_driver_info(struct ciuki_display_engine_info *out)
{ memset(out,0,sizeof(*out)); out->quarantined=quarantined; out->cpu_ready=!unsafe; }
int fbdev_accel_diagnostic(struct vfs *v,uint64_t deadline,bool (*cancel)(void *),void *ctx,uint32_t *tested)
{
    assert(v==&disk.vfs && deadline==15000 && !cancel && !ctx);
    diagnostics++; *tested=evidence; return result;
}
static void page_line(void *ctx,const char *line) { assert(!ctx && *line); lines++; }
static uint64_t check_now(void *ctx) { (void)ctx; assert(0 && "configuration sweep forbidden"); return 0; }
uint64_t deadline_after_ms(uint32_t ms) { (void)ms; assert(0); return 0; }
int display_check_begin(uint16_t mode) { (void)mode; assert(0 && "firmware mode must remain untouched"); return -EIO; }
int display_check_end(void) { assert(0); return -EIO; }
int display_check_sweep_begin(void) { assert(0); return -EIO; }
void display_check_sweep_end(bool restored) { (void)restored; assert(0); }
#include "display_check_page.inc"
int main(void)
{
    struct hw_check_result out;
    evidence=3u|DISPLAY_CHECK_CURSOR;
    accelerator_check(15000,&out); assert(out.status==HW_PASS && diagnostics==1 && lines==2);
    evidence=1; accelerator_check(15000,&out); assert(out.status==HW_FAIL && diagnostics==2);
    evidence=3; result=-EIO; accelerator_check(15000,&out); assert(out.status==HW_FAIL);
    result=-EOPNOTSUPP; accelerator_check(15000,&out); assert(out.status==HW_SKIP);
    quarantined=true; accelerator_check(15000,&out); assert(out.status==HW_FAIL);
    quarantined=false; unsafe=true; accelerator_check(15000,&out); assert(out.status==HW_FAIL);
    unsafe=false;
    disabled=true; unsigned before=diagnostics;
    accelerator_check(15000,&out); assert(out.status==HW_SKIP && diagnostics==before);
    disabled=false; absent=true;
    accelerator_check(15000,&out); assert(out.status==HW_SKIP && diagnostics==before);
    puts("native H-page: DM9 fill/copy evidence required, failures/skips, safe-mode refusal, no configuration sweep or mode transition PASS");
    return 0;
}
