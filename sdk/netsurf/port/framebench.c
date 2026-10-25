/* SPDX-License-Identifier: MIT
 * fx-nsperf host frame bench (--ciuki-frame-bench, host builds only).
 *
 * The host browser draws into libnsfb's RAM surface. Every libnsfb update
 * the frontend issues (fb_redraw, the pan copy, scroll bars) is mirrored into
 * a production Ciuki v3 surface (surface.c) through ciuki_browser_update, the
 * routine nsfb_surface.c installs on CiukiOS, over a model of the desktop
 * channel: the kernel holds CIUKI_CHANNEL_QUEUE_MAX (64) messages per
 * direction, a desktop service turn reads at most 16 messages of one client
 * (apps/desktop/desktop.c) and answers every v3 request with a RESULT. The
 * end of a frame is the end of the main-loop turn (ciuki_chrome_present):
 * present, and while the queue refuses, wait for desktop turns.
 *
 * Each frame is charged on the T23 model (Pentium III-M 1.13 GHz, PC133):
 * browser plotting and desktop damage bookkeeping at 32x their host time
 * (f6-20 T23_CLOCK_SCALE, tests/host/netsurf/test_local.py); kernel entries,
 * compared/copied bytes and composition at the rates of
 * tests/host/desktop/t23_cpu_model_test.c (3 us per entry, 3 ns per compared
 * byte, 1.67 ns per RAM byte, 12 ns per composed pixel, 7.46 ns per LFB byte).
 * Counts come from the production code; the rates are a model, not T23
 * timings. */
#include <stdbool.h>
#include "framebuffer/ciuki/chrome.h"
bool ciuki_frame_bench;
#ifndef CIUKI_HOST
void ciuki_frame_bench_run(void *root,struct gui_window *window) { (void)root;(void)window; }
#else
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <libnsfb.h>
#include <ciuki/font.h>
#include "framebuffer/gui.h"
#include "framebuffer/fbtk.h"
#include "framebuffer/ciuki/surface.h"
#include "utils/errors.h"
#include "framebuffer/ciuki/font.h"
#include "apps/desktop/desktop.h"

extern void (*nsfb_ram_update_hook)(nsfb_t *,nsfb_bbox_t *);
void ciuki_frame_bench_scroll(struct gui_window *,int);
void ciuki_frame_bench_queue(struct gui_window *,int,int,int,int);

#define BENCH_SCALE 32.0      /* host user time -> T23 */
#define ENTRY_US 3.0
#define CMP_NS_B 3.0
#define RAM_NS_B 1.67
#define COMPOSE_NS 12.0
#define LFB_NS_B 7.46
#define TURN_US 91.0          /* one desktop service turn: 17 entries + 40 us user (t23_cpu_model_test.c) */
#define DESKTOP_READS 16u     /* client messages per desktop service turn */

static uint64_t ns(void)
{ struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec; }

