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
#undef irq_save
#undef irq_restore
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
static unsigned scan_ms, attach_ms, prompt_ms, sweep_calls, mode_calls, restores, clears;
static int scenario;
static unsigned diagnostic_calls;
static int diagnostic_error;
static uint32_t diagnostic_mask;
static bool diagnostic_safe,diagnostic_quarantined;
static struct fb_device fb;
static struct storage disk;
static const struct pci_func gpu={.vendor=0x5333,.device=0x8c2e,.class_code=3};
static char lines[32][256];
static unsigned line_count;
static bool complete, tone_checked, cache_over_cap;
static unsigned timer_calls;
void cache_stats(struct block_cache *cache,struct cache_stats *out) {
    assert(cache==&disk.cache);
    *out=(struct cache_stats){.pages=2048,.hits=3,.misses=1,.growths=2,.reclaims=3,
        .prefetch_useful=32768,.prefetch_wasted=1024,.speculative_bytes=cache_over_cap ? 1049088 : 65536};
}
bool ledger_reconcile(void) { return true; }
void ledger_snapshot(struct mm_ledger *out) { memset(out,0,sizeof(*out)); }
void ledger_record(const char *profile) { assert(!strcmp(profile,"hardware-check")); }

/* Unchanged page items use ready/absent boundaries; their device protocols
 * have separate tests. The page must still reach audio and its final list. */
int hardware_check_ata(struct storage *s,struct hw_check_result *r) { assert(s==&disk); r->status=HW_PASS; return 0; }
int hardware_check_repair(struct storage *s,struct hw_check_result *r) { return hardware_check_ata(s,r); }
void timing_reset(void) {}
void timing_snapshot(struct timing_stats *s) { memset(s,0,sizeof(*s)); }
void hardware_check_timer(uint64_t until,bool experiment,struct hw_check_result *s,const struct hw_check_ops *ops)
{ (void)until; (void)ops; assert(experiment==(timer_calls++!=0)); s->status=HW_SKIP; }
void hardware_check_smi_summary(const struct hw_check_ops *ops) { (void)ops; }
void serial_snapshot(struct serial_stats *s) { memset(s,0,sizeof(*s)); }
uint32_t serial_evidence_begin(size_t n) { (void)n; assert(0); return 0; }
uint64_t serial_write_ticket(const char *s,size_t n) { (void)s; (void)n; assert(0); return 0; }
void bootlog_capture(const char *s,size_t n) { (void)s; (void)n; }
void console_write(const char *s,size_t n) { (void)s; (void)n; }
void console_wake(void) {}
void console_pin_page(const char *s) { assert(strstr(s,"Hardware check complete")); complete=true; }
void bootlog_snapshot(struct bootlog_stats *s) { *s=(struct bootlog_stats){.active=true}; }
int bootlog_try_sync(void) { return 0; }
void audio_diagnostics(struct ciuki_audio_diagnostics *d) { memset(d,0,sizeof(*d)); tone_checked=true; }
int audio_check_duration(uint32_t seconds,uint32_t *frames) { assert(seconds==10); *frames=seconds*AUDIO_RATE; return 0; }
uint32_t fnv1a32(const void *s,size_t n,uint32_t seed) { (void)s; (void)n; assert(0); return seed; }

