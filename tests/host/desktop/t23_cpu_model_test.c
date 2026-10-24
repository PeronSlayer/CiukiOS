/* f2-95: T23 CPU model of the desktop.
 *
 * The production compositor (X11/pixman damage regions, frame pacing with
 * the 30 fps small-damage cap, offscreen caches, hardware cursor) runs over
 * a fake display and 2D engine for a virtual time window. Every kernel
 * entry, composed pixel, uploaded byte, engine request and compared byte is
 * counted from the production code and charged at the T23 rates below.
 *
 * T23: ThinkPad T23, Pentium III-M 1.13 GHz, PC133 SDRAM, S3 SuperSavage
 * IX/C (savage2d engine, 1024x768x32, offscreen rows 768-2047).
 *
 * Rates (assumptions unless stated):
 *   ENTRY_US      3     kernel entry+exit with dispatch (sensitivity at 10)
 *   TURN_USER_US  40    user work of one service turn without events
 *   COMPOSE_NS    12    per composed pixel (wallpaper/window rows, ~8 B moved)
 *   LFB_NS_B      7.46  CPU bytes to the LFB: 134 MB/s measured on the T23
 *                       (docs/validation/f4-20-gfxdemo-frame-budget.md)
 *   WRITE_NS_B    7.46  host-to-screen engine writes (same CPU path)
 *   RAM_NS_B      1.67  RAM window copy (memmove, ~600 MB/s)
 *   CMP_NS_B      3     cache comparison incl. XRGB conversion
 *   ENGINE_US     2     engine request beyond its kernel entry
 *   INPUT_US      5     one pointer event through desk_input
 * Kernel entries per service turn, counted in apps/desktop/desktop.c's main
 * loop for a shell with one client and no Win32 program: a4d9454e 24
 * (3 clock reads in each of two empty input drains, two input-time reads,
 * performance-sample and cache-step clock reads); f2-95 17 (empty drains
 * read the clock once, gate-only input timing, the turn's clock for the
 * sample and the cache step, one pre-sleep input read).
 *
 * Desktop workers outside the compositor, measured in the a4d9454e trial
 * ([cpu] top, idle windows): desktop#30 (shortcut scans) 300 per mille,
 * desktop#31 (sound worker that never closed its stream) 85 per mille.
 * After f2-95: shortcut scans every 10 s without stat or game queries
 * (desktop_gate_test --links: 6 scans/min, 0 queries), each ~20 directory
 * opens at 1 ms (t23_desktop_share_test LOOKUP_US) and ~770 entries at
 * 2 us (f1-93b), the sound worker sleeps between sounds
 * (sound_runtime_test: 0 status polls in 100 ms idle).
 * SPDX-License-Identifier: MIT */
#include "fake.h"
#include "../../../apps/libdeskui/deskui.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENTRY_US 3.0
#define TURN_USER_US 40.0
#define COMPOSE_NS 12.0
#define LFB_NS_B 7.46
#define WRITE_NS_B 7.46
#define RAM_NS_B 1.67
#define CMP_NS_B 3.0
#define ENGINE_US 2.0
#define INPUT_US 5.0
#define TURN_ENTRIES_BEFORE 24u
#define TURN_ENTRIES_AFTER 17u
#define LINKS_BEFORE_PM 300.0
#define SOUND_BEFORE_PM 85.0
#define LINKS_AFTER_PM (6.0*(20*1000.0+770*2.0)/60.0/1000.0) /* us per s -> per mille */

static struct desktop d;
static uint64_t vnow;
/* fx-26: inside a timed frame the clock includes the frame's modeled cost
 * so far (the compositor reads it at frame start and end). */
