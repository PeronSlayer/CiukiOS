/* Real H-page item orchestration, delayed ordinary enumeration and display
 * service boundaries. No USB reset, firmware or disk I/O on the host. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#undef CLOCKS_PER_SEC
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#include <ciuki/cpu.h>
#define irq_save() 0u
#define irq_restore(flags) ((void)(flags))
#include "../../src/kernel/core/hardware_check.c"

struct ciuki_boot_info g_boot;
struct task *g_current;
volatile uint64_t g_ticks;
bool g_cpu_tsc;
uint64_t g_tsc_per_ms;
struct audio_state audio;
unsigned fs_host_contig_fail,fs_host_contig_live;
static struct usb_host host;
static bool has_host, live_prompt, event_sent, retained, swept, winner;
static unsigned attach_ms, prompt_ms, sweep_calls, mode_calls, restores, clears;
static int scenario;
static struct fb_device fb;
static struct storage disk;
static const struct pci_func gpu={.vendor=0x5333,.device=0x8c2e,.class_code=3};
static char lines[32][256];
static unsigned line_count;
static bool complete, tone_checked;

/* Unchanged page items use ready/absent boundaries; their device protocols
 * have separate tests. The page must still reach audio and its final list. */
int hardware_check_ata(struct storage *s,struct hw_check_result *r) { assert(s==&disk); r->status=HW_PASS; return 0; }
int hardware_check_repair(struct storage *s,struct hw_check_result *r) { return hardware_check_ata(s,r); }
void timing_reset(void) {}
void timing_snapshot(struct timing_stats *s) { memset(s,0,sizeof(*s)); }
void serial_snapshot(struct serial_stats *s) { memset(s,0,sizeof(*s)); }
uint64_t serial_write_ticket(const char *s,size_t n) { (void)s; (void)n; assert(0); return 0; }
void bootlog_capture(const char *s,size_t n) { (void)s; (void)n; }
void console_write(const char *s,size_t n) { (void)s; (void)n; }
void console_wake(void) {}
void console_pin_page(const char *s) { assert(strstr(s,"Hardware check complete")); complete=true; }
void bootlog_snapshot(struct bootlog_stats *s) { *s=(struct bootlog_stats){.active=true}; }
int bootlog_try_sync(void) { return 0; }
void audio_diagnostics(struct ciuki_audio_diagnostics *d) { (void)d; tone_checked=true; }
int audio_check_tone(uint32_t *frames) { *frames=AUDIO_RATE; return 0; }
uint32_t fnv1a32(const void *s,size_t n,uint32_t seed) { (void)s; (void)n; assert(0); return seed; }

