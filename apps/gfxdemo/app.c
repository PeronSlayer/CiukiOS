/* SPDX-License-Identifier: MIT */
#include "gfxdemo.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
static void layout(struct du_ui *u,void *ctx)
{
    (void)ctx;
    du_get(u,GFX_TABS)->box=(struct du_rect){8,8,u->canvas.width-16,u->canvas.height-88};
    du_get(u,GFX_RUN)->box=(struct du_rect){16,36,124,22};
    du_get(u,GFX_SHADE)->box=(struct du_rect){16,36,124,22};
    du_get(u,GFX_PAUSE)->box=(struct du_rect){148,36,80,22};
}
static void event(struct du_ui *u,const struct du_event *e,void *ctx)
{
    struct gfx_app *a=ctx;
    if (e->type==DU_EV_SELECT && e->id==GFX_TABS) {
        a->page=du_get(u,GFX_TABS)->active;
        du_get(u,GFX_RUN)->hidden=a->page!=0;
        du_get(u,GFX_SHADE)->hidden=a->page!=1;
        u->dirty=1;
    } else if (e->type==DU_EV_ACTIVATE && e->id==GFX_SHADE) {
        a->gouraud=!a->gouraud;
        du_set_text(du_get(u,GFX_SHADE),a->gouraud ? "Gouraud shading" : "Flat shading"); u->dirty=1;
    } else if (e->type==DU_EV_ACTIVATE && e->id==GFX_PAUSE) {
        a->paused=!a->paused; du_set_text(du_get(u,GFX_PAUSE),a->paused ? "Resume" : "Pause"); u->dirty=1;
    } else if (e->type==DU_EV_ACTIVATE && e->id==GFX_RUN && !a->bench_running) {
        memset(a->results,0,sizeof(a->results)); a->bench_index=0; a->bench_running=1;
        if (!a->request || a->request(a->request_ctx,DESK_GFX_BENCH,0)) {
            a->results[0].error=-EIO; a->bench_running=0;
        }
        u->dirty=1;
    } else if (e->type==DU_EV_TICK) gfx_advance(a,u->now);
}
void gfx_attach(struct gfx_app *a,struct du_ui *u,const uint32_t *portrait)
{
    memset(a,0,sizeof(*a)); a->ui=u; a->portrait=portrait;
    a->tabs[0].text[0]="2D benchmark"; a->tabs[1].text[0]="Software 3D"; a->tabs[2].text[0]="Bouncing sprites";
    struct du_widget *tabs=du_add(u,DU_TABS,GFX_TABS,(struct du_rect){0},"");
    tabs->items=a->tabs; tabs->count=3;
    du_add(u,DU_BUTTON,GFX_RUN,(struct du_rect){0},"Run benchmark");
    du_add(u,DU_BUTTON,GFX_SHADE,(struct du_rect){0},"Flat shading")->hidden=1;
    du_add(u,DU_BUTTON,GFX_PAUSE,(struct du_rect){0},"Pause");
    u->event=event; u->paint=gfx_paint; u->layout=layout; u->user=a;
    layout(u,a);
}
void gfx_advance(struct gfx_app *a,uint64_t now)
{
    if (!a->last_ns) { a->last_ns=now; a->sample_begin=now; a->next_ns=now; }
    if (now<a->last_ns) return;
    if (!a->paused) a->elapsed_ns+=now-a->last_ns;
    a->last_ns=now;
    if (now<a->next_ns) return;
    a->next_ns+=((now-a->next_ns)/GFX_FRAME_NS+1)*GFX_FRAME_NS;
    if (now>a->sample_begin) a->frames++;
    if (now-a->sample_begin>=1000000000ull) {
        uint64_t elapsed=now-a->sample_begin;
        a->fps10=(a->frames*10000000000ull+elapsed/2)/elapsed;
        a->ms1000=elapsed/a->frames/1000;
        a->frames=0; a->sample_begin=now;
    }
    a->ui->dirty=1;
}
void gfx_receive(struct gfx_app *a,const struct desk_packet *p)
{
    if (p->opcode==DESK_GFX_INFO) {
        if (a->have_info && !a->info_error && !p->result &&
            (a->accelerator.generation!=p->accelerator.generation || a->display_w!=p->width ||
             a->display_h!=p->height || a->bpp!=p->bpp)) {
            memset(a->results,0,sizeof(a->results)); a->bench_running=0;
        }
        a->have_info=1; a->info_error=p->result; a->accelerator=p->accelerator;
        a->display_w=p->width; a->display_h=p->height; a->bpp=p->bpp;
    } else if (p->opcode==DESK_SYSTEM_INFO) {
        a->have_desktop=p->performance[0]!=0;
        a->desktop_composite_ns=p->performance[0] ? p->performance[4]/p->performance[0] : 0;
        a->desktop_present_ns=p->performance[0] ? p->performance[5]/p->performance[0] : 0;
    } else if (p->opcode==DESK_GFX_RESULT && a->bench_running && p->index==a->bench_index) {
        struct gfx_result *r=&a->results[p->index];
        r->error=p->result;
        if (!p->result) { r->bytes+=p->benchmark[0]; r->ns+=p->benchmark[1]; r->operations+=p->benchmark[2]; r->samples++; }
        if (p->result || r->samples==4) a->bench_index++;
        if (a->bench_index>=6) a->bench_running=0;
        /* Runtime schedules the next request, rather than bursting here. */
    } else if (p->opcode==DESK_TASK_RESULT && (p->request==DESK_GFX_QUERY || p->request==DESK_SYSTEM_QUERY)) {
        a->info_error=p->result;
    } else if (p->opcode==DESK_TASK_RESULT && p->request==DESK_GFX_BENCH) {
        a->bench_running=0; a->results[a->bench_index].error=p->result;
    }
    a->ui->dirty=1;
}
static void label(struct du_canvas *c,int x,int y,const char *text,uint32_t colour)
{ du_text_fit(c,x,y,c->width-x-16,text,colour); }
static void rate(char *out,size_t cap,const struct gfx_result *r)
{
    if (r->error) { snprintf(out,cap,"Unavailable (%d)",-r->error); return; }
    if (!r->samples || !r->ns) { snprintf(out,cap,"Waiting for sample"); return; }
    /* Decimal MB/s, one destination byte counted once for a copy. */
    uint64_t mb100=r->bytes*100000ull/r->ns,us=r->ns/r->samples/1000;
    snprintf(out,cap,"%llu.%02llu MB/s  %llu.%03llu ms/batch",(unsigned long long)(mb100/100),
        (unsigned long long)(mb100%100),(unsigned long long)(us/1000),(unsigned long long)(us%1000));
}
static void benchmark(struct gfx_app *a,struct du_canvas *c)
{
    int col=(c->width-48)/2, y=74;
    label(c,20,y,"Accelerator (scanout)",DU_INK); label(c,32+col,y,"Software + CPU upload",DU_INK);
    const char *names[3]={"Screen-size fill","256 x 256 copy","Screen-size 8 x 8 pattern"};
    char text[96];
    for (unsigned op=0;op<3;op++) {
        y=100+(int)op*52;
        du_text(c,20,y,names[op],DU_GRAY); du_text(c,32+col,y,names[op],DU_GRAY);
        rate(text,sizeof(text),&a->results[op]); du_text_fit(c,20,y+18,col-8,text,DU_INK);
        rate(text,sizeof(text),&a->results[op+3]); du_text_fit(c,32+col,y+18,col-8,text,DU_INK);
    }
    label(c,20,c->height-123,a->bench_running ? "Sampling: four timed batches per result, one request at a time." :
        "Run briefly fills the display; the desktop is restored after each sample.",DU_GRAY);
    label(c,20,c->height-103,"Destination MB/s; hardware requires completed kernel ops.",DU_GRAY);
}
void gfx_paint(struct du_ui *u,void *ctx)
{
    struct gfx_app *a=ctx; struct du_canvas *c=&u->canvas; char text[160];
    snprintf(text,sizeof(text),"%llu.%llu FPS  %llu.%03llu ms/frame",(unsigned long long)(a->fps10/10),
        (unsigned long long)(a->fps10%10),(unsigned long long)(a->ms1000/1000),(unsigned long long)(a->ms1000%1000));
    du_text_fit(c,240,42,c->width-256,text,DU_INK);
    if (!a->page) benchmark(a,c);
    else {
        if (a->page==1) gfx_cube(&a->scene,a->elapsed_ns,a->portrait,a->gouraud);
        else gfx_sprites(&a->scene,a->elapsed_ns,a->portrait);
        int h=c->height-164,w=h*4/3;
        if (w>c->width-32) { w=c->width-32;h=w*3/4; }
        int x0=(c->width-w)/2,y0=66;
        for (int y=0;y<h;y++) for (int x=0;x<w;x++)
            c->pixels[(size_t)(y0+y)*c->width+x0+x]=a->scene.pixels[(y*GFX_H/h)*GFX_W+x*GFX_W/w];
        label(c,16,c->height-90,a->page==1 ? "320 x 240 CPU rasterizer, z-buffer, Ciuki texture. 3D hardware: F7." :
            "16 Ciuki sprites; elapsed-time motion, 60 Hz target, no vsync.",DU_GRAY);
    }
    du_hline(c,8,c->height-77,c->width-16,DU_SHADOW);
    if (!a->have_info) snprintf(text,sizeof(text),"System info: waiting for desktop");
    else if (a->info_error) snprintf(text,sizeof(text),"System info: %u x %u, %u bpp; accelerator query unavailable (%d)",
        a->display_w,a->display_h,a->bpp,-a->info_error);
    else snprintf(text,sizeof(text),"System info: %u x %u, %u bpp; driver=%s ops=%llu quarantined=%u",
        a->display_w,a->display_h,a->bpp,a->accelerator.driver==CIUKI_ACCEL_DRIVER_SAVAGE ? "savage" : "none",
        (unsigned long long)a->accelerator.operations,a->accelerator.quarantined);
    label(c,12,c->height-64,text,DU_INK);
    if (a->have_desktop) snprintf(text,sizeof(text),"Desktop average: present %llu.%03llu ms  composite %llu.%03llu ms",
        (unsigned long long)(a->desktop_present_ns/1000000),(unsigned long long)(a->desktop_present_ns/1000%1000),
        (unsigned long long)(a->desktop_composite_ns/1000000),(unsigned long long)(a->desktop_composite_ns/1000%1000));
    else snprintf(text,sizeof(text),"Desktop average: collecting present / composite times");
    label(c,12,c->height-45,text,DU_INK);
    label(c,12,c->height-24,!a->portrait ? "Ciuki texture unavailable: install the approved portrait payload." :
        (a->accelerator.capabilities&1) && !a->info_error ? "Presentation: CPU upload + available 2D row copies; FPS counts client frames." :
        "Presentation: CPU upload; FPS counts client frames.",DU_GRAY);
}
