/* SPDX-License-Identifier: MIT */
#include "gfxdemo.h"
#include "../desktop/rows.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
static int compact_scene(const struct du_canvas *c)
{ return c->width>640 && c->height<404; }
static uint64_t scene_now(struct gfx_app *a)
{ return a->time_ns ? a->time_ns() : 0; }
static void layout(struct du_ui *u,void *ctx)
{
    struct gfx_app *a=ctx;
    int compact=a->page && compact_scene(&u->canvas);
    du_get(u,GFX_TABS)->box=(struct du_rect){8,8,u->canvas.width-16,u->canvas.height-(compact ? 56 : 88)};
    du_get(u,GFX_RUN)->box=(struct du_rect){16,36,124,22};
    du_get(u,GFX_SHADE)->box=(struct du_rect){16,36,124,22};
    du_get(u,GFX_PAUSE)->box=(struct du_rect){148,36,80,22};
}
static void event(struct du_ui *u,const struct du_event *e,void *ctx)
{
    struct gfx_app *a=ctx;
    if (e->type==DU_EV_SELECT && e->id==GFX_TABS) {
        a->page=du_get(u,GFX_TABS)->active;
        layout(u,a);
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
    a->native_resolution=1;
    a->tabs[0].text[0]="2D benchmark"; a->tabs[1].text[0]="OpenGL cube"; a->tabs[2].text[0]="OpenGL tiles";
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
    if (a->stats_page!=a->page || a->stats_paused!=a->paused) {
        a->stats_page=a->page;a->stats_paused=a->paused;
        a->frames=0;a->sample_begin=now;a->fps10=0;a->ms1000=0;
    }
    if (now<a->last_ns) return;
    if (!a->paused) a->elapsed_ns+=now-a->last_ns;
    a->last_ns=now;
    /* Publish even while paused/hidden: missing presents must not leave a
     * stale positive rate on screen. Completion timestamps count frames. */
    if (a->page && now>a->sample_begin && now-a->sample_begin>=1000000000ull) {
        uint64_t elapsed=now-a->sample_begin;
        a->fps10=(a->frames*10000000000ull+elapsed/2)/elapsed;
        a->ms1000=a->frames ? elapsed/a->frames/1000 : 0;
        printf("[gfxdemo] fps=%llu.%llu frame_us=%llu scene=%s frames=%llu source=%ux%u\n",
            (unsigned long long)(a->fps10/10),(unsigned long long)(a->fps10%10),
            (unsigned long long)a->ms1000,a->page==1 ? "cube" : "tiles",
            (unsigned long long)a->total_frames,a->scene.width,a->scene.height);
        fflush(stdout);a->frames=0;a->sample_begin=now;a->ui->dirty=1;
    }
    if (now<a->next_ns) return;
    a->next_ns+=((now-a->next_ns)/GFX_FRAME_NS+1)*GFX_FRAME_NS;
    if (a->page && !a->paused) a->animation_due=1;
    if (!a->deferred && a->animation_due) a->ui->dirty=1;
}
void gfx_presented(struct gfx_app *a,uint64_t now)
{
    if (!a->sample_begin) a->sample_begin=now;
    a->frames++;
    a->total_frames++;
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
static void scene_layout(struct gfx_app *a)
{
    struct du_canvas *c=&a->ui->canvas;
    int h=c->height-(compact_scene(c) ? 120 : 164),w=h*4/3;
    if (w>c->width-32) { w=c->width-32;h=w*3/4; }
    /* Wide, short windows reserve 320x240 and compact the status strip. */
    int native=c->width<=640 && c->height<=480;
    if (!native) {
        int ratio=w/GFX_W<h/GFX_H ? w/GFX_W : h/GFX_H;
        if (ratio>=2) { w=GFX_W*ratio; h=GFX_H*ratio; }
    }
    a->viewport=(struct du_rect){(c->width-w)/2,66,w,h};
    a->scene.width=(native || a->native_resolution) ? (unsigned)w : GFX_W;
    a->scene.height=(native || a->native_resolution) ? (unsigned)h : GFX_H;
    if (a->scene.width>GFX_MAX_W || a->scene.height>GFX_MAX_H) {
        a->scene.width=GFX_MAX_W;a->scene.height=GFX_MAX_H;
    }
}
void gfx_update_scene(struct gfx_app *a)
{
    uint64_t begin=a->deferred ? scene_now(a) : 0;
    if (!a->page) { a->viewport=(struct du_rect){0};a->scene_damage=(struct du_rect){0};return; }
    scene_layout(a);
    if (a->page==1) gfx_cube(&a->scene,a->elapsed_ns,a->portrait,a->gouraud);
    else gfx_sprites(&a->scene,a->elapsed_ns,a->portrait);
    gfx_store_scene(a);
    if (a->deferred) a->scene_render_ns=scene_now(a)-begin;
}
/* One private successor; the compositor's front is read-only until completion.
 * Diff against the submitted source, not the toolkit's stale UI back buffer. */
void gfx_prepare_scene(struct gfx_app *a,const uint32_t *front,unsigned stride)
{
    uint64_t begin=scene_now(a);
    scene_layout(a);
    if (a->page==1) gfx_cube(&a->scene,a->elapsed_ns,a->portrait,a->gouraud);
    else gfx_sprites(&a->scene,a->elapsed_ns,a->portrait);
    gfx_diff_scene(a,front,stride);
    a->scene_render_ns=scene_now(a)-begin;
}
void gfx_diff_scene(struct gfx_app *a,const uint32_t *front,unsigned stride)
{
    unsigned w=a->scene.width,h=a->scene.height;
    int left=(int)w,right=-1,top=(int)h,bottom=-1;
    for (unsigned y=0;y<h;y++) {
        const uint32_t *src=a->scene.pixels+(size_t)y*w;
        const uint32_t *dst=front+(size_t)(a->viewport.y+(int)y)*stride+a->viewport.x;
        if (!memcmp(src,dst,w*4)) continue;
        unsigned first=0,last=w-1;
        while (src[first]==dst[first]) first++;
        while (src[last]==dst[last]) last--;
        if ((int)first<left) left=(int)first;
        if ((int)last>right) right=(int)last;
        if ((int)y<top) top=(int)y;
        bottom=(int)y;
    }
    a->scene_damage=right>=left ? (struct du_rect){left,top,right-left+1,bottom-top+1} : (struct du_rect){0};
    a->animation_due=0; a->scene_ready=1;
}
void gfx_submit_scene(struct gfx_app *a,uint32_t *front,unsigned stride)
{
    struct du_rect b=a->viewport,r=a->scene_damage;
    for (int y=0;y<r.h;y++) desk_row_copy(front+(size_t)(b.y+r.y+y)*stride+b.x+r.x,
        a->scene.pixels+(size_t)(r.y+y)*a->scene.width+r.x,(size_t)r.w);
}
void gfx_store_scene(struct gfx_app *a)
{
    struct du_canvas *c=&a->ui->canvas;
    unsigned w=a->scene.width,h=a->scene.height;
    int left=(int)w,right=-1,top=(int)h,bottom=-1;
    for (unsigned y=0;y<h;y++) {
        const uint32_t *src=a->scene.pixels+(size_t)y*w;
        uint32_t *dst=c->pixels+(size_t)(a->viewport.y+(int)y)*c->width+a->viewport.x;
        if (!memcmp(src,dst,w*4)) continue;
        unsigned first=0,last=w-1;
        while (src[first]==dst[first]) first++;
        while (src[last]==dst[last]) last--;
        desk_row_copy(dst+first,src+first,last-first+1);
        if ((int)first<left) left=(int)first;
        if ((int)last>right) right=(int)last;
        if ((int)y<top) top=(int)y;
        bottom=(int)y;
    }
    a->scene_damage=right>=left ? (struct du_rect){left,top,right-left+1,bottom-top+1} : (struct du_rect){0};
    a->animation_due=0; a->scene_ready=1;
}
void gfx_paint(struct du_ui *u,void *ctx)
{
    struct gfx_app *a=ctx; struct du_canvas *c=&u->canvas; char text[160];
    snprintf(text,sizeof(text),"Frame %llu  %llu.%llu FPS  %llu.%03llu ms/frame",(unsigned long long)a->total_frames,(unsigned long long)(a->fps10/10),
        (unsigned long long)(a->fps10%10),(unsigned long long)(a->ms1000/1000),(unsigned long long)(a->ms1000%1000));
    du_text_fit(c,240,42,c->width-256,text,DU_INK);
    if (!a->page) { benchmark(a,c); a->viewport=(struct du_rect){0}; a->scene_ready=1; }
    else {
        gfx_update_scene(a);
        if (!a->deferred) desk_scale_rows(c->pixels+(size_t)a->viewport.y*c->width+a->viewport.x,(unsigned)c->width,
            a->scene.pixels,a->scene.width,a->scene.width,a->scene.height,
            (unsigned)a->viewport.w,(unsigned)a->viewport.h,0,0,(unsigned)a->viewport.w,(unsigned)a->viewport.h);
        /* A full UI repaint changes the mapping/exposure even at a fixed phase. */
        a->scene_damage=(struct du_rect){0,0,(int)a->scene.width,(int)a->scene.height};
        if (!compact_scene(c)) {
            if (a->have_timing) snprintf(text,sizeof(text),"%dx%d / %ux%u: render %llu submit %llu wait %llu us",
                c->width,c->height,a->scene.width,a->scene.height,
                (unsigned long long)(a->render_ns/1000),(unsigned long long)(a->submit_ns/1000),(unsigned long long)(a->wait_ns/1000));
            else snprintf(text,sizeof(text),"%s",a->page==1 ? "TinyGL CPU renderer, depth buffer, lit Ciuki texture." :
                "16 OpenGL Ciuki tiles; elapsed-time motion, 60 Hz display pacing.");
            label(c,16,c->height-90,text,DU_GRAY);
        }
    }
    int compact=a->page && compact_scene(c);
    du_hline(c,8,c->height-(compact ? 46 : 77),c->width-16,DU_SHADOW);
    if (!a->have_info) snprintf(text,sizeof(text),"System info: waiting for desktop");
    else if (a->info_error) snprintf(text,sizeof(text),"System info: %u x %u, %u bpp; accelerator query unavailable (%d)",
        a->display_w,a->display_h,a->bpp,-a->info_error);
    else snprintf(text,sizeof(text),"System info: %u x %u, %u bpp; driver=%s ops=%llu quarantined=%u",
        a->display_w,a->display_h,a->bpp,a->accelerator.driver==CIUKI_ACCEL_DRIVER_SAVAGE ? "savage" : "none",
        (unsigned long long)a->accelerator.operations,a->accelerator.quarantined);
    label(c,12,c->height-(compact ? 35 : 64),text,DU_INK);
    if (a->have_desktop) snprintf(text,sizeof(text),"Desktop average: present %llu.%03llu ms  composite %llu.%03llu ms",
        (unsigned long long)(a->desktop_present_ns/1000000),(unsigned long long)(a->desktop_present_ns/1000%1000),
        (unsigned long long)(a->desktop_composite_ns/1000000),(unsigned long long)(a->desktop_composite_ns/1000%1000));
    else snprintf(text,sizeof(text),"Desktop average: collecting present / composite times");
    if (a->have_timing && a->page) snprintf(text,sizeof(text),
        "%dx%d R/S/W %llu/%llu/%llu us  convert/copy %llu/%llu us  slot %llu us (60 Hz assumed)",c->width,c->height,
        (unsigned long long)(a->render_ns/1000),(unsigned long long)(a->submit_ns/1000),(unsigned long long)(a->wait_ns/1000),
        (unsigned long long)(a->convert_ns/1000),(unsigned long long)(a->copy_ns/1000),(unsigned long long)(a->slot_ns/1000));
    label(c,12,c->height-(compact ? 18 : 45),text,DU_INK);
    if (!compact) label(c,12,c->height-24,a->scene.error ? "TinyGL rendering failed; see the application log." :
        !a->portrait ? "Ciuki texture unavailable: install the approved portrait payload." :
        "TinyGL by Fabrice Bellard, Gek (DMHSW), and C-Chads. FPS counts presents.",DU_GRAY);
}