/* Desktop channel model. */
static struct {
    unsigned queued;          /* client -> desktop, at most CIUKI_CHANNEL_QUEUE_MAX */
    uint64_t sends, client_reads, desktop_reads, desktop_turns, waits;
    uint64_t desktop_ns;      /* host time of the desktop's damage bookkeeping */
    struct desk_damage damage;
    nsfb_bbox_t pending[CIUKI_CHANNEL_QUEUE_MAX];
    bool damage_op[CIUKI_CHANNEL_QUEUE_MAX];
    unsigned head;
} channel;
static struct { struct ciuki_surface_info info; void *map; } shm[8];
static int fixture_create(uint32_t w,uint32_t h,uint32_t format)
{
    for(int fd=4;fd<8;fd++) if(!shm[fd].map) {
        shm[fd].info=(struct ciuki_surface_info){.size=sizeof(struct ciuki_surface_info),.width=w,.height=h,.stride=w*4,
            .format=format,.allocation_bytes=(w*4*h+4095)&~4095u};
        shm[fd].map=calloc(1,shm[fd].info.allocation_bytes);
        return shm[fd].map?fd:-1;
    }
    errno=EMFILE;return -1;
}
static int fixture_info(int fd,struct ciuki_surface_info *out)
{ if(fd<4 || fd>=8 || !shm[fd].map) { errno=EBADF;return -1; } *out=shm[fd].info;return 0; }
static void *fixture_map(int fd,int prot) { (void)prot;return fd>=4 && fd<8?shm[fd].map:MAP_FAILED; }
static int fixture_unmap(void *p,size_t n) { (void)p;(void)n;return 0; }
static int fixture_close(int fd)
{ if(fd>=4 && fd<8 && shm[fd].map) { free(shm[fd].map);shm[fd].map=NULL; } return 0; }
static int fixture_send(int fd,const struct ciuki_message *m,uint32_t flags)
{
    (void)fd;(void)flags;
    if(channel.queued>=CIUKI_CHANNEL_QUEUE_MAX) { errno=EAGAIN;return -1; }
    unsigned at=(channel.head+channel.queued++)%CIUKI_CHANNEL_QUEUE_MAX;
    channel.damage_op[at]=m->data[0]==DESK_V3_VERSION && m->data[1]==DESK_DAMAGE;
    if(channel.damage_op[at]) {
        int x=(int)desk_get32(m->data+16),y=(int)desk_get32(m->data+20);
        channel.pending[at]=(nsfb_bbox_t){x,y,x+(int)desk_get32(m->data+24),y+(int)desk_get32(m->data+28)};
    }
    channel.sends++;return 0;
}
/* RESULTs are counted as client reads; their content is not needed here. */
static int fixture_recv(int fd,struct ciuki_message *m,uint32_t flags) { (void)fd;(void)m;(void)flags;errno=EAGAIN;return -1; }
/* One desktop service turn: read up to 16 messages, record DAMAGE in the
 * desktop's damage list (compositor.c desk_damage_add, the production code),
 * queue a RESULT for each; the client reads them on its next UI turn. A
 * composed frame clears the list; the model composes after every turn. */
static void desktop_turn(void)
{
    unsigned n=channel.queued<DESKTOP_READS?channel.queued:DESKTOP_READS;
    uint64_t start=ns();
    for(unsigned i=0;i<n;i++) {
        unsigned at=(channel.head+i)%CIUKI_CHANNEL_QUEUE_MAX;
        if(channel.damage_op[at]) {
            nsfb_bbox_t b=channel.pending[at];
            desk_damage_add(&channel.damage,(struct desk_box){b.x0,b.y0,b.x1-b.x0,b.y1-b.y0},2048,2048);
        }
    }
    channel.desktop_ns+=ns()-start;
    channel.damage.count=0;
    channel.head=(channel.head+n)%CIUKI_CHANNEL_QUEUE_MAX;channel.queued-=n;
    channel.desktop_reads+=n;channel.client_reads+=n;channel.desktop_turns++;
}

static struct ciuki_browser_surface mirror;
static nsfb_t *ram;
static uint64_t hook_ns;
static uint64_t updates,update_px;
static void mirror_update(nsfb_t *fb,nsfb_bbox_t *box)
{
    uint64_t start=ns();
    uint8_t *pixels;int stride,w,h;
    nsfb_get_buffer(fb,&pixels,&stride);nsfb_get_geometry(fb,&w,&h,NULL);
    nsfb_bbox_t b=box?*box:(nsfb_bbox_t){0,0,w,h};
    if(b.x0<0)b.x0=0; if(b.y0<0)b.y0=0; if(b.x1>w)b.x1=w; if(b.y1>h)b.y1=h;
    if(b.x1>b.x0 && b.y1>b.y0) {
        /* On CiukiOS the frontend draws straight into the surface's back
         * buffer; only the updated box can differ from it here. */
        unsigned out=mirror.current.info.stride/4;
        for(int y=b.y0;y<b.y1;y++)
            memcpy(mirror.current.back+(size_t)(y+CIUKI_BROWSER_TOP)*out+b.x0,pixels+(size_t)y*stride+(size_t)b.x0*4,(size_t)(b.x1-b.x0)*4);
        updates++;update_px+=(uint64_t)(b.x1-b.x0)*(uint64_t)(b.y1-b.y0);
        b.y0+=CIUKI_BROWSER_TOP;b.y1+=CIUKI_BROWSER_TOP;
        ciuki_browser_update(&mirror,b);
    }
    hook_ns+=ns()-start;
}
/* The present at the end of a main-loop turn (ciuki_chrome_present, then
 * nsfb_surface.c input): while the channel refuses, the browser waits for
 * the desktop. */
