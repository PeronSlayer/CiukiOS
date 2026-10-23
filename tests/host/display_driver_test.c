/* Integrated production registry/fbdev/swdib with a recording adapter.
 * SPDX-License-Identifier: GPL-2.0-only */
#define DISPLAY_DRIVER_FIXTURE_REAL
#define main fbdev_regression_main
#include "fbdev_test.c"
#undef main
const struct display_driver mach64_driver = {0};
const struct display_driver rage128_driver = {0};
const struct display_driver radeon2d_driver = {0};
const struct display_driver sis300_driver = {0};
const struct display_driver sis315_driver = {0};
const struct display_driver cirrus_driver = {0};
const struct display_driver trident_blade_driver = {0};
const struct display_driver chipshiqv_driver = {0};
const struct display_driver matrox_driver = {0};
const struct display_driver nv2d_driver = {0};
const struct display_driver riva128_driver = {0};
const struct display_driver neomagic_driver = {0};
const struct display_driver unichrome_driver = {0};
const struct display_driver tdfx_driver = {0};
const struct display_driver vmwaresvga_driver = {0};
const struct display_driver s3trio64_driver = {0};
const struct display_driver s3virge_driver = {0};
const struct display_driver i740_driver = {0};
const struct display_driver i810_driver = {0};
const struct display_driver intel2d_gen2_driver = {0};
const struct display_driver intel2d_gen3_driver = {0};
#include "../../src/kernel/drivers/video/registry.c"

