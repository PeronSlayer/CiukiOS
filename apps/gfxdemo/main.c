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
        if (!p->result && r->scene_active) gfx_presented(&r->app,p->completed_ns);
        if (p->result && p->result!=-EAGAIN) { r->window.error=-p->result; r->window.closed=1; }
        /* Produce the next frame before the next display slot, one at a time. */
        r->app.next_ns=p->result ? p->next_frame_ns : p->completed_ns;
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
    if (!r->window.present_pending && !r->window.swap_sent && !r->ui.dirty) {
        if (r->app.animation_due && !r->app.scene_ready) gfx_update_scene(&r->app);
        if (r->app.scene_ready) {
            struct gfx_app *a=&r->app; struct du_rect box=a->viewport,dirty=a->scene_damage;
            if (a->page || r->scene_active || r->window.damaged) {
                /* Explicit changed source bounds; no whole-window diff on animation. */
                if (a->page) for (int y=0;y<dirty.h;y++) {
                    size_t at=(size_t)(box.y+dirty.y+y)*r->ui.canvas.width+box.x+dirty.x;
                    desk_row_copy(r->window.pixels+at,r->window.back+at,(size_t)dirty.w);
                }
                struct ciuki_message m; desk_message(&m,DESK_GFX_FRAME,64);
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
                    m.length=68;desk_put32(m.data+64,r->window.configure_serial);
                    m.fd_count=1;m.fds[0]=r->window.surface;
                }
                if (!du_window_queue(&r->window,&m)) {
                    r->serial=r->frame_serial=serial; r->window.present_pending=1; r->scene_active=a->page!=0;
                    a->scene_ready=0;
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
