/* SPDX-License-Identifier: MIT
 * F2-98a: production native views, clipped painting/presentation and T23
 * operation model. Rates inherit f2-95: RAM 1.67 ns/B, compare 3 ns/B,
 * paint 12 ns/px, kernel entry 3 us. Primitive dispatch is charged 0.2 us.
 * An empty 100 Hz native turn is charged 8 entries and 10 us user work.
 * Cold launch additionally reserves 100 ms for loader/fixed assets; this
 * allowance is an assumption, not physical disk or T23 timing evidence.
 * Host process CPU includes ASan/UBSan and is reported separately.
 */
#include "../../../apps/libdeskui/deskui.h"
#include "../../../apps/libdeskui/textarea.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static struct du_ui ui;
static struct du_window window;
static uint32_t back[760*520],front[760*520];
static uint64_t virtual_ns;
static struct { uint64_t pixels, compared, fills, frames; } cost;
int __real_memcmp(const void *,const void *,size_t);
int __wrap_memcmp(const void *a,const void *b,size_t n)
{ cost.compared+=n; return __real_memcmp(a,b,n); }
void __real_du_fill(struct du_canvas *,struct du_rect,uint32_t);
void __wrap_du_fill(struct du_canvas *c,struct du_rect r,uint32_t ink)
{ cost.fills++; __real_du_fill(c,r,ink); }
static uint64_t cpu_ns(void)
{ struct timespec t; assert(!clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t)); return (uint64_t)t.tv_sec*1000000000ull+(unsigned)t.tv_nsec; }
static void render(void)
{
    if (!ui.dirty) return;
    du_render(&ui); cost.frames++;
    for (unsigned i=0;i<ui.frame_count;i++) cost.pixels+=(uint64_t)ui.frames[i].w*ui.frames[i].h;
    du_window_present(&window); window.damaged=0; window.damage_count=0;
}
static double model_us(void)
{ return cost.pixels*12.0/1000.0+cost.compared*(3.0+1.67)/1000.0+cost.fills*0.2; }
static void init(void)
{
    memset(back,0,sizeof(back)); memset(front,0,sizeof(front)); memset(&window,0,sizeof(window));
#if defined(CASE_DEMO)
    const unsigned width=240,height=160;
#else
    const unsigned width=760,height=520;
#endif
    du_init(&ui,back,width,height); window.ui=&ui; window.back=back; window.pixels=front;
    window.info.width=width; window.info.height=height; window.present_rendered=1;
    virtual_ns=0; memset(&cost,0,sizeof(cost));
}
#if defined(CASE_CALC)
#include "../../../apps/accessories/calc.h"
static struct calc_app app;
static void setup(void) { calc_attach(&app,&ui); }
static void step(void) {}
static void cleanup(void) {}
#define APP_NAME "Calculator"
#elif defined(CASE_CLOCK)
#include "../../../apps/accessories/clock.h"
static struct clock_app app;
static int64_t epoch(void *p) { (void)p; return 1791711660+(int64_t)(virtual_ns/1000000000ull); }
static void setup(void) { clock_attach(&app,&ui,epoch,NULL); du_timer_start(&ui,1,250000000ull,1); }
static void step(void) {}
static void cleanup(void) {}
#define APP_NAME "Clock"
#elif defined(CASE_CHARMAP)
#include "../../../apps/accessories/charmap.h"
static struct charmap_app app;
static void setup(void) { charmap_attach(&app,&ui); }
static void step(void) {}
static void cleanup(void) {}
#define APP_NAME "Character Map"
#elif defined(CASE_HELP)
#include "../../../apps/accessories/help.h"
static struct help_app app;
static void setup(void) { assert(!help_attach(&app,&ui)); }
static void step(void) {}
static void cleanup(void) { help_detach(&app); }
#define APP_NAME "Help"
#elif defined(CASE_VIEWER)
#include "../../../apps/accessories/viewer.h"
static struct viewer_app app;
static void setup(void) { assert(!viewer_attach(&app,&ui,NULL)); }
static void step(void) { viewer_poll(&app); }
static void cleanup(void) { viewer_detach(&app); }
#define APP_NAME "Image Viewer"
#elif defined(CASE_MIXER)
#include "../../../apps/accessories/mixer.h"
static struct mixer_app app;
static void setup(void) { mixer_attach(&app,&ui,NULL); du_timer_start(&ui,MX_T_POLL,1000000000ull,1); }
static void step(void) {}
static void cleanup(void) {}
#define APP_NAME "Volume Control"
#elif defined(CASE_MUSIC)
#include "../../../apps/music/view.h"
static struct music_app app; static int music_playing;
static int send_music(void *p,const struct music_command *c) { (void)p; (void)c; return 0; }
static const struct music_ui_backend backend={.send=send_music};
static void setup(void) {
    music_attach(&app,&ui,&backend);
    if (music_playing) { app.snapshot.rate=44100; app.snapshot.duration=44100*180; app.snapshot.state=MUSIC_PLAYING; music_update(&app,&app.snapshot); }
}
static void step(void) { if(music_playing)app.snapshot.position=virtual_ns*44100/1000000000ull; music_update(&app,&app.snapshot); }
static void cleanup(void) {}
#define APP_NAME "Music Player (stopped)"
#elif defined(CASE_PAINT)
#include "../../../apps/paint/paint.h"
static void setup(void) { assert(!paint_attach(&ui)); }
static void step(void) {}
static void cleanup(void) { paint_detach(); }
#define APP_NAME "CiukPaint"
#elif defined(CASE_NOTE)
#include "../../../apps/ciuknote/app.h"
static struct cn_app app;
static time_t epoch(void *p) { (void)p; return 1791711660; }
static void setup(void) { app.now=epoch; assert(!cn_attach(&app,&ui)); }
static void step(void) {}
static void cleanup(void) { cn_detach(&app); }
#define APP_NAME "CiukNote"
#elif defined(CASE_CONTROL)
#include "../../../apps/control/control.h"
#include "device_fixture.h"
static struct cp_app app;
static int page;
static uint64_t epoch(void *p) { (void)p; return 1791711660+virtual_ns/1000000000ull; }
static int devices(void *p,struct ciuki_devices_info *s) { (void)p; device_fixture(s); return 0; }
static int load_config(void *p,char *out,size_t cap,size_t *n)
{ (void)p; const char *s="wallpaper=none\nclock_seconds=1\n"; assert(strlen(s)<cap); strcpy(out,s); *n=strlen(s); return 0; }
static void setup(void) { app.host.now=epoch; app.host.load=load_config; app.host.devices=devices; assert(!cp_attach(&app,&ui,page)); }
static void step(void) { if (!(virtual_ns%1000000000ull)) cp_tick(&app); }
static void cleanup(void) { cp_detach(&app); }
#define APP_NAME "Control Panel"
#elif defined(CASE_TASKS)
#include "../../../apps/tasks/view.h"
static struct tasks_view app; static struct tm_model model; static int task_page;
static void setup(void) { memset(&model,0,sizeof(model));model.system.tick_hz=100;tv_init(&app,&ui,NULL);tv_page(&app,task_page);tv_update(&app,&model); }
static void step(void) {
    if (!(virtual_ns%1000000000ull)) {
        model.system.total_ticks=virtual_ns/10000000ull;
        model.history_count=virtual_ns/1000000000ull<TM_HISTORY ? (unsigned)(virtual_ns/1000000000ull) : TM_HISTORY;
        tv_update(&app,&model);
    }
}
static void cleanup(void) {}
#define APP_NAME "Task Manager"
#elif defined(CASE_IDE)
#include "../../../apps/ciukide/app.h"
static struct ide_app app;
static void setup(void) { assert(!ide_attach(&app,&ui)); }
static void step(void) { ide_idle(&app); }
static void cleanup(void) { ide_detach(&app); }
#define APP_NAME "CiukIDE"
#elif defined(CASE_REGEDIT)
#include "../../../apps/regedit/regedit.h"
static struct re_app app;
int ciuki_devices(struct ciuki_devices_info *s) { (void)s; errno=ENODEV; return -1; }
static void setup(void) { assert(!re_attach(&app,&ui,getenv("CIUKI_MODEL_REGISTRY"))); }
static void step(void) {}
static void cleanup(void) { re_destroy(&app); }
#define APP_NAME "Registry Editor"
#elif defined(CASE_FILES)
#include "../../../apps/files/app.h"
#include "fake_vfs.h"
static struct files_app app;
static void setup(void) { fake_image_tree(); assert(!files_setup(&app,&ui,&fake_vfs,FM_COMPUTER)); }
static void step(void) {}
static void cleanup(void) { files_destroy(&app); }
#define APP_NAME "Files"
#elif defined(CASE_TERMINAL)
#include "../../../apps/terminal/terminal.h"
#include "fake_vfs.h"
static struct terminal app;
static void setup(void) { fake_image_tree();struct term_host h={.fs=fake_vfs};assert(!term_attach(&app,&ui,&h)); }
static void step(void) {}
static void cleanup(void) { term_detach(&app); }
#define APP_NAME "Terminal"
#elif defined(CASE_GFX)
#include "../../../apps/gfxdemo/gfxdemo.h"
#include <ciuki/gl.h>
static struct gfx_app app; static unsigned gfx_page,scene_calls;
void __real_gfx_cube(struct gfx_scene *,uint64_t,const uint32_t *,int);
void __wrap_gfx_cube(struct gfx_scene *s,uint64_t t,const uint32_t *p,int g) { scene_calls++;__real_gfx_cube(s,t,p,g); }
void __real_gfx_sprites(struct gfx_scene *,uint64_t,const uint32_t *);
void __wrap_gfx_sprites(struct gfx_scene *s,uint64_t t,const uint32_t *p) { scene_calls++;__real_gfx_sprites(s,t,p); }
static void setup(void) { gfx_attach(&app,&ui,NULL);app.page=gfx_page;app.paused=1;du_get(&ui,GFX_TABS)->active=gfx_page; }
static void step(void) { app.scene_ready=0;gfx_advance(&app,virtual_ns); }
static void cleanup(void) { ciuki_gl_close(); }
#define APP_NAME "Graphics Demo (static 2D page)"
#elif defined(CASE_DOSBOX)
#include "../../../apps/dosbox/loading.h"
static void launch_paint(struct du_ui *u,void *arg)
{ (void)arg; dosbox_panel(&u->canvas,"DOS game","applications-games",NULL,"Preparing the DOS session",virtual_ns/1000000ull); }
static void setup(void) { ui.paint=launch_paint; }
static void step(void) { if (!(virtual_ns%100000000ull)) du_invalidate(&ui,(struct du_rect){120,272,400,18}); }
static void cleanup(void) {}
#define APP_NAME "DOS game launcher (animated progress)"
#elif defined(CASE_DEMO)
#define main demo_native_main
#include "../../../apps/demo/demo.c"
#undef main
static void demo_paint(struct du_ui *u,void *arg)
{ (void)arg; draw_region(u->canvas.pixels,"Hello",0,0,u->canvas.clip.y,u->canvas.clip.y+u->canvas.clip.h); }
static void setup(void) { ui.paint=demo_paint; }
static void step(void) {}
static void cleanup(void) {}
#define APP_NAME "Demo"
#else
#error Missing case
#endif
static void budgets(const char *name)
{
    init(); uint64_t before=cpu_ns(); setup(); render();
    double cold_host=(cpu_ns()-before)/1e6, cold_model=100.0+model_us()/1000.0;
    assert(cold_model<=500.0);
    /* Settle background publication before measuring the stable visible view. */
    for (unsigned i=1;i<=1000;i++) { virtual_ns=i*10000000ull; du_tick(&ui,virtual_ns); step(); render(); }
    memset(&cost,0,sizeof(cost)); before=cpu_ns();
#if defined(CASE_GFX)
    scene_calls=0;
#endif
    for (unsigned i=1001;i<=2000;i++) { virtual_ns=i*10000000ull; du_tick(&ui,virtual_ns); step(); render(); }
    double idle_host=(cpu_ns()-before)/1e6;
    double idle_pct=(model_us()+1000*(8*3+10))/100000.0;
    printf("BUDGET %s cold_host_ms=%.3f cold_model_ms=%.3f idle_host_ms=%.3f idle_model_pct=%.4f frames=%llu pixels=%llu compared=%llu fills=%llu\n",
        name,cold_host,cold_model,idle_host,idle_pct,(unsigned long long)cost.frames,(unsigned long long)cost.pixels,
        (unsigned long long)cost.compared,(unsigned long long)cost.fills);
    assert(idle_pct<=1.0);
#if defined(CASE_GFX)
    assert(scene_calls==0);
#endif
#if defined(CASE_PAINT)
    struct du_widget *canvas=du_get(&ui,P_CANVAS); assert(canvas);
    memset(&cost,0,sizeof(cost)); before=cpu_ns();
    struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.value=canvas->box.x+14,.value2=canvas->box.y+14}; du_input(&ui,&e);
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_BUTTON,.code=1,.value=1}; du_input(&ui,&e); render();
    for (unsigned i=0;i<60;i++) {
        virtual_ns+=16666667; e=(struct ciuki_input_event){.type=CIUKI_INPUT_MOTION,.value=canvas->box.x+14+(int)i*3,.value2=canvas->box.y+14+(int)(i%16)};
        du_input(&ui,&e); render();
        assert(ui.rendered.w<ui.canvas.width || ui.rendered.h<ui.canvas.height);
    }
    double stroke_host=(cpu_ns()-before)/1e6,stroke_pct=(model_us()+60*(8*3+10))/10000.0;
    printf("STROKE CiukPaint events=60 host_ms=%.3f model_pct=%.4f fills=%llu pixels=%llu compared=%llu\n",stroke_host,stroke_pct,
        (unsigned long long)cost.fills,(unsigned long long)cost.pixels,(unsigned long long)cost.compared);
    assert(stroke_pct<=15.0); assert(cost.fills<20000); /* catches the old per-pixel fills */
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_BUTTON,.code=1,.value=0}; du_input(&ui,&e);
#endif
    cleanup(); du_dialog_close(&ui);
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
#if defined(CASE_CONTROL)
    static const char *names[]={"Display","Mouse","Keyboard","Sounds","Date and Time","System","Devices","Drivers","Network","Power","Adapter","Fonts","Storage","Programs"};
    for (page=CP_HOME;page<CP_PAGES;page++) { memset(&app,0,sizeof(app)); budgets(page<0 ? "Control Panel Home" : names[page]); }
#elif defined(CASE_GFX)
    for(gfx_page=0;gfx_page<3;gfx_page++)budgets(gfx_page==0 ? "Graphics Demo 2D" : gfx_page==1 ? "Graphics Demo paused cube" : "Graphics Demo paused tiles");
#elif defined(CASE_MUSIC)
    music_playing=0;budgets("Music Player (stopped)");music_playing=1;budgets("Music Player (playing UI)");
#elif defined(CASE_TASKS)
    for(task_page=TV_APPS;task_page<TV_PAGES;task_page++)budgets(task_page==TV_APPS ? "Task Manager Applications" : task_page==TV_PROCESSES ? "Task Manager Processes" : "Task Manager Performance");
#else
    budgets(APP_NAME);
#endif
    puts("PASS F2-98a native update budgets"); return 0;
}