uint64_t deadline_after_ms(uint32_t ms) { return g_ticks+ms; }
void task_sleep_ms(uint32_t ms)
{
    g_ticks+=ms;
    if (has_host && scan_ms && g_ticks>=scan_ms) host.scanned=true;
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
static unsigned ec_refresh_requests;
void thinkpad_ec_refresh_request(void) { ec_refresh_requests++; }
struct storage *storage_get(void) { return &disk; }
const struct fb_device *fbdev_get(void) { return &fb; }
unsigned pci_count(void) { return 1; }
const struct pci_func *pci_get(unsigned i) { return i ? 0 : &gpu; }
bool savage2d_supported(const struct pci_func *p) { return p==&gpu; }
int savage2d_check_begin(struct vfs *v,bool *prior) { assert(v==&disk.vfs); *prior=retained; return 0; }
int savage2d_attempt_end(struct vfs *v) { assert(v==&disk.vfs); clears++; return scenario==12 ? -EIO : 0; }
int savage2d_hardware_sweep(uint64_t end,void (*line)(void *,const char *),void *ctx,struct display_selftest_result *out)
{
    assert(end==g_ticks+5000);
    sweep_calls++;
    if (scenario>=7) {
        *out=(struct display_selftest_result){.first=-1,.restored=scenario!=10,.tried=scenario==9};
        return -EOPNOTSUPP;
    }
    assert(sweep_calls<3 && (sweep_calls==1 || winner && swept && fb.bpp==16));
    *out=(struct display_selftest_result){.tried=12,.passed=2,.skipped=84,.completed=96,.first=1,.restored=true};
    if (scenario==2 && sweep_calls==1) out->passed=0,out->first=-1;
    if (scenario==3 && sweep_calls==1) out->error=-ETIMEDOUT;
    if (scenario==4 && sweep_calls==2) out->restored=false,out->error=-EIO;
    line(ctx,"Savage bpp=32 config=1 result=pass first_exact=1");
    winner=out->first>=0;
    return out->error;
}
static struct display_driver diagnostic_driver={.name="Recording adapter",.check_begin=savage2d_check_begin,
    .attempt_end=savage2d_attempt_end,.selftest=savage2d_hardware_sweep};
bool display_accel_disabled(const char **reason) { (void)reason; return false; }
const struct display_driver *display_driver_bind(const struct fb_device *fb) { (void)fb; return &diagnostic_driver; }
int fbdev_accel_diagnostic(struct vfs *v,uint64_t until,bool (*cancel)(void *),void *ctx,uint32_t *tested)
{ (void)v;(void)until;(void)cancel;(void)ctx;diagnostic_calls++;
  *tested=scenario==13 ? 1 : diagnostic_mask;return scenario==11 ? -EIO : diagnostic_error; }
void display_driver_info(struct ciuki_display_engine_info *out)
{ *out=(struct ciuki_display_engine_info){.cpu_ready=diagnostic_safe,.quarantined=diagnostic_quarantined}; }
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
    g_ticks=0; host=(struct usb_host){.ready=true,.scanned=true}; audio=(struct audio_state){.ready=true};
    has_host=live_prompt=event_sent=retained=swept=winner=false;
    scan_ms=attach_ms=prompt_ms=sweep_calls=mode_calls=restores=clears=line_count=0; scenario=0;
    complete=tone_checked=false; summary_bytes=0; collecting_summary=false;
    timer_calls=0; diagnostic_calls=0; diagnostic_error=0; diagnostic_mask=15;
    diagnostic_safe=true; diagnostic_quarantined=false;
    diagnostic_driver.selftest=savage2d_hardware_sweep;
    diagnostic_driver.check_begin=savage2d_check_begin;
    fb=(struct fb_device){.present=true,.mode_id=0x118,.width=1024,.height=768,.bpp=32,.pitch=4096,.phys=0xe8000000};
    g_boot.display.mode_count=1;
    g_boot.display.modes[0]=(struct ciuki_boot_display_mode){.id=0x117,.width=1024,.height=768,.bpp=16,.pitch=2048,.linear=1};
}
int main(void)
{
    struct hw_check_result out;
    fixture(); has_host=true; host.scanned=false; scan_ms=3000; attach_ms=3500;
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
    fixture(); has_host=true; host.scanned=false;
    input_window(true,15000,&out);
    assert(out.status==HW_FAIL && g_ticks==8000 && !live_prompt && strstr(out.detail,"reason=scan_pending"));
    fixture(); has_host=true; host.ready=false; host.reason="resource_conflict";
    input_window(true,15000,&out);
    assert(out.status==HW_FAIL && !g_ticks && !live_prompt && strstr(out.detail,"reason=resource_conflict"));
    fixture(); has_host=true; host.scanned=false; scan_ms=7000; attach_ms=7500;
    input_window(true,15000,&out);
    assert(out.status==HW_PASS && prompt_ms==7500 && g_ticks==12500);
    fixture(); has_host=true; host.scanned=false; g_ticks=100;
    input_window(true,7095,&out);
    assert(out.status==HW_FAIL && g_ticks==100 && !live_prompt && strstr(out.detail,"reason=scan_pending"));
    puts("PASS f1-69 USB mouse: delayed attach then full 5 s events, absent waits 8 s, no host skips, inactive fails, short budget bounded");
    puts("PASS merged USB mouse: shared first-scan wait, pending/conflicted hosts fail, late first scan preserves live window, expired reserve issues no sleep");
    /* f1-100: the EC poll is off by default; the keyboard/EC key window asks
     * the EC worker for one rate-bounded read per second, none without EC. */
    fixture(); audio.master_in_hardware=true; ec_refresh_requests=0;
    input_window(false,15000,&out);
    assert(out.status==HW_PASS && g_ticks==5000 && ec_refresh_requests==5);
    fixture(); ec_refresh_requests=0; input_window(false,15000,&out);
    assert(out.status==HW_PASS && g_ticks==5000 && !ec_refresh_requests);
    puts("PASS f1-100 H keyboard/EC keys: one EC refresh request per second during the 5 s window, none without the EC");
    for (int s=0;s<=6;s++) {
        fixture(); scenario=s; retained=s==1;
        accelerator_check(15000,&out);
        if (s<=1) assert(out.status==(retained ? HW_SKIP : HW_PASS) && sweep_calls==2 && restores==1 && fb.bpp==32 && clears==!retained);
        else if (s<=3) assert(out.status==HW_FAIL && sweep_calls==1 && !mode_calls);
        else if (s==4) assert(out.status==HW_FAIL && sweep_calls==2 && !restores && !clears);
        else if (s==5) assert(out.status==HW_FAIL && sweep_calls==1 && mode_calls==1 && !restores);
        else assert(out.status==HW_FAIL && sweep_calls==1 && restores==1);
    }
    puts("PASS f1-69 H sweep: winner recorded before 16 bpp, no winner/timeout gate, restored desktop, retained marker, unsafe writer/late watchdog refusal");
    for (int s=7;s<=13;s++) {
        fixture(); scenario=s; retained=s==8; diagnostic_calls=0;
        accelerator_check(15000,&out);
        assert(out.status==(s==7 ? HW_PASS : s==8 ? HW_SKIP : HW_FAIL));
        assert(diagnostic_calls==(s==7 || s==11 || s==13));
        assert(!mode_calls && clears==(s==7 || s==9 || s==11 || s==12 || s==13));
    }
    puts("PASS optional display sweep: clean refusal runs DM9; retained marker, touched/unsafe sweep, clear failure, native failure/missing COPY fail closed");
    for (unsigned c=0;c<8;c++) {
        fixture(); diagnostic_driver.selftest=0; diagnostic_driver.check_begin=0;
        diagnostic_error=c==1 || c==4 || c==5 ? -EOPNOTSUPP : c==2 ? -EIO : c==7 ? -ETIMEDOUT : 0;
        diagnostic_mask=c==3 ? 1 : 15;
        diagnostic_safe=c!=4 && c!=6; diagnostic_quarantined=c==5;
        accelerator_check(15000,&out);
        assert(out.status==(c==0 ? HW_PASS : c==1 ? HW_SKIP : HW_FAIL));
        assert(diagnostic_calls==1 && !sweep_calls && !clears && !mode_calls);
    }
    puts("PASS generic DM9 without sweep: native coverage, refusal, error, timeout, CPU safety and quarantine");
    fixture(); hardware_check_run();
    assert(tone_checked && complete && sweep_calls==2 && restores==1 && fb.bpp==32 && timer_calls==2);
    fixture(); scenario=4; hardware_check_run();
    assert(tone_checked && complete && sweep_calls==2 && !restores && !clears && timer_calls==2);
    puts("PASS f1-69 complete H page: audio and pinned final results after a passing sweep and an unsafe-writer failure");
    fixture(); disk.ready=true; cache_over_cap=false;
    check_step(0,11,15000,&out);
    assert(out.status==HW_PASS && strstr(out.detail,"KiB=8192 hit=75% grow=2 reclaim=3") && strstr(out.detail,"32/1 KiB"));
    cache_over_cap=true; check_step(0,11,15000,&out); assert(out.status==HW_FAIL);
    disk.ready=false; check_step(0,11,15000,&out); assert(out.status==HW_SKIP);
    puts("PASS C4 real H Cache item: size/hit/growth/reclaim/usefulness snapshot, cap failure, absent storage");
    return 0;
}
