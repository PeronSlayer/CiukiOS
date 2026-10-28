/* SPDX-License-Identifier: MIT */
#include "gfxdemo.h"
#include "../desktop/rows.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
static int compact_scene(const struct du_canvas *c)
{ return c->width>640 && c->height<404; }
/* f7-14: the GL controls share one row from 680 pixels; narrower windows
 * move Native resolution and Full screen to a second row. */
static int two_rows(const struct du_canvas *c)
{ return c->width<680; }
static int scene_top(const struct gfx_app *a,const struct du_canvas *c)
{ return a->page && two_rows(c) ? 92 : 66; }
const char *gfx_backend_name(unsigned policy)
{
    return policy==CIUKI_GL_BACKEND_HARDWARE ? "Hardware" : policy==CIUKI_GL_BACKEND_SOFTWARE ? "Software" : "Automatic";
}
/* Presentation mode (contract 6): windowed frames are copied into the
 * desktop's composition; a lease presents by GPU copy or CPU copy. */
const char *gfx_present_name(const struct gfx_app *a)
{
    if (a->full_state!=GFX_FULL_ACTIVE) return "desktop-copy";
    return a->scene.presented>0 ? "gpu-copy" : "cpu-copy";
}
void gfx_gl_status(struct gfx_app *a)
{
    struct ciuki_gl_status s={.size=sizeof(s)};
    if (!ciuki_gl_status(&s)) a->gl=s;
}
void gfx_set_backend(struct gfx_app *a,unsigned policy)
{
    a->backend=policy;
    /* -ENODEV for a required but unavailable hardware path: the library
     * keeps rendering in software and the status line says so. */
    a->backend_error=ciuki_gl_set_backend(policy);
    if (a->ui && du_get(a->ui,GFX_BACKEND)) {
        char text[40];snprintf(text,sizeof(text),"Backend: %s",gfx_backend_name(policy));
        du_set_text(du_get(a->ui,GFX_BACKEND),text);du_invalidate_widget(a->ui,GFX_BACKEND);
    }
}
int gfx_status_line(const struct gfx_app *a,char *out,size_t cap)
{
    const char *active=a->gl.backend==CIUKI_GL_BACKEND_HARDWARE ? "hardware 3D" : "software";
    const char *note=a->gl.hardware_failed ? "; hardware failed, software for this context" :
        a->backend_error ? "; hardware unavailable" : "";
    return snprintf(out,cap,"OpenGL %s (%s): GPU frames %llu  CPU frames %llu  refused %llu  fallbacks %llu%s",
        active,gfx_backend_name(a->backend),(unsigned long long)a->gl.hardware_frames,
        (unsigned long long)a->gl.software_frames,(unsigned long long)a->gl.refusals,
        (unsigned long long)a->gl.fallbacks,note);
}
static uint64_t scene_now(struct gfx_app *a)
{ return a->time_ns ? a->time_ns() : 0; }
static void layout(struct du_ui *u,void *ctx)
{
    struct gfx_app *a=ctx;
    int compact=a->page && compact_scene(&u->canvas),rows=two_rows(&u->canvas);
    du_get(u,GFX_TABS)->box=(struct du_rect){8,8,u->canvas.width-16,u->canvas.height-(compact ? 56 : 88)};
    du_get(u,GFX_RUN)->box=(struct du_rect){16,36,124,22};
    du_get(u,GFX_SHADE)->box=(struct du_rect){16,36,124,22};
    du_get(u,GFX_PAUSE)->box=(struct du_rect){148,36,80,22};
    du_get(u,GFX_BACKEND)->box=(struct du_rect){236,36,150,22};
    du_get(u,GFX_NATIVE)->box=rows ? (struct du_rect){16,64,136,18} : (struct du_rect){396,38,136,18};
    du_get(u,GFX_FULLSCREEN)->box=rows ? (struct du_rect){160,62,96,22} : (struct du_rect){540,36,96,22};
}
static void show_controls(struct du_ui *u,struct gfx_app *a)
{
    du_get(u,GFX_RUN)->hidden=a->page!=0;
    du_get(u,GFX_SHADE)->hidden=a->page!=1;
    du_get(u,GFX_BACKEND)->hidden=a->page==0;
    du_get(u,GFX_NATIVE)->hidden=a->page==0;
    du_get(u,GFX_FULLSCREEN)->hidden=a->page==0;
}
static void event(struct du_ui *u,const struct du_event *e,void *ctx)
{
    struct gfx_app *a=ctx;
    if (e->type==DU_EV_SELECT && e->id==GFX_TABS) {
        a->page=du_get(u,GFX_TABS)->active;
        layout(u,a);
        show_controls(u,a);
        u->dirty=1;
    } else if (e->type==DU_EV_ACTIVATE && e->id==GFX_BACKEND) {
        gfx_set_backend(a,a->backend==CIUKI_GL_BACKEND_AUTO ? CIUKI_GL_BACKEND_HARDWARE :
            a->backend==CIUKI_GL_BACKEND_HARDWARE ? CIUKI_GL_BACKEND_SOFTWARE : CIUKI_GL_BACKEND_AUTO);
        a->frames=0;a->sample_begin=0;a->fps10=a->ms1000=0;
        u->dirty=1;
    } else if (e->type==DU_EV_CHANGE && e->id==GFX_NATIVE) {
        a->native_resolution=e->index!=0;
        u->dirty=1;
    } else if (e->type==DU_EV_ACTIVATE && e->id==GFX_FULLSCREEN && a->page && a->full_state==GFX_WINDOWED) {
        /* The desktop answers with DESK_LEASE_EVENT; the runtime renders
         * and presents while the lease holds. */
        a->full_error=a->request ? a->request(a->request_ctx,DESK_LEASE,1) : -ENOSYS;
        if (!a->full_error) a->full_state=GFX_FULL_REQUESTED;
        u->dirty=1;
    } else if (e->type==DU_EV_KEY && e->key==DU_KEY_ESC && a->full_state!=GFX_WINDOWED && a->request) {
        (void)a->request(a->request_ctx,DESK_LEASE,0);
    } else if (e->type==DU_EV_ACTIVATE && e->id==GFX_SHADE) {
        a->gouraud=!a->gouraud;
        du_set_text(du_get(u,GFX_SHADE),a->gouraud ? "Gouraud shading" : "Flat shading"); du_invalidate_widget(u,GFX_SHADE); du_invalidate(u,a->viewport);
    } else if (e->type==DU_EV_ACTIVATE && e->id==GFX_PAUSE) {
        a->paused=!a->paused; du_set_text(du_get(u,GFX_PAUSE),a->paused ? "Resume" : "Pause"); du_invalidate_widget(u,GFX_PAUSE);
    } else if (e->type==DU_EV_ACTIVATE && e->id==GFX_RUN && !a->bench_running) {
        memset(a->results,0,sizeof(a->results)); a->bench_index=0; a->bench_running=1;
        if (!a->request || a->request(a->request_ctx,DESK_GFX_BENCH,0)) {
            a->results[0].error=-EIO; a->bench_running=0;
        }
        du_invalidate(u,(struct du_rect){16,66,u->canvas.width-32,u->canvas.height-166});
    } else if (e->type==DU_EV_TICK) gfx_advance(a,u->now);
}
void gfx_attach(struct gfx_app *a,struct du_ui *u,const uint32_t *portrait)
{
    memset(a,0,sizeof(*a)); a->ui=u; a->portrait=portrait;
    a->native_resolution=1; a->backend=CIUKI_GL_BACKEND_AUTO;
    a->tabs[0].text[0]="2D benchmark"; a->tabs[1].text[0]="OpenGL cube"; a->tabs[2].text[0]="OpenGL tiles";
    struct du_widget *tabs=du_add(u,DU_TABS,GFX_TABS,(struct du_rect){0},"");
    tabs->items=a->tabs; tabs->count=3;
    du_add(u,DU_BUTTON,GFX_RUN,(struct du_rect){0},"Run benchmark");
    du_add(u,DU_BUTTON,GFX_SHADE,(struct du_rect){0},"Flat shading");
    du_add(u,DU_BUTTON,GFX_PAUSE,(struct du_rect){0},"Pause");
    du_add(u,DU_BUTTON,GFX_BACKEND,(struct du_rect){0},"Backend: Automatic");
    du_add(u,DU_CHECKBOX,GFX_NATIVE,(struct du_rect){0},"Native resolution")->value=1;
    du_add(u,DU_BUTTON,GFX_FULLSCREEN,(struct du_rect){0},"Full screen");
    du_tooltip(u,GFX_BACKEND,"Automatic uses qualified 3D hardware when available; Hardware requires it; Software always renders on the CPU.");
    du_tooltip(u,GFX_NATIVE,"Render at the size of the view (or of the screen in full screen) instead of 320 x 240 scaled.");
    du_tooltip(u,GFX_FULLSCREEN,"Show the scene on the whole screen. Press Esc to return.");
    u->event=event; u->paint=gfx_paint; u->layout=layout; u->user=a;
    show_controls(u,a);
    layout(u,a);
}
/* The rate and backend lines, below the scene (and beside the second row of
 * controls in narrow windows); never the scene viewport itself. */
