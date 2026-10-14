/* SPDX-License-Identifier: MIT */
#include "gfxdemo.h"
#include "../desktop/rows.h"
#include "../accessories/acc.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* The baseline YIELD syscall has no libc wrapper. */
extern uint32_t ciuki_syscall(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
struct runtime {
    struct du_ui ui;
    struct du_window window;
    struct gfx_app app;
    uint32_t *portrait;
    uint32_t serial,bench_serial,info_serial,system_serial;
    uint64_t next_info,next_bench;
    int bench_pending,scene_active;
    uint32_t frame_serial;
    uint64_t next_slot;
    uint64_t submitted_ns,submitted_render_ns,submitted_copy_ns;
    uint32_t timing_serial;
};
static int request(void *ctx,unsigned op,unsigned index)
{
    struct runtime *r=ctx; struct ciuki_message m;
    desk_message(&m,op,op==DESK_GFX_BENCH ? 12 : 8);
    uint32_t serial=++r->serial; desk_put32(m.data+4,serial);
    if (op==DESK_GFX_BENCH) desk_put32(m.data+8,index);
    if (du_window_queue(&r->window,&m)) return -1;
    if (op==DESK_GFX_BENCH) { r->bench_serial=serial;r->bench_pending=1;r->next_bench=du_now()+150000000ull; }
    else if (op==DESK_GFX_QUERY) r->info_serial=serial;
    else r->system_serial=serial;
    return 0;
}
static void message(struct du_window *w,const struct desk_packet *p,void *ctx)
{
    (void)w;struct runtime *r=ctx;
    if (p->opcode==DESK_GFX_PRESENTED && r->window.present_pending && p->serial==r->frame_serial) {
        r->window.present_pending=0; r->next_slot=p->next_frame_ns;
        uint64_t now=du_now();
        r->app.render_ns=r->submitted_render_ns;r->app.submit_ns=r->submitted_copy_ns;
        r->app.wait_ns=now>=r->submitted_ns ? now-r->submitted_ns : 0;
        r->app.convert_ns=p->convert_ns;r->app.copy_ns=p->copy_ns;
        r->app.slot_ns=p->slot_ns;r->app.refresh_ns=p->refresh_ns;r->app.have_timing=1;
        r->timing_serial=p->serial;
        if (!p->result && r->scene_active) gfx_presented(&r->app,p->completed_ns);
        if (p->result && p->result!=-EAGAIN) { r->window.error=-p->result; r->window.closed=1; }
        if (p->result) {
            r->app.scene_ready=0;r->app.animation_due=0;r->app.next_ns=p->next_frame_ns;
        }
    } else if ((p->opcode==DESK_GFX_RESULT || (p->opcode==DESK_TASK_RESULT && p->request==DESK_GFX_BENCH)) &&
        r->bench_pending && p->serial==r->bench_serial) {
        r->bench_pending=0;
        if (p->opcode==DESK_GFX_RESULT && p->result==-EBUSY) return; /* wait for focus/rate limit */
        gfx_receive(&r->app,p);
    } else if ((p->opcode==DESK_GFX_INFO || (p->opcode==DESK_TASK_RESULT && p->request==DESK_GFX_QUERY)) && p->serial==r->info_serial)
        gfx_receive(&r->app,p);
    else if ((p->opcode==DESK_SYSTEM_INFO || (p->opcode==DESK_TASK_RESULT && p->request==DESK_SYSTEM_QUERY)) && p->serial==r->system_serial)
        gfx_receive(&r->app,p);
}
static void idle(void *ctx)
{
    struct runtime *r=ctx;uint64_t now=du_now();
    if (!r->window.configured || r->window.closing) return;
    gfx_advance(&r->app,now);
    /* Rendering touches only the private scene. Toolkit repaint/replacement
     * stays completion-gated; it supersedes any prepared successor. */
    if (r->app.page && !r->app.paused && !r->ui.dirty && !r->window.swap_sent &&
        !r->window.request_serial && r->app.animation_due && !r->app.scene_ready) {
        uint64_t phase=r->app.elapsed_ns;
        /* Predict the successor's slot, including on a coarse monotonic clock.
         * Keep the elapsed-time accumulator tied to actual time. */
        if (r->window.present_pending) r->app.elapsed_ns+=GFX_FRAME_NS;
        gfx_prepare_scene(&r->app,r->window.pixels,r->window.info.width);
        r->app.elapsed_ns=phase;
    }
    if (!r->window.present_pending && !r->window.swap_sent && !r->ui.dirty && r->window.count<16) {
        if (r->app.scene_ready) {
            struct gfx_app *a=&r->app; struct du_rect box=a->viewport,dirty=a->scene_damage;
            uint64_t begin=du_now();
            if (a->page || r->scene_active || r->window.damaged) {
                /* Explicit changed source bounds; no whole-window diff on animation. */
                /* A toolkit repaint already published its source rows. */
                if (a->page && !r->window.damaged) gfx_submit_scene(a,r->window.pixels,r->window.info.width);
                struct ciuki_message m; desk_message(&m,DESK_GFX_FRAME,84);
                uint32_t serial=r->serial+1; if (!serial) serial++;
                desk_put32(m.data+4,serial);
                if (a->page) {
                    desk_put32(m.data+8,(uint32_t)box.x); desk_put32(m.data+12,(uint32_t)box.y);
                    desk_put32(m.data+16,(uint32_t)box.w); desk_put32(m.data+20,(uint32_t)box.h);
                    desk_put32(m.data+24,a->scene.width); desk_put32(m.data+28,a->scene.height);
                    desk_put32(m.data+32,(uint32_t)dirty.x); desk_put32(m.data+36,(uint32_t)dirty.y);
                    desk_put32(m.data+40,(uint32_t)dirty.w); desk_put32(m.data+44,(uint32_t)dirty.h);
                }
                if (r->window.damaged) {
                    struct du_rect ui=r->window.damage;
                    desk_put32(m.data+48,(uint32_t)ui.x);desk_put32(m.data+52,(uint32_t)ui.y);
                    desk_put32(m.data+56,(uint32_t)ui.w);desk_put32(m.data+60,(uint32_t)ui.h);
                }
                if (r->window.replacement) {
                    desk_put32(m.data+64,r->window.configure_serial);
                    m.fd_count=1;m.fds[0]=r->window.surface;
                }
                /* Previous acknowledged frame: wait is known only on reply.
                 * Serial makes the one-frame telemetry lag explicit. */
                desk_put32(m.data+68,r->timing_serial);
                desk_put32(m.data+72,(uint32_t)(a->render_ns/1000));
                desk_put32(m.data+76,(uint32_t)(a->submit_ns/1000));
                desk_put32(m.data+80,(uint32_t)(a->wait_ns/1000));
                if (!du_window_queue(&r->window,&m)) {
                    r->submitted_ns=du_now();r->submitted_copy_ns=r->submitted_ns-begin;
                    r->submitted_render_ns=a->scene_render_ns;
                    r->serial=r->frame_serial=serial; r->window.present_pending=1; r->scene_active=a->page!=0;
                    a->scene_ready=0;
                    /* Prepare immediately on the next turn, after FIFO flush.
                     * There is at most one successor and one outstanding frame. */
                    if (a->page && !a->paused) a->animation_due=1;
                    r->window.damaged=0;
                    if (r->window.replacement) r->window.swap_sent=1;
                }
            } else a->scene_ready=0;
        }
    }
    if (now>=r->next_info) {
        request(r,DESK_GFX_QUERY,0); request(r,DESK_SYSTEM_QUERY,0); r->next_info=now+1000000000ull;
    }
    if (r->app.bench_running && !r->bench_pending && now>=r->next_bench)
        request(r,DESK_GFX_BENCH,r->app.bench_index);
}
int main(int argc,char **argv)
{
    int channel,test;const char *path;
    int e=acc_args(argc,argv,"gfxdemo",&channel,&test,&path); if (e || path) return 2;
    if (signal(SIGPIPE,SIG_IGN)==SIG_ERR) return 1;
    struct runtime *r=calloc(1,sizeof(*r)); if (!r) return 1;
    /* Exact approved runtime conversion shared with Desktop and Setup. */
    FILE *f=fopen("/system/assets/ciuki-portrait.xrgb","rb");
    if (f) {
        r->portrait=malloc(256*256*4);
        int valid=r->portrait && fread(r->portrait,4,256*256,f)==256*256 && fgetc(f)==EOF && !ferror(f);
        if (fclose(f)) valid=0;
        if (!valid) { free(r->portrait);r->portrait=NULL; }
    }
    int width,height;acc_window_size(760,660,&width,&height);
    if (du_window_open_resizable(&r->window,&r->ui,channel,width,height,"Graphics Demo",560,360)) {
        fprintf(stderr,"gfxdemo: desktop connection failed: %s\n",strerror(errno)); free(r->portrait);free(r);return 1;
    }
    gfx_attach(&r->app,&r->ui,r->portrait);
    r->app.time_ns=du_now;
    r->app.request=request;r->app.request_ctx=r;
    r->window.message=message;r->window.user=r;
    r->app.deferred=1;
    r->window.manual_damage=1;
    if (test) du_window_move(&r->window,24,48);
    int result;
    while ((result=du_window_turn(&r->window,du_now()))>0) {
        idle(r);
        uint64_t now=du_now();
        /* NANOSLEEP rounds UP to milliseconds. Never request a fractional
         * sleep that would cross the desktop's upcoming nominal slot. */
        uint64_t wait=gfx_wait_ns(now,r->next_slot);
        struct timespec pause={.tv_sec=0,.tv_nsec=(long)wait};
        if (wait) nanosleep(&pause,NULL);
        else (void)ciuki_syscall(CIUKI_SYS_YIELD,0,0,0,0,0,0);
    }
    du_window_destroy(&r->window);free(r->portrait);free(r);return result<0 ? 1 : 0;
}
