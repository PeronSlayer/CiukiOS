/* Production presenter/registry plus real GD5446 engine with fake registers.
 * Adapter disk/port glue is covered independently by cirrus_driver_test.c.
 * SPDX-License-Identifier: GPL-2.0-only */
#define DISPLAY_DRIVER_FIXTURE_REAL
#define main framebuffer_regression_main
#include "fbdev_test.c"
#undef main
#include "cirrus_fake.h"
#include "../../src/kernel/drivers/video/registry.c"
static struct cirrus_fake registers;
static struct cirrus_engine native;
static struct pci_func adapter_pci;
const struct display_driver s3savage_driver={0};
unsigned pci_count(void) { return 1; }
const struct pci_func *pci_get(unsigned i) { return i ? NULL : &adapter_pci; }
static bool eligible_native(const struct fb_device *d) { return d==&device && !native.quarantined; }
static int initialize_native(void *ctx)
{
    CHECK(locks==1 && drawing==PRESENT_BUSY && !fbdev_console_begin());
    struct cirrus_io io=fake_io(&registers);
    return cirrus_engine_start(&native,ctx,&io,0xb8);
}
static const struct accel_ops *start_native(const struct fb_device *d, struct vfs *v)
{ (void)v; CHECK(!locks); return fbdev_accel_probe(d,initialize_native,(void *)d) ? NULL : &native.ops; }
static int marker_native(struct vfs *v) { (void)v; CHECK(!locks); return 0; }
static const struct accel_ops *guard_native(void)
{ (void)native.ops.sync(&native); return native.cpu_safe ? NULL : &native.ops; }
static void quarantine_native(const char *why) { cirrus_engine_quarantine(&native,why); }
static void snapshot_native(struct ciuki_accel_info *out) { out->quarantined=native.quarantined; }
static const struct display_match native_match[]={{0x1013,0xb8,3,255}};
const struct display_driver cirrus_driver={.id="cirrus",.name="Cirrus Logic GD54xx",
    .matches=native_match,.match_count=1,.framebuffer_bar=0,.eligible=eligible_native,
    .start=start_native,.fallback=guard_native,.attempt_begin=marker_native,.attempt_end=marker_native,
    .attempt_failed=marker_native,.quarantine=quarantine_native,.snapshot=snapshot_native};
static void prepare_presenter(void)
{
    memset(&native,0,sizeof(native)); memset(&registers,0,sizeof(registers));
    memset(&g_boot,0,sizeof(g_boot)); g_boot.display.vram_bytes=4*1024*1024;
    device=fake_fb(&registers,32);
    reservation=(struct resource){RES_MMIO,device.phys,device.phys+device.size,"fbdev",42,RS_ACTIVE,false};
    adapter_pci=(struct pci_func){.vendor=0x1013,.device=0xb8,.class_code=3,.command=3,.bar={device.phys}};
    acceleration=NULL; adapter=NULL; initialized=true; changing=false; drawing=console_rows=locks=0;
    accel_marker_task=NULL; accel_marker_request=ACCEL_MARKER_IDLE;
    accel_queued=accel_initializing=accel_inflight=accel_blocked=false;
    registers.step=100;
    fbdev_accel_schedule(NULL);
    for (unsigned i=0;i<3;i++) accel_marker_run();
    CHECK(acceleration==&native.ops && !accel_initializing && !locks);
}
static unsigned yielded;
void grants_display_yield(void)
{
    CHECK(!locks && drawing==PRESENT_BUSY && accel_inflight);
    CHECK(!fbdev_console_begin() && fbdev_change_begin()==-EBUSY);
    yielded++;
}
int main(void)
{
    prepare_presenter();
    struct ciuki_accel_request q={.size=sizeof(q),.generation=42,.operation=CIUKI_ACCEL_CURSOR_SET};
    unsigned commands=registers.starts;
    CHECK(fbdev_accel_request(&q)==-EOPNOTSUPP && registers.starts==commands);
    uint32_t tested=0;
    CHECK(!fbdev_accel_validate(10000,NULL,NULL,&tested) && (tested&3)==3 && !native.quarantined);
    uint8_t zero[sizeof(registers.pixels)]={0};
    CHECK(!memcmp(zero,registers.pixels,sizeof(zero))); /* DM9 saved/restored. */
    registers.delay_reads=30;
    q.operation=CIUKI_ACCEL_FILL; q.colour=0x123456; q.rect.width=q.rect.height=8;
    CHECK(!fbdev_accel_request(&q) && yielded && !locks);
    registers.stuck=registers.partial=registers.reset_fails=true;
    q.operation=CIUKI_ACCEL_COPY; q.rect.dst_x=q.rect.dst_y=1;
    CHECK(fbdev_accel_request(&q)==-EAGAIN && native.quarantined && !fbdev_cpu_ready());
    commands=registers.starts;
    CHECK(!fbdev_console_begin());
    uint8_t partial[sizeof(registers.pixels)]; memcpy(partial,registers.pixels,sizeof(partial));
    q.operation=CIUKI_ACCEL_FILL;
    CHECK(fbdev_accel_request(&q) && registers.starts==commands && !memcmp(partial,registers.pixels,sizeof(partial)));
    registers.reset_fails=false;
    CHECK(!fbdev_accel_request(&q) && fbdev_cpu_ready() && registers.starts==commands);
    CHECK(native.quarantined && fbdev_console_begin()); fbdev_console_end();
    for (unsigned y=1;y<9;y++) for (unsigned x=1;x<9;x++)
        CHECK(registers.pixels[y*device.pitch+x*4]==0x56);
    CHECK(!locks && !drawing && accel_blocked);
    printf("cirrus presenter: DM9, cursor refusal, sliced exclusion, unsafe guard and software recovery: %u failures\n",failures);
    return failures ? 1 : 0;
}