static void ui_turn(void)
{
    uint64_t start=ns();
    for(unsigned guard=0;guard<1000000;guard++) {
        ciuki_browser_present(&mirror);
        if(!mirror.dirty)break;
        channel.waits++;
        while(channel.queued)desktop_turn();
    }
    hook_ns+=ns()-start;
    while(channel.queued)desktop_turn();
}
struct sample {
    const char *kind;
    double plot_us,present_us,desktop_us,model_ms;
    uint64_t updates,update_px,messages,damage_px,compared_px,copied_px,waits,sends,turns,glyph_misses,glyph_hits;
};
static void frame(const char *kind,struct sample *out,void (*action)(struct gui_window *,int),struct gui_window *gw,int arg,fbtk_widget_t *root)
{
    struct ciuki_present_stats before=mirror.stats;
    struct ciuki_font_cache_stats glyphs0,glyphs1;ciuki_font_stats(&glyphs0);
    uint64_t sends=channel.sends,reads=channel.client_reads,dreads=channel.desktop_reads,turns=channel.desktop_turns,waits=channel.waits,desk=channel.desktop_ns;
    updates=update_px=0;hook_ns=0;
    uint64_t start=ns();
    action(gw,arg);
    fbtk_redraw(root);
    uint64_t plotted=ns()-start-hook_ns;
    ui_turn();
    ciuki_font_stats(&glyphs1);
    struct sample s={.kind=kind,.updates=updates,.update_px=update_px};
    s.messages=mirror.stats.messages-before.messages;s.damage_px=mirror.stats.damage_px-before.damage_px;
    s.compared_px=mirror.stats.compared_px-before.compared_px;s.copied_px=mirror.stats.copied_px-before.copied_px;
    s.waits=channel.waits-waits;s.sends=channel.sends-sends;s.turns=channel.desktop_turns-turns;
    s.glyph_misses=glyphs1.misses-glyphs0.misses;s.glyph_hits=glyphs1.hits-glyphs0.hits;
    uint64_t entries=s.sends+(channel.client_reads-reads)+(channel.desktop_reads-dreads)*2; /* desktop read + RESULT send */
    s.plot_us=(double)plotted*BENCH_SCALE/1000.0;
    s.present_us=(double)s.compared_px*4*CMP_NS_B/1000.0+(double)s.copied_px*4*RAM_NS_B/1000.0+(double)entries*ENTRY_US*0.5;
    s.desktop_us=(double)(channel.desktop_ns-desk)*BENCH_SCALE/1000.0+(double)entries*ENTRY_US*0.5+(double)s.turns*TURN_US+
        (double)s.damage_px*(COMPOSE_NS+4*LFB_NS_B)/1000.0;
    s.model_ms=(s.plot_us+s.present_us+s.desktop_us)/1000.0;
    *out=s;
    fprintf(stderr,"[netsurf] frame-bench kind=%s model_t23_ms=%.1f plot_us=%.0f present_us=%.0f desktop_us=%.0f updates=%llu update_px=%llu "
        "damage_messages=%llu damage_px=%llu compared_px=%llu copied_px=%llu channel_sends=%llu desktop_turns=%llu ui_waits=%llu glyph_hits=%llu glyph_misses=%llu\n",
        kind,s.model_ms,s.plot_us,s.present_us,s.desktop_us,(unsigned long long)s.updates,(unsigned long long)s.update_px,
        (unsigned long long)s.messages,(unsigned long long)s.damage_px,(unsigned long long)s.compared_px,(unsigned long long)s.copied_px,
        (unsigned long long)s.sends,(unsigned long long)s.turns,(unsigned long long)s.waits,
        (unsigned long long)s.glyph_hits,(unsigned long long)s.glyph_misses);
}
static void full(struct gui_window *gw,int unused) { (void)unused;fbtk_request_redraw(gw->browser); }
static void scroll(struct gui_window *gw,int dy) { ciuki_frame_bench_scroll(gw,dy); }
static void small_update(struct gui_window *gw,int unused)
{ (void)unused;ciuki_frame_bench_queue(gw,40,60,280,80); }
static int compare(const void *a,const void *b)
{ double x=*(const double *)a,y=*(const double *)b;return x<y?-1:x>y; }
static void summary(const char *kind,struct sample *s,unsigned n)
{
    double v[64];uint64_t messages=0,waits=0,misses=0,compared=0;
    for(unsigned i=0;i<n;i++) { v[i]=s[i].model_ms;messages+=s[i].messages;waits+=s[i].waits;misses+=s[i].glyph_misses;compared+=s[i].compared_px; }
    qsort(v,n,sizeof(*v),compare);
    fprintf(stderr,"[netsurf] frame-bench summary kind=%s frames=%u median_model_t23_ms=%.1f max_model_t23_ms=%.1f damage_messages=%llu ui_waits=%llu glyph_misses=%llu compared_px=%llu\n",
        kind,n,v[n/2],v[n-1],(unsigned long long)messages,(unsigned long long)waits,(unsigned long long)misses,(unsigned long long)compared);
}
void ciuki_frame_bench_run(void *root_widget,struct gui_window *gw)
{
    fbtk_widget_t *root=root_widget;
    ram=fbtk_get_nsfb(root);
    int w,h;nsfb_get_geometry(ram,&w,&h,NULL);
    const struct ciuki_browser_ops ops={.transport={fixture_send,fixture_recv,fixture_info,fixture_close},
        .create=fixture_create,.map=fixture_map,.unmap=fixture_unmap};
    if(ciuki_browser_open(&mirror,&ops,3,w,h+CIUKI_BROWSER_TOP+CIUKI_BROWSER_STATUS)) {
        fprintf(stderr,"[netsurf] frame-bench error=surface\n");return;
    }
    /* The desktop's CREATE_WINDOW RESULT. */
    struct ciuki_message m;struct desk_packet p;
    desk_v3_message(&m,DESK_V3_RESULT,8,mirror.create_request,mirror.window,mirror.generation);
    desk_put32(m.data+16,DESK_CREATE_WINDOW);desk_put32(m.data+20,0);
    if(!desk_parse(&m,0,NULL,&p) || ciuki_browser_packet(&mirror,&p) || !mirror.ready) {
        fprintf(stderr,"[netsurf] frame-bench error=create\n");ciuki_browser_close(&mirror);return;
    }
    /* The window's first full present (zero pixels on both sides). Only the
     * content area is mirrored; the chrome rows stay as created. */
    ui_turn();
    nsfb_ram_update_hook=mirror_update;
    struct sample first,repaint[3],scrolls[12],smalls[3],page;
    frame("first",&first,full,gw,0,root);
    for(unsigned i=0;i<3;i++)frame("repaint",&repaint[i],full,gw,0,root);
    for(unsigned i=0;i<12;i++)frame("scroll",&scrolls[i],scroll,gw,100,root);
    for(unsigned i=0;i<3;i++)frame("small",&smalls[i],small_update,gw,0,root);
    frame("page",&page,scroll,gw,h,root);
    summary("repaint",repaint,3);summary("scroll",scrolls,12);summary("small",smalls,3);summary("page",&page,1);
    nsfb_ram_update_hook=NULL;
    ciuki_browser_close(&mirror);
}
#endif
