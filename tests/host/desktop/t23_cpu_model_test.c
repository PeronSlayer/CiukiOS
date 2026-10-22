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
static uint64_t vclock(void) { return vnow; }
static uint32_t vram_first=768, vram_end=2048;
static struct { uint64_t entries, present_px, write_px, engine_ops, compared; } k;
static uint64_t __real_memcmp_bytes;
int __real_memcmp(const void *, const void *, size_t);
int __wrap_memcmp(const void *a, const void *b, size_t n) { __real_memcmp_bytes+=n; return __real_memcmp(a,b,n); }
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
    return 0;
}