uint64_t deadline_after_ms(uint32_t ms) { return g_ticks+ms; }
void task_sleep_ms(uint32_t ms)
{
    g_ticks+=ms;
    if (has_host && attach_ms && g_ticks>=attach_ms) {
        host.device_count=1;
        struct usb_device *d=&host.devices[0];
        d->present=true; d->vendor=0x046d; d->product=0xc52b;
        d->hid[0].protocol=2; d->hid[0].endpoint=1;
        d->hid[0].active=scenario!=1;
    }
}
unsigned usb_count(void) { return has_host; }
const struct usb_host *usb_get(unsigned i) { assert(has_host && !i); return &host; }
void klog(const char *fmt,...)
{
    char text[256]; va_list ap; va_start(ap,fmt); vsnprintf(text,sizeof(text),fmt,ap); va_end(ap);
    if (line_count<ARRAY_SIZE(lines)) strcpy(lines[line_count++],text);
    if (strstr(text,"Move/click/scroll")) { live_prompt=true; prompt_ms=g_ticks; }
}
void i8042_snapshot(struct i8042_stats *out) { *out=(struct i8042_stats){.active=true}; }
void input_snapshot(struct input_stats *out) { memset(out,0,sizeof(*out)); }
bool input_read(struct input_event *out)
{
    if (!live_prompt || event_sent) return false;
    event_sent=true;
    *out=(struct input_event){.source=INPUT_USB,.type=INPUT_REL,.code=INPUT_X,.value=7,.tick=g_ticks};
    return true;
}
int audio_master_set(struct ciuki_mixer_master *m) { audio.master=*m; return 0; }
struct storage *storage_get(void) { return &disk; }
const struct fb_device *fbdev_get(void) { return &fb; }
unsigned pci_count(void) { return 1; }
const struct pci_func *pci_get(unsigned i) { return i ? 0 : &gpu; }
bool savage2d_supported(const struct pci_func *p) { return p==&gpu; }
int savage2d_check_begin(struct vfs *v,bool *prior) { assert(v==&disk.vfs); *prior=retained; return 0; }
int savage2d_attempt_end(struct vfs *v) { assert(v==&disk.vfs); clears++; return 0; }
int savage2d_hardware_sweep(uint64_t end,savage_sweep_line line,void *ctx,struct savage_sweep_result *out)
{
    assert(end==g_ticks+3000);
    sweep_calls++;
    assert(sweep_calls<3 && (sweep_calls==1 || winner && swept && fb.bpp==16));
    *out=(struct savage_sweep_result){.tried=96,.passed=23,.first=1,.restored=true};
    if (scenario==2 && sweep_calls==1) out->passed=0,out->first=-1;
    if (scenario==3 && sweep_calls==1) out->error=-ETIMEDOUT;
    if (scenario==4 && sweep_calls==2) out->restored=false,out->error=-EIO;
    line(ctx,"Savage first exact configuration: bpp=32 config=1");
    winner=out->first>=0;
    return out->error;
}
int display_check_begin(uint16_t id)
{
    assert(id==0x117 && winner && sweep_calls==1 && !swept);
    bool recorded=false;
    for (unsigned i=0;i<line_count;i++) if (strstr(lines[i],"original-depth result: bpp=32 first=1")) recorded=true;
    assert(recorded); mode_calls++;
    if (scenario==5) return -EIO;
    fb.bpp=16; fb.pitch=2048; fb.mode_id=id;
    return 0;
}
int display_check_sweep_begin(void) { swept=true; return scenario==6 ? -ETIMEDOUT : 0; }
void display_check_sweep_end(bool safe) { assert(swept && safe==(scenario!=4)); }
int display_check_end(void)
{
    assert(scenario!=4); restores++;
    fb.bpp=32; fb.pitch=4096; fb.mode_id=0x118;
    return scenario==6 ? -ETIMEDOUT : 0;
}
static void fixture(void)
{
    g_ticks=0; host=(struct usb_host){0}; audio=(struct audio_state){0};
    has_host=live_prompt=event_sent=retained=swept=winner=false;
    attach_ms=prompt_ms=sweep_calls=mode_calls=restores=clears=line_count=0; scenario=0;
    complete=tone_checked=false; summary_bytes=0; collecting_summary=false;
    fb=(struct fb_device){.present=true,.mode_id=0x118,.width=1024,.height=768,.bpp=32,.pitch=4096,.phys=0xe8000000};
    g_boot.display.mode_count=1;
    g_boot.display.modes[0]=(struct ciuki_boot_display_mode){.id=0x117,.width=1024,.height=768,.bpp=16,.pitch=2048,.linear=1};
}
int main(void)
{
    struct hw_check_result out;
    fixture(); has_host=true; attach_ms=3500;
    input_window(true,15000,&out);
    assert(out.status==HW_PASS && prompt_ms==3500 && g_ticks==8500);
    assert(strstr(out.detail,"events=1") && strstr(out.detail,"enumeration_ms=3500"));
    fixture(); has_host=true;
    input_window(true,15000,&out);
    assert(out.status==HW_SKIP && g_ticks==8000 && !live_prompt);
    fixture(); input_window(true,15000,&out);
    assert(out.status==HW_SKIP && !g_ticks);
    fixture(); has_host=true; attach_ms=10; scenario=1;
    input_window(true,15000,&out);
    assert(out.status==HW_FAIL && g_ticks==13000);
    fixture(); has_host=true; attach_ms=3500;
    input_window(true,9000,&out);
    assert(out.status==HW_SKIP && g_ticks==2000 && !live_prompt);
    puts("PASS f1-69 USB mouse: delayed attach then full 5 s events, absent waits 8 s, no host skips, inactive fails, short budget bounded");
    for (int s=0;s<=6;s++) {
        fixture(); scenario=s; retained=s==1;
        savage_check(15000,&out);
        if (s<=1) assert(out.status==HW_PASS && sweep_calls==2 && restores==1 && fb.bpp==32 && clears==!retained);
        else if (s<=3) assert(out.status==HW_FAIL && sweep_calls==1 && !mode_calls);
        else if (s==4) assert(out.status==HW_FAIL && sweep_calls==2 && !restores && !clears);
        else if (s==5) assert(out.status==HW_FAIL && sweep_calls==1 && mode_calls==1 && !restores);
        else assert(out.status==HW_FAIL && sweep_calls==1 && restores==1);
    }
    puts("PASS f1-69 H sweep: winner recorded before 16 bpp, no winner/timeout gate, restored desktop, retained marker, unsafe writer/late watchdog refusal");
    fixture(); hardware_check_run();
    assert(tone_checked && complete && sweep_calls==2 && restores==1 && fb.bpp==32);
    fixture(); scenario=4; hardware_check_run();
    assert(tone_checked && complete && sweep_calls==2 && !restores && !clears);
    puts("PASS f1-69 complete H page: audio and pinned final results after a passing sweep and an unsafe-writer failure");
    return 0;
}
