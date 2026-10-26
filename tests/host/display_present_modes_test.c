/* Actual Win32 grant/revoke + display service + fbdev transactions, with
 * firmware/driver/hardware boundaries supplied by the existing host fixture. */
#define DISPLAY_PRESENT_MODE_TEST
#define main display_existing_main
#include "display_test.c"
#undef main
#include <ciuki/win32.h>
#include <ciuki/present_abi.h>

bool w32_service_desktop_client(struct process *p,uint32_t pid)
{ return p==&desktop && pid==other.pid; }
gen_t gen_alloc(void) { return ++generation; }
uint32_t pmm_alloc(void) { return 0; }
void pmm_free(uint32_t page) { assert(!page); }
void kpreempt_point(void) {}
int ua_pin(struct uaddr *u,struct ua_pin *pin,uint32_t base,uint32_t n,bool write)
{ (void)u;(void)pin;(void)base;(void)n;(void)write;return -EFAULT; }
void ua_unpin(struct uaddr *u,struct ua_pin *pin) { (void)u;(void)pin; }
int ua_read(const struct uaddr *u,void *out,uint32_t base,uint32_t n)
{ (void)u;(void)out;(void)base;(void)n;return -EFAULT; }

static void lease(uint16_t mode)
{
    desktop.state=other.state=PROC_LIVE; other.personality=PROC_WIN32;
    struct proc_thread thread={.process=&desktop};
    struct ciuki_w32_display_args a={.action=W32_DISPLAY_GRANT,.pid=other.pid,
        .mode=mode,.scale=1,.surface={640,480,2560,640*480*4,CIUKI_PRESENT_XRGB8888}};
    uint32_t before=fbdev_get()->generation;
    assert(!w32_display_call(&thread,&a) && !a.error && a.result && a.token && a.generation);
    assert(display_present_active() && suspended && fbdev_get()->present);
    if (mode==0x112) {
        assert(!switches && !native_begins && a.generation==before && fbdev_get()->mode_id==0x118);
    } else {
        assert(!switches && native_begins==1 && native_publishes==1 && a.generation!=before);
        assert(fbdev_get()->mode_id==0x111 && fbdev_get()->pitch==1280);
    }
    a.action=W32_DISPLAY_REVOKE;
    assert(!w32_display_call(&thread,&a) && !a.error && a.result);
    assert(!display_present_active() && !suspended && !switches && fbdev_get()->present);
    assert(fbdev_get()->mode_id==0x118 && fbdev_get()->pitch==4112);
    if (mode==0x111) assert(native_ends==1 && native_completes==1 && native_recovered);
    g_ticks+=10000;display_watchdog_poll();
    assert(!switches && fbdev_get()->mode_id==0x118 && !awaiting_frame);
}
int main(void)
{
    fixture(); prepare_failure=true;
    g_boot.display.mode_count=3;
    g_boot.display.modes[2]=g_boot.display.modes[0];g_boot.display.modes[2].id=0x112;
    lease(0x112);
    fixture(); is_supersavage=native_supported=prepare_failure=true;
    g_boot.display.modes[1].pitch=1280;
    acceleration=&test_acceleration;accel_ports_owned=true;
    lease(0x111);
    free(aperture);free(window);
    puts("PASS fx-biosvm actual Win32 leases: grant/revoke of boot-layout alias makes zero mode sets; native packed lease restores desktop without BIOS");
    return 0;
}