void gfx_invalidate_status(struct gfx_app *a)
{
    struct du_canvas *c=&a->ui->canvas;
    if (!a->page) { du_invalidate(a->ui,(struct du_rect){240,42,c->width-256,18}); return; }
    int top=c->height-(compact_scene(c) ? 50 : 94);
    du_invalidate(a->ui,(struct du_rect){8,top,c->width-16,c->height-top});
    if (two_rows(c)) du_invalidate(a->ui,(struct du_rect){264,62,c->width-280,22});
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
        printf("[gfxdemo] fps=%llu.%llu frame_us=%llu scene=%s frames=%llu source=%ux%u backend=%s policy=%s "
            "gpu_frames=%llu cpu_frames=%llu refusals=%llu fallbacks=%llu present=%s\n",
            (unsigned long long)(a->fps10/10),(unsigned long long)(a->fps10%10),
            (unsigned long long)a->ms1000,a->page==1 ? "cube" : "tiles",
            (unsigned long long)a->total_frames,a->full_state==GFX_FULL_ACTIVE ? a->full_width : a->scene.width,
            a->full_state==GFX_FULL_ACTIVE ? a->full_height : a->scene.height,
            a->gl.backend==CIUKI_GL_BACKEND_HARDWARE ? "hardware" : "software",gfx_backend_name(a->backend),
            (unsigned long long)a->gl.hardware_frames,(unsigned long long)a->gl.software_frames,
            (unsigned long long)a->gl.refusals,(unsigned long long)a->gl.fallbacks,gfx_present_name(a));
        fflush(stdout);a->frames=0;a->sample_begin=now; gfx_invalidate_status(a);
    }
    if (now<a->next_ns) return;
    a->next_ns+=((now-a->next_ns)/GFX_FRAME_NS+1)*GFX_FRAME_NS;
    if (a->page && !a->paused) a->animation_due=1;
    if (!a->deferred && a->animation_due) du_invalidate(a->ui,a->viewport);
}
void gfx_presented(struct gfx_app *a,uint64_t now)
{
    if (!a->sample_begin) a->sample_begin=now;
    a->frames++;
    a->total_frames++;
}
void gfx_receive(struct gfx_app *a,const struct desk_packet *p)
{
    if ((p->opcode==DESK_GFX_INFO || p->opcode==DESK_GFX_INFO_ENGINE)) {
        if (a->have_info && !a->info_error && !p->result &&
            (a->accelerator.generation!=p->accelerator.generation || a->display_w!=p->width ||
             a->display_h!=p->height || a->bpp!=p->bpp)) {
            memset(a->results,0,sizeof(a->results)); a->bench_running=0;
        }
        a->have_info=1; a->info_error=p->result; a->accelerator=p->accelerator; a->engine=p->engine;
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
    } else if (p->opcode==DESK_TASK_RESULT && (p->request==DESK_GFX_QUERY || p->request==DESK_GFX_QUERY_ENGINE || p->request==DESK_SYSTEM_QUERY)) {
        a->info_error=p->result;
    } else if (p->opcode==DESK_TASK_RESULT && p->request==DESK_GFX_BENCH) {
        a->bench_running=0; a->results[a->bench_index].error=p->result;
    }
    struct du_canvas *c=&a->ui->canvas;
    du_invalidate(a->ui,(struct du_rect){8,c->height-100,c->width-16,90});
    if (p->opcode==DESK_GFX_RESULT || (p->opcode==DESK_TASK_RESULT && p->request==DESK_GFX_BENCH))
        du_invalidate(a->ui,(struct du_rect){16,66,c->width-32,c->height-166});
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
    int h=c->height-(compact_scene(c) ? 120 : 164)-(scene_top(a,c)-66),w=h*4/3;
    if (w>c->width-32) { w=c->width-32;h=w*3/4; }
    /* Wide, short windows reserve 320x240 and compact the status strip. */
    int native=c->width<=640 && c->height<=480;
    if (!native) {
        int ratio=w/GFX_W<h/GFX_H ? w/GFX_W : h/GFX_H;
        if (ratio>=2) { w=GFX_W*ratio; h=GFX_H*ratio; }
    }
    a->viewport=(struct du_rect){(c->width-w)/2,scene_top(a,c),w,h};
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
    gfx_gl_status(a);
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
    gfx_gl_status(a);
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
    struct du_rect clip=du_intersection(c->clip,a->viewport);
    unsigned x0=clip.w>0 ? (unsigned)(clip.x-a->viewport.x) : 0;
    unsigned y0=clip.h>0 ? (unsigned)(clip.y-a->viewport.y) : 0;
    unsigned x1=x0+(clip.w>0 ? (unsigned)clip.w : 0),y1=y0+(clip.h>0 ? (unsigned)clip.h : 0);
    for (unsigned y=y0;y<y1 && x0<x1;y++) {
        const uint32_t *src=a->scene.pixels+(size_t)y*w;
        uint32_t *dst=c->pixels+(size_t)(a->viewport.y+(int)y)*c->width+a->viewport.x;
        if (!memcmp(src+x0,dst+x0,(x1-x0)*4)) continue;
        unsigned first=x0,last=x1-1;
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
    struct gfx_app *a=ctx; struct du_canvas *c=&u->canvas; char text[192];
    /* Completed frames: the desktop's present completion (windowed) or the
     * kernel's completed lease copy (full screen); never submissions. */
    snprintf(text,sizeof(text),"Frame %llu  %llu.%llu FPS  %llu.%03llu ms/frame",(unsigned long long)a->total_frames,(unsigned long long)(a->fps10/10),
        (unsigned long long)(a->fps10%10),(unsigned long long)(a->ms1000/1000),(unsigned long long)(a->ms1000%1000));
    if (!a->page) du_text_fit(c,240,42,c->width-256,text,DU_INK);
    else {
        int x=two_rows(c) ? 264 : 16,y=two_rows(c) ? 66 : 0;
        if (y) du_text_fit(c,x,y,c->width-x-16,text,DU_INK);
    }
    if (!a->page) { benchmark(a,c); a->viewport=(struct du_rect){0}; a->scene_ready=1; }
    else {
        scene_layout(a);
        struct du_rect clip=du_intersection(c->clip,a->viewport);
        /* Under the lease the scene is on the whole screen; a status repaint
         * of the hidden window must not rebind GL to the windowed target. */
        if (a->full_state==GFX_FULL_ACTIVE) clip=(struct du_rect){0};
        if (clip.w>0 && clip.h>0) {
            gfx_update_scene(a);
            if (!a->deferred) desk_scale_rows(c->pixels+(size_t)clip.y*c->width+clip.x,(unsigned)c->width,
                a->scene.pixels,a->scene.width,a->scene.width,a->scene.height,
                (unsigned)a->viewport.w,(unsigned)a->viewport.h,(unsigned)(clip.x-a->viewport.x),(unsigned)(clip.y-a->viewport.y),(unsigned)clip.w,(unsigned)clip.h);
            a->scene_damage=(struct du_rect){clip.x-a->viewport.x,clip.y-a->viewport.y,clip.w,clip.h};
        } else {
            /* UI-only updates never render the GL scene. Retain an already
             * prepared successor; otherwise this frame has only UI damage. */
            if (!a->scene_ready) a->scene_damage=(struct du_rect){0};
            a->scene_ready=1;
        }
        if (!compact_scene(c)) {
            char line[256];
            int n=snprintf(line,sizeof(line),"%s%sRender %u x %u in %d x %d (%s)",two_rows(c) ? "" : text,two_rows(c) ? "" : "  ",
                a->scene.width,a->scene.height,c->width,c->height,gfx_present_name(a));
            if (a->have_timing && n>0 && (size_t)n<sizeof(line))
                snprintf(line+n,sizeof(line)-(size_t)n,"  R/S/W %llu/%llu/%llu us",(unsigned long long)(a->render_ns/1000),
                    (unsigned long long)(a->submit_ns/1000),(unsigned long long)(a->wait_ns/1000));
            label(c,16,c->height-90,line,DU_GRAY);
        }
    }
    int compact=a->page && compact_scene(c);
    du_hline(c,8,c->height-(compact ? 46 : 77),c->width-16,DU_SHADOW);
    if (!a->have_info) snprintf(text,sizeof(text),"System info: waiting for desktop");
    else if (a->info_error) snprintf(text,sizeof(text),"System info: %u x %u, %u bpp; accelerator query unavailable (%d)",
        a->display_w,a->display_h,a->bpp,-a->info_error);
    else snprintf(text,sizeof(text),"System info: %u x %u, %u bpp; driver=%s ops=%llu quarantined=%u",
        a->display_w,a->display_h,a->bpp,a->engine.driver_id[0] ? a->engine.driver_id : "unavailable",
        (unsigned long long)a->accelerator.operations,a->accelerator.quarantined);
    label(c,12,c->height-(compact ? 35 : 64),text,DU_INK);
    if (a->page) {
        if (compact) {
            char status[160];gfx_status_line(a,status,sizeof(status));
            snprintf(text,sizeof(text),"%llu.%llu FPS  %s",(unsigned long long)(a->fps10/10),(unsigned long long)(a->fps10%10),status);
        } else gfx_status_line(a,text,sizeof(text));
        if (a->full_error && a->full_state==GFX_WINDOWED && !compact)
            snprintf(text+strlen(text),sizeof(text)-strlen(text),"; full screen refused (%d)",-a->full_error);
    } else if (a->have_desktop) snprintf(text,sizeof(text),"Desktop average: present %llu.%03llu ms  composite %llu.%03llu ms",
        (unsigned long long)(a->desktop_present_ns/1000000),(unsigned long long)(a->desktop_present_ns/1000%1000),
        (unsigned long long)(a->desktop_composite_ns/1000000),(unsigned long long)(a->desktop_composite_ns/1000%1000));
    else snprintf(text,sizeof(text),"Desktop average: collecting present / composite times");
    label(c,12,c->height-(compact ? 18 : 45),text,DU_INK);
    if (compact) return;
    label(c,12,c->height-24,a->scene.error ? "OpenGL rendering failed; see the application log." :
        !a->portrait ? "Ciuki texture unavailable: install the approved portrait payload." :
        "TinyGL by Fabrice Bellard, Gek (DMHSW), and C-Chads. FPS counts completed frames.",DU_GRAY);
}