static uint8_t pixels[32*32*4], before[sizeof(pixels)];
static struct pci_func gpu;
static unsigned gpu_count, probes, eligible_calls, starts, begins, clears, records;
static bool refuse_probe, unsafe, quarantined, start_fail, marker_fail, clear_fail, stop_fail, sliced, corrupt;
static int refuse;
static unsigned syncs, submissions, stops, hides, yields, cursor_checks;
static bool native_cursor;
static int (*pause_fn)(void *);
static void *pause_ctx;
unsigned pci_count(void) { return gpu_count; }
const struct pci_func *pci_get(unsigned i) { return i<gpu_count ? &gpu : NULL; }
static bool fake_probe(const struct pci_func *p) { CHECK(!locks && p==&gpu); probes++; return !refuse_probe; }
static bool fake_eligible(const struct fb_device *d) { CHECK(!locks && d==&device); eligible_calls++; return true; }
static unsigned fake_caps(void *ctx) { (void)ctx; return quarantined ? 0 : native_cursor ? 3 : 1; }
static bool fake_ready(void *ctx) { (void)ctx; return !unsafe; }
static int fake_sync(void *ctx) { (void)ctx; CHECK(locks==1); syncs++; return unsafe ? -EIO : 0; }
static int fake_fill(void *ctx,const struct fb_rect *r,uint32_t c)
{
    (void)ctx; CHECK(locks==1); submissions++;
    if (unsafe) return -EIO;
    if (refuse) return refuse;
    if (sliced && pause_fn) CHECK(!pause_fn(pause_ctx));
    if (!corrupt) swdib_fill(&device,r,c);
    return 0;
}
static int fake_copy(void *ctx,const struct fb_rect *r,int32_t x,int32_t y)
{
    (void)ctx; CHECK(locks==1); submissions++;
    if (refuse==-EIO) { pixels[0]^=1; return refuse; }
    if (refuse) return refuse;
    if (!corrupt) swdib_copy(&device,r,x,y);
    return 0;
}
static int fake_pattern(void *ctx,const struct fb_rect *r,const uint8_t p[8],uint32_t fg,uint32_t bg)
{
    (void)ctx; CHECK(locks==1); submissions++;
    if (refuse) return refuse;
    swdib_pattern(&device,r,p,fg,bg); return 0;
}
static int fake_write(void *ctx,const struct fb_rect *r,const struct fb_rect *c,const void *rows,uint32_t stride)
{
    (void)ctx; CHECK(locks==1); submissions++;
    if (refuse) return refuse;
    swdib_write(&device,r,c,rows,stride); return 0;
}
static int fake_stop(void *ctx) { (void)ctx; CHECK(locks==1); stops++; hides++; return stop_fail ? -EIO : 0; }
static uint32_t fake_rows(void *ctx,uint32_t *first) { (void)ctx; *first=32; return quarantined ? 0 : 48; }
static void fake_pause(void *ctx,int (*pause)(void *),void *arg) { (void)ctx; pause_fn=pause; pause_ctx=arg; }
static const struct accel_ops fake_ops={
    .sync=fake_sync,.copy_rect=fake_copy,.fill_rect=fake_fill,.pattern_rect=fake_pattern,
    .stop=fake_stop,.capabilities=fake_caps,.cpu_ready=fake_ready,.write_rect=fake_write,
    .offscreen=fake_rows,.set_pause=fake_pause
};
static const struct accel_ops *fake_guard(void) { return unsafe ? &fake_ops : NULL; }
static int pixel_probe(void *ctx)
{
    (void)ctx; CHECK(locks==1 && drawing==PRESENT_BUSY);
    CHECK(!fbdev_console_begin()); CHECK(fbdev_change_begin()==-EBUSY);
    return start_fail ? -EIO : 0;
}
static const struct accel_ops *fake_start(const struct fb_device *d,struct vfs *vfs)
{
    CHECK(!locks && d==&device && !vfs); starts++;
    CHECK(fbdev_accel_probe(d,pixel_probe,NULL)==(start_fail ? -EIO : 0));
    return start_fail ? NULL : &fake_ops;
}
static int fake_begin(struct vfs *vfs) { (void)vfs; CHECK(!locks); begins++; return marker_fail ? -EIO : 0; }
static int fake_clear(struct vfs *vfs) { (void)vfs; CHECK(!locks); clears++; return clear_fail ? -EIO : 0; }
static int fake_failed(struct vfs *vfs) { (void)vfs; CHECK(!locks); return 0; }
static void fake_quarantine(const char *why) { CHECK(locks==1 && why); quarantined=true; hides++; }
static void fake_record(void) { CHECK(!locks); records++; }
static bool fake_summary(bool *ready,bool *used,const char **reason)
{ *ready=starts && !quarantined; *used=false; *reason=quarantined ? "fault" : starts ? "none" : "pending"; return true; }
static void fake_snapshot(struct ciuki_accel_info *out) { CHECK(locks==1); out->driver=1; out->quarantined=quarantined; }
static int fake_cursor_test(const struct accel_ops *ops,uint64_t deadline,bool (*cancel)(void *),void *ctx)
{
    CHECK(ops==&fake_ops && locks==1 && drawing==PRESENT_BUSY && accel_inflight && deadline>g_ticks);
    CHECK(!cancel || !cancel(ctx)); cursor_checks++; return 0;
}
static bool fake_mode_adapter(const struct pci_func *p) { return p==&gpu; }
static const struct display_mode_provider fake_modes={0};
static const struct display_match fake_matches[]={{0x1234,0x5678,3,255}};
const struct display_driver s3savage_driver={
    .id="fake",.name="Recording adapter",.matches=fake_matches,.match_count=1,.framebuffer_bar=1,
    .probe=fake_probe,.eligible=fake_eligible,.start=fake_start,.fallback=fake_guard,
    .attempt_begin=fake_begin,.attempt_end=fake_clear,.attempt_failed=fake_failed,
    .quarantine=fake_quarantine,.record=fake_record,.summary=fake_summary,.snapshot=fake_snapshot,
    .modes=&fake_modes,.mode_adapter=fake_mode_adapter,.cursor_test=fake_cursor_test
};
void grants_display_yield(void)
{
    CHECK(!locks && drawing==PRESENT_BUSY && accel_inflight); yields++;
    CHECK(!fbdev_console_begin() && fbdev_change_begin()==-EBUSY);
    struct ciuki_accel_request q={.size=sizeof(q),.generation=42,.operation=CIUKI_ACCEL_CURSOR_SHOW};
    CHECK(fbdev_accel_request(&q)==-EBUSY);
    fbdev_cursor_hide();
}
void console_framebuffer_revoke(void) { }
void console_framebuffer_changed(const struct fb_device *fb) { (void)fb; }
static void setup(void)
{
    memset(&g_boot,0,sizeof(g_boot)); g_boot.display.vram_bytes=8*1024*1024;
    device=(struct fb_device){.width=32,.height=32,.pitch=128,.size=sizeof(pixels),.phys=0xe0000000,
        .mapped=pixels,.bpp=32,.red_size=8,.red_pos=16,.green_size=8,.green_pos=8,.blue_size=8,
        .rsvd_size=8,.rsvd_pos=24,.present=true,.handle=0,.generation=42};
    reservation=(struct resource){.type=RES_MMIO,.start=device.phys,.end=device.phys+device.size,
        .owner="fbdev",.state=RS_ACTIVE,.generation=42};
    gpu=(struct pci_func){.vendor=0x1234,.device=0x5678,.class_code=3,.command=3,.bar={0,device.phys}};
    gpu_count=1; probes=eligible_calls=starts=begins=clears=records=0;
    unsafe=quarantined=start_fail=marker_fail=clear_fail=stop_fail=sliced=corrupt=refuse_probe=false;
    syncs=submissions=stops=hides=yields=cursor_checks=0; native_cursor=false; refuse=0; pause_fn=NULL; pause_ctx=NULL;
    acceleration=NULL; adapter=NULL; bound=NULL; owner=NULL;
    initialized=true; changing=accel_queued=accel_initializing=accel_inflight=accel_blocked=false;
    drawing=console_rows=0; locks=0; accel_marker_task=NULL; accel_marker_request=ACCEL_MARKER_IDLE;
    memset(native_count,0,sizeof(native_count)); memset(software_count,0,sizeof(software_count));
    for (unsigned i=0;i<sizeof(pixels);i++) pixels[i]=(uint8_t)(i*71+13);
    memcpy(before,pixels,sizeof(pixels));
}
static void activate(void)
{
    fbdev_accel_schedule(NULL);
    CHECK(accel_marker_request==ACCEL_MARKER_BEGIN);
    accel_marker_run();
    if (accel_marker_request==ACCEL_MARKER_INIT) accel_marker_run();
    if (accel_marker_request==ACCEL_MARKER_CLEAR) {
        CHECK(accel_initializing && fbdev_change_begin()==-EBUSY);
        struct ciuki_accel_request q={.size=sizeof(q),.operation=CIUKI_ACCEL_FILL,.generation=42,
            .rect={.width=4,.height=4}};
        CHECK(fbdev_accel_request(&q)==-EBUSY);
        q.operation=CIUKI_ACCEL_QUERY; CHECK(!fbdev_accel_request(&q));
    }
    if (accel_marker_request!=ACCEL_MARKER_IDLE) accel_marker_run();
    CHECK(!locks && !accel_queued);
}
static void selection(void)
{
    setup(); const struct pci_func *found=NULL;
    CHECK(display_driver_bind(&device)==&s3savage_driver && owner==&gpu && probes==1);
    struct display_driver first=s3savage_driver, second=s3savage_driver;
    first.probe=NULL; second.id="second";
    const struct display_driver *table[]={&first,&second};
    CHECK(display_driver_select(&device,table,2,&found)==&first && found==&gpu);
    first.probe=fake_probe; refuse_probe=true;
    CHECK(display_driver_select(&device,table,2,&found)==NULL && !found);
    second.probe=NULL;
    CHECK(display_driver_select(&device,table,2,&found)==&second && found==&gpu);
    refuse_probe=false;
    reservation.end--; CHECK(!display_driver_bind(&device)); reservation.end++;
    reservation.generation++; CHECK(!display_driver_bind(&device)); reservation.generation--;
    reservation.state=RS_QUARANTINED; CHECK(!display_driver_bind(&device)); reservation.state=RS_ACTIVE;
    gpu_count=2; CHECK(!display_driver_bind(&device));
    const struct display_mode_provider *provider=NULL;
    CHECK(display_driver_mode_bind(&device,&provider)==-EIO && !provider);
    gpu_count=1; gpu.bar[1]|=4; CHECK(!display_driver_bind(&device));
    gpu.bar[1]=device.phys; g_boot.display.vram_bytes=device.size-1; CHECK(!display_driver_bind(&device));
    g_boot.display.vram_bytes=8*1024*1024;
    g_boot.e820_count=1; g_boot.e820[0]=(struct ciuki_e820){.base=device.phys,.length=device.size,.type=CBI_E820_RAM};
    CHECK(!display_driver_bind(&device));
    setup(); strcpy(g_boot.options,"fooaccel=0 accel=00"); CHECK(!display_accel_disabled(NULL));
    strcpy(g_boot.options,"quiet accel=0\r\n");
    fbdev_accel_schedule(NULL); CHECK(!eligible_calls && !begins && !starts);
    CHECK(display_driver_modes(&device)==&fake_modes); /* independent of accel=0 */
    g_boot.flags=CBI_F_SAFE_MODE; fbdev_accel_schedule(NULL); CHECK(!begins && !starts);
    (void)found;
}
static void lifecycle(void)
{
    setup(); marker_fail=true; activate(); CHECK(!starts && begins==1 && accel_blocked);
    fbdev_accel_schedule(NULL); CHECK(begins==1);
    setup(); start_fail=unsafe=true; activate();
    CHECK(acceleration==&fake_ops && !fbdev_console_begin() && accel_blocked);
    struct fb_rect r={0,0,4,4}; CHECK(fbdev_fill(&r,0x123456)==-EIO && !memcmp(before,pixels,sizeof(pixels)));
    setup(); clear_fail=stop_fail=unsafe=true; activate();
    CHECK(stops==1 && quarantined && acceleration==&fake_ops && !fbdev_console_begin());
    CHECK(fbdev_change_begin()==-EIO && !changing);
    setup(); activate(); CHECK(starts==1 && begins==1 && clears==1 && acceleration==&fake_ops);
    sliced=true;
    struct ciuki_accel_request q={.size=sizeof(q),.generation=42,.operation=CIUKI_ACCEL_FILL,.colour=0x123456,.rect={.dst_x=0,.dst_y=0,.width=8,.height=8}};
    CHECK(!fbdev_accel_request(&q) && yields==1 && !accel_inflight && !drawing);
    q.operation=CIUKI_ACCEL_OFFSCREEN; CHECK(fbdev_accel_request(&q)==(48<<16 | 32));
    quarantined=true; CHECK(!fbdev_accel_request(&q) && accel_blocked);
    q.operation=CIUKI_ACCEL_COPY; q.rect=(struct ciuki_rect){0,32,0,0,4,4}; CHECK(fbdev_accel_request(&q)<0);
}
static void punting(void)
{
    setup(); activate(); refuse=-EOPNOTSUPP;
    struct ciuki_accel_request q={.size=sizeof(q),.generation=42,.operation=CIUKI_ACCEL_FILL,.colour=0x123456,.rect={.dst_x=0,.dst_y=0,.width=8,.height=8}};
    CHECK(!fbdev_accel_request(&q) && native_count[0]==0 && software_count[0]==1 && syncs);
    q.operation=CIUKI_ACCEL_COPY; q.rect=(struct ciuki_rect){0,0,2,2,8,8};
    CHECK(!fbdev_accel_request(&q) && software_count[1]==1);
    refuse=-EIO; memcpy(before,pixels,sizeof(pixels));
    CHECK(fbdev_accel_request(&q)==-EAGAIN && software_count[1]==1 && pixels[0]!=(uint8_t)before[0]);
    refuse=-EOPNOTSUPP; q.operation=CIUKI_ACCEL_PATTERN;
    memset(q.pattern,0xaa,8); CHECK(!fbdev_accel_request(&q) && software_count[2]==1);
    uint32_t rows[4*4]={0}; struct fb_rect r={0,0,4,4};
    CHECK(fbdev_accel_write(42,&r,&r,rows,16)==1 && software_count[3]==1);
    q.operation=CIUKI_ACCEL_CURSOR_SET; CHECK(fbdev_accel_request(&q)==-EOPNOTSUPP);
    struct ciuki_display_engine_info info;
    CHECK(!fbdev_engine_info(42,&info) && !strcmp(info.driver_id,"fake") && info.capabilities==1 && info.software_operations==15);
    CHECK(!info.native_used && info.software[0]==1 && info.cpu_ready);
    CHECK(fbdev_engine_info(41,&info)==-EAGAIN);
    struct ciuki_accel_info legacy; CHECK(!fbdev_accel_info(42,&legacy) && sizeof(legacy)==40 && legacy.driver==1);
}
static bool cancel_check(void *ctx) { (void)ctx; return true; }
static void native_comparison(void)
{
    setup(); activate(); uint32_t tested=0;
    CHECK(fbdev_accel_validate(g_ticks,NULL,NULL,&tested)==-ETIMEDOUT && !submissions && !syncs);
    CHECK(fbdev_accel_validate(g_ticks+100,cancel_check,NULL,&tested)==-EINTR && !submissions && !syncs);
    CHECK(!fbdev_accel_validate(g_ticks+100,NULL,NULL,&tested) && tested==15 && !memcmp(before,pixels,sizeof(pixels)));
    CHECK(!native_count[0] && !native_count[1] && submissions>=6);
    setup(); g_boot.flags=CBI_F_HW_CHECK; native_cursor=true;
    CHECK(!fbdev_accel_diagnostic(NULL,g_ticks+100,NULL,NULL,&tested) && tested==31 && cursor_checks==1 && stops==1 && !acceleration);
    CHECK(!native_count[0] && !native_count[1]);
    setup(); activate();
    corrupt=true; CHECK(fbdev_accel_validate(g_ticks+100,NULL,NULL,&tested)==-EIO && quarantined);
    setup(); activate(); refuse=-EOPNOTSUPP;
    CHECK(fbdev_accel_validate(g_ticks+100,NULL,NULL,&tested)==-EOPNOTSUPP && !tested && !software_count[0]);
}
int main(void)
{
    selection(); lifecycle(); punting(); native_comparison();
    printf("display driver integration: %u failures (registry, policy, marker, unsafe guard, sliced waits, mode race, offscreen revocation, punts, native-only comparison)\n",failures);
    return failures ? 1 : 0;
}