static int timed_frame;
static uint64_t t_entries, t_engine, t_write, t_cmp;
static double frame_cost_ns(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t vclock(void);
static uint32_t vram_first=768, vram_end=2048;
static struct { uint64_t entries, present_px, write_px, engine_ops, compared; } k;
static uint64_t __real_memcmp_bytes;
int __real_memcmp(const void *, const void *, size_t);
int __wrap_memcmp(const void *a, const void *b, size_t n) { __real_memcmp_bytes+=n; return __real_memcmp(a,b,n); }
static uint64_t vclock(void)
{
    if (!timed_frame) return vnow;
    const struct desk_frame_stats *f=&d.frame_stats;
    return vnow+(uint64_t)frame_cost_ns(f->scene_px,f->lfb_bytes,k.entries-t_entries,k.engine_ops-t_engine,
                                        k.write_px-t_write,__real_memcmp_bytes-t_cmp,f->compare_px,f->copy_bytes);
}
static int upload(int display,int surface,const struct ciuki_rect *r)
{ (void)display; (void)surface; k.entries++; k.present_px+=(uint64_t)r->width*r->height; return 0; }
static int display_info(int display,struct ciuki_framebuffer_info *out)
{
    (void)display; k.entries++;
    *out=(struct ciuki_framebuffer_info){.size=sizeof(*out),.width=d.width,.height=d.height,.bpp=32,.generation=55};
    return 0;
}
static int service(int display,const struct ciuki_accel_request *q)
{
    (void)display; k.entries++;
    if (q->operation==CIUKI_ACCEL_QUERY) return 3;
    if (q->operation==CIUKI_ACCEL_OFFSCREEN) return (int)(vram_end<<16|vram_first);
    k.engine_ops++;
    if (q->operation==CIUKI_ACCEL_WRITE) k.write_px+=(uint64_t)q->rect.width*q->rect.height;
    return 0;
}
static int window_new(int ch,int surf,unsigned w,unsigned h,int x,int y)
{
    fake_channel(ch); int i=desk_add_client(&d,ch); assert(i>=0);
    struct ciuki_message m; desk_message(&m,DESK_HELLO,4); m.sender_pid=42+i; desk_receive(&d,i,&m);
    fake_surface(surf,w,h,0x123456);
    desk_message(&m,DESK_CREATE_WINDOW,20); m.sender_pid=42+i; m.fd_count=1; m.fds[0]=surf;
    desk_put32(m.data+4,w); desk_put32(m.data+8,h); desk_put32(m.data+12,4); memcpy(m.data+16,"Play",4);
    desk_receive(&d,i,&m); assert(d.clients[i].window);
    desk_move(&d,i,x,y);
    uint32_t *p=(uint32_t *)d.clients[i].pixels;
    for (unsigned row=0;row<h;row++) for (unsigned col=0;col<w;col++) p[row*w+col]=row<24 ? 0xd4d0c8 : 0xece9d8;
    return i;
}
struct tally {
    double seconds; uint64_t turns, frames, scene_px, lfb_bytes, fills, copies, writes, cache_rows, ram_bytes, events;
    uint64_t entries, write_px, engine_ops, compared, proof_px;
};
static void tally_begin(void)
{
    memset(&d.stats,0,sizeof(d.stats)); memset(&k,0,sizeof(k)); __real_memcmp_bytes=0;
}
static void tally_end(struct tally *t, double seconds, uint64_t turns, uint64_t ram_bytes, uint64_t events)
{
    *t=(struct tally){.seconds=seconds,.turns=turns,.frames=d.stats.frames,.scene_px=d.stats.scene_px,
        .lfb_bytes=d.stats.lfb_bytes,.fills=d.stats.accel_fills,.copies=d.stats.accel_copies,
        .writes=d.stats.accel_writes,.cache_rows=d.stats.cache_rows,.ram_bytes=ram_bytes,.events=events,
        .entries=k.entries,.write_px=k.write_px,.engine_ops=k.engine_ops,.compared=__real_memcmp_bytes,
        .proof_px=d.stats.compare_px};
    if(d.stats.copy_bytes>t->ram_bytes)t->ram_bytes=d.stats.copy_bytes;
}
/* Per mille of one CPU over the window; entry_us varies for sensitivity. */
static double main_pm(const struct tally *t, unsigned turn_entries, double entry_us)
{
    double us=(double)(t->entries+(uint64_t)turn_entries*t->turns+t->events)*entry_us
        +(double)t->turns*TURN_USER_US+(double)t->scene_px*COMPOSE_NS/1000.0
        +(double)t->lfb_bytes*LFB_NS_B/1000.0+(double)t->write_px*4.0*WRITE_NS_B/1000.0
        +(double)t->ram_bytes*RAM_NS_B/1000.0+(double)t->compared*CMP_NS_B/1000.0
        +(double)t->engine_ops*ENGINE_US+(double)t->events*INPUT_US
        +(double)t->proof_px*8.0*CMP_NS_B/1000.0;
    return us/(t->seconds*1000000.0)*1000.0;
}
static void report(const char *name, const struct tally *t)
{
    double w=t->seconds/2.0; /* the [desktop] accel record covers 2 s */
    printf("MODEL %s seconds=%.0f turns_per_s=%.0f frames_per_s=%.1f scene_px_per_frame=%.0f lfb_bytes_per_s=%.0f "
           "engine_ops_per_s=%.0f ram_copy_bytes_per_s=%.0f compared_bytes_per_s=%.0f kernel_entries_per_s=%.0f\n",
           name,t->seconds,t->turns/t->seconds,t->frames/t->seconds,t->frames ? (double)t->scene_px/t->frames : 0.0,
           t->lfb_bytes/t->seconds,t->engine_ops/t->seconds,t->ram_bytes/t->seconds,t->compared/t->seconds,
           (t->entries+(double)TURN_ENTRIES_AFTER*t->turns+t->events)/t->seconds);
    printf("EXPECT %s [desktop] accel fills=%.0f copies=%.0f writes=%.0f fallback=0 cache_rows=%.0f frames=%.0f lfb_bytes=%.0f caps=3 vram_rows=1280\n",
           name,t->fills/w,t->copies/w,t->writes/w,t->cache_rows/w,t->frames/w,t->lfb_bytes/w);
}
static void scene_setup(void)
{
    fake_reset(); struct desk_ops ops=fake_ops; ops.present=upload;
    desk_init(&d,&ops,1024,768); d.display=7; d.time_ns=vclock; d.display_info=display_info;
    d.pixels=calloc((size_t)d.width*d.height,4); assert(d.pixels);
    d.accel=service; desk_accel_reset(&d,55);
    desk_shell_enable(&d);
    snprintf(d.sh.cfg.wallpaper,sizeof(d.sh.cfg.wallpaper),"builtin:%s",desk_builtin_wallpapers[0].id);
    desk_shell_apply(&d); assert(!desk_shell_wallpaper_update(&d));
    d.mouse_x=1000; d.mouse_y=700; d.realtime_second=36000;
}
static void scene_free(void)
{
    for (int i=0;i<DESK_CLIENTS;i++) desk_drop_client(&d,i);
    desk_render_release(&d); free(d.pixels); free(d.sh.icons); free(d.sh.wall); free(d.sh.autoplay.workspace);
    assert(!fake_live());
}
/* The service loop's wait: 10 ms idle, the frame slot when damage waits,
 * the next scripted event; a deadline already passed waits 1 ms. */
static uint64_t wait_ns(uint64_t next_event)
{
    uint64_t wait=10000000ull;
    if (d.damage.count || d.move_damage.count || d.present_damage.count || d.cursor_dirty) {
        uint64_t due=desk_frame_time(&d,vnow); uint64_t left=due>vnow ? due-vnow : 1000000ull;
        if (left<wait) wait=left;
    }
    uint64_t left=next_event>vnow ? next_event-vnow : 1000000ull;
    return left<wait ? left : wait;
}
static void warm(void)
{
    for (unsigned turn=0;turn<3000;turn++) {
        d.now_ns=vnow;
        if (desk_frame_due(&d,vnow)) assert(!desk_redraw(&d));
        else if (!d.damage.count) desk_accel_cache_step(&d,vnow);
        vnow+=1000000ull;
    }
}
/* Music Player open (560x420 like apps/music), nothing else moving: its
 * time label and seek bar repaint 5 times a second (the trial's highest
 * repaint_mps, 5396), the top-bar clock once a minute. */
static void music_idle(struct tally *t)
{
    scene_setup(); vnow=1000000000ull;
    int music=window_new(10,20,560,420,232,170); (void)music;
    warm();
    tally_begin();
    uint64_t begin=vnow, end=begin+10000000000ull, next_music=begin, next_clock=begin+1000000000ull, turns=0;
    unsigned tick=0;
    while (vnow<end) {
        if (vnow>=next_music) {
            struct desk_box content=desk_content(&d.clients[music]);
            uint32_t *p=(uint32_t *)d.clients[music].pixels; unsigned w=d.clients[music].info.width;
            for (unsigned y=53;y<53+18;y++) for (unsigned x=12+(tick%40)*6;x<12+(tick%40)*6+6;x++) p[y*w+x]^=0x00101010;
            for (unsigned y=77;y<77+18;y++) for (unsigned x=12;x<548;x+=97) p[y*w+x]^=0x00080808;
            desk_damage_add(&d.damage,(struct desk_box){content.x+12,content.y+53,536,18},d.width,d.height);
            desk_damage_add(&d.damage,(struct desk_box){content.x+12,content.y+77,536,18},d.width,d.height);
            next_music+=200000000ull; tick++;
        }
        if (vnow>=next_clock) {
            uint64_t previous=d.realtime_second; d.realtime_second++;
            desk_shell_clock_damage(&d,previous); next_clock+=1000000000ull;
        }
        d.now_ns=vnow; turns++;
        if (desk_frame_due(&d,vnow)) assert(!desk_redraw(&d));
        else if (!d.damage.count && !d.move_damage.count && !d.present_damage.count) desk_accel_cache_step(&d,vnow);
        uint64_t next=next_music<next_clock ? next_music : next_clock;
        vnow+=wait_ns(next);
    }
    tally_end(t,(double)(vnow-begin)/1e9,turns,0,0);
    /* A 20-pixel-high label repaint composes the label, never the frame. */
    assert(t->frames && t->scene_px/t->frames<=2*536*18+4096);
    scene_free();
}
/* The same window dragged at 60 frames a second, 8 px right and 6 px down
 * per frame (reversing at the screen edges), PS/2 pointer at 100 Hz. */
static void music_drag(struct tally *t)
{
    scene_setup(); vnow=1000000000ull;
    int music=window_new(10,20,560,420,100,100);
    warm();
    tally_begin();
    uint64_t begin=vnow, end=begin+3000000000ull, next_input=begin, turns=0, events=0, ram=0;
    int dx=8, dy=6;
    while (vnow<end) {
        if (vnow>=next_input) { events++; next_input+=10000000ull; }
        d.now_ns=vnow; turns++;
        if (desk_frame_due(&d,vnow) || vnow>=d.next_frame_ns) {
            struct desk_box f=d.clients[music].frame;
            if (f.x+dx<0 || f.x+f.w+dx>(int)d.width) dx=-dx;
            if (f.y+dy<DESK_BAR || f.y+f.h+dy>(int)d.height-DESK_BAR) dy=-dy;
            desk_move(&d,music,f.x+dx,f.y+dy);
            if (d.copy_pending && d.frame_valid) ram+=(uint64_t)d.copy_rect.width*d.copy_rect.height*4;
            assert(!desk_redraw(&d));
        }
        vnow+=wait_ns(next_input<d.next_frame_ns ? next_input : d.next_frame_ns);
    }
    tally_end(t,(double)(vnow-begin)/1e9,turns,ram,events);
    scene_free();
}
/* F2-98b: producer-paced DOS at 70.09 Hz changing 32 full scanlines;
 * Win32 at 30 Hz filling a 200x200 area; NetSurf's two separate UI labels
 * at 5 Hz. These are explicit workloads, not a guarantee for arbitrary
 * full-screen random pixels (the LFB throughput alone rules that out). */
static void foreign_load(unsigned kind,struct tally *t)
{
    scene_setup();vnow=1000000000ull;
    int client=kind ? window_new(10,20,640,480,100,100) : -1;
    if(kind==1)d.clients[client].dosbox=1;
    warm();tally_begin();
    uint64_t begin=vnow,end=begin+3000000000ull,next=begin,turns=0,events=0;
    uint64_t period=kind==1 ? 14268000ull : kind==2 ? 33333334ull : 200000000ull;
    unsigned tick=0;
    while(vnow<end) {
        d.now_ns=vnow;turns++;
        if(kind && vnow>=next) {
            struct desk_client *c=&d.clients[client];uint32_t *p=(uint32_t *)c->pixels;
            unsigned count=kind==3 ? 2 : 1;
            for(unsigned part=0;part<count;part++) {
                unsigned x=kind==3 ? (part ? 600 : 10) : 0,y=20;
                unsigned w=kind==1 ? 640 : kind==2 ? 200 : 8,h=kind==1 ? 32 : kind==2 ? 200 : 16;
                for(unsigned row=y;row<y+h;row++)for(unsigned col=x;col<x+w;col++)
                    p[row*640+col]=kind==1 ? ((row*92761u+col*817u+tick*7631u)&0xffffff) : (0x112233u+tick);
                struct ciuki_message m;desk_message(&m,DESK_DAMAGE,20);
                m.sender_pid=c->pid;
                desk_put32(m.data+4,x);desk_put32(m.data+8,y);desk_put32(m.data+12,w);desk_put32(m.data+16,h);
                desk_receive(&d,client,&m);events++;
            }
            next+=period;tick++;
        }
        if(desk_frame_due(&d,vnow))assert(!desk_redraw(&d));
        else if(!d.damage.count)desk_accel_cache_step(&d,vnow);
        vnow+=wait_ns(kind ? next : end);
    }
    tally_end(t,(double)(vnow-begin)/1e9,turns,0,events);
    if(kind==0)assert(!t->frames && !t->lfb_bytes && !t->engine_ops);
    if(kind==1)assert(t->frames>=209 && t->frames<=211); /* no 30/60 Hz quantization */
    if(kind==2)assert(t->scene_px<=t->frames*200u*200u);
    if(kind==3)assert(t->scene_px<=t->frames*2u*8u*16u);
    scene_free();
}
/* fx-26 (T23 f441c5bd: window management slow while something else
 * draws): a large window (NetSurf-sized, 800x560) dragged at 60 frames a
 * second while a DOS window (640x480) changes 32 scanlines at 70.09 Hz and
 * the dragged program itself repaints a 200x16 status line at 5 Hz. The
 * modeled cost of every frame (composition, LFB bytes, RAM copy, engine
 * requests, kernel entries) must fit the 16.7 ms slot, so the drag keeps the
 * frame rate. Before fx-26 any pending damage disabled the move copy and
 * every frame recomposed and uploaded both outer boxes in software. */
static double frame_ms(const struct tally *t)
{
    double us=(double)t->entries*ENTRY_US+(double)t->scene_px*COMPOSE_NS/1000.0
        +(double)t->lfb_bytes*LFB_NS_B/1000.0+(double)t->write_px*4.0*WRITE_NS_B/1000.0
        +(double)t->ram_bytes*RAM_NS_B/1000.0+(double)t->compared*CMP_NS_B/1000.0
        +(double)t->engine_ops*ENGINE_US+(double)t->proof_px*8.0*CMP_NS_B/1000.0;
    return t->frames ? us/1000.0/(double)t->frames : 0.0;
}
static void drag_with_dos(struct tally *t, uint64_t *worst_input_ns)
{
    scene_setup(); vnow=1000000000ull;
    int dos=window_new(11,21,640,480,340,250); d.clients[dos].dosbox=1;
    int web=window_new(10,20,800,560,40,60);
    warm(); tally_begin();
    uint64_t begin=vnow, end=begin+3000000000ull, next_dos=begin, next_status=begin, next_input=begin, turns=0, events=0, ram=0;
    uint64_t pending_input=0; *worst_input_ns=0;
    int dx=8, dy=6; unsigned tick=0;
    while (vnow<end) {
        d.now_ns=vnow; turns++;
        if (vnow>=next_dos) {
            struct desk_client *c=&d.clients[dos]; uint32_t *p=(uint32_t *)c->pixels;
            for (unsigned row=20;row<52;row++) for (unsigned col=0;col<640;col++) p[row*640+col]=(row*92761u+col*817u+tick*7631u)&0xffffff;
            struct ciuki_message m; desk_message(&m,DESK_DAMAGE,20); m.sender_pid=c->pid;
            desk_put32(m.data+4,0); desk_put32(m.data+8,20); desk_put32(m.data+12,640); desk_put32(m.data+16,32);
            desk_receive(&d,dos,&m); events++; next_dos+=14268000ull; tick++;
        }
        if (vnow>=next_status) {
            struct desk_client *c=&d.clients[web]; uint32_t *p=(uint32_t *)c->pixels;
            for (unsigned row=530;row<546;row++) for (unsigned col=10;col<210;col++) p[row*800+col]^=0x00101010;
            struct ciuki_message m; desk_message(&m,DESK_DAMAGE,20); m.sender_pid=c->pid;
            desk_put32(m.data+4,10); desk_put32(m.data+8,530); desk_put32(m.data+12,200); desk_put32(m.data+16,16);
            desk_receive(&d,web,&m); events++; next_status+=200000000ull;
        }
        if (vnow>=next_input) {
            /* PS/2 pointer at 100 Hz: the drag moves the window. */
            struct desk_box f=d.clients[web].frame;
            if (f.x+dx<0 || f.x+f.w+dx>(int)d.width) dx=-dx;
            if (f.y+dy<DESK_BAR || f.y+f.h+dy>(int)d.height-DESK_BAR) dy=-dy;
            desk_move(&d,web,f.x+dx,f.y+dy); events++;
            if (!pending_input) pending_input=vnow;
            next_input+=10000000ull;
        }
        if (desk_frame_due(&d,vnow)) {
            if (d.copy_pending && d.frame_valid) ram+=(uint64_t)d.copy_rect.width*d.copy_rect.height*4;
            assert(!desk_redraw(&d));
            if (pending_input) { uint64_t lag=vnow-pending_input; if (lag>*worst_input_ns) *worst_input_ns=lag; pending_input=0; }
        }
        uint64_t next=next_input<next_dos ? next_input : next_dos;
        if (next_status<next) next=next_status;
        vnow+=wait_ns(next);
    }
    tally_end(t,(double)(vnow-begin)/1e9,turns,ram,events);
    scene_free();
}
/* fx-26: the same drags with the frame taking its modeled T23 time (the
 * virtual clock advances by each frame's composition, LFB, RAM-copy, engine
 * and kernel-entry cost) and the desktop's wake-ups late by a deterministic
 * 0-4 ms (T23 f441c5bd: resched_lat_max_ms 3-9 for the desktop). Measured:
 * frames per second and the pointer-motion -> frame latency. */
struct timed { double fps, frame_ms, input_max_ms, input_mean_ms; uint64_t frames; };
static double frame_cost_ns(uint64_t scene, uint64_t lfb, uint64_t entries, uint64_t engine, uint64_t write, uint64_t cmp, uint64_t proof, uint64_t ram)
{
    return (double)entries*ENTRY_US*1000.0+(double)scene*COMPOSE_NS+(double)lfb*LFB_NS_B+(double)write*4.0*WRITE_NS_B+
        (double)ram*RAM_NS_B+(double)cmp*CMP_NS_B+(double)engine*ENGINE_US*1000.0+(double)proof*8.0*CMP_NS_B;
}
static struct timed timed_drag(int w, int h, int dos_window, unsigned lateness_ms, int engine)
{
    scene_setup(); vnow=1000000000ull;
    if (!engine) { d.accel=NULL; d.accel_caps=0; d.hw_cursor=d.hw_cursor_shown=0; d.cursor_dirty=1; }
    int dos=dos_window ? window_new(11,21,640,480,340,250) : -1;
    if (dos>=0) d.clients[dos].dosbox=1;
    int win=window_new(10,20,(unsigned)w,(unsigned)h,40,60);
    warm(); tally_begin();
    uint64_t begin=vnow, end=begin+4000000000ull, next_dos=begin, next_input=begin, pending=0, lag_sum=0, lag_max=0, lags=0;
    double busy_ns=0; int dx=8, dy=6; unsigned tick=0, rnd=12345;
    while (vnow<end) {
        d.now_ns=vnow;
        if (dos>=0 && vnow>=next_dos) {
            struct desk_client *c=&d.clients[dos]; uint32_t *p=(uint32_t *)c->pixels;
            for (unsigned row=20;row<52;row++) for (unsigned col=0;col<640;col++) p[row*640+col]=(row*92761u+col*817u+tick*7631u)&0xffffff;
            struct ciuki_message m; desk_message(&m,DESK_DAMAGE,20); m.sender_pid=c->pid;
            desk_put32(m.data+4,0); desk_put32(m.data+8,20); desk_put32(m.data+12,640); desk_put32(m.data+16,32);
            desk_receive(&d,dos,&m); next_dos+=14268000ull; tick++;
        }
        while (vnow>=next_input) {
            struct desk_box f=d.clients[win].frame;
            if (f.x+dx<0 || f.x+f.w+dx>(int)d.width) dx=-dx;
            if (f.y+dy<DESK_BAR || f.y+f.h+dy>(int)d.height-DESK_BAR) dy=-dy;
            desk_move(&d,win,f.x+dx,f.y+dy);
            if (!pending) pending=next_input;
            next_input+=10000000ull;
        }
        if (desk_frame_due(&d,vnow)) {
            uint64_t scene=d.stats.scene_px, lfb=d.stats.lfb_bytes, entries=k.entries, engine=k.engine_ops, write=k.write_px,
                cmp=__real_memcmp_bytes, proof=d.stats.compare_px;
            uint64_t ram=d.copy_pending && d.frame_valid ? (uint64_t)d.copy_rect.width*d.copy_rect.height*4 : 0;
            uint64_t moved=pending;
            memset(&d.frame_stats,0,sizeof(d.frame_stats));
            t_entries=k.entries; t_engine=k.engine_ops; t_write=k.write_px; t_cmp=__real_memcmp_bytes; timed_frame=1;
            assert(!desk_redraw(&d));
            timed_frame=0;
            double cost=frame_cost_ns(d.stats.scene_px-scene,d.stats.lfb_bytes-lfb,k.entries-entries,k.engine_ops-engine,
                                      k.write_px-write,__real_memcmp_bytes-cmp,d.stats.compare_px-proof,ram);
            (void)ram;
            busy_ns+=cost; vnow+=(uint64_t)cost;
            if (moved) { uint64_t lag=vnow-moved; lag_sum+=lag; lags++; if (lag>lag_max) lag_max=lag; pending=0; }
            continue;
        }
        uint64_t next=next_input<next_dos ? next_input : next_dos;
        uint64_t wait=wait_ns(next);
        rnd=rnd*1103515245u+12345u;
        vnow+=wait+(lateness_ms ? (uint64_t)((rnd>>16)%(lateness_ms*1000u))*1000ull : 0); /* late wake-up */
    }
    struct timed r={.frames=d.stats.frames};
    double seconds=(double)(vnow-begin)/1e9;
    r.fps=d.stats.frames/seconds; r.frame_ms=d.stats.frames ? busy_ns/1e6/d.stats.frames : 0;
    r.input_max_ms=lag_max/1e6; r.input_mean_ms=lags ? (double)lag_sum/lags/1e6 : 0;
    scene_free();
    return r;
}
/* fx-26: minimise, restore, maximise and restore of an 800x560 window
 * while a DOS window plays, each operation at a random slot phase with the
 * clock advancing by the frame cost: operation -> first frame showing it,
 * the cost of that frame and the frames of the 150 ms caption animation. */
struct op_result { double first_ms, first_cost_ms, max_first_ms; unsigned animation_frames; };
static struct op_result timed_ops(unsigned lateness_ms)
{
    scene_setup(); vnow=1000000000ull;
    int dos=window_new(11,21,640,480,340,250); d.clients[dos].dosbox=1;
    int win=window_new(10,20,800,560,40,60);
    warm();
    struct op_result r={0,0,0,0}; unsigned ops=0, rnd=777; double sum=0, cost_sum=0;
    uint64_t next_dos=vnow;
    for (unsigned op=0;op<16;op++) {
        uint64_t at=vnow+50000000ull+(uint64_t)((op*7919u)%17u)*1000000ull, shown=0; double first_cost=0;
        unsigned frames_before=(unsigned)d.stats.frames; int done=0, anim_seen=0;
        uint64_t until=at+400000000ull;
        while (vnow<until) {
            d.now_ns=vnow;
            if (vnow>=next_dos) {
                struct desk_client *c=&d.clients[dos]; uint32_t *p=(uint32_t *)c->pixels;
                for (unsigned row=20;row<52;row++) for (unsigned col=0;col<640;col++) p[row*640+col]^=0x00010101;
                struct ciuki_message m; desk_message(&m,DESK_DAMAGE,20); m.sender_pid=c->pid;
                desk_put32(m.data+4,0); desk_put32(m.data+8,20); desk_put32(m.data+12,640); desk_put32(m.data+16,32);
                desk_receive(&d,dos,&m); next_dos+=14268000ull;
            }
            if (!done && vnow>=at) {
                if (op%4==2) desk_zoom(&d,win); else if (op%4==3) desk_zoom(&d,win); else desk_minimize(&d,win);
                done=1; at=vnow;
            }
            desk_animation_tick(&d);
            if (desk_frame_due(&d,vnow)) {
                memset(&d.frame_stats,0,sizeof(d.frame_stats));
                t_entries=k.entries; t_engine=k.engine_ops; t_write=k.write_px; t_cmp=__real_memcmp_bytes; timed_frame=1;
                uint64_t start=vnow; assert(!desk_redraw(&d)); uint64_t end=vclock(); timed_frame=0;
                vnow=end;
                if (done && !shown) { shown=vnow; first_cost=(double)(end-start)/1e6; }
                if (done && d.animation.active) anim_seen++;
                continue;
            }
            uint64_t wait=wait_ns(next_dos<at && !done ? next_dos : (done ? next_dos : at));
            uint64_t anim=d.animation.active ? d.animation.started+(d.animation.step+1)*25000000ull : UINT64_MAX;
            if (anim>vnow && anim-vnow<wait) wait=anim-vnow;
            rnd=rnd*1103515245u+12345u;
            vnow+=wait+(lateness_ms ? (uint64_t)((rnd>>16)%(lateness_ms*1000u))*1000ull : 0);
        }
        assert(shown);
        double ms=(double)(shown-at)/1e6; sum+=ms; cost_sum+=first_cost; if (ms>r.max_first_ms) r.max_first_ms=ms;
        r.animation_frames+=(unsigned)anim_seen; ops++;
        (void)frames_before;
    }
    r.first_ms=sum/ops; r.first_cost_ms=cost_sum/ops; r.animation_frames/=ops;
    scene_free();
    return r;
}
int main(void)
{
    struct tally idle, drag;
    music_idle(&idle); report("music-idle",&idle);
    music_drag(&drag); report("window-drag",&drag);
    double main_idle=main_pm(&idle,TURN_ENTRIES_AFTER,ENTRY_US), main_before=main_pm(&idle,TURN_ENTRIES_BEFORE,ENTRY_US);
    double main_idle10=main_pm(&idle,TURN_ENTRIES_AFTER,10.0), main_drag=main_pm(&drag,TURN_ENTRIES_AFTER,ENTRY_US);
    double main_drag10=main_pm(&drag,TURN_ENTRIES_AFTER,10.0);
    double before=main_before+LINKS_BEFORE_PM+SOUND_BEFORE_PM, after=main_idle+LINKS_AFTER_PM;
    printf("CPU music-idle before(a4d9454e) desktop#28=%.0f desktop#30=%.0f desktop#31=%.0f total=%.0f per mille\n",
           main_before,LINKS_BEFORE_PM,SOUND_BEFORE_PM,before);
    printf("CPU music-idle after(f2-95) desktop#28=%.1f (entry 10 us: %.1f) desktop#30=%.1f desktop#31=0 total=%.1f per mille\n",
           main_idle,main_idle10,LINKS_AFTER_PM,after);
    printf("CPU window-drag after(f2-95) desktop#28=%.0f (entry 10 us: %.0f) per mille\n",main_drag,main_drag10);
    printf("CPU composition share: music-idle %.2f per mille, window-drag %.1f per mille\n",
           (double)idle.scene_px*COMPOSE_NS/1000.0/(idle.seconds*1000.0),
           (double)drag.scene_px*COMPOSE_NS/1000.0/(drag.seconds*1000.0));
    printf("EXPECT music-idle [cpu] top: desktop#28 about %.0fk, desktop#30 and desktop#31 absent from the top five\n",main_idle);
    /* Directive budgets: under 10 % with the player open, under 40 % dragging. */
    assert(after<100.0 && main_idle10+LINKS_AFTER_PM<100.0);
    assert(main_drag<400.0 && main_drag10<400.0);
    puts("PASS F2-95 T23 CPU model: music player idle and window drag within budget");
    const char *names[]={"desktop-idle","dos-70hz-32rows","win32-200x200-fill","netsurf-disjoint-labels"};
    const double budgets[]={20.0,250.0,50.0,50.0};
    for(unsigned kind=0;kind<4;kind++) {
        struct tally t;foreign_load(kind,&t);report(names[kind],&t);
        double cpu=main_pm(&t,TURN_ENTRIES_AFTER,ENTRY_US)+LINKS_AFTER_PM;
        printf("CPU F2-98b %s total_pm=%.2f budget_pm=%.0f entry_10us_pm=%.2f proof_px=%llu\n",
            names[kind],cpu,budgets[kind],main_pm(&t,TURN_ENTRIES_AFTER,10.0)+LINKS_AFTER_PM,
            (unsigned long long)t.proof_px);
        assert(cpu<=budgets[kind]);
    }
    puts("PASS F2-98b T23 budgets: idle, native-rate DOS scanlines, Win32 area, NetSurf separated labels");
    {
        struct tally t; uint64_t input_ns;
        drag_with_dos(&t,&input_ns); report("fx26-drag-800x560-with-dos",&t);
        double ms=frame_ms(&t), fps=t.frames/t.seconds;
        printf("CPU fx-26 drag 800x560 with a DOS window at 70 Hz: frames_per_s=%.1f modeled_frame_ms=%.2f slot_ms=16.67 "
               "scene_px_per_frame=%.0f lfb_bytes_per_frame=%.0f engine_copies_per_frame=%.1f input_to_frame_max_ms=%.1f\n",
               fps,ms,t.frames ? (double)t.scene_px/t.frames : 0.0,t.frames ? (double)t.lfb_bytes/t.frames : 0.0,
               t.frames ? (double)t.copies/t.frames : 0.0,(double)input_ns/1e6);
        assert(fps>=59.0 && t.copies>=t.frames/2);
        assert(ms<=16.67);
        assert(input_ns<=17000000ull);
        puts("PASS fx-26 T23 window drag keeps the frame rate while other windows draw");
        static const struct { int w, h, dos, engine; const char *name; } drags[]={
            {560,420,0,1,"560x420"},{800,560,0,1,"800x560"},{800,560,1,1,"800x560+dos"},{1000,680,0,1,"1000x680"},
            {560,420,0,0,"560x420-software"}};
        static const unsigned lates[]={0,4,9}; /* f441c5bd: desktop resched_lat_max_ms 3, 9 with a DOS VM */
        /* FX26_NO_ASSERT=1 prints the table without asserting (negative-control
         * runs against older apps/desktop sources). */
        for (unsigned i=0;i<sizeof(drags)/sizeof(*drags);i++) for (unsigned li=0;li<3;li++) {
            unsigned late=lates[li];
            struct timed r=timed_drag(drags[i].w,drags[i].h,drags[i].dos,late,drags[i].engine);
            fflush(stdout); printf("TIMED fx-26 drag %s wake_late_max_ms=%u frames_per_s=%.1f modeled_frame_ms=%.2f input_to_frame_mean_ms=%.1f max_ms=%.1f\n",
                   drags[i].name,late,r.fps,r.frame_ms,r.input_mean_ms,r.input_max_ms); fflush(stdout);
            /* The frame rate holds while the frame fits its slot; input is
             * on screen within one frame period plus the frame itself. */
            if (r.frame_ms+late<16.0 && !getenv("FX26_NO_ASSERT")) assert(r.fps>=55.0);
            if (!getenv("FX26_NO_ASSERT")) assert(r.input_max_ms<=16.67+r.frame_ms*2+late+1.0);
        }
        puts("PASS fx-26 T23 timed drags: no lost slots from late frame starts");
        for (unsigned li=0;li<3;li++) {
            struct op_result o=timed_ops(lates[li]);
            fflush(stdout);
            printf("TIMED fx-26 minimise/restore/maximise 800x560+dos wake_late_max_ms=%u op_to_frame_mean_ms=%.1f max_ms=%.1f first_frame_cost_ms=%.1f animation_frames=%u\n",
                   lates[li],o.first_ms,o.max_first_ms,o.first_cost_ms,o.animation_frames); fflush(stdout);
            if (!getenv("FX26_NO_ASSERT")) assert(o.max_first_ms<=16.67+o.first_cost_ms*2+lates[li]+1.0 && o.animation_frames>=5);
        }
        puts("PASS fx-26 T23 window operations: first frame within one slot plus its own cost");
    }
    return 0;
}
